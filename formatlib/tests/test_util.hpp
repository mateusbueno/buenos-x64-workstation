#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "velora/format/bundle.hpp"

inline int g_fail = 0;
#define CHECK(c)                                               \
  do {                                                         \
    if (!(c)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)
#define CHECK_ERR(expr, err)                                                              \
  do {                                                                                    \
    const auto _e = (expr);                                                               \
    if (_e != (err)) {                                                                    \
      std::printf("FAIL %s:%d: %s -> %s (want %s)\n", __FILE__, __LINE__, #expr,          \
                  velora::format::to_string(_e), velora::format::to_string(err));         \
      ++g_fail;                                                                           \
    }                                                                                     \
  } while (0)

namespace vt {
using velora::format::PcmFormat;

struct Rng {
  std::uint64_t s;
  explicit Rng(std::uint64_t seed) : s(seed ? seed : 1) {}
  std::uint64_t next() {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return s;
  }
};

enum class Kind { kSilence, kSine, kNoise, kMixed };

// Integer PCM fixture in the interleaved exchange convention.
inline std::vector<std::int32_t> make_int(const PcmFormat& f, std::uint64_t frames, Kind k,
                                          std::uint64_t seed = 7) {
  Rng rng(seed);
  const std::int64_t hi = (std::int64_t(1) << (f.bits_per_sample - 1)) - 1;
  const std::int64_t lo = -hi - 1;
  std::vector<std::int32_t> v(frames * f.channels);
  for (std::uint64_t i = 0; i < frames; ++i)
    for (unsigned c = 0; c < f.channels; ++c) {
      std::int64_t x = 0;
      switch (k) {
        case Kind::kSilence: break;
        case Kind::kSine:
          x = std::llround(std::sin(double(i) * 0.05 * (c + 1)) * double(hi) * 0.8);
          break;
        case Kind::kNoise: {  // full scale, incl. both extremes
          const std::uint64_t r = rng.next();
          x = (r % 17 == 0) ? ((r & 1) ? hi : lo) : lo + std::int64_t(r % std::uint64_t(hi - lo + 1));
          break;
        }
        case Kind::kMixed:
          x = std::llround(std::sin(double(i) * 0.01) * double(hi) * 0.5) +
              std::int64_t(rng.next() % 64) - 32;
          break;
      }
      v[i * f.channels + c] = static_cast<std::int32_t>(x);
    }
  return v;
}

inline std::vector<std::int32_t> make_float(unsigned channels, std::uint64_t frames,
                                            std::uint64_t seed = 11) {
  Rng rng(seed);
  std::vector<std::int32_t> v(frames * channels);
  static const std::uint32_t special[] = {0x7fc00001u, 0xffc12345u, 0x80000000u, 0x7f800000u,
                                          0xff800000u, 0x00000001u, 0x007fffffu, 0x7fa00000u};
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i % 5 == 0) v[i] = static_cast<std::int32_t>(special[(i / 5) % 8]);
    else if (i % 5 == 1) v[i] = velora::format::float_to_bits(float(std::sin(double(i) * 0.1)));
    else v[i] = static_cast<std::int32_t>(rng.next());  // arbitrary bit patterns
  }
  return v;
}
}  // namespace vt
