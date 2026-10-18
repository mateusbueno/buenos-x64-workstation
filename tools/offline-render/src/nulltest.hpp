#pragma once

#include <cstdint>
#include <string>

#include "wav.hpp"

namespace velora::offline {

constexpr double kFloorDb = -300.0;  // reported for an exactly-zero residual

struct NullParams {
  double tolerance_db = -90.0;
  std::uint32_t max_offset_frames = 0;
  std::uint32_t window_frames = 4800;  // 100 ms at 48 kHz
  std::uint64_t max_length_diff_frames = UINT64_MAX;  // |candidate - reference| length; default unlimited
};

struct NullResult {
  bool pass = false;
  std::uint32_t sample_rate = 48000;
  bool comparable = false;  // false: format/empty mismatch, nothing was measured
  std::string message;
  double residual_db = 0.0;       // 10*log10(residual energy / reference energy), overlap only
  std::int64_t offset_frames = 0; // candidate[i + offset] aligned with reference[i]
  double worst_window_db = 0.0;   // worst 100 ms window vs. the reference's mean window energy
  std::uint64_t worst_window_start = 0;
  std::uint64_t overlap_frames = 0;
  std::int64_t length_diff_frames = 0;  // candidate frames - reference frames
};

// Tries every integer offset in [-max_offset, +max_offset] (no search beyond) and keeps the one with
// the smallest residual. Interleaved channels are treated as one signal.
NullResult null_test(const WavData& ref, const WavData& cand, const NullParams& p);

std::string format_report(const std::string& candidate_name, const NullResult& r, const NullParams& p);

}  // namespace velora::offline
