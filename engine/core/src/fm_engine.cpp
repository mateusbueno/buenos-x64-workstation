#include "velora/core/fm_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace velora::core {

FmEngine::FmEngine(float sample_rate, uint32_t voice_capacity)
    : sample_rate_(sample_rate), voices_(voice_capacity) {
  for (uint32_t p = 0; p < kPartCount; ++p) {
    part_params_[p] = create_ep_fm_voice();
    base_params_[p] = part_params_[p];
    part_sustain_[p] = false;
    part_pitch_bend_[p] = 0.0f;
  }
}

void FmEngine::set_part_voice_params(uint32_t part_idx, const FmVoiceParameters& params) noexcept {
  if (part_idx < kPartCount) {
    part_params_[part_idx] = params;
    base_params_[part_idx] = params;
    engaged_[part_idx].clear();
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

namespace {
constexpr uint32_t kFmFirstId = params::kFmAlgorithm;
constexpr uint32_t kFmOpFirstId = params::kFmOp1Rate1;
constexpr uint32_t kFmOpStride = params::kFmOp2Rate1 - params::kFmOp1Rate1;  // 14 parameters per operator
constexpr uint32_t kFmLastId = params::kFmOp6AmpModSens;

// The registry's own range of `id`, applied to an integer parameter.
int fm_int(uint32_t id, float v) noexcept {
  const params::ParamDef* d = params::find(id);
  return d ? part_params::to_int(v, static_cast<int>(d->min), static_cast<int>(d->max)) : 0;
}

// Writes registry parameter `id` into the patch. Mapping (registry unit -> FmVoiceParameters field): the DX-style 0..99 / 0..7 steps are the
// fields' own units, so the integer value is stored as is; fm_op*_detune is the -7..7 detune step; the LFO wave enum order equals the field's.
void apply_fm_param(FmVoiceParameters& p, uint32_t id, float v) noexcept {
  if (id >= kFmOpFirstId) {
    FmOperatorParameters& op = p.operators[(id - kFmOpFirstId) / kFmOpStride];
    const uint32_t k = (id - kFmOpFirstId) % kFmOpStride;
    const int n = fm_int(id, v);
    if (k < 4) op.rates[k] = static_cast<uint8_t>(n);
    else if (k < 8) op.levels[k - 4] = static_cast<uint8_t>(n);
    else if (k == 8) op.total_level = static_cast<uint8_t>(n);
    else if (k == 9) op.freq_coarse = static_cast<uint8_t>(n);
    else if (k == 10) op.freq_fine = static_cast<uint8_t>(n);
    else if (k == 11) op.detune = static_cast<int8_t>(n);
    else if (k == 12) op.velocity_sens = static_cast<uint8_t>(n);
    else op.amp_mod_sens = static_cast<uint8_t>(n);
    return;
  }
  const int n = fm_int(id, v);
  switch (id) {
    case params::kFmAlgorithm: p.algorithm = static_cast<uint8_t>(n); break;
    case params::kFmFeedback: p.feedback = static_cast<uint8_t>(n); break;
    case params::kFmOscKeySync: p.osc_key_sync = n != 0; break;
    case params::kFmLfoSpeed: p.lfo_speed = static_cast<uint8_t>(n); break;
    case params::kFmLfoDelay: p.lfo_delay = static_cast<uint8_t>(n); break;
    case params::kFmLfoPitchModDepth: p.lfo_pitch_mod_depth = static_cast<uint8_t>(n); break;
    case params::kFmLfoAmpModDepth: p.lfo_amp_mod_depth = static_cast<uint8_t>(n); break;
    case params::kFmLfoKeySync: p.lfo_key_sync = n != 0; break;
    case params::kFmLfoWave: p.lfo_wave = static_cast<uint8_t>(n); break;
    case params::kFmLfoPitchModSens: p.lfo_pitch_mod_sens = static_cast<uint8_t>(n); break;
    case params::kFmTranspose: p.transpose = static_cast<int8_t>(n); break;
    case params::kFmBrightness: p.brightness_offset = static_cast<uint8_t>(n); break;
    default:
      if (id >= params::kFmPitchEnvRate1 && id <= params::kFmPitchEnvRate4) p.pitch_env_rates[id - params::kFmPitchEnvRate1] = static_cast<uint8_t>(n);
      else if (id >= params::kFmPitchEnvLevel1 && id <= params::kFmPitchEnvLevel4) p.pitch_env_levels[id - params::kFmPitchEnvLevel1] = static_cast<uint8_t>(n);
      break;
  }
}
}  // namespace

void FmEngine::set_part_param(uint32_t part, uint32_t param_id, float value) noexcept { put_part_param(part, param_id, value, false); }
void FmEngine::set_part_param_edit(uint32_t part, uint32_t param_id, float value) noexcept { put_part_param(part, param_id, value, true); }

void FmEngine::put_part_param(uint32_t part, uint32_t param_id, float value, bool edit) noexcept {
  if (part >= kPartCount || param_id < kFmFirstId || param_id > kFmLastId || !std::isfinite(value)) return;
  if (edit) part_params::engage(engaged_[part], param_id - kFmFirstId);
  else if (!part_params::should_apply(engaged_[part], param_id - kFmFirstId, param_id, value)) return;
  apply_fm_param(part_params_[part], param_id, value);
  if (param_id == params::kFmBrightness) {  // the one parameter that also moves sounding notes (smoothed in the voice)
    const uint8_t b = part_params_[part].brightness_offset;
    for (auto& v : voices_)
      if (v.is_active() && v.channel() == part) v.set_brightness_target(b);
  }
}

void FmEngine::reset_play_state() noexcept {
  for (uint32_t p = 0; p < kPartCount; ++p) {
    if (!engaged_[p].any()) continue;
    part_params_[p] = base_params_[p];
    engaged_[p].clear();
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
