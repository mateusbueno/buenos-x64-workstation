#include <cstdlib>

#include "velora/core/version.hpp"

int main() { return velora::core::core_version() == 1 ? EXIT_SUCCESS : EXIT_FAILURE; }
