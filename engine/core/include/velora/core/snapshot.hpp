#pragma once
// Immutable structural snapshots (Story 2.1, AD-23) and the lock-free pieces that carry them to the RT
// threads. The control thread (engine/host) is the ONLY writer: it builds a complete Snapshot, then
// publishes it with one atomic pointer store. RT threads only ever read a published Snapshot, never a
// structure the control thread is mutating.
//
// Lifetime (epoch/hazard style, no locks):
//   * RT announces the generation it is reading ("held") before it dereferences it (hazard pointer).
//   * RT counts, per generation, the voices (notes) that still reference it (the ledger).
//   * The control thread frees a generation only when it is not current, not held by RT, and its ledger
//     count is zero. Freeing happens on the control thread, never on RT.
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "velora/core/arpeggiator.hpp"
#include "velora/core/assign_events.hpp"
#include "velora/core/diagnostics.hpp"
#include "velora/core/fx.hpp"
#include "velora/core/params_gen.hpp"
#include "velora/core/rt.hpp"
#include "velora/core/sample_bank.hpp"
#include "velora/core/voice_arbiter.hpp"

namespace velora::core {

struct InsertSlotCfg {
  std::uint8_t type = 0;    // registry enum insert_type (0 = none)
  std::uint8_t bypass = 0;
  std::array<float, params::kInsertParamCount> params = kDefaultInsertParams;
};

// Why a Live Set part sounds or not (Story 4.3). Only kReady parts receive notes; every other state is silence, never a substitute.
enum class PartState : std::uint8_t {
  kReady = 0,           // the part sounds
  kEmptyTimbre,         // a sample part with no timbre reference
  kMissingLibrary,      // the timbre's library or instrument could not be resolved
  kEngineNotAvailable,  // reserved engine type (FM, VA) or unknown type with no compiled engine
};

struct PartCfg {
  std::uint32_t timbre_ref = 0;  // opaque id (resolved by the librarian in a later story)
  // --- Live Set part model (Story 4.3). Used by the block loop only when Snapshot::live_set is true; the defaults make a part
  // that is a plain channel (the pre-4.3 behaviour). ---
  std::uint8_t low_key = 0, high_key = 127;  // zone, tested against the key AS PLAYED (before octave / transpose)
  std::int8_t octave = 0;                    // -4..4, applied to the pitch only
  std::int8_t transpose = 0;                 // -12..12 semitones, applied to the pitch only
  std::uint8_t keyboard_on = 1;              // 0 = the part receives no new notes (what it already sounds keeps releasing)
  PartState state = PartState::kReady;
  std::uint32_t engine_type = 0;             // core::EngineTypeId value of the part's engine (0 = none)
  float volume = 1.0f;                       // 0..1 part fader, linear; scales the part's Main/Aux feed (post insert chain, sends follow it)
  // The part's instrument for the sample engine (nullptr: not a sample part, or silent). Shared by every generation that
  // carries it: the pointer is the identity (RT rebinds only when it changes); the bank dies with the last generation that holds it.
  std::shared_ptr<SampleBank> sample_bank;
  std::uint8_t output = 0;       // registry enum part_output (main/aux)
  std::uint8_t insert_count = 0;
  std::array<InsertSlotCfg, params::kInsertSlotCount> inserts{};
  // The part's effect instances (Story 2.7), pre-allocated by the control thread (build_chain()) and shared by the generations
  // that carry the same chain: the pointer is the chain's identity, so a generation that does not touch this part's inserts
  // keeps the very same chain and RT does not cross-fade. nullptr = no inserts. RT reads the raw pointer only (it never copies or
  // releases the shared_ptr); the instances die with the last generation that holds them, on the control thread.
  std::shared_ptr<FxChain> chain;
  // Per-part arpeggiator configuration (Story 7.2). Default enabled = false (notes pass through unchanged).
  ArpConfig arp{};
};

struct Snapshot {
  static constexpr std::uint32_t kMagic = 0x56534E50u;  // 'VSNP'; the control thread clears it before freeing
  std::uint32_t magic = kMagic;
  std::uint32_t ledger_index = 0;  // assigned by the control thread, < SnapshotExchange::kLedgerSlots
  std::uint64_t revision = 0;      // strictly increasing, 1 = first generation
  std::array<PartCfg, params::kPartCount> parts{};
  // True for a generation built from a Live Set (Story 4.3): the block loop then fans keyboard notes out to the parts by zone
  // (PartRouter) and maps each part to the engine and instrument of its PartCfg. False = the plain channel==part addressing.
  bool live_set = false;
  std::array<float, params::kMasterParams> master{};  // structural master-scope parameters, by master_index
  // Initial performance values (send levels, drawbars, ...), indexed by last-value slot. RT applies a slot
  // only when it differs from the previous generation's, i.e. on generation change, never otherwise.
  std::array<float, params::kPerfSlotCount> perf_init{};
  // Scene recall (Story 4.4, AD-2 program_generation). A scene recall is a generation with a NEW `program_generation` (a number the
  // control plane never reuses). When RT adopts a generation whose program_generation differs from the one it last applied, it ALSO
  // applies, once, every slot flagged in `perf_force` even if its initial value equals the previous generation's: that is how a
  // recall overrides a live knob move for the parameters the scene stores (and only those). Any later generation copied from this
  // one carries the same program_generation, so the forced slots are applied once and never again. 0 / all clear = no program change
  // (every generation that is not a scene recall).
  std::uint64_t program_generation = 0;
  std::array<std::uint8_t, params::kPerfSlotCount> perf_force{};
  // The Live Set scene this generation was recalled from (-1 = none: a freshly loaded Live Set, or no Live Set) and how many scenes the
  // loaded Live Set has. Published in the engine state. `scene_source` identifies the load that owns them (see host SceneController).
  std::int32_t scene_index = -1;
  std::uint32_t scene_count = 0;
  std::uint64_t scene_source = 0;

