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
#include "velora/core/part_params.hpp"
#include "velora/core/sample_bank.hpp"
#include "velora/core/ump.hpp"
#include "velora/core/worker_pool.hpp"
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
// Story 13.8: an engine that also declares `Config::second_filter_voices` budgets every voice for a second filter in series (a stereo biquad pair: measured at about the
// cost of the first one, `sample_bench`) on top of the extended worst case. Fixed per engine like the rest; the default engine and the extended-only engine keep their
// figures, so no existing floor or budget changes.
inline constexpr std::uint32_t kSampleFilter2CostUnits = 3;
// Story 6.5: the length, in frames of engine output, of the fade to silence a voice makes before it reaches a damaged block of a protected library (and of the
// fade back in after it). It is a straight gain ramp; the voice reads the block-damage flags that non-real-time threads set (ports::SamplePcm::damaged_blocks).
inline constexpr std::uint32_t kIntegrityFadeFrames = 64;
// Story 1.17: the length, in frames of engine output, of the fade to silence a streaming voice makes when the stream data it needs has not arrived (and of the fade back in
// when it has). The voice holds its last output sample under the fade, so there is no click; the position keeps advancing.
inline constexpr std::uint32_t kStreamFadeFrames = 64;
constexpr std::uint32_t sample_voice_cost_units(SampleInterpolation q, bool extended = false, bool second_filter = false) noexcept {
  return (q == SampleInterpolation::kLinear ? 3u : 5u) + (extended ? kSampleFilterCostUnits + kSampleCrossfadeCostUnits : 0u) +
         (extended && second_filter ? kSampleFilter2CostUnits : 0u);
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
    // Story 13.8: with extended_voices, also budget every voice for a second filter (sample_voice_cost_units). A voice that starts with a second filter on an engine that
    // did not declare it is counted in undeclared_second_filter_voices().
    bool second_filter_voices = false;
    // Story 3.6: release-trigger regions use the Note Off's release velocity (when it is not 0) instead of the note-on velocity, to select the velocity layer
    // and to scale the gain (velocity squared, as for attacks): a fast key release plays a louder release sample. Off = sfizz behaviour (note-on velocity),
    // which the null tests need. The host turns it on. No curve is applied here: the keybed reader's velocity chain is the only place that shapes velocity.
    bool release_velocity_triggers = false;
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
  // Story 13.7: pitch bend (value32 centre 0x80000000) is stored per part; a region follows it only when it set a bend opcode (bend_up / bend_down / bend_step /
  // bend_smooth). Returns true only when the part's bank has such a region, so a part without them counts the event as ignored exactly as before. A CC that moves into
  // the trigger range of a region (on_locc / on_hicc) starts that region's voice from the controller, in set_part_cc(); take_cc_started() then reports the key the
  // voice was bound to (the region's pitch_keycenter) so the block loop can bind the snapshot generation to it.
  bool set_part_pitch_bend(std::uint32_t part, std::uint32_t value32) noexcept VELORA_NONBLOCKING override;
  float part_bend(std::uint32_t part) const noexcept { return bend_[part & 15u]; }
  // The read position, in frames of the sample, of the voice in `slot` (tests and diagnostics; 0 for a free slot).
  double voice_position(std::uint32_t slot) const noexcept { return slot < voices_.size() && voices_[slot].active ? voices_[slot].pos : 0.0; }
  // Drains one key per call from the set of keys that controller voices started since the last drain (a 128-bit mask: one controller event can start regions with different pitch_keycenters).
  bool take_cc_started(std::uint32_t& key) noexcept VELORA_NONBLOCKING override {
    for (std::uint32_t w = 0; w < 2; ++w) {
      if (cc_started_[w] == 0) continue;
      std::uint32_t b = 0;
      while (!((cc_started_[w] >> b) & 1u)) ++b;
      cc_started_[w] &= ~(std::uint64_t{1} << b);
      key = w * 64u + b;
      return true;
    }
    return false;
  }
  // The keyboard replay of a Live Set switch: stores the controller without firing triggers (no controller moved).
  bool set_part_cc_replay(std::uint32_t part, std::uint32_t cc, std::uint32_t value32) noexcept VELORA_NONBLOCKING override;
  bool key_released(std::uint32_t channel, std::uint32_t note) noexcept VELORA_NONBLOCKING override;
  void all_keys_released(std::uint32_t channel) noexcept VELORA_NONBLOCKING override { ks_[channel & 15u].held = {}; }  // trigger=first / legato must not see stuck keys
  void reset_play_state() noexcept VELORA_NONBLOCKING override;
  // Registry part parameters part_attack (403, ms) and part_release (404, ms) (Story 9.9 follow-up; see part_params.hpp "engagement"). Once moved off
  // their registry defaults (10 ms / 300 ms) they REPLACE the instrument's own ampeg_attack / ampeg_release for the part: the attack applies to
  // voices that start afterwards, the release is applied when a voice is released (so a held note follows the knob), through the same linear attack
  // and exponential release shapes the regions use. Untouched, the instrument's own envelope plays (sfizz-exact). part_brightness / part_eq_*
  // (402, 405..408) are not engine parameters: the Mixer's per-part tone stage takes them for every engine.
  void set_part_param(std::uint32_t part, std::uint32_t param_id, float value) noexcept VELORA_NONBLOCKING override;
  void set_part_param_edit(std::uint32_t part, std::uint32_t param_id, float value) noexcept VELORA_NONBLOCKING override;
  // The part's current "last keyswitch" (sw_last state), -1 = none yet.
  int part_last_keyswitch(std::uint32_t part) const noexcept { return ks_[part & 15u].last; }
  bool part_key_held(std::uint32_t part, std::uint32_t key) const noexcept { return key < 128 && ks_[part & 15u].is_held(static_cast<int>(key)); }
  int part_previous_note(std::uint32_t part) const noexcept { return ks_[part & 15u].prev; }

  // Story 1.18: parallel voice render (opt-in; nullptr = the serial render, bit-identical to before). With a pool the voice slots are split into
  // WorkerPool::kPartitions fixed partitions (slot % 3); each partition renders its voices into its own scratch buses, the pool's barrier
  // follows, and the scratch buses are summed into the outputs in partition order 0, 1, 2 (per part in render_split). Voices write only their own
  // state and their partition's scratch. The result does not depend on how many threads execute the partitions (1 inline or 3), and differs from
  // the serial render only by float summation order. Setup-time (allocates the scratch for `max_frames` frames per call; a longer call renders
  // serially); the pool is borrowed and must outlive its use.
  void set_parallel_voice_pool(WorkerPool* pool, std::uint32_t max_frames) override;
  bool parallel_voice_render() const noexcept { return pool_ != nullptr; }

  // Story 1.17: streaming from the SSD ahead of the playhead (opt-in; nullptr = off, every voice behaves as in Story 1.16 / 1.15). With a service, a voice whose region has a
  // tail reader (SamplePcm::tail_source) registers a stream at note start (StreamService::open, non-blocking), plays its head, continues from the stream's ring buffer
  // bit for bit as a fully resident render would, and gives the ring back when it ends. Data not there in time = silence with a short fade (kStreamFadeFrames), one counted
  // underrun per episode, playback continues and resumes when the data arrives. A voice that gets no ring keeps the Story 1.16 placeholder (fade at the head end, counted).
  // Setup-time; the service is borrowed, must outlive its use and no voice may be sounding when it is replaced (clear_instrument() / stop first).
  void set_stream_service(velora::ports::StreamService* service) noexcept { stream_ = service; }
  velora::ports::StreamService* stream_service() const noexcept { return stream_; }
  // Voices of a region with a tail that started with a service bound but got no ring (pool exhausted or request queue full): each plays the Story 1.16 fade at its head end.
  std::uint64_t stream_open_failures() const noexcept { return stream_open_failures_.load(std::memory_order_relaxed); }

  // Deterministic RNG for lorand/hirand: one draw per note on and per note off.
  void set_seed(std::uint64_t seed) noexcept;

  EngineTypeId type_id() const noexcept override { return EngineTypeId::kSample; }
  std::uint32_t cost_units_per_voice() const noexcept override { return sample_voice_cost_units(cfg_.interpolation, cfg_.extended_voices, cfg_.second_filter_voices); }
  std::uint32_t voice_capacity() const noexcept override { return capacity_; }

  // Starts a voice per matching region. Voice slots come from the pool arbiter when one is attached
  // (it may steal); kPoolExhausted when at least one matching region found no slot; kOk otherwise (also when no region matches: nothing to play is not an error).
  EngineStatus start_voice(std::uint32_t channel, std::uint32_t note,
                           std::uint32_t velocity16) noexcept VELORA_NONBLOCKING override;
  // Releases the note's voices and fires its release-trigger regions. kVoiceNotFound when the note
  // had no voice that was still held and no release region fired; kPoolExhausted when a release
  // region found no free voice. Release bookkeeping (velocity, attack voices) is per (channel, note).
  EngineStatus stop_voice(std::uint32_t channel, std::uint32_t note) noexcept VELORA_NONBLOCKING override;
  // Story 3.6: stop_voice() with the Note Off's 16-bit release velocity (see Config::release_velocity_triggers).
  EngineStatus release_voice(std::uint32_t channel, std::uint32_t note, std::uint32_t release_velocity16) noexcept VELORA_NONBLOCKING override;
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
  // Story 13.8: voices that started with a second filter on an engine that did not declare Config::second_filter_voices (budgeted without it).
  std::uint64_t undeclared_second_filter_voices() const noexcept { return undeclared_second_; }
  // Story 6.5: times a voice met a damaged block (a rising edge per voice: it faded out for that block). Written by the audio thread, readable anywhere.
  std::uint64_t content_integrity_events() const noexcept override { return integrity_events_.load(std::memory_order_relaxed); }
  std::uint64_t stream_underrun_events() const noexcept override { return underrun_events_.load(std::memory_order_relaxed); }
  std::uint32_t region_count() const noexcept { return default_bank_ ? static_cast<std::uint32_t>(default_bank_->regions.size()) : 0u; }

  // Story 3.11, half-pedal damping. The "half-pedal zone" is the pedal in (0, hold threshold): below half travel. At exactly half the note switches from the
  // maximum damping (just under: 10x longer) to held (design limitation: revisit with a real pedal). The release of a voice that is released while the sustain pedal (CC 64, `cc64` in CC units 0..127, fractional for MIDI 2.0
  // resolution) sits BELOW the hold threshold (63.5, the arbiter's "pedal down": >= 0x80000000 in 32 bits) is slowed: its release TIME is multiplied by
  // 1 / scale, scale = 1 - (1 - kHalfPedalMinScale) * min(1, cc64 / 63.5), i.e. a straight line from 1 (pedal up: the instrument's own release, unchanged) down to
  // kHalfPedalMinScale (0.1: ten times longer) just before the pedal holds the note; this is the felt damper lifting off the strings. At or above the threshold the notes
  // are held by the arbiter and the law does not apply (scale 1: a panic or a steal releases at the normal rate). Applied once, when the release begins; a voice that is
  // already releasing keeps its rate; release-trigger voices and the fast steal release are not touched. A binary CC 64 (0 or 127) gives scale 1, so the default is unchanged.
  static constexpr float kHalfPedalHoldCc = static_cast<float>(127.0 * Ump::kSustainDownThreshold32 / 4294967295.0);  // 63.5: the arbiter's threshold in CC units
  static constexpr float kHalfPedalMinScale = 0.1f;
  static float half_pedal_scale(float cc64) noexcept {
    if (!(cc64 > 0.0f)) return 1.0f;
    if (cc64 >= kHalfPedalHoldCc) return 1.0f;
    return 1.0f - (1.0f - kHalfPedalMinScale) * (cc64 / kHalfPedalHoldCc);
  }

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
    float attack_step = 1.0f;  // per-voice copy of the attack rate (the region's, or the part's part_attack override)
    float start = 0.0f;        // Story 13.5: per-voice copy of the start level (the region's, or its ampeg_start_oncc value at voice start)
    // Story 13.4: per-voice copies of the decay rate and the sustain level (the region's, or its ampeg_vel2decay / vel2sustain value at the note-on velocity).
    float decay_rate = 0.0f, sustain = 1.0f;
    bool free_running = false;
  };

  struct Voice {
    bool active = false;
    bool release_trigger = false;  // fired by a note off (not tied to the key)
    bool key_up = false;           // a note off arrived but the voice keeps playing (one_shot)
    bool ended = false;            // sample data exhausted: fast release in progress
    bool stalled = false;          // oracle_block_frames emulation: the one-frame hold already happened
    bool stolen = false;           // fading out under an arbiter steal ramp
    // Story 13.6 (sfizz Voice::offed_ and the voice's age). offed: released by a choke group or a polyphony limit (such a voice is not counted again and not cut
    // again). order: the start order of the voice, the engine's notion of "oldest" (smaller = older); event: the note on / note off that started it (its sister
    // voices share it); mark: scratch bit of the note polyphony check.
    bool offed = false, mark = false;
    // Story 13.7: started by a controller (on_locc / on_hicc), not by a key: no note off, key up or pedal ever releases it (note = the region's pitch_keycenter).
    bool cc_voice = false;
    // Story 13.7 pitch bend (only a region with `bends` uses them): the smoothed cents applied (and the previous frame's), the target they move to by bend_step per frame, the pitch factor of
    // bend_cur (recomputed while it moves).
    bool bend_live = false;  // the voice has stepped at least one frame (a wheel move before that needs no position correction)
    float bend_cur = 0.0f, bend_prev = 0.0f, bend_tgt = 0.0f, bend_step = 0.0f, bend_fac = 1.0f;
    std::uint64_t order = 0, event = 0;
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
    // Story 13.8: the voice's own filter coefficients, computed (or copied from the region) when the voice starts, and the second filter's coefficients and state.
    SampleFilterCoeffs fk, fk2;
    SampleFilterState fl2, fr2;
    float xf_cur = 1.0f, xf_tgt = 1.0f, xf_step = 0.0f;
    // Story 6.5: the integrity fade of a voice over a protected sample (used only when the region has a damage map).
    float integ_gain = 1.0f;
    bool integ_seen = false, integ_bad = false;
    // Story 1.17 (streaming). `streamed`: the voice started with a ring, so the sample's length is region.total_frames and the head is only its first part. `ring` is
    // non-null while the voice still needs the stream (it is given back as soon as the voice ends, or has captured the held last frame). seg / seg_start: the segment the
    // ring is in and its first frame; lo_pub: the last consumed_lo the voice published in this segment (a loop wrap that finds it still at the segment start needs no new seek).
    velora::ports::StreamRing* ring = nullptr;
    bool streamed = false;
    bool hold_valid = false;  // the held last frame of a streamed sample has been read (hold_l / hold_r)
    bool starved = false;     // the stream data the voice needs has not arrived (an underrun episode is open)
    std::uint32_t seg = 0;
    std::uint64_t seg_start = 0, lo_pub = 0;
    float hold_l = 0.0f, hold_r = 0.0f;
    float last_l = 0.0f, last_r = 0.0f;  // the last interpolated frame (held, faded, while starved)
    float sgain = 1.0f;                  // stream fade gain: 1 normally, ramps to 0 while starved (reset to 1 at re-entry)
    float rx_l = 0.0f, rx_r = 0.0f;      // the held output at re-entry after a starve, crossfaded away over kStreamFadeFrames (rx_n counts, 0 = not crossfading)
    std::uint32_t rx_n = 0;
  };

  // A part's keyswitch state (sw_last / sw_previous / held keys).
  struct KeyState {
    std::int16_t last = -1;                // last keyswitch key pressed, -1 none
    std::int16_t prev = -1;                // the note on before the current one, -1 none
    // Story 13.5 (pseudo-CCs, the part's "MidiState" of sfizz): the value of the last note on's alternate / key delta / velocity, and the alternate that the next note on takes.
    float alt = 0.0f, alt_next = 0.0f, keydelta = 0.0f;
    std::array<std::uint64_t, 2> held{};   // keys currently down (bit n of held[n >> 6])
    // Story 13.3 (trigger=first / legato): is any key other than `k` down? Fixed-size bit test, no allocation.
    bool others_held(int k) const noexcept {
      std::array<std::uint64_t, 2> h = held;
      if (k >= 0 && k < 128) h[static_cast<std::size_t>(k) >> 6] &= ~(std::uint64_t{1} << (k & 63));
      return (h[0] | h[1]) != 0;
    }
    bool is_held(int k) const noexcept { return k >= 0 && k < 128 && ((held[static_cast<std::size_t>(k) >> 6] >> (k & 63)) & 1u) != 0; }
    void set_held(int k, bool down) noexcept {
      const std::uint64_t m = std::uint64_t{1} << (k & 63);
      if (down) held[static_cast<std::size_t>(k) >> 6] |= m;
      else held[static_cast<std::size_t>(k) >> 6] &= ~m;
    }
  };

  // Story 1.17: gives the voice's ring back (idempotent) / ends the voice (releases the ring, then clears `active`). Atomics only: safe on a render worker thread.
  void release_stream(Voice& v) noexcept {
    if (v.ring != nullptr) {
      if (stream_ != nullptr) stream_->close(v.ring);
      v.ring = nullptr;
    }
  }
  void end_voice(Voice& v) noexcept {
    release_stream(v);
    v.active = false;
  }
  void init_env(Env& e, const SampleBank& bank, const RegionRt& r, std::uint32_t channel, std::uint8_t vel7) const noexcept;
  float env_next(Env& e, const RegionRt& r) const noexcept;
  // false: the voice must be dropped (released before its envelope delay elapsed).
  bool env_release(Env& e, const RegionRt& r) const noexcept;
  void env_fast_release(Env& e) const noexcept;

  bool start_region(const SampleBank& bank, std::uint32_t region, std::uint32_t channel, std::uint32_t note, std::uint8_t vel7,
                    bool release_trigger, bool cc_voice = false) noexcept;
  // Story 13.7. The cents the wheel position asks of a region (sfizz Region::getBendInCents, quantised to bend_step above 1), and the controller trigger of set_part_cc.
  static float bend_cents(const SampleBankRegion& r, float wheel) noexcept;
  bool put_part_cc(std::uint32_t part, std::uint32_t cc, std::uint32_t value32, bool fire) noexcept;
  void cc_trigger(SampleBank& bank, std::uint32_t part, std::uint32_t cc, float value) noexcept;
  SampleBank* bank_for(std::uint32_t channel) const noexcept {
    return part_bound_[channel & 15u] ? part_bank_[channel & 15u] : default_bank_.get();
  }
  // Story 13.6, sfizz's order at a voice start: choke groups (Synth::checkOffGroups), then the note, region and group polyphony limits (VoiceManager::
  // checkPolyphony; the engine-wide limit is the voice pool's arbiter). Real-time safe: fixed loops over the voice table, no allocation.
  void apply_voice_limits(const SampleBank& bank, std::uint32_t region, std::uint32_t channel, std::uint32_t note, std::uint8_t vel7, bool from_cc = false) noexcept;
  void off_voice(Voice& v) noexcept;
  void off_sisters(std::uint64_t event) noexcept;  // the sisters that have not been cut yet
  bool playing_attack_voice(const RegionRt& r, std::uint32_t channel, bool match_velocity = true) const noexcept;
  // Story 5.2 helpers.
  void reset_part_keys(std::uint32_t channel) noexcept;
  // Story 13.5. seed_part_cc: the bank's set_cc defaults into the part's CcState (at set_instrument and when a part binds another bank). cc_gate_open: every gate of the
  // region holds at the part's current controller values. pseudo_: the note-derived controllers (131, 135, 136, 137, 140, 141) of the event that is starting voices;
  // never filled from set_part_cc. cc_source / cc_curve: the value (normalized, raw semitones for 140 / 141) and the curve of a modulation entry.
  struct PseudoCc {
    float vel = 0.0f, vel_eg = 0.0f, uni = 0.0f, bi = 0.0f, alt = 0.0f, keydelta = 0.0f;
  };
  void seed_part_cc(std::uint32_t channel) noexcept;
  bool cc_gate_open(const RegionRt& r, std::uint32_t channel) const noexcept;
  float cc_norm(unsigned cc, std::uint32_t channel, bool eg) const noexcept;
  float cc_curve(const SampleBank& bank, std::uint8_t curve, float norm) const noexcept;
  float cc_mod_value(const SampleBank& bank, const velora::ports::SampleCcMod& m, std::uint32_t channel, bool eg) const noexcept;
  // Story 13.8: a tuned filter's coefficients for the voice that is starting (key track, velocity track, CC entries), see sample_filter.hpp.
  SampleFilterCoeffs voice_filter(const SampleBank& bank, velora::ports::SampleFilterType type, float cutoff_hz, float resonance_db,
                                  const velora::ports::SampleFilterTune& tune, std::uint32_t channel, std::uint32_t note, std::uint8_t vel7) const noexcept;
  void note_pseudo(const SampleBank* bank, std::uint32_t channel, std::uint32_t note, std::uint8_t vel7, bool note_on) noexcept;
  bool region_switched(const RegionRt& r, const KeyState& ks) const noexcept;
  float crossfade_target(const RegionRt& r, std::uint32_t channel) const noexcept;
  float next_random() noexcept;
  void put_part_param(std::uint32_t part, std::uint32_t param_id, float value, bool edit) noexcept;
  void render_voice(Voice& v, float* left, float* right, std::uint32_t frames) noexcept;
  // Story 1.18 parallel path. render_parallel() runs the partitions on the pool, then sums; render_partition() is one partition's work.
  bool parallel_applicable(std::uint32_t frames) const noexcept;
  void render_parallel(float* const* left, float* const* right, std::uint32_t frames, bool split) noexcept;
  void render_partition(std::uint32_t partition) noexcept;
  static void partition_task(void* self, std::uint32_t partition) noexcept;
  float* scratch(std::uint32_t partition, std::uint32_t bus, std::uint32_t channel) noexcept {
    return scratch_base_ + ((static_cast<std::size_t>(partition) * kSplitChannels + channel) * 2 + bus) * par_stride_;
  }

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
  EngineStatus stop_voice_impl(std::uint32_t channel, std::uint32_t note, std::uint32_t release_velocity16) noexcept;
  float tr_release_rate_off_ = 0.0f;  // exp rate of the 6 ms fast release
  std::uint64_t rng_ = 1;
  std::uint64_t clock_ = 0;
  std::uint64_t voice_order_ = 0, event_seq_ = 0;  // Story 13.6: start order of voices and the id of the current note on / off (sister voices share it)
  float gain_ = 1.0f;
  std::uint32_t unplayable_regions_ = 0;
  std::uint64_t skipped_unplayable_ = 0;
  std::uint64_t undeclared_extended_ = 0, undeclared_second_ = 0;
  std::atomic<std::uint64_t> integrity_events_{0};
  velora::ports::StreamService* stream_ = nullptr;  // Story 1.17
  std::atomic<std::uint64_t> stream_open_failures_{0};
  std::atomic<std::uint64_t> underrun_events_{0};  // Story 1.16: voices that reached the head end of a sample with a tail
  std::array<KeyState, 16> ks_{};                       // per part (channel) keyswitch state
  std::array<std::array<std::uint64_t, 2>, 16> cc_seeded_{}, cc_touched_{};  // Story 13.5: controllers a bank seeded / a real message set since the last bind
  PseudoCc pseudo_{};                                   // Story 13.5: audio thread only, set before each start_region batch
  std::array<std::array<float, 128>, 16> cc_{};         // per part CcState, CC units
  std::array<float, 16> bend_{};                        // Story 13.7: per part pitch wheel, -1..1
  std::array<std::uint64_t, 2> cc_started_{};           // Story 13.7: keys of the controller-started voices, until take_cc_started()
  // Story 1.18 (parallel voice render).
  WorkerPool* pool_ = nullptr;
  std::uint32_t par_max_frames_ = 0;
  std::uint32_t par_stride_ = 0;
  float* scratch_base_ = nullptr;                       // cache-line aligned start inside scratch_
  std::vector<float> scratch_;                          // [partition][channel][L/R][par_max_frames_]
  std::array<std::uint32_t, WorkerPool::kPartitions> partition_mask_{};  // channels (bit c; bit 0 when not split) each partition rendered into
  std::uint32_t par_frames_ = 0;
  bool par_split_ = false;
  // part_attack / part_release overrides (set_part_param): engaged flags (bit 0 attack, bit 1 release) and the converted rates per part.
  std::array<part_params::EngagedSet, 16> part_engaged_{};
  std::array<float, 16> part_attack_step_{};
  std::array<float, 16> part_release_rate_{};
  std::int32_t xf_frames_ = 240;                        // CC crossfade smoothing time in frames (sfizz xfadeSmoothing = 5 ms)
};

}  // namespace velora::core
