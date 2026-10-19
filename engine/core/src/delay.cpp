#include "velora/core/delay.hpp"

#include <algorithm>
#include <cmath>

#include "velora/core/block_renderer.hpp"

namespace velora::core {

// The enum values are the registry's.
static_assert(static_cast<std::uint8_t>(DelayNote::kQuarter) == params::kDelayNoteQuarter && static_cast<std::uint8_t>(DelayNote::kEighth) == params::kDelayNoteEighth &&
                  static_cast<std::uint8_t>(DelayNote::kSixteenth) == params::kDelayNoteSixteenth &&
                  static_cast<std::uint8_t>(DelayNote::kDottedQuarter) == params::kDelayNoteDottedQuarter &&
                  static_cast<std::uint8_t>(DelayNote::kDottedEighth) == params::kDelayNoteDottedEighth &&
                  static_cast<std::uint8_t>(DelayNote::kDottedSixteenth) == params::kDelayNoteDottedSixteenth &&
                  static_cast<std::uint8_t>(DelayNote::kTripletQuarter) == params::kDelayNoteTripletQuarter &&
                  static_cast<std::uint8_t>(DelayNote::kTripletEighth) == params::kDelayNoteTripletEighth &&
                  static_cast<std::uint8_t>(DelayNote::kTripletSixteenth) == params::kDelayNoteTripletSixteenth &&
                  params::kDelayNoteCount == kDelayNoteCount,
              "DelayNote follows the registry enum delay_note");

namespace delaymap {
namespace {
float finite_clamp(float v, float lo, float hi, float dflt) noexcept {
  if (!std::isfinite(v)) return dflt;
  return v < lo ? lo : (v > hi ? hi : v);
}
}  // namespace
float time_ms(float v) noexcept { return finite_clamp(v, kMinTimeMs, kMaxTimeMs, kDefaultTimeMs); }
bool sync(float v) noexcept { return std::isfinite(v) && v >= 0.5f; }
DelayNote note(float v) noexcept {
  if (!std::isfinite(v)) return DelayNote::kEighth;
  const float r = std::floor(v + 0.5f);
  return r <= 0.0f ? DelayNote::kQuarter
                   : (r >= static_cast<float>(kDelayNoteCount - 1) ? DelayNote::kTripletSixteenth : static_cast<DelayNote>(static_cast<int>(r)));
}
float feedback(float v) noexcept { return finite_clamp(v, 0.0f, kMaxFeedback, kDefaultFeedback); }
float level(float v) noexcept { return finite_clamp(v, 0.0f, 1.0f, kDefaultLevel); }
float damping(float v) noexcept { return finite_clamp(v, 0.0f, 1.0f, kDefaultDamping); }
float pingpong(float v) noexcept { return finite_clamp(v, 0.0f, 1.0f, kDefaultPingPong); }
float tempo_bpm(float v) noexcept { return finite_clamp(v, kMinTempoBpm, kMaxTempoBpm, kDefaultTempoBpm); }
double note_beats(DelayNote n) noexcept {
  switch (n) {
    case DelayNote::kQuarter: return 1.0;
    case DelayNote::kEighth: return 0.5;
    case DelayNote::kSixteenth: return 0.25;
    case DelayNote::kDottedQuarter: return 1.5;
    case DelayNote::kDottedEighth: return 0.75;
    case DelayNote::kDottedSixteenth: return 0.375;
    case DelayNote::kTripletQuarter: return 2.0 / 3.0;
    case DelayNote::kTripletEighth: return 1.0 / 3.0;
    case DelayNote::kTripletSixteenth: return 1.0 / 6.0;
  }
  return 0.5;
}
double sync_time_ms(DelayNote n, float bpm) noexcept {
  const double ms = note_beats(n) * 60000.0 / static_cast<double>(tempo_bpm(bpm));
  return ms > static_cast<double>(kMaxSyncMs) ? static_cast<double>(kMaxSyncMs) : ms;
}
double damping_cutoff_hz(float d) noexcept {
  const float p = damping(d);
  return p <= 0.0f ? 0.0 : 20000.0 * std::pow(0.05, static_cast<double>(p));
}
}  // namespace delaymap

namespace {
constexpr double kFs = static_cast<double>(kSampleRate);
constexpr double kPi = 3.14159265358979323846;
constexpr std::uint32_t kMask = DelaySend::kBufferMask;
constexpr float kParamK = 0.0020811f;           // 1 - exp(-1 / (10 ms * 48 kHz)): the parameter smoother
constexpr float kDcR = 0.9948f;                 // feedback DC blocker pole (about 40 Hz at 48 kHz)
constexpr float kDcK = 0.5f * (1.0f + kDcR);    // normalises the Nyquist gain of the blocker to exactly 1 (its magnitude never exceeds 1)

float damping_coef(float damping) noexcept {  // one-pole low-pass coefficient a: y += a (x - y); 1 = off
  const double fc = delaymap::damping_cutoff_hz(damping);
  if (fc <= 0.0) return 1.0f;
  return static_cast<float>(1.0 - std::exp(-2.0 * kPi * fc / kFs));
}

// 4-point Catmull-Rom (cubic Hermite) read at position j - fd of a circular buffer (j: integer index, fd in [0, 1)). The magnitude response is
// at most 1 for every fraction (checked numerically in the tests), the interpolator at fd = 0 is the exact sample.
// Taps older than `valid` samples (before the lines were started from silence) read as 0; `w` is the index about to be written.
inline float tap(const float* b, std::uint32_t k, std::uint32_t w, std::uint32_t valid) noexcept {
  return ((w - k) & kMask) <= valid ? b[k & kMask] : 0.0f;
}
inline float ftz(float x) noexcept { return (x < 1e-20f && x > -1e-20f) ? 0.0f : x; }  // no denormals in the loop

inline float read_at(const float* b, std::uint32_t j, float fd, std::uint32_t w, std::uint32_t valid) noexcept {
  if (fd <= 0.0f) return tap(b, j, w, valid);
  const float u = 1.0f - fd;
  const std::uint32_t i = j - 1;
  const float p0 = tap(b, i - 1, w, valid), p1 = tap(b, i, w, valid), p2 = tap(b, i + 1, w, valid), p3 = tap(b, i + 2, w, valid);
  return p1 + 0.5f * u * (p2 - p0 + u * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + u * (3.0f * (p1 - p2) + p3 - p0)));
}
}  // namespace

