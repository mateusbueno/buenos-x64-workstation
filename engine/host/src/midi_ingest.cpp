#include "velora/host/midi_ingest.hpp"

namespace velora::host {

bool normalise_packet(const std::uint32_t* words, std::size_t count, std::size_t& consumed,
                      core::Ump& out) noexcept {
  consumed = 0;
  if (count == 0) return false;
  const std::uint32_t w0 = words[0];
  const std::uint32_t mt = w0 >> 28;
  const std::size_t need = ump_packet_words(mt);
  if (need > count) {  // truncated packet: drop the remainder
    consumed = count;
    return false;
  }
  consumed = need;
  const std::uint32_t group = (w0 >> 24) & 0xF;
  const std::uint32_t status = (w0 >> 20) & 0xF;
  const std::uint32_t channel = (w0 >> 16) & 0xF;
  if (mt == 0x2 && (status == core::Ump::kStatusNoteOn || status == core::Ump::kStatusNoteOff)) {
    const std::uint32_t note = (w0 >> 8) & 0x7F;
    const std::uint32_t vel = w0 & 0x7F;
    if (status == core::Ump::kStatusNoteOn && vel != 0)
      out = core::make_note_on(group, channel, note, scale_velocity_7_to_16(vel));
    else
      out = core::make_note_off(group, channel, note, 0);  // Story 3.6 policy: an external MIDI 1.0 note off carries no release velocity (see below)
    return true;
  }
  // Channel-mode messages (CC 120-127: all sound off, reset all controllers, local control, all notes off, omni, mono, poly) are not controllers:
  // they are not queued as such (counted as ignored). The host does not implement them yet; queuing them as plain controllers would let a
  // "reset all controllers" overwrite the sample engine's controller state with a value and do nothing else.
  if ((mt == 0x2 || mt == core::Ump::kMessageTypeChannelVoice2) && status == core::Ump::kStatusControlChange && ((w0 >> 8) & 0x7F) >= 120)
    return false;
  // Control change -> performance parameter UMP (channel = part) for the controllers the registry maps (CC 70-94: they keep that
  // meaning). Every other controller except the pedal passes as the MIDI 2.0 control change itself (Story 5.2): the block loop
  // delivers it to the CcState of the sample engine (CC crossfades); a controller no engine reads is counted there as ignored.
  if (mt == 0x2 && status == core::Ump::kStatusControlChange) {
    const std::uint32_t v = w0 & 0x7F;
    const std::uint32_t v32 = v == 0 ? 0 : ((v << 25) | (v << 18) | (v << 11) | (v << 4) | (v >> 3));
    const core::Ump cc = core::make_control_change(group, channel, (w0 >> 8) & 0x7F, v32);
    if (cc.controller_index() == core::Ump::kControllerSustain) {  // the pedal goes to the arbiter as before
      out = cc;
      return true;
    }
    if (core::cc_to_param_event(cc, out)) return true;
    out = cc;
    return true;
  }
  if (mt == core::Ump::kMessageTypeChannelVoice2 && status == core::Ump::kStatusControlChange) {
    const core::Ump cc{w0, words[1]};
    if (cc.controller_index() == core::Ump::kControllerSustain) {
      out = cc;
      return true;
    }
    if (core::cc_to_param_event(cc, out)) return true;
    out = cc;
    return true;
  }
  if (mt == core::Ump::kMessageTypeChannelVoice2 &&
      (status == core::Ump::kStatusNoteOn || status == core::Ump::kStatusNoteOff)) {
    // Story 3.6 policy (to revisit): the release velocity of an external note off is dropped (0 = "none measured"), whether MIDI 1.0 (the common default 64,
    // or a note on with velocity 0) or MIDI 2.0, so the sample engine's release-velocity option (SampleEngine::Config::release_velocity_triggers) only reacts to the
    // keybed reader's Note Off, which does not pass through here.
    out = core::Ump{w0, status == core::Ump::kStatusNoteOff ? (words[1] & 0xFFFFu) : words[1]};
    return true;
  }
  return false;
}

void MidiIngest::on_ump_port(ports::MidiPortKind port, const std::uint32_t* words, std::size_t count, std::uint64_t host_ns) noexcept {
  while (count > 0) {
    const std::uint32_t w0 = words[0];
    const std::uint32_t mt = w0 >> 28;
    const std::size_t need = ump_packet_words(mt);
    if (need > count) {
      counters_.ignored.fetch_add(1, std::memory_order_relaxed);
      break;
    }
    // SysEx7 (Message Type 0x3) bypasses the audio queue entirely and is routed
    // directly to the lock-free sysex queue for the control thread (Story 12.4).
    if (mt == core::Ump::kMessageTypeData64) {
      if (sysex_queue_) {
        core::Ump pkt{w0, words[1]};
        if (!sysex_queue_->push(pkt)) {
          counters_.sysex_dropped.fetch_add(1, std::memory_order_relaxed);
        } else {
          counters_.sysex_packets.fetch_add(1, std::memory_order_relaxed);
        }
      }
      words += need;
      count -= need;
      continue;
    }

    // Realtime clock (Story 7.7): only the selected port's group 0 0xF8 / 0xFA / 0xFB / 0xFC reach the follower, stamped here on arrival (input edge thread).
    if (mt == core::Ump::kMessageTypeSystemRealTime) {
      const std::uint32_t sb = (w0 >> 16) & 0xFF;
      if (sb == core::Ump::kStatusTimingClock || sb == core::Ump::kStatusStart || sb == core::Ump::kStatusContinue || sb == core::Ump::kStatusStop) {
        const core::ClockSource cs = clock_source_.load(std::memory_order_acquire);
        const bool selected = (cs == core::ClockSource::kDin && port == ports::MidiPortKind::kDin) ||
                              (cs == core::ClockSource::kUsb && port == ports::MidiPortKind::kUsb);
        if (!selected) {
          counters_.clock_dropped.fetch_add(1, std::memory_order_relaxed);
        } else if (((w0 >> 24) & 0xF) != 0) {  // only group 0 (the RT plane's rule): a multi-cable device's other groups would double the tick rate
          counters_.clock_dropped_group.fetch_add(1, std::memory_order_relaxed);
        } else if (sb == core::Ump::kStatusTimingClock && queue_.size() + kNoteHeadroom >= HostEventQueue::capacity()) {
          counters_.clock_dropped_full.fetch_add(1, std::memory_order_relaxed);  // like controllers: ticks never take the slots kept for notes (Start / Continue / Stop do)
        } else if (queue_.push(StampedUmp{core::Ump{w0, 0}, host_ns, port})) {
          last_stamp_ns_.store(host_ns, std::memory_order_relaxed);
          counters_.clock_forwarded.fetch_add(1, std::memory_order_relaxed);
        } else {
          counters_.queue_full.fetch_add(1, std::memory_order_relaxed);
        }
        words += need;
        count -= need;
        continue;
      }
    }

    std::size_t used = 0;
    core::Ump u;
    if (normalise_packet(words, count, used, u)) {
      // Controllers (mapped or not) are the first to go when the queue is nearly full: the last kNoteHeadroom slots are for notes and the
      // pedal, so a controller flood can never make a note on or a note off miss the queue.
      const bool is_note_class = !u.is_control_change() && !u.is_assignable_controller();
      const bool is_pedal = u.is_control_change() && u.controller_index() == core::Ump::kControllerSustain;
      if (!is_note_class && !is_pedal && queue_.size() + kNoteHeadroom >= HostEventQueue::capacity()) {
        counters_.controllers_dropped.fetch_add(1, std::memory_order_relaxed);
      } else if (queue_.push(StampedUmp{u, host_ns, port})) {
        last_stamp_ns_.store(host_ns, std::memory_order_relaxed);
        const bool raw_controller = u.is_control_change() && u.controller_index() != core::Ump::kControllerSustain;
        (u.is_assignable_controller() ? counters_.params : raw_controller ? counters_.controllers : counters_.notes)
            .fetch_add(1, std::memory_order_relaxed);
      } else
        counters_.queue_full.fetch_add(1, std::memory_order_relaxed);
    } else {
      counters_.ignored.fetch_add(1, std::memory_order_relaxed);
    }
    if (used == 0) break;
    words += used;
    count -= used;
  }
}

}  // namespace velora::host
