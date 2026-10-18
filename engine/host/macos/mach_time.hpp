#pragma once

#include <mach/mach_time.h>

#include <cstdint>

namespace velora::host::macos {

// Mach host time (ticks) <-> nanoseconds. The timebase is fixed for the process lifetime.
inline const mach_timebase_info_data_t& timebase() noexcept {
  static const mach_timebase_info_data_t tb = [] {
    mach_timebase_info_data_t t{};
    mach_timebase_info(&t);
    return t;
  }();
  return tb;
}

inline std::uint64_t host_ticks_to_ns(std::uint64_t ticks) noexcept {
  const auto& tb = timebase();
  return (ticks / tb.denom) * tb.numer + (ticks % tb.denom) * tb.numer / tb.denom;
}

inline std::uint64_t now_ns() noexcept { return host_ticks_to_ns(mach_absolute_time()); }

}  // namespace velora::host::macos
