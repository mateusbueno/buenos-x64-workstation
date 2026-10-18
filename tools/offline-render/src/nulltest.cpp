#include "nulltest.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace velora::offline {
namespace {

struct Cut {
  double diff = 0.0, ref = 0.0;
  std::uint64_t overlap = 0;
};

// Overlap for offset k: ref frame i pairs with cand frame i+k.
bool range(std::uint64_t nr, std::uint64_t nc, std::int64_t k, std::uint64_t& lo, std::uint64_t& hi) {
  const std::int64_t a = std::max<std::int64_t>(0, -k);
  const std::int64_t b = std::min<std::int64_t>(static_cast<std::int64_t>(nr), static_cast<std::int64_t>(nc) - k);
  if (b <= a) return false;
  lo = static_cast<std::uint64_t>(a);
  hi = static_cast<std::uint64_t>(b);
  return true;
}

Cut measure(const WavData& ref, const WavData& cand, std::int64_t k, std::uint64_t lo, std::uint64_t hi) {
  Cut c;
  c.overlap = hi - lo;
  const std::size_t ch = ref.channels;
  for (std::uint64_t i = lo; i < hi; ++i) {
    for (std::size_t j = 0; j < ch; ++j) {
      const double a = ref.samples[i * ch + j];
      const double b = cand.samples[(static_cast<std::int64_t>(i) + k) * static_cast<std::int64_t>(ch) + j];
      c.ref += a * a;
      c.diff += (a - b) * (a - b);
    }
  }
  return c;
}

double to_db(double diff, double ref) {
  if (diff <= 0.0) return kFloorDb;
  if (ref <= 0.0) return -kFloorDb;
  return std::max(kFloorDb, 10.0 * std::log10(diff / ref));
}

}  // namespace

