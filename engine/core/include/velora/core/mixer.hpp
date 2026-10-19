#pragma once
// Mixer (Story 2.7, AR22/AR14 phase 5): the fixed routing topology after the voices render.
//
//   engine voices --(per part)--> part bus (stereo, 64 frames)
//   part bus --> insert chain (<= 5 effects, cross-faded when the chain changes) --> post-insert signal
//   post-insert --> Reverb send bus  (x send_reverb_level)  --> Reverb return (placeholder, unity) --> Main
//   post-insert --> Delay send bus   (x send_delay_level)   --> Delay return  (placeholder, unity) --> Main
//   post-insert --> Main bus or Aux bus (part_output; switched with a short ramp)
//   line-in (x line_in_level) --> Main bus
//   Main, Aux --> master section (master EQ, compressor: placeholders until Story 2.8; master_level trim) --> outputs
//
// RT discipline: nothing here allocates, locks or blocks. Effect instances and chains are built off the RT thread by the control
// thread (fx.hpp, build_chain()) and arrive with the snapshot generation; a chain change creates a new "chain entry" that fades in
// linearly over kFadeFrames while the old one fades out (the weights sum to 1: a signal that passes both chains unchanged is
// continuous, no dip and no bump). A change that arrives mid-fade waits (the latest wins) and starts when the fade is over, so
// there are never more than two chains per part in flight. Every live (or waiting) entry holds a ledger reference on the generation that owns its chain
// (SnapshotExchange::rt_ref), so the control thread cannot free the instances while RT still processes through them (AD-23).
//
// When the routing is trivial (all parts to Main at unity, no effect in any chain, sends at 0, line-in at 0, master at 0 dB)
// the Mixer is "inactive": the block loop renders the engines straight into the Main output exactly as before this story,
// so such renders are bit-identical to the pre-2.7 goldens.
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

#include "velora/core/fx.hpp"
#include "velora/core/snapshot.hpp"
#include "velora/core/voice_arbiter.hpp"
#include "velora/ports/line_in_source.hpp"

namespace velora::core {

// The final output of one block: Main always, Aux when the host plays it (nullptr = the Aux bus is discarded). Each buffer holds
// kBlockFrames floats and is added to (the block loop zeroes them first).
struct OutputBuses {
  float* main_left = nullptr;
  float* main_right = nullptr;
  float* aux_left = nullptr;
  float* aux_right = nullptr;
};

class Mixer {
 public:
  static constexpr std::uint32_t kParts = params::kPartCount;
  static constexpr std::uint32_t kFrames = kFxBlockFrames;
  static constexpr std::uint32_t kMaxChains = 2;          // chain entries per part: the one fading out and the one fading in
  static constexpr std::uint32_t kFadeFrames = 240;       // chain cross-fade and Main/Aux switch: 5 ms at 48 kHz
  static constexpr std::uint32_t kLevelRampFrames = 64;   // send, line-in and master level changes: one block

  explicit Mixer(SnapshotExchange& exchange);
  Mixer(const Mixer&) = delete;
  Mixer& operator=(const Mixer&) = delete;

  // Setup time: the line-in source (not owned; nullptr = silent). Must outlive the mixer's use.
  void set_line_in(ports::LineInSource* source) noexcept { line_in_.store(source, std::memory_order_release); }

  // --- RT, serial phase ---------------------------------------------------------------------------------
  // A new generation was adopted: part outputs, master level and chains. Where a part's chain changed, a new chain entry is
  // started (cross-fade); the very first generation applies instantly.
  void adopt(const Snapshot& s) noexcept VELORA_NONBLOCKING;
  // Once per block, before the voices render: updates level targets from the live values. True = routed block (render into the
  // part buses, then finish()); false = trivial routing, render straight into Main and do not call finish().
  bool begin_block(const LastValueTable& live) noexcept VELORA_NONBLOCKING;
  // Channel c of an engine's render_split() output (c = 0..15; channel c and c + 8 share a part bus), at frame `offset` of the block.
  void part_sinks(std::uint32_t offset, float** left, float** right) noexcept VELORA_NONBLOCKING;
  // After the voices rendered a routed block: chains, sends, Main/Aux, line-in, master; adds the result to `out`.
  void finish(const OutputBuses& out) noexcept VELORA_NONBLOCKING;