DelaySend::DelaySend() noexcept {
  d_cur_ = d_tgt_ = delaymap::kDefaultTimeMs * kFs / 1000.0;
  fb_.cur = fb_.tgt = delaymap::kDefaultFeedback;
  pp_.cur = pp_.tgt = delaymap::kDefaultPingPong;
  damp_a_.cur = damp_a_.tgt = damping_coef(delaymap::kDefaultDamping);
}

void DelaySend::update_target() noexcept {
  const double ms = sync_ ? delaymap::sync_time_ms(note_, tempo_) : static_cast<double>(time_ms_);
  d_tgt_ = std::clamp(ms * kFs / 1000.0, kMinDelaySamples, kMaxDelaySamples);
}

void DelaySend::clear_state() noexcept {
  lp_[0] = lp_[1] = 0.0f;
  dc_x1_[0] = dc_x1_[1] = 0.0f;
  dc_y1_[0] = dc_y1_[1] = 0.0f;
  valid_ = 0;  // the lines are not wiped: nothing older than this point is ever read
}

void DelaySend::go_quiet() noexcept {
  quiet_ = true;
  quiet_frames_ = 0;
  clear_state();
}

void DelaySend::wake() noexcept {
  clear_state();
  d_cur_ = d_tgt_;  // nothing rings: the delay time needs no glide
  fb_.cur = fb_.tgt;
  pp_.cur = pp_.tgt;
  damp_a_.cur = damp_a_.tgt;
  quiet_ = false;
  quiet_frames_ = 0;
}

