// 10,000 blocks of dense note + sustain-pedal traffic with stealing through the block loop and the
// VoiceArbiter, inside a nonblocking function. RTSan aborts the process on any malloc/lock/blocking call
// in the serial phase (allocation, victim selection, steal ramps, pedal handling, counters).
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/core/voice_arbiter.hpp"
#include "velora/ports/sample_source.hpp"

using namespace velora::core;
namespace ports = velora::ports;

namespace {

struct Instrument final : ports::SampleSource {
  std::vector<ports::SampleRegionDesc> regions;
  std::vector<ports::SamplePcm> pcm;
  std::vector<float> data;
  ports::InstrumentView instrument() const noexcept override { return {regions, pcm}; }
};

std::uint64_t g_over_budget = 0;

void run_block(BlockRenderer& br, float* l, float* r, std::uint32_t i) noexcept [[clang::nonblocking]] {
  const std::uint32_t ch = i % 3 == 0 ? 1u : 0u;  // 0: test tone, 1: sample engine
  const std::uint32_t note = 36 + (i * 7) % 61;
  const std::uint32_t vel = 1000 + (i * 977) % 64000;
  br.submit({make_note_on(0, ch, note, vel), (i * 5) % 200});
  br.submit({make_note_on(0, 1u - ch, note + 1, 65535), 17});
  br.submit({make_note_off(0, ch, 36 + ((i + 23) * 7) % 61), (i * 11) % 100});
  if (i % 60 == 0) br.submit({make_control_change(0, 0, 64, 0xFFFFFFFFu), 2});  // pedal down
  if (i % 60 == 30) br.submit({make_control_change(0, 0, 64, 0u), 9});          // pedal up
  if (i % 50 == 0)
    for (std::uint32_t k = 0; k < 40; ++k) br.submit({make_note_on(0, k % 2, 40 + k, 30000 + 500 * k), k});
  if (i % 400 == 0)
    for (std::uint32_t k = 36; k < 97; ++k) br.submit({make_note_off(0, k % 2, k), 3 + (k % 40)});
  br.process_block(l, r);
  const auto u = br.arbiter().usage();  // live + fading may exceed the voice budget by the fading cost only
  if (u.used() > br.arbiter().reservation().voice_budget + u.fading_cost) ++g_over_budget;
}

}  // namespace

int main() {
  Instrument ins;
  ins.data.assign(20000, 0.3f);
  ports::SamplePcm pc;
  pc.frames = ins.data.size();
  pc.left = ins.data.data();
  ins.pcm.push_back(pc);
  ports::SampleRegionDesc d;
  d.key_center = 66;
  ins.regions.push_back(d);

  SampleEngine::Config sc;
  sc.max_voices = 8 + VoiceArbiter::kFadeHeadroomVoices;
  auto se = std::make_unique<SampleEngine>(sc);
  se->set_instrument(ins);
  auto br = std::make_unique<BlockRenderer>(48);
  br->bind_engine(*se);
  br->set_channel_engine(1, static_cast<std::uint32_t>(EngineTypeId::kSample));

  VoiceArbiter::LiveSetConfig cfg;
  br->arbiter().set_min_sample_floor_for_tests(0);
  cfg.fx_cost = 10;
  cfg.organ_rotary_cost = 10;
  cfg.floors[cfg.floor_count++] = {static_cast<std::uint32_t>(EngineTypeId::kSample), 8};
  cfg.budget = 20 + 8 * 5 + 40;  // 40 shared units for 48+48 slots: the pool is always under pressure
  if (br->arbiter().reserve(cfg) != VoiceArbiter::ReserveStatus::kOk) {
    std::printf("rtsan_arbiter_test: FAILED reserve\n");
    return 1;
  }

  float l[kBlockFrames], r[kBlockFrames];
  double energy = 0.0;
  for (std::uint32_t i = 0; i < 10000; ++i) {
    run_block(*br, l, r, i);
    for (std::uint32_t k = 0; k < kBlockFrames; ++k) energy += static_cast<double>(l[k]) * l[k];
  }
  const auto& a = br->arbiter();
  std::printf("rtsan_arbiter_test: ok=%llu steals=%llu tone=%llu sample=%llu exhausted=%llu energy=%.3g\n",
              (unsigned long long)br->status_count(EngineStatus::kOk), (unsigned long long)a.steals(),
              (unsigned long long)a.steals(static_cast<std::uint32_t>(EngineTypeId::kTestTone)),
              (unsigned long long)a.steals(static_cast<std::uint32_t>(EngineTypeId::kSample)),
              (unsigned long long)a.exhausted(), energy);
  return (g_over_budget == 0 && a.steals() > 100 && energy > 0.0 && br->status_count(EngineStatus::kOk) > 0) ? 0 : 1;
}
