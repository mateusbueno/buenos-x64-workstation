#include "velora/core/snapshot.hpp"

namespace velora::core {

Snapshot Snapshot::defaults() noexcept {
  Snapshot s;
  for (const params::ParamDef& d : params::kParams) {
    if (d.cls == params::Class::kPerformance) {
      if (d.scope == params::Scope::kPart)
        for (std::uint32_t p = 0; p < params::kPartCount; ++p)
          s.perf_init[params::perf_slot(d, p)] = static_cast<float>(d.def);
      else
        s.perf_init[params::perf_slot(d, 0)] = static_cast<float>(d.def);
    } else if (d.scope == params::Scope::kMaster) {
      s.master[static_cast<std::size_t>(d.master_index)] = static_cast<float>(d.def);
    } else if (d.id == params::kPartOutput) {
      for (auto& part : s.parts) part.output = static_cast<std::uint8_t>(d.def);
    } else if (d.id == params::kTimbreRef) {
      for (auto& part : s.parts) part.timbre_ref = static_cast<std::uint32_t>(d.def);
    }
  }
  return s;
}

std::shared_ptr<FxChain> build_chain(const PartCfg& part) {
  if (part.insert_count == 0) return nullptr;
  auto chain = std::make_shared<FxChain>();
  const std::uint32_t n = part.insert_count < params::kInsertSlotCount ? part.insert_count : params::kInsertSlotCount;
  for (std::uint32_t i = 0; i < n; ++i) {
    const InsertSlotCfg& cfg = part.inserts[i];
    std::unique_ptr<Effect> fx = make_effect(static_cast<EffectType>(cfg.type));
    if (fx) fx->set_params(cfg.params);
    chain->set_slot(i, std::move(fx), cfg.bypass != 0);
  }
  return chain;
}

void build_chains(Snapshot& s) {
  for (PartCfg& p : s.parts) p.chain = build_chain(p);
}

std::uint64_t fx_cost_units(const Snapshot& s) noexcept {
  std::uint64_t total = kFixedFxCost;
  for (const PartCfg& p : s.parts) {
    if (p.chain) {
      total += p.chain->cost_units();
    } else {
      for (std::uint32_t i = 0; i < p.insert_count && i < params::kInsertSlotCount; ++i)
        total += effect_cost_units(static_cast<EffectType>(p.inserts[i].type));
    }
  }
  return total;
}

LastValueTable::LastValueTable() noexcept {
  for (auto& a : v_) a.store(0, std::memory_order_relaxed);
  for (auto& a : gr_) a.store(0, std::memory_order_relaxed);
  for (const params::ParamDef& d : params::kParams) {
    if (d.cls != params::Class::kPerformance) continue;
    const float def = static_cast<float>(d.def);
    if (d.scope == params::Scope::kPart)
      for (std::uint32_t p = 0; p < params::kPartCount; ++p) store(params::perf_slot(d, p), def);
    else
      store(params::perf_slot(d, 0), def);
  }
}

}  // namespace velora::core
