#pragma once
// ASSIGN-mode encoders on the RT path (Story 4.10, FR101 partial, FR102, FR103). The panel MCU delivers RELATIVE signed steps per encoder (Story 3.10
// produces them); this header defines how "encoder n turned by d steps" enters the engine, the encoder mode flag, the selected part, and the one
// function that turns steps into a new parameter value.
//
// RESERVED CONTROL EVENTS (the one place). MIDI 2.0 assignable controller (message type 4, status 0x3), group 0, CHANNEL 15 -- parts are 0..7, so a
// channel-15 event can never be a part's parameter -- with a reserved 14-bit id in bank/index like a parameter event (param_events.hpp). The registry
// (proto/params.json) cannot use these ids: the generator refuses ids >= 0x3FE0.
//
//   id            event                  word1
//   0x3FFF        control->RT fence      sequence number                     (Story 4.9, param_events.hpp)
//   0x3FE0        PanelControl           (control id << 16) | value16: a raw panel CONTROL frame (fader, VALUE encoder, pedal), Story 3.10; binding is Epic 4
//   0x3FF0..0x3FF7 EncoderStep(n)        signed delta, int32 two's complement, within int16 (encoder n = 0..7 = K1..K8)
//   0x3FF8        SetEncoderMode         0 = PART, 1 = ALL, 2 = ASSIGN
//   0x3FF9        SelectPart             part 0..7
//
// Any other id on channel 15 is refused and counted like an unknown parameter event (BlockRenderer::param_events_rejected()).
//
// ASSIGN ROUTING (BlockRenderer). A part-scope step whose target part has no engine still moves the live value but is counted in steps_no_part, not steps_applied. Only in ASSIGN mode an encoder step is routed: slot n of the CURRENT generation's ASSIGN map (Snapshot::assign,
// carried with the generation, so a Live Set switch changes it and a scene recall does not) names a performance parameter; the step is applied to that
// parameter's LIVE value (the RT last-value table) -- no pickup -- by apply_relative_step() and then goes through the same path as a CC (last-value table
// store + the engine's set_part_param, so smoothing and ramps apply). A part-scope parameter lands on the SELECTED part (RT value, default 0; Stories
// 4.13 / 4.22 will drive it), a master-scope one on part 0. An empty slot does nothing (counted). A parameter whose engine or part does not exist still
// updates the live value; the engine ignores it. PART and ALL are Story 4.13: here they only count (steps_unrouted).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "velora/core/param_events.hpp"
#include "velora/core/params_gen.hpp"
#include "velora/core/rt.hpp"
#include "velora/core/ump.hpp"

