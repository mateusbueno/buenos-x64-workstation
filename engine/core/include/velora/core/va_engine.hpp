#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "velora/core/engine.hpp"
#include "velora/core/va_voice.hpp"
#include "velora/core/va_parameters.hpp"

namespace velora::core {

class VaEngine final : public Engine {
 public:
  static constexpr const char* kEngineName = "Virtual Analog";
  static constexpr uint32_t kPartCount = 8;
  static constexpr uint32_t kBlockFrames = 64;
  static constexpr uint32_t kVoiceCapacity = 128;
  static constexpr uint32_t kCostUnitsPerVoice = 4;

  explicit VaEngine(float sample_rate = 48000.0f) noexcept;
  ~VaEngine() override = default;

  EngineTypeId type_id() const noexcept override { return EngineTypeId::kVa; }
  uint32_t cost_units_per_voice() const noexcept override { return kCostUnitsPerVoice; }
  uint32_t voice_capacity() const noexcept override { return kVoiceCapacity; }

  EngineStatus start_voice(uint32_t channel, uint32_t note, uint32_t velocity16) noexcept override;
  EngineStatus stop_voice(uint32_t channel, uint32_t note) noexcept override;
  void set_param(uint32_t param_id, float value) noexcept override;
  void render(float* left, float* right, uint32_t frames) noexcept override;
  void render_split(float* const* left, float* const* right, uint32_t frames) noexcept override;

  bool voice_info(uint32_t slot, VoiceInfo& out) const noexcept override;
  bool begin_steal_fade(uint32_t slot, uint32_t ramp_frames) noexcept override;

  // Voice parameter management per part (0..7)
  void set_part_voice_params(uint32_t part_idx, const VaVoiceParameters& params) noexcept;
  const VaVoiceParameters& part_voice_params(uint32_t part_idx) const noexcept;

  // Real-time control
  void set_sustain(uint32_t channel, bool sustain) noexcept;
  void set_pitch_bend(uint32_t channel, float semitones) noexcept;
  void set_brightness_offset(uint32_t channel, uint8_t brightness) noexcept;

  uint32_t active_voice_count() const noexcept;

 private:
  uint32_t allocate_voice_slot(uint32_t channel, uint32_t note) noexcept;

  float sample_rate_{48000.0f};
  uint64_t frame_clock_{0};

  std::array<VaVoiceParameters, kPartCount> part_params_{};
  std::array<bool, kPartCount> part_sustain_{};
  std::array<float, kPartCount> part_pitch_bend_{};
  std::array<int32_t, kPartCount> part_mono_slots_{}; // slot index if mono/legato active

  std::array<VaVoice, kVoiceCapacity> voices_{};
  alignas(16) std::array<float, kBlockFrames> scratch_left_{};
  alignas(16) std::array<float, kBlockFrames> scratch_right_{};
};

} // namespace velora::core
