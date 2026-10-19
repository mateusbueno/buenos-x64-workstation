// Organ percussion and key click tests (Story 2.3). Bare main, CHECK style as the other core tests.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/params_gen.hpp"

using namespace velora::core;
namespace tw = velora::core::tonewheel;

static int g_fail = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    if (!(c)) {                                                          \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
      ++g_fail;                                                          \
    }                                                                    \
  } while (0)

namespace {

constexpr double kFs = 48000.0;
constexpr double kTwoPi = 6.283185307179586;
constexpr int kKey = 60;

void set_drawbars(OrganEngine& e, const char* nine, std::uint32_t part = 0) {
  for (int d = 0; d < 9; ++d) e.set_part_param(part, params::kDrawbar16 + d, static_cast<float>(nine[d] - '0'));
}
void settle(OrganEngine& e) {
  std::vector<float> l(64, 0.0f), r(64, 0.0f);
  for (int i = 0; i < 8; ++i) e.render(l.data(), r.data(), 64);
}
std::vector<float> render_mono(OrganEngine& e, std::uint32_t frames) {
  std::vector<float> l(frames, 0.0f), r(frames, 0.0f);
  e.render(l.data(), r.data(), frames);
  return l;
}
// Amplitude of the sinusoid at `hz` in x[from, from+n) (single-bin DFT, absolute time index t0 + i). Hann windowed
// by default so a neighbouring tone a few hundred Hz away leaks below -50 dB; `hann` false gives the plain DFT.
double amp_at(const std::vector<float>& x, std::size_t from, std::size_t n, double hz, std::size_t t0 = 0,
              bool hann = true) {
  std::complex<double> acc = 0;
  double wsum = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double w = hann ? 0.5 - 0.5 * std::cos(kTwoPi * (static_cast<double>(i) + 0.5) / static_cast<double>(n)) : 1.0;
    acc += w * static_cast<double>(x[from + i]) * std::polar(1.0, -kTwoPi * hz * static_cast<double>(t0 + from + i) / kFs);
    wsum += w;
  }
  return 2.0 * std::abs(acc) / wsum;
}
double wheel_hz(int wheel) { return OrganEngine::wheel_frequency_hz(static_cast<std::uint32_t>(wheel)); }
double drawbar_hz(int note, int drawbar) { return wheel_hz(tw::drawbar_wheel(note, drawbar)); }
double energy(const std::vector<float>& x, std::size_t from, std::size_t n) {
  double e = 0;
  for (std::size_t i = from; i < from + n && i < x.size(); ++i) e += static_cast<double>(x[i]) * x[i];
  return e;
}
bool all_zero(const std::vector<float>& x, std::size_t from, std::size_t n) {
  for (std::size_t i = from; i < from + n && i < x.size(); ++i)
    if (x[i] != 0.0f) return false;
  return true;
}

// Percussion-only engine: all drawbars off, key click off, percussion on with the given settings.
void perc_setup(OrganEngine& e, int harmonic, int decay, int volume, std::uint32_t part = 0) {
  set_drawbars(e, "000000000", part);
  e.set_part_param(part, params::kKeyClick, 0.0f);
  e.set_part_param(part, params::kPercussionOn, 1.0f);
  e.set_part_param(part, params::kPercussionHarmonic, static_cast<float>(harmonic));
  e.set_part_param(part, params::kPercussionDecay, static_cast<float>(decay));
  e.set_part_param(part, params::kPercussionVolume, static_cast<float>(volume));
  settle(e);
}
constexpr int kSecond = params::kPercussionHarmonicSecond, kThird = params::kPercussionHarmonicThird;
constexpr int kFast = params::kPercussionDecayFast, kSlow = params::kPercussionDecaySlow;
constexpr int kSoft = params::kPercussionVolumeSoft, kNormal = params::kPercussionVolumeNormal;
constexpr int kDbSecond = OrganEngine::kPercSecondDrawbar, kDbThird = OrganEngine::kPercThirdDrawbar;

void test_registry_and_defaults() {
  CHECK(params::find_by_cc(80) == params::find(params::kPercussionOn));
  CHECK(params::find_by_cc(82) == params::find(params::kKeyClick));
  CHECK(params::find_by_cc(84) == params::find(params::kPercussionHarmonic));
  CHECK(params::find_by_cc(85) == params::find(params::kPercussionDecay));
  CHECK(params::find_by_cc(86) == params::find(params::kPercussionVolume));
  // Defaults: percussion off (1' and the drawbars untouched), key click at the registry default.
  OrganEngine e;
  CHECK(e.percussion_cancel_factor(0) == 1.0f && e.percussion_drawbar_factor(0) == 1.0f);
  CHECK(kDbSecond == 3 && kDbThird == 4);
  CHECK(tw::kDrawbarSemitones[kDbSecond] == 12 && tw::kDrawbarSemitones[kDbThird] == 19);  // octave and octave+fifth
}

