#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "velora/core/engine.hpp"

namespace velora::core {

constexpr bool is_reserved_engine_id(std::uint32_t id) noexcept {
  return id >= static_cast<std::uint32_t>(EngineTypeId::kSample) &&
         id <= static_cast<std::uint32_t>(EngineTypeId::kVa);
}

// Registry of compiled-in engines. bind() is setup-time only; resolve()/at() are RT-safe.
class EngineRegistry {
 public:
  static constexpr std::size_t kMaxEngines = 8;

  // Returns false if the registry is full or the type is already bound.
  bool bind(Engine& engine) noexcept;

  // kOk with `out` set; kEngineNotAvailable for a reserved id with no bound engine
  // (never substituted); kEngineUnknown otherwise.
  EngineStatus resolve(std::uint32_t id, Engine*& out) const noexcept VELORA_NONBLOCKING;

  std::size_t size() const noexcept { return count_; }
  Engine* at(std::size_t i) const noexcept { return engines_[i]; }

 private:
  std::array<Engine*, kMaxEngines> engines_{};
  std::size_t count_ = 0;
};

}  // namespace velora::core
