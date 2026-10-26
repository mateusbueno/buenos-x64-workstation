#include "velora/core/fm_engine.hpp"

#include <algorithm>
#include <cstring>

namespace velora::core {

FmEngine::FmEngine(float sample_rate, uint32_t voice_capacity)
    : sample_rate_(sample_rate), voices_(voice_capacity) {
  for (uint32_t p = 0; p < kPartCount; ++p) {
    part_params_[p] = create_ep_fm_voice();
    part_sustain_[p] = false;
    part_pitch_bend_[p] = 0.0f;
  }
}

void FmEngine::set_part_voice_params(uint32_t part_idx, const FmVoiceParameters& params) noexcept {
  if (part_idx < kPartCount) {
    part_params_[part_idx] = params;
  }
}

const FmVoiceParameters& FmEngine::part_voice_params(uint32_t part_idx) const noexcept {
  static const FmVoiceParameters kDefaultParams = create_init_fm_voice();
  if (part_idx < kPartCount) {
    return part_params_[part_idx];
  }
  return kDefaultParams;
}

void FmEngine::set_sustain(uint32_t channel, bool sustain) noexcept {
  uint32_t p = channel % kPartCount;
  part_sustain_[p] = sustain;
  for (auto& v : voices_) {
    if (v.is_active() && v.channel() == channel) {
      v.set_sustain(sustain);
    }
  }
}

void FmEngine::set_pitch_bend(uint32_t channel, float semitones) noexcept {
  uint32_t p = channel % kPartCount;
  part_pitch_bend_[p] = semitones;
  for (auto& v : voices_) {
    if (v.is_active() && v.channel() == channel) {
      v.set_pitch_bend(semitones);
    }
  }
}

void FmEngine::set_brightness_offset(uint32_t channel, uint8_t brightness) noexcept {
  uint32_t p = channel % kPartCount;
  part_params_[p].brightness_offset = brightness;
}

uint32_t FmEngine::allocate_voice_slot(uint32_t channel, uint32_t note) noexcept {
  (void)channel;
  (void)note;
  // 1. Search for an inactive slot
  for (uint32_t i = 0; i < voices_.size(); ++i) {
    if (!voices_[i].is_active()) {
      return i;
    }
  }

  // 2. Pool exhausted: steal oldest releasing voice
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

EngineStatus FmEngine::start_voice(uint32_t channel, uint32_t note, uint32_t velocity16) noexcept {
  uint32_t slot = acquire_slot(channel, note);
  if (slot == VoiceAllocator::kNoSlot || slot >= voices_.size()) {
    return EngineStatus::kPoolExhausted;
  }
  uint32_t part_idx = channel % kPartCount;
  uint8_t note7 = static_cast<uint8_t>(std::clamp(note, 0u, 127u));

  voices_[slot].start(channel, note7, velocity16, part_params_[part_idx], sample_rate_);
  voices_[slot].set_start_time(frame_clock_);
  voices_[slot].set_sustain(part_sustain_[part_idx]);
  voices_[slot].set_pitch_bend(part_pitch_bend_[part_idx]);

  return EngineStatus::kOk;
}

EngineStatus FmEngine::stop_voice(uint32_t channel, uint32_t note) noexcept {
  bool found = false;
  uint8_t note7 = static_cast<uint8_t>(std::clamp(note, 0u, 127u));

  for (auto& v : voices_) {
    if (v.is_active() && v.channel() == channel && v.note() == note7 && v.is_held()) {
      v.stop();
      found = true;
    }
  }
  return found ? EngineStatus::kOk : EngineStatus::kVoiceNotFound;
}

void FmEngine::key_released(uint32_t channel, uint32_t note) noexcept {
  uint8_t note7 = static_cast<uint8_t>(std::clamp(note, 0u, 127u));
  for (auto& v : voices_) {
    if (v.is_active() && v.channel() == channel && v.note() == note7 && v.is_held()) {
      v.key_released();
    }
  }
}

void FmEngine::set_param(uint32_t param_id, float value) noexcept {
  // Dispatches generic part parameters
  // e.g. param_id = 1000 + part_idx * 100 + field
  // or CC / Knob 2 Brightness
  if (param_id >= 1000) {
    uint32_t part_idx = (param_id / 100) % kPartCount;
    uint32_t sub_param = param_id % 100;
    if (sub_param == 1) { // Brightness
      set_brightness_offset(part_idx, static_cast<uint8_t>(std::clamp(value, 0.0f, 127.0f)));
    }
  }
}

void FmEngine::render(float* left, float* right, uint32_t frames) noexcept {
  while (frames > 0) {
    uint32_t chunk = std::min(frames, kBlockFrames);
    std::fill_n(scratch_buffer_.data(), chunk, 0.0f);

    for (auto& v : voices_) {
      if (v.is_active()) {
        v.render(scratch_buffer_.data(), chunk);
      }
    }

    for (uint32_t i = 0; i < chunk; ++i) {
      float sample = scratch_buffer_[i];
      left[i] += sample;
      right[i] += sample;
    }

    left += chunk;
    right += chunk;
    frames -= chunk;
    frame_clock_ += chunk;
  }
}

void FmEngine::render_split(float* const* left, float* const* right, uint32_t frames) noexcept {
  while (frames > 0) {
    uint32_t chunk = std::min(frames, kBlockFrames);

    for (uint32_t c = 0; c < kSplitChannels; ++c) {
      std::fill_n(scratch_buffer_.data(), chunk, 0.0f);
      bool has_channel_voices = false;

      for (auto& v : voices_) {
        if (v.is_active() && v.channel() == c) {
          v.render(scratch_buffer_.data(), chunk);
          has_channel_voices = true;
        }
      }

      if (has_channel_voices && left[c] && right[c]) {
        for (uint32_t i = 0; i < chunk; ++i) {
          float sample = scratch_buffer_[i];
          left[c][i] += sample;
          right[c][i] += sample;
        }
      }
    }

    // Advance pointer pointers if necessary or handle single block
    frames -= chunk;
    frame_clock_ += chunk;
  }
}

bool FmEngine::voice_info(uint32_t slot, VoiceInfo& out) const noexcept {
  if (slot >= voices_.size() || !voices_[slot].is_active()) {
    return false;
  }
  const auto& v = voices_[slot];
  out.start_time = v.start_time();
  out.level = v.current_level();
  out.channel = v.channel();
  out.note = v.note();
  if (v.is_steal_fading() || (!v.is_held() && !v.is_sustained())) {
    out.state = VoiceState::kReleasing;
  } else if (v.is_sustained()) {
    out.state = VoiceState::kSustained;
  } else {
    out.state = VoiceState::kHeld;
  }
  return true;
}

bool FmEngine::begin_steal_fade(uint32_t slot, uint32_t ramp_frames) noexcept {
  if (slot >= voices_.size() || !voices_[slot].is_active() || voices_[slot].is_steal_fading()) {
    return false;
  }
  voices_[slot].begin_steal_fade(ramp_frames);
  return true;
}

uint32_t FmEngine::active_voice_count() const noexcept {
  uint32_t count = 0;
  for (const auto& v : voices_) {
    if (v.is_active()) count++;
  }
  return count;
}

} // namespace velora::core