void test_single_trigger() {
  // Legato: a second key pressed while the first is held does not sound percussion.
  OrganEngine e;
  perc_setup(e, kThird, kSlow, kNormal);
  CHECK(e.start_voice(0, 60, 0xFFFF) == EngineStatus::kOk);
  auto x = render_mono(e, 2400);
  const double want = OrganEngine::kVoiceScale * OrganEngine::kPercNormalGain;
  const double a60 = amp_at(x, 480, 1920, drawbar_hz(60, kDbThird));
  CHECK(a60 > 0.6 * want && a60 < 1.05 * want);
  CHECK(e.start_voice(0, 64, 0xFFFF) == EngineStatus::kOk);  // legato
  x = render_mono(e, 2400);
  CHECK(amp_at(x, 480, 1920, drawbar_hz(64, kDbThird)) < 2e-3);
  CHECK(amp_at(x, 480, 1920, drawbar_hz(64, kDbThird)) < 0.01 * a60);
  // Releasing one of two keys and pressing a third while one is still held: still no trigger.
  e.stop_voice(0, 60);
  render_mono(e, 480);
  CHECK(e.start_voice(0, 67, 0xFFFF) == EngineStatus::kOk);  // 64 still held
  x = render_mono(e, 2400);
  CHECK(amp_at(x, 480, 1920, drawbar_hz(67, kDbThird)) < 1e-4);
  // Releasing all keys and pressing again retriggers.
  e.stop_voice(0, 64);
  e.stop_voice(0, 67);
  render_mono(e, 4800);
  CHECK(e.active_voices() == 0);
  CHECK(e.start_voice(0, 64, 0xFFFF) == EngineStatus::kOk);
  x = render_mono(e, 2400);
  const double again = amp_at(x, 480, 1920, drawbar_hz(64, kDbThird));
  CHECK(again > 0.6 * want && again < 1.05 * want);
  // A key still releasing (key up) does not count as held: it retriggers at once.
  e.stop_voice(0, 64);
  CHECK(e.start_voice(0, 72, 0xFFFF) == EngineStatus::kOk);
  x = render_mono(e, 2400);
  CHECK(amp_at(x, 480, 1920, drawbar_hz(72, kDbThird)) > 0.6 * want);
  // The same note re-pressed while held is not a press at all.
  CHECK(e.start_voice(0, 72, 0xFFFF) == EngineStatus::kOk);
  // Percussion off: a first key sounds no percussion.
  OrganEngine off;
  perc_setup(off, kThird, kSlow, kNormal);
  off.set_part_param(0, params::kPercussionOn, 0.0f);
  settle(off);
  off.start_voice(0, 60, 0xFFFF);
  x = render_mono(off, 2400);
  CHECK(all_zero(x, 0, 2400));
  // Each part has its own trigger: a key held on part 0 does not suppress part 1.
  OrganEngine two;
  perc_setup(two, kThird, kSlow, kNormal, 0);
  perc_setup(two, kThird, kSlow, kNormal, 1);
  two.start_voice(0, 60, 0xFFFF);
  render_mono(two, 480);
  two.start_voice(1, 64, 0xFFFF);
  x = render_mono(two, 2400);
  CHECK(amp_at(x, 480, 1920, drawbar_hz(64, kDbThird), 480) > 0.6 * want);
}

// Time constant from the envelope of the percussion tone: amplitude at two windows one tau apart.
double measured_tau(int decay, double expected_tau) {
  OrganEngine e;
  perc_setup(e, kThird, decay, kNormal);
  e.start_voice(0, kKey, 0xFFFF);
  const std::uint32_t win = 960;
  const std::uint32_t d = static_cast<std::uint32_t>(expected_tau * kFs);
  const std::uint32_t t0 = 1920;
  const auto x = render_mono(e, t0 + d + win);
  const double hz = drawbar_hz(kKey, kDbThird);
  const double a0 = amp_at(x, t0, win, hz), a1 = amp_at(x, t0 + d, win, hz);
  return static_cast<double>(d) / kFs / std::log(a0 / a1);
}

void test_decay_constants() {
  CHECK(OrganEngine::percussion_tau_seconds(kFast) == OrganEngine::kPercTauFastSeconds);
  CHECK(OrganEngine::percussion_tau_seconds(kSlow) == OrganEngine::kPercTauSlowSeconds);
  CHECK(OrganEngine::kPercTauFastSeconds < OrganEngine::kPercTauSlowSeconds);
  const double fast = measured_tau(kFast, OrganEngine::kPercTauFastSeconds);
  const double slow = measured_tau(kSlow, OrganEngine::kPercTauSlowSeconds);
  CHECK(std::fabs(fast / OrganEngine::kPercTauFastSeconds - 1.0) < 0.04);
  CHECK(std::fabs(slow / OrganEngine::kPercTauSlowSeconds - 1.0) < 0.04);
  CHECK(slow > 2.5 * fast);
  // Percussion ends: well past 10 tau the tone is gone while the key is still held.
  OrganEngine e;
  perc_setup(e, kThird, kSlow, kNormal);
  e.start_voice(0, kKey, 0xFFFF);
  const auto x = render_mono(e, 96000);
  CHECK(amp_at(x, 72000, 24000, drawbar_hz(kKey, kDbThird)) < 1e-4);
  CHECK(e.active_voices() == 1);
}

