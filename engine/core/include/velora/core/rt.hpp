#pragma once

// Marks functions that run on the real-time render path. Expands to clang's
// [[clang::nonblocking]] where supported so RTSan / -Wfunction-effects can
// check them; empty on gcc and older Apple clang so those still build.
#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::nonblocking)
#define VELORA_NONBLOCKING [[clang::nonblocking]]
#endif
#endif
#ifndef VELORA_NONBLOCKING
#define VELORA_NONBLOCKING
#endif
