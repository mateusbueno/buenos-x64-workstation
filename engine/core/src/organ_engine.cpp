#include "velora/core/organ_engine.hpp"

#include <algorithm>
#include <cmath>

#include "velora/core/params_gen.hpp"

namespace velora::core {

namespace {
constexpr std::uint32_t kD = tonewheel::kDrawbarCount;
constexpr std::uint32_t kW = tonewheel::kWheelCount;
constexpr double kTwoPi = 6.28318530717958647692;
static_assert(params::kDrawbar1 - params::kDrawbar16 == kD - 1, "drawbar ids are contiguous in drawbar order");

float soft_limit(float x) noexcept {  // linear to the knee, then bounded by 1.0, continuous slope
  const float a = std::fabs(x);
  if (a <= OrganEngine::kKnee) return x;
  constexpr float room = 1.0f - OrganEngine::kKnee;
  const float y = OrganEngine::kKnee + room * std::tanh((a - OrganEngine::kKnee) / room);
  return x < 0.0f ? -y : y;
}

constexpr double kFs = 48000.0;

// SplitMix64: a tiny fixed-width generator, identical on every platform (key click noise and per-key seeds).
std::uint64_t splitmix64(std::uint64_t& state) noexcept {
  std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}
float unit_float(std::uint64_t bits) noexcept {  // [0, 1) from the top 24 bits
  return static_cast<float>(bits >> 40) * (1.0f / 16777216.0f);
}
}  // namespace

double OrganEngine::wheel_frequency_hz(std::uint32_t wheel) noexcept {
  const int midi = tonewheel::kWheelNoteOffset + static_cast<int>(wheel);
  return tonewheel::kReferenceA4Hz * std::exp2((midi - 69) / 12.0);
}

float OrganEngine::drawbar_level(int step) noexcept {
  if (step <= 0) return 0.0f;
  if (step > 8) step = 8;
  return std::pow(10.0f, -kDrawbarStepDb * static_cast<float>(8 - step) / 20.0f);
}

OrganEngine::OrganEngine()
    : voices_(kVoiceCapacity),
      osc_cos_(static_cast<std::size_t>(kPartCount) * kW, 1.0),
      osc_sin_(static_cast<std::size_t>(kPartCount) * kW, 0.0),
      wheels_(static_cast<std::size_t>(kPartCount) * kWheelStride, 0.0f) {
  for (std::uint32_t w = 0; w < kW; ++w) {
    const double inc = kTwoPi * wheel_frequency_hz(w + 1) / 48000.0;
    rot_cos_[w] = std::cos(inc);
    rot_sin_[w] = std::sin(inc);
  }
  for (Part& p : parts_)
    for (std::uint32_t d = 0; d < kD; ++d) {
      const params::ParamDef* def = params::find(params::kDrawbar16 + d);
      const float lvl = drawbar_level(def ? static_cast<int>(def->def) : 0);
      p.cur[d] = p.tgt[d] = lvl;
    }
  for (Part& p : parts_) {  // percussion and key click start from the registry defaults
    p.perc_on = params::find(params::kPercussionOn)->def >= 0.5f;
    p.perc_harmonic = static_cast<int>(params::find(params::kPercussionHarmonic)->def);
    p.perc_decay = static_cast<int>(params::find(params::kPercussionDecay)->def);
    p.perc_volume = static_cast<int>(params::find(params::kPercussionVolume)->def);
    p.key_click = params::find(params::kKeyClick)->def;
    apply_percussion_targets(p);
    p.cancel.cur = p.cancel.tgt;
    p.lower.cur = p.lower.tgt;
    p.cancel.left = p.lower.left = 0;
    // Scanner and leakage start from the registry defaults (Story 2.4); the scanner starts settled, with no crossfade.
    p.vc_on = params::find(params::kVibratoChorusOn)->def >= 0.5f;
    p.vc_type = static_cast<int>(params::find(params::kVibratoChorusType)->def);
    p.scan.set_mode_immediate(p.vc_on ? static_cast<ScannerMode>(1 + p.vc_type) : ScannerMode::kOff);
    p.leak.cur = p.leak.tgt = params::find(params::kLeakage)->def;
  }
  // Leakage pattern: fixed per-wheel, per-neighbour gains from a documented seed (same on every run and platform).
  std::uint64_t ls = kLeakageSeed;
  for (std::uint32_t w = 0; w < kW; ++w)
    for (std::uint32_t side = 0; side < 2; ++side) {
      const float r = unit_float(splitmix64(ls));  // always drawn, so the table does not shift with the edges
      const bool exists = side == 0 ? w > 0 : w + 1 < kW;
      leak_pattern_[w * 2 + side] = exists ? kLeakageMaxGain * (kLeakageMinFraction + (1.0f - kLeakageMinFraction) * r) : 0.0f;
    }
}

void OrganEngine::apply_percussion_targets(Part& p) noexcept {
  p.cancel.set(p.perc_on ? 0.0f : 1.0f);
  p.lower.set(p.perc_on && p.perc_volume == params::kPercussionVolumeNormal ? kPercDrawbarGain : 1.0f);
}

void OrganEngine::start_click(Voice& v, std::uint32_t which, float level, float scale) noexcept {
  Voice::Burst& b = v.click[which];
  if (!(level > 0.0f)) return;  // off: nothing new (a burst already under way finishes)
  // Per-key character (stable across presses): amplitude and decay of the burst depend on the key.
  std::uint64_t ks = kClickSeed ^ (static_cast<std::uint64_t>(v.note) * 0xD6E8FEB86659FD93ull);
  const std::uint64_t kbits = splitmix64(ks);
  const float key_amp = 0.75f + 0.5f * unit_float(kbits);
  const float tau = 14.0f + 20.0f * unit_float(kbits << 24);  // frames, 0.3 .. 0.7 ms
  // Per-press variation: the noise stream and a +-10 % level jitter.
  const float jitter = 0.9f + 0.2f * unit_float(splitmix64(v.click_rng));
  b.amp = level * kClickGain * scale * key_amp * jitter;
  b.coef = std::exp(-1.0f / tau);
  b.env = 1.0f;
  b.prev = 0.0f;
  b.rng = splitmix64(v.click_rng) ^ (which * 0xA5A5A5A55A5A5A5Aull);
  b.left = kClickFrames;
}

std::uint32_t OrganEngine::active_voices() const noexcept {
  std::uint32_t n = 0;
  for (const Voice& v : voices_) n += v.active ? 1 : 0;
  return n;
}

EngineStatus OrganEngine::start_voice(std::uint32_t channel, std::uint32_t note, std::uint32_t) noexcept {
  if (channel >= kPartCount || !(enabled_ & (1u << channel))) return EngineStatus::kEngineNotAvailable;
  bool other_key_held = false;
  for (const Voice& v : voices_) {
    if (!v.active || v.state != VoiceState::kHeld || v.channel != channel) continue;
    if (v.note == note) return EngineStatus::kOk;  // the key is already down: nothing to start (no retrigger)
    other_key_held = true;                         // single trigger: another key of the part is down
  }
  std::uint32_t slot = acquire_slot(channel, note);
  if (slot == VoiceAllocator::kNoSlot || slot >= kVoiceCapacity) {
    // All slots busy: reclaim the oldest releasing voice (a tail), never a held one.
    Voice* oldest = nullptr;
    for (Voice& v : voices_) {  // prefer a tail with no click pending (reclaiming one would cut its burst)
      if (!v.active || v.state != VoiceState::kReleasing) continue;
      if (!oldest || (oldest->clicking() && !v.clicking()) ||
          (oldest->clicking() == v.clicking() && v.start_time < oldest->start_time))
        oldest = &v;
    }
    if (!oldest) return EngineStatus::kPoolExhausted;
    oldest->active = false;
    slot = acquire_slot(channel, note);
    if (slot == VoiceAllocator::kNoSlot || slot >= kVoiceCapacity) return EngineStatus::kPoolExhausted;
  }
  Voice& v = voices_[slot];
  v = Voice{};
  v.active = true;
  v.part = static_cast<std::uint8_t>(channel);
  v.channel = channel;
  v.note = note;
  v.start_time = clock_;
  for (std::uint32_t d = 0; d < kD; ++d)
    v.wheel[d] = static_cast<std::uint8_t>(tonewheel::drawbar_wheel(static_cast<int>(note), d) - 1);
  const Part& p = parts_[channel];
  if (p.perc_on && !other_key_held) {  // percussion sounds only for a key pressed with no other key held
    v.perc_env = 1.0f;
    v.perc_drawbar = static_cast<std::uint8_t>(p.perc_harmonic == params::kPercussionHarmonicSecond ? kPercSecondDrawbar
                                                                                                     : kPercThirdDrawbar);
    v.perc_coef = std::exp(-1.0f / (percussion_tau_seconds(p.perc_decay) * static_cast<float>(kFs)));
    v.perc_gain = p.perc_volume == params::kPercussionVolumeNormal ? kPercNormalGain : kPercSoftGain;
  }
  Part& pm = parts_[channel];
  ++pm.press_count;  // each press has its own noise stream, so repeated notes are not identical
  std::uint64_t seed = kClickSeed ^ (pm.press_count * 0x9E3779B97F4A7C15ull) ^ (static_cast<std::uint64_t>(note) << 8) ^ channel;
  v.click_rng = splitmix64(seed);
  start_click(v, 0, p.key_click, 1.0f);
  return EngineStatus::kOk;
}

EngineStatus OrganEngine::stop_voice(std::uint32_t channel, std::uint32_t note) noexcept {
  bool found = false;
  for (Voice& v : voices_)
    if (v.active && v.state == VoiceState::kHeld && v.channel == channel && v.note == note) {
      v.state = VoiceState::kReleasing;
      found = true;
      start_click(v, 1, parts_[v.part].key_click, kClickReleaseRatio);  // the contact opens: release click
    }
  return found ? EngineStatus::kOk : EngineStatus::kVoiceNotFound;
}

void OrganEngine::set_param(std::uint32_t, float) noexcept {}

void OrganEngine::set_part_param(std::uint32_t part, std::uint32_t param_id, float value) noexcept {
  if (part >= kPartCount || !std::isfinite(value)) return;
  Part& p = parts_[part];
  switch (param_id) {
    case params::kPercussionOn:
      p.perc_on = value >= 0.5f;
      apply_percussion_targets(p);
      return;
    case params::kPercussionHarmonic:
      p.perc_harmonic = std::clamp(static_cast<int>(std::lround(value)), 0, params::kPercussionHarmonicCount - 1);
      return;
    case params::kPercussionDecay:
      p.perc_decay = std::clamp(static_cast<int>(std::lround(value)), 0, params::kPercussionDecayCount - 1);
      return;
    case params::kPercussionVolume:
      p.perc_volume = std::clamp(static_cast<int>(std::lround(value)), 0, params::kPercussionVolumeCount - 1);
      apply_percussion_targets(p);
      return;
    case params::kVibratoChorusOn:
      p.vc_on = value >= 0.5f;
      p.update_scanner(!part_sounding(part));
      return;
    case params::kVibratoChorusType:
      p.vc_type = std::clamp(static_cast<int>(std::lround(value)), 0, params::kVibratoChorusTypeCount - 1);
      p.update_scanner(!part_sounding(part));
      return;
    case params::kLeakage:
    {
      const float target = std::fmin(1.0f, std::fmax(0.0f, value));
      if (part_sounding(part)) {
        p.leak.set(target);  // 5 ms ramp so a moving control does not click
      } else {  // nothing sounds on this part: no ramp needed (also makes the frame-0 setup of a render exact)
        p.leak.cur = p.leak.tgt = target;
        p.leak.left = 0;
      }
      return;
    }
    case params::kKeyClick:
      p.key_click = std::fmin(1.0f, std::fmax(0.0f, value));
      return;
    default:
      break;
  }
  if (param_id < params::kDrawbar16 || param_id > params::kDrawbar1) return;
  const std::uint32_t d = param_id - params::kDrawbar16;
  const int step = static_cast<int>(std::lround(std::fmin(8.0f, std::fmax(0.0f, value))));
  const float target = drawbar_level(step);
  if (target == p.tgt[d]) return;
  p.tgt[d] = target;
  p.left[d] = kDrawbarRampFrames;  // the ramp starts now, at this event's frame, from the current gain
  p.step[d] = (target - p.cur[d]) / static_cast<float>(kDrawbarRampFrames);
}

void OrganEngine::begin_block(std::uint32_t frames) noexcept {
  block_pos_ = 0;
  gen_frames_ = std::min(frames, kBlockFrames);
  for (std::uint32_t part = 0; part < kPartCount; ++part) {
    if (!(enabled_ & (1u << part))) continue;
    double* c = &osc_cos_[static_cast<std::size_t>(part) * kW];
    double* s = &osc_sin_[static_cast<std::size_t>(part) * kW];
    float* out = &wheels_[part * kWheelStride];
    for (std::uint32_t w = 0; w < kW; ++w) {
      double cw = c[w], sw = s[w];
      const double rc = rot_cos_[w], rs = rot_sin_[w];
      float* row = out + static_cast<std::size_t>(w) * kBlockFrames;
      for (std::uint32_t f = 0; f < gen_frames_; ++f) {
        row[f] = static_cast<float>(sw);
        const double nc = cw * rc - sw * rs;
        sw = sw * rc + cw * rs;
        cw = nc;
      }
      const double norm = 1.0 / std::sqrt(cw * cw + sw * sw);  // keep the phasor on the unit circle
      c[w] = cw * norm;
      s[w] = sw * norm;
    }
  }
}

void OrganEngine::render(float* left, float* right, std::uint32_t frames) noexcept {
  while (frames > 0) {
    if (block_pos_ >= gen_frames_) begin_block(kBlockFrames);  // standalone use: next block
    const std::uint32_t n = std::min(frames, gen_frames_ - block_pos_);
    render_chunk(left, right, n);
    left += n;
    right += n;
    frames -= n;
  }
}

void OrganEngine::render_chunk(float* left, float* right, std::uint32_t frames) noexcept {
  std::array<bool, kPartCount> has_voice{};
  for (const Voice& v : voices_)
    if (v.active) has_voice[v.part] = true;

  constexpr float kAttackStep = 1.0f / static_cast<float>(kAttackFrames);
  constexpr float kReleaseStep = 1.0f / static_cast<float>(kReleaseFrames);

  for (std::uint32_t part = 0; part < kPartCount; ++part) {
    Part& p = parts_[part];
    if (!(enabled_ & (1u << part))) {
      p.scan.skip(frames);  // a disabled part keeps its sweep time and has an empty delay line
      p.scan_tail = 0;
      continue;
    }
    // Drawbar gains per frame. Ramps advance whether or not keys are down, so a ramp is the same wherever
    // a note starts inside it.
    // Percussion's part-level effects: the 1' cancel and the drawbar lowering, smoothed like the drawbars.
    const bool cancel_live = p.cancel.cur != 0.0f || p.cancel.tgt != 0.0f;
    float cancel_g[kBlockFrames], lower_g[kBlockFrames];
    for (std::uint32_t f = 0; f < frames; ++f) {
      cancel_g[f] = p.cancel.advance();
      lower_g[f] = p.lower.advance();
    }
    const bool leak_live = p.leak.cur != 0.0f || p.leak.tgt != 0.0f;
    float leak_g[kBlockFrames];
    for (std::uint32_t f = 0; f < frames; ++f) leak_g[f] = p.leak.advance();
    std::array<bool, kD> live{};
    for (std::uint32_t d = 0; d < kD; ++d) {
      live[d] = (p.cur[d] != 0.0f || p.tgt[d] != 0.0f) && (d != kD - 1 || cancel_live);
      float* g = &gains_[d * kBlockFrames];
      for (std::uint32_t f = 0; f < frames; ++f) {
        if (p.left[d] > 0) {
          p.cur[d] += p.step[d];
          if (--p.left[d] == 0) p.cur[d] = p.tgt[d];
        }
        g[f] = d == kD - 1 ? p.cur[d] * lower_g[f] * cancel_g[f] : p.cur[d] * lower_g[f];
      }
    }
    float* mix = &mix_[part * kBlockFrames];
    if (!has_voice[part]) {
      if (p.scan_tail == 0) {
        p.scan.skip(frames);  // silent part: the sweep keeps its time, the (empty) delay line stays empty
        continue;
      }
      // The last voice just ended: the scanner's delay line still holds up to 48 samples of signal. Run zeros through
      // it so that tail drains (is not cut), then skip.
      std::fill(mix, mix + frames, 0.0f);
      p.scan.process(mix, mix, frames);
      p.scan_tail -= std::min(p.scan_tail, frames);
      for (std::uint32_t f = 0; f < frames; ++f) {
        const float y = soft_limit(mix[f] * kVoiceScale);
        left[f] += y;
        right[f] += y;
      }
      continue;
    }
    p.scan_tail = VibratoScanner::kMaxDelaySamples + 16;  // > the delay line's reach
    std::fill(mix, mix + frames, 0.0f);
    const float* wheels = &wheels_[part * kWheelStride];
    for (Voice& v : voices_) {
      if (!v.active || v.part != part) continue;
      float tmp[kBlockFrames] = {};
      for (std::uint32_t d = 0; d < kD; ++d) {
        if (!live[d]) continue;
        const float* g = &gains_[d * kBlockFrames];
        const float* wr = wheels + static_cast<std::size_t>(v.wheel[d]) * kBlockFrames + block_pos_;
        for (std::uint32_t f = 0; f < frames; ++f) tmp[f] += g[f] * wr[f];
      }
      if (leak_live) {  // neighbouring wheels leak into each footage's wheel (fixed seeded pattern, scaled by leakage)
        for (std::uint32_t d = 0; d < kD; ++d) {
          if (!live[d]) continue;
          const std::uint32_t w = v.wheel[d];
          const float a = leak_pattern_[w * 2], b = leak_pattern_[w * 2 + 1];  // 0 at the ends of the wheel set
          const float* g = &gains_[d * kBlockFrames];
          const float* row = wheels + static_cast<std::size_t>(w) * kBlockFrames + block_pos_;
          const float* lo = w > 0 ? row - kBlockFrames : row;
          const float* hi = w + 1 < kW ? row + kBlockFrames : row;
          for (std::uint32_t f = 0; f < frames; ++f) tmp[f] += g[f] * leak_g[f] * (a * lo[f] + b * hi[f]);
        }
      }
      if (v.perc_env > 0.0f) {  // percussion: the chosen harmonic's wheel under a decaying envelope
        const float* wp = wheels + static_cast<std::size_t>(v.wheel[v.perc_drawbar]) * kBlockFrames + block_pos_;
        for (std::uint32_t f = 0; f < frames; ++f) {
          tmp[f] += v.perc_gain * v.perc_env * wp[f];
          v.perc_env *= v.perc_coef;
        }
        if (v.perc_env < 1e-6f) v.perc_env = 0.0f;  // flush: no denormals, and the envelope is over
      }
      for (std::uint32_t f = 0; f < frames; ++f) {
        if (v.state == VoiceState::kReleasing) {
          v.env -= kReleaseStep;
          if (v.env < 0.0f) v.env = 0.0f;
        } else if (v.env < 1.0f) {
          v.env = std::min(1.0f, v.env + kAttackStep);
        }
        float click = 0.0f;
        for (Voice::Burst& b : v.click) {
          if (b.left == 0) continue;  // broadband burst: differenced white noise under a fast exponential decay
          const float n = unit_float(splitmix64(b.rng)) * 2.0f - 1.0f;
          click += 0.5f * (n - b.prev) * b.env * b.amp;
          b.prev = n;
          b.env *= b.coef;
          --b.left;
        }
        mix[f] += v.env * tmp[f] + click;
        if (v.state == VoiceState::kReleasing && v.env <= 0.0f && !v.clicking()) {
          v.active = false;
          break;
        }
      }
    }
    p.scan.process(mix, mix, frames);  // vibrato/chorus on the part's summed signal
    for (std::uint32_t f = 0; f < frames; ++f) {
      const float y = soft_limit(mix[f] * kVoiceScale);
      left[f] += y;
      right[f] += y;
    }
  }
  block_pos_ += frames;
  clock_ += frames;
}

bool OrganEngine::voice_info(std::uint32_t slot, VoiceInfo& out) const noexcept {
  if (slot >= kVoiceCapacity || !voices_[slot].active) return false;
  const Voice& v = voices_[slot];
  float sum = 0.0f;
  for (std::uint32_t d = 0; d < kD; ++d) sum += parts_[v.part].cur[d];
  out.start_time = v.start_time;
  float burst = 0.0f;
  for (const Voice::Burst& b : v.click)
    if (b.left > 0) burst += b.env * b.amp;
  out.level = v.env * (sum + v.perc_env * v.perc_gain) * kVoiceScale + burst * kVoiceScale;  // tone, percussion, pending click
  out.state = v.state;
  out.channel = v.channel;
  out.note = v.note;
  return true;
}

bool OrganEngine::begin_steal_fade(std::uint32_t, std::uint32_t) noexcept { return false; }  // never stolen

}  // namespace velora::core
