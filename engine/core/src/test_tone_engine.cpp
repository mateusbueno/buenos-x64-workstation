#include "velora/core/test_tone_engine.hpp"

#include <cmath>

namespace velora::core {

TestToneEngine::TestToneEngine(Arena& arena, std::uint32_t max_voices) {
  voices_ = arena.allocate_array<Voice>(max_voices);
  capacity_ = voices_ ? max_voices : 0;
}

std::uint32_t TestToneEngine::active_voices() const noexcept {
  std::uint32_t n = 0;
  for (std::uint32_t i = 0; i < capacity_; ++i) n += voices_[i].active ? 1 : 0;
  return n;
}

EngineStatus TestToneEngine::start_voice(std::uint32_t channel, std::uint32_t note,
                                         std::uint32_t velocity16) noexcept {
  const std::uint32_t slot = acquire_slot(channel, note);
  if (slot != VoiceAllocator::kNoSlot && slot < capacity_) {
    Voice& v = voices_[slot];
    const float vel = static_cast<float>(velocity16 == 0 ? 1 : velocity16) / 65535.0f;
    v = Voice{};
    v.active = true;
    v.state = VoiceState::kHeld;
    v.channel = channel;
    v.note = note;
    v.start_time = clock_;
    v.phase = 0.25f;
    v.inc = 440.0f * std::pow(2.0f, (static_cast<float>(note) - 69.0f) / 12.0f) * detune_ratio_ / 48000.0f;
    v.env = 0.0f;
    v.amp = 0.25f * vel;
    return EngineStatus::kOk;
  }
  return EngineStatus::kPoolExhausted;  // no free slot and no victim: newest note-on dropped
}

EngineStatus TestToneEngine::stop_voice(std::uint32_t channel, std::uint32_t note) noexcept {
  bool found = false;
  for (std::uint32_t i = 0; i < capacity_; ++i) {
    Voice& v = voices_[i];
    if (v.active && !v.stolen && v.state == VoiceState::kHeld && v.channel == channel && v.note == note) {
      v.state = VoiceState::kReleasing;
      found = true;
    }
  }
  return found ? EngineStatus::kOk : EngineStatus::kVoiceNotFound;
}

void TestToneEngine::set_param(std::uint32_t param_id, float value) noexcept {
  if (!std::isfinite(value)) return;
  if (param_id == kParamGain) gain_ = value;
  else if (param_id == kParamDetuneCents) detune_ratio_ = std::pow(2.0f, std::fmax(-4800.0f, std::fmin(4800.0f, value)) / 1200.0f);
}

void TestToneEngine::render(float* left, float* right, std::uint32_t frames) noexcept {
  float* l[kSplitChannels];
  float* r[kSplitChannels];
  for (std::uint32_t c = 0; c < kSplitChannels; ++c) {
    l[c] = left;
    r[c] = right;
  }
  render_split(l, r, frames);
}

void TestToneEngine::render_split(float* const* left, float* const* right, std::uint32_t frames) noexcept {
  constexpr float kAttackStep = 1.0f / static_cast<float>(kAttackFrames);
  constexpr float kReleaseStep = 1.0f / static_cast<float>(kReleaseFrames);
  for (std::uint32_t i = 0; i < capacity_; ++i) {
    Voice& v = voices_[i];
    if (!v.active) continue;
    float* const lc = left[v.channel & (kSplitChannels - 1)];
    float* const rc = right[v.channel & (kSplitChannels - 1)];
    for (std::uint32_t f = 0; f < frames; ++f) {
      float fade = 1.0f;
      if (v.stolen) {  // steal ramp: gain fade_left/fade_total, strictly falling, 0 ends the voice
        if (v.fade_left == 0) {
          v.active = false;
          break;
        }
        fade = static_cast<float>(v.fade_left) / static_cast<float>(v.fade_total);
        --v.fade_left;
      }
      if (v.state == VoiceState::kReleasing) {
        v.env -= kReleaseStep;
        if (v.env <= 0.0f) {
          v.active = false;
          break;
        }
      } else if (v.env < 1.0f) {
        v.env += kAttackStep;
        if (v.env > 1.0f) v.env = 1.0f;
      }
      const float s = std::sin(6.28318530717958647692f * v.phase) * v.env * v.amp * gain_ * fade;
      lc[f] += s;
      rc[f] += s;
      v.phase += v.inc;
      if (v.phase >= 1.0f) v.phase -= 1.0f;
    }
  }
  clock_ += frames;
}

bool TestToneEngine::voice_info(std::uint32_t slot, VoiceInfo& out) const noexcept {
  if (slot >= capacity_ || !voices_[slot].active) return false;
  const Voice& v = voices_[slot];
  out.start_time = v.start_time;
  out.level = v.env * v.amp * gain_;
  out.state = v.stolen ? VoiceState::kReleasing : v.state;
  out.channel = v.channel;
  out.note = v.note;
  return true;
}

bool TestToneEngine::begin_steal_fade(std::uint32_t slot, std::uint32_t ramp_frames) noexcept {
  if (slot >= capacity_ || !voices_[slot].active || voices_[slot].stolen) return false;
  Voice& v = voices_[slot];
  v.stolen = true;
  v.fade_total = ramp_frames == 0 ? 1 : ramp_frames;
  v.fade_left = v.fade_total;
  return true;
}

}  // namespace velora::core
