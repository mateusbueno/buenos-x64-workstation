#include <cstdlib>
#include <cstring>

#include "velora/host/host_info.hpp"

int main() { return std::strcmp(velora::host::host_name(), "velora-host") == 0 ? EXIT_SUCCESS : EXIT_FAILURE; }