void test_harmonic_selection() {
  const double want = OrganEngine::kVoiceScale * OrganEngine::kPercNormalGain;
  for (int harmonic : {kSecond, kThird}) {
    OrganEngine e;
    perc_setup(e, harmonic, kSlow, kNormal);
    e.start_voice(0, kKey, 0xFFFF);
    const auto x = render_mono(e, 4800);
    const int on = harmonic == kSecond ? kDbSecond : kDbThird, off = harmonic == kSecond ? kDbThird : kDbSecond;
    const double got = amp_at(x, 480, 1920, drawbar_hz(kKey, on));
    CHECK(got > 0.8 * want && got < 1.02 * want);  // the slow decay has not moved much by 10..50 ms
    CHECK(amp_at(x, 480, 1920, drawbar_hz(kKey, off)) < 0.01 * want);
    CHECK(amp_at(x, 480, 1920, drawbar_hz(kKey, 2)) < 0.01 * want);  // not the 8' either
  }
  // 2nd harmonic = octave above the key (4'), 3rd = the twelfth (2 2/3'): checked against musical intervals.
  CHECK(std::fabs(drawbar_hz(kKey, kDbSecond) / drawbar_hz(kKey, 2) - 2.0) < 1e-9);
  CHECK(std::fabs(1200.0 * std::log2(drawbar_hz(kKey, kDbThird) / drawbar_hz(kKey, 2) / 3.0)) < 2.5);  // 3:1 within 2 cents
  // A change of harmonic takes effect at the next press, not on a decaying voice.
  OrganEngine e;
  perc_setup(e, kSecond, kSlow, kNormal);
  e.start_voice(0, kKey, 0xFFFF);
  render_mono(e, 480);
  e.set_part_param(0, params::kPercussionHarmonic, static_cast<float>(kThird));
  const auto x = render_mono(e, 4800);
  CHECK(amp_at(x, 480, 1920, drawbar_hz(kKey, kDbSecond), 480) > 0.5 * want);
  CHECK(amp_at(x, 480, 1920, drawbar_hz(kKey, kDbThird), 480) < 0.01 * want);
}

void test_volume_and_drawbar_lowering() {
  // Normal vs soft percussion level: soft is 6 dB lower.
  double peak[2] = {};
  for (int vol : {kSoft, kNormal}) {
    OrganEngine e;
    perc_setup(e, kThird, kFast, vol);
    e.start_voice(0, kKey, 0xFFFF);
    const auto x = render_mono(e, 2400);
    peak[vol] = amp_at(x, 480, 960, drawbar_hz(kKey, kDbThird));
  }
  CHECK(std::fabs(peak[kNormal] / peak[kSoft] - 2.0) < 0.01);
  CHECK(std::fabs(OrganEngine::kPercNormalGain / OrganEngine::kPercSoftGain - 2.0) < 1e-6);
  // Normal volume lowers the drawbar level (-3 dB); soft leaves it alone. Key pressed before percussion is on, so it
  // carries no percussion tone: only the drawbar change is seen.
  const double want = OrganEngine::kVoiceScale;
  for (int vol : {kSoft, kNormal}) {
    OrganEngine e;
    set_drawbars(e, "008000000");
    e.set_part_param(0, params::kKeyClick, 0.0f);
    settle(e);
    e.start_voice(0, kKey, 0xFFFF);
    auto x = render_mono(e, 4800);
    CHECK(std::fabs(amp_at(x, 960, 3840, drawbar_hz(kKey, 2)) - want) < 0.01 * want);  // percussion off
    e.set_part_param(0, params::kPercussionVolume, static_cast<float>(vol));
    e.set_part_param(0, params::kPercussionOn, 1.0f);
    x = render_mono(e, 4800);
    const double got = amp_at(x, 960, 3840, drawbar_hz(kKey, 2), 4800);
    CHECK(std::fabs(got / want - (vol == kNormal ? OrganEngine::kPercDrawbarGain : 1.0f)) < 0.01);
    CHECK(amp_at(x, 960, 3840, drawbar_hz(kKey, kDbThird), 4800) < 1e-3);  // legato: no percussion on this key
    if (vol == kNormal) CHECK(std::fabs(20.0 * std::log10(OrganEngine::kPercDrawbarGain) + 3.0) < 0.02);
    e.set_part_param(0, params::kPercussionOn, 0.0f);  // percussion off again: the level comes back
    x = render_mono(e, 4800);
    CHECK(std::fabs(amp_at(x, 960, 3840, drawbar_hz(kKey, 2), 9600) - want) < 0.01 * want);
  }
}

