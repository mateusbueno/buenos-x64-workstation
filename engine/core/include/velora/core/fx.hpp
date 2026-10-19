#pragma once
// Effect framework (Story 2.7): the effect-instance interface, the factory, the insert chain and the fixed costs of the
// routing topology. Every insert type is a real effect since Story 2.9: EQ and compressor (Story 2.8, eq.hpp, compressor.hpp), chorus/phaser,
// EP amp/drive and rotary (Story 2.9, chorus_phaser.hpp, ep_amp.hpp, rotary_effect.hpp). The sends are real DSP: the reverb since Story 2.10 (reverb.hpp) and the
// delay since Story 2.11 (delay.hpp); see engine/core/README.md, "Routing topology". PassThroughEffect only remains as the factory's fallback.
//
// Threading: effects are created, prepared and destroyed ONLY by the control thread (make_effect(), FxChain construction and
// destruction, i.e. snapshot generations being built and freed). The RT thread only calls process() on instances that a
// published snapshot generation holds, and the AD-23 lifetime rules guarantee the control thread frees a generation only
// after RT stopped using it (including the chain cross-fade of the Mixer).
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "velora/core/params_gen.hpp"
#include "velora/core/rt.hpp"

namespace velora::core {

constexpr std::uint32_t kFxBlockFrames = 64;  // = kBlockFrames (checked in block_renderer.cpp)

// The registry defaults of insert_param_1..5 (0.5 for the first four, 0 for insert_param_5), checked by params_test.
inline constexpr std::array<float, params::kInsertParamCount> kDefaultInsertParams = {0.5f, 0.5f, 0.5f, 0.5f, 0.0f};

// A stereo block processed in place. `frames` <= kFxBlockFrames.
struct StereoBlock {
  float* left = nullptr;
  float* right = nullptr;
  std::uint32_t frames = 0;
};

// The effect types of an insert slot. The numeric values are the registry enum insert_type (params.json) and are
// checked against the generated constants below. Only these types exist: effects are compiled in, there is no plugin loading.
enum class EffectType : std::uint8_t {
  kNone = 0,
  kRotary = 1,
  kChorusPhaser = 2,
  kEpAmp = 3,
  kEq = 4,
  kCompressor = 5,
};
static_assert(static_cast<std::uint8_t>(EffectType::kNone) == params::kInsertTypeNone &&
              static_cast<std::uint8_t>(EffectType::kRotary) == params::kInsertTypeRotary &&
              static_cast<std::uint8_t>(EffectType::kChorusPhaser) == params::kInsertTypeChorusPhaser &&
              static_cast<std::uint8_t>(EffectType::kEpAmp) == params::kInsertTypeEpAmp &&
              static_cast<std::uint8_t>(EffectType::kEq) == params::kInsertTypeEq &&
              static_cast<std::uint8_t>(EffectType::kCompressor) == params::kInsertTypeCompressor &&
              params::kInsertTypeCount == 6);

// Declared cost of one instance in arbiter cost units (host-measured placeholders; the dev board measurement is Story 2.12). The FX budget
// is computed from these. A bypassed instance still reserves its cost: bypass is a worst-case-preserving toggle.
constexpr std::uint32_t effect_cost_units(EffectType t) noexcept {
  switch (t) {
    case EffectType::kRotary: return 24;  // = RotarySpeaker::kCostUnits (Story 2.6); the insert (Story 2.9) runs the same DSP plus a mono sum and a width stage
    case EffectType::kChorusPhaser: return 12;  // Story 2.9, host-measured (Release build: ~0.7 us / block in either mode), placeholder until 2.12
    case EffectType::kEpAmp: return 24;         // Story 2.9, host-measured (Release build: ~1.3 us / block, 4x oversampling included), placeholder until 2.12
    case EffectType::kEq: return 16;          // Story 2.8, host-measured (3 active bands, ~1.0 us / block on the dev Mac), placeholder until 2.12
    case EffectType::kCompressor: return 8;   // Story 2.8, host-measured (~0.5 us / block, signal above threshold), placeholder until 2.12
    case EffectType::kNone: break;
  }
  return 0;
}
constexpr std::uint32_t kMaxInsertCost = 24;  // the largest effect_cost_units()
static_assert(effect_cost_units(EffectType::kRotary) <= kMaxInsertCost && effect_cost_units(EffectType::kChorusPhaser) <= kMaxInsertCost &&
                  effect_cost_units(EffectType::kEpAmp) <= kMaxInsertCost && effect_cost_units(EffectType::kEq) <= kMaxInsertCost &&
                  effect_cost_units(EffectType::kCompressor) <= kMaxInsertCost,
              "kMaxInsertCost is the largest declared insert cost");

// The fixed part of the topology: per output bus (Main, Aux) a Reverb send, a Delay send (their returns go to the bus of the sending
// part, so the real DSP of Stories 2.10/2.11 needs one instance per bus: costed twice from the start) and a master EQ and compressor.
// Reverb (Story 2.10), host-measured with engine/core/tests/reverb_bench.cpp (Release build, longest time, 64-frame blocks): one instance
// costs about 1.1x an EQ insert (~0.75 us), 2.2x while a type cross-fade runs two engines (~1.5 us); the declared figure covers the
// cross-fade case ratio-wise (EQ = 16 units -> ~36) with a margin. Placeholder until the dev board measurement of Story 2.12.
constexpr std::uint32_t kReverbSendCost = 64;  // was 48: the bench now runs on -O3 code in every configuration, where the cross-fade case measures 2.2..3.5 x an EQ insert across runs
// Delay (Story 2.11), host-measured with engine/core/tests/delay_bench.cpp (Release build, 64-frame blocks, loud input, damping and ping-pong on, the time gliding all the
// time): one instance costs about 0.9x an EQ insert (~0.55 us; steady ~0.4 us). The declared 40 units cover the ratio (EQ = 16 units -> 14..22 on the Mac, up to ~36 on x86 gcc in Docker where the gliding path is relatively dearer) with a margin (the first block after an idle spell costs like any other: no bulk clear). Placeholder until the dev board measurement of Story 2.12 (it was a 16-unit placeholder; raising it
// raises kFixedFxCost by 48).
constexpr std::uint32_t kDelaySendCost = 40;
// Note (Story 2.8): re-basing the EQ from 6 to 16 units raises the fixed topology by 20 units (two buses) and every EQ insert by 10, so a
// Live Set accepted against an older, tighter FX budget can now exceed it and be refused with FX_BUDGET_EXCEEDED; budgets are derived from these
// constants (kMaxFxCost) and the dev-board numbers of Story 2.12 will replace the placeholders anyway.
constexpr std::uint32_t kMasterEqCost = 16;   // per bus (= effect_cost_units(kEq))
constexpr std::uint32_t kMasterCompCost = 8;  // per bus (= effect_cost_units(kCompressor))
constexpr std::uint32_t kOutputBusCount = 2;
static_assert(kMasterEqCost == effect_cost_units(EffectType::kEq) && kMasterCompCost == effect_cost_units(EffectType::kCompressor),
              "the master section costs the same as the insert effects it reuses");
constexpr std::uint32_t kFixedFxCost =
    kOutputBusCount * (kReverbSendCost + kDelaySendCost + kMasterEqCost + kMasterCompCost);
// Every slot of every part filled with the heaviest insert, plus the fixed topology: a budget at which no command is ever refused.
constexpr std::uint64_t kMaxFxCost =
    static_cast<std::uint64_t>(params::kPartCount) * params::kInsertSlotCount * kMaxInsertCost + kFixedFxCost;

// One effect instance. process() runs on the RT thread and must not allocate, lock or block.
class Effect {
 public:
  Effect() noexcept { live_.fetch_add(1, std::memory_order_relaxed); }
  virtual ~Effect() { live_.fetch_sub(1, std::memory_order_relaxed); }
  Effect(const Effect&) = delete;
  Effect& operator=(const Effect&) = delete;

