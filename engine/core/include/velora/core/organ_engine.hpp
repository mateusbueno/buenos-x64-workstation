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
//  * Vibrato/chorus scanner and leakage (Story 2.4, ORGAN.md): the scanner is a per-part tapped delay line
//    (vibrato_scanner.hpp) applied to the part's summed signal before the soft limit; settings crossfade over
//    5 ms. Leakage adds, for every drawbar of every sounding key, low-level contributions of the two
//    neighbouring wheels with a fixed seeded per-wheel table, scaled by the leakage parameter (0 = exactly off).
//  * Multi-contact onset (Story 2.5, ORGAN.md): contact_k UMP events (contact_events.hpp) start a key voice on contact 1 and
//    bring each drawbar busbar group in at its contact's frame (a short per-group gain ramp that starts at that frame), and
//    reverse that as contacts open; a source with only Note On/Off starts and stops all groups together as before.
//  * Rotary speaker (Story 2.6, rotary_speaker.hpp, ORGAN.md): each part's output passes through its own RotarySpeaker when
//    rotary_on is set (stereo out; off is bit-exact pass-through); rotary_speed (stop/slow/fast) ramps the horn and the drum
//    with their own time constants, and the state is published lock-free (rotary_status()).
//  * Drawbar levels 0..8 arrive as performance parameters (registry ids 100..108) through
//    Engine::set_part_param at the event's frame; a 5 ms linear ramp starts at exactly that frame.
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <vector>

#include "velora/core/engine.hpp"
#include "velora/core/params_gen.hpp"
#include "velora/core/rotary_speaker.hpp"
#include "velora/core/tonewheel_tables.hpp"
#include "velora/core/vibrato_scanner.hpp"

namespace velora::core {

class OrganEngine final : public Engine {
 public:
  static constexpr const char* kEngineName = "tonewheel organ";
  static constexpr std::uint32_t kPartCount = params::kPartCount;  // structural capacity (16); the tier profile caps the parts a Live Set may use (Story 4.1)
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

  // Leakage (Story 2.4). Each wheel picks up its two neighbours (wheel-1, wheel+1) with a fixed gain drawn at
  // construction from kLeakageSeed: gain = leakage x kLeakageMaxGain x (kLeakageMinFraction + (1 - kLeakageMinFraction) x r),
  // r in [0, 1) from the seeded generator. The table does not depend on the part or on any run state.
  static constexpr std::uint64_t kLeakageSeed = 0x1EAC4A6E5EED2024ull;
  static constexpr float kLeakageMaxGain = 0.02f;       // -34 dB relative to the leaking wheel at leakage 1
  static constexpr float kLeakageMinFraction = 0.25f;   // weakest pattern entry = 0.25 x max
  static constexpr std::uint32_t kLeakRampFrames = 240; // 5 ms ramp when the leakage parameter moves
  static constexpr std::uint32_t kRotaryRampFrames = 240;  // 5 ms crossfade when the rotary stage is switched on or off

  // Multi-contact onset (Story 2.5). The nine busbars are split into contact groups; contact k (1-based) brings group k-1 in.
  static constexpr std::uint32_t kMaxContacts = 3;
  static constexpr std::uint32_t kContactRampFrames = 8;  // 167 us linear gain ramp of a group; its first frame is the event frame
  // Contact groups by drawbar (16', 5 1/3', 8', 4', 2 2/3', 2', 1 3/5', 1 1/3', 1'): the table is part of the documented behaviour.
  static constexpr std::array<std::uint8_t, tonewheel::kDrawbarCount> kContactGroups3{0, 0, 0, 1, 1, 1, 2, 2, 2};
  static constexpr std::array<std::uint8_t, tonewheel::kDrawbarCount> kContactGroups2{0, 0, 0, 0, 1, 1, 1, 1, 1};
  // Group (0-based) of drawbar `drawbar` (0..8) for a keybed with `contact_count` (2 or 3) contacts per key.
  static constexpr std::uint32_t contact_group(std::uint32_t contact_count, std::uint32_t drawbar) noexcept {
    return contact_count == 2 ? kContactGroups2[drawbar % tonewheel::kDrawbarCount] : kContactGroups3[drawbar % tonewheel::kDrawbarCount];
  }