void test_one_foot_cancelled() {
  // Registration with only the 1' drawbar: sounds with percussion off, silent (apart from percussion) when on.
  const double want = OrganEngine::kVoiceScale;
  OrganEngine e;
  set_drawbars(e, "000000008");
  e.set_part_param(0, params::kKeyClick, 0.0f);
  settle(e);
  e.start_voice(0, kKey, 0xFFFF);
  auto x = render_mono(e, 4800);
  CHECK(std::fabs(amp_at(x, 960, 3840, drawbar_hz(kKey, 8)) - want) < 0.01 * want);
  e.set_part_param(0, params::kPercussionOn, 1.0f);
  e.set_part_param(0, params::kPercussionVolume, static_cast<float>(kSoft));  // soft: no lowering, only the cancel
  x = render_mono(e, 4800);
  CHECK(amp_at(x, 960, 3840, drawbar_hz(kKey, 8), 4800) < 1e-3 * want);
  CHECK(e.percussion_cancel_factor(0) == 0.0f);
  // It is a smooth 5 ms ramp, not a step: the largest change per sample stays within the ramp slope.
  OrganEngine r;
  set_drawbars(r, "000000008");
  r.set_part_param(0, params::kKeyClick, 0.0f);
  settle(r);
  r.start_voice(0, kKey, 0xFFFF);
  render_mono(r, 4800);
  r.set_part_param(0, params::kPercussionOn, 1.0f);
  const auto y = render_mono(r, 1200);
  double worst = 0;
  for (std::size_t i = 1; i < y.size(); ++i) worst = std::max(worst, static_cast<double>(std::fabs(y[i] - y[i - 1])));
  CHECK(worst < 0.5 * want);  // a hard step of the 1' tone would jump by up to 2 x want
  // With percussion on, a new key still has the 1' cancelled and a percussion tone instead.
  OrganEngine p;
  set_drawbars(p, "000000008");
  p.set_part_param(0, params::kKeyClick, 0.0f);
  p.set_part_param(0, params::kPercussionOn, 1.0f);
  p.set_part_param(0, params::kPercussionVolume, static_cast<float>(kSoft));
  p.set_part_param(0, params::kPercussionDecay, static_cast<float>(kSlow));
  settle(p);
  p.start_voice(0, kKey, 0xFFFF);
  x = render_mono(p, 4800);
  CHECK(amp_at(x, 960, 3840, drawbar_hz(kKey, 8)) < 1e-3 * want);
  CHECK(amp_at(x, 960, 3840, drawbar_hz(kKey, kDbThird)) > 0.5 * want);
}

// Onset click of `note` on a fresh engine (first press): 400 frames after the key closes.
std::vector<float> onset_click(int note, float level) {
  OrganEngine e;
  set_drawbars(e, "000000000");
  e.set_part_param(0, params::kKeyClick, level);
  settle(e);
  e.start_voice(0, static_cast<std::uint32_t>(note), 0xFFFF);
  return render_mono(e, 400);
}

double correlation(const std::vector<float>& a, const std::vector<float>& b, std::size_t n) {
  double ab = 0, aa = 0, bb = 0;
  for (std::size_t i = 0; i < n; ++i) {
    ab += static_cast<double>(a[i]) * b[i];
    aa += static_cast<double>(a[i]) * a[i];
    bb += static_cast<double>(b[i]) * b[i];
  }
  return ab / std::sqrt(aa * bb);
}

void test_key_click() {
  // Level 0 is off: exact silence at onset and release, and the voice ends with its gate.
  {
    OrganEngine e;
    set_drawbars(e, "000000000");
    e.set_part_param(0, params::kKeyClick, 0.0f);
    settle(e);
    e.start_voice(0, kKey, 0xFFFF);
    auto x = render_mono(e, 1000);
    e.stop_voice(0, kKey);
    auto y = render_mono(e, 1000);
    CHECK(all_zero(x, 0, 1000) && all_zero(y, 0, 1000));
    CHECK(e.active_voices() == 0);
  }
  // Present at onset and at release, short, and nothing between them.
  OrganEngine e;
  set_drawbars(e, "000000000");
  e.set_part_param(0, params::kKeyClick, 1.0f);
  settle(e);
  e.start_voice(0, kKey, 0xFFFF);
  const auto on = render_mono(e, 2000);
  CHECK(energy(on, 0, OrganEngine::kClickFrames) > 1e-6);
  CHECK(all_zero(on, OrganEngine::kClickFrames, 2000 - OrganEngine::kClickFrames));
  // Most of the burst is in the first millisecond (fast exponential decay).
  CHECK(energy(on, 0, 48) > 0.5 * energy(on, 0, OrganEngine::kClickFrames));
  e.stop_voice(0, kKey);
  const auto off = render_mono(e, 2000);
  CHECK(energy(off, 0, OrganEngine::kClickFrames) > 1e-7);
  CHECK(all_zero(off, OrganEngine::kClickFrames, 2000 - OrganEngine::kClickFrames));
  CHECK(e.active_voices() == 0);  // the voice lived until its release click was out
  const double e_on = energy(on, 0, OrganEngine::kClickFrames), e_off = energy(off, 0, OrganEngine::kClickFrames);
  CHECK(e_off < e_on && e_off > 0.05 * e_on);  // the release click is the smaller of the two, but there
  // Broadband: the burst has energy across the spectrum, not a tone.
  const double top = amp_at(on, 0, OrganEngine::kClickFrames, 4000.0, 0, false);
  int present = 0;
  for (double hz : {1500.0, 3000.0, 4500.0, 6000.0, 8000.0, 10000.0, 12000.0, 16000.0, 20000.0})
    present += amp_at(on, 0, OrganEngine::kClickFrames, hz, 0, false) > 0.08 * top ? 1 : 0;
  CHECK(present >= 8);
  // Scaled by the level: the same press at half the level is a quarter of the energy (the burst is linear in it).
  const auto half = onset_click(kKey, 0.5f), full = onset_click(kKey, 1.0f);
  CHECK(std::fabs(energy(half, 0, 400) / energy(full, 0, 400) - 0.25) < 0.01);
  CHECK(correlation(half, full, 400) > 0.999);
  // Varies per key: other keys give other bursts; and across the keyboard the key amplitudes are not all equal.
  const auto k61 = onset_click(kKey + 1, 1.0f);
  CHECK(std::fabs(correlation(full, k61, 400)) < 0.5);
  double lo = 1e9, hi = 0;
  for (int note = 28; note <= 100; ++note) {
    const double en = energy(onset_click(note, 1.0f), 0, 400);
    CHECK(en > 0.0);
    lo = std::min(lo, en);
    hi = std::max(hi, en);
  }
  CHECK(hi > 1.5 * lo);
  // Varies per press: the same key pressed again is not the same burst.
  OrganEngine rep;
  set_drawbars(rep, "000000000");
  rep.set_part_param(0, params::kKeyClick, 1.0f);
  settle(rep);
  std::vector<std::vector<float>> presses;
  for (int i = 0; i < 4; ++i) {
    rep.start_voice(0, kKey, 0xFFFF);
    presses.push_back(render_mono(rep, 400));
    rep.stop_voice(0, kKey);
    render_mono(rep, 1000);
  }
  for (int i = 0; i < 4; ++i)
    for (int j = i + 1; j < 4; ++j) {
      CHECK(presses[i] != presses[j]);
      CHECK(std::fabs(correlation(presses[i], presses[j], 400)) < 0.8);  // identical bursts would give 1
    }
  // Click is part of the signal, not of the tone: with the drawbars up, the tone is unchanged after the burst.
  OrganEngine t0, t1;
  for (OrganEngine* t : {&t0, &t1}) {
    set_drawbars(*t, "008000000");
    t->set_part_param(0, params::kKeyClick, t == &t0 ? 0.0f : 1.0f);
    settle(*t);
    t->start_voice(0, kKey, 0xFFFF);
  }
  const auto a = render_mono(t0, 4800), b = render_mono(t1, 4800);
  for (std::size_t i = OrganEngine::kClickFrames; i < 4800; ++i) CHECK(a[i] == b[i]);
}

