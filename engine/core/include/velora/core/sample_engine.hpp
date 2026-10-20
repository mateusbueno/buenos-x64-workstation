#pragma once
// SampleEngine (Story 1.15): plays one RAM-resident instrument through the static Engine contract.
//
// Behaviour implemented (the SFZ subset of Story 1.13, matched against sfizz 1.2.3 by the null
// tests in tools/offline-render): region selection by key, 7-bit velocity, round robin
// (seq_length/seq_position) and random (lorand/hirand) with a seedable deterministic RNG; pitch
// from pitch_keycenter + transpose + tune (and the sample/engine rate ratio); volume and pan;
// offset; the ampeg_* envelope; loop_mode (no_loop, one_shot, loop_continuous, loop_sustain);
// trigger=release regions fired on note off. The exact curve shapes and sfizz-specific details are
// documented in engine/core/README.md.
//
// Real-time contract: set_instrument()/clear_instrument() are setup-time (they allocate) and must
// not run concurrently with start_voice/stop_voice/render. start_voice, stop_voice, set_param,
// render and voice_info never allocate, lock or block.
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "velora/core/engine.hpp"
#include "velora/core/sample_bank.hpp"
#include "velora/ports/sample_source.hpp"

namespace velora::core {

enum class SampleInterpolation : std::uint8_t {
  kLinear = 0,    // 2 taps (sfizz sample quality 1)
  kHermite3 = 1,  // 4 taps, Catmull-Rom (sfizz sample quality 2, the sfizz default)
};

// Cost units one active voice consumes. A test-tone voice is 1 unit; a sample voice is 1 unit of
// envelope/gain/pan work plus one unit per interpolation tap (stereo samples read two channels but
// share the weights). Relative figures to be calibrated on the board (Stories 1.18/1.19).
//
// Story 5.2: an engine declared with `Config::extended_voices` budgets every voice as a worst case that may run a per-voice filter
// (a biquad in double per channel: kSampleFilterCostUnits) and a smoothed CC crossfade (kSampleCrossfadeCostUnits) on top of the plain voice. The
// declaration is per engine and fixed at construction (the arbiter refuses a cost that changes after bind()), so the default
// engine keeps the Story 1.15 figures (3 / 5) and every existing floor and budget. The extra figures come from
// `sample_bench` (tests/sample_bench.cpp, -O3 host figures, see engine/core/README.md); still to be calibrated on the board.
inline constexpr std::uint32_t kSampleFilterCostUnits = 2;
inline constexpr std::uint32_t kSampleCrossfadeCostUnits = 1;
// Story 6.5: the length, in frames of engine output, of the fade to silence a voice makes before it reaches a damaged block of a protected library (and of the
// fade back in after it). It is a straight gain ramp; the voice reads the block-damage flags that non-real-time threads set (ports::SamplePcm::damaged_blocks).
inline constexpr std::uint32_t kIntegrityFadeFrames = 64;
constexpr std::uint32_t sample_voice_cost_units(SampleInterpolation q, bool extended = false) noexcept {
  return (q == SampleInterpolation::kLinear ? 3u : 5u) + (extended ? kSampleFilterCostUnits + kSampleCrossfadeCostUnits : 0u);
}

class SampleEngine final : public Engine {
 public:
  static constexpr std::uint32_t kParamGain = 1;  // linear output gain, default 1
  static constexpr float kMaxGain = 16.0f;         // kParamGain is clamped to [0, kMaxGain]
  static constexpr std::uint32_t kParamSeed = 2;  // RNG seed (non-negative integer value; exact below 2^24, prefer set_seed)

  struct Config {
    std::uint32_t max_voices = 64;
    std::uint32_t sample_rate = 48000;  // engine rate; samples at other rates are resampled by the pitch ratio
    SampleInterpolation interpolation = SampleInterpolation::kHermite3;
    std::uint64_t seed = 1;
    // TEST-ONLY emulation of an sfizz 1.2.3 defect, off (0) in production. sfizz computes a voice's
    // `age` as min(age + block - trigger_delay, 0), which stays 0 after the first block, so the voice
    // "just started" rule (the first frame of a block does not advance the sample position) fires a
    // second time: every voice repeats one sample position at the first block boundary after the
    // block it started in. With N = the oracle's block size (64 in the null tests, aligned with the
    // engine's frame clock), the engine reproduces that hold so the null residual measures the
    // engine and not sfizz's stutter. See engine/core/README.md and tools/oracle/README.md.
    std::uint32_t oracle_block_frames = 0;
    // Story 5.2: declare the worst-case voice cost of filtered, crossfaded voices (see sample_voice_cost_units). Off = the plain
    // cost; instruments that use filters or CC crossfades still play, and every such voice that starts on an undeclared engine is
    // counted in undeclared_extended_voices() so the under-budgeting is visible.
    bool extended_voices = false;
  };

