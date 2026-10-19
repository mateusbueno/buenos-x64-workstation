#include "velora/core/rotary_speaker.hpp"

#include <algorithm>
#include <cmath>

namespace velora::core {

static_assert(4.0 + 3.0 * (RotarySpeaker::kMaxRadiusM / RotarySpeaker::kSpeedOfSoundMps * RotarySpeaker::kSampleRateHz) + 3.0 <
                  RotarySpeaker::kDelayLine,
              "the largest Doppler delay plus the interpolator margin must fit the delay line");

namespace {
constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kQ = 0.70710678118654752440;  // Butterworth

double coef_for(double tau_s) noexcept {
  if (!(tau_s > 0.0)) return 1.0;
  return 1.0 - std::exp(-1.0 / (tau_s * RotarySpeaker::kSampleRateHz));
}
}  // namespace

void RotarySpeaker::design_lowpass(Biquad& b, double fc) noexcept {
  const double w0 = kTwoPi * fc / kSampleRateHz, cw = std::cos(w0), alpha = std::sin(w0) / (2.0 * kQ), a0 = 1.0 + alpha;
  b.b0 = (1.0 - cw) * 0.5 / a0;
  b.b1 = (1.0 - cw) / a0;
  b.b2 = b.b0;
  b.a1 = -2.0 * cw / a0;
  b.a2 = (1.0 - alpha) / a0;
}

void RotarySpeaker::design_highpass(Biquad& b, double fc) noexcept {
  const double w0 = kTwoPi * fc / kSampleRateHz, cw = std::cos(w0), alpha = std::sin(w0) / (2.0 * kQ), a0 = 1.0 + alpha;
  b.b0 = (1.0 + cw) * 0.5 / a0;
  b.b1 = -(1.0 + cw) / a0;
  b.b2 = b.b0;
  b.a1 = -2.0 * cw / a0;
  b.a2 = (1.0 - alpha) / a0;
}

RotarySpeaker::RotarySpeaker(const RotaryConfig& cfg) noexcept : cfg_(cfg) {
  // Keep every parameter inside what the fixed storage and the filters can take. Delay budget: with swing s = radius/c*fs samples
  // (<= 35 at 0.25 m) and cos(mic) in [0, 1], base <= kMinDelaySamples + s and the largest delay is base + 2 s <= 4 + 3 s = 109,
  // and the interpolator reads 3 more: 112 < kDelayLine (the static_assert below).
  auto finite_or = [](double v, double fallback) { return std::isfinite(v) ? v : fallback; };
  const double fc = std::clamp(finite_or(cfg_.crossover_hz, 800.0), 100.0, 8000.0);
  cfg_.crossover_hz = fc;
  cfg_.mic_half_angle_rad = std::clamp(finite_or(cfg_.mic_half_angle_rad, 1.0471975511965976), 0.0, 1.5707963267948966);
  for (int i = 0; i < 2; ++i) {
    design_lowpass(lp_[i], fc);
    design_highpass(hp_[i], fc);
  }
  for (RotorSpec* s : {&cfg_.horn, &cfg_.drum}) {
    s->radius_m = std::clamp(finite_or(s->radius_m, 0.0), 0.0, kMaxRadiusM);
    s->am_depth = std::clamp(static_cast<float>(finite_or(s->am_depth, 0.0)), 0.0f, 1.0f);
    s->slow_hz = std::clamp(finite_or(s->slow_hz, 0.0), 0.0, 20.0);
    s->fast_hz = std::clamp(finite_or(s->fast_hz, 0.0), 0.0, 20.0);
    s->accel_tau_s = finite_or(s->accel_tau_s, 0.0);
    s->brake_tau_s = finite_or(s->brake_tau_s, 0.0);
  }
  const double ca = std::cos(cfg_.mic_half_angle_rad);
  const double kh = cfg_.horn.radius_m / kSpeedOfSoundMps * kSampleRateHz;
  const double kd = cfg_.drum.radius_m / kSpeedOfSoundMps * kSampleRateHz;
  // Equal delay for both rotors at angle 0, so the bands add without a comb at the crossover while the rotors rest there.
  horn_.spec = cfg_.horn;
  drum_.spec = cfg_.drum;
  horn_.delay_swing = kh;
  drum_.delay_swing = kd;
  horn_.delay_base = kMinDelaySamples + std::max(0.0, kd - kh) * (1.0 - ca);
  drum_.delay_base = kMinDelaySamples + std::max(0.0, kh - kd) * (1.0 - ca);
  for (Rotor* r : {&horn_, &drum_}) {
    r->accel = coef_for(r->spec.accel_tau_s);
    r->brake = coef_for(r->spec.brake_tau_s);
  }
  set_speed_immediate(RotarySpeed::kSlow);
}

void RotarySpeaker::set_speed_immediate(RotarySpeed s) noexcept {
  target_ = s;
  horn_.speed = target_hz(horn_.spec, s);
  drum_.speed = target_hz(drum_.spec, s);
}

void RotarySpeaker::Rotor::step(RotarySpeed target) noexcept {
  const double t = target == RotarySpeed::kStop ? -kStopOvershootHz : target_hz(spec, target);
  const bool moving = speed > 0.0;
  speed += (t - speed) * (t > speed ? accel : brake);
  if (speed < 0.0) speed = 0.0;
  if (target == RotarySpeed::kStop && moving) {
    // Stopping: keep creeping until the common rest angle 0 (= 1 cycle) is reached, then stand still there.
    const bool creeping = speed <= kRestCreepHz;  // only the last, slow stage may stop at the rest angle
    if (speed < kRestCreepHz) speed = kRestCreepHz;
    const double next = phase + speed / kSampleRateHz;
    if (creeping && next >= 1.0) {
      phase = 0.0;
      speed = 0.0;
      return;
    }
    phase = next >= 1.0 ? next - 1.0 : next;
    return;
  }
  phase += speed / kSampleRateHz;
  if (phase >= 1.0) phase -= 1.0;
}

void RotarySpeaker::skip(std::uint32_t frames) noexcept {
  for (std::uint32_t f = 0; f < frames; ++f) {
    horn_.step(target_);
    drum_.step(target_);
  }
}

void RotarySpeaker::clear_signal() noexcept {
  for (Biquad& b : lp_) b.z1 = b.z2 = 0.0;
  for (Biquad& b : hp_) b.z1 = b.z2 = 0.0;
  horn_.line.fill(0.0f);
  drum_.line.fill(0.0f);
}

// Sample at `delay` samples (>= 1) behind the newest one (index pos_), 4-point Lagrange.
float RotarySpeaker::tap(const Rotor& r, double delay) const noexcept {
  const std::uint32_t i = static_cast<std::uint32_t>(delay);
  const float t = static_cast<float>(delay - static_cast<double>(i));
  constexpr std::uint32_t mask = kDelayLine - 1;
  const float a = r.line[(pos_ - (i - 1)) & mask], b = r.line[(pos_ - i) & mask];
  const float c = r.line[(pos_ - (i + 1)) & mask], d = r.line[(pos_ - (i + 2)) & mask];
  const float w0 = -t * (t - 1.0f) * (t - 2.0f) * (1.0f / 6.0f);
  const float w1 = (t + 1.0f) * (t - 1.0f) * (t - 2.0f) * 0.5f;
  const float w2 = -(t + 1.0f) * t * (t - 2.0f) * 0.5f;
  const float w3 = (t + 1.0f) * t * (t - 1.0f) * (1.0f / 6.0f);
  return a * w0 + b * w1 + c * w2 + d * w3;
}

void RotarySpeaker::process(const float* in, float* out_left, float* out_right, std::uint32_t frames) noexcept {
  const double ca = std::cos(cfg_.mic_half_angle_rad), sa = std::sin(cfg_.mic_half_angle_rad);
  for (std::uint32_t f = 0; f < frames; ++f) {
    const double x = in[f];
    const double low = lp_[1].run(lp_[0].run(x));
    const double high = hp_[1].run(hp_[0].run(x));
    horn_.step(target_);
    drum_.step(target_);
    pos_ = (pos_ + 1) & (kDelayLine - 1);
    horn_.line[pos_] = static_cast<float>(high);
    drum_.line[pos_] = static_cast<float>(low);
    float yl = 0.0f, yr = 0.0f;
    for (const Rotor* r : {&horn_, &drum_}) {
      const double th = kTwoPi * r->phase, c = std::cos(th), s = std::sin(th);
      const double cl = c * ca - s * sa;  // cos(angle - mic) for the left microphone at -half angle
      const double cr = c * ca + s * sa;  // right microphone at +half angle
      const float depth = r->spec.am_depth;
      const float gl = 1.0f - depth * static_cast<float>(1.0 - cl) * 0.5f;
      const float gr = 1.0f - depth * static_cast<float>(1.0 - cr) * 0.5f;
      yl += gl * tap(*r, r->delay_base + r->delay_swing * (1.0 - cl));
      yr += gr * tap(*r, r->delay_base + r->delay_swing * (1.0 - cr));
    }
    out_left[f] = yl;
    out_right[f] = yr;
  }
}

}  // namespace velora::core
