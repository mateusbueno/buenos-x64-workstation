#pragma once

#include <cstdint>
#include <string>

#include "velora/ports/nonblocking.hpp"

namespace velora::ports {

// Receives audio blocks on the device's real-time thread. Implementations must not allocate,
// lock or block. `block_start_ns` is the device-clock time (nanoseconds, same clock the MIDI
// adapter stamps events with) at which the first frame of this block will be heard.
class BlockSink {
 public:
  virtual ~BlockSink() = default;
  virtual void render_block(float* left, float* right, std::uint64_t block_start_ns) noexcept VELORA_NONBLOCKING = 0;
};

// What the device actually granted (may differ from the request).
struct AudioOutputInfo {
  std::uint32_t sample_rate = 0;
  std::uint32_t buffer_frames = 0;
};

// Audio output device. Fixed block size is rendered by the sink (see engine block size).
class AudioOutput {
 public:
  virtual ~AudioOutput() = default;
  // Opens the default output at the requested rate/buffer size; returns false and fills `error`
  // if no device exists or the format cannot be set.
  virtual bool open(std::uint32_t sample_rate, std::uint32_t buffer_frames, BlockSink& sink,
                    std::string& error) = 0;
  virtual bool start(std::string& error) = 0;
  virtual void stop() = 0;
  virtual AudioOutputInfo info() const = 0;
};

}  // namespace velora::ports