  explicit SampleEngine(const Config& cfg);

  // Setup-time. Builds the per-note region lookup and the per-region constants from `src`, stops all
  // voices and resets round-robin counters. The source's memory is borrowed and must outlive its use.
  // Regions that cannot play (sample missing, empty, wrong shape, offset beyond the sample) are
  // flagged and counted: unplayable_regions().
  // This is the DEFAULT instrument, played by every channel that has no per-part bank (the single-instrument path of the
  // Story 1.15 null tests).
  bool set_instrument(const velora::ports::SampleSource& src);
  void clear_instrument();

  // Per-part instruments (Story 4.3). prepare() builds the bank of one instrument on the CONTROL thread (it allocates; it reads only
  // the engine's constant configuration, so it may run while audio runs). `owner` is kept alive by the bank and must own the PCM
  // that `src` lends. A Live Set carries one bank per part inside its snapshot generation; the block loop binds them at a
  // generation change with set_part_sample_bank() (real-time safe: it stores a pointer). A part whose bank is bound plays ONLY
  // that bank (nullptr = the part is silent); a part never bound plays the default instrument. Voices keep the bank they started
  // on, so a rebind never cuts or corrupts a sounding note; the caller keeps the old bank alive until those voices are gone (the
  // generation ledger does, AD-23). The seam for heads/streaming (Stories 1.16/1.17) is the SampleSource behind the bank.
  std::shared_ptr<SampleBank> prepare(const velora::ports::SampleSource& src, std::shared_ptr<const void> owner = nullptr) const;
  const Config& config() const noexcept { return cfg_; }
  // Binding a part to a bank it is not already bound to resets the part's keyswitch state to the bank's sw_default (Story 5.2); the
  // block loop calls this at every Live Set generation, so re-binding the same bank changes nothing.
  void set_part_sample_bank(std::uint32_t part, SampleBank* bank) noexcept VELORA_NONBLOCKING override;
  // Forget every per-part binding (parts play the default instrument again). Real-time safe.
  void clear_part_banks() noexcept;
  void clear_part_sample_banks() noexcept VELORA_NONBLOCKING override { clear_part_banks(); }

  // Story 5.2 (see engine/core/README.md "Keyswitches, CC crossfades and filters").
  // Controllers: the part's CcState, 128 values in CC units (0..127, fractional for MIDI 2.0 resolution). Sample engine defaults are
  // sfizz's: 0 everywhere except CC7 = 100, CC10 = 63.5, CC11 = 127. Delivered by the block loop, RT-owned, lock-free.
  bool set_part_cc(std::uint32_t part, std::uint32_t cc, std::uint32_t value32) noexcept VELORA_NONBLOCKING override;
  float part_cc(std::uint32_t part, std::uint32_t cc) const noexcept { return cc_[part & 15u][cc & 127u]; }
  void key_released(std::uint32_t channel, std::uint32_t note) noexcept VELORA_NONBLOCKING override;
  void reset_play_state() noexcept VELORA_NONBLOCKING override;
  // The part's current "last keyswitch" (sw_last state), -1 = none yet.
  int part_last_keyswitch(std::uint32_t part) const noexcept { return ks_[part & 15u].last; }
  bool part_key_held(std::uint32_t part, std::uint32_t key) const noexcept { return key < 128 && ks_[part & 15u].is_held(static_cast<int>(key)); }
  int part_previous_note(std::uint32_t part) const noexcept { return ks_[part & 15u].prev; }

  // Deterministic RNG for lorand/hirand: one draw per note on and per note off.
  void set_seed(std::uint64_t seed) noexcept;