  virtual EffectType type() const noexcept = 0;
  virtual std::uint32_t cost_units() const noexcept = 0;
  // Control thread, before the instance is handed over: allocate whatever the DSP needs for this rate / block size.
  virtual void prepare(std::uint32_t sample_rate, std::uint32_t max_block_frames) noexcept {
    (void)sample_rate;
    (void)max_block_frames;
  }
  virtual void reset() noexcept VELORA_NONBLOCKING {}
  // The normalised insert parameters (registry insert_param_1..5). Control thread, before the hand-over.
  virtual void set_params(const std::array<float, params::kInsertParamCount>& p) noexcept { (void)p; }
  virtual void process(StereoBlock& block) noexcept VELORA_NONBLOCKING = 0;
  // Gain reduction in dB (>= 0) the instance applied in its last block; 0 for effects that do not reduce gain (Story 2.8). Any thread.
  virtual float gain_reduction_db() const noexcept VELORA_NONBLOCKING { return 0.0f; }

  // Effect instances alive in the process (tests: generation lifetime).
  static std::int64_t live_instances() noexcept { return live_.load(std::memory_order_relaxed); }

 private:
  static inline std::atomic<std::int64_t> live_{0};
};

// The placeholder every effect type was until its DSP landed: leaves the block untouched (now only the factory's fallback).
class PassThroughEffect final : public Effect {
 public:
  PassThroughEffect(EffectType t, std::uint32_t cost) noexcept : type_(t), cost_(cost) {}
  EffectType type() const noexcept override { return type_; }
  std::uint32_t cost_units() const noexcept override { return cost_; }
  void set_params(const std::array<float, params::kInsertParamCount>& p) noexcept override { params_ = p; }
  void process(StereoBlock&) noexcept VELORA_NONBLOCKING override {}
  const std::array<float, params::kInsertParamCount>& params() const noexcept { return params_; }

