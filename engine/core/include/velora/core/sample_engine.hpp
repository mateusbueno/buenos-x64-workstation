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
constexpr std::uint32_t sample_voice_cost_units(SampleInterpolation q) noexcept {
  return q == SampleInterpolation::kLinear ? 3u : 5u;
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
  void set_part_sample_bank(std::uint32_t part, SampleBank* bank) noexcept VELORA_NONBLOCKING override {
    if (part >= part_bank_.size()) return;
    part_bank_[part] = bank;
    part_bound_[part] = true;
  }
  // Forget every per-part binding (parts play the default instrument again). Real-time safe.
  void clear_part_banks() noexcept {
    part_bank_.fill(nullptr);
    part_bound_.fill(false);
  }
  void clear_part_sample_banks() noexcept VELORA_NONBLOCKING override { clear_part_banks(); }

  // Deterministic RNG for lorand/hirand: one draw per note on and per note off.
  void set_seed(std::uint64_t seed) noexcept;

  EngineTypeId type_id() const noexcept override { return EngineTypeId::kSample; }
  std::uint32_t cost_units_per_voice() const noexcept override { return sample_voice_cost_units(cfg_.interpolation); }
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
  float next_random() noexcept;
  void render_voice(Voice& v, float* left, float* right, std::uint32_t frames) noexcept;

  Config cfg_;
  std::uint32_t capacity_ = 0;
  std::vector<Voice> voices_;
  std::shared_ptr<SampleBank> default_bank_;           // set_instrument()
  std::array<SampleBank*, 16> part_bank_{};            // per-channel banks (set_part_sample_bank), borrowed from the snapshot generation
  std::array<bool, 16> part_bound_{};
  // The bank each (channel, note) last started on (identity only, never dereferenced): a release-trigger region fires at note off only on
  // the bank the note started on, so a part rebound while the key is held does not mix the new bank with the old note's state.
  std::array<const SampleBank*, 16 * 128> last_bank_{};
  std::array<std::uint8_t, 16 * 128> last_vel7_{};  // note-on velocity per (channel, note)
  std::array<float, 4096> pan_table_{};
  float tr_release_rate_off_ = 0.0f;  // exp rate of the 6 ms fast release
  std::uint64_t rng_ = 1;
  std::uint64_t clock_ = 0;
  float gain_ = 1.0f;
  std::uint32_t unplayable_regions_ = 0;
  std::uint64_t skipped_unplayable_ = 0;
};

}  // namespace velora::core
