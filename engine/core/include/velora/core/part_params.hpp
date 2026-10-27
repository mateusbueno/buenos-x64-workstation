#pragma once
// Registry part parameters inside an engine (Story 9.9 follow-up). Shared helpers of FmEngine, VaEngine and SampleEngine; no OS, no allocation.
//
// ENGAGEMENT. The block loop forwards EVERY registry value of a part to its engine, also the initial ones at a generation change, and those initial
// values are the registry defaults. An engine whose own base patch is not the registry default (the FM engine starts a part on an electric-piano patch;
// an offline render or a future patch loader installs its own with set_part_voice_params()) must keep that sound when nothing was edited. So a
// parameter is "engaged" when it is edited, or when an initial / re-synced value differs from its registry default; from then on it follows every
// value it gets (a return to the default included). While it is not engaged the engine's base patch decides.
//   * Engine::set_part_param      initial / re-synced value (generation change, part remap): engages only when value != registry default.
//   * Engine::set_part_param_edit live edit (knob, UMP parameter event, encoder step): always engages, so a knob typed or dragged to exactly the
//                                 default value still takes effect.
// A Live Set switch (Engine::reset_play_state) disengages everything and restores the base patch; the new Live Set's stored values are forwarded right
// after and engage again where they differ from the default. The one thing this cannot express: a Live Set that STORES the registry default for a
// parameter over an engine whose base patch differs keeps the base patch's value (documented in engine/core/README.md).
#include <array>
#include <cmath>
#include <cstdint>

#include "velora/core/params_gen.hpp"

namespace velora::core::part_params {

// Registry default of `id` as float (0 when the id is unknown).
inline float default_of(std::uint32_t id) noexcept {
  const params::ParamDef* d = params::find(id);
  return d ? static_cast<float>(d->def) : 0.0f;
}

// One bit per parameter id in [first, first + 128): which parameters of one part are engaged.
class EngagedSet {
 public:
  void clear() noexcept { w_ = {}; }
  bool test(std::uint32_t off) const noexcept { return off < 128 && ((w_[off >> 6] >> (off & 63)) & 1u) != 0; }
  void set(std::uint32_t off) noexcept {
    if (off < 128) w_[off >> 6] |= std::uint64_t{1} << (off & 63);
  }
  bool any() const noexcept { return (w_[0] | w_[1]) != 0; }

 private:
  std::array<std::uint64_t, 2> w_{};
};

// The edit path: always applies, and engages.
inline void engage(EngagedSet& set, std::uint32_t off) noexcept { set.set(off); }

// True when `value` must be applied: the parameter is engaged, or this value leaves the registry default (which engages it).
inline bool should_apply(EngagedSet& set, std::uint32_t off, std::uint32_t id, float value) noexcept {
  if (set.test(off)) return true;
  if (value == default_of(id)) return false;
  set.set(off);
  return true;
}

// Float -> integer parameter value: nearest, clamped.
inline int to_int(float v, int lo, int hi) noexcept {
  if (!std::isfinite(v)) return lo;
  const long n = std::lround(v);
  return n < lo ? lo : (n > hi ? hi : static_cast<int>(n));
}

// Float parameter value, clamped to [lo, hi]; a non-finite value is "no change" (returns false).
inline bool clamp_finite(float v, float lo, float hi, float& out) noexcept {
  if (!std::isfinite(v)) return false;
  out = v < lo ? lo : (v > hi ? hi : v);
  return true;
}

}  // namespace velora::core::part_params