void test_determinism() {
  auto run = [](std::vector<float>& out) {
    OrganEngine e;
    set_drawbars(e, "808400000");
    e.set_part_param(0, params::kKeyClick, 0.7f);
    e.set_part_param(0, params::kPercussionOn, 1.0f);
    e.set_part_param(0, params::kPercussionVolume, static_cast<float>(kNormal));
    settle(e);
    for (int rep = 0; rep < 3; ++rep) {
      for (int n : {48, 55, 60, 64}) e.start_voice(0, static_cast<std::uint32_t>(n), 0xFFFF);
      auto x = render_mono(e, 7000);
      out.insert(out.end(), x.begin(), x.end());
      for (int n : {48, 55, 60, 64}) e.stop_voice(0, static_cast<std::uint32_t>(n));
      x = render_mono(e, 3000);
      out.insert(out.end(), x.begin(), x.end());
    }
  };
  std::vector<float> a, b;
  run(a);
  run(b);
  CHECK(a == b);
  CHECK(energy(a, 0, a.size()) > 0.0);
  for (float v : a) CHECK(std::isfinite(v) && std::fabs(v) <= 1.0f);
}

void test_through_block_loop() {
  // The parameters reach the engine through the same path as the drawbars: UMP events and the mapped CCs.
  OrganEngine organ;
  BlockRenderer br(32);
  CHECK(br.bind_engine(organ));
  br.set_channel_engine(0, static_cast<std::uint32_t>(EngineTypeId::kOrgan));
  for (std::uint32_t d = 0; d < 9; ++d)
    br.submit({make_param_event(*params::find(params::kDrawbar16 + d), 0, 0), 0});
  Ump ev;
  CHECK(cc_to_param_event(make_control_change(0, 0, 80, 0xFFFFFFFFu), ev));  // percussion on
  br.submit({ev, 0});
  CHECK(cc_to_param_event(make_control_change(0, 0, 82, 0u), ev));  // key click 0
  br.submit({ev, 0});
  CHECK(cc_to_param_event(make_control_change(0, 0, 84, 0u), ev));  // second harmonic
  br.submit({ev, 0});
  CHECK(cc_to_param_event(make_control_change(0, 0, 85, 0xFFFFFFFFu), ev));  // slow
  br.submit({ev, 0});
  CHECK(cc_to_param_event(make_control_change(0, 0, 86, 0xFFFFFFFFu), ev));  // normal
  br.submit({ev, 0});
  float l[64], r[64];
  for (int i = 0; i < 8; ++i) br.process_block(l, r);
  br.submit({make_note_on(0, 0, kKey, 0xFFFF), 5});
  std::vector<float> x;
  for (int i = 0; i < 80; ++i) {
    br.process_block(l, r);
    x.insert(x.end(), l, l + 64);
  }
  const double want = OrganEngine::kVoiceScale * OrganEngine::kPercNormalGain;
  CHECK(amp_at(x, 480, 1920, drawbar_hz(kKey, kDbSecond), 0) > 0.5 * want);
  CHECK(amp_at(x, 480, 1920, drawbar_hz(kKey, kDbThird), 0) < 0.01 * want);
  // Legato through the block loop: a second note-on does not sound percussion.
  br.submit({make_note_on(0, 0, kKey + 4, 0xFFFF), 3});
  std::vector<float> y;
  for (int i = 0; i < 80; ++i) {
    br.process_block(l, r);
    y.insert(y.end(), l, l + 64);
  }
  CHECK(amp_at(y, 480, 1920, drawbar_hz(kKey + 4, kDbSecond), 0) < 1e-4);
}

// Steps the engine `n` frames and returns the part-0 ramp factors.
struct Factors { float cancel, lower; };
Factors step_frames(OrganEngine& e, std::uint32_t n) {
  render_mono(e, n);
  return {e.percussion_cancel_factor(0), e.percussion_drawbar_factor(0)};
}

