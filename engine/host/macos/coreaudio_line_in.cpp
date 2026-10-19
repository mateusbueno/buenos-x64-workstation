#include "coreaudio_line_in.hpp"

#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <cassert>
#include <cstring>

namespace velora::host::macos {

namespace {
std::string os_err(const char* what, OSStatus s) { return std::string(what) + " failed (OSStatus " + std::to_string(s) + ")"; }
}  // namespace

CoreAudioLineIn::CoreAudioLineIn()
    : ring_l_(kRingFrames, 0.0f), ring_r_(kRingFrames, 0.0f), slice_l_(kMaxSlice, 0.0f), slice_r_(kMaxSlice, 0.0f) {}

CoreAudioLineIn::~CoreAudioLineIn() {
  stop();
  if (unit_) {
    AudioUnitUninitialize(unit_);
    AudioComponentInstanceDispose(unit_);
    unit_ = nullptr;
  }
}

bool CoreAudioLineIn::open(std::uint32_t sample_rate, std::string& error) {
  if (unit_) {
    error = "line-in already open";
    return false;
  }
  AudioDeviceID device = kAudioObjectUnknown;
  UInt32 size = sizeof(device);
  AudioObjectPropertyAddress addr{kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal,
                                  kAudioObjectPropertyElementMain};
  OSStatus s = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, &device);
  if (s != noErr || device == kAudioObjectUnknown) {
    error = "no default audio input device";
    return false;
  }
  AudioComponentDescription desc{};
  desc.componentType = kAudioUnitType_Output;
  desc.componentSubType = kAudioUnitSubType_HALOutput;
  desc.componentManufacturer = kAudioUnitManufacturer_Apple;
  AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
  if (!comp) {
    error = "HAL audio component not found";
    return false;
  }
  s = AudioComponentInstanceNew(comp, &unit_);
  if (s != noErr) {
    error = os_err("creating input AudioUnit", s);
    unit_ = nullptr;
    return false;
  }
  auto fail = [&](const char* what, OSStatus st) {
    error = os_err(what, st);
    AudioComponentInstanceDispose(unit_);
    unit_ = nullptr;
    return false;
  };
  UInt32 on = 1, off = 0;
  s = AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &on, sizeof(on));
  if (s != noErr) return fail("enabling input", s);
  s = AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &off, sizeof(off));
  if (s != noErr) return fail("disabling output", s);
  s = AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &device, sizeof(device));
  if (s != noErr) return fail("selecting input device", s);

  AudioStreamBasicDescription dev{};
  UInt32 dev_size = sizeof(dev);
  s = AudioUnitGetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1, &dev, &dev_size);
  if (s != noErr) return fail("reading the input device format", s);
  if (static_cast<std::uint32_t>(dev.mSampleRate + 0.5) != sample_rate) {
    error = "input device runs at " + std::to_string(static_cast<std::uint32_t>(dev.mSampleRate + 0.5)) + " Hz, the engine at " +
            std::to_string(sample_rate) + " Hz (captured audio is never resampled); set the input device to 48 kHz";
    AudioComponentInstanceDispose(unit_);
    unit_ = nullptr;
    return false;
  }
  channels_ = dev.mChannelsPerFrame == 1 ? 1 : 2;
  AudioStreamBasicDescription fmt{};
  fmt.mSampleRate = sample_rate;
  fmt.mFormatID = kAudioFormatLinearPCM;
  fmt.mFormatFlags = static_cast<UInt32>(kAudioFormatFlagsNativeFloatPacked) | static_cast<UInt32>(kAudioFormatFlagIsNonInterleaved);
  fmt.mChannelsPerFrame = channels_;
  fmt.mBitsPerChannel = 32;
  fmt.mBytesPerFrame = 4;
  fmt.mBytesPerPacket = 4;
  fmt.mFramesPerPacket = 1;
  s = AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &fmt, sizeof(fmt));
  if (s != noErr) return fail("setting 48 kHz stereo float input format", s);

  AURenderCallbackStruct cb{&CoreAudioLineIn::input_cb, this};
  s = AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &cb, sizeof(cb));
  if (s != noErr) return fail("installing input callback", s);
  s = AudioUnitInitialize(unit_);
  if (s != noErr) return fail("initialising input AudioUnit", s);
  return true;
}

