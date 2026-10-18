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
  for (std::uint32_t i = 0; i < capacity_; ++i) {
    Voice& v = voices_[i];
    if (v.active) continue;
    const float vel = static_cast<float>(velocity16 == 0 ? 1 : velocity16) / 65535.0f;
    v.active = true;
    v.state = VoiceState::kHeld;
    v.channel = channel;
    v.note = note;
    v.start_time = clock_;
    v.phase = 0.25f;
    v.inc = 440.0f * std::pow(2.0f, (static_cast<float>(note) - 69.0f) / 12.0f) / 48000.0f;
    v.env = 0.0f;
    v.amp = 0.25f * vel;
    return EngineStatus::kOk;
  }
  return EngineStatus::kPoolExhausted;  // newest note-on dropped
}

EngineStatus TestToneEngine::stop_voice(std::uint32_t channel, std::uint32_t note) noexcept {
  bool found = false;
  for (std::uint32_t i = 0; i < capacity_; ++i) {
    Voice& v = voices_[i];
    if (v.active && v.state == VoiceState::kHeld && v.channel == channel && v.note == note) {
      v.state = VoiceState::kReleasing;
      found = true;
    }
  }
  return found ? EngineStatus::kOk : EngineStatus::kVoiceNotFound;
}

void TestToneEngine::set_param(std::uint32_t param_id, float value) noexcept {
  if (param_id == kParamGain && std::isfinite(value)) gain_ = value;
}

void TestToneEngine::render(float* left, float* right, std::uint32_t frames) noexcept {
  constexpr float kAttackStep = 1.0f / static_cast<float>(kAttackFrames);
  constexpr float kReleaseStep = 1.0f / static_cast<float>(kReleaseFrames);
  for (std::uint32_t i = 0; i < capacity_; ++i) {
    Voice& v = voices_[i];
    if (!v.active) continue;
    for (std::uint32_t f = 0; f < frames; ++f) {
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
      const float s = std::sin(6.28318530717958647692f * v.phase) * v.env * v.amp * gain_;
      left[f] += s;
      right[f] += s;
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
  out.state = v.state;
  return true;
}

}  // namespace velora::core
