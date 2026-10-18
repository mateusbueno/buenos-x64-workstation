#include "velora/core/engine_registry.hpp"

namespace velora::core {

bool EngineRegistry::bind(Engine& engine) noexcept {
  if (count_ == kMaxEngines) return false;
  for (std::size_t i = 0; i < count_; ++i)
    if (engines_[i]->type_id() == engine.type_id()) return false;
  engines_[count_++] = &engine;
  return true;
}

EngineStatus EngineRegistry::resolve(std::uint32_t id, Engine*& out) const noexcept {
  for (std::size_t i = 0; i < count_; ++i) {
    if (static_cast<std::uint32_t>(engines_[i]->type_id()) == id) {
      out = engines_[i];
      return EngineStatus::kOk;
    }
  }
  out = nullptr;
  return is_reserved_engine_id(id) ? EngineStatus::kEngineNotAvailable : EngineStatus::kEngineUnknown;
}

}  // namespace velora::core
