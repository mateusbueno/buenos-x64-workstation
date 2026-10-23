#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "wav.hpp"

namespace velora::offline {

// Report produced by the gap and truncation detector (Story 4.8).
struct GapReport {
  bool pass = true;
  std::string message;
  std::uint64_t gap_frame = 0;
  double gap_time_sec = 0.0;
  float baseline_rms_db = 0.0f;
  float candidate_rms_db = 0.0f;
  float drop_db = 0.0f;
};

struct GapDetectorOptions {
  std::uint64_t switch_frame = 0;
  std::uint64_t check_until_frame = 0;    // 0 = until end of audio
  float min_active_db = -60.0f;           // baseline RMS level (dBFS) to consider active
  float max_allowed_drop_db = 6.0f;       // max allowable envelope drop below baseline
  float truncation_threshold_db = -75.0f; // level below which a signal is considered ended
  std::uint32_t window_frames = 240;      // 5 ms at 48 kHz
  std::uint32_t hop_frames = 120;         // 2.5 ms at 48 kHz
};

// Inspects candidate audio against baseline audio for drops in envelope or early truncation of sounding notes.
GapReport detect_gaps_and_truncation(const WavData& baseline,
                                    const WavData& candidate,
                                    const GapDetectorOptions& opt = {}) noexcept;

}  // namespace velora::offline
