// 10,000 blocks of organ note traffic, drawbar changes (UMP param events and CC) and pedal through the block
// loop with the arbiter reserved, inside a nonblocking function. RTSan aborts on any malloc/lock/blocking call
// in the generator hook, voices, drawbar ramps, param routing and arbiter.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>

#include "velora/core/block_renderer.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/params_gen.hpp"

using namespace velora::core;

namespace {

void run_block(BlockRenderer& br, float* l, float* r, std::uint32_t i) noexcept [[clang::nonblocking]] {
  const std::uint32_t part = i % 3;
  const std::uint32_t note = 28 + (i * 7) % 73;
  br.submit({make_note_on(0, part, note, 1000 + (i * 977) % 64000), (i * 5) % 64});
  br.submit({make_note_off(0, part, 28 + ((i + 31) * 7) % 73), (i * 11) % 64});
  if (i % 4 == 0) {  // a drawbar change at a varying in-block offset
    const params::ParamDef* d = params::find(params::kDrawbar16 + i % 9);
    br.submit({make_param_event(*d, part, static_cast<double>((i / 4) % 9)), (i * 13) % 64});
  }
  if (i % 9 == 0) {
    Ump ev;
    if (cc_to_param_event(make_control_change(0, part, 70 + i % 9, 0x10000000u * (i % 16)), ev)) br.submit({ev, 7});
  }
  if (i % 6 == 0) {  // percussion on/off, harmonic, decay, volume and key click (Story 2.3): CC 80, 84, 85, 86, 82
    static constexpr std::uint32_t kPercCc[5] = {80, 84, 85, 86, 82};
    Ump ev;
    const std::uint32_t k = (i / 6) % 5;
    if (cc_to_param_event(make_control_change(0, part, kPercCc[k], ((i / 30) % 3) * 0x7FFFFFFFu), ev)) br.submit({ev, 21});
  }
  if (i % 700 == 0) br.submit({make_control_change(0, 0, 64, 0xFFFFFFFFu), 2});  // pedal down
  if (i % 700 == 350) br.submit({make_control_change(0, 0, 64, 0u), 2});         // pedal up
  if (i % 500 == 0)
    for (std::uint32_t n = 28; n <= 100; ++n) br.submit({make_note_on(0, 3, n, 30000), n % 64});  // full chord burst
  if (i % 500 == 250)
    for (std::uint32_t n = 28; n <= 100; ++n) br.submit({make_note_off(0, 3, n), n % 64});
  br.process_block(l, r);
}

}  // namespace

int main() {
  auto organ = std::make_unique<OrganEngine>();
  auto br = std::make_unique<BlockRenderer>(256);
  br->bind_engine(*organ);
  for (std::uint32_t ch = 0; ch < 4; ++ch) br->set_channel_engine(ch, static_cast<std::uint32_t>(EngineTypeId::kOrgan));
  VoiceArbiter::LiveSetConfig cfg;
  cfg.organ_rotary_cost = OrganEngine::reservation_cost_units(4);
  cfg.budget = cfg.organ_rotary_cost + 256;
  int rc = 0;
  if (br->arbiter().reserve(cfg) != VoiceArbiter::ReserveStatus::kOk) {
    std::printf("rtsan_organ_test: reserve failed\n");
    return 1;
  }
  float l[kBlockFrames], r[kBlockFrames];
  double energy = 0.0;
  float peak = 0.0f;
  std::uint32_t peak_voices = 0;
  for (std::uint32_t i = 0; i < 10000; ++i) {
    run_block(*br, l, r, i);
    for (std::uint32_t k = 0; k < kBlockFrames; ++k) {
      energy += static_cast<double>(l[k]) * l[k];
      peak = std::max(peak, std::fabs(l[k]));
      if (!std::isfinite(l[k])) rc = 1;
    }
    peak_voices = std::max(peak_voices, organ->active_voices());
  }
  std::printf("rtsan_organ_test: energy=%.3g peak=%.3f peak_voices=%u organ_steals=%llu rejected=%llu\n", energy, peak,
              peak_voices, (unsigned long long)br->arbiter().steals(static_cast<std::uint32_t>(EngineTypeId::kOrgan)),
              (unsigned long long)br->param_events_rejected());
  if (!(energy > 0.0) || peak > 4.0f || peak_voices < 73 ||
      br->arbiter().steals(static_cast<std::uint32_t>(EngineTypeId::kOrgan)) != 0)
    rc = 1;
  return rc;
}
