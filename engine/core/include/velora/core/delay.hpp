#pragma once
// Delay send (Story 2.11): the processor behind the Delay send bus of the Mixer. Clean-room, textbook design: a stereo feedback delay line
// (two circular buffers, a fractional read with 4-point Catmull-Rom interpolation), a smoothed delay time, a feedback path with a
// DC blocker and a one-pole damping low-pass, an optional ping-pong cross-feedback and a safety soft limiter in the loop. The README
// ("Delay send") documents the structure, the mappings and the stability argument.
//
//   send sum (stereo) --> mono (L+R)/2 --> input gains (1, 1 - pingpong) --> + headroom-limited feedback --> write L / write R
//   read L / read R (delay D, fractional, glides to its target) --> x return level --> wet (stereo)
//   feedback source = (1 - pp) * own read + pp * other read --> damping low-pass --> DC blocker --> x feedback
//
// Time. With tempo sync off the delay time is `delay_time` in milliseconds (1 .. 2000). With tempo sync on it is note value x 60 / bpm
// (delaymap::sync_time_ms), where bpm is the RT plane's single tempo value (BlockRenderer::tempo_bpm(), registry `tempo_bpm`): the Mixer hands
// it to set_tempo() once per block, so a tempo change moves the target at the next block. The effective delay slews toward the target
// (an exponential approach with a rate limit of kSlewMax samples per sample, so the read position never moves faster than 1.25x or slower
// than 0.75x real time): changes glide like a tape delay and never click.
//
// Stability. The loop gain is at most the clamped feedback (<= kMaxFeedback = 0.95): the interpolator, the damping low-pass, the
// normalised DC blocker and the ping-pong mixing matrix (rows sum to 1, symmetric) all have a magnitude of at most 1. In addition the feedback
// that is added to the input passes a headroom limiter (limit()): the feedback contribution g is confined to |g| < 1 - |input| (identity up to
// kLimitKnee of that headroom, then a soft approach), so for any input within full scale |input| <= 1 no stored sample, and with it no wet output
// sample at return level <= 1, can exceed full scale, whatever the parameters or the glide do. The input itself is written unchanged (the first
// echo is exact); a feedback loop that would grow saturates softly instead. The cubic interpolator can overshoot a stored value by up to 25 % between
// samples (the sum of its absolute weights), so the wet tap is held to +-1 (full scale) as well; the wet output is therefore within +-return level
// whatever happens in the loop (an input beyond full scale has its echo held to full scale too).
//
// Idle: the work stops when nothing can be heard. quiet() is true once the tail has decayed (every value written during the last
// kBufferSize frames was below kFlushLevel with no input) or the return level is 0, and no input has arrived since. Nothing is ever cleared in bulk:
// the lines are never wiped, a restart (or reset()) only forgets how much has been written (valid_), and a read older than the data written since is
// silence, so waking up costs nothing extra in any block. Fixed storage, nothing allocates in process(); process() accepts any block length
// (slices of 64 frames, bit-identical to 64-frame calls). Values below 1e-20 in the loop are flushed to zero (no denormals).
#include <array>
#include <cstdint>

#include "velora/core/fx.hpp"
#include "velora/core/rt.hpp"

namespace velora::core {

// Note values of the tempo-synced delay: the numeric values are the registry enum delay_note (checked in delay.cpp).
enum class DelayNote : std::uint8_t {
  kQuarter = 0,
  kEighth,
  kSixteenth,
  kDottedQuarter,
  kDottedEighth,
  kDottedSixteenth,
  kTripletQuarter,
  kTripletEighth,
  kTripletSixteenth,
};
constexpr std::uint32_t kDelayNoteCount = 9;

namespace delaymap {
constexpr float kMinTimeMs = 1.0f, kMaxTimeMs = 2000.0f, kDefaultTimeMs = 350.0f;
constexpr float kMaxFeedback = 0.95f, kDefaultFeedback = 0.35f;
constexpr float kDefaultLevel = 1.0f, kDefaultDamping = 0.25f, kDefaultPingPong = 0.0f;
constexpr float kMinTempoBpm = 20.0f, kMaxTempoBpm = 300.0f, kDefaultTempoBpm = 120.0f;
constexpr float kMaxSyncMs = 5000.0f;  // the longest tempo-synced time (the longest note value at the slowest tempo is 4500 ms)
// Registry values to engine values. NaN and infinities -> the registry default, out of range -> clamped (a bool flag: >= 0.5 is on, NaN is off;
// the note: the nearest enum index, NaN is the default eighth).
float time_ms(float v) noexcept;
bool sync(float v) noexcept;
DelayNote note(float v) noexcept;
float feedback(float v) noexcept;
float level(float v) noexcept;
float damping(float v) noexcept;
float pingpong(float v) noexcept;
float tempo_bpm(float v) noexcept;
// Beats (quarter notes) of a note value: 1/4 = 1, 1/8 = 0.5, 1/16 = 0.25, dotted x 1.5, triplet x 2/3.
double note_beats(DelayNote n) noexcept;
// Tempo-synced time in milliseconds: beats x 60000 / bpm (bpm clamped through tempo_bpm()), limited to kMaxSyncMs.
double sync_time_ms(DelayNote n, float bpm) noexcept;
// Damping low-pass cutoff in Hz for the registry value 0..1: 20 kHz x 0.05^p (1 kHz at p = 1); 0 = off (no filter at all).
double damping_cutoff_hz(float damping) noexcept;
}  // namespace delaymap

class DelaySend final : public Effect {
 public:
  static constexpr std::uint32_t kBufferSize = 1u << 18;  // per channel (power of two) > the longest delay (kMaxSyncMs = 240000 samples) + interpolation taps
  static constexpr std::uint32_t kBufferMask = kBufferSize - 1;
  static constexpr double kMaxDelaySamples = 240000.0;
  static constexpr double kMinDelaySamples = 48.0;       // 1 ms
  static constexpr double kSlewMax = 0.25;               // samples of delay change per sample (read speed 0.75 .. 1.25)
  static constexpr double kSlewTauFrames = 960.0;        // exponential approach of the delay time (20 ms)
  static constexpr double kSnapSamples = 0.01;           // closer than this to the target: arrive
  static constexpr std::uint32_t kLevelRampFrames = 64;  // one block
  static constexpr float kLimitKnee = 0.8f;              // the feedback limiter is the identity below this fraction of the headroom
  static constexpr float kFlushLevel = 1e-6f;            // -120 dBFS

