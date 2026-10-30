#pragma once
// VoiceArbiter (Story 1.19, AD-22): the single allocator and stealer of voices. See engine/core/README.md
// ("Voice pool arbiter") for the policy; the short version:
//
//  * Budget in cost units (Engine::cost_units_per_voice()). reserve() takes the Live Set configuration
//    and reserves, in this order: the FX budget, the organ+rotary reservation (zero without an organ),
//    each engine's floor; what is left is shared.
//  * Engines obtain every voice slot through acquire() (they call it from start_voice). When the pool is
//    full the arbiter steals across the whole Live Set. Victim order (first = stolen first): releasing
//    voices; held voices; pedal-sustained voices; the lowest held note. Inside a class: quieter first,
//    then newer, then lower (engine, slot) id. A note never steals voices of its own note-on.
//  * A stolen voice fades over a short ramp (kStealRampFrames) and keeps counting against the budget until
//    the engine reports it silent.
//  * Engines that report steal_protected() (the tonewheel organ) are never victims. Their cost is covered by
//    LiveSetConfig::organ_rotary_cost, reserved ahead of every floor; they count against that reservation
//    first and only beyond it against the shared remainder. They cannot be given a floor.
//  * Floors: an engine below its floor is always granted a voice (stealing from engines above their
//    floor); nobody steals from an engine that is at or below its floor.
//
// Until reserve() succeeds the arbiter is "unreserved": first-free allocation, no stealing, a full pool
// reports kPoolExhausted (the pre-1.19 behaviour).
//
// Threading: bind(), reserve() and clear_reservation() run when no audio runs (Live Set load); everything
// else runs in the serial phase of the block loop on the RT plane and never allocates, locks or blocks.
// The counters (steals(), exhausted(), last_steal()) are relaxed atomics and may be read from any thread
// (last_steal() is one packed atomic word, so its fields are consistent). usage() refreshes the slot
// cache and therefore mutates: it is RT-thread-only (call it from the serial phase, never concurrently).
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "velora/core/engine.hpp"
#include "velora/core/engine_registry.hpp"

namespace velora::core {

class VoiceArbiter final : public VoiceAllocator {
 public:
  static constexpr std::size_t kMaxEngines = EngineRegistry::kMaxEngines;
  // Steal fade-out length: 192 frames = 4 ms at 48 kHz. Long enough to avoid a click, short enough to
  // free the budget quickly. A Live Set may override it (LiveSetConfig::steal_ramp_frames).
  static constexpr std::uint32_t kStealRampFrames = 192;
  static constexpr std::uint32_t kMaxStealRampFrames = 4800;  // 100 ms
  // The sample engine is guaranteed at least this many stereo voices (a product floor, NFR2).
  static constexpr std::uint32_t kSampleFloorVoices = 256;
  // The FM engine is guaranteed at least this many voices (Story 12.5, FR12).
  static constexpr std::uint32_t kFmFloorVoices = 128;
  // The VA engine is guaranteed at least this many voices (Story 12.7, FR13).
  static constexpr std::uint32_t kVaFloorVoices = 64;
  // The modeled electric piano engine is guaranteed at least this many voices (Story 14.5, FR123).
  static constexpr std::uint32_t kEpFloorVoices = 64;
  // A floored engine's voice_capacity() must be at least floor + this many voices: stolen voices keep
  // their slot for the whole ramp (4 ms), and the slack lets an engine below its floor always find a free
  // slot while up to this many voices are still fading (reserve() enforces it).
  static constexpr std::uint32_t kFadeHeadroomVoices = 64;

  struct EngineFloor {
    std::uint32_t engine_type_id = 0;
    std::uint32_t voices = 0;  // guaranteed voices, in that engine's own voices
    // Story 13.12: what the Live Set needs beyond the engine's static cost declaration (kCostFeature* bits, Engine::cost_units_for_features). The engine's cost per voice for
    // this reservation (floor cost, budget arithmetic, the slot scan) is the one it declares under them; 0 = the static declaration. Only meaningful on an engine's floor entry.
    std::uint32_t cost_features = 0;
  };

