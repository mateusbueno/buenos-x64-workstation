#pragma once

#include <atomic>
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
  std::atomic<std::uint32_t> last_frame{0};        // frame of the most recent mapped event
};

// The audio-thread render path, shared by the CoreAudio callback and device-free tests:
// drain host MIDI queue -> map timestamp to frame -> BlockRenderer::submit -> process_block.
// Allocation-, lock- and block-free.
class RenderDriver final : public ports::BlockSink {
 public:
  RenderDriver(core::BlockRenderer& renderer, HostEventQueue& queue, TimeMapperConfig cfg = {}) noexcept
      : renderer_(renderer), queue_(queue), mapper_(cfg) {}

  void render_block(float* left, float* right, std::uint64_t block_start_ns) noexcept VELORA_NONBLOCKING override;

  const RenderCounters& counters() const noexcept { return counters_; }
  const TimeMapper& mapper() const noexcept { return mapper_; }

 private:
  core::BlockRenderer& renderer_;
  HostEventQueue& queue_;
  TimeMapper mapper_;
  RenderCounters counters_;
  StampedUmp held_{};
  bool has_held_ = false;
  bool held_counted_ = false;
};

}  // namespace velora::host
