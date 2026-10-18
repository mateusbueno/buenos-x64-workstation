#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace velora::offline {

struct NoteEvent {
  std::uint64_t tick = 0;
  std::uint64_t frame = 0;  // absolute frame at 48 kHz from the fixed tempo map
  bool on = false;
  std::uint8_t channel = 0;
  std::uint8_t note = 0;
  std::uint16_t velocity16 = 0;  // 0 for note-off with no release velocity
};

struct SmfResult {
  bool ok = false;
  std::string error;
  std::uint16_t format = 0;
  std::uint16_t ppq = 0;
  std::vector<NoteEvent> events;  // time ordered (tick, then track, then file order)
};

// Parses a Standard MIDI File (format 0/1, PPQ division). Only note on/off and tempo meta events
// are kept. Frames use integer arithmetic only: frames = floor(sum(dticks*tempo_us) * 48000 /
// (ppq * 1e6)), so results are exact and identical on every platform.
SmfResult parse_smf(const std::uint8_t* data, std::size_t size, std::uint32_t sample_rate = 48000);
SmfResult parse_smf(const std::vector<std::uint8_t>& bytes, std::uint32_t sample_rate = 48000);
SmfResult load_smf_file(const std::string& path);

}  // namespace velora::offline
