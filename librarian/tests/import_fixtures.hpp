#pragma once
// Tiny deterministic source libraries for the Story 5.6 tests (nothing is committed): mono 16-bit WAVs of a few hundred frames, SFZ files that
// use them, a broken SFZ of each skip reason and General-MIDI-style SF2 files from the test-only writer formatlib/tests/sf2_builder.hpp.
#include <functional>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "sf2_builder.hpp"

namespace ifx {

namespace fs = std::filesystem;

inline void put(const fs::path& p, const std::string& bytes) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

inline std::string wav16(std::uint32_t frames, std::uint32_t seed) {
  std::string pcm;
  std::uint32_t x = seed * 2654435761u + 12345u;
  for (std::uint32_t i = 0; i < frames; ++i) {
    x = x * 1664525u + 1013904223u;
    const std::int16_t v = static_cast<std::int16_t>((x >> 16) & 0x3fff) - 0x2000;
    pcm.push_back(static_cast<char>(v & 0xff));
    pcm.push_back(static_cast<char>((v >> 8) & 0xff));
  }
  auto le32 = [](std::uint32_t v) { return std::string{char(v), char(v >> 8), char(v >> 16), char(v >> 24)}; };
  auto le16 = [](std::uint32_t v) { return std::string{char(v), char(v >> 8)}; };
  std::string b = "RIFF" + le32(static_cast<std::uint32_t>(36 + pcm.size())) + "WAVE" + "fmt " + le32(16) + le16(1) + le16(1) + le32(48000) + le32(96000) +
                  le16(2) + le16(16) + "data" + le32(static_cast<std::uint32_t>(pcm.size()));
  return b + pcm;
}

// <dir>/<name>.sfz with one sample <dir>/<name>_s.wav. `variant` changes the PCM (a changed source: same library, new bundle).
inline fs::path write_sfz(const fs::path& dir, const std::string& name, unsigned variant = 0, std::uint32_t frames = 300) {
  put(dir / (name + "_s.wav"), wav16(frames, static_cast<std::uint32_t>(std::hash<std::string>{}(name)) + variant * 7919u));
  put(dir / (name + ".sfz"), "<region> sample=" + name + "_s.wav key=60 lokey=48 hikey=72\n");
  return dir / (name + ".sfz");
}

// Skipped for `damaged`: the sample is missing.
inline fs::path write_damaged_sfz(const fs::path& dir, const std::string& name) {
  put(dir / (name + ".sfz"), "<region> sample=missing_" + name + ".wav key=60\n");
  return dir / (name + ".sfz");
}

// Skipped for `unsupported_features`: amp_random is outside the opcode subset.
inline fs::path write_unsupported_sfz(const fs::path& dir, const std::string& name) {
  put(dir / (name + "_s.wav"), wav16(200, 5));
  put(dir / (name + ".sfz"), "<region> sample=" + name + "_s.wav key=60 amp_random=3\n");
  return dir / (name + ".sfz");
}

// A General-MIDI-style SF2: 6 presets "0:0" "0:1" "0:48" "8:0" "128:0" "1:5".
inline fs::path write_sf2(const fs::path& path, unsigned variant = 0) {
  sf2b::Fixture f = sf2b::make_gm(false);
  if (variant) f.b.samples[0].pcm[10] = static_cast<std::int32_t>(f.b.samples[0].pcm[10] ^ static_cast<std::int32_t>(variant * 0x40));
  const auto bytes = f.b.build();
  put(path, std::string(bytes.begin(), bytes.end()));
  return path;
}

// Story 5.8: a SoundFont with `n` presets "0:0".."0:n-1", each its own instrument with `per` mono samples of `frames` frames, one per key band (preset k = timbre k).
// `changed` (1-based, 0 = none) alters one sample point of that timbre's first sample: the same library, a changed timbre.
inline fs::path write_many_sf2(const fs::path& path, unsigned n, unsigned changed = 0, std::size_t frames = 2200, unsigned per = 1) {
  sf2b::Builder b;
  for (unsigned i = 0; i < n; ++i) {
    sf2b::Instrument ins;
    ins.name = "I" + std::to_string(i);
    for (unsigned j = 0; j < per; ++j) {
      sf2b::Sample s;
      s.name = "smp" + std::to_string(i) + "_" + std::to_string(j);
      s.pcm = sf2b::wave(frames, 3 + static_cast<int>(i + j), false, 2 + static_cast<int>((i + j) % 3));
      if (changed == i + 1 && j == 0) s.pcm[7] = static_cast<std::int32_t>(s.pcm[7] ^ 0x55);
      b.samples.push_back(std::move(s));
      const int idx = static_cast<int>(i * per + j);
      if (per == 1) ins.zones.push_back(sf2b::izone(idx));
      else ins.zones.push_back(sf2b::izone(idx, {sf2b::range(sf2b::keyRange, static_cast<int>(j * 30), static_cast<int>(j * 30 + 29))}));
    }
    b.instruments.push_back(std::move(ins));
    sf2b::add_preset(b, ("Timbre " + std::to_string(i)).c_str(), 0, static_cast<int>(i), {sf2b::pzone(static_cast<int>(i))});
  }
  const auto bytes = b.build();
  put(path, std::string(bytes.begin(), bytes.end()));
  return path;
}

}  // namespace ifx
