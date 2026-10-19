// organ_wav_check full.wav alt.wav zero.wav: the 888000000 render is not silent, differs from the 008000000 render
// and from the all-zero-drawbar render, and the all-zero render is nearly silent (only the 5 ms ramp down from the default 8' remains). Exit 0 on success.
#include <cmath>
#include <cstdio>
#include <string>

#include "wav.hpp"

using namespace velora::offline;

static double rms(const WavData& w) {
  double e = 0;
  for (float x : w.samples) e += static_cast<double>(x) * x;
  return w.samples.empty() ? 0.0 : std::sqrt(e / static_cast<double>(w.samples.size()));
}
static double diff_rms(const WavData& a, const WavData& b) {
  if (a.samples.size() != b.samples.size()) return 1e9;
  double e = 0;
  for (std::size_t i = 0; i < a.samples.size(); ++i) e += std::pow(static_cast<double>(a.samples[i]) - b.samples[i], 2.0);
  return std::sqrt(e / static_cast<double>(a.samples.size()));
}

int main(int argc, char** argv) {
  if (argc != 4) return 2;
  WavData w[3];
  for (int i = 0; i < 3; ++i) {
    std::string err;
    if (!read_wav(argv[i + 1], w[i], err)) { std::printf("cannot read %s: %s\n", argv[i + 1], err.c_str()); return 1; }
  }
  const double full = rms(w[0]), alt = rms(w[1]), zero = rms(w[2]);
  const double d_alt = diff_rms(w[0], w[1]), d_zero = diff_rms(w[0], w[2]);
  std::printf("rms full=%g alt=%g zero=%g; diff(full,alt)=%g diff(full,zero)=%g\n", full, alt, zero, d_alt, d_zero);
  const bool ok = full > 1e-3 && alt > 1e-4 && zero < 0.05 * full && d_alt > 1e-3 && d_zero > 1e-3;
  std::printf("organ_wav_check: %s\n", ok ? "ok" : "FAILED");
  return ok ? 0 : 1;
}