 private:
  EffectType type_;
  std::uint32_t cost_;
  std::array<float, params::kInsertParamCount> params_ = kDefaultInsertParams;
};

// Factory: control thread only (it allocates). nullptr for kNone and for a value outside the enum. The instance is prepared.
// All six types are real DSP: EQ and compressor (Story 2.8), chorus/phaser, EP amp and rotary (Story 2.9: one RotarySpeaker per rotary insert slot; the organ's
// built-in rotary stage of Story 2.6 stays as it is and is never duplicated automatically).
std::unique_ptr<Effect> make_effect(EffectType type);

// An insert chain: up to params::kInsertSlotCount effects in order, each with a bypass flag. The topology (which effect in
// which slot, bypass) is fixed at construction; a command that changes it builds a NEW chain (new instances, pre-allocated by the
// control thread) and the Mixer cross-fades from the old to the new one. The instances' internal DSP state is mutated by RT.
class FxChain {
 public:
  static constexpr std::uint32_t kMaxSlots = params::kInsertSlotCount;

  struct Slot {
    std::unique_ptr<Effect> effect;  // nullptr: empty slot (type none)
    bool bypass = false;
    bool reset_done = false;  // RT: a bypassed slot's instance was reset (so stale state cannot step in when it is un-bypassed)
  };

  FxChain() = default;
  FxChain(const FxChain&) = delete;
  FxChain& operator=(const FxChain&) = delete;

  // Control thread, while building. False when the slot index is out of range.
  bool set_slot(std::uint32_t index, std::unique_ptr<Effect> effect, bool bypass) {
    if (index >= kMaxSlots) return false;
    slots_[index].effect = std::move(effect);
    slots_[index].bypass = bypass;
    if (index >= count_) count_ = index + 1;
    return true;
  }

  std::uint32_t slot_count() const noexcept { return count_; }
  const Slot& slot(std::uint32_t i) const noexcept { return slots_[i]; }
  // Declared cost of every instance in the chain, bypassed ones included.
  std::uint32_t cost_units() const noexcept {
    std::uint32_t c = 0;
    for (std::uint32_t i = 0; i < count_; ++i)
      if (slots_[i].effect) c += slots_[i].effect->cost_units();
    return c;
  }
  // True when no slot would process (no effect, or every effect bypassed): the chain leaves a block untouched.
  bool is_empty() const noexcept {
    for (std::uint32_t i = 0; i < count_; ++i)
      if (slots_[i].effect && !slots_[i].bypass) return false;
    return true;
  }

  // RT: the non-bypassed effects, in order, in place.
  void process(StereoBlock& block) noexcept VELORA_NONBLOCKING {
    for (std::uint32_t i = 0; i < count_; ++i) {
      Slot& sl = slots_[i];
      if (!sl.effect) continue;
      if (!sl.bypass) {
        sl.reset_done = false;
        sl.effect->process(block);
      } else if (!sl.reset_done) {
        sl.effect->reset();
        sl.reset_done = true;
      }
    }
  }

 private:
  std::array<Slot, kMaxSlots> slots_{};
  std::uint32_t count_ = 0;
};

}  // namespace velora::core
