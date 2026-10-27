#pragma once
// Internal: on-disk layout constants and little-endian helpers.
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "velora/format/bundle.hpp"

namespace velora::format::layout {

inline constexpr char kMagic[4] = {'V', 'L', 'R', 'B'};
inline constexpr std::size_t kHeaderSize = 128;
inline constexpr std::size_t kIndexEntrySize = 48;
// Header: magic[4] version u32 | manifest_off, manifest_size, index_off, index_size, data_off,
// data_size, total_size (u64 each, at 8..64) | manifest_sha256[32] @64 | index_sha256[32] @96.
inline constexpr std::size_t kMaxStringLen = 4096;
// Format v2 region record: u32 sample | 8 x u8 (key_lo, key_hi, key_center, vel_lo, vel_hi,
// loop_mode, trigger, has_loop_points) | i32 transpose | f32 tune, volume, pan | u64 offset |
// u32 seq_length, seq_position | f32 lorand, hirand | u64 loop_start, loop_end | 7 x f32 ampeg.
inline constexpr std::size_t kRegionRecordSize = 4 + 8 + 4 + 12 + 8 + 8 + 8 + 16 + 28;

// Format 2.1 (Story 5.1) extension record, one per region that uses keyswitches, CC crossfades or
// a filter; the table follows each instrument's region table (u32 count, records ascending by
// region index). The header/manifest version word is major | (minor << 16): minor 0 is exactly the
// Epic 1 format 2 byte stream, minor 1 appends the extension tables.
//   u32 region_index
//   u8  sw_flags (bit i set = opcode i given: sw_lokey, sw_hikey, sw_last, sw_down, sw_up,
//                 sw_previous, sw_default; bit 7 and unset bits' values must be zero)
//   7 x u8 notes in that order (0 when the flag is clear)
//   u8  xfin_count, xfout_count (<= 4), xf_curve (0 gain, 1 power), filter_type (0..5)
//   4 x (u8 cc, u8 lo, u8 hi, u8 0) xfin, then 4 x (...) xfout; unused entries are zero
//   f32 cutoff_hz, f32 resonance_db
inline constexpr std::size_t kExtRecordSize = 4 + 1 + 7 + 4 + 16 + 16 + 8;
// Format 2.2 (Story 13.3) trigger/end record, one per region with a trigger other than attack/release or with `end`; the table follows the
// minor 1 table of each instrument (u32 count, records ascending by region index). The region record's own trigger byte is the Epic 1 view:
// 1 for trigger=release, 0 for everything else (including first, legato, release_key).
//   u32 region_index
//   u8  trigger (0..4: attack, release, first, legato, release_key)
//   u8  flags (bit 0 = has_end; other bits zero)
//   u16 0
//   u64 end (0 when bit 0 is clear)
inline constexpr std::size_t kTrigRecordSize = 4 + 1 + 1 + 2 + 8;
// Format 2.3 (Story 13.4) amp record, one per region whose AmpMap differs from the default; the table follows the minor 2 table of each
// instrument (u32 count, records ascending by region index). Variable length: a fixed part, then `curve_count` points.
//   u32 region_index
//   f32 amp_veltrack (percent), f32 amplitude (percent), f32 amp_keytrack (dB per key)
//   u8  amp_keycenter, u8 curve_count (0..kMaxVelPoints), u16 0
//   f32 vel2attack, f32 vel2decay, f32 vel2sustain, f32 vel2release
//   curve_count x { u8 vel (strictly ascending), u8 0, u16 0, f32 gain }
inline constexpr std::size_t kAmpRecordFixedSize = 4 + 4 + 4 + 4 + 1 + 1 + 2 + 4 + 4 + 4 + 4;
inline constexpr std::size_t kAmpPointSize = 1 + 1 + 2 + 4;
// Format 2.4 (Story 13.5) cc table, per instrument, after the minor 3 table:
//   u32 n_cc_regions, then per region (ascending by region index; a record exists only for a region with a gate or a modulation entry):
//     u32 region_index, u8 gate_count (<= 8), u8 mod_count (<= 16), u16 0,
//     gate_count x { u8 cc, u8 0, u16 0, f32 lo, f32 hi }   (ascending cc)
//     mod_count  x { u8 target (0..11), u8 cc, u8 curve, u8 0, f32 depth }   (ascending target, cc)
//   u32 n_defaults, n_defaults x { u8 cc, u8 0, u16 0, f32 value }   (ascending cc)
//   u32 n_curves, per curve (ascending index): u8 index, u8 0, u16 n_points (<= 128), n_points x { u8 x, u8 0, u16 0, f32 y }   (ascending x)
inline constexpr std::size_t kCcRecordFixedSize = 4 + 1 + 1 + 2;
inline constexpr std::size_t kCcGateSize = 4 + 4 + 4;
inline constexpr std::size_t kCcModSize = 4 + 4;
inline constexpr std::size_t kCcDefaultSize = 4 + 4;
inline constexpr std::size_t kCcCurveFixedSize = 4;
inline constexpr std::size_t kCcPointSize = 4 + 4;
// Format 2.5 (Story 13.6) voice-group table, per instrument, after the minor 4 tables:
//   u32 n_vg_regions, then per region (ascending by region index; a record exists only for a region with a non-default VoiceGroup):
//     u32 region_index, i32 group, i32 off_by (0 without off_by), u8 flags (bit 0 has_off_by, bit 1 note_selfmask off, others 0), u8 off_mode (0 fast, 1 normal, 2 time),
//     u16 0, f32 off_time (0 unless off_mode is time), u16 polyphony, u16 group_polyphony, u16 note_polyphony (each 0..255, 0 = none), u16 0
inline constexpr std::size_t kVgRecordSize = 4 + 4 + 4 + 1 + 1 + 2 + 4 + 2 + 2 + 2 + 2;
inline constexpr unsigned kMinorShift = 16;

inline void put_u8(std::vector<std::uint8_t>& v, std::uint8_t x) { v.push_back(x); }
inline void put_u32(std::vector<std::uint8_t>& v, std::uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}
inline void put_u64(std::vector<std::uint8_t>& v, std::uint64_t x) {
  for (int i = 0; i < 8; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}
inline std::uint32_t get_u32(const std::uint8_t* p) {
  return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
         (std::uint32_t(p[3]) << 24);
}
inline std::uint64_t get_u64(const std::uint8_t* p) {
  return std::uint64_t(get_u32(p)) | (std::uint64_t(get_u32(p + 4)) << 32);
}

// Canonical PCM bytes for the per-sample hash: little-endian, bits/8 bytes per value.
void pcm_hash_update(Sha256& h, const PcmFormat& f, const std::int32_t* v, std::size_t count);

inline std::uint32_t block_count_for(std::uint64_t frames) {
  return static_cast<std::uint32_t>((frames + kBlockFrames - 1) / kBlockFrames);
}

}  // namespace velora::format::layout
