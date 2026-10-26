#include "velora/host/velocity_curve_holder.hpp"

#include <algorithm>

namespace velora::host {

void VelocityCurveHolder::attach(KeybedReader& r) {
  std::lock_guard<std::mutex> lk(mu_);
  if (std::find(readers_.begin(), readers_.end(), &r) == readers_.end()) readers_.push_back(&r);
  r.set_velocity_curve(curve());
}

void VelocityCurveHolder::detach(KeybedReader& r) {
  std::lock_guard<std::mutex> lk(mu_);
  readers_.erase(std::remove(readers_.begin(), readers_.end(), &r), readers_.end());
}

void VelocityCurveHolder::set(VelocityCurve c) {
  std::lock_guard<std::mutex> lk(mu_);
  curve_.store(static_cast<std::uint8_t>(c), std::memory_order_relaxed);
  for (KeybedReader* r : readers_) r->set_velocity_curve(c);
}

std::size_t VelocityCurveHolder::attached() const {
  std::lock_guard<std::mutex> lk(mu_);
  return readers_.size();
}

}  // namespace velora::host
