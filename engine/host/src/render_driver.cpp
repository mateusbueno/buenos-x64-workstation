#include "velora/host/render_driver.hpp"

#include <chrono>

namespace velora::host {

void RenderDriver::render_block(float* left, float* right, std::uint64_t block_start_ns) noexcept {
  drain(midi_, block_start_ns);
  drain(control_, block_start_ns);
  const auto t0 = std::chrono::steady_clock::now();
  renderer_.process_block(left, right);
  record_render_time(t0);
  counters_.blocks.fetch_add(1, std::memory_order_relaxed);
}

void RenderDriver::render_block_buses(const core::OutputBuses& out, std::uint64_t block_start_ns) noexcept {
  drain(midi_, block_start_ns);
  drain(control_, block_start_ns);
  const auto t0 = std::chrono::steady_clock::now();
  renderer_.process_block(out);
  record_render_time(t0);
  counters_.blocks.fetch_add(1, std::memory_order_relaxed);
}

void RenderDriver::record_render_time(std::chrono::steady_clock::time_point t0) noexcept {
  const auto dt = std::chrono::steady_clock::now() - t0;
  const std::uint64_t ns = dt.count() > 0 ? static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count()) : 0;
  const TimeMapperConfig& c = mapper_.config();
  renderer_.diagnostics_rt().record_render(ns, frames_to_ns(c.block_frames, c.sample_rate));
}

bool RenderDriver::stale_clock(const Lane& lane, const StampedUmp& e) const noexcept {
  if (&lane != &midi_ || e.ump.message_type() != core::Ump::kMessageTypeSystemRealTime) return false;
  const std::uint32_t sb = e.ump.status_byte();
  if (sb != core::Ump::kStatusTimingClock && sb != core::Ump::kStatusStart && sb != core::Ump::kStatusContinue && sb != core::Ump::kStatusStop) return false;
  const core::ClockSource cs = renderer_.clock_source();
  return !((cs == core::ClockSource::kDin && e.port == ports::MidiPortKind::kDin) || (cs == core::ClockSource::kUsb && e.port == ports::MidiPortKind::kUsb));
}

void RenderDriver::drain(Lane& lane, std::uint64_t block_start_ns) noexcept {
  if (!lane.queue) return;
  // A beyond-horizon event is held (with everything behind it) until it is within the horizon, so
  // events are never reordered or lost. It is counted once, when first held.
  for (;;) {
    if (!lane.has_held) {
      if (!lane.queue->pop(lane.held)) break;
      if (stale_clock(lane, lane.held)) {  // queued before a source switch: never reaches the follower
        counters_.clock_stale.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
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
    if (stale_clock(lane, lane.held)) {  // re-checked on every pass, so an event held beyond the horizon across a switch is dropped, and the lane behind it flows
      counters_.clock_stale.fetch_add(1, std::memory_order_relaxed);
      lane.has_held = false;
      lane.fence_waited = false;
      continue;
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
    if (renderer_.submit(core::TimedUmp{lane.held.ump, m.frame})) {
      counters_.events.fetch_add(1, std::memory_order_relaxed);
      // Story 1.21: event-to-onset latency = onset time - the event's host timestamp (late events, which play at frame 0 of this block, are not recorded).
      if (m.timing != EventTiming::kLate && lane.held.host_ns != 0) {
        const TimeMapperConfig& c = mapper_.config();
        const std::uint64_t onset_ns = block_start_ns + frames_to_ns(m.frame, c.sample_rate);
        if (onset_ns >= lane.held.host_ns) renderer_.diagnostics_rt().record_latency_ns(onset_ns - lane.held.host_ns);
      }
    } else
      counters_.renderer_rejected.fetch_add(1, std::memory_order_relaxed);
    lane.has_held = false;
    lane.fence_waited = false;
  }
}

}  // namespace velora::host