void test_ramps_are_five_ms_ramps() {
  const float lo = OrganEngine::kPercDrawbarGain;
  CHECK(OrganEngine::kPercRampFrames == 240);
  // Frame by frame, on and then off: monotonic, linear, not there until exactly 240 frames, then constant.
  OrganEngine e;
  e.set_part_param(0, params::kPercussionVolume, static_cast<float>(kNormal));
  CHECK(e.percussion_cancel_factor(0) == 1.0f && e.percussion_drawbar_factor(0) == 1.0f);
  e.set_part_param(0, params::kPercussionOn, 1.0f);
  Factors prev{1.0f, 1.0f};
  for (std::uint32_t i = 1; i <= 240; ++i) {
    const Factors f = step_frames(e, 1);
    CHECK(f.cancel < prev.cancel && f.lower < prev.lower);  // strictly falling during the ramp
    CHECK(std::fabs(f.cancel - (1.0f - static_cast<float>(i) / 240.0f)) < 1e-5f);
    CHECK(std::fabs(f.lower - (1.0f + (lo - 1.0f) * static_cast<float>(i) / 240.0f)) < 1e-5f);
    if (i < 240) CHECK(f.cancel > 0.0f && f.lower > lo);
    prev = f;
  }
  CHECK(prev.cancel == 0.0f && prev.lower == lo);
  CHECK(step_frames(e, 500).cancel == 0.0f && e.percussion_drawbar_factor(0) == lo);
  e.set_part_param(0, params::kPercussionOn, 0.0f);
  prev = {0.0f, lo};
  for (std::uint32_t i = 1; i <= 240; ++i) {
    const Factors f = step_frames(e, 1);
    CHECK(f.cancel > prev.cancel && f.lower > prev.lower);
    if (i < 240) CHECK(f.cancel < 1.0f && f.lower < 1.0f);
    prev = f;
  }
  CHECK(prev.cancel == 1.0f && prev.lower == 1.0f);
  // Block by block (64 frames): 3 blocks are short of the target, the 4th block's first 48 frames reach it exactly.
  OrganEngine b;
  b.set_part_param(0, params::kPercussionVolume, static_cast<float>(kNormal));
  b.set_part_param(0, params::kPercussionOn, 1.0f);
  float last = 1.0f;
  for (int blk = 1; blk <= 3; ++blk) {
    const Factors f = step_frames(b, 64);
    CHECK(f.cancel < last && f.cancel > 0.0f && f.lower > lo);
    CHECK(std::fabs(f.cancel - (1.0f - 64.0f * blk / 240.0f)) < 1e-5f);
    last = f.cancel;
  }
  const Factors done = step_frames(b, 48);
  CHECK(done.cancel == 0.0f && done.lower == lo);
}

void test_release_within_onset_burst() {
  auto make = [] {
    auto e = std::make_unique<OrganEngine>();
    set_drawbars(*e, "000000000");
    e->set_part_param(0, params::kKeyClick, 1.0f);
    settle(*e);
    return e;
  };
  auto a = make(), b = make();
  a->start_voice(0, kKey, 0xFFFF);
  const auto xa = render_mono(*a, 500);
  b->start_voice(0, kKey, 0xFFFF);
  auto xb = render_mono(*b, 20);
  b->stop_voice(0, kKey);  // the contact opens 20 frames (0.4 ms) into the onset burst
  const auto rest = render_mono(*b, 480);
  xb.insert(xb.end(), rest.begin(), rest.end());
  for (int i = 0; i < 20; ++i) CHECK(xa[i] == xb[i]);
  // The onset burst is not cut: B stays correlated with A through the rest of the onset burst (A is a component of B).
  std::vector<float> ta(xa.begin() + 20, xa.begin() + 192), tb(xb.begin() + 20, xb.begin() + 192);
  CHECK(correlation(ta, tb, ta.size()) > 0.3);  // without the fix the onset tail is gone and this is about 0
  // Exactly additive: B - A is the very release burst a late release (after the onset burst is over) produces.
  auto d = make();
  d->start_voice(0, kKey, 0xFFFF);
  auto xd = render_mono(*d, 300);
  d->stop_voice(0, kKey);
  const auto rd = render_mono(*d, 300);
  xd.insert(xd.end(), rd.begin(), rd.end());
  for (int k = 0; k < 192; ++k) CHECK(std::fabs((xb[20 + k] - xa[20 + k]) - xd[300 + k]) < 1e-6f);
  // Both end: the release burst runs 192 frames from the release; then silence, and the voice is gone.
  CHECK(all_zero(xb, 212, 288));
  CHECK(b->active_voices() == 0);
  CHECK(energy(xb, 20, 192) > energy(xa, 20, 192) * 0.9);  // release burst added, nothing removed
  // No discontinuity at the release: the largest sample step stays within the scale of the burst's own steps.
  double step_a = 0, step_b = 0;
  for (int i = 1; i < 200; ++i) {
    step_a = std::max(step_a, static_cast<double>(std::fabs(xa[i] - xa[i - 1])));
    step_b = std::max(step_b, static_cast<double>(std::fabs(xb[i] - xb[i - 1])));
  }
  CHECK(step_b < 2.5 * step_a);
}

