#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "velora/core/arena.hpp"
#include "velora/core/engine.hpp"
#include "velora/core/engine_registry.hpp"
#include "velora/core/spsc_queue.hpp"
#include "velora/core/test_tone_engine.hpp"
#include "velora/core/ump.hpp"

namespace velora::core {

constexpr std::uint32_t kBlockFrames = 64;
constexpr std::uint32_t kSampleRate = 48000;

// Single-threaded serial block renderer: 64-frame stereo float blocks at 48 kHz.
//
// Event frame semantics: submit() takes a TimedUmp whose target_frame is relative to the start of
// the NEXT block rendered. Events with target_frame < 64 apply at exactly that frame. Events with
// target_frame >= 64 are DEFERRED: carried to following blocks with 64 subtracted per block, so
// they apply at the correct absolute position (not rejected). Within a block events apply in frame
// order, ties in arrival order.
class BlockRenderer {
 public:
  static constexpr std::size_t kQueueCapacity = 1024;
  static constexpr std::size_t kPendingCapacity = 256;

  explicit BlockRenderer(std::uint32_t test_tone_voices = 32);

  // Producer side (any single producer thread). UMP is the only accepted input type.
  // False when the queue is full; events already queued are unaffected.
  bool submit(const TimedUmp& ev) noexcept VELORA_NONBLOCKING { return queue_.push(ev); }

  // Setup-time: choose the engine type that serves a UMP channel (default kTestTone).
  void set_channel_engine(std::uint32_t channel, std::uint32_t engine_type_id) noexcept {
    channel_engine_[channel & 0xF] = engine_type_id;
  }

  // Setup-time: bind another compiled-in engine (e.g. the SampleEngine) next to the always-bound test
  // tone, then route channels to it with set_channel_engine(). False when the registry is full or the
  // type is already bound. The engine must outlive the renderer and be configured before audio runs;
  // every bound engine is rendered each block (idle engines add nothing).
  bool bind_engine(Engine& engine) noexcept { return registry_.bind(engine); }

  // Render one block. left/right each hold kBlockFrames floats and are overwritten.
  void process_block(float* left, float* right) noexcept VELORA_NONBLOCKING;

  // Count of occurrences of each typed status since construction (kOk counts successes).
  std::uint64_t status_count(EngineStatus s) const noexcept { return status_counts_[static_cast<std::size_t>(s)]; }
  EngineStatus last_status() const noexcept { return last_status_; }
  std::uint64_t ignored_events() const noexcept { return ignored_; }

  bool ready() const noexcept { return bound_; }
  const EngineRegistry& registry() const noexcept { return registry_; }
  TestToneEngine& test_tone() noexcept { return tone_; }

 private:
  void apply(const Ump& ump) noexcept VELORA_NONBLOCKING;
  void record(EngineStatus s) noexcept VELORA_NONBLOCKING;
  void drain_queue() noexcept VELORA_NONBLOCKING;
  void render_segment(float* l, float* r, std::uint32_t from, std::uint32_t to) noexcept VELORA_NONBLOCKING;

  Arena arena_;
  TestToneEngine tone_;
  EngineRegistry registry_;
  SpscQueue<TimedUmp, kQueueCapacity> queue_;
  std::array<TimedUmp, kPendingCapacity> pending_{};  // sorted by target_frame, stable
  std::size_t pending_count_ = 0;
  std::array<std::uint32_t, 16> channel_engine_{};
  std::array<std::uint64_t, static_cast<std::size_t>(EngineStatus::kCount)> status_counts_{};
  EngineStatus last_status_ = EngineStatus::kOk;
  std::uint64_t ignored_ = 0;
  bool bound_ = false;
};

}  // namespace velora::core
