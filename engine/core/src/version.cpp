#include "velora/core/version.hpp"

#include "velora/ports/version.hpp"

namespace velora::core {

int core_version() noexcept { return 1 + velora::ports::kPortsApiVersion - 1; }

}  // namespace velora::core