  // --- introspection (RT thread or after rendering) -------------------------------------------------------
  bool routed() const noexcept { return routed_; }
  std::uint32_t chain_entries(std::uint32_t part) const noexcept { return part < kParts ? parts_[part].count : 0; }
  // True when every part has settled on a single chain entry (no cross-fade running or waiting).
  bool chains_settled() const noexcept {
    for (const Part& p : parts_)
      if (p.count != 1 || p.has_pending || !p.entries[0].w.steady(1.0f)) return false;
    return true;
  }
  // The chain a part is currently fading toward (nullptr = none).
  const FxChain* current_chain(std::uint32_t part) const noexcept {
    return part < kParts ? parts_[part].entries[parts_[part].count - 1].chain : nullptr;
  }

 private:
  // A linear gain ramp: cur moves to tgt in `left` steps. Steady = no ramp running.
  struct Ramp {
    float cur = 0.0f, tgt = 0.0f, step = 0.0f;
    std::uint32_t left = 0;
    bool steady(float v) const noexcept { return left == 0 && cur == v; }
    void retarget(float t, std::uint32_t frames) noexcept {
      if (t == tgt) return;
      tgt = t;
      if (frames == 0) {
        cur = t;
        left = 0;
        return;
      }
      step = (t - cur) / static_cast<float>(frames);
      left = frames;
    }
    // Starts a fresh ramp from the current value even when the target is unchanged (a fade that is already running restarts).
    void restart(float t, std::uint32_t frames) noexcept {
      if (left == 0 && cur == t) {
        tgt = t;
        return;
      }
      tgt = t;
      step = (t - cur) / static_cast<float>(frames);
      left = frames;
    }
    float next() noexcept {
      if (left > 0) {
        cur += step;
        if (--left == 0) cur = tgt;
      }
      return cur;
    }
  };
  struct Entry {
    FxChain* chain = nullptr;  // nullptr = no effects
    std::uint32_t ledger = 0;  // generation that owns `chain` (a reference is held while the entry lives)
    Ramp w;
  };
  struct Part {
    std::array<Entry, kMaxChains> entries{};
    std::uint32_t count = 1;
    // A chain that arrived while a cross-fade was running (the latest request wins; the ones in between are never heard). It holds
    // a generation reference like an entry, and starts fading in when the running fade is over.
    FxChain* pending = nullptr;
    std::uint32_t pending_ledger = 0;
    bool has_pending = false;
    Ramp main_g, aux_g, rev_g, dly_g;
    float main_target = 1.0f, aux_target = 0.0f;
  };

  void push_chain(Part& p, FxChain* chain, std::uint32_t ledger, bool instant, bool ref_held = false) noexcept VELORA_NONBLOCKING;
  void clear_pending(Part& p) noexcept VELORA_NONBLOCKING;
  void drop_entry(Part& p, std::uint32_t index) noexcept VELORA_NONBLOCKING;

  SnapshotExchange& exchange_;
  std::atomic<ports::LineInSource*> line_in_{nullptr};
  std::array<Part, kParts> parts_{};
  Ramp master_g_, line_g_;
  float master_target_ = 1.0f;
  bool primed_ = false;  // a generation has been adopted: until then (and for the adopting block) levels apply without a ramp
  bool routed_ = false;
  std::int16_t master_level_index_ = -1;
  bool instant_levels_ = true;  // level targets apply without a ramp (before and in the block of the first adopted generation)
  std::int16_t send_rev_perf_ = -1, send_dly_perf_ = -1, line_slot_ = -1;  // -1: not in the registry (the level stays 0)

  // Placeholders of the fixed topology (DSP in Stories 2.8, 2.10, 2.11), owned by the mixer, built at construction.
  std::array<std::unique_ptr<Effect>, kOutputBusCount> reverb_send_, delay_send_;
  std::array<std::unique_ptr<Effect>, kOutputBusCount> master_eq_, master_comp_;

  alignas(16) std::array<float, kParts * kFrames> bus_l_{}, bus_r_{};
};

// Fills the FX reservation of an arbiter configuration from the effect instances a snapshot holds (insert chains) and the fixed
// topology (two sends, master EQ and compressor per output bus): LiveSetConfig::fx_instance_costs gets one entry per instance
// and fx_instance_count is set. False (config untouched) if they do not fit. Together with ControlPlane's FX budget this is the
// "FX first" step of the reservation order FX -> organ+rotary -> floors.
bool fill_fx_costs(const Snapshot& s, VoiceArbiter::LiveSetConfig& cfg) noexcept;

}  // namespace velora::core