  EngineTypeId type_id() const noexcept override { return EngineTypeId::kSample; }
  std::uint32_t cost_units_per_voice() const noexcept override { return sample_voice_cost_units(cfg_.interpolation, cfg_.extended_voices); }
  std::uint32_t voice_capacity() const noexcept override { return capacity_; }

  // Starts a voice per matching region. Voice slots come from the pool arbiter when one is attached
  // (it may steal); kPoolExhausted when at least one matching region found no slot; kOk otherwise (also when no region matches: nothing to play is not an error).
  EngineStatus start_voice(std::uint32_t channel, std::uint32_t note,
                           std::uint32_t velocity16) noexcept VELORA_NONBLOCKING override;
  // Releases the note's voices and fires its release-trigger regions. kVoiceNotFound when the note
  // had no voice that was still held and no release region fired; kPoolExhausted when a release
  // region found no free voice. Release bookkeeping (velocity, attack voices) is per (channel, note).
  EngineStatus stop_voice(std::uint32_t channel, std::uint32_t note) noexcept VELORA_NONBLOCKING override;
  void set_param(std::uint32_t param_id, float value) noexcept VELORA_NONBLOCKING override;
  void render(float* left, float* right, std::uint32_t frames) noexcept VELORA_NONBLOCKING override;
  void render_split(float* const* left, float* const* right, std::uint32_t frames) noexcept VELORA_NONBLOCKING override;
  bool voice_info(std::uint32_t slot, VoiceInfo& out) const noexcept VELORA_NONBLOCKING override;
  bool begin_steal_fade(std::uint32_t slot, std::uint32_t ramp_frames) noexcept VELORA_NONBLOCKING override;

  std::uint32_t active_voices() const noexcept;
  SampleInterpolation interpolation() const noexcept { return cfg_.interpolation; }
  // Regions flagged unplayable by the last set_instrument().
  std::uint32_t unplayable_regions() const noexcept { return unplayable_regions_; }
  // Times a matching region was skipped because it is unplayable (missing sample data).
  std::uint64_t skipped_unplayable_starts() const noexcept { return skipped_unplayable_; }
  // Voices that started with a filter or a CC crossfade on an engine that did not declare Config::extended_voices (budgeted at the plain cost).
  std::uint64_t undeclared_extended_voices() const noexcept { return undeclared_extended_; }
  // Story 6.5: times a voice met a damaged block (a rising edge per voice: it faded out for that block). Written by the audio thread, readable anywhere.
  std::uint64_t content_integrity_events() const noexcept override { return integrity_events_.load(std::memory_order_relaxed); }
  std::uint32_t region_count() const noexcept { return default_bank_ ? static_cast<std::uint32_t>(default_bank_->regions.size()) : 0u; }

  // Envelope states, exposed for tests (order matters: >= kRelease means released).
  enum class EnvState : std::uint8_t { kDelay, kAttack, kHold, kDecay, kSustain, kRelease, kFadeout, kDone };

 private:
  using RegionRt = SampleBankRegion;

  struct Env {
    EnvState s = EnvState::kDone;
    float v = 0.0f;
    std::int32_t delay_left = 0, hold_left = 0;
    float transition_delta = 0.0f;
    float release_rate = 0.0f;
  };

  struct Voice {
    bool active = false;
    bool release_trigger = false;  // fired by a note off (not tied to the key)
    bool key_up = false;           // a note off arrived but the voice keeps playing (one_shot)
    bool ended = false;            // sample data exhausted: fast release in progress
    bool stalled = false;          // oracle_block_frames emulation: the one-frame hold already happened
    bool stolen = false;           // fading out under an arbiter steal ramp
    std::uint32_t fade_left = 0, fade_total = 1;
    std::uint32_t channel = 0, note = 0, region = 0;
    const SampleBank* bank = nullptr;  // the instrument this voice started on
    std::uint8_t vel7 = 0;
    std::uint64_t start_time = 0;
    double pos = 0.0;
    float ratio = 1.0f;
    float gain_l = 0.0f, gain_r = 0.0f;
    Env env;
    // Story 5.2. base_gain = velocity x volume (the pan gains are the region's chan_l / chan_r); the filter state per channel
    // (a mono sample uses `fl` only); the CC crossfade gain: xf_cur is the smoothed gain applied, xf_tgt the gain it moves
    // towards by xf_step per frame (a linear ramp, sfizz's LinearSmoother).
    float base_gain = 0.0f;
    SampleFilterState fl, fr;
    float xf_cur = 1.0f, xf_tgt = 1.0f, xf_step = 0.0f;
    // Story 6.5: the integrity fade of a voice over a protected sample (used only when the region has a damage map).
    float integ_gain = 1.0f;
    bool integ_seen = false, integ_bad = false;
  };