bool CoreAudioLineIn::start(std::string& error) {
  if (!unit_) {
    error = "line-in not open";
    return false;
  }
  assert(!started_.load() && "CoreAudioLineIn::start() while running");
  if (started_.load()) {
    error = "line-in already running";
    return false;
  }
  wr_.store(0, std::memory_order_relaxed);
  rd_.store(0, std::memory_order_relaxed);
  primed_ = false;
  const OSStatus s = AudioOutputUnitStart(unit_);
  if (s != noErr) {
    error = os_err("starting input", s);
    return false;
  }
  started_.store(true, std::memory_order_release);
  return true;
}

void CoreAudioLineIn::stop() {
  if (unit_ && started_.load()) AudioOutputUnitStop(unit_);
  started_.store(false, std::memory_order_release);
}

OSStatus CoreAudioLineIn::input_cb(void* ref, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts, UInt32 bus,
                                   UInt32 frames, AudioBufferList*) {
  return static_cast<CoreAudioLineIn*>(ref)->capture(flags, ts, bus, frames);
}

OSStatus CoreAudioLineIn::capture(AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts, UInt32 bus,
                                  UInt32 frames) noexcept {
  AudioTimeStamp t = *ts;
  while (frames > 0) {  // a callback larger than the slice buffers is captured in chunks
    const UInt32 n = std::min<UInt32>(frames, kMaxSlice);
    auto* abl = reinterpret_cast<AudioBufferList*>(abl_mem_);
    abl->mNumberBuffers = channels_;
    abl->mBuffers[0].mNumberChannels = 1;
    abl->mBuffers[0].mDataByteSize = n * sizeof(float);
    abl->mBuffers[0].mData = slice_l_.data();
    if (channels_ == 2) {
      abl->mBuffers[1].mNumberChannels = 1;
      abl->mBuffers[1].mDataByteSize = n * sizeof(float);
      abl->mBuffers[1].mData = slice_r_.data();
    }
    const OSStatus s = AudioUnitRender(unit_, flags, &t, bus, n, abl);
    if (s != noErr) {  // (no permission or the device went away: nothing is captured, the reader pads with silence)
      dropped_frames_.fetch_add(frames, std::memory_order_relaxed);
      return s;
    }
    const std::uint64_t w = wr_.load(std::memory_order_relaxed);
    const std::uint64_t r = rd_.load(std::memory_order_acquire);
    if (w - r + n > kRingFrames) {  // the reader is not draining: drop this chunk
      drops_.fetch_add(1, std::memory_order_relaxed);
      dropped_frames_.fetch_add(n, std::memory_order_relaxed);
    } else {
      const std::vector<float>& right = channels_ == 2 ? slice_r_ : slice_l_;  // mono: channel 0 on both
      for (UInt32 i = 0; i < n; ++i) {
        ring_l_[(w + i) & (kRingFrames - 1)] = slice_l_[i];
        ring_r_[(w + i) & (kRingFrames - 1)] = right[i];
      }
      wr_.store(w + n, std::memory_order_release);
    }
    frames -= n;
    t.mSampleTime += n;
  }
  return noErr;
}

void CoreAudioLineIn::read_block(float* left, float* right, std::uint32_t frames) noexcept {
  auto silence = [&] {
    for (std::uint32_t i = 0; i < frames; ++i) left[i] = right[i] = 0.0f;
  };
  if (!started_.load(std::memory_order_acquire)) {
    silence();
    return;
  }
  const std::uint64_t w = wr_.load(std::memory_order_acquire);
  std::uint64_t r = rd_.load(std::memory_order_relaxed);
  std::uint64_t avail = w - r;
  if (!primed_) {
    if (avail < kPrefill) {
      silence();
      return;
    }
    primed_ = true;
  }
  if (avail > kMaxBuffered) {  // the capture clock runs ahead: drop the oldest frames, keep a short jitter buffer
    r += avail - kPrefill;
    avail = kPrefill;
    drops_.fetch_add(1, std::memory_order_relaxed);
  }
  if (avail < frames) {  // the capture clock runs behind: pad with silence and re-prime
    underruns_.fetch_add(1, std::memory_order_relaxed);
    primed_ = false;
    rd_.store(r, std::memory_order_release);
    silence();
    return;
  }
  for (std::uint32_t i = 0; i < frames; ++i) {
    left[i] = ring_l_[(r + i) & (kRingFrames - 1)];
    right[i] = ring_r_[(r + i) & (kRingFrames - 1)];
  }
  rd_.store(r + frames, std::memory_order_release);
}

}  // namespace velora::host::macos
