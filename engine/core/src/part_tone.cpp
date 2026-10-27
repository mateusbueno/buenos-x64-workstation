#include "velora/core/part_tone.hpp"

#include <algorithm>
#include <cmath>
#include <complex>

namespace velora::core {

namespace tonemap {
float brightness_db(float percent) noexcept {
  if (std::isnan(percent)) return 0.0f;
  const float p = std::clamp(percent, 0.0f, 100.0f);
  return p == 50.0f ? 0.0f : (p - 50.0f) / 50.0f * kBrightnessMaxDb;
}
}  // namespace tonemap

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = static_cast<double>(kEqSampleRate);
constexpr float kSmoothPerBlock = 0.125f;  // same one-pole step per block as the EQ
constexpr float kSnapDb = 5e-4f;
constexpr double kFlush = 1e-30;
constexpr float kNeutralDb = 1e-3f;  // |gain| below this counts as 0 dB
}  // namespace

PartTone::PartTone() noexcept { eq_.prepare(kEqSampleRate, kFxBlockFrames); }

void PartTone::reset() noexcept VELORA_NONBLOCKING {
  eq_.reset();
  z1_[0] = z1_[1] = z2_[0] = z2_[1] = 0.0;
  bright_active_ = false;
}

void PartTone::set(float brightness_percent, float low_db, float mid_db, float mid_hz, float high_db, bool instant) noexcept VELORA_NONBLOCKING {
  // Dead band: a value that came back through the 32-bit parameter scale (0 dB arrives as ~1e-9 dB, 50 % as 50.000000012) is the neutral setting, so
  // "put the knob back to its default" returns to the exactly transparent (and free) stage.
  auto db0 = [](float db) { return std::fabs(db) < kNeutralDb ? 0.0f : db; };
  eq_.set_physical(db0(low_db), db0(mid_db), mid_hz, db0(high_db), instant);
  bright_tgt_ = db0(tonemap::brightness_db(brightness_percent));
  if (instant) {
    bright_cur_ = bright_tgt_;
    bright_moving_ = false;
    design();
  } else {
    bright_moving_ = bright_cur_ != bright_tgt_;
  }
}

// RBJ high shelf (S = 1), the same formula as the EQ's high band, at kBrightnessShelfHz.
void PartTone::design() noexcept VELORA_NONBLOCKING {
  coef_ = Coef{};
  if (bright_cur_ == 0.0f) return;
  const double A = std::pow(10.0, static_cast<double>(bright_cur_) / 40.0), w0 = 2.0 * kPi * tonemap::kBrightnessShelfHz / kFs, c = std::cos(w0);
  const double al = std::sin(w0) / 2.0 * std::sqrt(2.0);
  const double r = 2.0 * std::sqrt(A) * al;
  const double a0 = (A + 1) - (A - 1) * c + r;
  coef_ = {A * ((A + 1) + (A - 1) * c + r) / a0, -2 * A * ((A - 1) + (A + 1) * c) / a0, A * ((A + 1) + (A - 1) * c - r) / a0,
           2 * ((A - 1) - (A + 1) * c) / a0, ((A + 1) - (A - 1) * c - r) / a0};
}

void PartTone::process(StereoBlock& block) noexcept VELORA_NONBLOCKING {
  if (bright_moving_) {
    bright_cur_ += (bright_tgt_ - bright_cur_) * kSmoothPerBlock;
    if (std::fabs(bright_tgt_ - bright_cur_) < kSnapDb) {
      bright_cur_ = bright_tgt_;
      bright_moving_ = false;
    }
    design();
  }
  eq_.process(block);  // an identity EQ leaves the block untouched
  float* ch[2] = {block.left, block.right};
  if (bright_cur_ == 0.0f) {  // identity shelf: skipped, state cleared so it starts clean when it leaves 0 dB
    if (bright_active_) {
      z1_[0] = z1_[1] = z2_[0] = z2_[1] = 0.0;
      bright_active_ = false;
    }
    if (!eq_.is_identity()) flush_tiny(ch, block.frames);
    return;
  }
  bright_active_ = true;
  for (std::size_t c = 0; c < 2; ++c) {
    float* x = ch[c];
    double z1 = z1_[c], z2 = z2_[c];
    for (std::uint32_t i = 0; i < block.frames; ++i) {
      const double in = std::isfinite(x[i]) ? static_cast<double>(x[i]) : 0.0;
      const double y = coef_.b0 * in + z1;
      z1 = coef_.b1 * in - coef_.a1 * y + z2;
      z2 = coef_.b2 * in - coef_.a2 * y;
      x[i] = static_cast<float>(y);
    }
    z1_[c] = std::fabs(z1) < kFlush ? 0.0 : z1;
    z2_[c] = std::fabs(z2) < kFlush ? 0.0 : z2;
  }
  flush_tiny(ch, block.frames);
}

// Output samples below -600 dB become 0: a decaying tail never reaches the float subnormal range (a fast pole can cross 1e-38 inside one block).
void PartTone::flush_tiny(float* const* ch, std::uint32_t frames) noexcept VELORA_NONBLOCKING {
  for (std::size_t c = 0; c < 2; ++c)
    for (std::uint32_t i = 0; i < frames; ++i)
      if (std::fabs(ch[c][i]) < 1e-30f) ch[c][i] = 0.0f;
}

double PartTone::magnitude_db(double hz) const noexcept {
  double db = eq_.magnitude_db(hz);
  if (bright_cur_ != 0.0f) {
    const std::complex<double> z1 = std::polar(1.0, -2.0 * kPi * hz / kFs), z2 = z1 * z1;
    const std::complex<double> h = (coef_.b0 + coef_.b1 * z1 + coef_.b2 * z2) / (1.0 + coef_.a1 * z1 + coef_.a2 * z2);
    db += 20.0 * std::log10(std::abs(h));
  }
  return db;
}

}  // namespace velora::core
