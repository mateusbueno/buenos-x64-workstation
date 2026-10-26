#include "velora/host/velocity_curve.hpp"

#include <cmath>

namespace velora::host {

std::uint16_t velocity7_to_16(std::uint8_t v7) noexcept {
  if (v7 < 1) v7 = 1;
  if (v7 > 127) v7 = 127;
  return static_cast<std::uint16_t>((static_cast<std::uint32_t>(v7) * 65535u + 63u) / 127u);
}

std::uint8_t velocity16_to_7(std::uint16_t v16) noexcept {
  const std::uint32_t v = (static_cast<std::uint32_t>(v16) * 127u + 32767u) / 65535u;
  return static_cast<std::uint8_t>(v == 0 ? 1 : v);
}

double travel_position(const VelocityRange& r, std::uint64_t dt_ns) noexcept {
  const std::uint64_t fast = r.fast_ns ? r.fast_ns : 1;
  const std::uint64_t slow = r.slow_ns > fast ? r.slow_ns : fast + 1;
  if (dt_ns <= fast) return 1.0;
  if (dt_ns >= slow) return 0.0;
  const double s = std::log(static_cast<double>(slow) / static_cast<double>(dt_ns)) / std::log(static_cast<double>(slow) / static_cast<double>(fast));
  return s < 0.0 ? 0.0 : s > 1.0 ? 1.0 : s;
}

double apply_velocity_curve(VelocityCurve curve, double s) noexcept {
  switch (curve) {
    case VelocityCurve::kLight: return std::pow(s, 0.7);
    case VelocityCurve::kHeavy: return std::pow(s, 1.4);
    case VelocityCurve::kNormal:
    case VelocityCurve::kFixed:
    default: return s;
  }
}

std::uint16_t chain_velocity16(const VelocityRange& range, VelocityCurve curve, std::uint8_t fixed7, std::uint64_t dt_ns) noexcept {
  if (curve == VelocityCurve::kFixed) return velocity7_to_16(fixed7);
  const double c = apply_velocity_curve(curve, travel_position(range, dt_ns));
  const double v = static_cast<double>(kMinVelocity16) + std::floor(static_cast<double>(kMaxVelocity16 - kMinVelocity16) * c + 0.5);
  return static_cast<std::uint16_t>(v < kMinVelocity16 ? kMinVelocity16 : v > kMaxVelocity16 ? kMaxVelocity16 : v);
}

}  // namespace velora::host
