#include "sfizz_random_replay.hpp"

#include <cmath>
#include <limits>

namespace velora::offline {
namespace {

constexpr std::uint64_t kGolden = 0x9E3779B97F4A7C15ull;
constexpr std::uint64_t kMixA = 0xBF58476D1CE4E5B9ull;
constexpr std::uint64_t kMixB = 0x94D049BB133111EBull;
constexpr std::uint64_t kStarMul = 0x2545F4914F6CDD1Dull;

constexpr std::uint64_t mul_inverse(std::uint64_t m) noexcept {  // m odd: Newton iteration, 6 steps double the correct bits from 3 to 64+
  std::uint64_t x = m;
  for (int i = 0; i < 6; ++i) x *= 2 - m * x;
  return x;
}
constexpr std::uint64_t inv_xorshift_right(std::uint64_t y, int s) noexcept {  // inverse of x ^= x >> s
  std::uint64_t x = y;
  for (int i = 0; i < 64 / s + 1; ++i) x = y ^ (x >> s);
  return x;
}
constexpr std::uint64_t inv_xorshift_left(std::uint64_t y, int s) noexcept {  // inverse of x ^= x << s
  std::uint64_t x = y;
  for (int i = 0; i < 64 / s + 1; ++i) x = y ^ (x << s);
  return x;
}

}  // namespace

float sfizz_uniform_float(std::uint32_t u32) noexcept {
  float u = static_cast<float>(u32) / 4294967296.0f;
  if (u >= 1.0f) u = std::nextafter(1.0f, 0.0f);
  return u;
}

std::vector<float> sfizz_note_draws(const std::vector<NoteEvent>& events, std::uint32_t skip_draws) {
  SfizzLcg g;
  for (std::uint32_t i = 0; i < skip_draws; ++i) g.next();
  std::vector<float> out;
  for (const NoteEvent& e : events) {
    if (e.contact || e.bend) continue;
    if (e.cc) {
      if (e.note == 120 || e.note == 121 || e.note == 123) continue;  // Synth::Impl::performHdcc returns before ccDispatch draws
      g.next();
      continue;
    }
    out.push_back(sfizz_uniform_float(g.next()));
  }
  return out;
}

std::uint64_t engine_seed_for_first_draw(float u) noexcept {
  if (!(u >= 0.0f)) u = 0.0f;  // NaN, negative: clamp (a float to integer conversion of those is undefined)
  if (u > 1.0f) u = 1.0f;      // +inf and anything above 1 (the cast below is then bounded)
  std::uint64_t k = static_cast<std::uint64_t>(std::floor(static_cast<double>(u) * 16777216.0));
  // A first draw of 0 would need the xorshift state 0, which set_seed never produces (it maps a zero state to a fixed non-zero one), so no seed gives
  // engine_first_draw == 0: a target below 2^-24 is answered with 1/2^24 (documented in the header). Hence x below is never 0.
  if (k == 0) k = 1;
  if (k > 0xFFFFFFull) k = 0xFFFFFFull;
  // next_random: x ^= x >> 12; x ^= x << 25; x ^= x >> 27; draw = ((x * kStarMul) >> 40) / 2^24.
  const std::uint64_t x_after = (k << 40) * mul_inverse(kStarMul);
  std::uint64_t x = inv_xorshift_right(x_after, 27);
  x = inv_xorshift_left(x, 25);
  x = inv_xorshift_right(x, 12);
  // set_seed: z = seed + G; z = (z ^ z >> 30) * A; z = (z ^ z >> 27) * B; z ^= z >> 31; rng = z (never 0 here: x == 0 only for k == 0).
  std::uint64_t z = x;
  z = inv_xorshift_right(z, 31);
  z *= mul_inverse(kMixB);
  z = inv_xorshift_right(z, 27);
  z *= mul_inverse(kMixA);
  z = inv_xorshift_right(z, 30);
  return z - kGolden;
}

float engine_first_draw(std::uint64_t seed) noexcept {
  std::uint64_t z = seed + kGolden;
  z = (z ^ (z >> 30)) * kMixA;
  z = (z ^ (z >> 27)) * kMixB;
  z ^= z >> 31;
  std::uint64_t x = z ? z : 0x2545F4914F6CDD1Dull;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  const std::uint64_t r = x * kStarMul;
  return static_cast<float>(r >> 40) * (1.0f / 16777216.0f);
}

}  // namespace velora::offline
