#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "velora/core/engine.hpp"
#include "velora/core/ep_parameters.hpp"
#include "velora/core/ep_voice.hpp"
#include "velora/core/params_gen.hpp"
#include "velora/core/part_params.hpp"

namespace velora::core {

class EpEngine final : public Engine {
 public:
  static constexpr const char* kEngineName = "Modeled Electric Piano";
  static constexpr uint32_t kPartCount = params::kPartCount;  // 16 parts
  static constexpr uint32_t kBlockFrames = 64;
  static constexpr uint32_t kVoiceCapacity = 128;
  static constexpr uint32_t kCostUnitsPerVoice = 4;

  explicit EpEngine(float sample_rate = 48000.0f, uint32_t voice_capacity = kVoiceCapacity);
  ~EpEngine() override = default;

  EngineTypeId type_id() const noexcept override { return EngineTypeId::kModeledEP; }
  uint32_t cost_units_per_voice() const noexcept override { return kCostUnitsPerVoice; }
  uint32_t voice_capacity() const noexcept override { return static_cast<uint32_t>(voices_.size()); }
  uint32_t active_voices() const noexcept {
    uint32_t count = 0;
    for (const auto& v : voices_) {
      if (v.is_active()) ++count;
    }
    return count;
  }

  EngineStatus start_voice(uint32_t channel, uint32_t note, uint32_t velocity16) noexcept override;
  EngineStatus stop_voice(uint32_t channel, uint32_t note) noexcept override;
  EngineStatus release_voice(uint32_t channel, uint32_t note, uint32_t release_velocity16) noexcept override;
  bool key_released(uint32_t channel, uint32_t note) noexcept override;
  void set_param(uint32_t param_id, float value) noexcept override;

  void set_part_param(uint32_t part, uint32_t param_id, float value) noexcept VELORA_NONBLOCKING override;
  void set_part_param_edit(uint32_t part, uint32_t param_id, float value) noexcept VELORA_NONBLOCKING override;
  void reset_play_state() noexcept VELORA_NONBLOCKING override;

  void render(float* left, float* right, uint32_t frames) noexcept override;
  void render_split(float* const* left, float* const* right, uint32_t frames) noexcept override;

  bool voice_info(uint32_t slot, VoiceInfo& out) const noexcept override;
  bool begin_steal_fade(uint32_t slot, uint32_t ramp_frames) noexcept override;

  // Voicing parameters per part (0..15)
  void set_part_voice_params(uint32_t part_idx, const EpVoicingParameters& params) noexcept;
  const EpVoicingParameters& part_voice_params(uint32_t part_idx) const noexcept;

  // Real-time control
  void set_sustain(uint32_t channel, bool sustain) noexcept;
  void set_half_pedal(uint32_t channel, float cc64) noexcept;
  float part_pedal_cc(uint32_t part) const noexcept { return part_pedal_cc_[part % kPartCount]; }
  void set_pitch_bend(uint32_t channel, float semitones) noexcept;
  float part_pitch_bend(uint32_t part) const noexcept { return part_pitch_bend_[part % kPartCount]; }

  bool set_part_cc(uint32_t part, uint32_t cc, uint32_t value32) noexcept VELORA_NONBLOCKING override;

  bool set_part_pitch_bend(uint32_t part, uint32_t value32) noexcept override {
    set_pitch_bend(part, static_cast<float>((static_cast<double>(value32) / 2147483648.0 - 1.0) * 2.0));
    return true;
  }

  uint32_t active_voice_count() const noexcept;

 private:
  float sample_rate_{48000.0f};
  uint64_t frame_clock_{0};

  std::array<EpVoicingParameters, kPartCount> part_params_{};
  std::array<EpVoicingParameters, kPartCount> base_params_{};
  std::array<bool, kPartCount> part_sustain_{};
  std::array<float, kPartCount> part_pedal_cc_{};
  std::array<float, kPartCount> part_pitch_bend_{};

  std::vector<EpVoice> voices_;
  alignas(16) std::array<float, kBlockFrames> scratch_left_{};
  alignas(16) std::array<float, kBlockFrames> scratch_right_{};
};

}  // namespace velora::core
