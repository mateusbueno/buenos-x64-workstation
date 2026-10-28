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

std::unique_ptr<Ins> make(bool stereo, bool filter, bool xfade, bool second = false, bool tuned = false) {
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
    if (tuned) {  // Story 13.8: key track, velocity track and a CC entry on the cutoff, a CC entry on the resonance: coefficients are computed per voice
      d.filter_tune.keytrack = 100.0f;
      d.filter_tune.veltrack = 1200.0f;
      d.filter_tune.cutoff[0] = {1, 0, 1200.0f};
      d.filter_tune.cutoff_count = 1;
      d.filter_tune.res[0] = {1, 0, 6.0f};
      d.filter_tune.res_count = 1;
    }
    if (second) {
      d.filter2_type = ports::SampleFilterType::kHpf2p;
      d.filter2_cutoff_hz = 200.0f;
      d.filter2_resonance_db = 3.0f;
    }
  }
  if (xfade) {
    d.xfin_count = 1;
    d.xfin[0] = {1, 0, 127};
  }
  t->regions.push_back(d);
  return t;
}

double us_per_block_per_voice(bool stereo, bool filter, bool xfade, int voices, bool second = false, bool tuned = false) {
  auto ins = make(stereo, filter, xfade, second, tuned);
  SampleEngine::Config c;
  c.max_voices = static_cast<std::uint32_t>(voices);
  c.extended_voices = true;
  c.second_filter_voices = second;
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

// Story 13.8: the cost of starting a voice (note on, coefficients included), in microseconds per start, best of 5 repeats of 2000 starts.
double us_per_start(bool filter, bool tuned, bool second) {
  auto ins = make(true, filter, false, second, tuned);
  SampleEngine::Config c;
  c.max_voices = 64;
  c.extended_voices = true;
  SampleEngine e(c);
  e.set_instrument(*ins);
  e.set_part_cc(0, 1, 0xC0000000u);
  double best = 1e30;
  float sink = 0;
  std::vector<float> l(64), r(64);
  for (int rep = 0; rep < 5; ++rep) {
    double total = 0;
    for (int i = 0; i < 2000; ++i) {
      const std::uint32_t note = 36 + static_cast<std::uint32_t>(i % 60);
      const auto t0 = std::chrono::steady_clock::now();
      e.start_voice(0, note, 40000 + static_cast<std::uint32_t>(i % 20000));
      const auto t1 = std::chrono::steady_clock::now();
      total += std::chrono::duration<double, std::micro>(t1 - t0).count();
      if (i % 8 == 7) {  // keep the pool from filling: end the voices
        for (std::uint32_t n = 36; n < 96; ++n) e.stop_voice(0, n);
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
        e.render(l.data(), r.data(), 64);
        sink += l[0];
      }
    }
    best = std::min(best, total / 2000.0);
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
  // Story 13.8: a second filter and a filter whose coefficients are computed per voice.
  const double two_s = us_per_block_per_voice(true, true, false, kVoices, true, false);
  const double tuned_s = us_per_block_per_voice(true, true, false, kVoices, false, true);
  const double two_tuned_s = us_per_block_per_voice(true, true, false, kVoices, true, true);
  const double two_xf_s = us_per_block_per_voice(true, true, true, kVoices, true, true);
  std::printf("  + 2nd filter stereo %7.3f us/block/voice -> %.2f units over the plain voice (one filter: %.2f; declared filter: %u)\n", two_s, (two_s - plain_s) / unit,
              (fil_s - plain_s) / unit, kSampleFilterCostUnits);
  std::printf("  + tuned filter (per-voice coefficients) stereo %7.3f us/block/voice -> %.2f units (render cost is the same as the plain filter: %.2f)\n", tuned_s,
              (tuned_s - plain_s) / unit, (fil_s - plain_s) / unit);
  std::printf("  tuned + 2nd filter (both per-voice) stereo %7.3f; with a crossfade %7.3f us/block/voice -> %.2f units over a zero base (declared: %u units with one filter, %u with the second, plain %u)\n",
              two_tuned_s, two_xf_s, (two_xf_s - plain_s) / unit + sample_voice_cost_units(SampleInterpolation::kHermite3), sample_voice_cost_units(SampleInterpolation::kHermite3, true),
              sample_voice_cost_units(SampleInterpolation::kHermite3, true, true), sample_voice_cost_units(SampleInterpolation::kHermite3));
  const double st_plain = us_per_start(false, false, false), st_filter = us_per_start(true, false, false), st_tuned = us_per_start(true, true, false), st_two = us_per_start(true, true, true);
  std::printf("  voice start: plain %.3f us, filter %.3f us, tuned filter %.3f us, tuned + 2nd filter %.3f us (coefficients: sin, cos, pow per filter, once per note)\n", st_plain, st_filter,
              st_tuned, st_two);
  // Deterministic checks only (no timing assertion): the extended declaration costs more than the plain one, for both interpolations.
  bool ok = true;
  for (auto q : {SampleInterpolation::kHermite3, SampleInterpolation::kLinear})
    ok = ok && sample_voice_cost_units(q, true) == sample_voice_cost_units(q) + kSampleFilterCostUnits + kSampleCrossfadeCostUnits &&
         sample_voice_cost_units(q, true, true) == sample_voice_cost_units(q, true) + kSampleFilter2CostUnits;
  if (!ok) std::printf("sample_bench: FAILED declared cost\n");
  return ok ? 0 : 1;
}
