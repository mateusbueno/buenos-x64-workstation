#pragma once

#include <cstdint>

#include "velora/core/rt.hpp"

namespace velora::core {

// Engine type IDs. The numeric values of the four reserved IDs are STABLE and must never change
// or be reused (they will appear in saved data). kTestTone is a non-reserved, test-only engine.
enum class EngineTypeId : std::uint32_t {
  kSample = 1,  // reserved
  kOrgan = 2,   // reserved
  kFm = 3,      // reserved
  kVa = 4,      // reserved
  kTestTone = 0xF000,
};

enum class EngineStatus : std::uint8_t {
  kOk = 0,
  kEngineNotAvailable,  // reserved type with no compiled-in engine: silence, never another engine
  kEngineUnknown,       // id outside the reserved set (and not the test engine)
  kPoolExhausted,       // no free voice: newest note-on dropped (stealing arrives in Story 1.19)
  kVoiceNotFound,       // note-off with no matching held voice
  kEventOverflow,       // pending-event buffer full: event dropped (queue keeps draining)
  kCount
};

enum class VoiceState : std::uint8_t { kHeld, kSustained, kReleasing };

// Steal metadata an engine reports per voice (consumed by the Story 1.19 arbiter).
struct VoiceInfo {
  std::uint64_t start_time = 0;  // engine frame clock at voice start
  float level = 0.0f;            // current output level estimate
  VoiceState state = VoiceState::kHeld;
};

// The single static engine contract for Sample, Organ, FM and VA. Engines are compiled in (no
// plugin loading). All voice start/stop calls are made by the core, on the render thread, between
// render segments; render() accumulates (adds) into the stereo buffers.
class Engine {
 public:
  virtual ~Engine() = default;

  virtual EngineTypeId type_id() const noexcept = 0;
  // Cost units one active voice consumes (used by the pool arbiter's budget).
  virtual std::uint32_t cost_units_per_voice() const noexcept = 0;
  virtual std::uint32_t voice_capacity() const noexcept = 0;

  virtual EngineStatus start_voice(std::uint32_t channel, std::uint32_t note,
                                   std::uint32_t velocity16) noexcept VELORA_NONBLOCKING = 0;
  // Begins release of every held voice for (channel, note).
  virtual EngineStatus stop_voice(std::uint32_t channel, std::uint32_t note) noexcept VELORA_NONBLOCKING = 0;
  // Sound parameters are addressed by param_id.
  virtual void set_param(std::uint32_t param_id, float value) noexcept VELORA_NONBLOCKING = 0;
  // Adds `frames` frames of output into left/right.
  virtual void render(float* left, float* right, std::uint32_t frames) noexcept VELORA_NONBLOCKING = 0;

  // Steal metadata. Returns false when the slot is inactive.
  virtual bool voice_info(std::uint32_t slot, VoiceInfo& out) const noexcept VELORA_NONBLOCKING = 0;
};

}  // namespace velora::core
