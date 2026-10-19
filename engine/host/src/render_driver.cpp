#include "velora/host/render_driver.hpp"

namespace velora::host {

void RenderDriver::render_block(float* left, float* right, std::uint64_t block_start_ns) noexcept {
  drain(midi_, block_start_ns);
  drain(control_, block_start_ns);
  renderer_.process_block(left, right);
  counters_.blocks.fetch_add(1, std::memory_order_relaxed);
}

void RenderDriver::render_block_buses(const core::OutputBuses& out, std::uint64_t block_start_ns) noexcept {
  drain(midi_, block_start_ns);
  drain(control_, block_start_ns);
  renderer_.process_block(out);
  counters_.blocks.fetch_add(1, std::memory_order_relaxed);
}

void RenderDriver::drain(Lane& lane, std::uint64_t block_start_ns) noexcept {
  if (!lane.queue) return;
  // A beyond-horizon event is held (with everything behind it) until it is within the horizon, so
  // events are never reordered or lost. It is counted once, when first held.
  for (;;) {
    if (!lane.has_held) {
      if (!lane.queue->pop(lane.held)) break;
      lane.has_held = true;
      lane.counted = false;
    }
    // Story 4.9: a control->RT fence is held back for ONE block after it is first seen, so the next block drains the MIDI lane again before the fence is
    // submitted: a hardware knob move (a CC, stamped earlier) that the MIDI thread was still delivering when the fence arrived is in the renderer before
    // the fence is, and applies before the acknowledgement (same-frame ties keep arrival order, MIDI lane first). The bound is the delivery latency of
    // the MIDI thread: an event stamped before the fence but delivered more than one block later is not covered.
    if (&lane == &control_ && core::is_fence_event(lane.held.ump) && !lane.fence_waited) {
      lane.fence_waited = true;
      break;
    }
    const MappedEvent m = mapper_.map(lane.held.host_ns, block_start_ns);
    if (m.timing == EventTiming::kBeyondHorizon) {
      if (!lane.counted) {
        counters_.beyond_horizon.fetch_add(1, std::memory_order_relaxed);
        lane.counted = true;
      }
      break;
    }
    if (m.timing == EventTiming::kLate) counters_.late.fetch_add(1, std::memory_order_relaxed);
    else if (m.timing == EventTiming::kDeferred) counters_.deferred.fetch_add(1, std::memory_order_relaxed);
    counters_.last_frame.store(m.frame, std::memory_order_relaxed);
    if (renderer_.submit(core::TimedUmp{lane.held.ump, m.frame}))
      counters_.events.fetch_add(1, std::memory_order_relaxed);
    else
      counters_.renderer_rejected.fetch_add(1, std::memory_order_relaxed);
    lane.has_held = false;
    lane.fence_waited = false;
  }
}

}  // namespace velora::host
