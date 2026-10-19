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
