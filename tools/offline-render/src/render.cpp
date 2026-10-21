#include "render.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

#include "velora/core/block_renderer.hpp"
#include "velora/core/contact_events.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/params_gen.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/mixer.hpp"
#include "velora/core/snapshot.hpp"
#include "velora/ports/line_in_source.hpp"

namespace velora::offline {

namespace {
// Reads a WAV (mono or stereo, 48 kHz) block by block; silent after its end.
class WavLineIn final : public velora::ports::LineInSource {
 public:
  explicit WavLineIn(const WavData& w) : w_(w) {}
  void read_block(float* left, float* right, std::uint32_t frames) noexcept VELORA_NONBLOCKING override {
    for (std::uint32_t i = 0; i < frames; ++i, ++pos_) {
      if (pos_ >= w_.frames()) {
        left[i] = right[i] = 0.0f;
      } else if (w_.channels == 1) {
        left[i] = right[i] = w_.samples[pos_];
      } else {
        left[i] = w_.samples[pos_ * w_.channels];
        right[i] = w_.samples[pos_ * w_.channels + 1];
      }
    }
  }

 private:
  const WavData& w_;
  std::uint64_t pos_ = 0;
};
}  // namespace

bool render_events(const std::vector<NoteEvent>& events, const RenderOptions& opt, WavData& out,
                   std::string& error) {
  return render_events(events, opt, out, error, nullptr);
}

bool render_events(const std::vector<NoteEvent>& events, const RenderOptions& opt, WavData& out,
                   std::string& error, SampleRenderStats* stats) {
  using namespace velora::core;
  if (opt.voices == 0 || !(opt.tail_seconds >= 0.0) || opt.tail_seconds > 600.0) {
    error = "invalid render options";
    return false;
  }
  if (opt.routing.enabled)  // UMP channels 8..15 would alias parts 0..7 in the routing topology: refuse them
    for (const NoteEvent& e : events)
      if (e.channel > 7) {
        error = "routing options need MIDI channels 0..7 (parts); the input has an event on channel " + std::to_string(e.channel + 1);
        return false;
      }
  const std::uint64_t tail = static_cast<std::uint64_t>(std::llround(opt.tail_seconds * kSampleRate));
  // Controllers that map to no parameter are dropped (they neither play nor extend the render).
  // The organ engine maps controllers to its parameters (its parts are channels 0..7). The sample engine (Story 5.2) takes every
  // control change as the control change itself: the part's CcState (CC crossfades) follows it; sustain (CC 64) is the pedal.
  // Other renders ignore controllers entirely.
  const bool organ_render = opt.engine == EngineKind::kOrgan;
  const bool sample_render = opt.engine == EngineKind::kSample;
  const bool routing_enabled = opt.routing.enabled;
  auto control = [organ_render, sample_render, routing_enabled, &opt](const NoteEvent& e, Ump& out) {
    const std::uint32_t v32 = static_cast<std::uint32_t>(e.value * 0xFFFFFFFFull / 127);
    if (sample_render && !e.contact) {
      out = make_control_change(0, e.channel, e.note, v32);
      return true;
    }
    const bool arp_on_channel = e.channel < params::kPartCount &&
        (opt.routing.arp[e.channel].enabled ||
         std::any_of(opt.routing.arp_switches.begin(), opt.routing.arp_switches.end(),
                     [&](const auto& sw) { return sw.arp[e.channel].enabled; }));
    if (routing_enabled && !e.contact && e.note == Ump::kControllerSustain && arp_on_channel) {
      out = make_control_change(0, e.channel, e.note, v32);
      return true;
    }
    if (!organ_render || e.contact || e.channel >= OrganEngine::kPartCount) return false;
    return cc_to_param_event(make_control_change(0, e.channel, e.note, v32), out);
  };
  std::uint64_t last = 0;
  for (const NoteEvent& e : events) {
    Ump u;
    if (e.contact) {
      if (organ_render && e.channel < OrganEngine::kPartCount) last = std::max(last, e.frame);  // an organ feature on parts, like controllers
    } else if (!e.cc || control(e, u)) {
      last = std::max(last, e.frame);
    }
  }
  for (const auto& as : opt.routing.arp_switches) {
    if (as.seconds > 0.0) {
      last = std::max(last, static_cast<std::uint64_t>(std::llround(as.seconds * kSampleRate)));
    }
  }
  const std::uint64_t total = last + tail;
  if (total > static_cast<std::uint64_t>(kSampleRate) * 3600) {
    error = "render would exceed one hour";
    return false;
  }
  const std::uint64_t blocks = (total + kBlockFrames) / kBlockFrames;  // covers frame `total` too

  auto br = std::make_unique<BlockRenderer>(opt.voices);
  if (!br->ready()) {
    error = "block renderer failed to initialise";
    return false;
  }
  if (opt.emitted_notes) {
    opt.emitted_notes->clear();
    br->set_note_observer(
        [](void* ctx, const EmittedNoteEvent& ev) noexcept {
          static_cast<std::vector<EmittedNoteEvent>*>(ctx)->push_back(ev);
        },
        opt.emitted_notes);
  }
  std::unique_ptr<Snapshot> routing_snapshot;
  std::unique_ptr<WavLineIn> line_in;
  if (opt.routing.enabled) {
    const RoutingOptions& ro = opt.routing;
    if (ro.line_in) {
      if (ro.line_in->sample_rate != kSampleRate || (ro.line_in->channels != 1 && ro.line_in->channels != 2)) {
        error = "line-in must be a mono or stereo 48 kHz WAV";
        return false;
      }
      line_in = std::make_unique<WavLineIn>(*ro.line_in);
      br->set_line_in(line_in.get());
    }
    routing_snapshot = std::make_unique<Snapshot>(Snapshot::defaults());
    routing_snapshot->revision = 1;
    routing_snapshot->ledger_index = 0;
    for (std::uint32_t p = 0; p < params::kPartCount; ++p) {
      PartCfg& pc = routing_snapshot->parts[p];
      pc.output = ro.output[p];
      pc.arp = ro.arp[p];
      if (ro.inserts[p].size() > params::kInsertSlotCount) {
        error = "too many inserts on a part (at most 5)";
        return false;
      }
      pc.insert_count = static_cast<std::uint8_t>(ro.inserts[p].size());
      for (std::size_t i = 0; i < ro.inserts[p].size(); ++i) {
        pc.inserts[i].type = ro.inserts[p][i].type;
        pc.inserts[i].bypass = ro.inserts[p][i].bypass ? 1 : 0;
        if (ro.inserts[p][i].has_params) pc.inserts[i].params = ro.inserts[p][i].params;
      }
      routing_snapshot->perf_init[params::perf_slot(*params::find(params::kSendReverbLevel), p)] = ro.send_reverb[p];
      routing_snapshot->perf_init[params::perf_slot(*params::find(params::kSendDelayLevel), p)] = ro.send_delay[p];
    }
    routing_snapshot->perf_init[params::perf_slot(*params::find(params::kLineInLevel), 0)] = ro.line_in_level;
    auto set_master = [&](std::uint16_t id, float v) {
      routing_snapshot->master[static_cast<std::size_t>(params::find(id)->master_index)] = v;
    };
    set_master(params::kMasterLevel, ro.master_level_db);
    set_master(params::kReverbType, static_cast<float>(ro.reverb_type));
    set_master(params::kReverbTime, ro.reverb_time_s);
    set_master(params::kReverbLevel, ro.reverb_level);
    routing_snapshot->perf_init[params::perf_slot(*params::find(params::kSendReverbMaster), 0)] = ro.reverb_master;
    set_master(params::kDelayTime, ro.delay_time_ms);
    set_master(params::kDelaySync, ro.delay_sync ? 1.0f : 0.0f);
    set_master(params::kDelayNote, static_cast<float>(ro.delay_note));
    set_master(params::kDelayFeedback, ro.delay_feedback);
    set_master(params::kDelayLevel, ro.delay_level);
    set_master(params::kDelayDamping, ro.delay_damping);
    set_master(params::kDelayPingpong, ro.delay_pingpong);
    routing_snapshot->perf_init[params::perf_slot(*params::find(params::kTempoBpm), 0)] = ro.tempo_bpm;
    set_master(params::kMasterEqLowGain, ro.master_eq.low_db);
    set_master(params::kMasterEqMidGain, ro.master_eq.mid_db);
    set_master(params::kMasterEqMidFreq, ro.master_eq.mid_hz);
    set_master(params::kMasterEqHighGain, ro.master_eq.high_db);
    set_master(params::kMasterEqBypass, ro.master_eq.bypass ? 1.0f : 0.0f);
    set_master(params::kMasterCompThreshold, ro.master_comp.threshold_db);
    set_master(params::kMasterCompRatio, ro.master_comp.ratio);
    set_master(params::kMasterCompAttack, ro.master_comp.attack_ms);
    set_master(params::kMasterCompRelease, ro.master_comp.release_ms);
    set_master(params::kMasterCompMakeup, ro.master_comp.makeup_db);
    set_master(params::kMasterCompBypass, ro.master_comp.bypass ? 1.0f : 0.0f);
    build_chains(*routing_snapshot);
    br->snapshots().publish(routing_snapshot.get());
  }
  if (opt.detune_cents != 0.0f) br->test_tone().set_param(TestToneEngine::kParamDetuneCents, opt.detune_cents);

  std::unique_ptr<SampleEngine> sample;
  if (opt.engine == EngineKind::kSample) {
    if (!opt.instrument) {
      error = "sample engine needs an instrument";
      return false;
    }
    SampleEngine::Config cfg;
    cfg.max_voices = opt.voices;
    cfg.sample_rate = kSampleRate;
    cfg.interpolation = opt.interpolation;
    cfg.seed = opt.seed;
    cfg.oracle_block_frames = opt.oracle_block_frames;
    sample = std::make_unique<SampleEngine>(cfg);
    sample->set_param(SampleEngine::kParamGain, std::pow(10.0f, opt.gain_db * 0.05f));
    if (!sample->set_instrument(*opt.instrument) || !br->bind_engine(*sample)) {
      error = "cannot bind the sample engine";
      return false;
    }
    for (std::uint32_t ch = 0; ch < 16; ++ch)
      br->set_channel_engine(ch, static_cast<std::uint32_t>(EngineTypeId::kSample));
  }

  std::unique_ptr<OrganEngine> organ;
  // Frame-0 setup per organ part: 9 drawbars + 5 percussion/key click parameters + 3 scanner/leakage parameters + 2 rotary parameters. All of it must fit the block loop's
  // pending-event buffer together with the events of the first block (checked after the first block, see below).
  static_assert(OrganEngine::kPartCount * 19 <= BlockRenderer::kPendingCapacity, "organ setup events must fit the pending buffer");
  if (opt.engine == EngineKind::kOrgan) {
    if (opt.drawbars.size() != 9 ||
        std::any_of(opt.drawbars.begin(), opt.drawbars.end(), [](char c) { return c < '0' || c > '8'; })) {
      error = "invalid drawbars (nine digits 0..8)";
      return false;
    }
    organ = std::make_unique<OrganEngine>();
    if (!organ->set_contact_count(opt.contact_count)) {
      error = "invalid contact count (2 or 3)";
      return false;
    }
    if (!br->bind_engine(*organ)) {
      error = "cannot bind the organ engine";
      return false;
    }
    for (std::uint32_t ch = 0; ch < OrganEngine::kPartCount; ++ch) {
      br->set_channel_engine(ch, static_cast<std::uint32_t>(EngineTypeId::kOrgan));
      for (std::uint32_t d = 0; d < 9; ++d)
        if (!br->submit({make_param_event(*params::find(params::kDrawbar16 + d), ch, opt.drawbars[d] - '0'), 0})) {
          error = "event queue full while setting the drawbars (try a render with fewer simultaneous events)";
          return false;
        }
      const struct { std::uint32_t id; double value; } perc[] = {
          {params::kPercussionOn, opt.percussion_on ? 1.0 : 0.0},
          {params::kPercussionHarmonic, static_cast<double>(opt.percussion_harmonic)},
          {params::kPercussionDecay, static_cast<double>(opt.percussion_decay)},
          {params::kPercussionVolume, static_cast<double>(opt.percussion_volume)},
          {params::kKeyClick, static_cast<double>(opt.key_click)},
          {params::kVibratoChorusOn, opt.vibrato > 0 ? 1.0 : 0.0},
          {params::kVibratoChorusType, opt.vibrato > 0 ? static_cast<double>(opt.vibrato - 1) : static_cast<double>(params::find(params::kVibratoChorusType)->def)},
          {params::kLeakage, static_cast<double>(opt.leakage)},
          // rotary: 0 off (speed stays at the registry default), 1 slow, 2 fast, 3 stop; speed first so that it settles before the stage is switched on
          {params::kRotarySpeed, opt.rotary == 0 ? static_cast<double>(params::find(params::kRotarySpeed)->def)
                                                  : static_cast<double>(opt.rotary == 3 ? 0 : opt.rotary)},
          {params::kRotaryOn, opt.rotary > 0 ? 1.0 : 0.0}};
      for (const auto& pv : perc)
        if (!br->submit({make_param_event(*params::find(pv.id), ch, pv.value), 0})) {
          error = "event queue full while setting percussion, key click, scanner, leakage and rotary";
          return false;
        }
    }
    // Channels 8..15 have no organ part: their notes are refused (engine not available), not played as test tone.
    for (std::uint32_t ch = OrganEngine::kPartCount; ch < 16; ++ch)
      br->set_channel_engine(ch, static_cast<std::uint32_t>(EngineTypeId::kFm));
  }

  out.sample_rate = kSampleRate;
  out.channels = 2;
  out.samples.assign(static_cast<std::size_t>(blocks * kBlockFrames * 2), 0.0f);
  if (opt.aux_out) {
    opt.aux_out->sample_rate = kSampleRate;
    opt.aux_out->channels = 2;
    opt.aux_out->samples.assign(static_cast<std::size_t>(blocks * kBlockFrames * 2), 0.0f);
  }

  // Tempo changes (Story 2.11): ordered by time, submitted in the block that contains their frame like every other event.
  std::vector<std::pair<std::uint64_t, float>> tempo_events;
  for (const auto& tc : opt.routing.tempo_changes)
    tempo_events.emplace_back(static_cast<std::uint64_t>(std::llround(tc.first * kSampleRate)), tc.second);
  std::stable_sort(tempo_events.begin(), tempo_events.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::size_t next_tempo = 0;

  std::vector<std::pair<std::uint64_t, RoutingOptions::ArpSwitch>> arp_switches;
  for (const auto& as : opt.routing.arp_switches) {
    arp_switches.emplace_back(static_cast<std::uint64_t>(std::llround(std::max(0.0, as.seconds) * kSampleRate)), as);
  }
  std::stable_sort(arp_switches.begin(), arp_switches.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::size_t next_arp_switch = 0;
  std::vector<std::unique_ptr<Snapshot>> switch_snapshots;

  float l[kBlockFrames], r[kBlockFrames], al[kBlockFrames], ar[kBlockFrames];
  std::size_t next = 0;
  for (std::uint64_t b = 0; b < blocks; ++b) {
    const std::uint64_t end = (b + 1) * kBlockFrames;
    while (routing_snapshot && next_arp_switch < arp_switches.size() && arp_switches[next_arp_switch].first < end) {
      const auto& as = arp_switches[next_arp_switch++];
      const Snapshot& base = switch_snapshots.empty() ? *routing_snapshot : *switch_snapshots.back();
      auto s = std::make_unique<Snapshot>(base);
      s->revision = 1 + next_arp_switch;
      s->program_generation = next_arp_switch;
      s->ledger_index = static_cast<std::uint32_t>(next_arp_switch % SnapshotExchange::kLedgerSlots);
      for (std::uint32_t p = 0; p < params::kPartCount; ++p) {
        s->parts[p].arp = as.second.arp[p];
      }
      br->snapshots().publish(s.get());
      switch_snapshots.push_back(std::move(s));
    }
    while (next_tempo < tempo_events.size() && tempo_events[next_tempo].first < end) {
      const auto& te = tempo_events[next_tempo++];
      const std::uint32_t off = te.first < b * kBlockFrames ? 0u : static_cast<std::uint32_t>(te.first - b * kBlockFrames);
      if (!br->submit({make_param_event(*params::find(params::kTempoBpm), 0, static_cast<double>(te.second)), off})) {
        error = "event queue overflow (too many events in one block)";
        return false;
      }
    }
    while (next < events.size() && events[next].frame < end) {
      const NoteEvent& e = events[next++];
      const std::uint32_t off = static_cast<std::uint32_t>(e.frame - b * kBlockFrames);
      if (e.contact) {
        if (organ_render && !br->submit({make_contact_event(0, e.channel, e.note, e.contact_index, e.closed), off})) {
          error = "event queue overflow (too many events in one block)";
          return false;
        }
        continue;
      }
      if (e.cc) {
        Ump pu;
        if (control(e, pu) && !br->submit({pu, off})) {
          error = "event queue overflow (too many events in one block)";
          return false;
        }
        continue;
      }
      const Ump u = e.on ? make_note_on(0, e.channel, e.note, e.velocity16)
                         : make_note_off(0, e.channel, e.note, e.velocity16);
      if (!br->submit({u, off})) {
        error = "event queue overflow (too many events in one block)";
        return false;
      }
    }
    for (std::uint32_t i = 0; i < kBlockFrames; ++i) { l[i] = 0.0f; r[i] = 0.0f; }
    if (opt.aux_out) {
      OutputBuses ob;
      ob.main_left = l;
      ob.main_right = r;
      ob.aux_left = al;
      ob.aux_right = ar;
      br->process_block(ob);
    } else {
      br->process_block(l, r);
    }
    if (br->status_count(EngineStatus::kEventOverflow) != 0) {
      error = "event buffer overflow (too many events in one block, including the organ setup events)";
      return false;
    }
    float* dst = &out.samples[static_cast<std::size_t>(b * kBlockFrames * 2)];
    for (std::uint32_t i = 0; i < kBlockFrames; ++i) {
      dst[2 * i] = l[i];
      dst[2 * i + 1] = r[i];
    }
    if (opt.aux_out) {
      float* adst = &opt.aux_out->samples[static_cast<std::size_t>(b * kBlockFrames * 2)];
      for (std::uint32_t i = 0; i < kBlockFrames; ++i) {
        adst[2 * i] = al[i];
        adst[2 * i + 1] = ar[i];
      }
    }
  }
  if (stats && organ) {
    stats->pool_exhausted = br->status_count(EngineStatus::kPoolExhausted);
    stats->voice_not_found = br->status_count(EngineStatus::kVoiceNotFound);
  }
  if (stats && sample) {
    stats->pool_exhausted = br->status_count(EngineStatus::kPoolExhausted);
    stats->voice_not_found = br->status_count(EngineStatus::kVoiceNotFound);
    stats->skipped_unplayable_starts = sample->skipped_unplayable_starts();
    stats->unplayable_regions = sample->unplayable_regions();
  }
  return true;
}

}  // namespace velora::offline
