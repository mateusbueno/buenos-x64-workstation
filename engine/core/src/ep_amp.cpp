#include "velora/core/ep_amp.hpp"

#include <algorithm>
#include <cmath>

#include "velora/core/block_renderer.hpp"

namespace velora::core {

namespace epmap {
float clamp01(float v) noexcept { return std::isnan(v) ? 0.5f : (v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v)); }
float drive_db(float norm) noexcept { return kMaxDriveDb * clamp01(norm); }
float tone_db(float norm) noexcept {
  const float n = clamp01(norm);
  return n == 0.5f ? 0.0f : (n - 0.5f) * 2.0f * kTiltDb;
}
float presence_db(float norm) noexcept { return kMaxPresenceDb * clamp01(norm); }
float level_db(float norm) noexcept {
  const float n = clamp01(norm);
  return n == 0.5f ? 0.0f : (n - 0.5f) * 2.0f * kLevelRangeDb;
}
float cabinet(float norm) noexcept { return clamp01(norm); }
float shape(float x) noexcept {
  x = x > 3.0f ? 3.0f : (x < -3.0f ? -3.0f : x);  // at |x| = 3 the formula is exactly +-1
  const float x2 = x * x;
  const float y = x * (27.0f + x2) / (27.0f + 9.0f * x2);
  return y > 1.0f ? 1.0f : (y < -1.0f ? -1.0f : y);  // float rounding next to the knee must not overshoot
}
}  // namespace epmap

namespace {
constexpr float kInputLimit = 1.0e6f;
constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = static_cast<double>(kSampleRate);
constexpr float kSmoothPerBlock = 0.125f;
constexpr float kSnap = 1e-4f;

double bessel_i0(double x) noexcept {
  double sum = 1.0, term = 1.0;
  for (int k = 1; k < 60; ++k) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += term;
    if (term < 1e-18 * sum) break;
  }
  return sum;
}

float db_to_lin(float db) noexcept { return std::pow(10.0f, db * 0.05f); }

struct Raw {
  double b0, b1, b2, a1, a2;
};
// RBJ cookbook biquads (bilinear transform), coefficients normalised by a0.
Raw lowpass(double f0, double q, double fs) noexcept {
  const double w0 = 2.0 * kPi * f0 / fs, c = std::cos(w0), al = std::sin(w0) / (2.0 * q), a0 = 1.0 + al;
  return {(1.0 - c) * 0.5 / a0, (1.0 - c) / a0, (1.0 - c) * 0.5 / a0, -2.0 * c / a0, (1.0 - al) / a0};
}
Raw highpass(double f0, double q, double fs) noexcept {
  const double w0 = 2.0 * kPi * f0 / fs, c = std::cos(w0), al = std::sin(w0) / (2.0 * q), a0 = 1.0 + al;
  return {(1.0 + c) * 0.5 / a0, -(1.0 + c) / a0, (1.0 + c) * 0.5 / a0, -2.0 * c / a0, (1.0 - al) / a0};
}
Raw peaking(double db, double f0, double q, double fs) noexcept {
  const double A = std::pow(10.0, db / 40.0), w0 = 2.0 * kPi * f0 / fs, c = std::cos(w0), al = std::sin(w0) / (2.0 * q), a0 = 1.0 + al / A;
  return {(1.0 + al * A) / a0, -2.0 * c / a0, (1.0 - al * A) / a0, -2.0 * c / a0, (1.0 - al / A) / a0};
}
}  // namespace

template <std::uint32_t P>
HalfBand2x<P>::HalfBand2x() noexcept {
  // Half-band low-pass (cut-off a quarter of the high rate): h(0) = 1/2, h(k) = sin(pi k / 2) / (pi k) for odd k, Kaiser window, then the odd
  // taps are scaled so that the taps sum to exactly 1 (unity gain at DC).
  constexpr double kBeta = P >= 6 ? 8.0 : 6.0;
  constexpr int C = static_cast<int>(2 * P - 1);
  std::array<double, P> h{};
  double sum_odd = 0.0;
  for (std::uint32_t s = 1; s <= P; ++s) {
    const int k = static_cast<int>(2 * s - 1);
    const double r = static_cast<double>(k) / static_cast<double>(C + 1);  // window spans +-(C + 1): the end tap is non-zero
    const double w = bessel_i0(kBeta * std::sqrt(std::max(0.0, 1.0 - r * r))) / bessel_i0(kBeta);
    h[s - 1] = (k % 4 == 1 ? 1.0 : -1.0) / (kPi * k) * w;
    sum_odd += h[s - 1];
  }
  const double scale = 0.25 / sum_odd;  // 0.5 + 2 * scale * sum_odd = 1
  for (std::uint32_t s = 0; s < P; ++s) {
    h_[s] = static_cast<float>(h[s] * scale);
    c_[s] = static_cast<float>(2.0 * h[s] * scale);
  }
}