  // Seamless Live Set switch (Story 4.5, AD-22). The control thread prepares the voice-pool reservation of the Live Set this generation
  // was built from (VoiceArbiter::prepare_reservation: validation and costs, may allocate) and carries it here. RT applies it in the
  // serial phase when it adopts a generation whose `reservation_epoch` it has not applied yet (VoiceArbiter::commit_prepared: a bounded
  // copy, no allocation), so the swap and the new floors / budget take effect at the same block. 0 = this generation carries none (every
  // generation but a seamless switch); later generations copied from a switch generation carry the same epoch and apply nothing again.
  std::uint64_t reservation_epoch = 0;
  VoiceArbiter::PreparedReservation reservation{};

  // The Live Set's ASSIGN map (Story 4.10): K1..K8 -> a registry performance parameter id, 0 = empty. Set by the loader from the Live Set, edited by
  // the control thread's ASSIGN commands (a new generation that is a copy of the current one with this array changed), carried unchanged by every
  // other generation (a scene recall does not change it). RT reads it from the generation it holds when an encoder step arrives in ASSIGN mode.
  std::array<std::uint16_t, kAssignSlotCount> assign{};

  // Every field at its registry default.
  static Snapshot defaults() noexcept;
};

// Control thread: builds the part's effect chain from its inserts[0..insert_count) (new, prepared instances, one per non-none
// slot; nullptr when the part has no slot at all).
std::shared_ptr<FxChain> build_chain(const PartCfg& part);
// Control thread: build_chain() for every part (setup code and tests that fill a Snapshot by hand).
void build_chains(Snapshot& s);
// Declared cost of every effect instance the snapshot holds plus the fixed topology (sends, master), in arbiter cost units.
// Computed from the chain instances; a part with inserts configured but no chain built counts the cost of its configured types.
std::uint64_t fx_cost_units(const Snapshot& s) noexcept;

// RT-owned live values of the performance parameters. RT is the single writer (it applies the
// parameter UMPs); any thread may read without locks (one 32-bit atomic per slot).
class LastValueTable {
 public:
  LastValueTable() noexcept;
  void store(std::size_t slot, float v) noexcept VELORA_NONBLOCKING {
    v_[slot].store(std::bit_cast<std::uint32_t>(v), std::memory_order_release);
  }
  float load(std::size_t slot) const noexcept VELORA_NONBLOCKING {
    return std::bit_cast<float>(v_[slot].load(std::memory_order_acquire));
  }

  // Compressor gain reduction in dB (>= 0) per compressor instance (Story 2.8), published by RT (the Mixer, once per block) and read
  // lock-free by the control thread (ws.v1 GetPerformanceValues). Not a parameter: read-only engine state. Slots: one per part insert
  // slot (part * kInsertSlotCount + slot), then the master compressor of Main and Aux. 0 when the slot holds no compressor or it is bypassed.
  static constexpr std::size_t kGainReductionSlots = params::kPartCount * params::kInsertSlotCount + kOutputBusCount;
  static constexpr std::size_t gr_insert_slot(std::uint32_t part, std::uint32_t slot) noexcept {
    return static_cast<std::size_t>(part) * params::kInsertSlotCount + slot;
  }
  static constexpr std::size_t gr_master_slot(std::uint32_t bus) noexcept {
    return static_cast<std::size_t>(params::kPartCount) * params::kInsertSlotCount + bus;
  }
  void store_gain_reduction(std::size_t slot, float db) noexcept VELORA_NONBLOCKING {
    gr_[slot].store(std::bit_cast<std::uint32_t>(db), std::memory_order_release);
  }
  float load_gain_reduction(std::size_t slot) const noexcept VELORA_NONBLOCKING {
    return std::bit_cast<float>(gr_[slot].load(std::memory_order_acquire));
  }

