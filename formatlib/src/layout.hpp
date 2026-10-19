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
