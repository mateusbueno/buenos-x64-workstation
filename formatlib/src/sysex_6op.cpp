#include "velora/format/sysex_6op.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_set>

namespace velora::format {

namespace {

inline bool check_range(uint8_t val, uint8_t max_val) {
  return val <= max_val;
}

} // namespace

std::string sanitize_sysex_name(std::string_view raw, uint8_t slot) {
  std::string s;
  s.reserve(raw.size());
  for (char c : raw) {
    auto uc = static_cast<unsigned char>(c);
    if (uc < 32 || uc > 126) {
      s.push_back(' ');
    } else {
      s.push_back(c);
    }
  }

  // Trim trailing spaces
  while (!s.empty() && s.back() == ' ') {
    s.pop_back();
  }

  if (s.empty()) {
    s = "Voice " + std::to_string(slot);
  }
  return s;
}

bool unpack_128_voice(std::span<const uint8_t, kSysexPackedVoiceSize> data, FmVoiceParameters& out, uint8_t slot) {
  // Ensure no MIDI status bytes (MSB set) in the 128 data bytes
  for (uint8_t b : data) {
    if (b & 0x80) return false;
  }

  // 6 Operators, ordered OP6 (index 5) down to OP1 (index 0)
  for (int op_idx = 5; op_idx >= 0; --op_idx) {
    const size_t base = (5 - op_idx) * 17;
    auto& op = out.operators[op_idx];

    // Rates 1..4 (0..99)
    for (int i = 0; i < 4; ++i) {
      uint8_t r = data[base + i];
      if (!check_range(r, 99)) return false;
      op.rates[i] = r;
    }

    // Levels 1..4 (0..99)
    for (int i = 0; i < 4; ++i) {
      uint8_t l = data[base + 4 + i];
      if (!check_range(l, 99)) return false;
      op.levels[i] = l;
    }

    // Keyboard Breakpoint (0..99)
    uint8_t bp = data[base + 8];
    if (!check_range(bp, 99)) return false;
    op.kbd_breakpoint = bp;

    // Left Depth, Right Depth (0..99)
    uint8_t ld = data[base + 9];
    uint8_t rd = data[base + 10];
    if (!check_range(ld, 99) || !check_range(rd, 99)) return false;
    op.left_depth = ld;
    op.right_depth = rd;

    // Byte 11: Left Curve (bits 0..1), Right Curve (bits 2..3)
    uint8_t b11 = data[base + 11];
    if (b11 & 0xF0) return false; // unused bits must be 0
    op.left_curve = b11 & 0x03;
    op.right_curve = (b11 >> 2) & 0x03;

    // Byte 12: Rate Scaling (bits 0..2), Detune (bits 3..6)
    uint8_t b12 = data[base + 12];
    if (b12 & 0x80) return false;
    op.kbd_rate_scaling = b12 & 0x07;
    uint8_t raw_detune = (b12 >> 3) & 0x0F;
    if (raw_detune > 14) return false;
    op.detune = static_cast<int8_t>(raw_detune) - 7; // -7..+7

    // Byte 13: Amp Mod Sens (bits 0..1), Key Velocity Sens (bits 2..4)
    uint8_t b13 = data[base + 13];
    if (b13 & 0xE0) return false; // bits 5..7 must be 0
    op.amp_mod_sens = b13 & 0x03;
    op.velocity_sens = (b13 >> 2) & 0x07;

    // Byte 14: Total Level (0..99)
    uint8_t tl = data[base + 14];
    if (!check_range(tl, 99)) return false;
    op.total_level = tl;

    // Byte 15: Fixed Freq Mode (bit 0), Freq Coarse (bits 1..5)
    uint8_t b15 = data[base + 15];
    if (b15 & 0xC0) return false; // bits 6..7 must be 0
    op.fixed_freq_mode = (b15 & 0x01) != 0;
    op.freq_coarse = (b15 >> 1) & 0x1F;

    // Byte 16: Frequency Fine (0..99)
    uint8_t ff = data[base + 16];
    if (!check_range(ff, 99)) return false;
    op.freq_fine = ff;
  }

  // Global parameters (offsets 102..127)
  // Pitch Env Rates 1..4 (0..99)
  for (int i = 0; i < 4; ++i) {
    uint8_t pr = data[102 + i];
    if (!check_range(pr, 99)) return false;
    out.pitch_env_rates[i] = pr;
  }

  // Pitch Env Levels 1..4 (0..99)
  for (int i = 0; i < 4; ++i) {
    uint8_t pl = data[106 + i];
    if (!check_range(pl, 99)) return false;
    out.pitch_env_levels[i] = pl;
  }

  // Byte 110: Algorithm (0..31 -> 1..32)
  uint8_t alg = data[110];
  if (alg > 31) return false;
  out.algorithm = alg + 1;

  // Byte 111: Feedback (bits 0..2), Osc Key Sync (bit 3)
  uint8_t b111 = data[111];
  if (b111 & 0xF0) return false; // bits 4..7 must be 0
  out.feedback = b111 & 0x07;
  out.osc_key_sync = (b111 & 0x08) != 0;

  // Bytes 112..115: LFO Speed, Delay, Pitch Mod Depth, Amp Mod Depth (0..99)
  if (!check_range(data[112], 99) || !check_range(data[113], 99) ||
      !check_range(data[114], 99) || !check_range(data[115], 99)) {
    return false;
  }
  out.lfo_speed = data[112];
  out.lfo_delay = data[113];
  out.lfo_pitch_mod_depth = data[114];
  out.lfo_amp_mod_depth = data[115];

  // Byte 116: LFO Key Sync (bit 0), LFO Wave (bits 1..3), LFO PMS (bits 4..6)
  uint8_t b116 = data[116];
  if (b116 & 0x80) return false;
  out.lfo_key_sync = (b116 & 0x01) != 0;
  uint8_t wave = (b116 >> 1) & 0x07;
  if (wave > 5) return false; // 0..5 valid
  out.lfo_wave = wave;
  out.lfo_pitch_mod_sens = (b116 >> 4) & 0x07;

  // Byte 117: Transpose (0..48, 24 = C3)
  uint8_t tr = data[117];
  if (tr > 48) return false;
  out.transpose = static_cast<int8_t>(tr) - 24;

  // Bytes 118..127: Voice Name (10 characters)
  char raw_name[11]{};
  std::memcpy(raw_name, data.data() + 118, 10);
  raw_name[10] = '\0';
  std::string sanitized = sanitize_sysex_name(std::string_view(raw_name, 10), slot);
  std::memset(out.name, 0, sizeof(out.name));
  std::strncpy(out.name, sanitized.c_str(), 10);

  out.brightness_offset = 64;
  return true;
}

void pack_128_voice(const FmVoiceParameters& in, std::span<uint8_t, kSysexPackedVoiceSize> out) {
  std::fill(out.begin(), out.end(), 0);

  // 6 Operators, ordered OP6 (index 5) down to OP1 (index 0)
  for (int op_idx = 5; op_idx >= 0; --op_idx) {
    const size_t base = (5 - op_idx) * 17;
    const auto& op = in.operators[op_idx];

    for (int i = 0; i < 4; ++i) {
      out[base + i] = std::min<uint8_t>(op.rates[i], 99);
      out[base + 4 + i] = std::min<uint8_t>(op.levels[i], 99);
    }
    out[base + 8] = std::min<uint8_t>(op.kbd_breakpoint, 99);
    out[base + 9] = std::min<uint8_t>(op.left_depth, 99);
    out[base + 10] = std::min<uint8_t>(op.right_depth, 99);

    out[base + 11] = (op.left_curve & 0x03) | ((op.right_curve & 0x03) << 2);

    uint8_t raw_detune = static_cast<uint8_t>(std::clamp<int>(op.detune + 7, 0, 14));
    out[base + 12] = (op.kbd_rate_scaling & 0x07) | ((raw_detune & 0x0F) << 3);

    out[base + 13] = (op.amp_mod_sens & 0x03) | ((op.velocity_sens & 0x07) << 2);
    out[base + 14] = std::min<uint8_t>(op.total_level, 99);

    out[base + 15] = (op.fixed_freq_mode ? 1 : 0) | ((op.freq_coarse & 0x1F) << 1);
    out[base + 16] = std::min<uint8_t>(op.freq_fine, 99);
  }

  for (int i = 0; i < 4; ++i) {
    out[102 + i] = std::min<uint8_t>(in.pitch_env_rates[i], 99);
    out[106 + i] = std::min<uint8_t>(in.pitch_env_levels[i], 99);
  }

  out[110] = in.algorithm > 0 ? (std::min<uint8_t>(in.algorithm, 32) - 1) : 0;
  out[111] = (in.feedback & 0x07) | (in.osc_key_sync ? 0x08 : 0x00);

  out[112] = std::min<uint8_t>(in.lfo_speed, 99);
  out[113] = std::min<uint8_t>(in.lfo_delay, 99);
  out[114] = std::min<uint8_t>(in.lfo_pitch_mod_depth, 99);
  out[115] = std::min<uint8_t>(in.lfo_amp_mod_depth, 99);

  out[116] = (in.lfo_key_sync ? 0x01 : 0x00) |
             ((std::min<uint8_t>(in.lfo_wave, 5) & 0x07) << 1) |
             ((in.lfo_pitch_mod_sens & 0x07) << 4);

  out[117] = static_cast<uint8_t>(std::clamp<int>(in.transpose + 24, 0, 48));

  // Voice name: pad to 10 chars with spaces
  size_t len = std::strlen(in.name);
  for (size_t i = 0; i < 10; ++i) {
    out[118 + i] = i < len ? static_cast<uint8_t>(in.name[i]) : static_cast<uint8_t>(' ');
  }
}

bool unpack_155_voice(std::span<const uint8_t, kSysexUnpackedVoiceSize> data, FmVoiceParameters& out, uint8_t slot) {
  for (uint8_t b : data) {
    if (b & 0x80) return false;
  }

  // 6 Operators, ordered OP6 (index 5) down to OP1 (index 0)
  for (int op_idx = 5; op_idx >= 0; --op_idx) {
    const size_t base = (5 - op_idx) * 21;
    auto& op = out.operators[op_idx];

    for (int i = 0; i < 4; ++i) {
      uint8_t r = data[base + i];
      if (!check_range(r, 99)) return false;
      op.rates[i] = r;
    }
    for (int i = 0; i < 4; ++i) {
      uint8_t l = data[base + 4 + i];
      if (!check_range(l, 99)) return false;
      op.levels[i] = l;
    }

    if (!check_range(data[base + 8], 99) || !check_range(data[base + 9], 99) || !check_range(data[base + 10], 99)) {
      return false;
    }
    op.kbd_breakpoint = data[base + 8];
    op.left_depth = data[base + 9];
    op.right_depth = data[base + 10];

    if (!check_range(data[base + 11], 3) || !check_range(data[base + 12], 3) ||
        !check_range(data[base + 13], 7) || !check_range(data[base + 14], 3) ||
        !check_range(data[base + 15], 7) || !check_range(data[base + 16], 99) ||
        !check_range(data[base + 17], 1) || !check_range(data[base + 18], 31) ||
        !check_range(data[base + 19], 99) || !check_range(data[base + 20], 14)) {
      return false;
    }

    op.left_curve = data[base + 11];
    op.right_curve = data[base + 12];
    op.kbd_rate_scaling = data[base + 13];
    op.amp_mod_sens = data[base + 14];
    op.velocity_sens = data[base + 15];
    op.total_level = data[base + 16];
    op.fixed_freq_mode = data[base + 17] != 0;
    op.freq_coarse = data[base + 18];
    op.freq_fine = data[base + 19];
    op.detune = static_cast<int8_t>(data[base + 20]) - 7;
  }

  // Global parameters (offsets 126..154)
  for (int i = 0; i < 4; ++i) {
    if (!check_range(data[126 + i], 99) || !check_range(data[130 + i], 99)) return false;
    out.pitch_env_rates[i] = data[126 + i];
    out.pitch_env_levels[i] = data[130 + i];
  }

  if (data[134] > 31 || data[135] > 7 || data[136] > 1 ||
      !check_range(data[137], 99) || !check_range(data[138], 99) ||
      !check_range(data[139], 99) || !check_range(data[140], 99) ||
      data[141] > 1 || data[142] > 5 || data[143] > 7 || data[144] > 48) {
    return false;
  }

  out.algorithm = data[134] + 1;
  out.feedback = data[135];
  out.osc_key_sync = data[136] != 0;
  out.lfo_speed = data[137];
  out.lfo_delay = data[138];
  out.lfo_pitch_mod_depth = data[139];
  out.lfo_amp_mod_depth = data[140];
  out.lfo_key_sync = data[141] != 0;
  out.lfo_wave = data[142];
  out.lfo_pitch_mod_sens = data[143];
  out.transpose = static_cast<int8_t>(data[144]) - 24;

  char raw_name[11]{};
  std::memcpy(raw_name, data.data() + 145, 10);
  raw_name[10] = '\0';
  std::string sanitized = sanitize_sysex_name(std::string_view(raw_name, 10), slot);
  std::memset(out.name, 0, sizeof(out.name));
  std::strncpy(out.name, sanitized.c_str(), 10);

  out.brightness_offset = 64;
  return true;
}

void pack_155_voice(const FmVoiceParameters& in, std::span<uint8_t, kSysexUnpackedVoiceSize> out) {
  std::fill(out.begin(), out.end(), 0);

  for (int op_idx = 5; op_idx >= 0; --op_idx) {
    const size_t base = (5 - op_idx) * 21;
    const auto& op = in.operators[op_idx];

    for (int i = 0; i < 4; ++i) {
      out[base + i] = std::min<uint8_t>(op.rates[i], 99);
      out[base + 4 + i] = std::min<uint8_t>(op.levels[i], 99);
    }
    out[base + 8] = std::min<uint8_t>(op.kbd_breakpoint, 99);
    out[base + 9] = std::min<uint8_t>(op.left_depth, 99);
    out[base + 10] = std::min<uint8_t>(op.right_depth, 99);
    out[base + 11] = op.left_curve & 0x03;
    out[base + 12] = op.right_curve & 0x03;
    out[base + 13] = op.kbd_rate_scaling & 0x07;
    out[base + 14] = op.amp_mod_sens & 0x03;
    out[base + 15] = op.velocity_sens & 0x07;
    out[base + 16] = std::min<uint8_t>(op.total_level, 99);
    out[base + 17] = op.fixed_freq_mode ? 1 : 0;
    out[base + 18] = op.freq_coarse & 0x1F;
    out[base + 19] = std::min<uint8_t>(op.freq_fine, 99);
    out[base + 20] = static_cast<uint8_t>(std::clamp<int>(op.detune + 7, 0, 14));
  }

  for (int i = 0; i < 4; ++i) {
    out[126 + i] = std::min<uint8_t>(in.pitch_env_rates[i], 99);
    out[130 + i] = std::min<uint8_t>(in.pitch_env_levels[i], 99);
  }

  out[134] = in.algorithm > 0 ? (std::min<uint8_t>(in.algorithm, 32) - 1) : 0;
  out[135] = in.feedback & 0x07;
  out[136] = in.osc_key_sync ? 1 : 0;
  out[137] = std::min<uint8_t>(in.lfo_speed, 99);
  out[138] = std::min<uint8_t>(in.lfo_delay, 99);
  out[139] = std::min<uint8_t>(in.lfo_pitch_mod_depth, 99);
  out[140] = std::min<uint8_t>(in.lfo_amp_mod_depth, 99);
  out[141] = in.lfo_key_sync ? 1 : 0;
  out[142] = std::min<uint8_t>(in.lfo_wave, 5);
  out[143] = in.lfo_pitch_mod_sens & 0x07;
  out[144] = static_cast<uint8_t>(std::clamp<int>(in.transpose + 24, 0, 48));

  size_t len = std::strlen(in.name);
  for (size_t i = 0; i < 10; ++i) {
    out[145 + i] = i < len ? static_cast<uint8_t>(in.name[i]) : static_cast<uint8_t>(' ');
  }
}

SysexParseResult parse_sysex_memory(std::span<const uint8_t> data) {
  SysexParseResult res;
  auto fail = [&]() -> SysexParseResult {
    res.status = SysexParseStatus::kDamaged;
    res.error_message = kSysexDamagedMessage;
    res.voices.clear();
    return res;
  };

  if (data.size() != kSysexSingleDumpSize && data.size() != kSysexBulkDumpSize) {
    return fail();
  }

  // Common header checks: 0xF0, 0x43 (Yamaha ID), substatus channel (0x00..0x0F)
  if (data[0] != 0xF0 || data[1] != 0x43 || (data[2] & 0xF0) != 0x00) {
    return fail();
  }
  res.channel = data[2] & 0x0F;

  if (data.back() != 0xF7) {
    return fail();
  }

  if (data.size() == kSysexSingleDumpSize) {
    // Single Voice Dump format:
    // Header: F0 43 [substatus] 00 01 1B
    if (data[3] != 0x00 || data[4] != 0x01 || data[5] != 0x1B) {
      return fail();
    }

    // Payload: data[6..160] (155 bytes)
    const uint8_t* payload = data.data() + 6;
    uint8_t sum = 0;
    for (size_t i = 0; i < kSysexUnpackedVoiceSize; ++i) {
      sum = (sum + payload[i]) & 0x7F;
    }
    uint8_t expected_checksum = (-sum) & 0x7F;
    if (data[161] != expected_checksum) {
      return fail();
    }

    FmVoiceParameters voice;
    std::span<const uint8_t, kSysexUnpackedVoiceSize> payload_span(payload, kSysexUnpackedVoiceSize);
    if (!unpack_155_voice(payload_span, voice, 1)) {
      return fail();
    }

    res.status = SysexParseStatus::kOk;
    res.is_bulk = false;
    SysexVoiceDump dump;
    dump.parameters = voice;
    dump.name = voice.name;
    dump.slot = 1;
    res.voices.push_back(std::move(dump));
    return res;
  }

  // 32-Voice Bulk Dump format (4104 bytes):
  // Header: F0 43 [substatus] 09 20 00
  if (data[3] != 0x09 || data[4] != 0x20 || data[5] != 0x00) {
    return fail();
  }

  // Payload: data[6..4101] (4096 bytes: 32 * 128 bytes)
  const uint8_t* payload = data.data() + 6;
  uint8_t sum = 0;
  for (size_t i = 0; i < 4096; ++i) {
    sum = (sum + payload[i]) & 0x7F;
  }
  uint8_t expected_checksum = (-sum) & 0x7F;
  if (data[4102] != expected_checksum) {
    return fail();
  }

  res.is_bulk = true;
  res.voices.reserve(32);

  // Unpack all 32 voices
  std::unordered_set<std::string> seen_names;
  for (uint8_t slot = 1; slot <= 32; ++slot) {
    const size_t offset = (slot - 1) * kSysexPackedVoiceSize;
    std::span<const uint8_t, kSysexPackedVoiceSize> voice_span(payload + offset, kSysexPackedVoiceSize);
    FmVoiceParameters voice;
    if (!unpack_128_voice(voice_span, voice, slot)) {
      return fail();
    }

    std::string name = voice.name;
    // Disambiguate duplicate names in the same bank
    if (seen_names.find(name) != seen_names.end()) {
      name += " (" + std::to_string(slot) + ")";
      std::memset(voice.name, 0, sizeof(voice.name));
      std::strncpy(voice.name, name.c_str(), 10);
    } else {
      seen_names.insert(name);
    }

    SysexVoiceDump dump;
    dump.parameters = voice;
    dump.name = name;
    dump.slot = slot;
    res.voices.push_back(std::move(dump));
  }

  res.status = SysexParseStatus::kOk;
  return res;
}

SysexParseResult parse_sysex_file(const std::string& path) {
  SysexParseResult res;
  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) {
    res.status = SysexParseStatus::kDamaged;
    res.error_message = kSysexDamagedMessage;
    return res;
  }