void test_voice_info_level() {
  auto level_of = [](OrganEngine& e, std::uint32_t note, float& lvl) {
    VoiceInfo vi;
    for (std::uint32_t s = 0; s < e.voice_capacity(); ++s)
      if (e.voice_info(s, vi) && vi.note == note) {
        lvl = vi.level;
        return true;
      }
    return false;
  };
  float lvl = 0;
  OrganEngine p;  // percussion only
  perc_setup(p, kThird, kSlow, kNormal);
  p.start_voice(0, kKey, 0xFFFF);
  render_mono(p, 480);
  CHECK(level_of(p, kKey, lvl) && lvl > 0.5f * OrganEngine::kVoiceScale * OrganEngine::kPercNormalGain);
  OrganEngine c;  // a releasing voice whose tone is gone but whose click is still going
  set_drawbars(c, "000000000");
  c.set_part_param(0, params::kKeyClick, 1.0f);
  settle(c);
  c.start_voice(0, kKey, 0xFFFF);
  render_mono(c, 10);
  c.stop_voice(0, kKey);
  render_mono(c, 120);  // 96-frame gate is over, 130 < 192 frames of burst
  CHECK(level_of(c, kKey, lvl) && lvl > 0.0f);
  render_mono(c, 200);
  CHECK(!level_of(c, kKey, lvl));
}

void test_part_disable_enable() {
  OrganEngine e;
  perc_setup(e, kThird, kSlow, kNormal);
  e.start_voice(0, kKey, 0xFFFF);
  render_mono(e, 480);
  CHECK(e.active_voices() == 1);
  e.set_enabled_parts(0xFE);  // part 0 off: its voices (and so their percussion envelopes) are gone
  CHECK(e.active_voices() == 0);
  e.set_enabled_parts(0xFF);
  CHECK(all_zero(render_mono(e, 2400), 0, 2400));  // nothing resurrected
  e.start_voice(0, kKey, 0xFFFF);  // a new press triggers afresh
  const auto x = render_mono(e, 2400);
  CHECK(amp_at(x, 480, 1920, drawbar_hz(kKey, kDbThird)) > 0.6 * OrganEngine::kVoiceScale * OrganEngine::kPercNormalGain);
}

void test_click_per_part_determinism() {
  // Part 0's clicks do not depend on how often other parts were pressed (silent presses on part 1 in between).
  auto run = [](bool other_part) {
    OrganEngine e;
    set_drawbars(e, "000000000", 0);
    set_drawbars(e, "000000000", 1);
    e.set_part_param(0, params::kKeyClick, 1.0f);
    e.set_part_param(1, params::kKeyClick, 0.0f);
    settle(e);
    std::vector<float> out;
    for (int n : {60, 64, 60, 67}) {
      if (other_part) {
        e.start_voice(1, 50, 0xFFFF);
        e.stop_voice(1, 50);
      }
      e.start_voice(0, static_cast<std::uint32_t>(n), 0xFFFF);
      auto x = render_mono(e, 300);
      out.insert(out.end(), x.begin(), x.end());
      e.stop_voice(0, static_cast<std::uint32_t>(n));
      x = render_mono(e, 300);
      out.insert(out.end(), x.begin(), x.end());
    }
    return out;
  };
  const auto a = run(false), b = run(true);
  CHECK(a == b);
  CHECK(energy(a, 0, a.size()) > 0.0);
}

void test_reclaim_prefers_tail_without_click() {
  OrganEngine e;
  for (std::uint32_t part = 0; part < 8; ++part) {
    set_drawbars(e, "000000000", part);
    e.set_part_param(part, params::kKeyClick, 0.0f);
  }
  settle(e);
  // 584 keys down; the 56 keys part 0 notes 29..84 are pressed last (younger than the others).
  for (std::uint32_t part = 0; part < 8; ++part)
    for (std::uint32_t n = 28; n <= 100; ++n)
      if (!(part == 0 && n >= 29 && n < 29 + 56)) e.start_voice(part, n, 0xFFFF);
  render_mono(e, 64);
  for (std::uint32_t n = 29; n < 29 + 56; ++n) e.start_voice(0, n, 0xFFFF);
  render_mono(e, 400);
  CHECK(e.active_voices() == 584);
  for (std::uint32_t n = 29; n < 29 + 56; ++n) e.stop_voice(0, n);  // click off: tails with no burst pending
  for (std::uint32_t n = 29; n < 29 + 56; ++n) e.start_voice(0, n, 0xFFFF);
  CHECK(e.active_voices() == OrganEngine::kVoiceCapacity);  // pool full
  e.set_part_param(1, params::kKeyClick, 1.0f);
  e.stop_voice(1, 28);  // older than the tails, with a release burst pending
  CHECK(e.start_voice(1, 28, 0xFFFF) == EngineStatus::kOk);  // needs a slot: reclaims a tail, not the clicking voice
  VoiceInfo vi;
  bool found_releasing_click = false;
  for (std::uint32_t s = 0; s < e.voice_capacity(); ++s)
    if (e.voice_info(s, vi) && vi.channel == 1 && vi.note == 28 && vi.state == VoiceState::kReleasing && vi.level > 0.0f)
      found_releasing_click = true;
  CHECK(found_releasing_click);
}

