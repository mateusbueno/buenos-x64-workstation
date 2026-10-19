#include "velora/core/sample_engine.hpp"

#include <algorithm>
#include <cmath>

namespace velora::core {

namespace {

// --- constants shared with the sfizz reference (see engine/core/README.md for the sources) -----
constexpr float kReleaseThreshold = 1e-4f;        // exponential release hands over to the fade-out below this
constexpr float kTransitionTime = 50e-3f;         // linear fade-out time after the exponential release
constexpr float kFastReleaseTime = 6e-3f;         // minimum exponential time and the "off" time at sample end
constexpr float kDefaultRelease = 0.001f;         // ampeg_release when the region does not carry one (<= 0)
constexpr float kFreeRunningSustain = 0.0032f;    // sustain at or below this releases by itself
constexpr float kVirtuallyZero = 0.001f;
constexpr int kPanSize = 4095;
constexpr float kMonoPanCompensation = 1.4125375446227544f;    // +3 dB, one pan stage
constexpr float kStereoPanCompensation = 1.9952623149688797f;  // +6 dB, pan and position stages

constexpr float kMaxRatio = 1024.0f, kMinRatio = 1.0f / 4096.0f;
constexpr float kMaxSeconds = 100.0f;

bool finite_time(float t) noexcept { return std::isfinite(t) && t >= 0.0f && t <= kMaxSeconds; }

// Everything the audio thread later turns into counts, rates and gains must be finite and bounded.
bool region_params_sane(const velora::ports::SampleRegionDesc& d) noexcept {
  return finite_time(d.ampeg_delay) && finite_time(d.ampeg_attack) && finite_time(d.ampeg_hold) &&
         finite_time(d.ampeg_decay) && finite_time(d.ampeg_release) && std::isfinite(d.ampeg_start) &&
         std::isfinite(d.ampeg_sustain) && std::isfinite(d.lorand) && std::isfinite(d.hirand) &&
         d.lorand < d.hirand && d.seq_length >= 1 && d.seq_position >= 1 && d.seq_position <= d.seq_length &&
         std::isfinite(d.tune_cents) && std::isfinite(d.volume_db) && d.volume_db >= -144.0f &&
         d.volume_db <= 48.0f && std::isfinite(d.pan);
}

constexpr double kHalfPi = 1.57079632679489661923;

// sfizz maps a SFZ time to samples/rates in single precision; so do we.
std::int32_t seconds_to_samples(float t, float sr) noexcept {
  return t <= 0.0f ? 0 : static_cast<std::int32_t>(t * sr);
}
float seconds_to_lin_rate(float t, float sr) noexcept { return t <= 0.0f ? 1.0f : 1.0f / (sr * t); }
float seconds_to_exp_rate(float t, float sr) noexcept {
  if (t <= 0.0f) return 0.0f;
  t = std::max(kFastReleaseTime, t);
  return std::exp(-9.0f / (t * sr));
}

float pan_lookup(const std::array<float, 4096>& table, float p) noexcept {
  const long idx = static_cast<long>(std::lround(p * static_cast<float>(kPanSize - 1)));
  return table[static_cast<std::size_t>(std::clamp(idx, 0L, static_cast<long>(kPanSize)))];
}

inline void hermite_weights(float c, float w[4]) noexcept {
  for (int i = -1; i < 3; ++i) {
    const float x = std::fabs(static_cast<float>(i) - c);
    const float x2 = x * x, x3 = x2 * x;
    const float q = 2.5f * x2;
    const float p1 = 1.0f - q + 1.5f * x3;
    const float p2 = 2.0f - 4.0f * x + q - 0.5f * x3;
    float y = 0.0f;
    y = (x < 2.0f) ? p2 : y;
    y = (x < 1.0f) ? p1 : y;
    w[i + 1] = y;
  }
}

}  // namespace

SampleEngine::SampleEngine(const Config& cfg) : cfg_(cfg) {
  if (cfg_.max_voices == 0) cfg_.max_voices = 1;
  if (cfg_.sample_rate == 0) cfg_.sample_rate = 48000;
  capacity_ = cfg_.max_voices;
  voices_.resize(capacity_);
  for (int i = 0; i < kPanSize; ++i)
    pan_table_[static_cast<std::size_t>(i)] = static_cast<float>(std::cos(i * (kHalfPi / (kPanSize - 1))));
  for (std::size_t i = kPanSize; i < pan_table_.size(); ++i) pan_table_[i] = pan_table_[kPanSize - 1];
  tr_release_rate_off_ = seconds_to_exp_rate(kFastReleaseTime, static_cast<float>(cfg_.sample_rate));
  set_seed(cfg_.seed);
}

void SampleEngine::set_seed(std::uint64_t seed) noexcept {
  // splitmix64 scramble so that nearby seeds give unrelated streams; never zero for xorshift.
  std::uint64_t z = seed + 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  rng_ = z ? z : 0x2545F4914F6CDD1Dull;
}

float SampleEngine::next_random() noexcept {
  // xorshift64*; the top 24 bits give a float uniformly distributed in [0, 1).
  std::uint64_t x = rng_;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  rng_ = x;
  const std::uint64_t r = x * 0x2545F4914F6CDD1Dull;
  return static_cast<float>(r >> 40) * (1.0f / 16777216.0f);
}

bool SampleEngine::set_instrument(const velora::ports::SampleSource& src) {
  clear_instrument();
  const velora::ports::InstrumentView view = src.instrument();
  const float sr = static_cast<float>(cfg_.sample_rate);
  regions_.reserve(view.regions.size());
  for (const auto& d : view.regions) {
    RegionRt r;
    r.d = d;
    if (d.sample_index < view.samples.size()) {
      const velora::ports::SamplePcm& pcm = view.samples[d.sample_index];
      r.frames = pcm.frames;
      r.left = pcm.left;
      r.right = pcm.right;
      r.stereo = pcm.channels == 2;
      r.speed = pcm.sample_rate == 0 ? 0.0f
                                     : static_cast<float>(static_cast<double>(pcm.sample_rate) /
                                                          static_cast<double>(cfg_.sample_rate));
      r.playable = (pcm.channels == 1 || pcm.channels == 2) && pcm.frames > 0 && pcm.left != nullptr &&
                   (pcm.channels == 1 || pcm.right != nullptr) && pcm.sample_rate != 0 &&
                   d.offset < pcm.frames && d.key_lo <= d.key_hi && d.vel_lo <= d.vel_hi &&
                   region_params_sane(d);
      if (r.playable) {
        // Pitch ratio at the extreme keys of the range must be finite and sane.
        const float spd = r.speed;
        const float base = d.tune_cents + 100.0f * static_cast<float>(d.transpose);
        const float up = 100.0f * static_cast<float>(static_cast<int>(d.key_hi) - static_cast<int>(d.key_center)) + base;
        const float dn = 100.0f * static_cast<float>(static_cast<int>(d.key_lo) - static_cast<int>(d.key_center)) + base;
        const float rmax = std::pow(2.0f, up / 1200.0f) * spd, rmin = std::pow(2.0f, dn / 1200.0f) * spd;
        r.playable = std::isfinite(rmax) && std::isfinite(rmin) && rmax <= kMaxRatio && rmin >= kMinRatio;
      }
    }
    if (!r.playable) ++unplayable_regions_;

    r.uses_sequence = d.seq_length != 1 || d.seq_position != 1;

    // Loops: only the two looping modes, with the range inside the sample (otherwise no loop, as sfizz).
    if (d.loop_mode == velora::ports::SampleLoopMode::kLoopContinuous ||
        d.loop_mode == velora::ports::SampleLoopMode::kLoopSustain) {
      std::int64_t ls = 0, le = static_cast<std::int64_t>(r.frames) - 1;
      if (d.has_loop_points) {
        ls = static_cast<std::int64_t>(std::min<std::uint64_t>(d.loop_start, INT64_MAX));
        le = static_cast<std::int64_t>(std::min<std::uint64_t>(d.loop_end, INT64_MAX));
      }
      if (r.frames > 0 && ls <= le && le < static_cast<std::int64_t>(r.frames)) {
        r.loops = true;
        r.loop_start = ls;
        r.loop_end = le;
        r.loop_size = le + 1 - ls;
      }
    }

    r.base_cents = d.tune_cents + 100.0f * static_cast<float>(d.transpose);
    r.vol_gain = std::pow(10.0f, d.volume_db * 0.05f);
    const float p = std::clamp((d.pan / 100.0f + 1.0f) * 0.5f, 0.0f, 1.0f);
    const float pl = pan_lookup(pan_table_, p), pr = pan_lookup(pan_table_, 1.0f - p);
    if (r.stereo) {
      const float pos = pan_lookup(pan_table_, 0.5f);  // position stage at 0
      r.chan_l = pl * pos * kStereoPanCompensation;
      r.chan_r = pr * pos * kStereoPanCompensation;
    } else {
      r.chan_l = pl * kMonoPanCompensation;
      r.chan_r = pr * kMonoPanCompensation;
    }

    r.env_delay = seconds_to_samples(d.ampeg_delay, sr);
    r.env_hold = seconds_to_samples(d.ampeg_hold, sr);
    r.env_start = std::clamp(d.ampeg_start / 100.0f, 0.0f, 1.0f);
    r.env_attack_step = seconds_to_lin_rate(d.ampeg_attack, sr);
    r.env_decay_rate = seconds_to_exp_rate(d.ampeg_decay, sr);
    r.env_sustain = std::clamp(d.ampeg_sustain / 100.0f, 0.0f, 1.0f);
    r.env_release_rate = seconds_to_exp_rate(d.ampeg_release > 0.0f ? d.ampeg_release : kDefaultRelease, sr);
    r.env_free_running = r.env_sustain <= kFreeRunningSustain;
    regions_.push_back(r);
  }

  // Per-note lookup, regions in table order (a note's regions are visited in that order).
  std::array<std::uint32_t, 128> counts{};
  for (const auto& r : regions_)
    for (unsigned n = r.d.key_lo; n <= std::min<unsigned>(r.d.key_hi, 127); ++n) ++counts[n];
  note_begin_[0] = 0;
  for (unsigned n = 0; n < 128; ++n) note_begin_[n + 1] = note_begin_[n] + counts[n];
  note_list_.assign(note_begin_[128], 0);
  std::array<std::uint32_t, 128> fill{};
  for (std::uint32_t i = 0; i < regions_.size(); ++i)
    for (unsigned n = regions_[i].d.key_lo; n <= std::min<unsigned>(regions_[i].d.key_hi, 127); ++n)
      note_list_[note_begin_[n] + fill[n]++] = i;

  seq_counter_.assign(regions_.size(), 0);
  seq_switched_.assign(regions_.size(), 0);
  for (std::size_t i = 0; i < regions_.size(); ++i) seq_switched_[i] = regions_[i].uses_sequence ? 0 : 1;
  return true;
}

void SampleEngine::clear_instrument() {
  for (auto& v : voices_) v = Voice{};
  regions_.clear();
  note_list_.clear();
  note_begin_.fill(0);
  seq_counter_.clear();
  seq_switched_.clear();
  last_vel7_.fill(0);
  unplayable_regions_ = 0;
  skipped_unplayable_ = 0;
}

std::uint32_t SampleEngine::active_voices() const noexcept {
  std::uint32_t n = 0;
  for (const auto& v : voices_) n += v.active ? 1 : 0;
  return n;
}

// --- amplitude envelope: a sample-by-sample port of sfizz's ADSREnvelope --------------------------

void SampleEngine::init_env(Env& e, const RegionRt& r) const noexcept {
  e.s = EnvState::kDelay;
  e.v = r.env_start;
  e.delay_left = r.env_delay;
  e.hold_left = r.env_hold;
  e.transition_delta = 0.0f;
  e.release_rate = r.env_release_rate;
}

float SampleEngine::env_next(Env& e, const RegionRt& r) const noexcept {
  const float sr = static_cast<float>(cfg_.sample_rate);
  for (;;) {
    switch (e.s) {
      case EnvState::kDelay:
        if (e.delay_left-- > 0) {
          e.v = r.env_start;
          return e.v;
        }
        e.s = EnvState::kAttack;
        continue;
      case EnvState::kAttack:
        e.v += r.env_attack_step;
        if (e.v < 1.0f) return e.v;
        e.v = 1.0f;
        e.s = EnvState::kHold;
        continue;
      case EnvState::kHold:
        if (e.hold_left-- > 0) return e.v;
        e.s = EnvState::kDecay;
        continue;
      case EnvState::kDecay: {
        e.v *= r.env_decay_rate;
        if (e.v > r.env_sustain) {
          const float out = e.v;
          if (e.v <= r.env_sustain + kVirtuallyZero) {
            e.s = EnvState::kSustain;
            e.transition_delta = (r.env_sustain - e.v) / (sr * kTransitionTime);
          }
          return out;
        }
        e.v = std::max(r.env_sustain, e.v);
        e.s = EnvState::kSustain;
        e.transition_delta = (r.env_sustain - e.v) / (sr * kTransitionTime);
        continue;
      }
      case EnvState::kSustain:
        if (r.env_free_running) {  // a (near) zero sustain level releases by itself
          e.s = EnvState::kRelease;
          continue;
        }
        if (e.v > r.env_sustain) e.v += e.transition_delta;
        return e.v;
      case EnvState::kRelease: {
        const float nv = e.v * e.release_rate;
        if (nv > kReleaseThreshold) {
          e.v = nv;
          return nv;
        }
        // Exponential tail exhausted: linear fade-out from the last emitted value.
        e.s = EnvState::kFadeout;
        e.transition_delta = -std::max(kReleaseThreshold, e.v) / (sr * kTransitionTime);
        continue;
      }
      case EnvState::kFadeout: {
        const float nv = e.v + e.transition_delta;
        if (nv > 0.0f) {
          e.v = nv;
          return nv;
        }
        e.s = EnvState::kDone;
        e.v = 0.0f;
        continue;
      }
      case EnvState::kDone:
      default:
        e.v = 0.0f;
        return 0.0f;
    }
  }
}

bool SampleEngine::env_release(Env& e, const RegionRt&) const noexcept {
  if (e.s == EnvState::kDelay && e.delay_left > 0) return false;  // released before it ever sounded
  if (e.s < EnvState::kRelease) e.s = EnvState::kRelease;
  return true;
}

void SampleEngine::env_fast_release(Env& e) const noexcept {
  e.release_rate = tr_release_rate_off_;
  if (e.s < EnvState::kRelease) e.s = EnvState::kRelease;
}

// --- voices ---------------------------------------------------------------------------------------

bool SampleEngine::start_region(std::uint32_t region, std::uint32_t channel, std::uint32_t note,
                                std::uint8_t vel7, bool release_trigger) noexcept {
  Voice* v = nullptr;
  for (auto& c : voices_) {
    if (!c.active) {
      v = &c;
      break;
    }
  }
  if (!v) return false;
  const RegionRt& r = regions_[region];

  // Pitch: key tracking (100 cents per key) from pitch_keycenter, tune, transpose; single precision.
  float cents = 100.0f * (static_cast<float>(note) - static_cast<float>(r.d.key_center));
  cents += r.base_cents;
  const float ratio = std::pow(2.0f, cents / 1200.0f) * r.speed;

  // Amplitude velocity tracking at the SFZ default amp_veltrack=100: gain = velocity squared.
  const float vf = static_cast<float>(vel7) / 127.0f;
  const float vgain = 1.0f - 1.0f * (1.0f - vf * vf);
  const float base = vgain * r.vol_gain;

  *v = Voice{};
  v->active = true;
  v->release_trigger = release_trigger;
  v->channel = channel;
  v->note = note;
  v->region = region;
  v->vel7 = vel7;
  v->start_time = clock_;
  v->pos = static_cast<double>(r.d.offset);
  v->ratio = ratio;
  v->gain_l = base * r.chan_l;
  v->gain_r = base * r.chan_r;
  init_env(v->env, r);
  return true;
}

bool SampleEngine::playing_attack_voice(const RegionRt& r, std::uint32_t channel) const noexcept {
  // sfizz rule: a release region only fires while an attack voice of a compatible note is alive.
  for (const auto& v : voices_) {
    if (v.active && !v.release_trigger && v.channel == channel && v.note >= r.d.key_lo && v.note <= r.d.key_hi &&
        v.vel7 >= r.d.vel_lo && v.vel7 <= r.d.vel_hi)
      return true;
  }
  return false;
}

EngineStatus SampleEngine::start_voice(std::uint32_t channel, std::uint32_t note,
                                       std::uint32_t velocity16) noexcept {
  if (note > 127 || regions_.empty()) return EngineStatus::kOk;
  channel &= 15u;
  const std::uint32_t v16 = std::min<std::uint32_t>(velocity16, 65535u);
  // 7-bit velocity, as MIDI 1.0 and SFZ define it (the UMP value is rounded to the nearest step).
  std::uint8_t vel7 = static_cast<std::uint8_t>((v16 * 127u + 32767u) / 65535u);
  if (vel7 == 0) vel7 = 1;  // a note on is never velocity 0
  last_vel7_[(channel & 15u) * 128u + note] = vel7;
  const float rnd = next_random();

  bool exhausted = false;
  for (std::uint32_t i = note_begin_[note]; i < note_begin_[note + 1]; ++i) {
    const std::uint32_t ri = note_list_[i];
    const RegionRt& r = regions_[ri];
    // Round robin: every region whose key range holds the note advances its own counter.
    seq_switched_[ri] = (seq_counter_[ri]++ % r.d.seq_length) == r.d.seq_position - 1 ? 1 : 0;
    if (r.d.trigger != velora::ports::SampleTrigger::kAttack) continue;
    if (!seq_switched_[ri]) continue;
    if (vel7 < r.d.vel_lo || vel7 > r.d.vel_hi) continue;
    if (!(rnd >= r.d.lorand && rnd < r.d.hirand)) continue;
    if (!r.playable) {
      ++skipped_unplayable_;
      continue;
    }
    if (!start_region(ri, channel, note, vel7, false)) exhausted = true;
  }
  return exhausted ? EngineStatus::kPoolExhausted : EngineStatus::kOk;
}

EngineStatus SampleEngine::stop_voice(std::uint32_t channel, std::uint32_t note) noexcept {
  if (note > 127) return EngineStatus::kVoiceNotFound;
  channel &= 15u;
  bool found = false;
  for (auto& v : voices_) {
    if (!v.active || v.release_trigger || v.channel != channel || v.note != note || v.key_up) continue;
    const RegionRt& r = regions_[v.region];
    if (v.env.s >= EnvState::kRelease) continue;  // already releasing
    found = true;
    if (r.d.loop_mode == velora::ports::SampleLoopMode::kOneShot) {
      v.key_up = true;  // one_shot ignores the note off
      continue;
    }
    if (!env_release(v.env, r)) v.active = false;
  }

  // Release-trigger regions: the note-on velocity, a fresh random draw.
  const float rnd = next_random();
  const std::uint8_t vel7 = last_vel7_[(channel & 15u) * 128u + note];
  bool exhausted = false;
  for (std::uint32_t i = note_begin_[note]; i < note_begin_[note + 1]; ++i) {
    const std::uint32_t ri = note_list_[i];
    const RegionRt& r = regions_[ri];
    if (r.d.trigger != velora::ports::SampleTrigger::kRelease) continue;
    if (!seq_switched_[ri]) continue;
    if (vel7 < r.d.vel_lo || vel7 > r.d.vel_hi) continue;
    if (!(rnd >= r.d.lorand && rnd < r.d.hirand)) continue;
    if (!playing_attack_voice(r, channel)) continue;
    if (!r.playable) {
      ++skipped_unplayable_;
      continue;
    }
    if (start_region(ri, channel, note, vel7, true)) found = true;
    else exhausted = true;
  }
  if (exhausted) return EngineStatus::kPoolExhausted;
  return found ? EngineStatus::kOk : EngineStatus::kVoiceNotFound;
}

void SampleEngine::set_param(std::uint32_t param_id, float value) noexcept {
  if (!std::isfinite(value)) return;
  if (param_id == kParamGain) gain_ = std::clamp(value, 0.0f, kMaxGain);
  else if (param_id == kParamSeed && value >= 0.0f && value < 9.0e18f) set_seed(static_cast<std::uint64_t>(value));
}

void SampleEngine::render(float* left, float* right, std::uint32_t frames) noexcept {
  for (auto& v : voices_)
    if (v.active) render_voice(v, left, right, frames);
  clock_ += frames;
}

void SampleEngine::render_voice(Voice& v, float* left, float* right, std::uint32_t frames) noexcept {
  const RegionRt& r = regions_[v.region];
  const bool linear = cfg_.interpolation == SampleInterpolation::kLinear;
  const std::int64_t nframes = static_cast<std::int64_t>(r.frames);
  const std::int64_t end_idx = nframes - 2;  // sfizz: positions at or past this index end the sample
  const bool sustain_loop = r.d.loop_mode == velora::ports::SampleLoopMode::kLoopSustain;
  const float gl = v.gain_l * gain_, gr = v.gain_r * gain_;

  const std::uint64_t oracle_n = cfg_.oracle_block_frames;
  for (std::uint32_t f = 0; f < frames; ++f) {
    if (oracle_n != 0 && !v.stalled) {
      const std::uint64_t abs = clock_ + f;
      if (abs % oracle_n == 0 && abs / oracle_n > v.start_time / oracle_n) {
        v.pos = std::max(0.0, v.pos - static_cast<double>(v.ratio));  // repeat the previous frame's position once
        v.stalled = true;
      }
    }
    // Loop wrap: continuous loops always, sustain loops until the envelope is released.
    const bool looping = r.loops && !v.release_trigger && (!sustain_loop || v.env.s < EnvState::kRelease);
    std::int64_t idx = static_cast<std::int64_t>(std::floor(v.pos));
    if (looping) {
      if (idx > r.loop_end) {  // same as subtracting the loop size repeatedly, in bounded time
        v.pos = static_cast<double>(r.loop_start) +
                std::fmod(v.pos - static_cast<double>(r.loop_start), static_cast<double>(r.loop_size));
        idx = static_cast<std::int64_t>(std::floor(v.pos));
      }
    } else if (idx >= end_idx && !v.ended) {
      // Sample exhausted: a 6 ms release starts on this very frame (sfizz turns the voice off at
      // the frame where the position reaches the end) while the last frame is held below.
      v.ended = true;
      env_fast_release(v.env);
    }

    const float env = env_next(v.env, r);
    if (v.env.s == EnvState::kDone) {
      v.active = false;
      return;
    }

    float sl, sr_;
    if (!looping && idx >= end_idx) {
      sl = r.left[nframes - 1];
      sr_ = r.stereo ? r.right[nframes - 1] : sl;
    } else {
      const float c = static_cast<float>(v.pos - static_cast<double>(idx));
      if (linear) {
        const float a = 1.0f - c;
        const float l0 = (idx >= 0 && idx < nframes) ? r.left[idx] : 0.0f, l1 = (idx >= -1 && idx + 1 < nframes) ? r.left[idx + 1] : 0.0f;
        sl = l0 * a + l1 * c;
        if (r.stereo) {
          const float r0 = (idx >= 0 && idx < nframes) ? r.right[idx] : 0.0f, r1 = (idx >= -1 && idx + 1 < nframes) ? r.right[idx + 1] : 0.0f;
          sr_ = r0 * a + r1 * c;
        } else {
          sr_ = sl;
        }
      } else {
        float w[4];
        hermite_weights(c, w);
        if (idx >= 1 && idx + 2 < nframes) {
          const float* p = r.left + idx;
          sl = w[0] * p[-1] + w[1] * p[0] + w[2] * p[1] + w[3] * p[2];
          if (r.stereo) {
            const float* q = r.right + idx;
            sr_ = w[0] * q[-1] + w[1] * q[0] + w[2] * q[1] + w[3] * q[2];
          } else {
            sr_ = sl;
          }
        } else {
          auto tap = [&](const float* d, std::int64_t i) { return (i < 0 || i >= nframes) ? 0.0f : d[i]; };
          sl = w[0] * tap(r.left, idx - 1) + w[1] * tap(r.left, idx) + w[2] * tap(r.left, idx + 1) +
               w[3] * tap(r.left, idx + 2);
          if (r.stereo)
            sr_ = w[0] * tap(r.right, idx - 1) + w[1] * tap(r.right, idx) + w[2] * tap(r.right, idx + 1) +
                  w[3] * tap(r.right, idx + 2);
          else
            sr_ = sl;
        }
      }
    }

    left[f] += sl * env * gl;
    right[f] += sr_ * env * gr;
    v.pos += static_cast<double>(v.ratio);
  }
}

bool SampleEngine::voice_info(std::uint32_t slot, VoiceInfo& out) const noexcept {
  if (slot >= capacity_ || !voices_[slot].active) return false;
  const Voice& v = voices_[slot];
  out.start_time = v.start_time;
  out.level = v.env.v * std::max(v.gain_l, v.gain_r) * gain_;
  if (v.release_trigger || v.env.s >= EnvState::kRelease) out.state = VoiceState::kReleasing;
  else if (v.key_up) out.state = VoiceState::kSustained;
  else out.state = VoiceState::kHeld;
  return true;
}

}  // namespace velora::core