template <std::uint32_t P>
void HalfBand2x<P>::clear() noexcept {
  uw_.fill(0.0f);
  de_.fill(0.0f);
  do_.fill(0.0f);
}

// Linear work buffers hold [history (older samples) | new samples]. The loops run over the samples with unit stride (the outer loop is over the
// taps), so the compiler can vectorise them: no reduction across the loop index.
template <std::uint32_t P>
void HalfBand2x<P>::up(const float* in, std::uint32_t n, float* out) noexcept {
  float* w = uw_.data();
  for (std::uint32_t i = 0; i < n; ++i) w[kUpHist + i] = in[i];
  float* acc = acc_.data();
  for (std::uint32_t i = 0; i < n; ++i) acc[i] = 0.0f;
  // out[2i] = sum_s c_s (x[i - P + s] + x[i - (P - 1) - s]);  out[2i + 1] = x[i - (P - 1)]
  for (std::uint32_t s = 1; s <= P; ++s) {
    const float c = c_[s - 1];
    const float* a = w + kUpHist - (P - s);
    const float* b = w + kUpHist - (P - 1 + s);
    for (std::uint32_t i = 0; i < n; ++i) acc[i] += c * (a[i] + b[i]);
  }
  const float* mid = w + kUpHist - (P - 1);
  for (std::uint32_t i = 0; i < n; ++i) {
    out[2 * i] = acc[i];
    out[2 * i + 1] = mid[i];
  }
  for (std::uint32_t k = 0; k < kUpHist; ++k) w[k] = w[n + k];  // keep the newest kUpHist samples
}

template <std::uint32_t P>
void HalfBand2x<P>::down(const float* in, std::uint32_t n, float* out) noexcept {
  float* we = de_.data();
  float* wo = do_.data();
  for (std::uint32_t i = 0; i < n; ++i) {
    we[kEvenHist + i] = in[2 * i];
    wo[kOddHist + i] = in[2 * i + 1];
  }
  // out[i] = 0.5 ue[i - P + 1] + sum_s h_s (uo[i - (P - s)] + uo[i - (P + s - 1)])  (ue / uo: even / odd samples of the high-rate input)
  float* acc = acc_.data();
  const float* mid = we + kEvenHist - (P - 1);
  for (std::uint32_t i = 0; i < n; ++i) acc[i] = 0.5f * mid[i];
  for (std::uint32_t s = 1; s <= P; ++s) {
    const float h = h_[s - 1];
    const float* a = wo + kOddHist - (P - s);
    const float* b = wo + kOddHist - (P + s - 1);
    for (std::uint32_t i = 0; i < n; ++i) acc[i] += h * (a[i] + b[i]);
  }
  for (std::uint32_t i = 0; i < n; ++i) out[i] = acc[i];
  for (std::uint32_t k = 0; k < kEvenHist; ++k) we[k] = we[n + k];
  for (std::uint32_t k = 0; k < kOddHist; ++k) wo[k] = wo[n + k];
}

template class HalfBand2x<12>;
template class HalfBand2x<8>;
template class HalfBand2x<4>;
template class HalfBand2x<3>;

EpAmpEffect::EpAmpEffect() noexcept {
  const Raw hp = highpass(kHighPassHz, 0.70710678118654752440, kFs), ch = highpass(kCabHighPassHz, 0.70710678118654752440, kFs),
            cl = lowpass(kCabLowPassHz, 0.70710678118654752440, kFs);
  hp_ = {hp.b0, hp.b1, hp.b2, hp.a1, hp.a2};
  cab_hp_ = {ch.b0, ch.b1, ch.b2, ch.a1, ch.a2};
  cab_lp_ = {cl.b0, cl.b1, cl.b2, cl.a1, cl.a2};
  tilt_a_ = 1.0 - std::exp(-2.0 * kPi * kTiltHz / kFs);
  set_params(kDefaultInsertParams);
  snap();
}

void EpAmpEffect::prepare(std::uint32_t sample_rate, std::uint32_t max_block_frames) noexcept {
  // Filters and oversampler are designed for the engine rate and block size; anything else is refused (pass-through; prepare_ok() tells).
  prepare_ok_ = sample_rate == kSampleRate && max_block_frames <= kFxBlockFrames;
  reset();
}

void EpAmpEffect::clear_state() noexcept VELORA_NONBLOCKING {
  for (Channel& c : ch_) {
    c.hb1.clear();
    c.hb2.clear();
    c.hp = c.pres = c.cab_hp = c.cab_lp = BiquadState{};
    c.tilt_lp = 0.0;
  }
}

void EpAmpEffect::reset() noexcept VELORA_NONBLOCKING {
  started_ = false;
  clear_state();
  snap();
}