void DelaySend::reset() noexcept {
  clear_state();
  w_ = 0;
  d_cur_ = d_tgt_;
  fb_.cur = fb_.tgt;
  pp_.cur = pp_.tgt;
  damp_a_.cur = damp_a_.tgt;
  lvl_cur_ = lvl_tgt_;
  lvl_left_ = 0;
  quiet_ = true;
  quiet_frames_ = 0;
}

void DelaySend::configure(float time_ms, bool sync, DelayNote note, float feedback, float level, float damping, float pingpong, bool instant) noexcept {
  time_ms_ = delaymap::time_ms(time_ms);
  sync_ = sync;
  note_ = static_cast<std::uint32_t>(note) < kDelayNoteCount ? note : DelayNote::kEighth;
  damping_ = delaymap::damping(damping);
  fb_.tgt = delaymap::feedback(feedback);
  pp_.tgt = delaymap::pingpong(pingpong);
  damp_a_.tgt = damping_coef(damping_);
  const float lv = delaymap::level(level);
  update_target();
  if (instant) {
    d_cur_ = d_tgt_;
    fb_.cur = fb_.tgt;
    pp_.cur = pp_.tgt;
    damp_a_.cur = damp_a_.tgt;
    lvl_cur_ = lvl_tgt_ = lv;
    lvl_left_ = 0;
  } else if (lv != lvl_tgt_) {
    lvl_tgt_ = lv;
    lvl_step_ = (lv - lvl_cur_) / static_cast<float>(kLevelRampFrames);
    lvl_left_ = kLevelRampFrames;
  }
}

void DelaySend::set_tempo(float bpm) noexcept {
  const float t = delaymap::tempo_bpm(bpm);
  if (t == tempo_) return;
  tempo_ = t;
  if (sync_) update_target();
}

void DelaySend::process(StereoBlock& b) noexcept {
  // Any block length: processed in slices of at most kFxBlockFrames (the scratch arrays are that size).
  for (std::uint32_t pos = 0; pos < b.frames; pos += kFxBlockFrames) {
    StereoBlock sl{b.left + pos, b.right + pos, std::min(kFxBlockFrames, b.frames - pos)};
    process_slice(sl);
  }
}

