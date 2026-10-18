#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace velora::offline {

struct WavData {
  std::uint32_t sample_rate = 48000;
  std::uint16_t channels = 2;
  std::vector<float> samples;  // interleaved
  std::uint64_t frames() const { return channels ? samples.size() / channels : 0; }
};

// 32-bit IEEE float PCM WAV (format tag 3). Little-endian on disk regardless of host.
bool write_wav(const std::string& path, const WavData& wav, std::string& error);
bool read_wav(const std::string& path, WavData& wav, std::string& error);
std::vector<std::uint8_t> encode_wav(const WavData& wav);
bool decode_wav(const std::vector<std::uint8_t>& bytes, WavData& wav, std::string& error);

}  // namespace velora::offline