  // Everything the arbiter needs when a Live Set loads. Plain data, no allocation.
  struct LiveSetConfig {
    std::uint64_t budget = 0;             // total, in cost units
    std::uint64_t fx_cost = 0;            // reserved first (a flat amount; see fx_instance_costs for the Story 2.7 computation)
    // The declared costs of the configured effect instances (insert effects, sends, master), added to fx_cost: the FX
    // reservation is computed from them (fill_fx_costs() in mixer.hpp fills the list from a Snapshot).
    static constexpr std::size_t kMaxFxInstances = 128;
    std::array<std::uint32_t, kMaxFxInstances> fx_instance_costs{};
    std::uint32_t fx_instance_count = 0;
    std::uint64_t organ_rotary_cost = 0;  // reserved second, never stolen; 0 when there is no organ
    std::array<EngineFloor, kMaxEngines> floors{};
    std::uint32_t floor_count = 0;
    std::uint32_t steal_ramp_frames = kStealRampFrames;
    // Live Set load (Story 4.3): a Live Set with no sample part has a sample floor of zero, and one with no organ part reserves nothing for
    // the organ, even though the engines stay bound. These flags waive the "bound sample engine needs a floor of at least 256" and "bound
    // organ needs 73 keys' cost" checks, and only when the floor / the organ reservation really is zero. Default false: unchanged behaviour.
    bool waive_sample_floor = false;
    bool waive_organ_reservation = false;
    bool waive_fm_floor = false;
    bool waive_va_floor = false;
    bool waive_ep_floor = false;
  };

  enum class ReserveStatus : std::uint8_t {
    kOk = 0,
    kInvalidConfig,         // duplicate floor, zero-cost engine, too many floors, bad ramp
    kUnknownEngine,         // a floor names an engine that is not bound
    kFxExceedsBudget,       // step 1 does not fit
    kOrganExceedsBudget,    // step 2 does not fit after the FX budget
    kFloorsExceedBudget,    // step 3 does not fit after FX and organ
    kFloorExceedsCapacity,  // voice_capacity() < floor + kFadeHeadroomVoices
    kSampleFloorTooLow,     // sample engine bound with a floor below kSampleFloorVoices
    kFmFloorTooLow,         // FM engine bound with a floor below kFmFloorVoices
    kVaFloorTooLow,         // VA engine bound with a floor below kVaFloorVoices
    kEpFloorTooLow,         // EP engine bound with a floor below kEpFloorVoices
  };

  struct Reservation {
    std::uint64_t budget = 0, fx = 0, organ_rotary = 0, floors = 0;
    std::uint64_t voice_budget = 0;  // budget - fx - organ_rotary: what voices may use
    std::uint64_t shared = 0;        // voice_budget - floors
  };

  struct StealRecord {  // the latest steal, for diagnostics and tests (valid == false before the first)
    bool valid = false;
    std::uint32_t engine_type_id = 0, channel = 0, note = 0, slot = 0;
    VoiceState state = VoiceState::kHeld;
  };

  // Setup-time. Attaches the arbiter to `engine` (set_voice_allocator). False when full, the type is
  // already bound, the engine's cost is 0, or a reservation is active. Bind every engine before reserve().
  bool bind(Engine& engine);
  void unbind_last() noexcept;  // setup-time: undo the latest successful bind()

#ifdef VELORA_ARBITER_TEST_HOOKS
  // Tests only: lets small pools use a floor below the engine's standard floor. Not compiled in production.
  void set_min_sample_floor_for_tests(std::uint32_t voices) noexcept { min_sample_floor_ = voices; }
  void set_min_fm_floor_for_tests(std::uint32_t voices) noexcept { min_fm_floor_ = voices; }
  void set_min_va_floor_for_tests(std::uint32_t voices) noexcept { min_va_floor_ = voices; }
  void set_min_ep_floor_for_tests(std::uint32_t voices) noexcept { min_ep_floor_ = voices; }
#endif

  // Applies a Live Set configuration atomically: either everything is reserved or nothing changes.
  // A bound engine whose voice_capacity() or static cost declaration changed since bind() is kInvalidConfig. Story 13.12: the cost per voice an engine has during a
  // reservation is its declaration under the floor entry's cost_features, installed (Engine::apply_cost_features) together with the budget and the floors. On success
  // (and in clear_reservation()) the event state (key/pedal/deferred note offs), the serial and all
  // counters are reset; voices that are already sounding keep sounding and are simply no longer tracked
  // as held/pedal-sustained until new events arrive.
  // Quiesced only: no audio may render while this runs.
  ReserveStatus reserve(const LiveSetConfig& cfg) noexcept;

