#include "velora/core/eq.hpp"

#include <algorithm>
#include <cmath>
#include <complex>

#include "velora/core/block_renderer.hpp"

namespace velora::core {

static_assert(kEqSampleRate == kSampleRate, "the EQ is designed at the engine rate");

namespace eqmap {
float clamp01(float v) noexcept { return std::isnan(v) ? 0.5f : (v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v)); }
float gain_db(float norm) noexcept {
  const float n = clamp01(norm);
  return n == 0.5f ? 0.0f : kMinGainDb + (kMaxGainDb - kMinGainDb) * n;
}
float mid_hz(float norm) noexcept {
  return kMinMidHz * std::pow(kMaxMidHz / kMinMidHz, clamp01(norm));
}
}  // namespace eqmap

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = static_cast<double>(kEqSampleRate);
constexpr float kSmoothPerBlock = 0.125f;  // one-pole step per 64-frame block: ~8 blocks = 10.7 ms time constant
constexpr float kSnapDb = 5e-4f;
constexpr float kSnapRel = 1e-4f;

float clamp_gain(float g) noexcept {
  if (std::isnan(g)) return 0.0f;
  return std::clamp(g, eqmap::kMinGainDb, eqmap::kMaxGainDb);
}
float clamp_freq(float f) noexcept {
  if (std::isnan(f)) return 1000.0f;
  return std::clamp(f, eqmap::kMinMidHz, eqmap::kMaxMidHz);
}

// RBJ cookbook, coefficients normalised by a0.
struct Raw {
  double b0, b1, b2, a1, a2;
};
// Peak: the analog RBJ prototype H(s) = (s^2 + s A/Q + 1) / (s^2 + s/(A Q) + 1) realised with M. Vicanek's "Matched Second Order
// Digital Filters" (2016): the poles are the impulse-invariant ones of the prototype (damping 1/(2 A Q)) and the zeros are chosen so the
// digital magnitude equals the analog one at DC, at the centre frequency and at Nyquist. That removes the bilinear warp that pulled
// the response above a high centre down by up to 2.8 dB (RBJ's own bilinear peak is no longer used).
Raw peak(double db, double f0, double q) {
  const double A = std::pow(10.0, db / 40.0), G = A * A, w0 = 2.0 * kPi * f0 / kFs;
  const double qd = 1.0 / (2.0 * A * q);  // pole damping
  const double e = std::exp(-qd * w0);
  const double a1 = qd <= 1.0 ? -2.0 * e * std::cos(std::sqrt(1.0 - qd * qd) * w0) : -2.0 * e * std::cosh(std::sqrt(qd * qd - 1.0) * w0);
  const double a2 = e * e;
  const double A0 = (1.0 + a1 + a2) * (1.0 + a1 + a2), A1 = (1.0 - a1 + a2) * (1.0 - a1 + a2), A2 = -4.0 * a2;
  const double s = std::sin(w0 / 2.0) * std::sin(w0 / 2.0), p0 = 1.0 - s, p1 = s, p2 = 4.0 * p0 * p1;
  const double R1 = (A0 * p0 + A1 * p1 + A2 * p2) * G * G, R2 = (-A0 + A1 + 4.0 * (p0 - p1) * A2) * G * G;
  const double B0 = A0, B2 = (R1 - R2 * p1 - B0) / (4.0 * p1 * p1), B1 = R2 + B0 + 4.0 * (p1 - p0) * B2;
  const double W = (std::sqrt(B0) + std::sqrt(std::max(B1, 0.0))) / 2.0;
  const double b0 = (W + std::sqrt(std::max(W * W + B2, 0.0))) / 2.0;
  const double b1 = (std::sqrt(B0) - std::sqrt(std::max(B1, 0.0))) / 2.0;
  const double b2 = -B2 / (4.0 * b0);
  return {b0, b1, b2, a1, a2};
}
Raw shelf(bool high, double db, double f0) {
  const double A = std::pow(10.0, db / 40.0), w0 = 2.0 * kPi * f0 / kFs, c = std::cos(w0);
  const double al = std::sin(w0) / 2.0 * std::sqrt(2.0);  // S = 1
  const double r = 2.0 * std::sqrt(A) * al;
  if (!high) {
    const double a0 = (A + 1) + (A - 1) * c + r;
    return {A * ((A + 1) - (A - 1) * c + r) / a0, 2 * A * ((A - 1) - (A + 1) * c) / a0, A * ((A + 1) - (A - 1) * c - r) / a0,
            -2 * ((A - 1) + (A + 1) * c) / a0, ((A + 1) + (A - 1) * c - r) / a0};
  }
  const double a0 = (A + 1) - (A - 1) * c + r;
  return {A * ((A + 1) + (A - 1) * c + r) / a0, -2 * A * ((A - 1) + (A + 1) * c) / a0, A * ((A + 1) + (A - 1) * c - r) / a0,
          2 * ((A - 1) - (A + 1) * c) / a0, ((A + 1) - (A - 1) * c - r) / a0};
}
}  // namespace

