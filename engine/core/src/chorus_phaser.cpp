#include "velora/core/chorus_phaser.hpp"

#include <algorithm>
#include <cmath>

#include "velora/core/block_renderer.hpp"

namespace velora::core {

namespace cpmap {
float clamp01(float v) noexcept { return std::isnan(v) ? 0.5f : (v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v)); }
ChorusPhaserMode mode(float norm) noexcept { return clamp01(norm) > kModeThreshold ? ChorusPhaserMode::kPhaser : ChorusPhaserMode::kChorus; }
float rate_hz(float norm) noexcept { return kMinRateHz * std::pow(kMaxRateHz / kMinRateHz, clamp01(norm)); }
float depth(float norm) noexcept { return clamp01(norm); }
float feedback(float norm) noexcept {
  const float n = clamp01(norm);
  return n == 0.5f ? 0.0f : (n - 0.5f) * 2.0f * kMaxFeedback;
}
float mix(float norm) noexcept { return clamp01(norm); }
}  // namespace cpmap

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr float kFs = static_cast<float>(kSampleRate);
constexpr float kSmoothPerBlock = 0.125f;  // one-pole step per 64-frame block (about 10 ms)
constexpr float kSnap = 1e-4f;
constexpr float kCentreSamples = ChorusPhaserEffect::kChorusCentreMs * 0.001f * kFs;
constexpr float kSwingSamples = ChorusPhaserEffect::kChorusSwingMs * 0.001f * kFs;
static_assert(kCentreSamples + kSwingSamples + 3.0f < static_cast<float>(ChorusPhaserEffect::kLineSize), "the longest delay fits the line");
static_assert(kCentreSamples - kSwingSamples >= 1.0f, "the shortest delay is at least one sample");

// Parabolic sine approximation of sin(2 pi p), p in cycles (any finite value): error below 0.1 %.
float lfo_sine(float p) noexcept {
  float x = p - std::floor(p);  // [0, 1)
  x = x >= 0.5f ? x - 1.0f : x;  // [-0.5, 0.5)
  const float y = 8.0f * x * (1.0f - 2.0f * std::fabs(x));
  return 0.225f * (y * std::fabs(y) - y) + y;
}

float approach(float cur, float tgt) noexcept {
  if (cur == tgt) return cur;
  cur += (tgt - cur) * kSmoothPerBlock;
  return std::fabs(tgt - cur) < kSnap ? tgt : cur;
}

// Sample `delay` samples (>= 1) behind the newest one (index pos), 4-point Lagrange.
float tap(const std::array<float, ChorusPhaserEffect::kLineSize>& line, std::uint32_t pos, float delay) noexcept {
  constexpr std::uint32_t mask = ChorusPhaserEffect::kLineSize - 1;
  const std::uint32_t i = static_cast<std::uint32_t>(delay);
  const float t = delay - static_cast<float>(i);
  const float a = line[(pos - (i - 1)) & mask], b = line[(pos - i) & mask];
  const float c = line[(pos - (i + 1)) & mask], d = line[(pos - (i + 2)) & mask];
  const float w0 = -t * (t - 1.0f) * (t - 2.0f) * (1.0f / 6.0f);
  const float w1 = (t + 1.0f) * (t - 1.0f) * (t - 2.0f) * 0.5f;
  const float w2 = -(t + 1.0f) * t * (t - 2.0f) * 0.5f;
  const float w3 = (t + 1.0f) * t * (t - 1.0f) * (1.0f / 6.0f);
  return a * w0 + b * w1 + c * w2 + d * w3;
}
}  // namespace

ChorusPhaserEffect::ChorusPhaserEffect() noexcept {
  set_params(kDefaultInsertParams);
  snap();
  clear_state();
}

void ChorusPhaserEffect::prepare(std::uint32_t sample_rate, std::uint32_t max_block_frames) noexcept {
  // The delays, the LFO and the all-pass coefficients are designed for the engine rate and block size; anything else is refused (the effect then leaves
  // every block untouched; prepare_ok() tells).
  prepare_ok_ = sample_rate == kSampleRate && max_block_frames <= kFxBlockFrames;
  reset();
}

void ChorusPhaserEffect::snap() noexcept VELORA_NONBLOCKING {
  rate_cur_ = rate_tgt_;
  depth_cur_ = depth_tgt_;
  fb_cur_ = fb_tgt_;
  mix_cur_ = mix_tgt_;
  w_ = mode_tgt_ == ChorusPhaserMode::kPhaser ? 1.0f : 0.0f;
}

void ChorusPhaserEffect::clear_state() noexcept VELORA_NONBLOCKING {
  for (Lines& c : ch_) {
    c.line.fill(0.0f);
    c.z.fill(0.0);
    c.fb = 0.0;
    c.a = 0.0;
  }
  pos_ = 0;
  hold_ = kCoefHold;
  ph_dirty_ = false;
}

void ChorusPhaserEffect::reset() noexcept VELORA_NONBLOCKING {
  clear_state();
  phase_ = 0.0;
  active_ = false;
  started_ = false;
  snap();
}