  // --- seamless handover (Story 4.5) ----------------------------------------------------------------------
  // reserve() split in two so a Live Set can be switched while audio renders. prepare_reservation() is the validate-and-compute
  // half: it checks `cfg` exactly as reserve() does (same ReserveStatus, same order) and fills `out`, and changes NOTHING in the
  // arbiter, so any thread may run it while the RT thread renders (it reads only what bind() fixed: the engine table, capacities
  // and costs). commit_prepared() is the cheap half, run by the RT plane in the serial phase when it adopts the generation that
  // carries `out` (bounded copy of plain data, no allocation, lock or blocking): it installs the new budget, floors and steal
  // ramp and leaves the event state alone (held keys, pedal, deferred releases, the note serial, counters).
  //
  // Accounting rule for the overlap. The voices that sound at the commit keep their engine slots and are counted exactly as before,
  // by the slot scan (live voices at their engine's cost, fading ones as fading); nothing is released, restarted, double counted
  // or forgotten. From the commit on, the NEW budget, FX/organ reservation and floors judge every admission: an old voice is a
  // voice of its engine like any other, so it counts against that engine's new floor (an engine whose new floor is zero, because
  // the new Live Set has no part of that type, sees its old voices as shared usage) and against the new shared remainder, until it
  // is silent. If the old voices alone exceed the new budget, new notes that need room steal by the usual victim order (releasing
  // tails first, then held, ...), exactly as under one reservation; nothing is cut at the swap itself. The new floors and the
  // organ protection apply to NEW notes only: a voice is protected or a victim by its engine's rules at the moment of each steal.
  struct PreparedReservation {
    bool valid = false;
    LiveSetConfig cfg{};
    Reservation res{};
    std::array<std::uint64_t, kMaxEngines> floor_cost{};
    std::array<std::uint32_t, kMaxEngines> cost{};      // Story 13.12: each bound engine's cost per voice under this reservation (by bind order)
    std::array<std::uint32_t, kMaxEngines> features{};  // and the cost features it was computed with
    std::uint32_t ramp = kStealRampFrames;
  };
  ReserveStatus prepare_reservation(const LiveSetConfig& cfg, PreparedReservation& out) const noexcept;
  void commit_prepared(const PreparedReservation& prepared) noexcept VELORA_NONBLOCKING;
  void clear_reservation() noexcept;  // back to the unreserved mode
  // reserved() / reservation() / config() / floor_cost() are plain reads of state that commit_prepared() rewrites on the RT thread: they are
  // QUIESCED-ONLY diagnostics (set-up, the quiesced load, tests between renders). Once audio renders and Live Sets are switched seamlessly,
  // read the reservation from the published Snapshot (Snapshot::reservation) instead; a read concurrent with a commit can be torn.
  bool reserved() const noexcept { return reserved_; }
  const Reservation& reservation() const noexcept { return res_; }
  // The configuration the active reservation was made with (meaningful while reserved()): the Live Set loader snapshots it before it
  // reserves, so a load that fails later can put back exactly what was there.
  const LiveSetConfig& config() const noexcept { return cfg_; }
  std::uint64_t floor_cost(std::uint32_t engine_type_id) const noexcept;

  // --- event side (serial phase) ---------------------------------------------------------------
  void note_on(std::uint32_t channel, std::uint32_t note) noexcept VELORA_NONBLOCKING;
  // True: the engine should release the note now. False: the pedal holds it (release deferred).
  // `engine_type_id` is remembered for a deferred release so the pedal-up goes to that engine.
  bool note_off(std::uint32_t channel, std::uint32_t note, std::uint32_t engine_type_id) noexcept VELORA_NONBLOCKING;
  // Key bookkeeping for keys driven by contact_k (Story 2.5): contact 1 closing is a key press (key down, stale deferred release
  // and stolen flag cleared, exactly like note_on); the last contact opening is a key release that the sustain pedal does not
  // defer (key up, nothing deferred).
  void contact_down(std::uint32_t channel, std::uint32_t note) noexcept VELORA_NONBLOCKING { note_on(channel, note); }
  void contact_up(std::uint32_t channel, std::uint32_t note) noexcept VELORA_NONBLOCKING;
  void set_sustain(std::uint32_t channel, bool down) noexcept VELORA_NONBLOCKING;
  bool sustain_down(std::uint32_t channel) const noexcept { return pedal_[channel & 15u] != 0; }
  // After the pedal went up: true once per note whose release was deferred (the caller releases it).
  bool take_deferred_release(std::uint32_t channel, std::uint32_t note, std::uint32_t& engine_type_id) noexcept
      VELORA_NONBLOCKING;
  // Whether a note off of (channel, note) is deferred by the pedal, and for which engine, without consuming it (Story 4.3).
  bool deferred_release_type(std::uint32_t channel, std::uint32_t note, std::uint32_t& engine_type_id) const noexcept VELORA_NONBLOCKING {
    if (note > 127) return false;
    const std::size_t k = (channel & 15u) * 128u + note;
    if (!deferred_[k]) return false;
    engine_type_id = deferred_type_[k];
    return true;
  }
  // True once after the voices of (channel, note) were (partly) stolen and before its next note on:
  // a later note off finding nothing to release is then benign, not an error.
  bool take_stolen_flag(std::uint32_t channel, std::uint32_t note) noexcept VELORA_NONBLOCKING;

