#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace velora::format {

// Operator parameter set matching the Story 12.1 clean-room model.
struct FmOperatorParameters {
  std::array<uint8_t, 4> rates{99, 99, 99, 99};  // 0..99
  std::array<uint8_t, 4> levels{99, 99, 99, 0};   // 0..99

  uint8_t kbd_breakpoint{60}; // C3 (note 60, range 0..99)
  uint8_t left_depth{0};      // 0..99
  uint8_t right_depth{0};     // 0..99
  uint8_t left_curve{0};      // 0: -LIN, 1: -EXP, 2: +EXP, 3: +LIN
  uint8_t right_curve{0};     // 0: -LIN, 1: -EXP, 2: +EXP, 3: +LIN

  uint8_t kbd_rate_scaling{0}; // 0..7
  uint8_t amp_mod_sens{0};     // 0..3
  uint8_t velocity_sens{0};    // 0..7
  uint8_t total_level{99};     // 0..99 (carrier volume or modulation index)

  bool fixed_freq_mode{false}; // false: ratio (tracked), true: fixed Hz
  uint8_t freq_coarse{1};      // 0..31
  uint8_t freq_fine{0};        // 0..99
  int8_t detune{0};            // -7..+7

  friend bool operator==(const FmOperatorParameters&, const FmOperatorParameters&) = default;
};

// Complete voice parameter set matching the Story 12.1 clean-room model.
struct FmVoiceParameters {
  // 6 Operators: index 0 = OP1, index 5 = OP6.
  std::array<FmOperatorParameters, 6> operators{};

  std::array<uint8_t, 4> pitch_env_rates{99, 99, 99, 99};  // 0..99
  std::array<uint8_t, 4> pitch_env_levels{50, 50, 50, 50}; // 0..99 (50 = center)

  uint8_t algorithm{1};        // 1..32 (stored in SysEx as 0..31)
  uint8_t feedback{0};         // 0..7
  bool osc_key_sync{true};

  uint8_t lfo_speed{35};          // 0..99
  uint8_t lfo_delay{0};           // 0..99
  uint8_t lfo_pitch_mod_depth{0}; // 0..99
  uint8_t lfo_amp_mod_depth{0};   // 0..99
  bool lfo_key_sync{true};
  uint8_t lfo_wave{0};            // 0: Tri, 1: SawDown, 2: SawUp, 3: Sqr, 4: Sine, 5: S&H
  uint8_t lfo_pitch_mod_sens{0};  // 0..7

  int8_t transpose{0};            // -24..+24 semitones (stored in SysEx as 0..48, 24 = C3)
  char name[11]{"INIT VOICE"};   // 10 characters ASCII + null terminator

  uint8_t brightness_offset{64};  // 0..127 (Velora extension)

  friend bool operator==(const FmVoiceParameters&, const FmVoiceParameters&) = default;
};

// Represents a decoded voice from a SysEx dump with its slot index (1..32).
struct SysexVoiceDump {
  FmVoiceParameters parameters;
  std::string name; // sanitized 10-char voice name
  uint8_t slot{1};  // 1..32
};

enum class SysexParseStatus : uint8_t {
  kOk = 0,
  kDamaged = 1,
};

struct SysexParseResult {
  SysexParseStatus status{SysexParseStatus::kOk};
  std::string error_message; // "This SysEx file is damaged or not a 6-OP voice dump. Nothing was imported."
  std::vector<SysexVoiceDump> voices;
  bool is_bulk{false};
  uint8_t channel{0};

  bool ok() const { return status == SysexParseStatus::kOk; }
  bool damaged() const { return status == SysexParseStatus::kDamaged; }
};

// Standard damaged error message copy required by Story 12.3:
inline constexpr const char* kSysexDamagedMessage =
    "This SysEx file is damaged or not a 6-OP voice dump. Nothing was imported.";

// Story 12.4: exact copy for SysEx arriving over MIDI (USB MIDI / DIN)
inline constexpr const char* kSysexMidiDamagedMessage =
    "SysEx received but damaged. Nothing was imported.";

// SysEx file sizes
inline constexpr size_t kSysexSingleDumpSize = 163;
inline constexpr size_t kSysexBulkDumpSize = 4104;
inline constexpr size_t kSysexPackedVoiceSize = 128;
inline constexpr size_t kSysexUnpackedVoiceSize = 155;

// Sanitizes a 10-character voice name according to Story 12.3:
// - Replace non-printable ASCII (< 32 or > 126) with spaces
// - Trim trailing spaces
// - Empty names become "Voice n" (where n is the 1-based slot number)
std::string sanitize_sysex_name(std::string_view raw, uint8_t slot);

// Unpacks 128 packed bytes into FmVoiceParameters. Validates all parameter ranges.
// Returns true on success, false if any parameter or byte is invalid / out-of-range.
bool unpack_128_voice(std::span<const uint8_t, kSysexPackedVoiceSize> data, FmVoiceParameters& out, uint8_t slot = 1);

// Packs FmVoiceParameters into 128 packed bytes.
void pack_128_voice(const FmVoiceParameters& in, std::span<uint8_t, kSysexPackedVoiceSize> out);

// Unpacks 155 unpacked bytes into FmVoiceParameters. Validates all parameter ranges.
// Returns true on success, false if any parameter or byte is invalid / out-of-range.
bool unpack_155_voice(std::span<const uint8_t, kSysexUnpackedVoiceSize> data, FmVoiceParameters& out, uint8_t slot = 1);

// Packs FmVoiceParameters into 155 unpacked bytes.
void pack_155_voice(const FmVoiceParameters& in, std::span<uint8_t, kSysexUnpackedVoiceSize> out);

// Parses a complete SysEx file (single-voice 163 bytes or 32-voice bulk dump 4104 bytes).
// Validates header, byte count, checksum and all parameter ranges.
// Sanitizes voice names and disambiguates duplicate names in the bank with their slot number.
SysexParseResult parse_sysex_memory(std::span<const uint8_t> data);
SysexParseResult parse_sysex_file(const std::string& path);

// Encodes a single voice into a 163-byte SysEx dump with header, checksum, and 0xF7 footer.
std::vector<uint8_t> encode_sysex_single_dump(const FmVoiceParameters& voice, uint8_t channel = 0);

// Encodes voices (up to 32) into a 4104-byte SysEx bulk dump with header, checksum, and 0xF7 footer.
// If fewer than 32 voices are provided, the remaining slots are filled with default voices.
std::vector<uint8_t> encode_sysex_bulk_dump(std::span<const FmVoiceParameters> voices, uint8_t channel = 0);

} // namespace velora::format
