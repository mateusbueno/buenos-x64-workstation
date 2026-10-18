#pragma once

#include <AudioToolbox/AudioToolbox.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "velora/host/block_slicer.hpp"
#include "velora/ports/audio_output.hpp"

namespace velora::host::macos {

using host::BlockSlicer;

// CoreAudio (HAL output AudioUnit) adapter: 48 kHz stereo float, requested 64-frame device buffer.
// The device may grant a different buffer size; info() reports what it granted. The engine always
// renders fixed 64-frame blocks; if the device buffer is larger or not a multiple of 64 the
// leftover frames of a block are carried to the next callback (see BlockSlicer).
class CoreAudioOutput final : public ports::AudioOutput {
 public:
  CoreAudioOutput() = default;
  ~CoreAudioOutput() override;
  CoreAudioOutput(const CoreAudioOutput&) = delete;
  CoreAudioOutput& operator=(const CoreAudioOutput&) = delete;

  bool open(std::uint32_t sample_rate, std::uint32_t buffer_frames, ports::BlockSink& sink,
            std::string& error) override;
  bool start(std::string& error) override;
  void stop() override;
  ports::AudioOutputInfo info() const override { return info_; }

  std::uint64_t callbacks() const noexcept { return callbacks_.load(std::memory_order_relaxed); }

 private:
  static OSStatus render_cb(void* ref, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32,
                            UInt32 frames, AudioBufferList* io);
  OSStatus render(const AudioTimeStamp* ts, UInt32 frames, AudioBufferList* io) noexcept;

  AudioUnit unit_ = nullptr;
  bool started_ = false;
  ports::BlockSink* sink_ = nullptr;
  ports::AudioOutputInfo info_{};
  BlockSlicer slicer_;
  std::atomic<std::uint64_t> callbacks_{0};
};

}  // namespace velora::host::macos
