#include "render.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

#include "velora/core/block_renderer.hpp"

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
  const std::uint64_t last = events.empty() ? 0 : events.back().frame;
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
      const Ump u = e.on ? make_note_on(0, e.channel, e.note, e.velocity16)
                         : make_note_off(0, e.channel, e.note, e.velocity16);
      if (!br->submit({u, off})) {
        error = "event queue overflow (too many events in one block)";
        return false;
      }
    }
    for (std::uint32_t i = 0; i < kBlockFrames; ++i) { l[i] = 0.0f; r[i] = 0.0f; }
    br->process_block(l, r);
    float* dst = &out.samples[static_cast<std::size_t>(b * kBlockFrames * 2)];
    for (std::uint32_t i = 0; i < kBlockFrames; ++i) {
      dst[2 * i] = l[i];
      dst[2 * i + 1] = r[i];
    }
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
