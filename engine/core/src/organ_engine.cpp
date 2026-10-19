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
}

std::uint32_t OrganEngine::active_voices() const noexcept {
  std::uint32_t n = 0;
  for (const Voice& v : voices_) n += v.active ? 1 : 0;
  return n;
}

EngineStatus OrganEngine::start_voice(std::uint32_t channel, std::uint32_t note, std::uint32_t) noexcept {
  if (channel >= kPartCount || !(enabled_ & (1u << channel))) return EngineStatus::kEngineNotAvailable;
  for (const Voice& v : voices_)  // the key is already down: nothing to start (the organ has no retrigger)
    if (v.active && v.state == VoiceState::kHeld && v.channel == channel && v.note == note) return EngineStatus::kOk;
  std::uint32_t slot = acquire_slot(channel, note);
  if (slot == VoiceAllocator::kNoSlot || slot >= kVoiceCapacity) {
    // All slots busy: reclaim the oldest releasing voice (a tail), never a held one.
    Voice* oldest = nullptr;
    for (Voice& v : voices_)
      if (v.active && v.state == VoiceState::kReleasing && (!oldest || v.start_time < oldest->start_time)) oldest = &v;
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
  return EngineStatus::kOk;
}

EngineStatus OrganEngine::stop_voice(std::uint32_t channel, std::uint32_t note) noexcept {
  bool found = false;
  for (Voice& v : voices_)
    if (v.active && v.state == VoiceState::kHeld && v.channel == channel && v.note == note) {
      v.state = VoiceState::kReleasing;
      found = true;
    }
  return found ? EngineStatus::kOk : EngineStatus::kVoiceNotFound;
}

void OrganEngine::set_param(std::uint32_t, float) noexcept {}

void OrganEngine::set_part_param(std::uint32_t part, std::uint32_t param_id, float value) noexcept {
  if (part >= kPartCount || param_id < params::kDrawbar16 || param_id > params::kDrawbar1 || !std::isfinite(value))
    return;
  const std::uint32_t d = param_id - params::kDrawbar16;
  Part& p = parts_[part];
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
    if (!(enabled_ & (1u << part))) continue;
    // Drawbar gains per frame. Ramps advance whether or not keys are down, so a ramp is the same wherever
    // a note starts inside it.
    std::array<bool, kD> live{};
    for (std::uint32_t d = 0; d < kD; ++d) {
      live[d] = p.cur[d] != 0.0f || p.tgt[d] != 0.0f;
      float* g = &gains_[d * kBlockFrames];
      for (std::uint32_t f = 0; f < frames; ++f) {
        if (p.left[d] > 0) {
          p.cur[d] += p.step[d];
          if (--p.left[d] == 0) p.cur[d] = p.tgt[d];
        }
        g[f] = p.cur[d];
      }
    }
    if (!has_voice[part]) continue;
    float* mix = &mix_[part * kBlockFrames];
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
      for (std::uint32_t f = 0; f < frames; ++f) {
        if (v.state == VoiceState::kReleasing) {
          v.env -= kReleaseStep;
          if (v.env <= 0.0f) {
            v.active = false;
            break;
          }
        } else if (v.env < 1.0f) {
          v.env = std::min(1.0f, v.env + kAttackStep);
        }
        mix[f] += v.env * tmp[f];
      }
    }
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
  out.level = v.env * sum * kVoiceScale;
  out.state = v.state;
  out.channel = v.channel;
  out.note = v.note;
  return true;
}

bool OrganEngine::begin_steal_fade(std::uint32_t, std::uint32_t) noexcept { return false; }  // never stolen

}  // namespace velora::core
