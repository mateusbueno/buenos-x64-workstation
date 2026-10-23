#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "fm_fixtures.hpp"
#include "velora/format/sysex_6op.hpp"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: make_fixtures <output_dir>\n";
    return 2;
  }

  const fs::path out_dir = argv[1];
  fs::create_directories(out_dir);

  auto cases = fmsuite::make_cases();
  for (const auto& c : cases) {
    // 1. Encode single SysEx voice dump
    std::vector<uint8_t> syx = velora::format::encode_sysex_single_dump(c.voice, 0);
    const fs::path syx_path = out_dir / (c.name + ".syx");
    std::ofstream syx_file(syx_path, std::ios::binary);
    syx_file.write(reinterpret_cast<const char*>(syx.data()), static_cast<std::streamsize>(syx.size()));

    // 2. Encode Standard MIDI File
    std::vector<uint8_t> mid = c.midi.smf(375); // 0.5s tail ticks
    const fs::path mid_path = out_dir / (c.name + ".mid");
    std::ofstream mid_file(mid_path, std::ios::binary);
    mid_file.write(reinterpret_cast<const char*>(mid.data()), static_cast<std::streamsize>(mid.size()));

    std::cout << "Emitted " << c.name << " (.syx + .mid)\n";
  }

  return 0;
}