  // A part's keyswitch state (sw_last / sw_previous / held keys).
  struct KeyState {
    std::int16_t last = -1;                // last keyswitch key pressed, -1 none
    std::int16_t prev = -1;                // the note on before the current one, -1 none
    std::array<std::uint64_t, 2> held{};   // keys currently down (bit n of held[n >> 6])
    bool is_held(int k) const noexcept { return k >= 0 && k < 128 && ((held[static_cast<std::size_t>(k) >> 6] >> (k & 63)) & 1u) != 0; }
    void set_held(int k, bool down) noexcept {
      const std::uint64_t m = std::uint64_t{1} << (k & 63);
      if (down) held[static_cast<std::size_t>(k) >> 6] |= m;
      else held[static_cast<std::size_t>(k) >> 6] &= ~m;
    }
  };

  void init_env(Env& e, const RegionRt& r) const noexcept;
  float env_next(Env& e, const RegionRt& r) const noexcept;
  // false: the voice must be dropped (released before its envelope delay elapsed).
  bool env_release(Env& e, const RegionRt& r) const noexcept;
  void env_fast_release(Env& e) const noexcept;

  bool start_region(const SampleBank& bank, std::uint32_t region, std::uint32_t channel, std::uint32_t note, std::uint8_t vel7,
                    bool release_trigger) noexcept;
  SampleBank* bank_for(std::uint32_t channel) const noexcept {
    return part_bound_[channel & 15u] ? part_bank_[channel & 15u] : default_bank_.get();
  }
  bool playing_attack_voice(const RegionRt& r, std::uint32_t channel) const noexcept;
  // Story 5.2 helpers.
  void reset_part_keys(std::uint32_t channel) noexcept;
  bool region_switched(const RegionRt& r, const KeyState& ks) const noexcept;
  float crossfade_target(const RegionRt& r, std::uint32_t channel) const noexcept;
  float next_random() noexcept;
  void render_voice(Voice& v, float* left, float* right, std::uint32_t frames) noexcept;

  Config cfg_;
  std::uint32_t capacity_ = 0;
  std::vector<Voice> voices_;
  std::shared_ptr<SampleBank> default_bank_;           // set_instrument()
  std::array<SampleBank*, 16> part_bank_{};            // per-channel banks (set_part_sample_bank), borrowed from the snapshot generation
  std::array<bool, 16> part_bound_{};
  std::array<std::uint64_t, 16> part_bank_id_{};      // SampleBank::id of the bound bank (0 = bound to nothing)
  // The bank each (channel, note) last started on (identity only, never dereferenced): a release-trigger region fires at note off only on
  // the bank the note started on, so a part rebound while the key is held does not mix the new bank with the old note's state.
  std::array<std::uint64_t, 16 * 128> last_bank_{};  // SampleBank::id (0 = none)
  std::array<std::uint8_t, 16 * 128> last_vel7_{};  // note-on velocity per (channel, note)
  std::array<float, 4096> pan_table_{};
  float tr_release_rate_off_ = 0.0f;  // exp rate of the 6 ms fast release
  std::uint64_t rng_ = 1;
  std::uint64_t clock_ = 0;
  float gain_ = 1.0f;
  std::uint32_t unplayable_regions_ = 0;
  std::uint64_t skipped_unplayable_ = 0;
  std::uint64_t undeclared_extended_ = 0;
  std::atomic<std::uint64_t> integrity_events_{0};
  std::array<KeyState, 16> ks_{};                       // per part (channel) keyswitch state
  std::array<std::array<float, 128>, 16> cc_{};         // per part CcState, CC units
  std::int32_t xf_frames_ = 240;                        // CC crossfade smoothing time in frames (sfizz xfadeSmoothing = 5 ms)
};

}  // namespace velora::core
