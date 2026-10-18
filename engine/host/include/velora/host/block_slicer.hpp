#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include "velora/core/block_renderer.hpp"
#include "velora/host/time_mapper.hpp"
#include "velora/ports/audio_output.hpp"

namespace velora::host {

// Adapts a device callback of any size to the engine's fixed 64-frame blocks. render_block() is
// called exactly once per 64 output frames; leftover frames of a block carry to the next callback.
// A block's start time is the callback's t0 (when output frame 0 is heard) plus the position of
// the block's first frame within that callback. Real-time safe.
class BlockSlicer {
 public:
  explicit BlockSlicer(std::uint32_t sample_rate = core::kSampleRate) noexcept : rate_(sample_rate) {}

  void reset() noexcept { pos_ = core::kBlockFrames; }

  void process(ports::BlockSink& sink, float* out_l, float* out_r, std::uint32_t frames,
               std::uint64_t t0_ns) noexcept VELORA_NONBLOCKING {
    std::uint32_t done = 0;
    while (done < frames) {
      if (pos_ >= core::kBlockFrames) {
        sink.render_block(l_.data(), r_.data(), t0_ns + frames_to_ns(done, rate_));
        pos_ = 0;
      }
      const std::uint32_t n = std::min<std::uint32_t>(frames - done, core::kBlockFrames - pos_);
      std::memcpy(out_l + done, l_.data() + pos_, n * sizeof(float));
      std::memcpy(out_r + done, r_.data() + pos_, n * sizeof(float));
      pos_ += n;
      done += n;
    }
  }

 private:
  std::uint32_t rate_;
  std::array<float, core::kBlockFrames> l_{};
  std::array<float, core::kBlockFrames> r_{};
  std::uint32_t pos_ = core::kBlockFrames;  // == kBlockFrames: nothing carried
};

}  // namespace velora::host
