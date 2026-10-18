#include "coreaudio_output.hpp"

#include <CoreAudio/CoreAudio.h>

#include <chrono>
#include <cstring>
#include <thread>

#include "mach_time.hpp"
#include "velora/host/time_mapper.hpp"

namespace velora::host::macos {

namespace {
std::string os_err(const char* what, OSStatus s) { return std::string(what) + " failed (OSStatus " + std::to_string(s) + ")"; }
}  // namespace

CoreAudioOutput::~CoreAudioOutput() {
  stop();
  if (unit_) {
    AudioUnitUninitialize(unit_);
    AudioComponentInstanceDispose(unit_);
    unit_ = nullptr;
  }
}

bool CoreAudioOutput::open(std::uint32_t sample_rate, std::uint32_t buffer_frames,
                           ports::BlockSink& sink, std::string& error) {
  sink_ = &sink;
  if (unit_) {  // repeated open(): release the previous unit
    stop();
    AudioUnitUninitialize(unit_);
    AudioComponentInstanceDispose(unit_);
    unit_ = nullptr;
  }
  AudioDeviceID device = kAudioObjectUnknown;
  UInt32 size = sizeof(device);
  AudioObjectPropertyAddress addr{kAudioHardwarePropertyDefaultOutputDevice,
                                  kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
  OSStatus s = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, &device);
  if (s != noErr || device == kAudioObjectUnknown) {
    error = "no default audio output device";
    return false;
  }

  // Ask the device for the nominal rate and buffer size; read back what was granted.
  addr = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
          kAudioObjectPropertyElementMain};
  auto read_rate = [&]() {
    Float64 r = 0;
    UInt32 sz = sizeof(r);
    if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &sz, &r) != noErr) return 0u;
    return static_cast<std::uint32_t>(r + 0.5);
  };
  std::uint32_t got = read_rate();
  if (got != sample_rate) {
    Float64 want = sample_rate;
    s = AudioObjectSetPropertyData(device, &addr, 0, nullptr, sizeof(want), &want);
    if (s != noErr) {
      error = os_err("setting device sample rate", s);
      return false;
    }
    for (int i = 0; i < 50 && got != sample_rate; ++i) {  // the change is asynchronous; up to ~500 ms
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      got = read_rate();
    }
  }
  if (got != sample_rate) {
    error = "output device does not run at " + std::to_string(sample_rate) + " Hz (it reports " +
            std::to_string(got) + " Hz); engine never resamples";
    return false;
  }
  addr.mSelector = kAudioDevicePropertyBufferFrameSize;
  UInt32 frames = buffer_frames;
  // Not fatal if refused: the device keeps its size and info() reports it.
  AudioObjectSetPropertyData(device, &addr, 0, nullptr, sizeof(frames), &frames);
  size = sizeof(frames);
  frames = 0;
  s = AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, &frames);
  if (s != noErr || frames == 0) {
    error = os_err("reading device buffer size", s);
    return false;
  }
  info_.sample_rate = sample_rate;
  info_.buffer_frames = frames;

  AudioComponentDescription desc{};
  desc.componentType = kAudioUnitType_Output;
  desc.componentSubType = kAudioUnitSubType_HALOutput;
  desc.componentManufacturer = kAudioUnitManufacturer_Apple;
  AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
  if (!comp) {
    error = "HAL output audio component not found";
    return false;
  }
  s = AudioComponentInstanceNew(comp, &unit_);
  if (s != noErr) {
    error = os_err("creating output AudioUnit", s);
    return false;
  }
  s = AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0,
                           &device, sizeof(device));
  if (s != noErr) {
    error = os_err("selecting output device", s);
    return false;
  }

  AudioStreamBasicDescription fmt{};
  fmt.mSampleRate = sample_rate;
  fmt.mFormatID = kAudioFormatLinearPCM;
  fmt.mFormatFlags = static_cast<UInt32>(kAudioFormatFlagsNativeFloatPacked) | static_cast<UInt32>(kAudioFormatFlagIsNonInterleaved);
  fmt.mChannelsPerFrame = 2;
  fmt.mBitsPerChannel = 32;
  fmt.mBytesPerFrame = 4;
  fmt.mBytesPerPacket = 4;
  fmt.mFramesPerPacket = 1;
  s = AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt,
                           sizeof(fmt));
  if (s != noErr) {
    error = os_err("setting 48 kHz stereo float format", s);
    return false;
  }

  AURenderCallbackStruct cb{&CoreAudioOutput::render_cb, this};
  s = AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb,
                           sizeof(cb));
  if (s != noErr) {
    error = os_err("installing render callback", s);
    return false;
  }
  s = AudioUnitInitialize(unit_);
  if (s != noErr) {
    error = os_err("initialising output AudioUnit", s);
    return false;
  }
  return true;
}

bool CoreAudioOutput::start(std::string& error) {
  if (!unit_) {
    error = "output not open";
    return false;
  }
  slicer_.reset();
  const OSStatus s = AudioOutputUnitStart(unit_);
  if (s != noErr) {
    error = os_err("starting output", s);
    return false;
  }
  started_ = true;
  return true;
}

void CoreAudioOutput::stop() {
  if (unit_ && started_) AudioOutputUnitStop(unit_);
  started_ = false;
}

OSStatus CoreAudioOutput::render_cb(void* ref, AudioUnitRenderActionFlags*, const AudioTimeStamp* ts,
                                    UInt32, UInt32 frames, AudioBufferList* io) {
  return static_cast<CoreAudioOutput*>(ref)->render(ts, frames, io);
}

OSStatus CoreAudioOutput::render(const AudioTimeStamp* ts, UInt32 frames, AudioBufferList* io) noexcept {
  callbacks_.fetch_add(1, std::memory_order_relaxed);
  if (!io) return noErr;
  const UInt32 bytes = frames * sizeof(float);
  const bool usable = io->mNumberBuffers >= 2 && io->mBuffers[0].mData && io->mBuffers[1].mData &&
                      io->mBuffers[0].mDataByteSize >= bytes && io->mBuffers[1].mDataByteSize >= bytes;
  if (!usable) {  // unexpected layout or short buffers: emit silence
    for (UInt32 b = 0; b < io->mNumberBuffers; ++b)
      if (io->mBuffers[b].mData) std::memset(io->mBuffers[b].mData, 0, io->mBuffers[b].mDataByteSize);
    return noErr;
  }
  // Time at which output frame 0 of this callback is heard.
  const bool have_host = ts && (ts->mFlags & kAudioTimeStampHostTimeValid);
  const std::uint64_t t0 = have_host ? host_ticks_to_ns(ts->mHostTime) : now_ns();
  slicer_.process(*sink_, static_cast<float*>(io->mBuffers[0].mData),
                  static_cast<float*>(io->mBuffers[1].mData), frames, t0);
  return noErr;
}

}  // namespace velora::host::macos