namespace velora::core {

inline constexpr std::size_t kAssignSlotCount = 8;  // K1..K8
inline constexpr std::uint32_t kEncoderCount = 8;

inline constexpr std::uint32_t kReservedEventChannel = 0xF;  // == kFenceChannel
inline constexpr std::uint32_t kReservedEventFirstId = 0x3FE0;  // the registry refuses ids from here on
inline constexpr std::uint32_t kPanelControlEventId = 0x3FE0;
inline constexpr std::uint32_t kEncoderStepFirstId = 0x3FF0;    // ..0x3FF7
inline constexpr std::uint32_t kEncoderModeEventId = 0x3FF8;
inline constexpr std::uint32_t kSelectPartEventId = 0x3FF9;

enum class EncoderMode : std::uint8_t { kPart = 0, kAll = 1, kAssign = 2 };
inline constexpr std::uint32_t kEncoderModeCount = 3;

namespace detail {
constexpr Ump make_reserved_event(std::uint32_t id, std::uint32_t value) noexcept {
  return Ump{(Ump::kMessageTypeChannelVoice2 << 28) | (Ump::kStatusAssignableController << 20) | (kReservedEventChannel << 16) |
                 (((id >> 7) & 0x7Fu) << 8) | (id & 0x7Fu),
             value};
}
}  // namespace detail

// "Encoder `encoder` (0..7 = K1..K8) turned by `delta` steps" (clamped to the int16 range).
constexpr Ump make_encoder_step_event(std::uint32_t encoder, std::int32_t delta) noexcept {
  if (delta > 32767) delta = 32767;
  if (delta < -32768) delta = -32768;
  return detail::make_reserved_event(kEncoderStepFirstId + (encoder & 7u), static_cast<std::uint32_t>(delta));
}
// A raw panel control event (Story 3.10): MCU control id (1..255) and the 16-bit value as the frame carried it (an encoder delta in two's complement).
constexpr Ump make_panel_control_event(std::uint32_t control_id, std::uint32_t value16) noexcept {
  return detail::make_reserved_event(kPanelControlEventId, ((control_id & 0xFFu) << 16) | (value16 & 0xFFFFu));
}
constexpr std::uint32_t panel_control_id(const Ump& u) noexcept { return (u.word1 >> 16) & 0xFFu; }
constexpr std::uint32_t panel_control_value16(const Ump& u) noexcept { return u.word1 & 0xFFFFu; }
constexpr Ump make_encoder_mode_event(EncoderMode mode) noexcept {
  return detail::make_reserved_event(kEncoderModeEventId, static_cast<std::uint32_t>(mode));
}
constexpr Ump make_select_part_event(std::uint32_t part) noexcept { return detail::make_reserved_event(kSelectPartEventId, part); }

// The reserved id of a channel-15 assignable controller; false when `ump` is not one.
// Only ids from kReservedEventFirstId on are control events. A lower id on channel 15 is not diverted: it falls through to decode_param_event: a part-scope parameter is part 15's (the build carries 16 parts, Story 4.1), a master-scope one is refused (it
// needs channel 0) and counted in param_events_rejected().
constexpr bool decode_reserved_event_id(const Ump& ump, std::uint32_t& id) noexcept {
  if (ump.group() != 0 || !ump.is_assignable_controller() || ump.channel() != kReservedEventChannel) return false;
  const std::uint32_t v = (((ump.word0 >> 8) & 0x7Fu) << 7) | (ump.word0 & 0x7Fu);
  if (v < kReservedEventFirstId) return false;
  id = v;
  return true;
}

// THE assignability rule, shared by the loader, the file validators, the commands and the RT routing: a performance parameter that the registry does not
// opt out, with a last-value slot and part or master scope; whole-step types must have a range a float holds exactly (<= 2^24), so +-1 never vanishes.
inline bool is_assignable(const params::ParamDef& d) noexcept {
  if (d.cls != params::Class::kPerformance || !d.assignable || d.perf_index < 0) return false;
  if (d.scope != params::Scope::kPart && d.scope != params::Scope::kMaster) return false;
  constexpr double kExact = 16777216.0;
  if (d.type != params::Type::kFloat && (d.max - d.min > kExact || std::fabs(d.min) > kExact || std::fabs(d.max) > kExact)) return false;
  return true;
}
// The assignable parameter `id` names, nullptr for an unknown, structural or non-assignable one. The id is looked up whole: nothing is narrowed to 16 bits,
// so 0x10000 + N never aliases parameter N.
inline const params::ParamDef* find_assignable(std::uint32_t id) noexcept {
  const params::ParamDef* d = params::find(id);
  return d && is_assignable(*d) ? d : nullptr;
}

// Steps -> value, the single rule (exhaustively unit-tested). `current` is the live value, `delta` signed steps; the result is always inside [min, max].
//   float : current + delta * (max - min) / 128, clamped (one step is 1/128 of the range; a full turn of 128 steps sweeps it)
//   int / uint / enum : current rounded to a whole step, plus delta, clamped (+-1 per step, NO wrap)
//   bool  : delta > 0 -> true (max), delta < 0 -> false (min)
//   delta 0 -> the current value (clamped; a non-finite current counts as the registry default)
// Pure: the same inputs give the same result on a given build (double arithmetic, one rounding to float at the end; a compiler that fuses the multiply-add may differ in the last bit between platforms, which the -100 dB goldens absorb).
inline float apply_relative_step(const params::ParamDef& d, float current, std::int32_t delta) noexcept {
  const double lo = d.min, hi = d.max;
  double cur = std::isfinite(current) ? static_cast<double>(current) : d.def;
  if (cur < lo) cur = lo;
  if (cur > hi) cur = hi;
  if (delta == 0) return static_cast<float>(cur);
  double v = cur;
  switch (d.type) {
    case params::Type::kFloat: v = cur + static_cast<double>(delta) * ((hi - lo) / 128.0); break;
    case params::Type::kBool: v = delta > 0 ? hi : lo; break;
    case params::Type::kInt:
    case params::Type::kUint:
    case params::Type::kEnum: {  // whole steps in int64, rounded to float only at the end
      std::int64_t n = static_cast<std::int64_t>(std::floor(cur + 0.5)) + static_cast<std::int64_t>(delta);
      const std::int64_t ilo = static_cast<std::int64_t>(lo), ihi = static_cast<std::int64_t>(hi);
      if (n < ilo) n = ilo;
      if (n > ihi) n = ihi;
      return static_cast<float>(n);
    }
  }
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  return static_cast<float>(v);
}

// RT-published encoder state: the mode flag, the selected part and the counters of what the encoders did. RT is the single writer (the BlockRenderer
// applies the events); any thread reads without locks (relaxed atomics).
// Story 4.13: Fixed parameter functions for encoders 1..8 (0..7) in PART and ALL modes:
// Delay send, Brightness, Attack, Release, EQ Low, Mid, Mid Freq, High.
inline constexpr std::array<std::uint32_t, kEncoderCount> kPartModeParams = {
    params::kSendDelayLevel,
    params::kPartBrightness,
    params::kPartAttack,
    params::kPartRelease,
    params::kPartEqLow,
    params::kPartEqMid,
    params::kPartEqMidFreq,
    params::kPartEqHigh,
};

class EncoderState {
 public:
  struct Counters {
    std::uint64_t steps_applied = 0;     // steps that changed a live value (also when the value was already at the end of its range)
    std::uint64_t steps_empty_slot = 0;  // ASSIGN steps on an empty slot: nothing happened
    std::uint64_t steps_unrouted = 0;    // unrouted steps
    std::uint64_t steps_rejected = 0;    // a step the map cannot honour (the slot names an unknown or non-assignable parameter) or a malformed event
    std::uint64_t events_rejected = 0;   // a mode / select-part event with a bad value
    std::uint64_t steps_no_part = 0;     // a part-scope step whose target part has no engine (silent, unloaded): the live value still moved, not counted as applied
  };

