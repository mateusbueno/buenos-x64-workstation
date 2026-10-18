#pragma once

// Same definition as velora/core/rt.hpp (ports must not include core). An identical
// redefinition is legal, so including both headers in either order is fine.
#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::nonblocking)
#define VELORA_NONBLOCKING [[clang::nonblocking]]
#endif
#endif
#ifndef VELORA_NONBLOCKING
#define VELORA_NONBLOCKING
#endif
