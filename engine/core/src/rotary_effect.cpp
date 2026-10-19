#include "velora/core/rotary_effect.hpp"

#include <algorithm>
#include <cmath>

#include "velora/core/block_renderer.hpp"

namespace velora::core {

namespace rotmap {
float clamp01(float v) noexcept { return std::isnan(v) ? 0.5f : (v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v)); }
RotarySpeed speed(float norm) noexcept {
  const float n = clamp01(norm);
  return n < kStopBelow ? RotarySpeed::kStop : (n < kFastFrom ? RotarySpeed::kSlow : RotarySpeed::kFast);
}
float width(float norm) noexcept { return clamp01(norm) * 2.0f; }
}  // namespace rotmap

namespace {
constexpr float kWidthSmoothPerBlock = 0.125f;
}

RotaryEffect::RotaryEffect() noexcept {
  live_.fetch_add(1, std::memory_order_relaxed);
  set_params(kDefaultInsertParams);
}

RotaryEffect::~RotaryEffect() { live_.fetch_sub(1, std::memory_order_relaxed); }

void RotaryEffect::prepare(std::uint32_t sample_rate, std::uint32_t max_block_frames) noexcept {
  // The rotary unit is designed for the engine rate and block size; anything else is refused (pass-through; prepare_ok() tells).
  prepare_ok_ = sample_rate == kSampleRate && max_block_frames <= kFxBlockFrames;
  reset();
}

void RotaryEffect::reset() noexcept VELORA_NONBLOCKING {
  rot_.clear_signal();
  rot_.set_angles(0.0, 0.0);
  rot_.set_speed_immediate(speed_tgt_);
  width_cur_ = width_tgt_;
  started_ = false;  // a set_params() after a reset applies the speed and the width at once (settled start)
}

void RotaryEffect::set_params(const std::array<float, params::kInsertParamCount>& p) noexcept {
  speed_tgt_ = rotmap::speed(p[0]);
  width_tgt_ = rotmap::width(p[1]);
  if (!started_) {
    rot_.set_speed_immediate(speed_tgt_);
    width_cur_ = width_tgt_;
  } else {
    rot_.set_target(speed_tgt_);  // ramps with the horn and drum time constants
  }
}

void RotaryEffect::process(StereoBlock& block) noexcept {
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
  started_ = true;
  for (std::uint32_t i = 0; i < n; ++i) {
    const float l = std::isfinite(block.left[i]) ? block.left[i] : 0.0f;
    const float r = std::isfinite(block.right[i]) ? block.right[i] : 0.0f;
    mono_[i] = 0.5f * (l + r);
  }
  rot_.process(mono_.data(), wl_.data(), wr_.data(), n);
  const float w0 = width_cur_;
  if (width_cur_ != width_tgt_) {
    width_cur_ += (width_tgt_ - width_cur_) * kWidthSmoothPerBlock;
    if (std::fabs(width_tgt_ - width_cur_) < 1e-4f) width_cur_ = width_tgt_;
  }
  const float inv = 1.0f / static_cast<float>(n);
  for (std::uint32_t i = 0; i < n; ++i) {
    const float w = w0 + (width_cur_ - w0) * (static_cast<float>(i + 1) * inv);
    const float mid = 0.5f * (wl_[i] + wr_[i]), side = 0.5f * (wl_[i] - wr_[i]) * w;
    block.left[i] = mid + side;
    block.right[i] = mid - side;
  }
}

}  // namespace velora::core