NullResult null_test(const WavData& ref, const WavData& cand, const NullParams& p) {
  NullResult r;
  r.length_diff_frames = static_cast<std::int64_t>(cand.frames()) - static_cast<std::int64_t>(ref.frames());
  if (ref.sample_rate != cand.sample_rate) {
    r.message = "sample rate mismatch (" + std::to_string(ref.sample_rate) + " vs " + std::to_string(cand.sample_rate) + ")";
    return r;
  }
  if (ref.channels != cand.channels) {
    r.message = "channel count mismatch (" + std::to_string(ref.channels) + " vs " + std::to_string(cand.channels) + ")";
    return r;
  }
  if (ref.frames() == 0 || cand.frames() == 0) {
    r.message = "empty audio";
    return r;
  }
  r.sample_rate = ref.sample_rate;
  double ref_total = 0.0, cand_total = 0.0;
  for (float x : ref.samples) {
    if (!std::isfinite(x)) { r.message = "reference contains non-finite samples"; return r; }
    ref_total += static_cast<double>(x) * x;
  }
  for (float x : cand.samples) {
    if (!std::isfinite(x)) { r.message = "candidate contains non-finite samples"; return r; }
    cand_total += static_cast<double>(x) * x;
  }
  r.comparable = true;
  const std::uint64_t ldiff = static_cast<std::uint64_t>(r.length_diff_frames < 0 ? -r.length_diff_frames : r.length_diff_frames);
  const bool length_ok = ldiff <= p.max_length_diff_frames;

  if (ref_total <= 0.0) {  // silent reference: judged on whole-file energy
    r.overlap_frames = std::min(ref.frames(), cand.frames());
    const bool silent = cand_total <= 0.0;
    r.residual_db = r.worst_window_db = silent ? kFloorDb : -kFloorDb;
    r.pass = silent && length_ok;
    r.message = !silent ? "reference is silent but candidate is not (fail)"
                        : !length_ok ? "length difference above maximum"
                                     : "reference is silent and candidate is silent";
    return r;
  }

  // Offsets in order of increasing magnitude so ties keep the smallest shift.
  const std::int64_t m = p.max_offset_frames;
  bool found = false;
  double best_ratio = std::numeric_limits<double>::infinity();
  std::int64_t best_k = 0;
  Cut best;
  for (std::int64_t mag = 0; mag <= m; ++mag) {
    for (int s = 0; s < (mag == 0 ? 1 : 2); ++s) {
      const std::int64_t k = s == 0 ? mag : -mag;
      std::uint64_t lo, hi;
      if (!range(ref.frames(), cand.frames(), k, lo, hi)) continue;
      const Cut c = measure(ref, cand, k, lo, hi);
      if (c.ref <= 0.0) continue;  // no reference signal in the overlap: cannot validate anything
      const double ratio = c.ref > 0.0 ? c.diff / c.ref : (c.diff > 0.0 ? std::numeric_limits<double>::infinity() : 0.0);
      if (!found || ratio < best_ratio) {
        found = true;
        best_ratio = ratio;
        best_k = k;
        best = c;
      }
    }
  }
  if (!found) {
    r.comparable = false;
    r.message = "no overlap containing reference signal within the offset range";
    return r;
  }
  r.offset_frames = best_k;
  r.overlap_frames = best.overlap;
  r.residual_db = to_db(best.diff, best.ref);

  // Worst window: residual energy per window against the reference's mean energy per window.
  std::uint64_t lo, hi;
  range(ref.frames(), cand.frames(), best_k, lo, hi);
  const std::size_t ch = ref.channels;
  const std::uint64_t w = std::max<std::uint32_t>(1, p.window_frames);
  const double mean_ref_window = best.ref * static_cast<double>(std::min<std::uint64_t>(w, best.overlap)) /
                                 static_cast<double>(best.overlap);
  double worst = -1.0;
  for (std::uint64_t s = lo; s < hi; s += w) {
    const std::uint64_t e = std::min(hi, s + w);
    double d = 0.0;
    for (std::uint64_t i = s; i < e; ++i)
      for (std::size_t j = 0; j < ch; ++j) {
        const double a = ref.samples[i * ch + j];
        const double b = cand.samples[(static_cast<std::int64_t>(i) + best_k) * static_cast<std::int64_t>(ch) + j];
        d += (a - b) * (a - b);
      }
    if (d > worst) {
      worst = d;
      r.worst_window_start = s;
    }
  }
  r.worst_window_db = to_db(std::max(worst, 0.0), mean_ref_window);

  r.pass = r.residual_db <= p.tolerance_db;
  if (!r.pass) r.message = "residual above tolerance";
  else if (!length_ok) { r.pass = false; r.message = "length difference above maximum"; }
  return r;
}

std::string format_report(const std::string& name, const NullResult& r, const NullParams& p) {
  char buf[512];
  const double rate = r.sample_rate ? r.sample_rate : 48000;
  std::string s = "file: " + name + "\n";
  if (!r.comparable) {
    s += "  result: FAIL (" + r.message + ")\n";
    return s;
  }
  std::snprintf(buf, sizeof buf,
                "  residual: %.2f dB (tolerance %.2f dB)\n"
                "  alignment offset: %lld frames (max %u)\n"
                "  worst %.0f ms window: %.2f dB at %.1f ms\n"
                "  compared frames: %llu (length difference %lld frames)\n",
                r.residual_db, p.tolerance_db, static_cast<long long>(r.offset_frames), p.max_offset_frames,
                1000.0 * p.window_frames / rate, r.worst_window_db,
                1000.0 * static_cast<double>(r.worst_window_start) / rate,
                static_cast<unsigned long long>(r.overlap_frames), static_cast<long long>(r.length_diff_frames));
  s += buf;
  s += std::string("  result: ") + (r.pass ? "PASS" : "FAIL") +
       (r.message.empty() ? "" : " (" + r.message + ")") + "\n";
  return s;
}

}  // namespace velora::offline
