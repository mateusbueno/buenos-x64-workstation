#include <cstdlib>

#include "velora/ports/version.hpp"

int main() { return velora::ports::kPortsApiVersion == 1 ? EXIT_SUCCESS : EXIT_FAILURE; }