void EpAmpEffect::snap() noexcept VELORA_NONBLOCKING {
  cur_ = tgt_;
  design_presence();
}

void EpAmpEffect::design_presence() noexcept VELORA_NONBLOCKING {
  const Raw p = peaking(static_cast<double>(cur_[2]), kPresenceHz, kPresenceQ, kFs);
  pres_ = {p.b0, p.b1, p.b2, p.a1, p.a2};
}

void EpAmpEffect::set_params(const std::array<float, params::kInsertParamCount>& p) noexcept {
  tgt_ = {epmap::drive_db(p[0]), epmap::tone_db(p[1]), epmap::presence_db(p[2]), epmap::level_db(p[3]), epmap::cabinet(p[4])};
  if (!started_) snap();
}

void EpAmpEffect::process(StereoBlock& block) noexcept {
  if (!prepare_ok_) return;
  if (block.frames > kMaxFrames) {  // oversized block: chunks of kMaxFrames (the state does not depend on the split)
    for (std::uint32_t off = 0; off < block.frames; off += kMaxFrames) {
      StereoBlock sub{block.left + off, block.right + off, std::min(kMaxFrames, block.frames - off)};
      process(sub);
    }
    return;
  }
  const std::uint32_t n = block.frames;
  if (n == 0) return;
  started_ = true;
  const std::array<float, 5> old = cur_;
  for (std::size_t i = 0; i < 5; ++i) {
    if (cur_[i] != tgt_[i]) {
      cur_[i] += (tgt_[i] - cur_[i]) * kSmoothPerBlock;
      if (std::fabs(tgt_[i] - cur_[i]) < kSnap) cur_[i] = tgt_[i];
    }
  }
  if (cur_[2] != old[2]) design_presence();
  // Linear gains at the start and at the end of the block, interpolated per sample.
  const float g0 = db_to_lin(old[0]), g1 = db_to_lin(cur_[0]);
  const float c0 = 1.0f / std::sqrt(g0), c1 = 1.0f / std::sqrt(g1);  // loudness compensation 1 / sqrt(g)
  const float tl0 = db_to_lin(-old[1]), tl1 = db_to_lin(-cur_[1]), th0 = db_to_lin(old[1]), th1 = db_to_lin(cur_[1]);
  const float lv0 = db_to_lin(old[3]), lv1 = db_to_lin(cur_[3]);
  const float cb0 = old[4], cb1 = cur_[4];
  const float inv = 1.0f / static_cast<float>(n);

  float* const chp[2] = {block.left, block.right};
  float* const a1 = sa_.data();  // 1x
  float* const b2 = sb_.data();  // 2x
  for (std::uint32_t c = 0; c < 2; ++c) {
    Channel& ch = ch_[c];
    float* x = chp[c];
    // pre-gain into the oversampler
    for (std::uint32_t i = 0; i < n; ++i) {
      const float t = static_cast<float>(i + 1) * inv;
      // non-finite -> 0; huge finite values are limited so that gain and the oversampler's filters can never overflow to inf/NaN
      const float in = std::isfinite(x[i]) ? std::clamp(x[i], -kInputLimit, kInputLimit) : 0.0f;
      a1[i] = in * (g0 + (g1 - g0) * t);
    }
    // 1x -> 2x (into sb_) -> 4x (into sa_), shaper, 4x -> 2x (into sb_) -> 1x (into sa_)
    ch.hb1.up(a1, n, b2);
    float* const q4 = sa_.data();
    ch.hb2.up(b2, 2 * n, q4);
    for (std::uint32_t i = 0; i < 4 * n; ++i) q4[i] = epmap::shape(q4[i]);
    ch.hb2.down(q4, 2 * n, b2);
    ch.hb1.down(b2, n, a1);
    for (std::uint32_t i = 0; i < n; ++i) {
      const float t = static_cast<float>(i + 1) * inv;
      double y = static_cast<double>(a1[i]) * static_cast<double>(c0 + (c1 - c0) * t);
      // amp voicing
      y = ch.hp.run(hp_, y);
      ch.tilt_lp += tilt_a_ * (y - ch.tilt_lp);
      y = static_cast<double>(th0 + (th1 - th0) * t) * y + static_cast<double>((tl0 - th0) + ((tl1 - th1) - (tl0 - th0)) * t) * ch.tilt_lp;
      y = ch.pres.run(pres_, y);
      {  // the cabinet filters always run (primed), so raising CABINET from 0 starts from a settled state
        const double cab = ch.cab_lp.run(cab_lp_, ch.cab_hp.run(cab_hp_, y));
        y += static_cast<double>(cb0 + (cb1 - cb0) * t) * (cab - y);
      }
      x[i] = static_cast<float>(y * static_cast<double>(lv0 + (lv1 - lv0) * t));
    }
  }
}

}  // namespace velora::core