  // Cost units (placeholders, to be measured on the dev board; the host benchmark prints host figures).
  static constexpr std::uint32_t kVoiceCostUnits = 2;
  static constexpr std::uint32_t kGeneratorCostUnits = 48;  // per part
  static constexpr std::uint32_t kRotaryCostUnits = RotarySpeaker::kCostUnits;  // per part (Story 2.6)
  // The organ+rotary reservation for `organ_parts` organ parts with all 73 keys down: pass it as
  // VoiceArbiter::LiveSetConfig::organ_rotary_cost.
  static constexpr std::uint64_t reservation_cost_units(std::uint32_t organ_parts = 1) noexcept {
    return static_cast<std::uint64_t>(organ_parts) *
           (kGeneratorCostUnits + kRotaryCostUnits + static_cast<std::uint64_t>(tonewheel::kKeyCount) * kVoiceCostUnits);
  }

  // `part_count` parts run a generator (v1: 8, Pro: 16; at most kPartCount) and the pool holds `voice_capacity` voices (v1: kVoiceCapacity). Both are fixed at
  // construction (Story 4.1: tier profile); a part above part_count is refused with kEngineNotAvailable, and an engine built with 8 parts costs what it always did.
  explicit OrganEngine(std::uint32_t part_count = 8, std::uint32_t voice_capacity = kVoiceCapacity);

  EngineTypeId type_id() const noexcept override { return EngineTypeId::kOrgan; }
  std::uint32_t cost_units_per_voice() const noexcept override { return kVoiceCostUnits; }
  std::uint32_t voice_capacity() const noexcept override { return voice_capacity_; }
  bool steal_protected() const noexcept override { return true; }

  // Setup-time: which parts run a generator (default all the parts the engine was built for). A voice for a disabled part, or for a channel
  // above 7, is refused with kEngineNotAvailable.
  // Disabling a part deactivates its voices at once (they are not stranded; the arbiter sees the slots free).
  void set_enabled_parts(std::uint32_t mask) noexcept {
    enabled_ = mask & part_mask_;
    for (Voice& v : voices_)
      if (v.active && !(enabled_ & (1u << v.part))) v.active = false;
  }

