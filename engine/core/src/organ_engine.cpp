#include "velora/core/organ_engine.hpp"

#include <algorithm>
#include <cmath>

#include "velora/core/params_gen.hpp"

namespace velora::core {

namespace {
constexpr std::uint32_t kD = tonewheel::kDrawbarCount;
constexpr std::uint32_t kW = tonewheel::kWheelCount;
constexpr double kTwoPi = 6.28318530717958647692;
static_assert(static_cast<std::uint8_t>(RotarySpeed::kStop) == params::kRotarySpeedStop &&
                  static_cast<std::uint8_t>(RotarySpeed::kSlow) == params::kRotarySpeedSlow &&
                  static_cast<std::uint8_t>(RotarySpeed::kFast) == params::kRotarySpeedFast,
              "RotarySpeed follows the registry enum rotary_speed");
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
    // Rotary speaker (Story 2.6): rotors settled at the registry default speed, stage on or off as the registry says.
    p.rot.set_speed_immediate(static_cast<RotarySpeed>(static_cast<int>(params::find(params::kRotarySpeed)->def)));
    p.rot_on = params::find(params::kRotaryOn)->def >= 0.5f;
    p.wet.cur = p.wet.tgt = p.rot_on ? 1.0f : 0.0f;
  }
  for (std::uint32_t part = 0; part < kPartCount; ++part) publish_rotary(part);
  // Leakage pattern: fixed per-wheel, per-neighbour gains from a documented seed (same on every run and platform).
  std::uint64_t ls = kLeakageSeed;
  for (std::uint32_t w = 0; w < kW; ++w)
    for (std::uint32_t side = 0; side < 2; ++side) {
      const float r = unit_float(splitmix64(ls));  // always drawn, so the table does not shift with the edges
      const bool exists = side == 0 ? w > 0 : w + 1 < kW;
      leak_pattern_[w * 2 + side] = exists ? kLeakageMaxGain * (kLeakageMinFraction + (1.0f - kLeakageMinFraction) * r) : 0.0f;
    }
}

