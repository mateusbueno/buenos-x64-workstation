#include "velora/core/reverb.hpp"

#include <algorithm>
#include <cmath>

#include "velora/core/block_renderer.hpp"

namespace velora::core {

namespace reverbmap {
ReverbType type(float v) noexcept {
  if (!(v == v)) return ReverbType::kHall;
  const float r = std::floor(v + 0.5f);
  return r <= 0.0f ? ReverbType::kRoom : (r >= 2.0f ? ReverbType::kPlate : static_cast<ReverbType>(static_cast<int>(r)));
}
float time_s(float v) noexcept {
  if (!(v == v)) return kDefaultTimeS;
  return v < kMinTimeS ? kMinTimeS : (v > kMaxTimeS ? kMaxTimeS : v);
}
float level(float v) noexcept {
  if (!(v == v)) return 1.0f;  // the registry default
  return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}
}  // namespace reverbmap

namespace {
constexpr float kFs = static_cast<float>(kSampleRate);
constexpr float kPi = 3.14159265358979f;
constexpr std::uint32_t kN = ReverbSend::kLines;
constexpr std::uint32_t kLineMask = ReverbSend::kLineSize - 1;
constexpr std::uint32_t kPreMask = ReverbSend::kPreDelaySize - 1;
constexpr float kMeasureHz = 1000.0f;  // the decay time parameter is exact at this frequency

// The three parameter sets. Delay lengths are mutually non-commensurate (primes) so that the modes of the network do not pile up.
struct TypeSet {
  std::array<std::uint16_t, kN> lines;                // FDN delay lines, samples at 48 kHz
  std::uint16_t pre_delay;                            // samples
  std::array<std::uint16_t, ReverbSend::kDiffusers> diffusers;  // all-pass delays, samples
  float diffusion;                                    // all-pass coefficient
  float damping;                                      // one-pole coefficient p of the line filters (0 = none; higher = darker)
  float mod_depth;                                    // read-position modulation, samples (peak)
  float mod_rate_hz;                                  // mean modulation rate
};
constexpr TypeSet kSets[kReverbTypeCount] = {
    // Room: short lines (12.5..29.7 ms), almost no pre-delay, light diffusion, no modulation
    {{601, 709, 827, 947, 1061, 1193, 1297, 1427}, 96, {89, 131, 191, 263}, 0.50f, 0.20f, 0.0f, 0.0f},
    // Hall: long lines (31..65 ms), 20 ms pre-delay, more diffusion, darker, gentle modulation
    {{1499, 1699, 1901, 2111, 2347, 2593, 2837, 3109}, 960, {137, 199, 293, 419}, 0.60f, 0.32f, 0.8f, 0.45f},
    // Plate: medium lines (20..47 ms), no pre-delay, dense diffusion, bright, more modulation
    {{977, 1151, 1303, 1481, 1657, 1861, 2053, 2273}, 0, {73, 109, 157, 223}, 0.70f, 0.10f, 1.2f, 0.9f},
};
static_assert(2273 + 3 < ReverbSend::kLineSize && 3109 + 3 < ReverbSend::kLineSize, "the longest line plus modulation fits");
static_assert(960 < ReverbSend::kPreDelaySize && 419 < ReverbSend::kDiffuserSize, "pre-delay and diffusers fit");

// Output tap signs: left and right use two orthogonal Walsh patterns (decorrelated).
constexpr float kSignL[kN] = {1, 1, -1, -1, 1, 1, -1, -1};
constexpr float kSignR[kN] = {1, -1, 1, -1, 1, -1, 1, -1};

float tri(float ph) noexcept {  // ph in [0, 1) -> triangle in [-1, 1]
  const float a = ph < 0.5f ? ph : 1.0f - ph;
  return 4.0f * a - 1.0f;
}
}  // namespace

void ReverbSend::Engine::clear() noexcept {
  for (auto& l : line) l.fill(0.0f);
  pre.fill(0.0f);
  for (auto& a : ap) a.fill(0.0f);
  ap_pos.fill(0);
  damp_state.fill(0.0f);
  w = pre_w = 0;
  dc_x1 = dc_y1 = 0.0f;
  for (std::uint32_t i = 0; i < kN; ++i) lfo_phase[i] = static_cast<float>(i) / static_cast<float>(kN);
}

void ReverbSend::Engine::configure(ReverbType t, float time_s) noexcept {
  type = t;
  const TypeSet& s = kSets[static_cast<std::uint32_t>(t)];
  for (std::uint32_t i = 0; i < kN; ++i) {
    delay[i] = s.lines[i];
    // modulation rates spread around the mean (0.7 .. 1.3 x)
    lfo_inc[i] = s.mod_rate_hz * (0.7f + 0.6f * static_cast<float>(i) / static_cast<float>(kN - 1)) / kFs;
  }
  for (std::uint32_t i = 0; i < kDiffusers; ++i) ap_delay[i] = s.diffusers[i];
  pre_delay = s.pre_delay;
  ap_gain = s.diffusion;
  damping = s.damping;
  mod_depth = s.mod_depth;
  set_time(time_s);
}

// Loop gain per line: the lossless loop gain for RT60 T at 1 kHz is 10^(-3 d / (T fs)); the one-pole damping filter attenuates
// 1 kHz by |H|, which the line gain compensates (the gain is limited below unity, the DC gain is gain * 1).
void ReverbSend::Engine::set_time(float time_s) noexcept {
  const float p = damping;
  const float wm = 2.0f * kPi * kMeasureHz / kFs;
  const float h = (1.0f - p) / std::sqrt(1.0f - 2.0f * p * std::cos(wm) + p * p);  // |H(1 kHz)|, H = (1-p)/(1-p z^-1)
  // A modulated read position interpolates linearly, which attenuates 1 kHz slightly for fractional positions (per trip this matters at
  // long times and short lines): the mean magnitude over uniformly spread fractions is compensated as well.
  float hi = 1.0f;
  if (mod_depth > 0.0f) {
    float acc = 0.0f;
    for (int k = 0; k < 16; ++k) {
      const float fr = (static_cast<float>(k) + 0.5f) / 16.0f;
      acc += std::sqrt(fr * fr + (1.0f - fr) * (1.0f - fr) + 2.0f * fr * (1.0f - fr) * std::cos(wm));
    }
    hi = acc / 16.0f;
  }
  for (std::uint32_t i = 0; i < kN; ++i) {
    const float target = std::exp(-6.907755279f * static_cast<float>(delay[i]) / (time_s * kFs));  // 10^(-3 d / (T fs))
    gain[i] = std::min(target / (h * hi), 0.9999f);
    // Unit-power normalisation of the wet signal: for a white send the loop power gain of a line is gain^2 times the mean power gain of
    // the one-pole filter over the spectrum, (1-p)/(1+p); the steady-state line power is then b^2 / (1 - that), and the tap gain undoes it.
    const float loop_power = gain[i] * gain[i] * (1.0f - p) / (1.0f + p);
    out_gain[i] = std::sqrt(std::max(0.0f, 1.0f - loop_power));
  }
}

void ReverbSend::Engine::run(const float* in, float* out_l, float* out_r, std::uint32_t n) noexcept {
  constexpr float kHouseholder = 2.0f / static_cast<float>(kN);  // x - (2/N) sum(x): the Householder reflection
  constexpr float kInject = 0.35355339f;  // 1/sqrt(8)
  const float p = damping, q = 1.0f - damping;
  for (std::uint32_t f = 0; f < n; ++f) {
    // DC blocker, pre-delay, diffusers
    float x = in[f];
    const float dc = x - dc_x1 + 0.9995f * dc_y1;
    dc_x1 = x;
    dc_y1 = dc;
    x = dc;
    pre[pre_w] = x;
    x = pre[(pre_w - pre_delay) & kPreMask];
    pre_w = (pre_w + 1) & kPreMask;
    for (std::uint32_t k = 0; k < kDiffusers; ++k) {
      const std::uint32_t d = ap_delay[k];
      std::uint32_t& pos = ap_pos[k];
      std::array<float, kDiffuserSize>& buf = ap[k];
      const float wd = buf[pos];
      const float wv = x + ap_gain * wd;
      x = wd - ap_gain * wv;
      buf[pos] = wv;
      if (++pos >= d) pos = 0;
    }
    // FDN: read (modulated), damp, mix, write
    float s[kN], fl[kN];
    float sum = 0.0f, ol = 0.0f, orr = 0.0f;
    for (std::uint32_t i = 0; i < kN; ++i) {
      float ph = lfo_phase[i] + lfo_inc[i];
      if (ph >= 1.0f) ph -= 1.0f;
      lfo_phase[i] = ph;
      const float dly = static_cast<float>(delay[i]) + mod_depth * tri(ph);
      const std::uint32_t di = static_cast<std::uint32_t>(dly);
      const float fr = dly - static_cast<float>(di);
      const float a = line[i][(w - di) & kLineMask], b = line[i][(w - di - 1) & kLineMask];
      s[i] = a + fr * (b - a);
      ol += kSignL[i] * out_gain[i] * s[i];
      orr += kSignR[i] * out_gain[i] * s[i];
      damp_state[i] = q * s[i] + p * damp_state[i];
      fl[i] = gain[i] * damp_state[i];
      sum += fl[i];
    }
    const float fb = kHouseholder * sum;
    for (std::uint32_t i = 0; i < kN; ++i) line[i][w] = (fl[i] - fb) + kInject * x;
    w = (w + 1) & kLineMask;
    out_l[f] = ol;
    out_r[f] = orr;
  }
}

ReverbSend::ReverbSend() noexcept {
  engines_[0].configure(ReverbType::kHall, time_);
  engines_[1].configure(ReverbType::kHall, time_);
  engines_[0].clear();
  engines_[1].clear();
  lvl_cur_ = lvl_tgt_ = 1.0f;
}

void ReverbSend::clear_all() noexcept {
  // A fade that was running (or a type that waited) is finished at once: nothing rings any more, so the newest requested type is simply adopted.
  const ReverbType newest = has_pending_ ? pending_ : (fade_left_ > 0 ? engines_[1 - cur_].type : engines_[cur_].type);
  engines_[0].clear();
  engines_[1].clear();
  engines_[cur_].configure(newest, time_);
  fade_left_ = 0;
  has_pending_ = false;
  silent_frames_ = 0;
  quiet_blocks_ = 0;
  quiet_ = true;
}

void ReverbSend::reset() noexcept { clear_all(); }

void ReverbSend::start_fade(ReverbType t) noexcept {
  const std::uint32_t inc = 1 - cur_;
  engines_[inc].configure(t, time_);
  engines_[inc].clear();
  fade_left_ = kTypeFadeFrames;
  has_pending_ = false;
}

void ReverbSend::configure(ReverbType t, float time_s, float level, bool instant) noexcept {
  time_ = reverbmap::time_s(time_s);
  const float lv = reverbmap::level(level);
  if (instant) {
    lvl_cur_ = lvl_tgt_ = lv;
    lvl_left_ = 0;
  } else if (lv != lvl_tgt_) {
    lvl_tgt_ = lv;
    lvl_step_ = (lv - lvl_cur_) / static_cast<float>(kLevelRampFrames);
    lvl_left_ = kLevelRampFrames;
  }
  engines_[cur_].set_time(time_);
  if (fade_left_ > 0) engines_[1 - cur_].set_time(time_);
  if (instant || quiet_) {  // nothing rings (or the first generation): switch type at once, no audible change
    fade_left_ = 0;
    has_pending_ = false;
    engines_[cur_].configure(t, time_);
    if (instant) engines_[cur_].clear();
    return;
  }
  const ReverbType newest = fade_left_ > 0 || has_pending_ ? (has_pending_ ? pending_ : engines_[1 - cur_].type) : engines_[cur_].type;
  if (t == newest) return;
  if (fade_left_ > 0) {  // a fade is running: the latest request waits for it
    pending_ = t;
    has_pending_ = true;
  } else {
    start_fade(t);
  }
}

void ReverbSend::process(StereoBlock& b) noexcept {
  // Any block length: processed in slices of at most kFxBlockFrames (the scratch arrays are that size).
  for (std::uint32_t pos = 0; pos < b.frames; pos += kFxBlockFrames) {
    StereoBlock sl{b.left + pos, b.right + pos, std::min(kFxBlockFrames, b.frames - pos)};
    process_slice(sl);
  }
}

void ReverbSend::process_slice(StereoBlock& b) noexcept {
  const std::uint32_t n = b.frames;
  float in[kFxBlockFrames];
  float peak = 0.0f;
  for (std::uint32_t f = 0; f < n; ++f) {
    const float l = std::isfinite(b.left[f]) ? b.left[f] : 0.0f, r = std::isfinite(b.right[f]) ? b.right[f] : 0.0f;
    in[f] = 0.5f * (l + r);
    peak = std::max(peak, std::fabs(l) + std::fabs(r));
  }
  auto zero = [&] {
    std::fill(b.left, b.left + n, 0.0f);
    std::fill(b.right, b.right + n, 0.0f);
  };
  // Return level 0 (steady): the reverb is silent and does no work; its state is cleared once.
  if (lvl_left_ == 0 && lvl_tgt_ == 0.0f && lvl_cur_ == 0.0f) {
    if (!quiet_) clear_all();
    zero();
    return;
  }
  if (peak == 0.0f && quiet_) {  // idle: nothing in, nothing ringing; a pending level ramp is settled (the output is silent anyway)
    lvl_cur_ = lvl_tgt_;
    lvl_left_ = 0;
    zero();
    return;
  }
  if (peak != 0.0f) {
    quiet_ = false;
    silent_frames_ = 0;
    quiet_blocks_ = 0;
  } else {
    silent_frames_ += n;
  }

  ++engine_blocks_;
  float wl[kFxBlockFrames], wr[kFxBlockFrames];
  engines_[cur_].run(in, wl, wr, n);
  if (fade_left_ > 0) {
    float il[kFxBlockFrames], ir[kFxBlockFrames];
    engines_[1 - cur_].run(in, il, ir, n);
    for (std::uint32_t f = 0; f < n; ++f) {
      const std::uint32_t done = kTypeFadeFrames - fade_left_ + f + 1;
      const float wi = done >= kTypeFadeFrames ? 1.0f : static_cast<float>(done) / static_cast<float>(kTypeFadeFrames);
      wl[f] += wi * (il[f] - wl[f]);
      wr[f] += wi * (ir[f] - wr[f]);
    }
    fade_left_ = fade_left_ > n ? fade_left_ - n : 0;
    if (fade_left_ == 0) {
      cur_ = 1 - cur_;
      engines_[1 - cur_].clear();
      if (has_pending_ && pending_ != engines_[cur_].type) start_fade(pending_);
      else has_pending_ = false;
    }
  }

  float wet_peak = 0.0f;
  for (std::uint32_t f = 0; f < n; ++f) {
    wet_peak = std::max(wet_peak, std::fabs(wl[f]) + std::fabs(wr[f]));
    if (lvl_left_ > 0) {
      lvl_cur_ += lvl_step_;
      if (--lvl_left_ == 0) lvl_cur_ = lvl_tgt_;
    }
    b.left[f] = lvl_cur_ * wl[f];
    b.right[f] = lvl_cur_ * wr[f];
  }
  // The tail has decayed: clear the state once so that the idle reverb does no work (and no denormals can build up).
  if (peak == 0.0f && fade_left_ == 0 && !has_pending_ && silent_frames_ >= kFlushAfterFrames) {
    if (wet_peak < kFlushLevel) {
      if (++quiet_blocks_ >= kFlushBlocks) clear_all();
    } else {
      quiet_blocks_ = 0;
    }
  }
}

}  // namespace velora::core
