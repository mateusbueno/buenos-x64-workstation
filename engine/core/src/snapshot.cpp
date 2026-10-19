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

LastValueTable::LastValueTable() noexcept {
  for (auto& a : v_) a.store(0, std::memory_order_relaxed);
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
