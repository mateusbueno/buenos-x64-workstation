#pragma once

#include <cstdint>

#include "velora/core/arena.hpp"
#include "velora/core/engine.hpp"

namespace velora::core {

// Sine voices with a linear attack/release envelope. The sine starts at its peak (cosine phase)
// so the first rendered sample of a voice is non-zero: onset is observable at its exact frame.
class TestToneEngine final : public Engine {
 public:
  static constexpr std::uint32_t kParamGain = 1;
  static constexpr std::uint32_t kAttackFrames = 48;
  static constexpr std::uint32_t kReleaseFrames = 48;

  // Voice storage is taken from `arena` (construction is non-RT).
  TestToneEngine(Arena& arena, std::uint32_t max_voices);

  EngineTypeId type_id() const noexcept override { return EngineTypeId::kTestTone; }
  std::uint32_t cost_units_per_voice() const noexcept override { return 1; }
  std::uint32_t voice_capacity() const noexcept override { return capacity_; }

  EngineStatus start_voice(std::uint32_t channel, std::uint32_t note,
                           std::uint32_t velocity16) noexcept VELORA_NONBLOCKING override;
  EngineStatus stop_voice(std::uint32_t channel, std::uint32_t note) noexcept VELORA_NONBLOCKING override;
  void set_param(std::uint32_t param_id, float value) noexcept VELORA_NONBLOCKING override;
  void render(float* left, float* right, std::uint32_t frames) noexcept VELORA_NONBLOCKING override;
  bool voice_info(std::uint32_t slot, VoiceInfo& out) const noexcept VELORA_NONBLOCKING override;

  std::uint32_t active_voices() const noexcept;

 private:
  struct Voice {
    bool active = false;
    VoiceState state = VoiceState::kHeld;
    std::uint32_t channel = 0;
    std::uint32_t note = 0;
    std::uint64_t start_time = 0;
    float phase = 0.25f;  // cycles
    float inc = 0.0f;     // cycles per frame
    float env = 0.0f;
    float amp = 0.0f;
  };

  Voice* voices_ = nullptr;
  std::uint32_t capacity_ = 0;
  std::uint64_t clock_ = 0;
  float gain_ = 1.0f;
};

}  // namespace velora::core
