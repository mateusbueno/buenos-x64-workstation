#pragma once
// Minimal RIFF/WAVE reader that feeds BundleWriter: PCM 16/24-bit and IEEE float32, mono/stereo.
// No resampling or conversion: values are passed through as stored.
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "velora/format/bundle.hpp"

namespace velora::format {

struct WavData {
  PcmFormat format;
  std::uint64_t frame_count = 0;
  std::vector<std::int32_t> data;  // interleaved, PCM exchange convention (see bundle.hpp)

  // The returned SampleInput views `data`: do not call on a temporary.
  SampleInput as_input(std::string name) const& {
    return SampleInput{std::move(name), format, frame_count, data};
  }
  SampleInput as_input(std::string) const&& = delete;
};

// Accepts WAVE_FORMAT_PCM, WAVE_FORMAT_IEEE_FLOAT and WAVE_FORMAT_EXTENSIBLE wrapping them.
// Errors: kBadMagic (not RIFF/WAVE), kTruncated, kUnsupportedFormat, kCorruptHeader.
Result<WavData> parse_wav(std::span<const std::uint8_t> bytes);
Result<WavData> load_wav_file(const std::string& path);

}  // namespace velora::format
