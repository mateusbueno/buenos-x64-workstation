// Story 6.5 RT safety: voices over PROTECTED samples render inside a nonblocking function while block-damage flags are flipped between blocks (the way the host's
// I/O edge thread sets them; the same flags flipped from a real second thread are exercised by engine/host/tests/rtsan_protected_test.cpp, which may link threads). RTSan aborts the process on any malloc/lock/blocking call in the render path: the damage check is one relaxed byte load per voice per frame and
// the counter is one relaxed fetch_add -- the audio thread never hashes, decrypts or waits. The decrypt itself is a non-RT operation by contract and is NOT part of
// this path; engine/host/tests/rtsan_protected_test.cpp renders a real protected instrument loaded through the key provider.
#include <atomic>
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

constexpr std::uint32_t kFrames = 40000, kBlocks = 10;

struct Instrument final : ports::SampleSource {
  std::vector<ports::SampleRegionDesc> regions;
  std::vector<ports::SamplePcm> pcm;
  std::vector<float> data;
  std::unique_ptr<std::atomic<std::uint8_t>[]> map = std::make_unique<std::atomic<std::uint8_t>[]>(kBlocks);
  ports::InstrumentView instrument() const noexcept override { return {regions, pcm}; }
  Instrument() {
    data.resize(kFrames);
    for (std::uint32_t i = 0; i < kFrames; ++i) data[i] = 0.2f * static_cast<float>(i % 211) / 211.0f - 0.1f;
    ports::SamplePcm p;
    p.channels = 1;
    p.frames = kFrames;
    p.left = data.data();
    p.damaged_blocks = map.get();
    p.damaged_block_frames = 4096;
    p.damaged_block_count = kBlocks;
    pcm.push_back(p);
    ports::SampleRegionDesc d;
    d.sample_index = 0;
    d.key_lo = 0;
    d.key_hi = 127;
    d.key_center = 60;
    d.loop_mode = ports::SampleLoopMode::kLoopContinuous;
    d.ampeg_release = 0.02f;
    regions.push_back(d);
    d.loop_mode = ports::SampleLoopMode::kNoLoop;  // a one-shot too
    d.key_lo = 90;
    regions.push_back(d);
  }
};

void run_block(BlockRenderer& br, float* l, float* r, std::uint32_t i) noexcept [[clang::nonblocking]] {
  br.submit({make_note_on(0, 0, 36 + (i * 7) % 61, 20000 + (i * 977) % 40000), (i * 5) % 60});
  br.submit({make_note_off(0, 0, 36 + ((i + 23) * 7) % 61, 0), (i * 11) % 60});
  if (i % 40 == 0) br.submit({make_note_on(0, 0, 92, 50000), 3});
  br.process_block(l, r);
}

}  // namespace

int main() {
  Instrument ins;
  SampleEngine::Config cfg;
  cfg.max_voices = 48;
  auto se = std::make_unique<SampleEngine>(cfg);
  se->set_instrument(ins);
  auto br = std::make_unique<BlockRenderer>();
  br->bind_engine(*se);
  for (std::uint32_t ch = 0; ch < 16; ++ch) br->set_channel_engine(ch, static_cast<std::uint32_t>(EngineTypeId::kSample));

  float l[kBlockFrames], r[kBlockFrames];
  double energy = 0.0;
  std::uint32_t peak_voices = 0;
  for (std::uint32_t i = 0; i < 6000; ++i) {
    // The "edge thread" part, outside the real-time function: block flags go up and down while voices play them.
    if (i % 3 == 0) {
      ins.map[(i / 3 * 3) % kBlocks].store(1, std::memory_order_relaxed);
      ins.map[(i / 3 * 5 + 1) % kBlocks].store(0, std::memory_order_relaxed);
    }
    run_block(*br, l, r, i);
    for (std::uint32_t k = 0; k < kBlockFrames; ++k) energy += static_cast<double>(l[k]) * l[k];
    peak_voices = std::max(peak_voices, se->active_voices());
  }
  const std::uint64_t events = se->content_integrity_events();
  std::printf("rtsan_integrity_test: events=%llu published=%llu peak_voices=%u energy=%.3g\n", (unsigned long long)events,
              (unsigned long long)br->last_values().load_content_integrity(), peak_voices, energy);
  int rc = 0;
  auto need = [&](bool cond, const char* what) {
    if (!cond) {
      std::printf("rtsan_integrity_test: FAILED condition: %s\n", what);
      rc = 1;
    }
  };
  need(events > 0, "voices met damaged blocks");
  need(br->last_values().load_content_integrity() == events, "the published counter equals the engine's");
  need(energy > 0.0, "audio was produced");
  need(peak_voices > 4, "several voices played");
  return rc;
}