  std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return parse_sysex_memory(buffer);
}

std::vector<uint8_t> encode_sysex_single_dump(const FmVoiceParameters& voice, uint8_t channel) {
  std::vector<uint8_t> out(kSysexSingleDumpSize, 0);

  // Header: F0 43 [channel] 00 01 1B
  out[0] = 0xF0;
  out[1] = 0x43;
  out[2] = channel & 0x0F;
  out[3] = 0x00;
  out[4] = 0x01;
  out[5] = 0x1B;

  // Payload: 155 bytes
  std::span<uint8_t, kSysexUnpackedVoiceSize> payload_span(out.data() + 6, kSysexUnpackedVoiceSize);
  pack_155_voice(voice, payload_span);

  // Checksum
  uint8_t sum = 0;
  for (size_t i = 0; i < kSysexUnpackedVoiceSize; ++i) {
    sum = (sum + out[6 + i]) & 0x7F;
  }
  out[161] = (-sum) & 0x7F;

  // Footer
  out[162] = 0xF7;
  return out;
}

std::vector<uint8_t> encode_sysex_bulk_dump(std::span<const FmVoiceParameters> voices, uint8_t channel) {
  std::vector<uint8_t> out(kSysexBulkDumpSize, 0);

  // Header: F0 43 [channel] 09 20 00
  out[0] = 0xF0;
  out[1] = 0x43;
  out[2] = channel & 0x0F;
  out[3] = 0x09;
  out[4] = 0x20;
  out[5] = 0x00;

  // 32 voices packed into 4096 bytes
  for (size_t i = 0; i < 32; ++i) {
    const size_t offset = 6 + i * kSysexPackedVoiceSize;
    std::span<uint8_t, kSysexPackedVoiceSize> voice_span(out.data() + offset, kSysexPackedVoiceSize);
    if (i < voices.size()) {
      pack_128_voice(voices[i], voice_span);
    } else {
      FmVoiceParameters init_v{};
      pack_128_voice(init_v, voice_span);
    }
  }

  // Checksum over the 4096 bytes payload
  uint8_t sum = 0;
  for (size_t i = 0; i < 4096; ++i) {
    sum = (sum + out[6 + i]) & 0x7F;
  }
  out[4102] = (-sum) & 0x7F;

  // Footer
  out[4103] = 0xF7;
  return out;
}

} // namespace velora::format
