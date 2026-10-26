#include "velora/core/va_engine.hpp"

#include <algorithm>
#include <cstring>

namespace velora::core {

VaEngine::VaEngine(float sample_rate, uint32_t voice_capacity)
    : sample_rate_(sample_rate), voices_(voice_capacity) {
  for (uint32_t p = 0; p < kPartCount; ++p) {
    part_params_[p] = create_init_va_voice();
    part_sustain_[p] = false;
    part_pitch_bend_[p] = 0.0f;
    part_mono_slots_[p] = -1;
  }
}

void VaEngine::set_part_voice_params(uint32_t part_idx, const VaVoiceParameters& params) noexcept {
  if (part_idx < kPartCount) {
    part_params_[part_idx] = params;
  }
}

const VaVoiceParameters& VaEngine::part_voice_params(uint32_t part_idx) const noexcept {
  static const VaVoiceParameters kDefaultParams = create_init_va_voice();
  if (part_idx < kPartCount) {
    return part_params_[part_idx];
  }
  return kDefaultParams;
}

void VaEngine::set_sustain(uint32_t channel, bool sustain) noexcept {
  uint32_t p = channel % kPartCount;
  part_sustain_[p] = sustain;
  for (auto& v : voices_) {
    if (v.is_active() && v.channel() == channel) {
      v.set_sustain(sustain);
    }
  }
}

void VaEngine::set_pitch_bend(uint32_t channel, float semitones) noexcept {
  uint32_t p = channel % kPartCount;
  part_pitch_bend_[p] = semitones;
  for (auto& v : voices_) {
    if (v.is_active() && v.channel() == channel) {
      v.set_pitch_bend(semitones);
    }
  }
}

void VaEngine::set_brightness_offset(uint32_t channel, uint8_t brightness) noexcept {
  uint32_t p = channel % kPartCount;
  part_params_[p].brightness_offset = brightness;
  for (auto& v : voices_) {
    if (v.is_active() && v.channel() == channel) {
      v.set_brightness_offset(brightness);
    }
  }
}

uint32_t VaEngine::allocate_voice_slot(uint32_t channel, uint32_t note) noexcept {
  (void)channel;
  (void)note;
  // 1. Inactive slot
  for (uint32_t i = 0; i < voices_.size(); ++i) {
    if (!voices_[i].is_active()) {
      return i;
    }
  }

  // 2. Pool exhausted: steal oldest releasing voice, or oldest held voice
  uint32_t victim = 0;
  uint64_t oldest_time = UINT64_MAX;
  bool found_releasing = false;

  for (uint32_t i = 0; i < voices_.size(); ++i) {
    if (!voices_[i].is_held() && !voices_[i].is_sustained()) {
      if (!found_releasing || voices_[i].start_time() < oldest_time) {
        found_releasing = true;
        oldest_time = voices_[i].start_time();
        victim = i;
      }
    } else if (!found_releasing && voices_[i].start_time() < oldest_time) {
      oldest_time = voices_[i].start_time();
      victim = i;
    }
  }

  return victim;
}

EngineStatus VaEngine::start_voice(uint32_t channel, uint32_t note, uint32_t velocity16) noexcept {
  uint32_t part_idx = channel % kPartCount;
  uint8_t note7 = static_cast<uint8_t>(std::clamp(note, 0u, 127u));
  const auto& params = part_params_[part_idx];

  // Check Mono / Legato mode
  if (params.voice_mode == VaVoiceMode::kMono || params.voice_mode == VaVoiceMode::kLegato) {
    int32_t current_slot = part_mono_slots_[part_idx];
    if (current_slot >= 0 && static_cast<uint32_t>(current_slot) < voices_.size()) {
      auto& mono_voice = voices_[current_slot];
      if (mono_voice.is_active()) {
        if (params.voice_mode == VaVoiceMode::kLegato && mono_voice.is_held()) {
          // Legato: retune without re-triggering envelopes
          mono_voice.legato_retune(note7, velocity16);
          return EngineStatus::kOk;
        } else {
          // Mono: retrigger
          mono_voice.start(channel, note7, velocity16, params, sample_rate_);
          mono_voice.set_start_time(frame_clock_);
          mono_voice.set_sustain(part_sustain_[part_idx]);
          mono_voice.set_pitch_bend(part_pitch_bend_[part_idx]);
          return EngineStatus::kOk;
        }
      }
    }

    // Allocate new mono voice
    uint32_t slot = acquire_slot(channel, note);
    if (slot == VoiceAllocator::kNoSlot || slot >= voices_.size()) {
      return EngineStatus::kPoolExhausted;
    }
    part_mono_slots_[part_idx] = static_cast<int32_t>(slot);
    voices_[slot].start(channel, note7, velocity16, params, sample_rate_);
    voices_[slot].set_start_time(frame_clock_);
    voices_[slot].set_sustain(part_sustain_[part_idx]);
    voices_[slot].set_pitch_bend(part_pitch_bend_[part_idx]);
    return EngineStatus::kOk;
  }

  // Polyphonic mode
  uint32_t slot = acquire_slot(channel, note);
  if (slot == VoiceAllocator::kNoSlot || slot >= voices_.size()) {
    return EngineStatus::kPoolExhausted;
  }
  voices_[slot].start(channel, note7, velocity16, params, sample_rate_);
  voices_[slot].set_start_time(frame_clock_);
  voices_[slot].set_sustain(part_sustain_[part_idx]);
  voices_[slot].set_pitch_bend(part_pitch_bend_[part_idx]);
  return EngineStatus::kOk;
}

void VaEngine::key_released(uint32_t channel, uint32_t note) noexcept {
  uint8_t note7 = static_cast<uint8_t>(std::clamp(note, 0u, 127u));
  for (auto& v : voices_) {
    if (v.is_active() && v.channel() == channel && v.note() == note7 && v.is_held()) {
      v.key_released();
    }
  }
}

EngineStatus VaEngine::stop_voice(uint32_t channel, uint32_t note) noexcept {
  bool found = false;
  uint8_t note7 = static_cast<uint8_t>(std::clamp(note, 0u, 127u));
  uint32_t part_idx = channel % kPartCount;

  for (uint32_t i = 0; i < voices_.size(); ++i) {
    auto& v = voices_[i];
    if (v.is_active() && v.channel() == channel && v.note() == note7 && v.is_held()) {
      v.stop();
      found = true;
      if (part_mono_slots_[part_idx] == static_cast<int32_t>(i)) {
        part_mono_slots_[part_idx] = -1;
      }
    }
  }
  return found ? EngineStatus::kOk : EngineStatus::kVoiceNotFound;
}

void VaEngine::set_param(uint32_t param_id, float value) noexcept {
  if (param_id >= 1000) {
    uint32_t part_idx = (param_id / 100) % kPartCount;
    uint32_t sub_param = param_id % 100;
    if (sub_param == 1) { // Knob 2 Brightness
      set_brightness_offset(part_idx, static_cast<uint8_t>(std::clamp(value, 0.0f, 127.0f)));
    }
  }
}

void VaEngine::render(float* left, float* right, uint32_t frames) noexcept {
  while (frames > 0) {
    uint32_t chunk = std::min(frames, kBlockFrames);
    std::fill_n(scratch_left_.data(), chunk, 0.0f);
    std::fill_n(scratch_right_.data(), chunk, 0.0f);

    for (auto& v : voices_) {
      if (v.is_active()) {
        v.render(scratch_left_.data(), scratch_right_.data(), chunk);
      }
    }

    for (uint32_t i = 0; i < chunk; ++i) {
      left[i] += scratch_left_[i];
      right[i] += scratch_right_[i];
    }

    left += chunk;
    right += chunk;
    frames -= chunk;
    frame_clock_ += chunk;
  }
}

void VaEngine::render_split(float* const* left, float* const* right, uint32_t frames) noexcept {
  while (frames > 0) {
    uint32_t chunk = std::min(frames, kBlockFrames);

    for (uint32_t c = 0; c < kSplitChannels; ++c) {
      std::fill_n(scratch_left_.data(), chunk, 0.0f);
      std::fill_n(scratch_right_.data(), chunk, 0.0f);
      bool has_channel_voices = false;

      for (auto& v : voices_) {
        if (v.is_active() && v.channel() == c) {
          v.render(scratch_left_.data(), scratch_right_.data(), chunk);
          has_channel_voices = true;
        }
      }

      if (has_channel_voices && left[c] && right[c]) {
        for (uint32_t i = 0; i < chunk; ++i) {
          left[c][i] += scratch_left_[i];
          right[c][i] += scratch_right_[i];
        }
      }
    }

    frames -= chunk;
    frame_clock_ += chunk;
  }
}

bool VaEngine::voice_info(uint32_t slot, VoiceInfo& out) const noexcept {
  if (slot >= voices_.size() || !voices_[slot].is_active()) {
    return false;
  }
  const auto& v = voices_[slot];
  out.start_time = v.start_time();
  out.level = v.last_level();
  out.channel = v.channel();
  out.note = v.note();
  out.state = v.voice_state();
  return true;
}

bool VaEngine::begin_steal_fade(uint32_t slot, uint32_t ramp_frames) noexcept {
  if (slot >= voices_.size() || !voices_[slot].is_active() || voices_[slot].is_steal_fading()) {
    return false;
  }
  voices_[slot].begin_steal_fade(ramp_frames);
  return true;
}

uint32_t VaEngine::active_voice_count() const noexcept {
  uint32_t count = 0;
  for (const auto& v : voices_) {
    if (v.is_active()) count++;
  }
  return count;
}

} // namespace velora::core
