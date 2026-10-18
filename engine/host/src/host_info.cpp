#include "velora/host/host_info.hpp"

#include "velora/core/version.hpp"

namespace velora::host {

const char* host_name() noexcept { return velora::core::core_version() > 0 ? "velora-host" : ""; }

}  // namespace velora::host
