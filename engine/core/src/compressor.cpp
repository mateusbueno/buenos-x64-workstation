#include "velora/core/compressor.hpp"

#include <algorithm>
#include <cmath>

#include "velora/core/block_renderer.hpp"

namespace velora::core {

namespace compmap {
namespace {
float c01(float v, float dflt) noexcept { return std::isnan(v) ? dflt : (v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v)); }
}  // namespace
float threshold_db(float n) noexcept { return kMinThresholdDb + (kMaxThresholdDb - kMinThresholdDb) * c01(n, 0.5f); }
float ratio(float n) noexcept {
  const float c = c01(n, 0.5f);
  return c == 0.0f ? 1.0f : std::pow(kMaxRatio, c);
}
float attack_ms(float n) noexcept { return kMinAttackMs * std::pow(kMaxAttackMs / kMinAttackMs, c01(n, 0.5f)); }
float release_ms(float n) noexcept { return kMinReleaseMs * std::pow(kMaxReleaseMs / kMinReleaseMs, c01(n, 0.5f)); }
float makeup_db(float n) noexcept { return kMaxMakeupDb * c01(n, 0.0f); }
}  // namespace compmap

namespace {
constexpr double kFs = static_cast<double>(kSampleRate);
constexpr double kLn10Over20 = 0.11512925464970229;  // ln(10) / 20
constexpr double kSnapDb = 1e-4;                      // |g| below this snaps to 0 while the target is 0 (reaches identity)

float clampf(float v, float lo, float hi, float dflt) noexcept { return std::isnan(v) ? dflt : std::clamp(v, lo, hi); }
}  // namespace

CompressorEffect::CompressorEffect() noexcept { set_times(); }

void CompressorEffect::set_times() noexcept VELORA_NONBLOCKING {
  att_coef_ = std::exp(-1.0 / (static_cast<double>(attack_ms_) * 1e-3 * kFs));
  rel_coef_ = std::exp(-1.0 / (static_cast<double>(release_ms_) * 1e-3 * kFs));
}

void CompressorEffect::prepare(std::uint32_t, std::uint32_t) noexcept { reset(); }

void CompressorEffect::reset() noexcept VELORA_NONBLOCKING {
  g_db_ = 0.0;
  makeup_cur_ = makeup_tgt_;
  gr_published_.store(0.0f, std::memory_order_relaxed);
}

void CompressorEffect::set_params(const std::array<float, params::kInsertParamCount>& p) noexcept {
  set_physical(compmap::threshold_db(p[0]), compmap::ratio(p[1]), compmap::attack_ms(p[2]), compmap::release_ms(p[3]),
               compmap::makeup_db(p[4]), /*instant=*/true);
}

void CompressorEffect::set_physical(float threshold_db, float ratio, float attack_ms, float release_ms, float makeup_db,
                                    bool instant) noexcept VELORA_NONBLOCKING {
  threshold_ = clampf(threshold_db, compmap::kMinThresholdDb, compmap::kMaxThresholdDb, -12.0f);
  ratio_ = clampf(ratio, compmap::kMinRatio, compmap::kMaxRatio, 1.0f);
  attack_ms_ = clampf(attack_ms, compmap::kMinAttackMs, compmap::kMaxAttackMs, 10.0f);
  release_ms_ = clampf(release_ms, compmap::kMinReleaseMs, compmap::kMaxReleaseMs, 100.0f);
  makeup_tgt_ = clampf(makeup_db, 0.0f, compmap::kMaxMakeupDb, 0.0f);
  if (instant) makeup_cur_ = makeup_tgt_;
  set_times();
}

void CompressorEffect::process(StereoBlock& block) noexcept {
  const std::uint32_t n = block.frames;
  if (n == 0) return;
  if (is_identity()) return;
  const double k = ratio_ > 1.0f ? 1.0 - 1.0 / static_cast<double>(ratio_) : 0.0;  // slope of the static curve above threshold
  const double thr_lin = std::pow(10.0, static_cast<double>(threshold_) / 20.0);
  const double mk_step = (static_cast<double>(makeup_tgt_) - makeup_cur_) / static_cast<double>(n);
  double mk = makeup_cur_;
  double g = g_db_;
  for (std::uint32_t i = 0; i < n; ++i) {
    if (!std::isfinite(block.left[i])) block.left[i] = 0.0f;  // a non-finite sample is replaced by silence (like the EQ), never passed on
    if (!std::isfinite(block.right[i])) block.right[i] = 0.0f;
    const double l = std::fabs(static_cast<double>(block.left[i]));
    const double r = std::fabs(static_cast<double>(block.right[i]));
    const double x = std::max(l, r);
    double target = 0.0;
    if (x > thr_lin && k > 0.0) target = -(20.0 * std::log10(x) - static_cast<double>(threshold_)) * k;
    const double c = target < g ? att_coef_ : rel_coef_;
    g = target + c * (g - target);
    if (target == 0.0 && -g < kSnapDb) g = 0.0;
    mk += mk_step;
    const double total = g + mk;
    if (total != 0.0) {
      const float gain = static_cast<float>(std::exp(total * kLn10Over20));
      block.left[i] *= gain;
      block.right[i] *= gain;
    }
  }
  g_db_ = g;
  makeup_cur_ = makeup_tgt_;
  gr_published_.store(static_cast<float>(0.0 - g), std::memory_order_relaxed);
}

}  // namespace velora::core
