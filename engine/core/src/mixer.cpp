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
  if (const params::ParamDef* d = params::find(params::kLineInLevel)) line_slot_ = static_cast<std::int16_t>(params::perf_slot(*d, 0));
  for (std::uint32_t b = 0; b < kOutputBusCount; ++b) {
    reverb_send_[b] = std::make_unique<PassThroughEffect>(EffectType::kNone, kReverbSendCost);
    delay_send_[b] = std::make_unique<PassThroughEffect>(EffectType::kNone, kDelaySendCost);
    master_eq_[b] = std::make_unique<PassThroughEffect>(EffectType::kEq, kMasterEqCost);
    master_comp_[b] = std::make_unique<PassThroughEffect>(EffectType::kCompressor, kMasterCompCost);
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

void Mixer::adopt(const Snapshot& s) noexcept {
  const bool first = !primed_;
  for (std::uint32_t i = 0; i < kParts; ++i) {
    Part& p = parts_[i];
    const PartCfg& cfg = s.parts[i];
    p.main_target = cfg.output == params::kPartOutputMain ? 1.0f : 0.0f;
    p.aux_target = cfg.output == params::kPartOutputAux ? 1.0f : 0.0f;
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
  primed_ = true;
}

bool Mixer::begin_block(const LastValueTable& live) noexcept {
  const std::uint32_t ramp = instant_levels_ ? 0 : kLevelRampFrames;
  const std::uint32_t fade = instant_levels_ ? 0 : kFadeFrames;
  bool trivial = true;
  for (std::uint32_t i = 0; i < kParts; ++i) {
    Part& p = parts_[i];
    p.main_g.retarget(p.main_target, fade);
    p.aux_g.retarget(p.aux_target, fade);
    if (send_rev_perf_ >= 0) p.rev_g.retarget(clamp01(live.load(static_cast<std::size_t>(send_rev_perf_) * kParts + i)), ramp);
    if (send_dly_perf_ >= 0) p.dly_g.retarget(clamp01(live.load(static_cast<std::size_t>(send_dly_perf_) * kParts + i)), ramp);
    trivial = trivial && p.main_g.steady(1.0f) && p.aux_g.steady(0.0f) && p.rev_g.steady(0.0f) && p.dly_g.steady(0.0f) &&
              p.count == 1 && p.entries[0].chain == nullptr;
  }
  master_g_.retarget(master_target_, ramp);
  if (line_slot_ >= 0) line_g_.retarget(clamp01(live.load(static_cast<std::size_t>(line_slot_))), ramp);
  trivial = trivial && master_g_.steady(1.0f) && line_g_.steady(0.0f);  // (master effects are placeholders: no influence yet)
  instant_levels_ = !primed_;
  routed_ = !trivial;
  if (routed_) {
    bus_l_.fill(0.0f);
    bus_r_.fill(0.0f);
  } else if (ports::LineInSource* li = line_in_.load(std::memory_order_acquire)) {  // keep the source in step with the block clock
    float dl[kN], dr[kN];
    li->read_block(dl, dr, kN);
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

void Mixer::finish(const OutputBuses& out) noexcept {
  // [0] = Main, [1] = Aux. The sends of a part are returned to the bus the part sits on.
  float bus_l[2][kN] = {}, bus_r[2][kN] = {}, rev_l[2][kN] = {}, rev_r[2][kN] = {}, dly_l[2][kN] = {}, dly_r[2][kN] = {};

  for (std::uint32_t i = 0; i < kParts; ++i) {
    Part& p = parts_[i];
    StereoBlock in{&bus_l_[i * kN], &bus_r_[i * kN], kN};
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
      const float gm = p.main_g.next(), ga = p.aux_g.next(), gr = p.rev_g.next(), gd = p.dly_g.next();
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

  // The sends, one instance per bus: placeholders now (real DSP in Stories 2.10/2.11); the return is unity, into the same bus.
  for (std::uint32_t b = 0; b < kOutputBusCount; ++b) {
    StereoBlock rev{rev_l[b], rev_r[b], kN}, dly{dly_l[b], dly_r[b], kN};
    reverb_send_[b]->process(rev);
    delay_send_[b]->process(dly);
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

  // Master section: EQ and compressor per bus (placeholders until Story 2.8), then the master level.
  StereoBlock mb{bus_l[0], bus_r[0], kN}, ab{bus_l[1], bus_r[1], kN};
  master_eq_[0]->process(mb);
  master_comp_[0]->process(mb);
  master_eq_[1]->process(ab);
  master_comp_[1]->process(ab);
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
