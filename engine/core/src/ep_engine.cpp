#include "velora/core/ep_engine.hpp"

#include <algorithm>
#include <cstring>

namespace velora::core {

EpEngine::EpEngine(float sample_rate, uint32_t voice_capacity)
    : sample_rate_(sample_rate > 8000.0f ? sample_rate : 48000.0f) {
  for (uint32_t i = 0; i < kPartCount; ++i) {
    base_params_[i] = create_default_tine_parameters();
    part_params_[i] = base_params_[i];
    part_sustain_[i] = false;
    part_pitch_bend_[i] = 0.0f;
  }

  uint32_t cap = std::clamp(voice_capacity, 1u, 256u);
  voices_.resize(cap);
  for (auto& v : voices_) {
    v.init(sample_rate_);
  }
}

EngineStatus EpEngine::start_voice(uint32_t channel, uint32_t note, uint32_t velocity16) noexcept {
  uint32_t part = channel % kPartCount;
  uint8_t note7 = static_cast<uint8_t>(std::clamp(note, 0u, 127u));
  uint32_t slot = acquire_slot(channel, note7);
  if (slot == VoiceAllocator::kNoSlot || slot >= voices_.size()) {
    return EngineStatus::kPoolExhausted;
  }

  voices_[slot].start(channel, note7, velocity16, part_params_[part],
                      part_pitch_bend_[part], frame_clock_);
  if (part_sustain_[part]) {
    voices_[slot].set_sustain(true);
  }

  return EngineStatus::kOk;
}

EngineStatus EpEngine::stop_voice(uint32_t channel, uint32_t note) noexcept {
  bool found = false;
  uint8_t note7 = static_cast<uint8_t>(std::clamp(note, 0u, 127u));
  for (auto& v : voices_) {
    if (v.is_active() && v.channel() == channel && v.note() == note7 &&
        (v.state() == VoiceState::kHeld || v.state() == VoiceState::kSustained)) {
      v.stop(64);
      found = true;
    }
  }
  return found ? EngineStatus::kOk : EngineStatus::kVoiceNotFound;
}

EngineStatus EpEngine::release_voice(uint32_t channel, uint32_t note, uint32_t release_velocity16) noexcept {
  bool found = false;
  uint8_t note7 = static_cast<uint8_t>(std::clamp(note, 0u, 127u));
  uint8_t rel_vel = static_cast<uint8_t>(release_velocity16 >> 8);
  if (rel_vel == 0) rel_vel = 64;
  for (auto& v : voices_) {
    if (v.is_active() && v.channel() == channel && v.note() == note7 &&
        (v.state() == VoiceState::kHeld || v.state() == VoiceState::kSustained)) {
      v.stop(rel_vel);
      found = true;
    }
  }
  return found ? EngineStatus::kOk : EngineStatus::kVoiceNotFound;
}

bool EpEngine::key_released(uint32_t channel, uint32_t note) noexcept {
  (void)channel;
  (void)note;
  return false;
}

void EpEngine::set_param(uint32_t param_id, float value) noexcept {
  (void)param_id;
  (void)value;
}

void EpEngine::set_part_param(uint32_t part, uint32_t param_id, float value) noexcept {
  (void)part;
  (void)param_id;
  (void)value;
}

void EpEngine::set_part_param_edit(uint32_t part, uint32_t param_id, float value) noexcept {
  set_part_param(part, param_id, value);
}

void EpEngine::reset_play_state() noexcept {
  for (uint32_t i = 0; i < kPartCount; ++i) {
    part_params_[i] = base_params_[i];
    part_sustain_[i] = false;
    part_pitch_bend_[i] = 0.0f;
  }
  for (auto& v : voices_) {
    v.reset();
  }
}

void EpEngine::render(float* left, float* right, uint32_t frames) noexcept {
  uint32_t processed = 0;
  while (processed < frames) {
    uint32_t block = std::min(frames - processed, kBlockFrames);
    float* l_ptr = left + processed;
    float* r_ptr = right + processed;

    for (auto& v : voices_) {
      if (v.is_active()) {
        v.render_block(l_ptr, r_ptr, block);
      }
    }

    processed += block;
    frame_clock_ += block;
  }
}

void EpEngine::render_split(float* const* left, float* const* right, uint32_t frames) noexcept {
  for (uint32_t p = 0; p < kPartCount; ++p) {
    if (left[p]) std::memset(left[p], 0, frames * sizeof(float));
    if (right[p]) std::memset(right[p], 0, frames * sizeof(float));
  }

  uint32_t processed = 0;
  while (processed < frames) {
    uint32_t block = std::min(frames - processed, kBlockFrames);

    for (auto& v : voices_) {
      if (v.is_active()) {
        uint32_t p = v.channel() % kPartCount;
        if (left[p] && right[p]) {
          v.render_block(left[p] + processed, right[p] + processed, block);
        }
      }
    }

    processed += block;
    frame_clock_ += block;
  }
}

bool EpEngine::voice_info(uint32_t slot, VoiceInfo& out) const noexcept {
  if (slot >= voices_.size() || !voices_[slot].is_active()) {
    return false;
  }
  const auto& v = voices_[slot];
  out.start_time = v.start_time();
  out.level = v.current_level();
  out.state = v.state();
  out.channel = v.channel();
  return true;
}

bool EpEngine::begin_steal_fade(uint32_t slot, uint32_t ramp_frames) noexcept {
  if (slot >= voices_.size() || !voices_[slot].is_active()) {
    return false;
  }
  voices_[slot].begin_steal_fade(ramp_frames);
  return true;
}

void EpEngine::set_part_voice_params(uint32_t part_idx, const EpVoicingParameters& params) noexcept {
  uint32_t p = part_idx % kPartCount;
  base_params_[p] = params;
  part_params_[p] = params;
}

const EpVoicingParameters& EpEngine::part_voice_params(uint32_t part_idx) const noexcept {
  return part_params_[part_idx % kPartCount];
}

void EpEngine::set_sustain(uint32_t channel, bool sustain) noexcept {
  uint32_t p = channel % kPartCount;
  part_sustain_[p] = sustain;
  for (auto& v : voices_) {
    if (v.is_active() && v.channel() == channel) {
      v.set_sustain(sustain);
    }
  }
}

void EpEngine::set_pitch_bend(uint32_t channel, float semitones) noexcept {
  uint32_t p = channel % kPartCount;
  part_pitch_bend_[p] = semitones;
  for (auto& v : voices_) {
    if (v.is_active() && v.channel() == channel) {
      v.set_pitch_bend(semitones);
    }
  }
}

uint32_t EpEngine::active_voice_count() const noexcept {
  uint32_t count = 0;
  for (const auto& v : voices_) {
    if (v.is_active()) ++count;
  }
  return count;
}

uint32_t EpEngine::allocate_voice_slot(uint32_t channel, uint32_t note) noexcept {
  (void)channel;
  (void)note;

  // 1. Find inactive slot
  for (uint32_t i = 0; i < voices_.size(); ++i) {
    if (!voices_[i].is_active()) return i;
  }

  // 2. Find releasing voice with lowest level or oldest
  uint32_t best_releasing = voices_.size();
  float lowest_rel_level = 1e9f;
  for (uint32_t i = 0; i < voices_.size(); ++i) {
    if (voices_[i].state() == VoiceState::kReleasing) {
      if (voices_[i].current_level() < lowest_rel_level) {
        lowest_rel_level = voices_[i].current_level();
        best_releasing = i;
      }
    }
  }
  if (best_releasing < voices_.size()) return best_releasing;

  // 3. Fallback: steal oldest active voice
  uint32_t oldest_slot = 0;
  uint64_t oldest_time = ~0ULL;
  for (uint32_t i = 0; i < voices_.size(); ++i) {
    if (voices_[i].start_time() < oldest_time) {
      oldest_time = voices_[i].start_time();
      oldest_slot = i;
    }
  }
  return oldest_slot;
}

}  // namespace velora::core
