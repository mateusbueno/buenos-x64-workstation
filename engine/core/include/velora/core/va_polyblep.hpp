#pragma once

#include <cmath>
#include <cstdint>

namespace velora::core {

// Polynomial Band-Limited Step (PolyBLEP) function.
// Derived from Välimäki & Huovilainen (IEEE Trans. Speech Audio 2006).
// Smooths step discontinuities over a 2-sample window [-dt, +dt].
inline float poly_blep(float t, float dt) noexcept {
  if (dt <= 0.0f) return 0.0f;
  if (t < dt) {
    t /= dt;
    return t + t - t * t - 1.0f;
  } else if (t > 1.0f - dt) {
    t = (t - 1.0f) / dt;
    return t * t + t + t + 1.0f;
  }
  return 0.0f;
}

// 32-bit xorshift pseudo-random generator for white noise (clean-room, Marsaglia 2003)
inline uint32_t xorshift32(uint32_t& state) noexcept {
  uint32_t x = state;
  if (x == 0) x = 0x12345678;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  state = x;
  return x;
}

// Generate normalized uniform white noise in [-1.0, +1.0]
inline float generate_white_noise(uint32_t& state) noexcept {
  uint32_t r = xorshift32(state);
  return (static_cast<float>(r) / 2147483648.0f) - 1.0f;
}

// 3-pole IIR filter for Pink Noise generation (Paul Kellet's filter)
class PinkNoiseFilter {
 public:
  void reset() noexcept {
    b0_ = b1_ = b2_ = b3_ = b4_ = b5_ = b6_ = 0.0f;
  }

  float process(float white) noexcept {
    b0_ = 0.99886f * b0_ + white * 0.0555179f;
    b1_ = 0.99332f * b1_ + white * 0.0750759f;
    b2_ = 0.96900f * b2_ + white * 0.1538520f;
    b3_ = 0.86650f * b3_ + white * 0.3104856f;
    b4_ = 0.55000f * b4_ + white * 0.5329522f;
    b5_ = -0.7616f * b5_ - white * 0.0168980f;
    float pink = (b0_ + b1_ + b2_ + b3_ + b4_ + b5_ + b6_ + white * 0.5362f) * 0.11f;
    b6_ = white * 0.115926f;
    return pink;
  }

 private:
  float b0_{0.0f}, b1_{0.0f}, b2_{0.0f}, b3_{0.0f}, b4_{0.0f}, b5_{0.0f}, b6_{0.0f};
};

} // namespace velora::core
