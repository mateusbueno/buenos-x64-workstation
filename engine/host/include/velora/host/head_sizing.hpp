#pragma once
// Instrument head sizing (Story 1.16, FR4 / AR19 / AR20). Pure functions, standard library only: no OS, no I/O.
//
// A sample's HEAD is the first part of it kept decoded in locked RAM, so the attack of a note never waits for the SSD. It must last until the stream of the
// rest of the sample (Story 1.17) is certain to have arrived: the qualified SSD latency (p99.99, measured by tools/ssd-qualify on the board) plus a margin.
// During that time the voice consumes `sample_rate * pitch_ratio` frames of the sample per second, where pitch_ratio is the HIGHEST the instrument's region
// table lets that sample play at (key range, pitch_keycenter, transpose and tune of every region that uses it).
//
//   head = start_offset + ceil((p99.99 + margin) * sample_rate * max_pitch_ratio) + guard      (at least one engine block at that pitch, plus the guard)
//
// capped at the sample's length: a sample whose head would reach its end is fully resident and has no tail.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace velora::host {

struct HeadSizing {
  std::uint64_t p9999_ns = 2'000'000;  // qualified SSD read latency at p99.99 (default: the 2 ms working figure until the board run replaces it)
  std::uint64_t margin_ns = 1'000'000;
  std::uint32_t block_frames = 64;     // one engine block: the minimum head is one block at the highest pitch
  std::uint32_t guard_frames = 4;      // interpolation taps (Hermite reads idx-1 .. idx+2) and the one-frame oracle hold
};

// The highest pitch ratio (2^(cents/1200), pitch only: the sample-rate ratio is NOT in it) a region plays at: its top key, tuned and transposed.
// Clamped to [1/4096, 1024] (the engine's own bounds; a region outside them is unplayable anyway).
inline double region_max_pitch_ratio(int key_hi, int key_center, int transpose, double tune_cents) noexcept {
  const double cents = 100.0 * (static_cast<double>(key_hi) - static_cast<double>(key_center)) + tune_cents + 100.0 * static_cast<double>(transpose);
  const double r = std::exp2(cents / 1200.0);
  return std::isfinite(r) ? std::clamp(r, 1.0 / 4096.0, 1024.0) : 1024.0;
}

struct HeadLength {
  std::uint64_t frames = 0;  // frames to keep resident (decoded from frame 0)
  bool has_tail = false;     // frames < total_frames
};

// Head length in frames for a sample of `total_frames` frames at `sample_rate`, whose highest playback pitch ratio is `max_pitch_ratio` and whose regions start
// at most at frame `max_start_offset`. 0 frames for an empty sample.
inline HeadLength head_length(const HeadSizing& s, std::uint32_t sample_rate, double max_pitch_ratio, std::uint64_t total_frames, std::uint64_t max_start_offset = 0) noexcept {
  HeadLength h;
  if (total_frames == 0) return h;
  const double ratio = std::isfinite(max_pitch_ratio) && max_pitch_ratio > 0.0 ? std::min(max_pitch_ratio, 1024.0) : 1.0;
  const double seconds = static_cast<double>(s.p9999_ns + s.margin_ns) * 1e-9;
  const double latency_frames = std::ceil(seconds * static_cast<double>(sample_rate) * ratio);
  const double block_frames = std::ceil(static_cast<double>(s.block_frames) * ratio);
  const double need = static_cast<double>(max_start_offset) + std::max(latency_frames, block_frames) + static_cast<double>(s.guard_frames);
  if (!(need < static_cast<double>(total_frames))) {
    h.frames = total_frames;
    return h;
  }
  h.frames = static_cast<std::uint64_t>(need);
  h.has_tail = true;
  return h;
}

}  // namespace velora::host
