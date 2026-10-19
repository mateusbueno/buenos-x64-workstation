// 10,000 blocks of mixed note traffic through the block loop with the SampleEngine bound, inside a
// nonblocking function. RTSan aborts the process on any malloc/lock/blocking call in the render path
// (note on/off dispatch, region selection, round robin / random, voices, envelopes, loops, release
// triggers, pool exhaustion, parameter changes).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/ports/sample_source.hpp"

using namespace velora::core;
namespace ports = velora::ports;

namespace {

struct Instrument final : ports::SampleSource {
  std::vector<ports::SampleRegionDesc> regions;
  std::vector<ports::SamplePcm> pcm;
  std::vector<std::vector<float>> data;
  ports::InstrumentView instrument() const noexcept override { return {regions, pcm}; }

  std::uint32_t add(std::size_t frames, bool stereo, std::uint32_t rate, float f) {
    std::vector<float> buf(frames * (stereo ? 2 : 1));
    for (std::size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<float>((i * 37 % 211)) / 211.0f * f - 0.5f * f;
    data.push_back(std::move(buf));
    ports::SamplePcm p;
    p.sample_rate = rate;
    p.channels = stereo ? 2 : 1;
    p.frames = frames;
    p.left = data.back().data();
    p.right = stereo ? data.back().data() + frames : nullptr;
    pcm.push_back(p);
    return static_cast<std::uint32_t>(pcm.size() - 1);
  }
  ports::SampleRegionDesc& region(std::uint32_t sample, int lo, int hi) {
    ports::SampleRegionDesc d;
    d.sample_index = sample;
    d.key_lo = static_cast<std::uint8_t>(lo);
    d.key_hi = static_cast<std::uint8_t>(hi);
    d.key_center = static_cast<std::uint8_t>((lo + hi) / 2);
    regions.push_back(d);
    return regions.back();
  }
};

void run_block(BlockRenderer& br, SampleEngine& se, float* l, float* r, std::uint32_t i) noexcept [[clang::nonblocking]] {
  const std::uint32_t note = 36 + (i * 7) % 61;
  const std::uint32_t vel = 1000 + (i * 977) % 64000;
  br.submit({make_note_on(0, 0, note, vel), (i * 5) % 200});            // some deferred past the block
  br.submit({make_note_off(0, 0, 36 + ((i + 23) * 7) % 61), (i * 11) % 100});
  if (i % 3 == 0) br.submit({make_note_on(0, 0, note + 1, 65535), 17});   // chords
  if (i % 50 == 0)
    for (std::uint32_t k = 0; k < 40; ++k) br.submit({make_note_on(0, 0, 40 + k, 30000 + 500 * k), k});  // pool-full burst
  if (i % 400 == 0) for (std::uint32_t k = 36; k < 97; ++k) br.submit({make_note_off(0, 0, k), 3 + (k % 40)});
  if (i % 997 == 0) se.set_param(SampleEngine::kParamGain, 0.5f + 0.0001f * static_cast<float>(i % 7));
  if (i % 1999 == 0) se.set_param(SampleEngine::kParamSeed, static_cast<float>(i));
  br.process_block(l, r);
}

}  // namespace

int main() {
  Instrument ins;
  const auto mono = ins.add(9000, false, 48000, 1.0f);
  const auto stereo = ins.add(7000, true, 44100, 0.8f);
  const auto small = ins.add(1500, false, 48000, 0.6f);
  const auto lp = ins.add(12000, false, 48000, 0.7f);
  for (int p = 1; p <= 3; ++p) {  // round robin
    auto& r = ins.region(mono, 36, 60);
    r.seq_length = 3;
    r.seq_position = static_cast<std::uint32_t>(p);
    r.ampeg_attack = 0.002f;
    r.ampeg_release = 0.03f;
  }
  {
    auto& a = ins.region(stereo, 61, 96);
    a.lorand = 0.0f; a.hirand = 0.5f; a.pan = -40.0f; a.volume_db = -3.0f; a.ampeg_delay = 0.001f; a.ampeg_decay = 0.02f; a.ampeg_sustain = 40.0f;
    auto& b = ins.region(stereo, 61, 96);
    b.lorand = 0.5f; b.hirand = 1.0f; b.tune_cents = 20.0f; b.transpose = -2; b.offset = 100;
  }
  {
    auto& c = ins.region(lp, 40, 80);
    c.loop_mode = ports::SampleLoopMode::kLoopContinuous; c.has_loop_points = true; c.loop_start = 1000; c.loop_end = 8000;
    auto& d = ins.region(lp, 50, 90);
    d.loop_mode = ports::SampleLoopMode::kLoopSustain; d.has_loop_points = true; d.loop_start = 500; d.loop_end = 6000; d.ampeg_release = 0.1f;
    auto& e = ins.region(lp, 45, 70);
    e.loop_mode = ports::SampleLoopMode::kOneShot;
  }
  {
    auto& r1 = ins.region(small, 36, 96);
    r1.trigger = ports::SampleTrigger::kRelease;
    auto& r2 = ins.region(small, 50, 70);  // release round robin
    r2.trigger = ports::SampleTrigger::kRelease; r2.seq_length = 2; r2.seq_position = 2;
  }
  ins.region(99, 36, 96);                  // missing sample: skipped and counted

  SampleEngine::Config cfg;
  cfg.max_voices = 32;  // small enough that bursts exhaust the pool
  auto se = std::make_unique<SampleEngine>(cfg);
  se->set_instrument(ins);
  auto br = std::make_unique<BlockRenderer>();
  br->bind_engine(*se);
  for (std::uint32_t ch = 0; ch < 16; ++ch) br->set_channel_engine(ch, static_cast<std::uint32_t>(EngineTypeId::kSample));
  float l[kBlockFrames], r[kBlockFrames];
  double energy = 0.0;
  std::uint32_t peak_voices = 0;
  for (std::uint32_t i = 0; i < 10000; ++i) {
    run_block(*br, *se, l, r, i);
    for (std::uint32_t k = 0; k < kBlockFrames; ++k) energy += static_cast<double>(l[k]) * l[k] + static_cast<double>(r[k]) * r[k];
    peak_voices = std::max(peak_voices, se->active_voices());
  }
  const auto ok = br->status_count(EngineStatus::kOk);
  const auto full = br->status_count(EngineStatus::kPoolExhausted);
  const auto nf = br->status_count(EngineStatus::kVoiceNotFound);
  std::printf("rtsan_sample_engine_test: ok=%llu pool_exhausted=%llu voice_not_found=%llu peak_voices=%u skipped=%llu energy=%.3g\n",
              (unsigned long long)ok, (unsigned long long)full, (unsigned long long)nf, peak_voices,
              (unsigned long long)se->skipped_unplayable_starts(), energy);
  // voice_not_found is informational: with release regions on every key a note off without a held voice
  // reports kPoolExhausted (the release region found no free voice) rather than kVoiceNotFound.
  (void)nf;
  int rc = 0;
  auto need = [&](bool cond, const char* what) {
    if (!cond) {
      std::printf("rtsan_sample_engine_test: FAILED condition: %s\n", what);
      rc = 1;
    }
  };
  need(ok > 0, "ok > 0");
  need(full > 0, "pool_exhausted > 0");
  need(peak_voices == 32, "peak_voices == 32");
  need(se->skipped_unplayable_starts() > 0, "skipped_unplayable_starts > 0");
  need(energy > 0.0, "energy > 0");
  return rc;
}