  // Setup-time: contacts per key of the keybed behind contact_k events (2 or 3, default 3); false and unchanged otherwise.
  // A voice latches the value when it starts. Note On/Off-only sources are not affected by it.
  // Refused (false) while any voice is active: a voice latched the old count.
  bool set_contact_count(std::uint32_t n) noexcept {
    if (n < 2 || n > kMaxContacts || active_voices() > 0) return false;
    contact_count_ = n;
    return true;
  }
  std::uint32_t contact_count() const noexcept { return contact_count_; }
  bool supports_contacts() const noexcept override { return true; }
  EngineStatus set_contact(std::uint32_t channel, std::uint32_t note, std::uint32_t contact, bool closed,
                           ContactEffect& effect) noexcept VELORA_NONBLOCKING override;
  // contact_k events handled / refused as malformed (open without close, duplicate close, close of k > 1 without contact 1, index 0
  // or above contact_count()). RT thread (or after rendering) only.
  std::uint64_t contact_events_applied() const noexcept { return contact_applied_; }
  std::uint64_t contact_events_ignored() const noexcept { return contact_ignored_; }
  // Bit k-1 set = contact k of the held voice of (channel, note) is closed; 0 when the key has no held voice.
  std::uint32_t contact_mask(std::uint32_t channel, std::uint32_t note) const noexcept;

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
  // Part p is added into left[p]/right[p] (Story 2.7, per-part buses); render() is render_split() with all pointers equal.
  void render_split(float* const* left, float* const* right, std::uint32_t frames) noexcept VELORA_NONBLOCKING override;
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
  // Leakage gain of wheel `wheel` (1..91) from its neighbour `side` (0 = wheel - 1, 1 = wheel + 1) at leakage 1;
  // 0 at the two ends of the wheel set, which have only one neighbour.
  float leakage_pattern(std::uint32_t wheel, std::uint32_t side) const noexcept {
    if (wheel < 1 || wheel > tonewheel::kWheelCount || side > 1) return 0.0f;
    return leak_pattern_[(wheel - 1) * 2 + side];
  }
  // Sweep position (0..1, triangle) of a part's scanner: advances with time whether or not the part sounds.
  float scanner_sweep_position(std::uint32_t part) const noexcept { return parts_[part].scan.sweep_position(); }
  // Current (ramped) leakage amount of a part, 0..1, and the scanner setting in force.
  float leakage_amount(std::uint32_t part) const noexcept { return parts_[part].leak.cur; }
  ScannerMode scanner_mode(std::uint32_t part) const noexcept { return parts_[part].scan.mode(); }
  // Rotary speaker state of a part, readable from any thread without locks: the commanded speed (reported the moment the command
  // is applied, before the rotors have reached it), whether the rotary stage is on, and the actual horn and drum speeds in Hz as of
  // the end of the last rendered chunk. The fields are individually atomic.
  struct RotaryStatus {
    RotarySpeed target = RotarySpeed::kSlow;
    bool on = false;
    float horn_hz = 0.0f, drum_hz = 0.0f;
  };
  RotaryStatus rotary_status(std::uint32_t part) const noexcept {
    RotaryStatus s;
    if (part >= kPartCount) return s;
    const std::uint32_t w = rot_state_[part].load(std::memory_order_acquire);
    s.target = static_cast<RotarySpeed>(w & 3u);
    s.on = (w & 4u) != 0;
    s.horn_hz = std::bit_cast<float>(rot_horn_[part].load(std::memory_order_relaxed));
    s.drum_hz = std::bit_cast<float>(rot_drum_[part].load(std::memory_order_relaxed));
    return s;
  }
  // The part's rotary unit (introspection for tests and tools; RT thread or after rendering).
  const RotarySpeaker& rotary(std::uint32_t part) const noexcept { return parts_[part < kPartCount ? part : 0].rot; }
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
    // Multi-contact busbar groups (Story 2.5): one linear gain per contact group. A Note On voice has every group at 1 and
    // never ramps (the render path is then unchanged); a contact-started voice begins at 0 and each closure ramps its group in.
    std::uint8_t groups = 3;       // contact count latched at the start
    std::uint8_t closed_mask = 0;  // bit k-1 = contact k closed
    std::array<float, kMaxContacts> gate_cur{1.0f, 1.0f, 1.0f}, gate_tgt{1.0f, 1.0f, 1.0f}, gate_step{};
    std::array<std::uint8_t, kMaxContacts> gate_left{};
    std::uint64_t click_rng = 0;
    bool clicking() const noexcept { return click[0].left > 0 || click[1].left > 0; }
  };
  struct Ramp {  // linear 5 ms ramp of one part-level factor
    float cur = 1.0f, tgt = 1.0f, step = 0.0f;
    std::uint32_t left = 0, length = kPercRampFrames;
    void set(float target) noexcept {
      if (target == tgt) return;
      tgt = target;
      left = length;
      step = (target - cur) / static_cast<float>(length);
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
    // Story 2.4: scanner (per-part state) and leakage amount (5 ms ramp).
    VibratoScanner scan;
    bool vc_on = false;
    int vc_type = 3;
    Ramp leak{0.0f, 0.0f, 0.0f, 0, kLeakRampFrames};
    std::uint32_t scan_tail = 0;  // frames of zeros still to run through the scanner after the last voice ended
    // Story 2.6: rotary speaker (per-part state: filters, delay lines, rotor speed and angle; a part stays on one worker).
    // `wet` is the 5 ms crossfade between the plain mono output (0) and the rotary output (1); at 0 with target 0 the
    // rotary stage is bypassed exactly (no processing, only the rotors keep time).
    RotarySpeaker rot;
    bool rot_on = false;
    Ramp wet{0.0f, 0.0f, 0.0f, 0, kRotaryRampFrames};
    std::uint32_t rot_tail = 0;  // frames of zeros still to run through the rotary stage after the last voice ended
    std::uint64_t rot_on_frame = ~0ull;  // engine frame at which the stage was last switched on with no crossfade (same-frame speed commands settle)
    bool rot_dirty = false;      // the stage holds signal state that must be cleared when it is bypassed
    bool rot_live() const noexcept { return wet.cur != 0.0f || wet.tgt != 0.0f; }
    void update_scanner(bool immediate) noexcept {
      const ScannerMode m = vc_on ? static_cast<ScannerMode>(1 + vc_type) : ScannerMode::kOff;
      if (immediate) scan.set_mode_immediate(m);  // nothing sounds: no crossfade needed
      else scan.set_mode(m);
    }
  };

