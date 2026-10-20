#include "velora/host/damage_registry.hpp"

#include <algorithm>

namespace velora::host {

void DamageRegistry::report(DamageEvent e) {
  std::function<void()> fn;
  {
    std::lock_guard<std::mutex> lock(mu_);
    ++reports_;
    if (std::find(libraries_.begin(), libraries_.end(), e.library_id) == libraries_.end()) libraries_.push_back(e.library_id);
    if (events_.size() < kMaxEvents) events_.push_back(std::move(e));
    fn = listener_;
  }
  if (fn) fn();
}

bool DamageRegistry::library_damaged(std::string_view library_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  return std::find(libraries_.begin(), libraries_.end(), library_id) != libraries_.end();
}

std::vector<std::string> DamageRegistry::damaged_libraries() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::string> v = libraries_;
  std::sort(v.begin(), v.end());
  return v;
}

std::vector<DamageEvent> DamageRegistry::events() const {
  std::lock_guard<std::mutex> lock(mu_);
  return events_;
}

std::uint64_t DamageRegistry::reports() const {
  std::lock_guard<std::mutex> lock(mu_);
  return reports_;
}

std::uint64_t DamageRegistry::seq_snapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  return reports_ + io_errors_ + clears_;
}

void DamageRegistry::report_io_error(std::string_view) {
  std::function<void()> fn;
  {
    std::lock_guard<std::mutex> lock(mu_);
    ++io_errors_;
    fn = listener_;
  }
  if (fn) fn();
}

std::uint64_t DamageRegistry::io_errors() const {
  std::lock_guard<std::mutex> lock(mu_);
  return io_errors_;
}

void DamageRegistry::clear_library(std::string_view library_id) {
  std::function<void()> fn;
  {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = std::find(libraries_.begin(), libraries_.end(), library_id);
    if (it == libraries_.end()) return;
    libraries_.erase(it);
    events_.erase(std::remove_if(events_.begin(), events_.end(), [&](const DamageEvent& e) { return e.library_id == library_id; }), events_.end());
    ++clears_;
    fn = listener_;
  }
  if (fn) fn();
}

void DamageRegistry::set_listener(std::function<void()> fn) {
  std::lock_guard<std::mutex> lock(mu_);
  listener_ = std::move(fn);
}

}  // namespace velora::host