void ChorusPhaserEffect::set_params(const std::array<float, params::kInsertParamCount>& p) noexcept {
  mode_tgt_ = cpmap::mode(p[0]);
  rate_tgt_ = cpmap::rate_hz(p[1]);
  depth_tgt_ = cpmap::depth(p[2]);
  fb_tgt_ = cpmap::feedback(p[3]);
  mix_tgt_ = cpmap::mix(p[4]);
  if (!started_) snap();
}

void ChorusPhaserEffect::process(StereoBlock& block) noexcept {
  if (!prepare_ok_) return;
  if (block.frames > kFxBlockFrames) {  // oversized block: chunks of kFxBlockFrames (the state does not depend on the split)
    for (std::uint32_t off = 0; off < block.frames; off += kFxBlockFrames) {
      StereoBlock sub{block.left + off, block.right + off, std::min(kFxBlockFrames, block.frames - off)};
      process(sub);
    }
    return;
  }
  const std::uint32_t n = block.frames;
  if (n == 0) return;
  if (!started_) {
    snap();
    started_ = true;
  }
  if (is_identity()) {
    // Mix 0: the block stays untouched for finite input (non-finite samples become 0); the state is cleared once so the effect starts clean when
    // the mix leaves 0, the mode weight follows the mode target and the phaser coefficient is recomputed on re-entry.
    for (std::uint32_t i = 0; i < n; ++i) {
      if (!std::isfinite(block.left[i])) block.left[i] = 0.0f;
      if (!std::isfinite(block.right[i])) block.right[i] = 0.0f;
    }
    w_ = mode_tgt_ == ChorusPhaserMode::kPhaser ? 1.0f : 0.0f;
    hold_ = kCoefHold;
    if (active_) {
      clear_state();
      phase_ = 0.0;
      active_ = false;
    }
    return;
  }
  active_ = true;

  const float r0 = rate_cur_, d0 = depth_cur_, f0 = fb_cur_, m0 = mix_cur_;
  rate_cur_ = approach(rate_cur_, rate_tgt_);
  depth_cur_ = approach(depth_cur_, depth_tgt_);
  fb_cur_ = approach(fb_cur_, fb_tgt_);
  mix_cur_ = approach(mix_cur_, mix_tgt_);
  const float inv = 1.0f / static_cast<float>(n);
  const float w_tgt = mode_tgt_ == ChorusPhaserMode::kPhaser ? 1.0f : 0.0f;
  constexpr float kWStep = 1.0f / static_cast<float>(kModeRampFrames);
  constexpr double kLogHalfRange = 0.5 * 2.99573227355399099343;  // 0.5 * ln(4000 / 200)
  constexpr double kCentreHz = 894.427190999915878564;             // sqrt(200 * 4000)

  float* const chp[2] = {block.left, block.right};
  for (std::uint32_t i = 0; i < n; ++i) {
    const float t = static_cast<float>(i + 1) * inv;
    const float rate = r0 + (rate_cur_ - r0) * t, depth = d0 + (depth_cur_ - d0) * t, fb = f0 + (fb_cur_ - f0) * t,
                mix = m0 + (mix_cur_ - m0) * t;
    if (w_ < w_tgt) w_ = std::min(w_ + kWStep, w_tgt);
    else if (w_ > w_tgt) w_ = std::max(w_ - kWStep, w_tgt);
    const float w = w_;
    const bool do_chorus = w < 1.0f, do_phaser = w > 0.0f;
    if (!do_phaser && ph_dirty_) {
      for (Lines& c : ch_) {
        c.z.fill(0.0);
        c.fb = 0.0;
      }
      ph_dirty_ = false;
    }
    pos_ = (pos_ + 1) & (kLineSize - 1);
    if (!do_phaser) hold_ = kCoefHold;  // the coefficient is recomputed when the phaser weight leaves 0
    const bool coef_due = do_phaser && hold_ >= kCoefHold;
    if (coef_due) hold_ = 0;
    if (do_phaser) ++hold_;
    for (std::uint32_t c = 0; c < 2; ++c) {
      Lines& L = ch_[c];
      const float x = std::isfinite(chp[c][i]) ? chp[c][i] : 0.0f;
      const float lfo = lfo_sine(static_cast<float>(phase_) + (c == 0 ? 0.0f : kChannelPhaseOffset));
      L.line[pos_] = x;
      float wet = 0.0f;
      if (do_chorus) wet = (1.0f - w) * tap(L.line, pos_, kCentreSamples + kSwingSamples * depth * lfo);
      if (do_phaser) {
        if (coef_due) {
          const double f = kCentreHz * std::exp(static_cast<double>(depth) * kLogHalfRange * static_cast<double>(lfo));
          const double tn = std::tan(kPi * f / static_cast<double>(kFs));
          L.a = (tn - 1.0) / (tn + 1.0);
        }
        double v = static_cast<double>(x) + static_cast<double>(fb) * L.fb;
        for (std::uint32_t s = 0; s < kStages; ++s) {
          const double y = L.a * v + L.z[s];
          L.z[s] = v - L.a * y;
          v = y;
        }
        L.fb = v;
        wet += w * static_cast<float>(v);
        ph_dirty_ = true;
      }
      chp[c][i] = x + mix * (wet - x);
    }
    phase_ += static_cast<double>(rate) / static_cast<double>(kFs);
    if (phase_ >= 1.0) phase_ -= 1.0;
  }
}

}  // namespace velora::core
