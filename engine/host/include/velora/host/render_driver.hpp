#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#include "velora/core/block_renderer.hpp"
#include "velora/host/midi_ingest.hpp"
#include "velora/host/time_mapper.hpp"
#include "velora/ports/audio_output.hpp"

namespace velora::host {

struct RenderCounters {
  std::atomic<std::uint64_t> blocks{0};
  std::atomic<std::uint64_t> events{0};            // events handed to the renderer
  std::atomic<std::uint64_t> late{0};              // timestamp before block start -> frame 0
  std::atomic<std::uint64_t> deferred{0};          // mapped beyond this block, carried forward
  std::atomic<std::uint64_t> beyond_horizon{0};    // events held (once each) until within the horizon
  std::atomic<std::uint64_t> renderer_rejected{0}; // BlockRenderer queue full -> dropped
  std::atomic<std::uint64_t> clock_stale{0};       // Story 7.7: realtime clock of a port the clock source no longer selects (dropped at pop time / before submit)
  std::atomic<std::uint32_t> last_frame{0};        // frame of the most recent mapped event
};

// The audio-thread render path, shared by the CoreAudio callback and device-free tests:
// drain host MIDI queue -> map timestamp to frame -> BlockRenderer::submit -> process_block.
// Allocation-, lock- and block-free.
class RenderDriver final : public ports::BlockSink {
 public:
  // `control` (optional) is the control thread's own SPSC lane (parameter UMPs from ws.v1), so the MIDI
  // thread stays the sole producer of `queue`. Both lanes get the same timestamp mapping.
  RenderDriver(core::BlockRenderer& renderer, HostEventQueue& queue, TimeMapperConfig cfg = {},
               HostEventQueue* control = nullptr) noexcept
      : renderer_(renderer), mapper_(cfg), midi_{&queue}, control_{control} {}

  // The BlockSink path of the device adapters: the Main bus (the Aux bus is not played by any adapter yet; physical outputs: Epic 3).
  void render_block(float* left, float* right, std::uint64_t block_start_ns) noexcept VELORA_NONBLOCKING override;
  // The same block with both output buses (Story 2.7): Main and, when set, Aux.
  void render_block_buses(const core::OutputBuses& out, std::uint64_t block_start_ns) noexcept VELORA_NONBLOCKING;

  const RenderCounters& counters() const noexcept { return counters_; }
  const TimeMapper& mapper() const noexcept { return mapper_; }
  // Story 1.21: a device adapter that detects an underrun / xrun itself (e.g. an ALSA xrun) records it here.
  void record_xrun() noexcept VELORA_NONBLOCKING { renderer_.diagnostics_rt().record_xrun(); }

 private:
  struct Lane {
    HostEventQueue* queue = nullptr;
    StampedUmp held{};
    bool has_held = false;
    bool counted = false;
    bool fence_waited = false;  // a fence event was held back one block (see drain)
  };
  void record_render_time(std::chrono::steady_clock::time_point t0) noexcept VELORA_NONBLOCKING;
  void drain(Lane& lane, std::uint64_t block_start_ns) noexcept VELORA_NONBLOCKING;
  // Story 7.7: an external clock message (0xF8/FA/FB/FC) of the MIDI lane whose port is not the renderer's current clock source.
  bool stale_clock(const Lane& lane, const StampedUmp& e) const noexcept VELORA_NONBLOCKING;

  core::BlockRenderer& renderer_;
  TimeMapper mapper_;
  RenderCounters counters_;
  Lane midi_, control_;
};

}  // namespace velora::host
