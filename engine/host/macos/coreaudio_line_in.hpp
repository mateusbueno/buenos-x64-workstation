#pragma once

#include <AudioToolbox/AudioToolbox.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "velora/ports/line_in_source.hpp"

namespace velora::host::macos {

// Optional line-in (Story 2.7): the default CoreAudio input device, captured as 48 kHz stereo float (a mono device is spread to
// both channels by the audio unit) into a lock-free ring that the engine drains with read_block() on its render thread. The
// capture and the playback callbacks run on different clocks, so the ring keeps a small jitter buffer and drops/pads (counted in
// underruns()/drops()) instead of ever blocking.
//
// The input must run at the engine rate: a device whose input rate differs (the HAL does not resample captured audio) makes
// open() fail with a clear message and the caller keeps the silent port. A mono device is duplicated onto both channels.
// Everything is optional: open() fails with a message when there is no input device or microphone access is refused, and the
// caller keeps the default silent port. A line-in that was never opened (or is not started) reads as digital silence.
// macOS asks for microphone permission the first time an input is opened; engine-host therefore opens it only with --line-in.
class CoreAudioLineIn final : public ports::LineInSource {
 public:
  CoreAudioLineIn();
  ~CoreAudioLineIn() override;
  CoreAudioLineIn(const CoreAudioLineIn&) = delete;
  CoreAudioLineIn& operator=(const CoreAudioLineIn&) = delete;

  bool open(std::uint32_t sample_rate, std::string& error);
  bool start(std::string& error);
  void stop();
  bool running() const noexcept { return started_.load(std::memory_order_acquire); }

  void read_block(float* left, float* right, std::uint32_t frames) noexcept override;

  std::uint64_t captured_frames() const noexcept { return wr_.load(std::memory_order_relaxed); }
  std::uint64_t underruns() const noexcept { return underruns_.load(std::memory_order_relaxed); }
  std::uint64_t drops() const noexcept { return drops_.load(std::memory_order_relaxed); }
  std::uint64_t dropped_frames() const noexcept { return dropped_frames_.load(std::memory_order_relaxed); }

 private:
  static constexpr std::uint32_t kRingFrames = 16384;  // power of two
  static constexpr std::uint32_t kMaxSlice = 4096;     // largest input callback handled
  static constexpr std::uint32_t kPrefill = 256;       // frames buffered before the first read
  static constexpr std::uint32_t kMaxBuffered = 4096;  // above this the oldest frames are dropped

  static OSStatus input_cb(void* ref, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts, UInt32 bus,
                           UInt32 frames, AudioBufferList* io);
  OSStatus capture(AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts, UInt32 bus, UInt32 frames) noexcept;

  AudioUnit unit_ = nullptr;
  std::atomic<bool> started_{false};
  UInt32 channels_ = 2;  // channels the unit delivers (1 for a mono device: channel 0 is duplicated)
  std::vector<float> ring_l_, ring_r_;
  std::vector<float> slice_l_, slice_r_;
  alignas(AudioBufferList) unsigned char abl_mem_[sizeof(AudioBufferList) + sizeof(AudioBuffer)]{};
  alignas(64) std::atomic<std::uint64_t> wr_{0};
  alignas(64) std::atomic<std::uint64_t> rd_{0};
  bool primed_ = false;  // reader only
  std::atomic<std::uint64_t> underruns_{0}, drops_{0}, dropped_frames_{0};
};

}  // namespace velora::host::macos