  // VoiceAllocator.
  std::uint32_t acquire(Engine& engine, std::uint32_t channel, std::uint32_t note) noexcept
      VELORA_NONBLOCKING override;

  // --- accounting ------------------------------------------------------------------------------
  struct Usage {
    std::uint64_t live_cost = 0;    // voices that are not being stolen
    std::uint64_t fading_cost = 0;  // stolen voices still fading: still counted
    std::uint64_t used() const noexcept { return live_cost + fading_cost; }
  };
  Usage usage() noexcept VELORA_NONBLOCKING;
  Usage usage(std::uint32_t engine_type_id) noexcept VELORA_NONBLOCKING;

  std::uint64_t steals() const noexcept { return steals_total_.load(std::memory_order_relaxed); }
  // Voices stolen FROM this engine.
  std::uint64_t steals(std::uint32_t engine_type_id) const noexcept;
  // Requests that got no slot (no victim, or the engine's own table is full of live/fading voices).
  std::uint64_t exhausted() const noexcept { return exhausted_.load(std::memory_order_relaxed); }
  StealRecord last_steal() const noexcept;
  std::uint32_t steal_ramp_frames() const noexcept { return ramp_; }

 private:
  struct Slot {
    VoiceInfo info{};
    std::uint64_t born = 0;  // note-on serial that took the slot
    bool active = false, fading = false, tentative = false;
    std::uint8_t prio = 0;  // 0 releasing, 1 held, 2 pedal-sustained, 3 lowest held
    std::uint64_t refused_req = 0;  // request that found the engine refusing begin_steal_fade
  };

  void reset_state() noexcept;
  void install_costs(const std::array<std::uint32_t, kMaxEngines>& cost, const std::array<std::uint32_t, kMaxEngines>& features) noexcept VELORA_NONBLOCKING;
  int index_of(const Engine& e) const noexcept;
  int index_of_type(std::uint32_t id) const noexcept;
  void scan() noexcept;
  std::uint64_t excess(std::size_t ei, bool ghosts) const noexcept;
  std::uint32_t take(std::size_t ei, std::uint32_t slot) noexcept;
  // One victim-selection attempt; `retry` is set when an engine refused a fade (try again).
  std::uint32_t acquire_reserved(std::size_t ei, bool& retry) noexcept;

  std::array<Engine*, kMaxEngines> engines_{};
  std::array<std::uint32_t, kMaxEngines> type_{}, cost_{}, capacity_{};
  std::array<std::uint32_t, kMaxEngines> base_cost_{}, features_{};  // Story 13.12: the static declaration at bind(); the cost features installed now
  std::array<std::vector<Slot>, kMaxEngines> slots_{};
  std::array<std::uint64_t, kMaxEngines> floor_cost_{};
  // A bound protected engine needs at least this many of its voices' cost in organ_rotary_cost (73 keys).
  static constexpr std::uint32_t kMinProtectedVoices = 73;
  std::array<bool, kMaxEngines> protected_{};  // Engine::steal_protected() at bind(): never a victim
  std::array<std::uint32_t, kMaxEngines> live_{}, fade_{};
  std::array<std::atomic<std::uint64_t>, kMaxEngines> steals_{};
  std::size_t count_ = 0;
  std::vector<std::uint32_t> victims_;  // scratch: (engine << 24 | slot) of tentative victims

  bool reserved_ = false;
  LiveSetConfig cfg_{};
  Reservation res_{};
  std::uint32_t ramp_ = kStealRampFrames;

  std::array<std::uint8_t, 16 * 128> key_down_{}, deferred_{}, stolen_flag_{};
  std::array<std::uint32_t, 16 * 128> deferred_type_{};
  std::array<std::uint8_t, 16> pedal_{};
  std::uint64_t serial_ = 1;  // note-on / note-off counter; a request never steals voices of its own event
  std::uint64_t req_ = 0;
  std::uint32_t min_sample_floor_ = kSampleFloorVoices;
  std::uint32_t min_fm_floor_ = kFmFloorVoices;
  std::uint32_t min_va_floor_ = kVaFloorVoices;
  std::uint32_t min_ep_floor_ = kEpFloorVoices;
  std::atomic<std::uint64_t> steals_total_{0}, exhausted_{0};
  std::atomic<std::uint64_t> last_steal_{0};  // packed, 0 = none
};

}  // namespace velora::core
