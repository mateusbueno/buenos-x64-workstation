#include "gap_detector.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace velora::offline {

GapReport detect_gaps_and_truncation(const WavData& baseline,
                                    const WavData& candidate,
                                    const GapDetectorOptions& opt) noexcept {
  GapReport rep;
  if (baseline.sample_rate != candidate.sample_rate || baseline.channels != candidate.channels) {
    rep.pass = false;
    rep.message = "Format mismatch: baseline (" + std::to_string(baseline.sample_rate) + " Hz, " +
                  std::to_string(baseline.channels) + " ch) vs candidate (" +
                  std::to_string(candidate.sample_rate) + " Hz, " +
                  std::to_string(candidate.channels) + " ch)";
    return rep;
  }
  if (baseline.channels == 0 || baseline.sample_rate == 0) {
    rep.pass = false;
    rep.message = "Invalid audio: channels or sample rate is zero";
    return rep;
  }

  const std::size_t ch = baseline.channels;
  const std::uint64_t base_frames = static_cast<std::uint64_t>(baseline.samples.size() / ch);
  const std::uint64_t cand_frames = static_cast<std::uint64_t>(candidate.samples.size() / ch);
  const std::uint64_t pre_switch = std::min(opt.switch_frame, std::min(base_frames, cand_frames));

  // 1. Verify audio before switch matches baseline bit-identically (within FP roundoff).
  for (std::uint64_t f = 0; f < pre_switch; ++f) {
    for (std::size_t c = 0; c < ch; ++c) {
      const float diff = std::fabs(baseline.samples[f * ch + c] - candidate.samples[f * ch + c]);
      if (diff > 1e-4f) {
        rep.pass = false;
        rep.gap_frame = f;
        rep.gap_time_sec = static_cast<double>(f) / baseline.sample_rate;
        rep.drop_db = 0.0f;
        std::ostringstream ss;
        ss << "Audio divergence before switch at " << rep.gap_time_sec << "s (frame " << f
           << "): sample diff " << diff;
        rep.message = ss.str();
        return rep;
      }
    }
  }

  // 2. Analyze RMS envelopes across and after the switch.
  const std::uint32_t win = std::max(16u, opt.window_frames);
  const std::uint32_t hop = std::max(8u, opt.hop_frames);
  const std::uint64_t check_end = opt.check_until_frame > 0
                                      ? std::min(opt.check_until_frame, std::min(base_frames, cand_frames))
                                      : std::min(base_frames, cand_frames);

  auto calc_rms_db = [](const WavData& w, std::uint64_t start_f, std::uint32_t frames, std::size_t ch_count) -> float {
    const std::uint64_t total_f = w.samples.size() / ch_count;
    if (start_f >= total_f) return -140.0f;
    const std::uint32_t count = static_cast<std::uint32_t>(std::min(static_cast<std::uint64_t>(frames), total_f - start_f));
    if (count == 0) return -140.0f;
    double sum_sq = 0.0;
    for (std::uint32_t i = 0; i < count; ++i) {
      for (std::size_t c = 0; c < ch_count; ++c) {
        const double s = static_cast<double>(w.samples[(start_f + i) * ch_count + c]);
        sum_sq += s * s;
      }
    }
    const double rms = std::sqrt(sum_sq / static_cast<double>(count * ch_count));
    if (rms <= 1e-7) return -140.0f;
    return static_cast<float>(20.0 * std::log10(rms));
  };

  const std::uint64_t start_window = (opt.switch_frame >= win) ? (opt.switch_frame - win / 2) : 0;
  for (std::uint64_t f = start_window; f + win <= check_end; f += hop) {
    const float base_db = calc_rms_db(baseline, f, win, ch);
    const float cand_db = calc_rms_db(candidate, f, win, ch);

    if (base_db < opt.min_active_db) {
      continue;  // below active threshold, quiet/silence
    }

    // Check for early truncation: baseline note still sounding strongly, candidate went silent
    if (base_db >= -40.0f && cand_db <= opt.truncation_threshold_db) {
      rep.pass = false;
      rep.gap_frame = f;
      rep.gap_time_sec = static_cast<double>(f) / baseline.sample_rate;
      rep.baseline_rms_db = base_db;
      rep.candidate_rms_db = cand_db;
      rep.drop_db = base_db - cand_db;
      std::ostringstream ss;
      ss << "Early truncation detected at " << rep.gap_time_sec << "s (frame " << f
         << "): baseline note sounding at " << base_db << " dBFS, candidate dropped to " << cand_db << " dBFS";
      rep.message = ss.str();
      return rep;
    }

    // Check for drop in envelope (gap / dip below baseline)
    const float drop = base_db - cand_db;
    if (drop > opt.max_allowed_drop_db) {
      rep.pass = false;
      rep.gap_frame = f;
      rep.gap_time_sec = static_cast<double>(f) / baseline.sample_rate;
      rep.baseline_rms_db = base_db;
      rep.candidate_rms_db = cand_db;
      rep.drop_db = drop;
      std::ostringstream ss;
      ss << "Envelope drop / gap detected at " << rep.gap_time_sec << "s (frame " << f
         << "): candidate dipped by " << drop << " dB below baseline (baseline " << base_db
         << " dBFS, candidate " << cand_db << " dBFS)";
      rep.message = ss.str();
      return rep;
    }
  }

  rep.pass = true;
  rep.message = "No gap or truncation detected: envelope preserved smoothly across switch";
  return rep;
}

}  // namespace velora::offline