void test_release_click_ratio() {
  // Over many presses the release burst carries kClickReleaseRatio^2 of the onset burst's energy (0.36).
  OrganEngine e;
  set_drawbars(e, "000000000");
  e.set_part_param(0, params::kKeyClick, 1.0f);
  settle(e);
  double on = 0, off = 0;
  for (std::uint32_t n = 28; n <= 100; ++n) {
    e.start_voice(0, n, 0xFFFF);
    on += energy(render_mono(e, 300), 0, 300);
    e.stop_voice(0, n);
    off += energy(render_mono(e, 300), 0, 300);
  }
  const double ratio = off / on;
  const double want = OrganEngine::kClickReleaseRatio * OrganEngine::kClickReleaseRatio;
  CHECK(ratio > 0.8 * want && ratio < 1.25 * want);  // would fail for a release ratio of 0.3 (0.09) or 0.9 (0.81)
}

void test_enum_cc_boundaries() {
  // 7-bit controller values map to the 32-bit scale as the offline renderer does; 63 is the first entry, 64 the second.
  auto decoded = [](std::uint32_t cc, std::uint32_t v7) {
    Ump ev, ce = make_control_change(0, 0, cc, static_cast<std::uint32_t>(v7 * 0xFFFFFFFFull / 127));
    ParamEvent pe;
    if (!cc_to_param_event(ce, ev) || !decode_param_event(ev, pe)) return -1.0f;
    return pe.value;
  };
  for (std::uint32_t cc : {84u, 85u, 86u}) {
    CHECK(decoded(cc, 0) == 0.0f && decoded(cc, 63) == 0.0f);
    CHECK(decoded(cc, 64) == 1.0f && decoded(cc, 127) == 1.0f);
  }
  CHECK(decoded(80, 63) == 0.0f && decoded(80, 64) == 1.0f);  // the bool too
}

void test_pedal_and_percussion() {
  // Documented (ORGAN.md): a key held by the pedal still counts as held, so a second key pressed after the first
  // was released under the pedal gets no percussion.
  OrganEngine organ;
  BlockRenderer br(32);
  CHECK(br.bind_engine(organ));
  br.set_channel_engine(0, static_cast<std::uint32_t>(EngineTypeId::kOrgan));
  for (std::uint32_t d = 0; d < 9; ++d) br.submit({make_param_event(*params::find(params::kDrawbar16 + d), 0, 0), 0});
  Ump ev;
  CHECK(cc_to_param_event(make_control_change(0, 0, 80, 0xFFFFFFFFu), ev));
  br.submit({ev, 0});
  CHECK(cc_to_param_event(make_control_change(0, 0, 82, 0u), ev));
  br.submit({ev, 0});
  CHECK(cc_to_param_event(make_control_change(0, 0, 85, 0xFFFFFFFFu), ev));
  br.submit({ev, 0});
  float l[64], r[64];
  for (int i = 0; i < 8; ++i) br.process_block(l, r);
  br.submit({make_control_change(0, 0, 64, 0xFFFFFFFFu), 0});  // pedal down
  br.submit({make_note_on(0, 0, kKey, 0xFFFF), 1});
  std::vector<float> x;
  auto run = [&](int blocks) {
    for (int i = 0; i < blocks; ++i) {
      br.process_block(l, r);
      x.insert(x.end(), l, l + 64);
    }
  };
  run(30);
  br.submit({make_note_off(0, 0, kKey), 0});  // key up, pedal keeps it sounding
  run(30);
  const std::size_t second_start = x.size();
  br.submit({make_note_on(0, 0, kKey + 4, 0xFFFF), 0});
  run(40);
  const double want = OrganEngine::kVoiceScale * OrganEngine::kPercNormalGain * 0.5;
  CHECK(amp_at(x, 1920, 1920, drawbar_hz(kKey, kDbThird)) > 0.5 * want);  // the first key's percussion sounds
  CHECK(amp_at(x, second_start + 480, 1920, drawbar_hz(kKey + 4, kDbThird)) < 0.01 * want);  // none on the second
  // Pedal up: everything released; a fresh press triggers again.
  br.submit({make_control_change(0, 0, 64, 0u), 0});
  for (int i = 0; i < 100; ++i) br.process_block(l, r);
  br.submit({make_note_off(0, 0, kKey + 4), 0});
  for (int i = 0; i < 100; ++i) br.process_block(l, r);
  CHECK(organ.active_voices() == 0);
  x.clear();
  br.submit({make_note_on(0, 0, kKey + 7, 0xFFFF), 0});
  run(40);
  CHECK(amp_at(x, 480, 1920, drawbar_hz(kKey + 7, kDbThird)) > 0.5 * want);
}

}  // namespace

int main() {
  test_registry_and_defaults();
  test_single_trigger();
  test_decay_constants();
  test_harmonic_selection();
  test_volume_and_drawbar_lowering();
  test_one_foot_cancelled();
  test_key_click();
  test_determinism();
  test_through_block_loop();
  test_ramps_are_five_ms_ramps();
  test_release_within_onset_burst();
  test_voice_info_level();
  test_part_disable_enable();
  test_click_per_part_determinism();
  test_reclaim_prefers_tail_without_click();
  test_release_click_ratio();
  test_enum_cc_boundaries();
  test_pedal_and_percussion();
  if (g_fail) {
    std::printf("organ_percussion_tests: %d failures\n", g_fail);
    return 1;
  }
  std::printf("organ_percussion_tests: ok\n");
  return 0;
}
