#include "render.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

#include "velora/core/block_renderer.hpp"
#include "velora/core/contact_events.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/params_gen.hpp"
#include "velora/core/param_events.hpp"

namespace velora::offline {

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
  const std::uint64_t tail = static_cast<std::uint64_t>(std::llround(opt.tail_seconds * kSampleRate));
  // Controllers that map to no parameter are dropped (they neither play nor extend the render).
  // Controllers are an organ-engine feature (its parts are channels 0..7); other renders ignore them entirely.
  const bool organ_render = opt.engine == EngineKind::kOrgan;
  auto control = [organ_render](const NoteEvent& e, Ump& out) {
    if (!organ_render || e.contact || e.channel >= OrganEngine::kPartCount) return false;
    return cc_to_param_event(
        make_control_change(0, e.channel, e.note, static_cast<std::uint32_t>(e.value * 0xFFFFFFFFull / 127)), out);
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

  float l[kBlockFrames], r[kBlockFrames];
  std::size_t next = 0;
  for (std::uint64_t b = 0; b < blocks; ++b) {
    const std::uint64_t end = (b + 1) * kBlockFrames;
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
    br->process_block(l, r);
    if (br->status_count(EngineStatus::kEventOverflow) != 0) {
      error = "event buffer overflow (too many events in one block, including the organ setup events)";
      return false;
    }
    float* dst = &out.samples[static_cast<std::size_t>(b * kBlockFrames * 2)];
    for (std::uint32_t i = 0; i < kBlockFrames; ++i) {
      dst[2 * i] = l[i];
      dst[2 * i + 1] = r[i];
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