EqEffect::EqEffect() noexcept {
  freq_cur_ = freq_tgt_ = 1000.0f;
  design();
}

void EqEffect::prepare(std::uint32_t, std::uint32_t) noexcept { reset(); }

void EqEffect::reset() noexcept VELORA_NONBLOCKING {
  state_ = {};
  active_ = {};
}

void EqEffect::set_params(const std::array<float, params::kInsertParamCount>& p) noexcept {
  set_physical(eqmap::gain_db(p[0]), eqmap::gain_db(p[1]), eqmap::mid_hz(p[2]), eqmap::gain_db(p[3]), /*instant=*/true);
}

void EqEffect::set_physical(float low_db, float mid_db, float mid_hz, float high_db, bool instant) noexcept VELORA_NONBLOCKING {
  gain_tgt_ = {clamp_gain(low_db), clamp_gain(mid_db), clamp_gain(high_db)};
  freq_tgt_ = clamp_freq(mid_hz);
  if (instant) {
    gain_cur_ = gain_tgt_;
    freq_cur_ = freq_tgt_;
    moving_ = false;
    design();
  } else {
    moving_ = true;
  }
}

void EqEffect::design() noexcept VELORA_NONBLOCKING {
  coef_[0] = coef_[1] = coef_[2] = Coef{};
  if (gain_cur_[0] != 0.0f) {
    const Raw r = shelf(false, gain_cur_[0], eqmap::kLowShelfHz);
    coef_[0] = {r.b0, r.b1, r.b2, r.a1, r.a2};
  }
  if (gain_cur_[1] != 0.0f) {
    const Raw r = peak(gain_cur_[1], freq_cur_, eqmap::kMidQ);
    coef_[1] = {r.b0, r.b1, r.b2, r.a1, r.a2};
  }
  if (gain_cur_[2] != 0.0f) {
    const Raw r = shelf(true, gain_cur_[2], eqmap::kHighShelfHz);
    coef_[2] = {r.b0, r.b1, r.b2, r.a1, r.a2};
  }
}

void EqEffect::step_smoothing() noexcept VELORA_NONBLOCKING {
  bool still = false;
  for (int i = 0; i < 3; ++i) {
    float& c = gain_cur_[static_cast<std::size_t>(i)];
    const float t = gain_tgt_[static_cast<std::size_t>(i)];
    if (c != t) {
      c += (t - c) * kSmoothPerBlock;
      if (std::fabs(t - c) < kSnapDb) c = t;
      still = still || c != t;
    }
  }
  if (freq_cur_ != freq_tgt_) {
    freq_cur_ *= std::pow(freq_tgt_ / freq_cur_, kSmoothPerBlock);
    if (std::fabs(freq_tgt_ - freq_cur_) < kSnapRel * freq_tgt_) freq_cur_ = freq_tgt_;
    still = still || freq_cur_ != freq_tgt_;
  }
  moving_ = still;
  design();
}

void EqEffect::process(StereoBlock& block) noexcept {
  if (moving_) step_smoothing();
  float* ch[2] = {block.left, block.right};
  for (std::size_t b = 0; b < 3; ++b) {
    if (gain_cur_[b] == 0.0f) {  // identity band: skipped, state cleared so it starts clean when it leaves 0 dB
      if (active_[b]) {
        state_[b] = {};
        active_[b] = false;
      }
      continue;
    }
    active_[b] = true;
    const Coef& k = coef_[b];
    State& s = state_[b];
    for (std::size_t c = 0; c < 2; ++c) {
      float* x = ch[c];
      double z1 = s.z1[c], z2 = s.z2[c];
      for (std::uint32_t i = 0; i < block.frames; ++i) {
        const double in = std::isfinite(x[i]) ? static_cast<double>(x[i]) : 0.0;
        const double y = k.b0 * in + z1;  // transposed direct form II
        z1 = k.b1 * in - k.a1 * y + z2;
        z2 = k.b2 * in - k.a2 * y;
        x[i] = static_cast<float>(y);
      }
      s.z1[c] = z1;
      s.z2[c] = z2;
    }
  }
}

double EqEffect::magnitude_db(double hz) const noexcept {
  const std::complex<double> z1 = std::polar(1.0, -2.0 * kPi * hz / kFs), z2 = z1 * z1;
  double db = 0.0;
  for (std::size_t b = 0; b < 3; ++b) {
    if (gain_cur_[b] == 0.0f) continue;
    const Coef& k = coef_[b];
    const std::complex<double> h = (k.b0 + k.b1 * z1 + k.b2 * z2) / (1.0 + k.a1 * z1 + k.a2 * z2);
    db += 20.0 * std::log10(std::abs(h));
  }
  return db;
}

}  // namespace velora::core
