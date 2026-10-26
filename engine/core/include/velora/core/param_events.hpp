#pragma once
// Performance parameters on the RT path (Story 2.1, AD-8/AD-9). A performance parameter change is
// ONE timestamped UMP, never a pointer or a snapshot:
//
//   MIDI 2.0 assignable controller (message type 4, status 0x3), group 0
//     word0: [31:28]=4 | [27:24]=0 | [23:20]=3 | [19:16]=channel=PART | [15:8]=bank=(param_id>>7)&0x7F | [7:0]=index=param_id&0x7F
//     word1: the value normalised to the parameter range, 0 = min .. 0xFFFFFFFF = max
//
// Parameter ids are therefore limited to 14 bits (the generator enforces it). Channel = part (0..7).
// Integer, bool and enum parameters round to the nearest step on decode, so the 32-bit scale is lossless
// for them. MIDI CC numbers map to the same event through ParamDef::cc (see cc_to_param_event).
#include <cmath>
#include <cstdint>

#include "velora/core/params_gen.hpp"
#include "velora/core/ump.hpp"

namespace velora::core {

struct ParamEvent {
  const params::ParamDef* def = nullptr;
  std::uint32_t part = 0;
  float value = 0.0f;
};

constexpr std::uint32_t normalise_param(const params::ParamDef& d, double v) noexcept {
  if (!(v > d.min)) return 0;
  if (!(v < d.max)) return 0xFFFFFFFFu;
  return static_cast<std::uint32_t>((v - d.min) / (d.max - d.min) * 4294967295.0 + 0.5);
}

inline float denormalise_param(const params::ParamDef& d, std::uint32_t n) noexcept {
  double v = d.min + (static_cast<double>(n) / 4294967295.0) * (d.max - d.min);
  if (d.type != params::Type::kFloat) v = std::floor(v + 0.5);
  if (v < d.min) v = d.min;
  if (v > d.max) v = d.max;
  return static_cast<float>(v);
}

// Builds the event for a performance parameter of `part`. The caller validated id/part/range.
constexpr Ump make_param_event(const params::ParamDef& d, std::uint32_t part, double value) noexcept {
  return Ump{(Ump::kMessageTypeChannelVoice2 << 28) | (Ump::kStatusAssignableController << 20) |
                 ((part & 0xF) << 16) | (((d.id >> 7) & 0x7Fu) << 8) | (d.id & 0x7Fu),
             normalise_param(d, value)};
}

// Decodes a parameter event. False when `ump` is not one, names an unknown id, a structural
// parameter or an out-of-range part: such events never change a live value.
inline bool decode_param_event(const Ump& ump, ParamEvent& out) noexcept {
  if (ump.group() != 0 || !ump.is_assignable_controller()) return false;
  const std::uint32_t id = (((ump.word0 >> 8) & 0x7Fu) << 7) | (ump.word0 & 0x7Fu);
  const params::ParamDef* d = params::find(id);
  if (!d || d->cls != params::Class::kPerformance) return false;
  const std::uint32_t part = ump.channel();
  if (d->scope == params::Scope::kPart ? part >= params::kPartCount : part != 0) return false;
  out.def = d;
  out.part = part;
  out.value = denormalise_param(*d, ump.word1);
  return true;
}

// Control->RT fence event (Story 4.9). Same wire shape as a parameter event but on channel 15 with the reserved id 0x3FFF (the registry refuses ids from 0x3FE0, so it never is a parameter event of part 15); word1 carries the sequence number. RT acknowledges it in LastValueTable::store_fence_ack().
constexpr std::uint32_t kFenceChannel = 0xF;
constexpr std::uint32_t kFenceParamId = 0x3FFF;
constexpr Ump make_fence_event(std::uint32_t seq) noexcept {
  return Ump{(Ump::kMessageTypeChannelVoice2 << 28) | (Ump::kStatusAssignableController << 20) | (kFenceChannel << 16) |
                 (((kFenceParamId >> 7) & 0x7Fu) << 8) | (kFenceParamId & 0x7Fu),
             seq};
}
constexpr bool is_fence_event(const Ump& ump) noexcept {
  return ump.group() == 0 && ump.is_assignable_controller() && ump.channel() == kFenceChannel && ((ump.word0 >> 8) & 0x7Fu) == ((kFenceParamId >> 7) & 0x7Fu) &&
         (ump.word0 & 0x7Fu) == (kFenceParamId & 0x7Fu);
}

// Origin mark of a parameter event that was made from a genuine MIDI control change (cc_to_param_event): bit 7 of the index byte, which
// the 7-bit index never uses, so decoding and every comparison of ids are unchanged. The block loop (Story 5.2) feeds the sample engine's raw
// controller state from MIDI control changes only: a knob move from the UI / ws.v1 builds its event with make_param_event and carries no mark.
constexpr std::uint32_t kMidiCcOriginBit = 0x80u;
constexpr bool is_midi_cc_origin(const Ump& ump) noexcept { return (ump.word0 & kMidiCcOriginBit) != 0; }

// MIDI CC (MIDI 2.0 control change: channel = part, 32-bit value) -> parameter event (marked as MIDI-originated). False when the
// controller is not mapped or the channel is not a part.
inline bool cc_to_param_event(const Ump& cc, Ump& out) noexcept {
  if (!cc.is_control_change() || cc.group() != 0 || cc.channel() >= params::kPartCount) return false;
  const params::ParamDef* d = params::find_by_cc(cc.controller_index());
  if (!d) return false;
  const double n = static_cast<double>(cc.controller_value32()) / 4294967295.0;
  out = make_param_event(*d, cc.channel(), d->min + n * (d->max - d->min));
  out.word0 |= kMidiCcOriginBit;
  return true;
}

}  // namespace velora::core
