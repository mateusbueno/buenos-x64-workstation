#include "velora/host/render_driver.hpp"

namespace velora::host {

void RenderDriver::render_block(float* left, float* right, std::uint64_t block_start_ns) noexcept {
  // A beyond-horizon event is held (with everything behind it) until it is within the horizon, so
  // events are never reordered or lost. It is counted once, when first held.
  for (;;) {
    if (!has_held_) {
      if (!queue_.pop(held_)) break;
      has_held_ = true;
      held_counted_ = false;
    }
    const MappedEvent m = mapper_.map(held_.host_ns, block_start_ns);
    if (m.timing == EventTiming::kBeyondHorizon) {
      if (!held_counted_) {
        counters_.beyond_horizon.fetch_add(1, std::memory_order_relaxed);
        held_counted_ = true;
      }
      break;
    }
    if (m.timing == EventTiming::kLate) counters_.late.fetch_add(1, std::memory_order_relaxed);
    else if (m.timing == EventTiming::kDeferred) counters_.deferred.fetch_add(1, std::memory_order_relaxed);
    counters_.last_frame.store(m.frame, std::memory_order_relaxed);
    if (renderer_.submit(core::TimedUmp{held_.ump, m.frame}))
      counters_.events.fetch_add(1, std::memory_order_relaxed);
    else
      counters_.renderer_rejected.fetch_add(1, std::memory_order_relaxed);
    has_held_ = false;
  }
  renderer_.process_block(left, right);
  counters_.blocks.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace velora::host
