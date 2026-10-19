#pragma once
// Vibrato/chorus scanner of the tonewheel organ (Story 2.4). Clean-room; the public facts and design values
// are listed in engine/core/ORGAN.md ("Vibrato/chorus scanner").
//
// Model: a delay line with taps at multiples of kTapSpacingSamples. A scanner sweeps a read position back and
// forth (triangle) across the first `tap_intervals(mode)` tap intervals at kSweepHz; the signal is read at that
// position with linear interpolation between the two neighbouring samples, so the delay changes smoothly and the
// pitch is frequency-modulated by the sweep. Vibrato settings output the scanned signal alone; chorus settings
// output an equal mix of the dry and the scanned signal. Switching settings crossfades over kCrossfadeFrames.
//
// One scanner belongs to one part (state: delay line, sweep phase, crossfade); a part runs on one worker.
// Fixed-size storage, no allocation, no locks: safe on the audio thread.
#include <array>
#include <cstdint>

namespace velora::core {

enum class ScannerMode : std::uint8_t { kOff = 0, kV1, kV2, kV3, kC1, kC2, kC3 };

class VibratoScanner {
 public:
  static constexpr std::uint32_t kModeCount = 7;
  static constexpr double kSampleRateHz = 48000.0;
  static constexpr double kSweepHz = 6.87;                  // public reference: scanner at about 412 rpm
  static constexpr std::uint32_t kTapSpacingSamples = 6;    // 0.125 ms between taps
  static constexpr std::uint32_t kTapCount = 9;             // full line: 9 taps = 8 intervals (V3, C3)
  static constexpr std::uint32_t kMaxDelaySamples = (kTapCount - 1) * kTapSpacingSamples;  // 48
  static constexpr std::uint32_t kCrossfadeFrames = 240;    // 5 ms
  static constexpr float kChorusWet = 0.5f;                 // chorus: dry (1 - kChorusWet) + scanned kChorusWet

  // Tap intervals swept by a setting: V1/C1 3 taps (2 intervals), V2/C2 5 taps (4), V3/C3 9 taps (8). 0 = off.
  static constexpr std::uint32_t tap_intervals(ScannerMode m) noexcept {
    switch (m) {
      case ScannerMode::kV1: case ScannerMode::kC1: return 2;
      case ScannerMode::kV2: case ScannerMode::kC2: return 4;
      case ScannerMode::kV3: case ScannerMode::kC3: return 8;
      default: return 0;
    }
  }
  static constexpr bool is_chorus(ScannerMode m) noexcept { return m == ScannerMode::kC1 || m == ScannerMode::kC2 || m == ScannerMode::kC3; }
  // Weight of the scanned signal in the output (0 off, 1 vibrato, kChorusWet chorus).
  static constexpr float wet_mix(ScannerMode m) noexcept {
    return m == ScannerMode::kOff ? 0.0f : is_chorus(m) ? kChorusWet : 1.0f;
  }
  // Peak-to-peak delay swing in samples of a setting.
  static constexpr float swing_samples(ScannerMode m) noexcept { return static_cast<float>(tap_intervals(m) * kTapSpacingSamples); }

  ScannerMode mode() const noexcept { return target_; }
  bool crossfading() const noexcept { return u_left_ > 0; }

  // Selects a setting at the current frame; the output crossfades from what it is now (also from a crossfade in
  // progress: the weights are interpolated, so there is no jump).
  void set_mode(ScannerMode m) noexcept {
    if (m == target_) return;
    for (std::uint32_t i = 0; i < kModeCount; ++i) start_[i] = weight(i);
    target_ = m;
    u_left_ = kCrossfadeFrames;
  }

  // Setup time: selects a setting with no crossfade (initial state).
  void set_mode_immediate(ScannerMode m) noexcept {
    target_ = m;
    start_ = {};
    u_left_ = 0;
  }

  // Processes `n` frames in place or out of place (in == out allowed).
  void process(const float* in, float* out, std::uint32_t n) noexcept {
    for (std::uint32_t f = 0; f < n; ++f) {
      const float x = in[f];
      buf_[pos_ & kMask] = x;
      const float tri = triangle();
      float y;
      if (u_left_ == 0 && target_ == ScannerMode::kOff) {
        y = x;
      } else if (u_left_ == 0) {
        y = render_mode(target_, x, tri);
      } else {
        const float u = 1.0f - static_cast<float>(u_left_) / static_cast<float>(kCrossfadeFrames);
        y = 0.0f;
        for (std::uint32_t i = 0; i < kModeCount; ++i) {
          const float w = weight_at(i, u);
          if (w != 0.0f) y += w * render_mode(static_cast<ScannerMode>(i), x, tri);
        }
        if (--u_left_ == 0) start_ = {};
      }
      out[f] = y;
      ++pos_;
      phase_ += kPhaseStep;
    }
  }

  // The part has no signal for `n` frames: advance the sweep and any crossfade, and clear the delay line, as if
  // zeros had been processed (so stale samples never come back when a key starts again).
  void skip(std::uint32_t n) noexcept {
    phase_ += static_cast<std::uint32_t>(kPhaseStep) * n;
    pos_ += n;
    buf_.fill(0.0f);
    if (u_left_ > 0) {
      u_left_ = n >= u_left_ ? 0 : u_left_ - n;
      if (u_left_ == 0) start_ = {};
    }
  }

  // Introspection for tests: the scanner position in 0..1 (triangle) for the current sweep phase.
  float sweep_position() const noexcept { return triangle(); }

 private:
  static constexpr std::uint32_t kMask = 63;  // delay line length 64 > kMaxDelaySamples + 1
  static constexpr std::uint32_t kPhaseStep = static_cast<std::uint32_t>(kSweepHz / kSampleRateHz * 4294967296.0 + 0.5);

  float triangle() const noexcept {
    const float u = static_cast<float>(phase_) * (1.0f / 4294967296.0f);
    return u < 0.5f ? 2.0f * u : 2.0f - 2.0f * u;
  }
  float weight(std::uint32_t i) const noexcept {
    const float u = u_left_ == 0 ? 1.0f : 1.0f - static_cast<float>(u_left_) / static_cast<float>(kCrossfadeFrames);
    return weight_at(i, u);
  }
  float weight_at(std::uint32_t i, float u) const noexcept {
    const float t = i == static_cast<std::uint32_t>(target_) ? 1.0f : 0.0f;
    return start_[i] + (t - start_[i]) * u;
  }
  float read(float delay) const noexcept {  // delay in samples, 0 <= delay <= kMaxDelaySamples
    const std::uint32_t whole = static_cast<std::uint32_t>(delay);
    const float fr = delay - static_cast<float>(whole);
    const float a = buf_[(pos_ - whole) & kMask];
    const float b = buf_[(pos_ - whole - 1) & kMask];
    return a + (b - a) * fr;
  }
  float render_mode(ScannerMode m, float dry, float tri) const noexcept {
    if (m == ScannerMode::kOff) return dry;
    const float wet = wet_mix(m);
    const float scanned = read(tri * swing_samples(m));
    return dry + (scanned - dry) * wet;
  }

  std::array<float, 64> buf_{};
  std::array<float, kModeCount> start_{{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}};  // weights when the crossfade began
  ScannerMode target_ = ScannerMode::kOff;
  std::uint32_t u_left_ = 0;
  std::uint32_t pos_ = 0, phase_ = 0;
};

}  // namespace velora::core
