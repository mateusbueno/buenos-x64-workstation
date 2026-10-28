#include "velora/core/voice_arbiter.hpp"

#include <algorithm>
#include <limits>

namespace velora::core {

namespace {
constexpr std::uint64_t kMax64 = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t sat_sub(std::uint64_t a, std::uint64_t b) noexcept { return a > b ? a - b : 0; }
}  // namespace

int VoiceArbiter::index_of(const Engine& e) const noexcept {
  for (std::size_t i = 0; i < count_; ++i)
    if (engines_[i] == &e) return static_cast<int>(i);
  return -1;
}

int VoiceArbiter::index_of_type(std::uint32_t id) const noexcept {
  for (std::size_t i = 0; i < count_; ++i)
    if (type_[i] == id) return static_cast<int>(i);
  return -1;
}

bool VoiceArbiter::bind(Engine& engine) {
  if (reserved_ || count_ == kMaxEngines || engine.cost_units_per_voice() == 0) return false;
  const auto id = static_cast<std::uint32_t>(engine.type_id());
  if (index_of_type(id) >= 0) return false;
  const std::size_t i = count_++;
  engines_[i] = &engine;
  type_[i] = id;
  cost_[i] = engine.cost_units_per_voice();
  base_cost_[i] = engine.cost_units_for_features(0);
  features_[i] = 0;
  protected_[i] = engine.steal_protected();
  capacity_[i] = engine.voice_capacity();
  slots_[i].assign(capacity_[i], Slot{});
  std::size_t total = 0;
  for (std::size_t k = 0; k < count_; ++k) total += slots_[k].size();
  victims_.assign(total, 0);
  engine.set_voice_allocator(this);
  return true;
}

void VoiceArbiter::unbind_last() noexcept {
  if (count_ == 0) return;
  const std::size_t i = --count_;
  engines_[i]->set_voice_allocator(nullptr);
  engines_[i] = nullptr;
  protected_[i] = false;
  slots_[i].clear();
  steals_[i].store(0, std::memory_order_relaxed);
}

void VoiceArbiter::install_costs(const std::array<std::uint32_t, kMaxEngines>& cost, const std::array<std::uint32_t, kMaxEngines>& features) noexcept {
  cost_ = cost;
  features_ = features;
  for (std::size_t i = 0; i < count_; ++i) engines_[i]->apply_cost_features(features_[i]);
}

VoiceArbiter::ReserveStatus VoiceArbiter::reserve(const LiveSetConfig& cfg) noexcept {
  PreparedReservation prep;
  const ReserveStatus st = prepare_reservation(cfg, prep);
  if (st != ReserveStatus::kOk) return st;
  res_ = prep.res;
  floor_cost_ = prep.floor_cost;
  ramp_ = prep.ramp;
  cfg_ = prep.cfg;
  reserved_ = true;
  install_costs(prep.cost, prep.features);
  reset_state();
  return ReserveStatus::kOk;
}

void VoiceArbiter::commit_prepared(const PreparedReservation& prep) noexcept {
  if (!prep.valid) return;
  res_ = prep.res;
  floor_cost_ = prep.floor_cost;
  ramp_ = prep.ramp;
  cfg_ = prep.cfg;
  install_costs(prep.cost, prep.features);
  reserved_ = true;  // event state (keys, pedal, deferred releases, serial, counters) is deliberately left alone
}

VoiceArbiter::ReserveStatus VoiceArbiter::prepare_reservation(const LiveSetConfig& cfg, PreparedReservation& out) const noexcept {
  out.valid = false;
  if (cfg.floor_count > kMaxEngines || cfg.steal_ramp_frames == 0 || cfg.steal_ramp_frames > kMaxStealRampFrames ||
      cfg.fx_instance_count > LiveSetConfig::kMaxFxInstances)
    return ReserveStatus::kInvalidConfig;
  // Step 1 of the reservation: the FX budget = the flat amount plus the declared cost of every configured effect instance.
  std::uint64_t fx_total = cfg.fx_cost;
  for (std::uint32_t i = 0; i < cfg.fx_instance_count; ++i) {
    if (cfg.fx_instance_costs[i] > kMax64 - fx_total) return ReserveStatus::kFxExceedsBudget;
    fx_total += cfg.fx_instance_costs[i];
  }
  for (std::size_t i = 0; i < count_; ++i)  // an engine must not have changed since bind()
    if (engines_[i]->voice_capacity() != capacity_[i] || engines_[i]->cost_units_for_features(0) != base_cost_[i])
      return ReserveStatus::kInvalidConfig;
  // Story 13.12: the cost per voice each engine declares under this reservation's cost features (an engine without a floor entry: its static declaration).
  std::array<std::uint32_t, kMaxEngines> nc{}, nf{};
  for (std::uint32_t i = 0; i < cfg.floor_count; ++i) {
    const int idx = index_of_type(cfg.floors[i].engine_type_id);
    if (idx >= 0) nf[static_cast<std::size_t>(idx)] = cfg.floors[i].cost_features;
  }
  for (std::size_t i = 0; i < count_; ++i) nc[i] = engines_[i]->cost_units_for_features(nf[i]);
  std::array<std::uint64_t, kMaxEngines> fc{};
  std::array<bool, kMaxEngines> seen{};
  std::array<std::uint32_t, kMaxEngines> fv{};
  std::uint64_t floors = 0;
  for (std::uint32_t i = 0; i < cfg.floor_count; ++i) {
    const EngineFloor& f = cfg.floors[i];
    const int idx = index_of_type(f.engine_type_id);
    if (idx < 0) return ReserveStatus::kUnknownEngine;
    const auto k = static_cast<std::size_t>(idx);
    if (seen[k] || nc[k] == 0 || protected_[k]) return ReserveStatus::kInvalidConfig;
    if (static_cast<std::uint64_t>(f.voices) + kFadeHeadroomVoices > capacity_[k])
      return ReserveStatus::kFloorExceedsCapacity;
    seen[k] = true;
    fv[k] = f.voices;
    fc[k] = static_cast<std::uint64_t>(f.voices) * nc[k];  // u32 * u32 cannot overflow u64
    if (fc[k] > kMax64 - floors) return ReserveStatus::kFloorsExceedBudget;
    floors += fc[k];
  }
  for (std::size_t i = 0; i < count_; ++i)  // a bound protected engine needs its real cost reserved
    if (protected_[i] && cfg.organ_rotary_cost < static_cast<std::uint64_t>(nc[i]) * kMinProtectedVoices &&
        !(cfg.waive_organ_reservation && cfg.organ_rotary_cost == 0))
      return ReserveStatus::kInvalidConfig;
  const int sample = index_of_type(static_cast<std::uint32_t>(EngineTypeId::kSample));
  if (sample >= 0 && fv[static_cast<std::size_t>(sample)] < min_sample_floor_ &&
      !(cfg.waive_sample_floor && fv[static_cast<std::size_t>(sample)] == 0))
    return ReserveStatus::kSampleFloorTooLow;
  const int fm = index_of_type(static_cast<std::uint32_t>(EngineTypeId::kFm));
  if (fm >= 0 && fv[static_cast<std::size_t>(fm)] < min_fm_floor_ &&
      !(cfg.waive_fm_floor && fv[static_cast<std::size_t>(fm)] == 0))
    return ReserveStatus::kFmFloorTooLow;
  const int va = index_of_type(static_cast<std::uint32_t>(EngineTypeId::kVa));
  if (va >= 0 && fv[static_cast<std::size_t>(va)] < min_va_floor_ &&
      !(cfg.waive_va_floor && fv[static_cast<std::size_t>(va)] == 0))
    return ReserveStatus::kVaFloorTooLow;

  // Reservation order: FX, organ+rotary, floors.
  std::uint64_t rem = cfg.budget;
  if (fx_total > rem) return ReserveStatus::kFxExceedsBudget;
  rem -= fx_total;
  if (cfg.organ_rotary_cost > rem) return ReserveStatus::kOrganExceedsBudget;
  rem -= cfg.organ_rotary_cost;
  if (floors > rem) return ReserveStatus::kFloorsExceedBudget;

  // Everything checked; nothing above has modified state. Fill the result.
  out.res = Reservation{};
  out.res.budget = cfg.budget;
  out.res.fx = fx_total;
  out.res.organ_rotary = cfg.organ_rotary_cost;
  out.res.floors = floors;
  out.res.voice_budget = rem;
  out.res.shared = rem - floors;
  for (std::size_t i = 0; i < count_; ++i)  // protected engines draw on the organ reservation first
    if (protected_[i]) fc[i] = cfg.organ_rotary_cost;
  out.floor_cost = fc;
  out.cost = nc;
  out.features = nf;
  out.ramp = cfg.steal_ramp_frames;
  out.cfg = cfg;
  out.valid = true;
  return ReserveStatus::kOk;
}

void VoiceArbiter::reset_state() noexcept {
  key_down_.fill(0);
  deferred_.fill(0);
  stolen_flag_.fill(0);
  deferred_type_.fill(0);
  pedal_.fill(0);
  serial_ = 1;
  for (auto& x : steals_) x.store(0, std::memory_order_relaxed);
  steals_total_.store(0, std::memory_order_relaxed);
  exhausted_.store(0, std::memory_order_relaxed);
  last_steal_.store(0, std::memory_order_relaxed);
}

void VoiceArbiter::clear_reservation() noexcept {
  reset_state();
  reserved_ = false;
  res_ = Reservation{};
  floor_cost_.fill(0);
  ramp_ = kStealRampFrames;
  install_costs(base_cost_, std::array<std::uint32_t, kMaxEngines>{});  // Story 13.12: back to the static declarations
}

std::uint64_t VoiceArbiter::floor_cost(std::uint32_t id) const noexcept {
  const int i = index_of_type(id);
  return i < 0 ? 0 : floor_cost_[static_cast<std::size_t>(i)];
}

std::uint64_t VoiceArbiter::steals(std::uint32_t id) const noexcept {
  const int i = index_of_type(id);
  return i < 0 ? 0 : steals_[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
}

// --- events ---------------------------------------------------------------------------------------

void VoiceArbiter::note_on(std::uint32_t channel, std::uint32_t note) noexcept {
  ++serial_;
  if (note > 127) return;
  const std::size_t k = (channel & 15u) * 128u + note;
  key_down_[k] = 1;
  deferred_[k] = 0;
  stolen_flag_[k] = 0;
}

bool VoiceArbiter::note_off(std::uint32_t channel, std::uint32_t note, std::uint32_t engine_type_id) noexcept {
  ++serial_;
  if (note > 127) return true;
  const std::size_t k = (channel & 15u) * 128u + note;
  const bool was_down = key_down_[k] != 0;
  key_down_[k] = 0;
  if (was_down && pedal_[channel & 15u]) {
    deferred_[k] = 1;
    deferred_type_[k] = engine_type_id;
    return false;
  }
  return true;
}

void VoiceArbiter::contact_up(std::uint32_t channel, std::uint32_t note) noexcept {
  ++serial_;
  if (note > 127) return;
  const std::size_t k = (channel & 15u) * 128u + note;
  key_down_[k] = 0;
  deferred_[k] = 0;
}

void VoiceArbiter::set_sustain(std::uint32_t channel, bool down) noexcept { pedal_[channel & 15u] = down ? 1 : 0; }

bool VoiceArbiter::take_stolen_flag(std::uint32_t channel, std::uint32_t note) noexcept {
  if (note > 127) return false;
  const std::size_t k = (channel & 15u) * 128u + note;
  const bool f = stolen_flag_[k] != 0;
  stolen_flag_[k] = 0;
  return f;
}

VoiceArbiter::StealRecord VoiceArbiter::last_steal() const noexcept {
  StealRecord r;
  const std::uint64_t v = last_steal_.load(std::memory_order_relaxed);
  if (v == 0) return r;
  const std::size_t g = static_cast<std::size_t>(v & 0xFF) - 1;
  r.valid = true;
  r.engine_type_id = type_[g];
  r.channel = static_cast<std::uint32_t>((v >> 8) & 0xF);
  r.note = static_cast<std::uint32_t>((v >> 12) & 0x7F);
  r.state = static_cast<VoiceState>((v >> 19) & 0x3);
  r.slot = static_cast<std::uint32_t>((v >> 21) & 0xFFFFFF);
  return r;
}

bool VoiceArbiter::take_deferred_release(std::uint32_t channel, std::uint32_t note, std::uint32_t& engine_type_id) noexcept {
  if (note > 127) return false;
  const std::size_t k = (channel & 15u) * 128u + note;
  if (!deferred_[k]) return false;
  deferred_[k] = 0;
  engine_type_id = deferred_type_[k];
  ++serial_;
  return true;
}

// --- accounting -----------------------------------------------------------------------------------

// Refreshes the slot cache from the engines and classifies every live voice.
void VoiceArbiter::scan() noexcept {
  std::uint32_t lowest_held = 128;
  for (std::size_t g = 0; g < count_; ++g) {
    live_[g] = fade_[g] = 0;
    Slot* sl = slots_[g].data();
    for (std::uint32_t s = 0; s < capacity_[g]; ++s) {
      Slot& x = sl[s];
      x.tentative = false;
      x.active = engines_[g]->voice_info(s, x.info);
      if (!x.active) {
        x.fading = false;
        continue;
      }
      if (x.fading) {
        ++fade_[g];
        continue;
      }
      ++live_[g];
      const std::uint32_t ch = x.info.channel & 15u, note = x.info.note & 127u;
      switch (x.info.state) {
        case VoiceState::kReleasing: x.prio = 0; break;
        case VoiceState::kSustained: x.prio = 2; break;
        case VoiceState::kHeld:
        default:
          if (key_down_[ch * 128u + note] || !pedal_[ch]) {
            x.prio = 1;
            if (note < lowest_held) lowest_held = note;
          } else {
            x.prio = 2;  // key up, pedal down: the deferred note off keeps it sounding
          }
          break;
      }
    }
  }
  // The lowest held note (all engines and channels) is the last resort.
  for (std::size_t g = 0; g < count_; ++g) {
    Slot* sl = slots_[g].data();
    for (std::uint32_t s = 0; s < capacity_[g]; ++s)
      if (sl[s].active && !sl[s].fading && sl[s].prio == 1 && (sl[s].info.note & 127u) == lowest_held) sl[s].prio = 3;
  }
}

VoiceArbiter::Usage VoiceArbiter::usage() noexcept {
  scan();
  Usage u;
  for (std::size_t g = 0; g < count_; ++g) {
    u.live_cost += static_cast<std::uint64_t>(live_[g]) * cost_[g];
    u.fading_cost += static_cast<std::uint64_t>(fade_[g]) * cost_[g];
  }
  return u;
}

VoiceArbiter::Usage VoiceArbiter::usage(std::uint32_t id) noexcept {
  Usage u;
  const int i = index_of_type(id);
  if (i < 0) return u;
  scan();
  const auto g = static_cast<std::size_t>(i);
  u.live_cost = static_cast<std::uint64_t>(live_[g]) * cost_[g];
  u.fading_cost = static_cast<std::uint64_t>(fade_[g]) * cost_[g];
  return u;
}

// --- allocation -----------------------------------------------------------------------------------

// Shared units by which admitting a new voice of engine `ei` would overshoot the shared remainder
// (0 = it fits). Inside its own reserved floor an engine always fits. With `ghosts`, fading voices
// count as occupied budget; without, only live voices do.
std::uint64_t VoiceArbiter::excess(std::size_t ei, bool ghosts) const noexcept {
  const std::uint64_t c = cost_[ei];
  if (static_cast<std::uint64_t>(live_[ei]) * c + c <= floor_cost_[ei]) return 0;
  std::uint64_t shared_in_use = 0;
  for (std::size_t g = 0; g < count_; ++g) {
    const std::uint64_t n = static_cast<std::uint64_t>(live_[g]) + (ghosts ? fade_[g] : 0u);
    shared_in_use += sat_sub(n * cost_[g], floor_cost_[g]);
  }
  const std::uint64_t used_e = (static_cast<std::uint64_t>(live_[ei]) + (ghosts ? fade_[ei] : 0u)) * c;
  const std::uint64_t after = shared_in_use - sat_sub(used_e, floor_cost_[ei]) + sat_sub(used_e + c, floor_cost_[ei]);
  return sat_sub(after, res_.shared);
}

std::uint32_t VoiceArbiter::take(std::size_t ei, std::uint32_t slot) noexcept {
  Slot& s = slots_[ei][slot];
  s.born = serial_;
  s.fading = false;
  return slot;
}

std::uint32_t VoiceArbiter::acquire(Engine& engine, std::uint32_t, std::uint32_t) noexcept {
  const int idx = index_of(engine);
  if (idx < 0) return kNoSlot;
  const auto ei = static_cast<std::size_t>(idx);

  if (!reserved_) {  // unreserved: first free slot, never steal
    VoiceInfo vi;
    for (std::uint32_t s = 0; s < capacity_[ei]; ++s)
      if (!engine.voice_info(s, vi)) return take(ei, s);
    exhausted_.fetch_add(1, std::memory_order_relaxed);
    return kNoSlot;
  }
  ++req_;
  // An engine that refuses a fade (cannot happen for an active, non-fading slot) is skipped and the
  // next victim is tried; bounded so a misbehaving engine cannot stall the serial phase.
  for (int attempt = 0; attempt < 4; ++attempt) {
    bool retry = false;
    const std::uint32_t slot = acquire_reserved(ei, retry);
    if (!retry) return slot;
  }
  exhausted_.fetch_add(1, std::memory_order_relaxed);
  return kNoSlot;
}

std::uint32_t VoiceArbiter::acquire_reserved(std::size_t ei, bool& retry) noexcept {
  scan();
  std::uint32_t free_slot = kNoSlot;
  for (std::uint32_t s = 0; s < capacity_[ei]; ++s)
    if (!slots_[ei][s].active) {
      free_slot = s;
      break;
    }
  if (free_slot == kNoSlot) {  // physical table full (live + fading voices)
    exhausted_.fetch_add(1, std::memory_order_relaxed);
    return kNoSlot;
  }

  // Fading voices occupy budget: without stealing, the new voice must fit with them counted.
  const std::uint64_t over_total = excess(ei, true);
  if (over_total == 0) return take(ei, free_slot);

  // Dry run: pick victims until the live voices fit again AND the new voice has taken over the budget of
  // victims worth min(over_total, its own cost): a steal replaces cost 1:1, fading voices are not
  // charged a second time (that would make a burst of notes steal ever more voices). Rolls back when
  // there are not enough victims.
  const std::uint64_t ghost_need = std::min<std::uint64_t>(over_total, cost_[ei]);
  std::uint64_t freed = 0;
  std::uint32_t nv = 0;
  while (excess(ei, false) > 0 || freed < ghost_need) {
    Slot* best = nullptr;
    std::size_t bg = 0;
    std::uint32_t bs = 0;
    for (std::size_t g = 0; g < count_; ++g) {
      if (protected_[g]) continue;  // the organ is never a steal victim
      // Never take from another engine that would drop to or below its floor.
      if (g != ei && static_cast<std::uint64_t>(live_[g]) * cost_[g] < floor_cost_[g] + cost_[g]) continue;
      Slot* sl = slots_[g].data();
      for (std::uint32_t s = 0; s < capacity_[g]; ++s) {
        Slot& x = sl[s];
        if (!x.active || x.fading || x.tentative || x.born == serial_ || x.refused_req == req_) continue;
        if (best) {
          // Order: class, quieter, newer; (engine, slot) ids break ties because we scan in id order
          // and only replace on a strict improvement.
          if (x.prio != best->prio) {
            if (x.prio > best->prio) continue;
          } else if (x.info.level != best->info.level) {
            if (!(x.info.level < best->info.level)) continue;
          } else if (x.info.start_time != best->info.start_time) {
            if (!(x.info.start_time > best->info.start_time)) continue;
          } else {
            continue;
          }
        }
        best = &x;
        bg = g;
        bs = s;
      }
    }
    if (!best) {
      for (std::uint32_t i = 0; i < nv; ++i) {
        const std::uint32_t v = victims_[i];
        slots_[v >> 24][v & 0xFFFFFFu].tentative = false;
        ++live_[v >> 24];
      }
      exhausted_.fetch_add(1, std::memory_order_relaxed);
      return kNoSlot;
    }
    best->tentative = true;
    --live_[bg];
    freed += cost_[bg];
    victims_[nv++] = static_cast<std::uint32_t>((bg << 24) | bs);
  }

  bool refused = false;
  for (std::uint32_t i = 0; i < nv; ++i) {
    const std::uint32_t v = victims_[i];
    const std::size_t g = v >> 24;
    const std::uint32_t s = v & 0xFFFFFFu;
    Slot& x = slots_[g][s];
    x.tentative = false;
    if (!engines_[g]->begin_steal_fade(s, ramp_)) {  // refused: not fading, not counted, try the next victim
      x.refused_req = req_;
      refused = true;
      continue;
    }
    x.fading = true;
    steals_[g].fetch_add(1, std::memory_order_relaxed);
    steals_total_.fetch_add(1, std::memory_order_relaxed);
    if ((x.info.note & 127u) == x.info.note) stolen_flag_[(x.info.channel & 15u) * 128u + x.info.note] = 1;
    last_steal_.store((static_cast<std::uint64_t>(g) + 1) | (static_cast<std::uint64_t>(x.info.channel & 15u) << 8) |
                          (static_cast<std::uint64_t>(x.info.note & 127u) << 12) |
                          (static_cast<std::uint64_t>(x.info.state) << 19) | (static_cast<std::uint64_t>(s) << 21),
                      std::memory_order_relaxed);
  }
  if (refused) {
    retry = true;
    return kNoSlot;
  }
  return take(ei, free_slot);
}

}  // namespace velora::core
