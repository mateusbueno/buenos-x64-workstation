// Host micro-benchmark for the sample engine voice (Story 5.2): the cost of a plain voice, a voice with a filter, one with a CC crossfade and
// one with both, in microseconds per 64-frame block per voice, converted to cost units with the plain Hermite voice = 5 units (the Story 1.15
// scale). These are host figures, not representative of the dev board; the declared units (kSampleFilterCostUnits, kSampleCrossfadeCostUnits)
// are placeholders until measured there (Stories 1.18/1.19). The program asserts nothing about time (a timing assertion in a normal test is
// noise): it prints the figures and checks only what is deterministic -- the declared cost ordering.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "velora/core/sample_engine.hpp"

using namespace velora::core;
namespace ports = velora::ports;

namespace {

struct Ins final : ports::SampleSource {
  std::vector<ports::SampleRegionDesc> regions;
  std::vector<ports::SamplePcm> pcm;
  std::vector<float> data;
  ports::InstrumentView instrument() const noexcept override { return {regions, pcm}; }
};

std::unique_ptr<Ins> make(bool stereo, bool filter, bool xfade) {
  auto t = std::make_unique<Ins>();
  const std::size_t n = 48000;
  t->data.resize(n * (stereo ? 2 : 1));
  for (std::size_t i = 0; i < t->data.size(); ++i) t->data[i] = 0.3f * std::sin(0.05f * static_cast<float>(i % n)) + 0.1f * std::sin(0.31f * static_cast<float>(i));
  ports::SamplePcm p;
  p.channels = stereo ? 2 : 1;
  p.frames = n;
  p.left = t->data.data();
  p.right = stereo ? t->data.data() + n : nullptr;
  t->pcm.push_back(p);
  ports::SampleRegionDesc d;
  d.key_lo = 0;
  d.key_hi = 127;
  d.key_center = 60;
  d.loop_mode = ports::SampleLoopMode::kLoopContinuous;
  if (filter) {
    d.filter_type = ports::SampleFilterType::kLpf2p;
    d.filter_cutoff_hz = 3000.0f;
    d.filter_resonance_db = 6.0f;
  }
  if (xfade) {
    d.xfin_count = 1;
    d.xfin[0] = {1, 0, 127};
  }
  t->regions.push_back(d);
  return t;
}

double us_per_block_per_voice(bool stereo, bool filter, bool xfade, int voices) {
  auto ins = make(stereo, filter, xfade);
  SampleEngine::Config c;
  c.max_voices = static_cast<std::uint32_t>(voices);
  c.extended_voices = true;
  SampleEngine e(c);
  e.set_instrument(*ins);
  e.set_part_cc(0, 1, 0xC0000000u);
  for (int v = 0; v < voices; ++v) e.start_voice(0, 40 + static_cast<std::uint32_t>(v % 48), 50000);
  std::vector<float> l(64), r(64);
  float sink = 0;
  double best = 1e30;
  for (int rep = 0; rep < 5; ++rep) {  // best of 5: the least disturbed repeat
    const auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < 1500; ++b) {
      std::fill(l.begin(), l.end(), 0.0f);
      std::fill(r.begin(), r.end(), 0.0f);
      e.render(l.data(), r.data(), 64);
      sink += l[63];
    }
    const auto t1 = std::chrono::steady_clock::now();
    best = std::min(best, std::chrono::duration<double, std::micro>(t1 - t0).count() / 1500.0 / voices);
  }
  if (sink == 12345.678f) std::printf("%f\n", sink);
  return best;
}

}  // namespace

int main() {
  constexpr int kVoices = 64;
  std::printf("sample engine voice benchmark (host, not representative of the dev board)\n");
  const double plain_m = us_per_block_per_voice(false, false, false, kVoices), plain_s = us_per_block_per_voice(true, false, false, kVoices);
  const double fil_m = us_per_block_per_voice(false, true, false, kVoices), fil_s = us_per_block_per_voice(true, true, false, kVoices);
  const double xf_m = us_per_block_per_voice(false, false, true, kVoices), xf_s = us_per_block_per_voice(true, false, true, kVoices);
  const double all_s = us_per_block_per_voice(true, true, true, kVoices);
  const double unit = plain_s / static_cast<double>(sample_voice_cost_units(SampleInterpolation::kHermite3));  // the stereo Hermite voice is the 5 units
  std::printf("  plain voice, mono   : %7.3f us/block/voice\n", plain_m);
  std::printf("  plain voice, stereo : %7.3f us/block/voice  (= %u units, so 1 unit = %.3f us)\n", plain_s,
              sample_voice_cost_units(SampleInterpolation::kHermite3), unit);
  std::printf("  + filter  mono %7.3f  stereo %7.3f us/block/voice  -> extra %.2f / %.2f units (declared %u)\n", fil_m, fil_s, (fil_m - plain_m) / unit,
              (fil_s - plain_s) / unit, kSampleFilterCostUnits);
  std::printf("  + xfade   mono %7.3f  stereo %7.3f us/block/voice  -> extra %.2f / %.2f units (declared %u)\n", xf_m, xf_s, (xf_m - plain_m) / unit,
              (xf_s - plain_s) / unit, kSampleCrossfadeCostUnits);
  std::printf("  filter + xfade, stereo %7.3f us/block/voice  -> %.2f units over the plain voice (declared extended voice: %u units)\n", all_s,
              (all_s - plain_s) / unit + sample_voice_cost_units(SampleInterpolation::kHermite3),
              sample_voice_cost_units(SampleInterpolation::kHermite3, true));
  // Deterministic checks only (no timing assertion): the extended declaration costs more than the plain one, for both interpolations.
  bool ok = true;
  for (auto q : {SampleInterpolation::kHermite3, SampleInterpolation::kLinear})
    ok = ok && sample_voice_cost_units(q, true) == sample_voice_cost_units(q) + kSampleFilterCostUnits + kSampleCrossfadeCostUnits;
  if (!ok) std::printf("sample_bench: FAILED declared cost\n");
  return ok ? 0 : 1;
}