  EncoderMode mode() const noexcept { return static_cast<EncoderMode>(mode_.load(std::memory_order_acquire)); }
  std::uint32_t selected_part() const noexcept { return part_.load(std::memory_order_acquire); }
  Counters counters() const noexcept {
    return Counters{applied_.load(std::memory_order_relaxed), empty_.load(std::memory_order_relaxed), unrouted_.load(std::memory_order_relaxed),
                    rejected_.load(std::memory_order_relaxed), bad_events_.load(std::memory_order_relaxed),
                    no_part_.load(std::memory_order_relaxed)};
  }

  // Story 4.13: ALL mode relative offset per encoder [-0.5f, 0.5f].
  float all_offset(std::uint32_t enc) const noexcept {
    if (enc >= kEncoderCount) return 0.0f;
    return std::bit_cast<float>(all_offsets_[enc].load(std::memory_order_relaxed));
  }
  void set_all_offset(std::uint32_t enc, float val) noexcept VELORA_NONBLOCKING {
    if (enc < kEncoderCount) {
      all_offsets_[enc].store(std::bit_cast<std::uint32_t>(val), std::memory_order_relaxed);
    }
  }
  void reset_all_offsets() noexcept VELORA_NONBLOCKING {
    for (std::uint32_t i = 0; i < kEncoderCount; ++i) {
      all_offsets_[i].store(std::bit_cast<std::uint32_t>(0.0f), std::memory_order_relaxed);
    }
  }

  // RT only.
  void set_mode(EncoderMode m) noexcept VELORA_NONBLOCKING { mode_.store(static_cast<std::uint8_t>(m), std::memory_order_release); }
  void set_selected_part(std::uint32_t p) noexcept VELORA_NONBLOCKING { part_.store(p, std::memory_order_release); }
  void count_applied() noexcept VELORA_NONBLOCKING { applied_.fetch_add(1, std::memory_order_relaxed); }
  void count_empty() noexcept VELORA_NONBLOCKING { empty_.fetch_add(1, std::memory_order_relaxed); }
  void count_unrouted() noexcept VELORA_NONBLOCKING { unrouted_.fetch_add(1, std::memory_order_relaxed); }
  void count_rejected() noexcept VELORA_NONBLOCKING { rejected_.fetch_add(1, std::memory_order_relaxed); }
  void count_bad_event() noexcept VELORA_NONBLOCKING { bad_events_.fetch_add(1, std::memory_order_relaxed); }
  void count_no_part() noexcept VELORA_NONBLOCKING { no_part_.fetch_add(1, std::memory_order_relaxed); }

 private:
  std::atomic<std::uint8_t> mode_{static_cast<std::uint8_t>(EncoderMode::kPart)};
  std::atomic<std::uint32_t> part_{0};
  std::atomic<std::uint64_t> applied_{0}, empty_{0}, unrouted_{0}, rejected_{0}, bad_events_{0}, no_part_{0};
  std::array<std::atomic<std::uint32_t>, kEncoderCount> all_offsets_{};
};

// Ring value (0.0f .. 1.0f) calculation for encoder 0..7 (Story 4.13, UX-DR52, UX-DR85).
template <typename LoadFn>
inline float compute_ring_value(std::uint32_t encoder,
                                EncoderMode mode,
                                std::uint32_t selected_part,
                                float all_offset,
                                const std::array<std::uint16_t, kAssignSlotCount>* assign_map,
                                LoadFn&& load_val) noexcept {
  if (encoder >= kEncoderCount) return 0.0f;
  if (mode == EncoderMode::kAll) {
    return std::clamp(0.5f + all_offset, 0.0f, 1.0f);
  }
  const params::ParamDef* d = nullptr;
  std::uint32_t part = selected_part;
  if (mode == EncoderMode::kPart) {
    d = params::find(kPartModeParams[encoder]);
  } else if (mode == EncoderMode::kAssign) {
    if (!assign_map) return 0.0f;
    const std::uint32_t id = (*assign_map)[encoder];
    if (id == 0) return 0.0f;
    d = find_assignable(id);
    if (!d) return 0.0f;
    part = (d->scope == params::Scope::kPart) ? selected_part : 0u;
  }
  if (!d || d->max <= d->min) return 0.0f;
  const float val = load_val(*d, part);
  const double norm = (static_cast<double>(val) - d->min) / (d->max - d->min);
  return std::clamp(static_cast<float>(norm), 0.0f, 1.0f);
}

static_assert(kReservedEventChannel == kFenceChannel, "the fence lives on the reserved channel");
static_assert(kFenceParamId >= kReservedEventFirstId && kEncoderStepFirstId >= kReservedEventFirstId, "reserved ids start at kReservedEventFirstId");
static_assert(kSelectPartEventId < kFenceParamId, "reserved ids do not overlap");

}  // namespace velora::core