  bool part_sounding(std::uint32_t part) const noexcept {
    for (const Voice& v : voices_)
      if (v.active && v.part == part) return true;
    return false;
  }
  EngineStatus spawn_voice(std::uint32_t channel, std::uint32_t note, bool contact_driven) noexcept;
  void set_gate(Voice& v, std::uint32_t group, float target) noexcept;
  void release_key(Voice& v) noexcept;  // the key's last contact opened or Note Off: the voice goes to its release
  Voice* held_voice(std::uint32_t channel, std::uint32_t note) noexcept;
  void start_click(Voice& v, std::uint32_t which, float level, float scale) noexcept;
  void apply_percussion_targets(Part& p) noexcept;
  void emit_part(Part& p, const float* dry, std::uint32_t frames, float* left, float* right) noexcept;
  void publish_rotary(std::uint32_t part) noexcept;

  void render_chunk(float* const* left, float* const* right, std::uint32_t frames) noexcept VELORA_NONBLOCKING;

  std::vector<Voice> voices_;
  std::array<Part, kPartCount> parts_{};
  std::vector<double> osc_cos_, osc_sin_;  // generator state: kPartCount x kWheelCount unit phasors
  std::array<double, tonewheel::kWheelCount> rot_cos_{}, rot_sin_{};  // per-frame rotation of each wheel
  std::vector<float> wheels_;              // kPartCount x kWheelCount x kBlockFrames
  std::array<float, tonewheel::kDrawbarCount * kBlockFrames> gains_{};  // scratch: per-frame drawbar gains
  std::array<float, kPartCount * kBlockFrames> mix_{};                  // scratch: per-part mono mix
  std::array<float, tonewheel::kWheelCount * 2> leak_pattern_{};  // [wheel-1][side], filled at construction
  std::array<std::uint8_t, kPartCount * 128> contact_released_{};  // key whose voice ended by contacts opening: a later Note Off is benign
  std::uint32_t contact_count_ = 3;
  std::uint64_t contact_applied_ = 0, contact_ignored_ = 0;
  std::array<std::atomic<std::uint32_t>, kPartCount> rot_state_{}, rot_horn_{}, rot_drum_{};  // published rotary state
  std::uint32_t voice_capacity_ = kVoiceCapacity;
  std::uint32_t part_mask_ = 0xFFu;  // the parts this engine was built for
  std::uint32_t enabled_ = 0xFF;
  std::uint32_t block_pos_ = 0, gen_frames_ = 0;
  std::uint64_t clock_ = 0;
};

}  // namespace velora::core
