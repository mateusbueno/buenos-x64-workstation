#pragma once

#include <cstdint>

#include "velora/ports/nonblocking.hpp"

namespace velora::ports {

// Line-in port (Story 2.7, FR40 partial): a stereo source the engine mixes into the master only, at the line_in_level parameter.
// The engine calls read_block() once per 64-frame block on the real-time thread, in step with its own output; an adapter
// that captures from a device in another thread hands the audio over through its own lock-free buffer and returns silence for
// whatever it does not have yet. read_block() must not allocate, lock or block. The dev board has no line-in until Epic 3, and
// the macOS adapter is optional: both use SilentLineIn (or no source at all).
class LineInSource {
 public:
  virtual ~LineInSource() = default;
  // Overwrites left[0..frames) and right[0..frames) with the next `frames` input frames (planar float, 48 kHz).
  virtual void read_block(float* left, float* right, std::uint32_t frames) noexcept VELORA_NONBLOCKING = 0;
};

// The default port: digital silence.
class SilentLineIn final : public LineInSource {
 public:
  void read_block(float* left, float* right, std::uint32_t frames) noexcept VELORA_NONBLOCKING override {
    for (std::uint32_t i = 0; i < frames; ++i) left[i] = right[i] = 0.0f;
  }
};

}  // namespace velora::ports
