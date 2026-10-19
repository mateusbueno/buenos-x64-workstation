// Host micro-benchmark for the tonewheel organ (Story 2.2). Prints cost per shared generator and per voice
// in microseconds per 64-frame block and as a share of the 1.333 ms block period. These are host figures,
// not representative of the dev board; the cost units in OrganEngine are placeholders until measured there.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "velora/core/organ_engine.hpp"
#include "velora/core/params_gen.hpp"

using namespace velora::core;

namespace {
double us_per_block_once(OrganEngine& e, int blocks) {
  std::vector<float> l(64), r(64);
  float sink = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (int b = 0; b < blocks; ++b) {
    std::fill(l.begin(), l.end(), 0.0f);
    std::fill(r.begin(), r.end(), 0.0f);
    e.begin_block(64);
    e.render(l.data(), r.data(), 64);
    sink += l[63];
  }
  const auto t1 = std::chrono::steady_clock::now();
  if (sink == 12345.678f) std::printf("%f\n", sink);  // keep the work alive
  return std::chrono::duration<double, std::micro>(t1 - t0).count() / blocks;
}
// Best of 5: the least disturbed repeat, so work running beside the benchmark cannot inflate the figure.
double us_per_block(OrganEngine& e, int blocks) {
  double best = us_per_block_once(e, blocks);
  for (int i = 1; i < 5; ++i) best = std::min(best, us_per_block_once(e, blocks));
  return best;
}
}  // namespace

int main() {
  constexpr int kBlocks = 4000;
  constexpr double kPeriodUs = 64.0 / 48000.0 * 1e6;
  auto setup = [](OrganEngine& e, std::uint32_t keys) {
    for (std::uint32_t d = 0; d < 9; ++d) e.set_part_param(0, params::kDrawbar16 + d, 8.0f);
    e.set_enabled_parts(1u);
    for (std::uint32_t n = 0; n < keys; ++n) e.start_voice(0, 28 + n, 0xFFFF);
  };
  OrganEngine idle;
  setup(idle, 0);
  const double gen = us_per_block(idle, kBlocks);
  OrganEngine full;
  setup(full, 73);
  const double all = us_per_block(full, kBlocks);
  const double per_voice = (all - gen) / 73.0;
  std::printf("organ benchmark (host, not representative of the dev board)\n");
  std::printf("  shared generator, 1 part, 91 wheels : %8.2f us/block  (%.2f%% of the block period)\n", gen, 100.0 * gen / kPeriodUs);
  std::printf("  voice, 9 drawbars                   : %8.3f us/block  (%.3f%%)\n", per_voice, 100.0 * per_voice / kPeriodUs);
  std::printf("  73 keys x 9 drawbars + generator    : %8.2f us/block  (%.2f%%)\n", all, 100.0 * all / kPeriodUs);
  std::printf("  declared cost units: voice %u, generator %u, reservation for one part %llu (placeholders)\n",
              OrganEngine::kVoiceCostUnits, OrganEngine::kGeneratorCostUnits,
              static_cast<unsigned long long>(OrganEngine::reservation_cost_units(1)));
  std::printf("  measured ratio generator : voice    = %.1f : 1 (declared %u : %u)\n", gen / per_voice,
              OrganEngine::kGeneratorCostUnits, OrganEngine::kVoiceCostUnits);
  // Sanity gate, deliberately loose: figures are finite and positive, and the full chord is far below
  // ten block periods on any host; a pathological regression fails.
  const bool sane = std::isfinite(gen) && std::isfinite(all) && std::isfinite(per_voice) && gen > 0 && all > 0 &&
                    per_voice > 0 && all < 10.0 * kPeriodUs;
  if (!sane) std::printf("organ_bench: FAILED sanity bound (all=%.2f us, limit %.2f us)\n", all, 10.0 * kPeriodUs);
  return sane ? 0 : 1;
}
