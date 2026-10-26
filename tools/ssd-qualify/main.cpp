#include <iostream>
#include <string>
#include <vector>

#include "ssd_qualify.hpp"

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  return velora::ssd_qualify::cli_main(args, std::cout, std::cerr);
}