  DelaySend() noexcept;

  EffectType type() const noexcept override { return EffectType::kNone; }  // not an insert type: the send bus has no registry insert_type
  std::uint32_t cost_units() const noexcept override { return kDelaySendCost; }
  void reset() noexcept VELORA_NONBLOCKING override;

  // Delay settings (registry delay_* values, already clamped through delaymap). RT or control thread. `instant`: the first generation, the
  // time and the level apply at once (no glide, no ramp).
  void configure(float time_ms, bool sync, DelayNote note, float feedback, float level, float damping, float pingpong, bool instant) noexcept
      VELORA_NONBLOCKING;
  // The RT plane's tempo value (bpm), once per block. Only a changed value does anything; in sync mode it moves the target delay time.
  void set_tempo(float bpm) noexcept VELORA_NONBLOCKING;

  // In place: the block holds the send sum (stereo) on entry and the wet signal (stereo, already scaled by the return level) on exit.
  void process(StereoBlock& block) noexcept VELORA_NONBLOCKING override;

  bool quiet() const noexcept { return quiet_; }
  // Blocks (64-frame slices) in which the line actually ran (tests: an idle delay does no work).
  std::uint64_t engine_blocks() const noexcept { return engine_blocks_; }
  double target_delay_samples() const noexcept { return d_tgt_; }
  double current_delay_samples() const noexcept { return d_cur_; }
  bool sync_on() const noexcept { return sync_; }
  DelayNote note() const noexcept { return note_; }
  float time_ms() const noexcept { return time_ms_; }
  float feedback() const noexcept { return fb_.tgt; }
  float level() const noexcept { return lvl_tgt_; }
  float damping() const noexcept { return damping_; }
  float pingpong() const noexcept { return pp_.tgt; }
  float tempo_bpm() const noexcept { return tempo_; }

  // The headroom limiter of the loop (public for the tests): x limited to |y| < h (h >= 0 is the headroom, 0 gives 0): the identity for |x| <= kLimitKnee h,
  // then kLimitKnee h + (1 - kLimitKnee) h t / (1 + t) with t = (|x| - kLimitKnee h) / ((1 - kLimitKnee) h); odd, monotone, slope 1 at the knee.
  static float limit(float x, float h) noexcept VELORA_NONBLOCKING {
    if (!(h > 0.0f)) return 0.0f;
    const float a = x < 0.0f ? -x : x;
    const float k = kLimitKnee * h;
    if (a <= k) return x;
    const float room = (1.0f - kLimitKnee) * h;
    const float t = (a - k) / room;
    const float y = k + room * t / (1.0f + t);
    return x < 0.0f ? -y : y;
  }

 private:
  // A parameter that approaches its target with a one-pole per sample (time constant about 10 ms) and arrives exactly.
  struct Smooth {
    float cur = 0.0f, tgt = 0.0f;
    bool settled() const noexcept { return cur == tgt; }
    void step(float k) noexcept {
      cur += (tgt - cur) * k;
      const float d = tgt - cur;
      if (d < 1e-6f && d > -1e-6f) cur = tgt;
    }
  };

  void process_slice(StereoBlock& block) noexcept VELORA_NONBLOCKING;  // <= kFxBlockFrames
  void update_target() noexcept VELORA_NONBLOCKING;
  void go_quiet() noexcept VELORA_NONBLOCKING;      // start the idle state
  void wake() noexcept VELORA_NONBLOCKING;          // input arrived while quiet: start from silence (forget the old contents)
  void clear_state() noexcept VELORA_NONBLOCKING;   // filters and the written-sample count (not the buffers)

  alignas(16) std::array<std::array<float, kBufferSize>, 2> buf_{};
  std::uint32_t w_ = 0;
  double d_cur_ = 0.0, d_tgt_ = 0.0;
  float time_ms_ = delaymap::kDefaultTimeMs, tempo_ = delaymap::kDefaultTempoBpm, damping_ = delaymap::kDefaultDamping;
  bool sync_ = false;
  DelayNote note_ = DelayNote::kEighth;
  Smooth fb_, pp_, damp_a_;  // feedback, ping-pong and the damping low-pass coefficient (1 = off)
  float lp_[2] = {0.0f, 0.0f}, dc_x1_[2] = {0.0f, 0.0f}, dc_y1_[2] = {0.0f, 0.0f};
  float lvl_cur_ = 0.0f, lvl_tgt_ = 0.0f, lvl_step_ = 0.0f;
  std::uint32_t lvl_left_ = 0;
  bool quiet_ = true;
  std::uint32_t quiet_frames_ = 0;   // consecutive frames whose written values were below kFlushLevel
  std::uint32_t valid_ = 0;          // samples written since the lines were last started from silence (saturates at kBufferSize): older reads are 0
  std::uint64_t engine_blocks_ = 0;
};

}  // namespace velora::core
