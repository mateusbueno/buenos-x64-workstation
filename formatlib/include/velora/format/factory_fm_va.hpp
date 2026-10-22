#pragma once

#include <string_view>
#include "velora/format/parametric_bundle.hpp"

namespace velora::format {

// Canonical UUID for the founder-authored factory FM/VA sound pack.
inline constexpr const char* kFactoryFmVaLibraryId = "7f3b8901-4c2e-4b95-a162-8e3b5a7c9d01";
inline constexpr const char* kFactoryFmVaLibraryName = "Factory Synth Collection";

// Builds the complete founder-authored factory FM/VA pack (52 timbres across categories).
// All timbres use generic descriptive names free of third-party trademarks.
ParametricBundle create_factory_fm_va_bundle();

} // namespace velora::format
