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
//
// By default a data chunk that claims more bytes than the file holds is clamped to what is there
// (streamed WAVs). With `reject_truncated_data` such a file is kTruncated, except for the
// "unknown length" marker 0xFFFFFFFF: the importer (Story 5.1) sets it so a cut-off sample file is
// reported as damaged instead of being imported shorter than its source.
struct WavOptions {
  bool reject_truncated_data = false;
};
Result<WavData> parse_wav(std::span<const std::uint8_t> bytes, WavOptions opt = {});
Result<WavData> parse_flac(std::span<const std::uint8_t> bytes);
Result<WavData> load_wav_file(const std::string& path, WavOptions opt = {});

}  // namespace velora::format
