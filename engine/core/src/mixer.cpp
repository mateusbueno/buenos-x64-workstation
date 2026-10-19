#include "velora/core/mixer.hpp"

#include <algorithm>
#include <cmath>

namespace velora::core {

namespace {
constexpr std::uint32_t kN = Mixer::kFrames;

float clamp01(float v) noexcept { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
}  // namespace

Mixer::Mixer(SnapshotExchange& exchange) : exchange_(exchange) {
  for (Part& p : parts_) {
    p.entries[0].w.cur = p.entries[0].w.tgt = 1.0f;
    p.main_g.cur = p.main_g.tgt = 1.0f;
  }
  master_g_.cur = master_g_.tgt = 1.0f;
  const params::ParamDef* lvl = params::find(params::kMasterLevel);
  master_level_index_ = lvl ? lvl->master_index : -1;
  if (const params::ParamDef* d = params::find(params::kSendReverbLevel)) send_rev_perf_ = d->perf_index;
  if (const params::ParamDef* d = params::find(params::kSendDelayLevel)) send_dly_perf_ = d->perf_index;
  if (const params::ParamDef* d = params::find(params::kSendReverbMaster)) rev_master_slot_ = static_cast<std::int16_t>(params::perf_slot(*d, 0));
  rev_master_g_.cur = rev_master_g_.tgt = 1.0f;
  {
    static constexpr std::uint16_t kRevIds[3] = {params::kReverbType, params::kReverbTime, params::kReverbLevel};
    for (std::size_t i = 0; i < 3; ++i) {
      const params::ParamDef* d = params::find(kRevIds[i]);
      reverb_idx_[i] = d ? d->master_index : static_cast<std::int16_t>(-1);
    }
  }
  if (const params::ParamDef* d = params::find(params::kTempoBpm)) tempo_slot_ = static_cast<std::int16_t>(params::perf_slot(*d, 0));
  {
    static constexpr std::uint16_t kDlyIds[7] = {params::kDelayTime,  params::kDelaySync,  params::kDelayNote,    params::kDelayFeedback,
                                                 params::kDelayLevel, params::kDelayDamping, params::kDelayPingpong};
    for (std::size_t i = 0; i < 7; ++i) {
      const params::ParamDef* d = params::find(kDlyIds[i]);
      delay_idx_[i] = d ? d->master_index : static_cast<std::int16_t>(-1);
    }
  }
  if (const params::ParamDef* d = params::find(params::kLineInLevel)) line_slot_ = static_cast<std::int16_t>(params::perf_slot(*d, 0));
  for (std::uint32_t b = 0; b < kOutputBusCount; ++b) {
    reverb_send_[b] = std::make_unique<ReverbSend>();
    delay_send_[b] = std::make_unique<DelaySend>();
    eq_wet_[b].cur = eq_wet_[b].tgt = 1.0f;
    comp_wet_[b].cur = comp_wet_[b].tgt = 1.0f;
    master_eq_[b].prepare(kEqSampleRate, kFxBlockFrames);
    master_comp_[b].prepare(kEqSampleRate, kFxBlockFrames);
  }
  static constexpr std::uint16_t kIds[11] = {params::kMasterEqLowGain,    params::kMasterEqMidGain,     params::kMasterEqMidFreq,
                                             params::kMasterEqHighGain,   params::kMasterEqBypass,      params::kMasterCompThreshold,
                                             params::kMasterCompRatio,    params::kMasterCompAttack,    params::kMasterCompRelease,
                                             params::kMasterCompMakeup,   params::kMasterCompBypass};
  for (std::size_t i = 0; i < master_idx_.size(); ++i) {
    const params::ParamDef* d = params::find(kIds[i]);
    master_idx_[i] = d ? d->master_index : static_cast<std::int16_t>(-1);
  }
}

void Mixer::drop_entry(Part& p, std::uint32_t index) noexcept {
  if (p.entries[index].chain) exchange_.rt_unref(p.entries[index].ledger);
  for (std::uint32_t i = index; i + 1 < p.count; ++i) p.entries[i] = p.entries[i + 1];
  --p.count;
}

void Mixer::clear_pending(Part& p) noexcept {
  if (p.has_pending && p.pending) exchange_.rt_unref(p.pending_ledger);
  p.has_pending = false;
  p.pending = nullptr;
}

// A chain entry is added as the newest one (only while the part is settled on a single entry). `instant` (the first generation):
// the part starts on that chain, no fade. `ref_held`: the generation reference was already taken (a pending chain).
void Mixer::push_chain(Part& p, FxChain* chain, std::uint32_t ledger, bool instant, bool ref_held) noexcept {
  if (instant) {
    clear_pending(p);  // no leaked generation reference
    while (p.count > 0) drop_entry(p, p.count - 1);
  }
  Entry e;
  e.chain = chain;
  e.ledger = ledger;
  if (chain && !ref_held) exchange_.rt_ref(ledger);
  if (instant) {
    e.w.cur = e.w.tgt = 1.0f;
  } else {
    for (std::uint32_t i = 0; i < p.count; ++i) p.entries[i].w.restart(0.0f, kFadeFrames);
    e.w.cur = 0.0f;
    e.w.retarget(1.0f, kFadeFrames);
  }
  p.entries[p.count++] = e;
}

void Mixer::adopt(const Snapshot& s, const LastValueTable* live) noexcept {
  const bool first = !primed_;
  for (std::uint32_t i = 0; i < kParts; ++i) {
    Part& p = parts_[i];
    const PartCfg& cfg = s.parts[i];
    // The part fader (Story 4.3, PartCfg::volume, 1 = unity: the pre-4.3 value) scales the feed to the bus the part sits on.
    const float vol = clamp01(cfg.volume);
    p.main_target = cfg.output == params::kPartOutputMain ? vol : 0.0f;
    p.aux_target = cfg.output == params::kPartOutputAux ? vol : 0.0f;
    FxChain* chain = cfg.chain.get();
    // The generation that introduced a chain is not kept alive by it for as long as the chain lives: an entry (or the waiting chain)
    // whose chain this generation still carries moves its reference to this (held, so valid) generation; the older one can be freed.
    for (std::uint32_t k = 0; k < p.count; ++k) {
      Entry& e = p.entries[k];
      if (e.chain && e.chain == chain && e.ledger != s.ledger_index) {
        exchange_.rt_ref(s.ledger_index);
        exchange_.rt_unref(e.ledger);
        e.ledger = s.ledger_index;
      }
    }
    if (p.has_pending && p.pending && p.pending == chain && p.pending_ledger != s.ledger_index) {
      exchange_.rt_ref(s.ledger_index);
      exchange_.rt_unref(p.pending_ledger);
      p.pending_ledger = s.ledger_index;
    }
    if (chain && chain->is_empty()) chain = nullptr;  // nothing would process: the same as no chain
    if (first) {
      push_chain(p, chain, s.ledger_index, /*instant=*/true);
      continue;
    }
    FxChain* newest = p.entries[p.count - 1].chain;
    if (p.count == 1) {
      if (chain != newest) push_chain(p, chain, s.ledger_index, /*instant=*/false);
      continue;
    }
    // A cross-fade is running: the latest request waits for it (a request for the chain that is fading in cancels the wait).
    clear_pending(p);
    if (chain != newest) {
      p.pending = chain;
      p.pending_ledger = s.ledger_index;
      p.has_pending = true;
      if (chain) exchange_.rt_ref(s.ledger_index);
    }
  }
  float db = 0.0f;
  if (master_level_index_ >= 0) db = s.master[static_cast<std::size_t>(master_level_index_)];
  master_target_ = db == 0.0f ? 1.0f : std::pow(10.0f, db * 0.05f);
  // Master EQ and compressor: the registry values of this generation (defaults when the registry lacks one).
  auto mv = [&](std::size_t i, float dflt) {
    return master_idx_[i] >= 0 ? s.master[static_cast<std::size_t>(master_idx_[i])] : dflt;
  };
  for (std::uint32_t b = 0; b < kOutputBusCount; ++b) {
    master_eq_[b].set_physical(mv(0, 0.0f), mv(1, 0.0f), mv(2, 1000.0f), mv(3, 0.0f), /*instant=*/first);
    master_comp_[b].set_physical(mv(5, -12.0f), mv(6, 1.0f), mv(7, 10.0f), mv(8, 100.0f), mv(9, 0.0f), /*instant=*/first);
    eq_wet_[b].retarget(mv(4, 0.0f) >= 0.5f ? 0.0f : 1.0f, first ? 0 : kFadeFrames);  // flag: >= 0.5 bypasses; NaN and < 0.5 do not
    comp_wet_[b].retarget(mv(10, 0.0f) >= 0.5f ? 0.0f : 1.0f, first ? 0 : kFadeFrames);
  }
  // Reverb (Story 2.10): type, time and level of this generation (registry defaults when the registry lacks one).
  {
    auto rv = [&](std::size_t i, float dflt) {
      return reverb_idx_[i] >= 0 ? s.master[static_cast<std::size_t>(reverb_idx_[i])] : dflt;
    };
    const ReverbType type = reverbmap::type(rv(0, static_cast<float>(params::kReverbTypeHall)));
    const float time = rv(1, reverbmap::kDefaultTimeS), level = rv(2, 1.0f);
    for (std::uint32_t b = 0; b < kOutputBusCount; ++b) reverb_send_[b]->configure(type, time, level, /*instant=*/first);
  }
  // Delay (Story 2.11): time, sync, note value, feedback, return level, damping and ping-pong of this generation (registry defaults when the registry lacks one).
  {
    auto dv = [&](std::size_t i, float dflt) {
      return delay_idx_[i] >= 0 ? s.master[static_cast<std::size_t>(delay_idx_[i])] : dflt;
    };
    const float time = dv(0, delaymap::kDefaultTimeMs), fb = dv(3, delaymap::kDefaultFeedback), level = dv(4, delaymap::kDefaultLevel),
                damping = dv(5, delaymap::kDefaultDamping), pp = dv(6, delaymap::kDefaultPingPong);
    const bool sync = delaymap::sync(dv(1, 0.0f));
    if (live && tempo_slot_ >= 0) {  // the current tempo first: an instant (first-generation) sync target must already use it
      tempo_bpm_ = delaymap::tempo_bpm(live->load(static_cast<std::size_t>(tempo_slot_)));
      for (const auto& d : delay_send_) d->set_tempo(tempo_bpm_);
    }
    const DelayNote note = delaymap::note(dv(2, static_cast<float>(params::kDelayNoteEighth)));
    for (std::uint32_t b = 0; b < kOutputBusCount; ++b) delay_send_[b]->configure(time, sync, note, fb, level, damping, pp, /*instant=*/first);
  }
  primed_ = true;
}

bool Mixer::master_trivial() const noexcept VELORA_NONBLOCKING {
  for (std::uint32_t b = 0; b < kOutputBusCount; ++b) {
    const bool eq_ok = eq_wet_[b].steady(0.0f) || (eq_wet_[b].steady(1.0f) && master_eq_[b].is_identity());
    const bool comp_ok = comp_wet_[b].steady(0.0f) || (comp_wet_[b].steady(1.0f) && master_comp_[b].is_identity());
    if (!eq_ok || !comp_ok) return false;
  }
  return true;
}

bool Mixer::begin_block(LastValueTable& live) noexcept {
  const std::uint32_t ramp = instant_levels_ ? 0 : kLevelRampFrames;
  const std::uint32_t fade = instant_levels_ ? 0 : kFadeFrames;
  bool trivial = true;
  // The RT plane's single tempo value (BlockRenderer::tempo_bpm()): read once per block, so a tempo change takes effect at the next block.
  tempo_bpm_ = tempo_slot_ >= 0 ? delaymap::tempo_bpm(live.load(static_cast<std::size_t>(tempo_slot_))) : delaymap::kDefaultTempoBpm;
  for (const auto& d : delay_send_) d->set_tempo(tempo_bpm_);
  if (rev_master_slot_ >= 0) rev_master_g_.retarget(clamp01(live.load(static_cast<std::size_t>(rev_master_slot_))), ramp);
  const bool rev_master_off = rev_master_g_.steady(0.0f);
  for (std::uint32_t i = 0; i < kParts; ++i) {
    Part& p = parts_[i];
    p.main_g.retarget(p.main_target, fade);
    p.aux_g.retarget(p.aux_target, fade);
    if (send_rev_perf_ >= 0) p.rev_g.retarget(clamp01(live.load(static_cast<std::size_t>(send_rev_perf_) * kParts + i)), ramp);
    if (send_dly_perf_ >= 0) p.dly_g.retarget(clamp01(live.load(static_cast<std::size_t>(send_dly_perf_) * kParts + i)), ramp);
    // A send counts as zero when the part's own level or the REV master level is steadily 0 (the master is checked below).
    trivial = trivial && p.main_g.steady(1.0f) && p.aux_g.steady(0.0f) && (p.rev_g.steady(0.0f) || rev_master_off) && p.dly_g.steady(0.0f) &&
              p.count == 1 && p.entries[0].chain == nullptr;
  }
  for (const auto& r : reverb_send_) trivial = trivial && r->quiet();  // a tail that still rings is not transparent
  for (const auto& d : delay_send_) trivial = trivial && d->quiet();
  master_g_.retarget(master_target_, ramp);
  if (line_slot_ >= 0) line_g_.retarget(clamp01(live.load(static_cast<std::size_t>(line_slot_))), ramp);
  trivial = trivial && master_g_.steady(1.0f) && line_g_.steady(0.0f) && master_trivial();
  instant_levels_ = !primed_;
  routed_ = !trivial;
  if (routed_) {
    bus_l_.fill(0.0f);
    bus_r_.fill(0.0f);
  } else {
    // The sends are muted by the REV master: settle the ramps that will not run in this skipped block.
    for (Part& p : parts_) {
      p.rev_g.cur = p.rev_g.tgt;
      p.rev_g.left = 0;
    }
    rev_master_g_.cur = rev_master_g_.tgt;  // together with the part gains, so a master that left 0 later ramps up from the settled value
    rev_master_g_.left = 0;
    if (gr_nonzero_) {  // nothing runs any more: no compressor reduces gain
      for (std::size_t i = 0; i < LastValueTable::kGainReductionSlots; ++i) live.store_gain_reduction(i, 0.0f);
      gr_nonzero_ = false;
    }
    if (ports::LineInSource* li = line_in_.load(std::memory_order_acquire)) {  // keep the source in step with the block clock
      float dl[kN], dr[kN];
      li->read_block(dl, dr, kN);
    }
  }
  return routed_;
}

void Mixer::part_sinks(std::uint32_t offset, float** left, float** right) noexcept {
  for (std::uint32_t c = 0; c < Engine::kSplitChannels; ++c) {
    const std::uint32_t part = c & (kParts - 1);
    left[c] = &bus_l_[part * kN + offset];
    right[c] = &bus_r_[part * kN + offset];
  }
}

void Mixer::run_stage(Effect& fx, Ramp& wet, bool& off, StereoBlock& b) noexcept VELORA_NONBLOCKING {
  if (wet.steady(0.0f)) {  // bypassed: does not run; the state is cleared once so it starts clean when it comes back
    if (!off) {
      fx.reset();
      off = true;
    }
    return;
  }
  off = false;
  if (wet.steady(1.0f)) {
    fx.process(b);
    return;
  }
  float wl[kN], wr[kN];  // cross-fade between bypassed and active
  std::copy(b.left, b.left + kN, wl);
  std::copy(b.right, b.right + kN, wr);
  StereoBlock wb{wl, wr, kN};
  fx.process(wb);
  for (std::uint32_t f = 0; f < kN; ++f) {
    const float w = wet.next();
    b.left[f] += w * (wl[f] - b.left[f]);
    b.right[f] += w * (wr[f] - b.right[f]);
  }
}

void Mixer::publish_gain_reduction(LastValueTable& live) noexcept VELORA_NONBLOCKING {
  bool any = false;
  for (std::uint32_t i = 0; i < kParts; ++i) {
    const Part& p = parts_[i];
    for (std::uint32_t s = 0; s < params::kInsertSlotCount; ++s) {
      float gr = 0.0f;  // the largest reduction of any chain still audible in this slot (a chain fading out counts until it is gone)
      for (std::uint32_t k = 0; k < p.count; ++k) {
        const FxChain* chain = p.entries[k].chain;
        if (!chain || s >= chain->slot_count()) continue;
        const FxChain::Slot& sl = chain->slot(s);
        if (sl.effect && !sl.bypass) gr = std::max(gr, sl.effect->gain_reduction_db());
      }
      any = any || gr != 0.0f;
      live.store_gain_reduction(LastValueTable::gr_insert_slot(i, s), gr);
    }
  }
  for (std::uint32_t b = 0; b < kOutputBusCount; ++b) {
    const float gr = comp_wet_[b].steady(0.0f) ? 0.0f : master_comp_[b].gain_reduction_db();
    any = any || gr != 0.0f;
    live.store_gain_reduction(LastValueTable::gr_master_slot(b), gr);
  }
  gr_nonzero_ = any;
}

void Mixer::finish(const OutputBuses& out, LastValueTable& live) noexcept {
  // [0] = Main, [1] = Aux. The sends of a part are returned to the bus the part sits on.
  float bus_l[2][kN] = {}, bus_r[2][kN] = {}, rev_l[2][kN] = {}, rev_r[2][kN] = {}, dly_l[2][kN] = {}, dly_r[2][kN] = {};

  float rev_m[kN];  // REV master send level per frame
  const bool rev_master_on = !rev_master_g_.steady(0.0f);
  for (std::uint32_t f = 0; f < kN; ++f) rev_m[f] = rev_master_g_.next();
  bool rev_in[kOutputBusCount] = {false, false};  // some part may feed the reverb of this bus in this block
  bool dly_in[kOutputBusCount] = {false, false};  // ... and the delay
  for (std::uint32_t i = 0; i < kParts; ++i) {
    Part& p = parts_[i];
    StereoBlock in{&bus_l_[i * kN], &bus_r_[i * kN], kN};
    if (rev_master_on && !p.rev_g.steady(0.0f)) {  // this part may feed the reverb of the bus(es) it sits on (judged before the ramps advance)
      rev_in[0] = rev_in[0] || !p.main_g.steady(0.0f);
      rev_in[1] = rev_in[1] || !p.aux_g.steady(0.0f);
    }
    if (!p.dly_g.steady(0.0f)) {
      dly_in[0] = dly_in[0] || !p.main_g.steady(0.0f);
      dly_in[1] = dly_in[1] || !p.aux_g.steady(0.0f);
    }
    float acc_l[kN], acc_r[kN];
    const float* ol;
    const float* orr;
    if (p.count == 1 && p.entries[0].w.steady(1.0f)) {  // settled: the chain works in place, no copy
      if (p.entries[0].chain) p.entries[0].chain->process(in);
      ol = in.left;
      orr = in.right;
    } else {  // cross-fade: every entry processes a copy of the part bus, weighted sum
      std::fill(acc_l, acc_l + kN, 0.0f);
      std::fill(acc_r, acc_r + kN, 0.0f);
      for (std::uint32_t k = 0; k < p.count; ++k) {
        Entry& e = p.entries[k];
        float tl[kN], tr[kN];
        std::copy(in.left, in.left + kN, tl);
        std::copy(in.right, in.right + kN, tr);
        if (e.chain) {
          StereoBlock tb{tl, tr, kN};
          e.chain->process(tb);
        }
        for (std::uint32_t f = 0; f < kN; ++f) {
          const float w = e.w.next();
          acc_l[f] += w * tl[f];
          acc_r[f] += w * tr[f];
        }
      }
      ol = acc_l;
      orr = acc_r;
    }
    for (std::uint32_t f = 0; f < kN; ++f) {
      const float gm = p.main_g.next(), ga = p.aux_g.next(), gr = p.rev_g.next() * rev_m[f], gd = p.dly_g.next();
      const float l = ol[f], r = orr[f];
      bus_l[0][f] += gm * l;
      bus_r[0][f] += gm * r;
      bus_l[1][f] += ga * l;
      bus_r[1][f] += ga * r;
      rev_l[0][f] += gr * gm * l;
      rev_r[0][f] += gr * gm * r;
      rev_l[1][f] += gr * ga * l;
      rev_r[1][f] += gr * ga * r;
      dly_l[0][f] += gd * gm * l;
      dly_r[0][f] += gd * gm * r;
      dly_l[1][f] += gd * ga * l;
      dly_r[1][f] += gd * ga * r;
    }
    // Entries that finished fading out are released (the generation reference goes with them); the newest one stays.
    for (std::uint32_t k = 0; k + 1 < p.count;) {
      if (p.entries[k].w.steady(0.0f)) drop_entry(p, k);
      else ++k;
    }
    if (p.count == 1 && p.has_pending) {  // the fade is over: the chain that waited starts to fade in
      FxChain* c = p.pending;
      const std::uint32_t led = p.pending_ledger;
      p.has_pending = false;
      p.pending = nullptr;
      push_chain(p, c, led, /*instant=*/false, /*ref_held=*/true);
    }
  }

  // The sends, one instance per bus, returned into the same bus. Reverb (Story 2.10) and delay (Story 2.11): skipped while nothing feeds
  // them and nothing rings.
  for (std::uint32_t b = 0; b < kOutputBusCount; ++b) {
    StereoBlock rev{rev_l[b], rev_r[b], kN}, dly{dly_l[b], dly_r[b], kN};
    if (rev_in[b] || !reverb_send_[b]->quiet()) reverb_send_[b]->process(rev);
    else std::fill(rev_l[b], rev_l[b] + kN, 0.0f), std::fill(rev_r[b], rev_r[b] + kN, 0.0f);
    if (dly_in[b] || !delay_send_[b]->quiet()) delay_send_[b]->process(dly);
    else std::fill(dly_l[b], dly_l[b] + kN, 0.0f), std::fill(dly_r[b], dly_r[b] + kN, 0.0f);
    for (std::uint32_t f = 0; f < kN; ++f) {
      bus_l[b][f] += rev_l[b][f] + dly_l[b][f];
      bus_r[b][f] += rev_r[b][f] + dly_r[b][f];
    }
  }

  // Line-in: into the master (Main) only. Non-finite input is muted before it can poison the mix.
  if (ports::LineInSource* li = line_in_.load(std::memory_order_acquire)) {
    float ll[kN], lr[kN];
    li->read_block(ll, lr, kN);
    for (std::uint32_t f = 0; f < kN; ++f) {
      const float g = line_g_.next();
      const float a = std::isfinite(ll[f]) ? ll[f] : 0.0f, b = std::isfinite(lr[f]) ? lr[f] : 0.0f;
      bus_l[0][f] += g * a;
      bus_r[0][f] += g * b;
    }
  } else {
    for (std::uint32_t f = 0; f < kN; ++f) line_g_.next();
  }

  // Master section: EQ, then compressor, per bus (each with its own bypass cross-fade), then the master level.
  StereoBlock mb{bus_l[0], bus_r[0], kN}, ab{bus_l[1], bus_r[1], kN};
  run_stage(master_eq_[0], eq_wet_[0], eq_off_[0], mb);
  run_stage(master_comp_[0], comp_wet_[0], comp_off_[0], mb);
  run_stage(master_eq_[1], eq_wet_[1], eq_off_[1], ab);
  run_stage(master_comp_[1], comp_wet_[1], comp_off_[1], ab);
  publish_gain_reduction(live);
  for (std::uint32_t f = 0; f < kN; ++f) {
    const float g = master_g_.next();
    out.main_left[f] += g * bus_l[0][f];
    out.main_right[f] += g * bus_r[0][f];
    if (out.aux_left && out.aux_right) {
      out.aux_left[f] += g * bus_l[1][f];
      out.aux_right[f] += g * bus_r[1][f];
    }
  }
}

bool fill_fx_costs(const Snapshot& s, VoiceArbiter::LiveSetConfig& cfg) noexcept {
  std::array<std::uint32_t, VoiceArbiter::LiveSetConfig::kMaxFxInstances> costs{};
  std::uint32_t n = 0;
  auto add = [&](std::uint32_t c) {
    if (n >= costs.size()) return false;
    costs[n++] = c;
    return true;
  };
  for (std::uint32_t b = 0; b < kOutputBusCount; ++b)  // per output bus: a reverb send, a delay send, master EQ and compressor
    if (!add(kReverbSendCost) || !add(kDelaySendCost) || !add(kMasterEqCost) || !add(kMasterCompCost)) return false;
  for (const PartCfg& p : s.parts) {
    if (p.chain) {
      for (std::uint32_t i = 0; i < p.chain->slot_count(); ++i)
        if (p.chain->slot(i).effect && !add(p.chain->slot(i).effect->cost_units())) return false;
    } else {
      for (std::uint32_t i = 0; i < p.insert_count && i < params::kInsertSlotCount; ++i) {
        const std::uint32_t c = effect_cost_units(static_cast<EffectType>(p.inserts[i].type));
        if (c != 0 && !add(c)) return false;
      }
    }
  }
  cfg.fx_instance_costs = costs;
  cfg.fx_instance_count = n;
  return true;
}

}  // namespace velora::core
