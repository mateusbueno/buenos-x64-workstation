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
      out = core::make_note_off(group, channel, note, scale_velocity_7_to_16(vel));
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
    out = core::Ump{w0, words[1]};
    return true;
  }
  return false;
}

void MidiIngest::on_ump(const std::uint32_t* words, std::size_t count, std::uint64_t host_ns) noexcept {
  while (count > 0) {
    std::size_t used = 0;
    core::Ump u;
    if (normalise_packet(words, count, used, u)) {
      // Controllers (mapped or not) are the first to go when the queue is nearly full: the last kNoteHeadroom slots are for notes and the
      // pedal, so a controller flood can never make a note on or a note off miss the queue.
      const bool is_note_class = !u.is_control_change() && !u.is_assignable_controller();
      const bool is_pedal = u.is_control_change() && u.controller_index() == core::Ump::kControllerSustain;
      if (!is_note_class && !is_pedal && queue_.size() + kNoteHeadroom >= HostEventQueue::capacity()) {
        counters_.controllers_dropped.fetch_add(1, std::memory_order_relaxed);
      } else if (queue_.push(StampedUmp{u, host_ns})) {
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
