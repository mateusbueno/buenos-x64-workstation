#pragma once

#include <string_view>
#include "velora/format/parametric_bundle.hpp"

namespace velora::format {

// Canonical UUID for the factory modeled electric piano sound pack.
inline constexpr const char* kFactoryEpLibraryId = "8a4c9012-5d3f-4e06-b273-9f4c6b8d0e02";
inline constexpr const char* kFactoryEpLibraryName = "Factory Electric Piano Collection";

// Builds the complete factory modeled electric piano pack (7 tine + 5 reed = 12 timbres).
// Includes 1 Init Tine and 1 Init Reed preset.
// All timbres use generic descriptive names strictly adhering to NFR29 (free of third-party trademarks).
ParametricBundle create_factory_ep_bundle();

}  // namespace velora::format