void DelaySend::process_slice(StereoBlock& b) noexcept {
  const std::uint32_t n = b.frames;
  float in[kFxBlockFrames];
  float peak = 0.0f;
  for (std::uint32_t f = 0; f < n; ++f) {
    // non-finite input is silence; huge input is held to +-1e6 so that nothing near FLT_MAX can overflow the sums or the filters
    const float l = std::isfinite(b.left[f]) ? std::clamp(b.left[f], -1e6f, 1e6f) : 0.0f, r = std::isfinite(b.right[f]) ? std::clamp(b.right[f], -1e6f, 1e6f) : 0.0f;
    in[f] = 0.5f * (l + r);
    peak = std::max(peak, std::fabs(l) + std::fabs(r));
  }
  auto zero = [&] {
    std::fill(b.left, b.left + n, 0.0f);
    std::fill(b.right, b.right + n, 0.0f);
  };
  // Return level 0 (steady): the delay is silent and does no work; its state is cleared (in slices).
  if (lvl_left_ == 0 && lvl_tgt_ == 0.0f && lvl_cur_ == 0.0f) {
    if (!quiet_) go_quiet();
    zero();
    return;
  }
  if (peak == 0.0f && quiet_) {  // idle: nothing in, nothing ringing; a pending level ramp is settled (the output is silent anyway)
    lvl_cur_ = lvl_tgt_;
    lvl_left_ = 0;
    zero();
    return;
  }
  if (quiet_) wake();  // input after an idle spell (peak != 0)

  ++engine_blocks_;
  float wp = 0.0f;  // largest value written to the line in this slice
  for (std::uint32_t f = 0; f < n; ++f) {
    if (d_cur_ != d_tgt_) {  // glide: exponential approach, rate limited, arrives exactly
      const double diff = d_tgt_ - d_cur_;
      if (diff <= kSnapSamples && diff >= -kSnapSamples) {
        d_cur_ = d_tgt_;
      } else {
        d_cur_ += std::clamp(diff / kSlewTauFrames, -kSlewMax, kSlewMax);
      }
    }
    if (!fb_.settled()) fb_.step(kParamK);
    if (!pp_.settled()) pp_.step(kParamK);
    if (!damp_a_.settled()) damp_a_.step(kParamK);

    const std::uint32_t id = static_cast<std::uint32_t>(d_cur_);
    const float fd = static_cast<float>(d_cur_ - static_cast<double>(id));
    const std::uint32_t j = w_ - id;
    const float rd[2] = {ftz(read_at(buf_[0].data(), j, fd, w_, valid_)), ftz(read_at(buf_[1].data(), j, fd, w_, valid_))};

    const float s = pp_.cur;
    const float src[2] = {(1.0f - s) * rd[0] + s * rd[1], (1.0f - s) * rd[1] + s * rd[0]};
    const float a = damp_a_.cur;
    float fbv[2];
    for (int c = 0; c < 2; ++c) {
      float x = src[c];
      // the low-pass state follows the signal even when the filter is off (a >= 1), so raising the damping mid-tail starts from a settled state
      lp_[c] = a < 1.0f ? ftz(lp_[c] + a * (x - lp_[c])) : x;
      x = lp_[c];
      const float y = ftz(kDcK * (x - dc_x1_[c]) + kDcR * dc_y1_[c]);  // high-pass: no DC in the loop
      dc_x1_[c] = x;
      dc_y1_[c] = y;
      fbv[c] = fb_.cur * y;
    }
    // the input is written as it is; the feedback is limited to the headroom the input leaves below full scale (see the header)
    const float in0 = in[f], in1 = (1.0f - s) * in[f];
    // (the clamp only removes float rounding at the edge: a stored value never exceeds full scale unless the input itself does)
    const float b0 = std::max(1.0f, std::fabs(in0)), b1 = std::max(1.0f, std::fabs(in1));
    const float v0 = ftz(std::clamp(in0 + limit(fbv[0], 1.0f - std::fabs(in0)), -b0, b0)), v1 = ftz(std::clamp(in1 + limit(fbv[1], 1.0f - std::fabs(in1)), -b1, b1));
    buf_[0][w_ & kMask] = v0;
    buf_[1][w_ & kMask] = v1;
    wp = std::max(wp, std::max(std::fabs(v0), std::fabs(v1)));
    w_ = (w_ + 1) & kMask;
    if (valid_ < kBufferSize) ++valid_;

    if (lvl_left_ > 0) {
      lvl_cur_ += lvl_step_;
      if (--lvl_left_ == 0) lvl_cur_ = lvl_tgt_;
    }
    // The wet tap is held to full scale: the cubic interpolator can overshoot a stored value by up to 25 % between samples.
    b.left[f] = lvl_cur_ * std::clamp(rd[0], -1.0f, 1.0f);
    b.right[f] = lvl_cur_ * std::clamp(rd[1], -1.0f, 1.0f);
  }
  // The tail has decayed: after every value written during a whole buffer length was below kFlushLevel (so the line holds nothing audible),
  // go idle. Input that is not exactly silent keeps the delay running.
  if (peak == 0.0f && wp < kFlushLevel) {
    quiet_frames_ += n;
    if (quiet_frames_ >= kBufferSize) go_quiet();
  } else {
    quiet_frames_ = 0;
  }
}

}  // namespace velora::core
