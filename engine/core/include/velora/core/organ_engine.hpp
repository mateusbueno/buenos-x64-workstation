#pragma once
// Tonewheel organ engine (Story 2.2, type id Organ=2). Clean-room; see engine/core/ORGAN.md.
//
// Structure (AD-6, AD-22):
//  * One shared tonewheel generator per part (UMP channel 0..7). In block phase 2 (Engine::begin_block) it
//    produces the 91 wheel signals of the whole block into a fixed table; every voice of the part reads
//    the same wheels, so one wheel has one phase no matter how many keys use it.
//  * A voice is one key. It sums its 9 drawbar footages from the shared wheels (foldback at both ends),
//    scaled by the part's smoothed drawbar gains. Voices write only their own state; the part mix lives in
//    the engine's per-render scratch. Voices are never stolen (steal_protected()) and there is no voice
//    limit below 73 keys x 8 parts.
//  * Drawbar levels 0..8 arrive as performance parameters (registry ids 100..108) through
//    Engine::set_part_param at the event's frame; a 5 ms linear ramp starts at exactly that frame.
#include <array>
#include <cstdint>
#include <vector>

#include "velora/core/engine.hpp"
#include "velora/core/tonewheel_tables.hpp"

namespace velora::core {

class OrganEngine final : public Engine {
 public:
  static constexpr const char* kEngineName = "tonewheel organ";
  static constexpr std::uint32_t kPartCount = 8;
  static constexpr std::uint32_t kBlockFrames = 64;
  static constexpr std::uint32_t kVoiceCapacity = 640;  // 8 parts x 73 keys = 584, plus release tails
  static constexpr std::uint32_t kAttackFrames = 48;    // declick gate only (1 ms); not a key-click model
  static constexpr std::uint32_t kReleaseFrames = 96;   // 2 ms
  static constexpr std::uint32_t kDrawbarRampFrames = 240;  // 5 ms linear ramp at 48 kHz
  static constexpr float kDrawbarStepDb = 3.0f;         // level of step s is -3 dB x (8 - s); step 0 is silent
  static constexpr float kVoiceScale = 0.06f;           // amplitude of one drawbar at step 8
  static constexpr float kKnee = 0.8f;                  // output is linear below the knee, bounded by 1.0 above

  // Cost units (placeholders, to be measured on the dev board; the host benchmark prints host figures).
  static constexpr std::uint32_t kVoiceCostUnits = 2;
  static constexpr std::uint32_t kGeneratorCostUnits = 48;  // per part
  static constexpr std::uint32_t kRotaryCostUnits = 24;     // per part, reserved now, used by Story 2.6
  // The organ+rotary reservation for `organ_parts` organ parts with all 73 keys down: pass it as
  // VoiceArbiter::LiveSetConfig::organ_rotary_cost.
  static constexpr std::uint64_t reservation_cost_units(std::uint32_t organ_parts = 1) noexcept {
    return static_cast<std::uint64_t>(organ_parts) *
           (kGeneratorCostUnits + kRotaryCostUnits + static_cast<std::uint64_t>(tonewheel::kKeyCount) * kVoiceCostUnits);
  }

  OrganEngine();

  EngineTypeId type_id() const noexcept override { return EngineTypeId::kOrgan; }
  std::uint32_t cost_units_per_voice() const noexcept override { return kVoiceCostUnits; }
  std::uint32_t voice_capacity() const noexcept override { return kVoiceCapacity; }
  bool steal_protected() const noexcept override { return true; }

  // Setup-time: which parts run a generator (default all 8). A voice for a disabled part, or for a channel
  // above 7, is refused with kEngineNotAvailable.
  // Disabling a part deactivates its voices at once (they are not stranded; the arbiter sees the slots free).
  void set_enabled_parts(std::uint32_t mask) noexcept {
    enabled_ = mask & 0xFFu;
    for (Voice& v : voices_)
      if (v.active && !(enabled_ & (1u << v.part))) v.active = false;
  }

  EngineStatus start_voice(std::uint32_t channel, std::uint32_t note, std::uint32_t velocity16) noexcept
      VELORA_NONBLOCKING override;
  EngineStatus stop_voice(std::uint32_t channel, std::uint32_t note) noexcept VELORA_NONBLOCKING override;
  void set_param(std::uint32_t param_id, float value) noexcept VELORA_NONBLOCKING override;
  void set_part_param(std::uint32_t part, std::uint32_t param_id, float value) noexcept VELORA_NONBLOCKING override;
  void begin_block(std::uint32_t frames) noexcept VELORA_NONBLOCKING override;
  // Adds frames of output. The block loop calls begin_block() once per block and then render() for
  // contiguous segments summing to the block; a standalone caller may call render() alone, in which case
  // it runs the generator itself at every block boundary.
  void render(float* left, float* right, std::uint32_t frames) noexcept VELORA_NONBLOCKING override;
  bool voice_info(std::uint32_t slot, VoiceInfo& out) const noexcept VELORA_NONBLOCKING override;
  bool begin_steal_fade(std::uint32_t slot, std::uint32_t ramp_frames) noexcept VELORA_NONBLOCKING override;

  std::uint32_t active_voices() const noexcept;

  // --- introspection for tests and tools ------------------------------------------------------------
  static double wheel_frequency_hz(std::uint32_t wheel) noexcept;  // wheel 1..91
  static float drawbar_level(int step) noexcept;                   // 0..8 -> linear gain (clamped)
  // The shared wheel table of `part` for the current block: kWheelCount rows of kBlockFrames samples,
  // row w-1 is wheel w. Valid after begin_block().
  const float* wheel_block(std::uint32_t part) const noexcept { return &wheels_[part * kWheelStride]; }
  float drawbar_gain(std::uint32_t part, std::uint32_t drawbar) const noexcept { return parts_[part].cur[drawbar]; }

 private:
  static constexpr std::size_t kWheelStride = tonewheel::kWheelCount * kBlockFrames;
  struct Voice {
    bool active = false;
    VoiceState state = VoiceState::kHeld;
    std::uint8_t part = 0;
    std::array<std::uint8_t, tonewheel::kDrawbarCount> wheel{};  // 0-based wheel index per drawbar
    std::uint32_t channel = 0, note = 0;
    std::uint64_t start_time = 0;
    float env = 0.0f;
  };
  struct Part {
    std::array<float, tonewheel::kDrawbarCount> cur{}, tgt{}, step{};
    std::array<std::uint32_t, tonewheel::kDrawbarCount> left{};
  };

  void render_chunk(float* left, float* right, std::uint32_t frames) noexcept VELORA_NONBLOCKING;

  std::vector<Voice> voices_;
  std::array<Part, kPartCount> parts_{};
  std::vector<double> osc_cos_, osc_sin_;  // generator state: kPartCount x kWheelCount unit phasors
  std::array<double, tonewheel::kWheelCount> rot_cos_{}, rot_sin_{};  // per-frame rotation of each wheel
  std::vector<float> wheels_;              // kPartCount x kWheelCount x kBlockFrames
  std::array<float, tonewheel::kDrawbarCount * kBlockFrames> gains_{};  // scratch: per-frame drawbar gains
  std::array<float, kPartCount * kBlockFrames> mix_{};                  // scratch: per-part mono mix
  std::uint32_t enabled_ = 0xFF;
  std::uint32_t block_pos_ = 0, gen_frames_ = 0;
  std::uint64_t clock_ = 0;
};

}  // namespace velora::core