  // Story 4.9, control->RT fence (AR12). The control thread raises a fence by sending a fence event (make_fence_event(seq), param_events.hpp)
  // down the control lane, behind every parameter event it sent before. RT applies the events in order, so when it applies the fence it has
  // applied everything sent earlier, and it acknowledges with ONE release store of the sequence number here. The control thread waits for
  // the acknowledgement (bounded, on its own thread) and only then reads the table, so a Write never sees a value that was still in flight.
  void store_fence_ack(std::uint32_t seq) noexcept VELORA_NONBLOCKING { fence_ack_.store(seq, std::memory_order_release); }
  std::uint32_t load_fence_ack() const noexcept { return fence_ack_.load(std::memory_order_acquire); }
  // Story 4.10: the encoder mode flag, the selected part and the encoder counters (assign_events.hpp). RT writes, anyone reads.
  EncoderState& encoder_rt() noexcept { return encoder_; }
  const EncoderState& encoder() const noexcept { return encoder_; }
  // The revision of the newest generation whose initial values RT has FULLY applied to this table (stored after the last slot of the
  // adoption, once per generation change). Until it equals the published revision a value that differs from the generation's initial
  // value may be a recall or a switch RT has not applied yet, not a change the player made.
  void store_adopted_revision(std::uint64_t rev) noexcept VELORA_NONBLOCKING { adopted_revision_.store(rev, std::memory_order_release); }
  std::uint64_t load_adopted_revision() const noexcept { return adopted_revision_.load(std::memory_order_acquire); }
  // Story 6.5: the engines' content_integrity count (voices that faded to silence for a damaged block of a protected library), summed over the engines and
  // stored by RT once per block; read lock-free by the control thread (State.content). Monotone.
  void store_content_integrity(std::uint64_t n) noexcept VELORA_NONBLOCKING { content_integrity_.store(n, std::memory_order_release); }
  std::uint64_t load_content_integrity() const noexcept { return content_integrity_.load(std::memory_order_acquire); }

  // Story 1.21: in-RAM diagnostics counters and histograms (RT / host render path writes, control thread reads).
  Diagnostics& diagnostics_rt() noexcept { return diag_; }
  const Diagnostics& diagnostics() const noexcept { return diag_; }

  // Story 7.6 / 7.8: published external clock lost state.
  void store_clock_lost(bool lost) noexcept VELORA_NONBLOCKING { clock_lost_.store(lost, std::memory_order_release); }
  bool load_clock_lost() const noexcept { return clock_lost_.load(std::memory_order_acquire); }

 private:
  std::array<std::atomic<std::uint32_t>, params::kPerfSlotCount> v_;
  std::array<std::atomic<std::uint32_t>, kGainReductionSlots> gr_;
  std::atomic<std::uint32_t> fence_ack_{0};
  std::atomic<std::uint64_t> adopted_revision_{0};
  std::atomic<std::uint64_t> content_integrity_{0};
  std::atomic<bool> clock_lost_{false};
  EncoderState encoder_;
  Diagnostics diag_;
};

// Publication point between the control thread and the RT side.
class SnapshotExchange {
 public:
  static constexpr std::uint32_t kLedgerSlots = 32;

  // --- control thread only ---------------------------------------------------------------------
  // The snapshot must be fully built and never modified afterwards.
  void publish(const Snapshot* s) noexcept { current_.store(s, std::memory_order_seq_cst); }
  const Snapshot* current() const noexcept { return current_.load(std::memory_order_seq_cst); }
  const Snapshot* rt_held() const noexcept { return held_.load(std::memory_order_seq_cst); }
  std::uint32_t refs(std::uint32_t ledger_index) const noexcept {
    return refs_[ledger_index % kLedgerSlots].load(std::memory_order_acquire);
  }
  // True when no thread can still read `s`: not current, not held by RT, no voice references it.
  bool safe_to_free(const Snapshot* s) const noexcept {
    return s != current() && s != rt_held() && refs(s->ledger_index) == 0;
  }

  // --- RT thread only --------------------------------------------------------------------------
  // Announce-then-validate: returns the current generation with the hazard set, lock-free.
  const Snapshot* rt_acquire() noexcept VELORA_NONBLOCKING {
    const Snapshot* p = current_.load(std::memory_order_seq_cst);
    for (;;) {
      held_.store(p, std::memory_order_seq_cst);
      const Snapshot* again = current_.load(std::memory_order_seq_cst);
      if (again == p) return p;
      p = again;
    }
  }
  void rt_ref(std::uint32_t ledger_index) noexcept VELORA_NONBLOCKING {
    refs_[ledger_index % kLedgerSlots].fetch_add(1, std::memory_order_seq_cst);
  }
  void rt_unref(std::uint32_t ledger_index) noexcept VELORA_NONBLOCKING {
    refs_[ledger_index % kLedgerSlots].fetch_sub(1, std::memory_order_seq_cst);
  }

 private:
  std::atomic<const Snapshot*> current_{nullptr};
  std::atomic<const Snapshot*> held_{nullptr};
  std::array<std::atomic<std::uint32_t>, kLedgerSlots> refs_{};
};

}  // namespace velora::core