void OrganEngine::publish_rotary(std::uint32_t part) noexcept {
  const Part& p = parts_[part];
  rot_state_[part].store(static_cast<std::uint32_t>(p.rot.target()) | (p.rot_on ? 4u : 0u), std::memory_order_release);
  rot_horn_[part].store(std::bit_cast<std::uint32_t>(static_cast<float>(p.rot.horn_hz())), std::memory_order_relaxed);
  rot_drum_[part].store(std::bit_cast<std::uint32_t>(static_cast<float>(p.rot.drum_hz())), std::memory_order_relaxed);
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

OrganEngine::Voice* OrganEngine::held_voice(std::uint32_t channel, std::uint32_t note) noexcept {
  for (Voice& v : voices_)
    if (v.active && v.state == VoiceState::kHeld && v.channel == channel && v.note == note) return &v;
  return nullptr;
}

EngineStatus OrganEngine::start_voice(std::uint32_t channel, std::uint32_t note, std::uint32_t) noexcept {
  if (channel >= kPartCount || !(enabled_ & (1u << channel))) return EngineStatus::kEngineNotAvailable;
  // The key is already down (a repeated Note On, or its voice was started by contact 1): nothing to start. No second voice, no
  // percussion retrigger, no click, and the contact groups keep following their own events.
  if (held_voice(channel, note)) return EngineStatus::kOk;
  return spawn_voice(channel, note, false);
}

// Starts the voice of key (channel, note); the caller checked that the part is enabled and that the key has no held voice.
// contact_driven: the voice begins silent (every group at 0, no contact closed) and the caller closes contact 1; otherwise this
// is a Note On voice with every busbar group in at once (the Story 2.2-2.4 behaviour, bit-identical).
EngineStatus OrganEngine::spawn_voice(std::uint32_t channel, std::uint32_t note, bool contact_driven) noexcept {
  bool other_key_held = false;
  for (const Voice& v : voices_)
    if (v.active && v.state == VoiceState::kHeld && v.channel == channel) other_key_held = true;  // single trigger: another key is down
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
  v.groups = static_cast<std::uint8_t>(contact_count_);
  if (contact_driven) {
    v.closed_mask = 0;
    v.gate_cur = {0.0f, 0.0f, 0.0f};
    v.gate_tgt = {0.0f, 0.0f, 0.0f};
  } else {
    v.closed_mask = static_cast<std::uint8_t>((1u << contact_count_) - 1u);
  }
  contact_released_[channel * 128 + (note & 127u)] = 0;
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

void OrganEngine::set_gate(Voice& v, std::uint32_t group, float target) noexcept {
  v.gate_tgt[group] = target;
  v.gate_left[group] = static_cast<std::uint8_t>(kContactRampFrames);  // the ramp starts at the next rendered frame = the event frame
  v.gate_step[group] = (target - v.gate_cur[group]) / static_cast<float>(kContactRampFrames);
}

void OrganEngine::release_key(Voice& v) noexcept {
  v.state = VoiceState::kReleasing;
  v.closed_mask = 0;
  contact_released_[v.channel * 128 + (v.note & 127u)] = 1;
  start_click(v, 1, parts_[v.part].key_click, kClickReleaseRatio);  // the last contact opens: release click
}

EngineStatus OrganEngine::stop_voice(std::uint32_t channel, std::uint32_t note) noexcept {
  bool found = false;
  for (Voice& v : voices_)
    if (v.active && v.state == VoiceState::kHeld && v.channel == channel && v.note == note) {
      v.state = VoiceState::kReleasing;
      v.closed_mask = 0;  // the key is up: any group still in follows the voice's 2 ms release together
      found = true;
      start_click(v, 1, parts_[v.part].key_click, kClickReleaseRatio);  // the contact opens: release click
    }
  if (found) return EngineStatus::kOk;
  // Note Off after the contacts already opened the key (contact_k open of the last contact): benign, repeatable.
  if (channel < kPartCount && contact_released_[channel * 128 + (note & 127u)]) return EngineStatus::kOk;
  return EngineStatus::kVoiceNotFound;
}

std::uint32_t OrganEngine::contact_mask(std::uint32_t channel, std::uint32_t note) const noexcept {
  for (const Voice& v : voices_)
    if (v.active && v.state == VoiceState::kHeld && v.channel == channel && v.note == note) return v.closed_mask;
  return 0;
}

// contact_k (Story 2.5). Defined behaviour for every sequence (nothing here can crash or loop):
//  * close of contact 1 with no held voice starts the voice at this frame (percussion and onset click as a Note On would);
//  * close of contact k > 1 brings group k-1 in, only while contact 1 is closed (i.e. the key has a held voice);
//  * open of a contact that is closed drops group k-1 and every closed group above it (a key cannot have contact 3 closed with contact 2
//    open), and the voice ends when no contact is left closed;
//  * everything else (duplicate close, open of a contact that is not itself closed even if a higher one is, close of k > 1 with no voice, k == 0 or above
//    contact_count()) changes nothing and is counted in contact_events_ignored().
EngineStatus OrganEngine::set_contact(std::uint32_t channel, std::uint32_t note, std::uint32_t contact, bool closed,
                                      ContactEffect& effect) noexcept {
  effect = ContactEffect::kNone;
  if (channel >= kPartCount || !(enabled_ & (1u << channel))) return EngineStatus::kEngineNotAvailable;
  if (note > 127 || contact < 1 || contact > contact_count_) {
    ++contact_ignored_;
    return EngineStatus::kOk;
  }
  const std::uint32_t bit = 1u << (contact - 1);
  Voice* v = held_voice(channel, note);
  if (closed) {
    if (!v) {
      if (contact != 1) {  // contact k closed with no contact 1: the key never went down
        ++contact_ignored_;
        return EngineStatus::kOk;
      }
      const EngineStatus st = spawn_voice(channel, note, true);
      if (st != EngineStatus::kOk) return st;
      v = held_voice(channel, note);
      if (!v) return EngineStatus::kPoolExhausted;  // cannot happen: spawn_voice just made it
      effect = ContactEffect::kStarted;
    } else if (v->closed_mask & bit) {  // duplicate close (or a Note On voice: every contact is closed)
      ++contact_ignored_;
      return EngineStatus::kOk;
    }
    v->closed_mask = static_cast<std::uint8_t>(v->closed_mask | bit);
    set_gate(*v, contact - 1, 1.0f);
    ++contact_applied_;
    return EngineStatus::kOk;
  }
  const std::uint32_t above = ~(bit - 1u) & ((1u << kMaxContacts) - 1u);  // this contact and every higher one
  if (!v || !(v->closed_mask & bit)) {  // open of a contact that is not closed (a higher one may be): ignored
    ++contact_ignored_;
    return EngineStatus::kOk;
  }
  for (std::uint32_t g = contact - 1; g < kMaxContacts; ++g)
    if (v->closed_mask & (1u << g)) set_gate(*v, g, 0.0f);
  v->closed_mask = static_cast<std::uint8_t>(v->closed_mask & ~above);
  ++contact_applied_;
  if (v->closed_mask == 0) {
    release_key(*v);
    effect = ContactEffect::kReleased;
  }
  return EngineStatus::kOk;
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
    case params::kRotarySpeed: {
      const RotarySpeed sp = static_cast<RotarySpeed>(std::clamp(static_cast<int>(std::lround(value)), 0, params::kRotarySpeedCount - 1));
      // While the stage is bypassed the rotors are inaudible: the command settles at once (a later switch-on starts from that
      // speed, and the frame-0 setup of an offline render is exact). Otherwise the rotors ramp from where they are, from this frame.
      // A stage switched on at this very frame with nothing sounding counts as bypassed, so "on" and "speed" given at the same
      // frame give the same result in either order.
      if (p.rot_live() && p.rot_on_frame != clock_) p.rot.set_target(sp);
      else p.rot.set_speed_immediate(sp);
      publish_rotary(part);
      return;
    }
    case params::kRotaryOn:
      p.rot_on = value >= 0.5f;
      if (part_sounding(part) || p.rot_tail > 0) {
        p.wet.set(p.rot_on ? 1.0f : 0.0f);  // 5 ms crossfade so switching does not click
      } else {  // nothing sounds on this part: no crossfade needed (also makes the frame-0 setup of a render exact)
        p.wet.cur = p.wet.tgt = p.rot_on ? 1.0f : 0.0f;
        p.wet.left = 0;
        if (p.rot_on) p.rot_on_frame = clock_;
      }
      publish_rotary(part);
      return;
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
  float* l[kSplitChannels];
  float* r[kSplitChannels];
  for (std::uint32_t c = 0; c < kSplitChannels; ++c) {
    l[c] = left;
    r[c] = right;
  }
  render_split(l, r, frames);
}

// Part p (a UMP channel 0..7) is added into left[p]/right[p]; a render() call has all of them equal.
void OrganEngine::render_split(float* const* left, float* const* right, std::uint32_t frames) noexcept {
  float* l[kPartCount];
  float* r[kPartCount];
  for (std::uint32_t p = 0; p < kPartCount; ++p) {
    l[p] = left[p];
    r[p] = right[p];
  }
  while (frames > 0) {
    if (block_pos_ >= gen_frames_) begin_block(kBlockFrames);  // standalone use: next block
    const std::uint32_t n = std::min(frames, gen_frames_ - block_pos_);
    render_chunk(l, r, n);
    for (std::uint32_t p = 0; p < kPartCount; ++p) {
      l[p] += n;
      r[p] += n;
    }
    frames -= n;
  }
}

// Adds one part's output for `frames` frames. Rotary off (stage bypassed, no crossfade running) adds the plain mono signal to both
// channels exactly as before Story 2.6; otherwise the signal goes through the part's rotary speaker and, while the stage is being
// switched on or off, is crossfaded with the plain signal over 5 ms.
void OrganEngine::emit_part(Part& p, const float* dry, std::uint32_t frames, float* left, float* right) noexcept {
  if (!p.rot_live()) {
    p.rot.skip(frames);  // the rotors keep time while the stage is bypassed
    if (p.rot_dirty) { p.rot.clear_signal(); p.rot_dirty = false; }
    for (std::uint32_t f = 0; f < frames; ++f) {
      left[f] += dry[f];
      right[f] += dry[f];
    }
    return;
  }
  float rl[kBlockFrames], rr[kBlockFrames];
  p.rot.process(dry, rl, rr, frames);
  p.rot_dirty = true;
  // The crossover and the two rotors can add up to more than the (soft-limited) dry signal on a saturated chord (measured up to 3x):
  // the same soft limit keeps the stage's output below 1.0 per channel. It is linear below the knee, so ordinary levels are untouched.
  for (std::uint32_t f = 0; f < frames; ++f) {
    rl[f] = soft_limit(rl[f]);
    rr[f] = soft_limit(rr[f]);
  }
  if (p.wet.left == 0 && p.wet.cur == 1.0f) {
    for (std::uint32_t f = 0; f < frames; ++f) {
      left[f] += rl[f];
      right[f] += rr[f];
    }
    return;
  }
  for (std::uint32_t f = 0; f < frames; ++f) {
    const float w = p.wet.advance();
    left[f] += (1.0f - w) * dry[f] + w * rl[f];
    right[f] += (1.0f - w) * dry[f] + w * rr[f];
  }
}

void OrganEngine::render_chunk(float* const* left, float* const* right, std::uint32_t frames) noexcept {
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
      p.rot_tail = 0;
      p.rot.skip(frames);  // the rotors keep time too
      if (p.rot_dirty) { p.rot.clear_signal(); p.rot_dirty = false; }
      for (std::uint32_t f = 0; p.wet.left > 0 && f < frames; ++f) p.wet.advance();
      publish_rotary(part);
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
      const bool rot_run = p.rot_live() && p.rot_tail > 0;
      if (p.scan_tail == 0 && !rot_run) {
        p.scan.skip(frames);  // silent part: the sweep keeps its time, the (empty) delay line stays empty
        p.rot.skip(frames);
        if (p.rot_dirty) { p.rot.clear_signal(); p.rot_dirty = false; }  // tail expired (stage on or off): nothing left to drain
        for (std::uint32_t f = 0; p.wet.left > 0 && f < frames; ++f) p.wet.advance();
        publish_rotary(part);
        continue;
      }
      // The last voice just ended: the scanner's delay line still holds up to 48 samples of signal and the rotary stage's
      // filters and delay lines some more. Run zeros through them so those tails drain (are not cut), then skip.
      std::fill(mix, mix + frames, 0.0f);
      if (p.scan_tail > 0) p.scan.process(mix, mix, frames);
      else p.scan.skip(frames);
      p.scan_tail -= std::min(p.scan_tail, frames);
      p.rot_tail -= std::min(p.rot_tail, frames);
      float dry[kBlockFrames];
      for (std::uint32_t f = 0; f < frames; ++f) dry[f] = soft_limit(mix[f] * kVoiceScale);
      emit_part(p, dry, frames, left[part], right[part]);
      publish_rotary(part);
      continue;
    }
    p.scan_tail = VibratoScanner::kMaxDelaySamples + 16;  // > the delay line's reach
    p.rot_tail = RotarySpeaker::kTailFrames;
    std::fill(mix, mix + frames, 0.0f);
    const float* wheels = &wheels_[part * kWheelStride];
    for (Voice& v : voices_) {
      if (!v.active || v.part != part) continue;
      float tmp[kBlockFrames] = {};
      // Contact groups (Story 2.5): per group, "in" (gate 1, the plain path), "out" (gate 0, skipped) or ramping (per-frame gate).
      // A Note On voice never ramps, so its sums are exactly the pre-2.5 ones.
      float gate_f[kMaxContacts][kBlockFrames];
      std::array<bool, kMaxContacts> gate_ramp{}, gate_on{};
      for (std::uint32_t gi = 0; gi < kMaxContacts; ++gi) {
        gate_on[gi] = v.gate_cur[gi] != 0.0f || v.gate_tgt[gi] != 0.0f;
        gate_ramp[gi] = v.gate_left[gi] > 0;
        if (!gate_ramp[gi]) continue;
        for (std::uint32_t f = 0; f < frames; ++f) {
          if (v.gate_left[gi] > 0) {
            v.gate_cur[gi] += v.gate_step[gi];
            if (--v.gate_left[gi] == 0) v.gate_cur[gi] = v.gate_tgt[gi];
          }
          gate_f[gi][f] = v.gate_cur[gi];
        }
      }
      for (std::uint32_t d = 0; d < kD; ++d) {
        if (!live[d]) continue;
        const std::uint32_t gi = contact_group(v.groups, d);
        if (!gate_on[gi]) continue;
        const float* g = &gains_[d * kBlockFrames];
        const float* wr = wheels + static_cast<std::size_t>(v.wheel[d]) * kBlockFrames + block_pos_;
        if (gate_ramp[gi]) {
          for (std::uint32_t f = 0; f < frames; ++f) tmp[f] += g[f] * gate_f[gi][f] * wr[f];
        } else {
          for (std::uint32_t f = 0; f < frames; ++f) tmp[f] += g[f] * wr[f];
        }
      }
      if (leak_live) {  // neighbouring wheels leak into each footage's wheel (fixed seeded pattern, scaled by leakage)
        for (std::uint32_t d = 0; d < kD; ++d) {
          if (!live[d]) continue;
          const std::uint32_t gi = contact_group(v.groups, d);
          if (!gate_on[gi]) continue;
          const std::uint32_t w = v.wheel[d];
          const float a = leak_pattern_[w * 2], b = leak_pattern_[w * 2 + 1];  // 0 at the ends of the wheel set
          const float* g = &gains_[d * kBlockFrames];
          const float* row = wheels + static_cast<std::size_t>(w) * kBlockFrames + block_pos_;
          const float* lo = w > 0 ? row - kBlockFrames : row;
          const float* hi = w + 1 < kW ? row + kBlockFrames : row;
          if (gate_ramp[gi]) {
            for (std::uint32_t f = 0; f < frames; ++f) tmp[f] += g[f] * gate_f[gi][f] * leak_g[f] * (a * lo[f] + b * hi[f]);
          } else {
            for (std::uint32_t f = 0; f < frames; ++f) tmp[f] += g[f] * leak_g[f] * (a * lo[f] + b * hi[f]);
          }
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
    float dry[kBlockFrames];
    for (std::uint32_t f = 0; f < frames; ++f) dry[f] = soft_limit(mix[f] * kVoiceScale);
    emit_part(p, dry, frames, left[part], right[part]);
    publish_rotary(part);
  }
  block_pos_ += frames;
  clock_ += frames;
}

bool OrganEngine::voice_info(std::uint32_t slot, VoiceInfo& out) const noexcept {
  if (slot >= kVoiceCapacity || !voices_[slot].active) return false;
  const Voice& v = voices_[slot];
  float sum = 0.0f;
  for (std::uint32_t d = 0; d < kD; ++d) sum += parts_[v.part].cur[d] * v.gate_cur[contact_group(v.groups, d)];
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
