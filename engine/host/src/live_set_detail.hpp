#pragma once
// Helpers shared by the Live Set loader (Story 4.3) and the scene controller (Story 4.4): how a Live Set's scalar values, parameter lists and
// insert chains become fields of a core::Snapshot. Internal to engine/host (not installed); control thread only.
#include <algorithm>
#include <cmath>

#include "live_set.pb.h"
#include "velora/core/params_gen.hpp"
#include "velora/core/snapshot.hpp"

namespace velora::host::detail {

namespace p = core::params;
namespace lsp = ::velora::liveset::v1;

// A scalar the registry knows: non-finite takes the default, anything else is clamped to the registry range.
inline float sane(const p::ParamDef& d, float v) {
  if (!std::isfinite(v)) return static_cast<float>(d.def);
  return static_cast<float>(std::min<double>(std::max<double>(v, d.min), d.max));
}
inline float sane_volume(float v) { return std::isfinite(v) ? std::min(std::max(v, 0.0f), 1.0f) : 1.0f; }

// Applies one ParamEntry list to the generation: a registry performance parameter becomes the initial value of its slot (for `part` when
// part-scoped); structural master parameters go to the master array; ids this build does not know are preserved in the file and ignored here.
// Anything else (a structural part parameter such as part_output / timbre_ref, a wrong scope) is ignored: a list can never reach the timbre.
// `force` (optional) gets a 1 for every performance slot written (a scene recall re-applies exactly those, see Snapshot::perf_force).
inline void apply_entries(core::Snapshot& s, const google::protobuf::RepeatedPtrField<lsp::ParamEntry>& list, std::uint32_t part, bool master_list,
                          std::array<std::uint8_t, p::kPerfSlotCount>* force = nullptr) {
  for (const lsp::ParamEntry& e : list) {
    const p::ParamDef* d = p::find(e.param_id());
    if (!d || !std::isfinite(e.value())) continue;
    if (master_list && d->id == p::kTempoBpm) continue;  // the tempo is the Live Set's own field, never a master-list or scene value
    const float value = sane(*d, e.value());
    if (d->cls == p::Class::kPerformance) {
      if (master_list ? d->scope != p::Scope::kMaster : d->scope != p::Scope::kPart) continue;
      const std::size_t slot = p::perf_slot(*d, d->scope == p::Scope::kPart ? part : 0);
      s.perf_init[slot] = value;
      if (force) (*force)[slot] = 1;
    } else if (master_list && d->scope == p::Scope::kMaster && d->master_index >= 0) {
      s.master[static_cast<std::size_t>(d->master_index)] = value;
    }
  }
}

inline void fill_inserts(core::PartCfg& pc, const google::protobuf::RepeatedPtrField<ws::v1::InsertSlot>& slots) {
  pc.inserts = {};
  const int n = slots.size() < static_cast<int>(p::kInsertSlotCount) ? slots.size() : static_cast<int>(p::kInsertSlotCount);
  pc.insert_count = static_cast<std::uint8_t>(n);
  for (int i = 0; i < n; ++i) {
    const ws::v1::InsertSlot& in = slots[i];
    core::InsertSlotCfg& dst = pc.inserts[static_cast<std::size_t>(i)];
    for (std::uint32_t k = 0; k < p::kInsertParamCount; ++k) {  // missing params take the registry default
      const p::ParamDef* d = p::find(p::kInsertParam1 + k);
      dst.params[k] = d ? static_cast<float>(d->def) : 0.5f;
    }
    // A type above this build's enum comes from a newer build: it is kept in the file, here the slot is empty ("none").
    dst.type = in.type() >= 0 && static_cast<std::uint32_t>(in.type()) < p::kInsertTypeCount ? static_cast<std::uint8_t>(in.type()) : 0;
    dst.bypass = in.bypass() ? 1 : 0;
    for (int k = 0; k < in.params_size() && k < static_cast<int>(p::kInsertParamCount); ++k) {
      const p::ParamDef* d = p::find(p::kInsertParam1 + static_cast<std::uint32_t>(k));
      if (d && std::isfinite(in.params(k))) dst.params[static_cast<std::size_t>(k)] = in.params(k);
    }
  }
}

// The values of one Live Set part as fields of a generation: zone, octave, transpose, keyboard flag, volume, output, insert chain (configuration only,
// no instances), send levels and the engine / envelope / EQ sound parameters. Not the engine type, the timbre or the part state (those need the
// resolver). Shared by the loader, which builds a generation with it, and by the edit session, which rebuilds the scenes' base values from the
// edited state without loading anything.
inline void fill_part_values(core::Snapshot& snap, const lsp::Part& lp, std::uint32_t pi) {
  core::PartCfg& pc = snap.parts[pi];
  pc.low_key = static_cast<std::uint8_t>(lp.low_key());
  pc.high_key = static_cast<std::uint8_t>(lp.high_key());
  pc.octave = static_cast<std::int8_t>(lp.octave());
  pc.transpose = static_cast<std::int8_t>(lp.transpose());
  pc.keyboard_on = lp.keyboard_on() ? 1 : 0;
  pc.volume = sane_volume(lp.volume());
  pc.output = lp.output() == ws::v1::PART_OUTPUT_AUX ? static_cast<std::uint8_t>(p::kPartOutputAux) : static_cast<std::uint8_t>(p::kPartOutputMain);
  fill_inserts(pc, lp.inserts());
  snap.perf_init[p::perf_slot(*p::find(p::kSendReverbLevel), pi)] = sane(*p::find(p::kSendReverbLevel), lp.send_reverb());
  snap.perf_init[p::perf_slot(*p::find(p::kSendDelayLevel), pi)] = sane(*p::find(p::kSendDelayLevel), lp.send_delay());
  apply_entries(snap, lp.engine().params(), pi, false);
  apply_entries(snap, lp.envelope(), pi, false);
  apply_entries(snap, lp.eq(), pi, false);
}

// The Live Set's master list and tempo (the loader does these after the parts).
inline void fill_master_values(core::Snapshot& snap, const lsp::LiveSet& ls) {
  apply_entries(snap, ls.master(), 0, true);
  snap.perf_init[p::perf_slot(*p::find(p::kTempoBpm), 0)] = sane(*p::find(p::kTempoBpm), ls.tempo());
}

inline bool same_inserts(const core::PartCfg& a, const core::PartCfg& b) {
  if (a.insert_count != b.insert_count) return false;
  for (std::uint32_t i = 0; i < a.insert_count; ++i) {
    const core::InsertSlotCfg &x = a.inserts[i], &y = b.inserts[i];
    if (x.type != y.type || x.bypass != y.bypass || x.params != y.params) return false;
  }
  return true;
}

}  // namespace velora::host::detail
