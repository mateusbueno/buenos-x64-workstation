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

#include "velora/core/params_gen.hpp"
#include "velora/core/rt.hpp"

namespace velora::core {

struct InsertSlotCfg {
  std::uint8_t type = 0;    // registry enum insert_type (0 = none)
  std::uint8_t bypass = 0;
  std::array<float, params::kInsertParamCount> params{0.5f, 0.5f, 0.5f, 0.5f};
};

struct PartCfg {
  std::uint32_t timbre_ref = 0;  // opaque id (resolved by the librarian in a later story)
  std::uint8_t output = 0;       // registry enum part_output (main/aux)
  std::uint8_t insert_count = 0;
  std::array<InsertSlotCfg, params::kInsertSlotCount> inserts{};
};

struct Snapshot {
  static constexpr std::uint32_t kMagic = 0x56534E50u;  // 'VSNP'; the control thread clears it before freeing
  std::uint32_t magic = kMagic;
  std::uint32_t ledger_index = 0;  // assigned by the control thread, < SnapshotExchange::kLedgerSlots
  std::uint64_t revision = 0;      // strictly increasing, 1 = first generation
  std::array<PartCfg, params::kPartCount> parts{};
  std::array<float, params::kMasterParams> master{};  // structural master-scope parameters, by master_index
  // Initial performance values (send levels, drawbars, ...), indexed by last-value slot. RT applies a slot
  // only when it differs from the previous generation's, i.e. on generation change, never otherwise.
  std::array<float, params::kPerfSlotCount> perf_init{};

  // Every field at its registry default.
  static Snapshot defaults() noexcept;
};

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

 private:
  std::array<std::atomic<std::uint32_t>, params::kPerfSlotCount> v_;
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
