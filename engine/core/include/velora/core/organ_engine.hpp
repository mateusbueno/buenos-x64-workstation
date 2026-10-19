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
//  * Percussion and key click (Story 2.3, ORGAN.md): percussion is a per-voice decaying 2nd or 3rd harmonic
//    triggered only by a key pressed while no other key of the part is held; the part-level effects of
//    percussion (1' cancelled, normal volume lowers the drawbars) are smoothed 5 ms ramps. Key click is a
//    short noise burst per contact event (onset and release) drawn from a seeded deterministic generator.
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

  // Percussion (Story 2.3). Time constants are of the exponential envelope exp(-t / tau); gains are in
  // units of one drawbar at step 8. Justification in ORGAN.md.
  static constexpr float kPercTauFastSeconds = 0.06f;   // -40 dB after about 0.28 s
  static constexpr float kPercTauSlowSeconds = 0.20f;   // -40 dB after about 0.92 s
  static constexpr float kPercNormalGain = 1.6f;        // peak amplitude of normal percussion
  static constexpr float kPercSoftGain = 0.8f;          // soft = normal - 6 dB
  static constexpr float kPercDrawbarGain = 0.70794578f;  // drawbars x -3 dB while percussion is on at normal volume
  static constexpr std::uint32_t kPercRampFrames = 240; // 5 ms ramps for the 1' cancel and the drawbar lowering
  static constexpr std::uint32_t kPercSecondDrawbar = 3;  // 4' : the 2nd harmonic of the key pitch
  static constexpr std::uint32_t kPercThirdDrawbar = 4;   // 2 2/3' : the 3rd harmonic of the key pitch

  // Key click (Story 2.3). Gains in drawbar-step-8 units at click level 1.
  static constexpr std::uint32_t kClickFrames = 192;    // hard length of one burst (4 ms)
  static constexpr float kClickGain = 2.0f;             // onset burst peak scale at level 1
  static constexpr float kClickReleaseRatio = 0.6f;     // release (key opens) burst relative to the onset burst
  static constexpr std::uint64_t kClickSeed = 0x5EED0C71C4B1D001ull;  // fixed: renders are reproducible

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
  // Percussion time constant in seconds for a registry decay value (0 fast, 1 slow).
  static float percussion_tau_seconds(int decay) noexcept { return decay == 0 ? kPercTauFastSeconds : kPercTauSlowSeconds; }
  // Current (ramped) part-level factors: the 1' cancel (1 = 1' sounds, 0 = cancelled) and the drawbar factor.
  float percussion_cancel_factor(std::uint32_t part) const noexcept { return parts_[part].cancel.cur; }
  float percussion_drawbar_factor(std::uint32_t part) const noexcept { return parts_[part].lower.cur; }

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
    // Percussion: latched when the key is pressed (perc_env 0 = no percussion on this voice).
    float perc_env = 0.0f, perc_coef = 1.0f, perc_gain = 0.0f;
    std::uint8_t perc_drawbar = 0;  // which footage's wheel carries the percussion tone
    // Key click bursts in progress: [0] the onset contact, [1] the release contact. They add, so a release
    // within 4 ms of the onset lets the onset burst finish (no discontinuity).
    struct Burst {
      std::uint32_t left = 0;  // frames remaining
      float env = 0.0f, coef = 0.0f, amp = 0.0f, prev = 0.0f;
      std::uint64_t rng = 0;  // each burst has its own noise stream, so an earlier burst is unchanged by a later one
    };
    std::array<Burst, 2> click{};
    std::uint64_t click_rng = 0;
    bool clicking() const noexcept { return click[0].left > 0 || click[1].left > 0; }
  };
  struct Ramp {  // linear 5 ms ramp of one part-level factor
    float cur = 1.0f, tgt = 1.0f, step = 0.0f;
    std::uint32_t left = 0;
    void set(float target) noexcept {
      if (target == tgt) return;
      tgt = target;
      left = kPercRampFrames;
      step = (target - cur) / static_cast<float>(kPercRampFrames);
    }
    float advance() noexcept {
      if (left > 0) {
        cur += step;
        if (--left == 0) cur = tgt;
      }
      return cur;
    }
  };
  struct Part {
    std::array<float, tonewheel::kDrawbarCount> cur{}, tgt{}, step{};
    std::array<std::uint32_t, tonewheel::kDrawbarCount> left{};
    // Percussion settings as last set (registry values) and the ramped part-level effects.
    bool perc_on = false;
    int perc_harmonic = 1, perc_decay = 0, perc_volume = 0;
    float key_click = 0.0f;
    std::uint64_t press_count = 0;  // key presses of this part so far: seeds its key clicks (independent of other parts)
    Ramp cancel, lower;  // 1' cancel factor, drawbar-lowering factor
  };

  void start_click(Voice& v, std::uint32_t which, float level, float scale) noexcept;
  void apply_percussion_targets(Part& p) noexcept;

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
