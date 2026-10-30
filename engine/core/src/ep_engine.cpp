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
    part_pedal_cc_[i] = 0.0f;
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
  if (part_pedal_cc_[part] > 0.0f) {
    voices_[slot].set_half_pedal(part_pedal_cc_[part]);
  } else if (part_sustain_[part]) {
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
  put_part_param(part, param_id, value, false);
}

void EpEngine::set_part_param_edit(uint32_t part, uint32_t param_id, float value) noexcept {
  put_part_param(part, param_id, value, true);
}

void EpEngine::put_part_param(uint32_t part, uint32_t param_id, float value, bool edit) noexcept {
  constexpr uint32_t kEpFirstId = params::kEpModelType;
  constexpr uint32_t kEpLastId = params::kEpToneBarCoupling;
  uint32_t p = part % kPartCount;
  if (param_id < kEpFirstId || param_id > kEpLastId || !std::isfinite(value)) return;
  uint32_t idx = param_id - kEpFirstId;
  if (edit) {
    part_params::engage(engaged_[p], idx);
  } else if (!part_params::should_apply(engaged_[p], idx, param_id, value)) {
    return;
  }

  switch (param_id) {
    case params::kEpModelType:
      part_params_[p].instrument_type = (static_cast<int>(value) == params::kEpModelTypeReed)
                                           ? EpInstrumentType::kReed
                                           : EpInstrumentType::kTine;
      break;
    case params::kEpHammerHardness:
      part_params_[p].hammer_hardness = std::clamp(value, 0.0f, 1.0f);
      break;
    case params::kEpPickupPosition:
      part_params_[p].pickup_position = std::clamp(value, 0.0f, 1.0f);
      break;
    case params::kEpPickupLevel:
      part_params_[p].pickup_level = std::clamp(value, 0.0f, 1.0f);
      break;
    case params::kEpDamperNoise:
      part_params_[p].damper_noise = std::clamp(value, 0.0f, 1.0f);
      break;
    case params::kEpAttackNoise:
      part_params_[p].attack_noise = std::clamp(value, 0.0f, 1.0f);
      break;
    case params::kEpDecayRate:
      part_params_[p].decay_rate = std::clamp(value, 0.0f, 1.0f);
      break;
    case params::kEpToneBarCoupling:
      part_params_[p].tone_bar_coupling = std::clamp(value, 0.0f, 1.0f);
      break;
    default:
      break;
  }
}

void EpEngine::reset_play_state() noexcept {
  for (uint32_t i = 0; i < kPartCount; ++i) {
    part_params_[i] = base_params_[i];
    part_sustain_[i] = false;
    part_pedal_cc_[i] = 0.0f;
    part_pitch_bend_[i] = 0.0f;
    engaged_[i].clear();
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
  uint32_t offset = 0;
  while (frames > 0) {
    uint32_t chunk = std::min(frames, kBlockFrames);

    for (uint32_t c = 0; c < kSplitChannels; ++c) {
      if (!left[c] && !right[c]) continue;

      std::fill_n(scratch_left_.data(), chunk, 0.0f);
      std::fill_n(scratch_right_.data(), chunk, 0.0f);
      bool has_channel_voices = false;

      for (auto& v : voices_) {
        if (v.is_active() && (v.channel() % kPartCount) == c) {
          v.render_block(scratch_left_.data(), scratch_right_.data(), chunk);
          has_channel_voices = true;
        }
      }

      if (has_channel_voices) {
        if (left[c]) {
          for (uint32_t i = 0; i < chunk; ++i) left[c][offset + i] += scratch_left_[i];
        }
        if (right[c]) {
          for (uint32_t i = 0; i < chunk; ++i) right[c][offset + i] += scratch_right_[i];
        }
      }
    }

    offset += chunk;
    frames -= chunk;
    frame_clock_ += chunk;
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
  out.note = v.note();
  return true;
}

bool EpEngine::begin_steal_fade(uint32_t slot, uint32_t ramp_frames) noexcept {
  if (slot >= voices_.size() || !voices_[slot].is_active() || voices_[slot].is_steal_fading()) {
    return false;
  }
  voices_[slot].begin_steal_fade(ramp_frames);
  return true;
}

void EpEngine::set_part_voice_params(uint32_t part_idx, const EpVoicingParameters& params) noexcept {
  uint32_t p = part_idx % kPartCount;
  base_params_[p] = params;
  part_params_[p] = params;
  engaged_[p].clear();
}

const EpVoicingParameters& EpEngine::part_voice_params(uint32_t part_idx) const noexcept {
  return part_params_[part_idx % kPartCount];
}

void EpEngine::set_sustain(uint32_t channel, bool sustain) noexcept {
  uint32_t p = channel % kPartCount;
  part_sustain_[p] = sustain;
  part_pedal_cc_[p] = sustain ? 127.0f : 0.0f;
  for (auto& v : voices_) {
    if (v.is_active() && v.channel() == channel) {
      v.set_sustain(sustain);
    }
  }
}

void EpEngine::set_half_pedal(uint32_t channel, float cc64) noexcept {
  uint32_t p = channel % kPartCount;
  part_pedal_cc_[p] = cc64;
  part_sustain_[p] = (cc64 >= EpVoice::kHalfPedalHoldCc);
  for (auto& v : voices_) {
    if (v.is_active() && v.channel() == channel) {
      v.set_half_pedal(cc64);
    }
  }
}

bool EpEngine::set_part_cc(uint32_t part, uint32_t cc, uint32_t value32) noexcept {
  if (cc == 64) {
    float cc64 = static_cast<float>(value32) / 4294967295.0f * 127.0f;
    set_half_pedal(part, cc64);
    return true;
  }
  return false;
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

}  // namespace velora::core
