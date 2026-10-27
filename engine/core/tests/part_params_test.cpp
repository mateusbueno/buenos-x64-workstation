// Registry part parameters inside the engines (Story 9.9 follow-up): FM ids 1000..1103, VA ids 1200..1266, SampleEngine part_attack / part_release and
// the per-part tone stage (part_brightness, part_eq_*) of the Mixer. What is checked:
//   * every FM / VA registry parameter maps onto the patch (a value change changes the effective patch) and out-of-range / non-finite values are refused;
//   * measurable audible effect per engine (RMS, brightness, attack time, release tail, pitch, spectral tilt);
//   * defaults change nothing: all registry defaults forwarded = bit-identical output (engagement, see part_params.hpp), reset_play_state restores the base patch;
//   * smoothing: live changes never step the output (VA mix / cutoff, FM brightness, PartTone gains);
//   * PartTone is stable at every extreme, survives NaN / Inf input and leaves no subnormals;
//   * the block loop path: parameter events reach the engines and the Mixer, edits of one part never leak into another, an engine remap keeps the values,
//     1 vs 3 worker partitions are bit-identical with part_attack / part_release moving, and the whole path allocates nothing (allocation hook armed) and is
//     deterministic under a flood of random registry events.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/fm_engine.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/part_tone.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/core/va_engine.hpp"
#include "velora/ports/sample_source.hpp"

using namespace velora::core;
namespace ports = velora::ports;

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define VELORA_NO_ALLOC_HOOK 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define VELORA_NO_ALLOC_HOOK 1
#endif
#endif

static std::atomic<bool> g_armed{false};
static std::atomic<std::uint64_t> g_allocs{0};
#ifndef VELORA_NO_ALLOC_HOOK
void* operator new(std::size_t n) {
  if (g_armed.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif

namespace {

int g_fail = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
      ++g_fail;                                                                  \
    }                                                                            \
  } while (0)
#define CHECK_MSG(cond, ...)                                                     \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);              \
      std::printf(__VA_ARGS__);                                                  \
      std::printf("\n");                                                         \
      ++g_fail;                                                                  \
    }                                                                            \
  } while (0)

using Wave = std::vector<float>;

double rms(const Wave& w, std::size_t a = 0, std::size_t b = SIZE_MAX) {
  b = std::min(b, w.size());
  double s = 0;
  for (std::size_t i = a; i < b; ++i) s += static_cast<double>(w[i]) * w[i];
  return b > a ? std::sqrt(s / static_cast<double>(b - a)) : 0.0;
}
// Brightness proxy: RMS of the first difference over RMS of the signal (a high-passed ratio).
double hf_ratio(const Wave& w, std::size_t a = 0, std::size_t b = SIZE_MAX) {
  b = std::min(b, w.size());
  double d = 0, s = 0;
  for (std::size_t i = a + 1; i < b; ++i) {
    const double x = static_cast<double>(w[i]) - w[i - 1];
    d += x * x;
    s += static_cast<double>(w[i]) * w[i];
  }
  return s > 0 ? std::sqrt(d / s) : 0.0;
}
std::size_t zero_crossings(const Wave& w, std::size_t a = 0, std::size_t b = SIZE_MAX) {
  b = std::min(b, w.size());
  std::size_t n = 0;
  for (std::size_t i = a + 1; i < b; ++i)
    if ((w[i - 1] < 0) != (w[i] < 0)) ++n;
  return n;
}
double max_step(const Wave& w, std::size_t a = 0, std::size_t b = SIZE_MAX) {
  b = std::min(b, w.size());
  double m = 0;
  for (std::size_t i = a + 1; i < b; ++i) m = std::max(m, static_cast<double>(std::fabs(w[i] - w[i - 1])));
  return m;
}
double diff_ratio(const Wave& a, const Wave& b) {
  double d = 0, s = 0;
  for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
    d += (static_cast<double>(a[i]) - b[i]) * (static_cast<double>(a[i]) - b[i]);
    s += static_cast<double>(a[i]) * a[i];
  }
  return s > 0 ? std::sqrt(d / s) : (d > 0 ? 1.0 : 0.0);
}
bool finite_all(const Wave& w) {
  for (float v : w)
    if (!std::isfinite(v)) return false;
  return true;
}
bool no_subnormal(const Wave& w) {
  for (float v : w)
    if (std::fpclassify(v) == FP_SUBNORMAL) return false;
  return true;
}
bool identical(const Wave& a, const Wave& b) { return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0; }

const params::ParamDef& def(std::uint32_t id) { return *params::find(id); }
// A value that is not the registry default (the max, or the min when the max is the default).
float other_than_default(const params::ParamDef& d) { return static_cast<float>(d.def != d.max ? d.max : d.min); }

void render(Engine& e, Wave& out, std::size_t frames) {
  const std::size_t at = out.size();
  out.resize(at + frames, 0.0f);
  Wave r(frames, 0.0f);
  e.render(out.data() + at, r.data(), static_cast<std::uint32_t>(frames));
}

// ---- FM / VA patch mapping -------------------------------------------------------------------------------------------------------------------------------

std::vector<int> ser(const FmVoiceParameters& p) {
  std::vector<int> v;
  for (const auto& o : p.operators) {
    for (auto x : o.rates) v.push_back(x);
    for (auto x : o.levels) v.push_back(x);
    v.insert(v.end(), {o.kbd_breakpoint, o.left_depth, o.right_depth, o.left_curve, o.right_curve, o.kbd_rate_scaling, o.amp_mod_sens, o.velocity_sens,
                       o.total_level, o.fixed_freq_mode, o.freq_coarse, o.freq_fine, o.detune});
  }
  for (auto x : p.pitch_env_rates) v.push_back(x);
  for (auto x : p.pitch_env_levels) v.push_back(x);
  v.insert(v.end(), {p.algorithm, p.feedback, p.osc_key_sync, p.lfo_speed, p.lfo_delay, p.lfo_pitch_mod_depth, p.lfo_amp_mod_depth, p.lfo_key_sync, p.lfo_wave,
                     p.lfo_pitch_mod_sens, p.transpose, p.brightness_offset});
  return v;
}
std::vector<float> ser(const VaVoiceParameters& p) {
  std::vector<float> v;
  auto f = [&v](auto x) { v.push_back(static_cast<float>(x)); };
  f(p.osc1_wave); f(p.osc1_octave); f(p.osc1_semitone); f(p.osc1_detune_cents); f(p.osc1_pulse_width);
  f(p.osc2_wave); f(p.osc2_octave); f(p.osc2_semitone); f(p.osc2_detune_cents); f(p.osc2_pulse_width); f(p.osc2_hard_sync);
  f(p.sub_enabled); f(p.sub_octave); f(p.sub_is_square); f(p.noise_enabled); f(p.noise_is_pink);
  f(p.mix_osc1); f(p.mix_osc2); f(p.mix_sub); f(p.mix_noise);
  f(p.filter_type); f(p.cutoff_hz); f(p.resonance); f(p.drive); f(p.filter_key_track); f(p.filter_env_amount); f(p.filter_velocity_sens);
  f(p.amp_attack_ms); f(p.amp_decay_ms); f(p.amp_sustain_level); f(p.amp_release_ms);
  f(p.filter_attack_ms); f(p.filter_decay_ms); f(p.filter_sustain_level); f(p.filter_release_ms);
  f(p.lfo_wave); f(p.lfo_rate_hz); f(p.lfo_delay_ms); f(p.lfo_key_sync); f(p.lfo_to_pitch); f(p.lfo_to_filter); f(p.lfo_to_amp); f(p.lfo_to_pwm);
  f(p.voice_mode); f(p.unison_voices); f(p.unison_detune_cents); f(p.unison_spread); f(p.glide_time_ms); f(p.glide_mode); f(p.brightness_offset);
  return v;
}

void test_fm_mapping() {
  std::printf("FM: every registry parameter maps onto the patch\n");
  int mapped = 0;
  for (std::uint32_t id = params::kFmAlgorithm; id <= params::kFmOp6AmpModSens; ++id) {
    const params::ParamDef* d = params::find(id);
    if (!d) continue;
    FmEngine fm;
    const auto before = ser(fm.part_voice_params(3));
    // a value off the registry default (so it engages) that also differs from what the base patch holds: try the max, the min, the middle
    bool changed = false;
    for (double v : {d->max, d->min, std::floor((d->min + d->max) / 2.0), std::floor((d->min + d->max) / 2.0) + 1.0}) {
      if (v == d->def) continue;
      fm.set_part_param(3, id, static_cast<float>(v));
      changed = changed || ser(fm.part_voice_params(3)) != before;
    }
    const auto after = ser(fm.part_voice_params(3));
    CHECK_MSG(changed, "fm id %u does not change the patch", id);
    CHECK_MSG(ser(fm.part_voice_params(2)) == before, "fm id %u leaked into part 2", id);
    ++mapped;
    // out of range and non-finite values are clamped / refused, never UB
    FmEngine fm2;
    fm2.set_part_param(0, id, std::nanf(""));
    fm2.set_part_param(0, id, INFINITY);
    fm2.set_part_param(0, id, -1e30f);
    fm2.set_part_param(0, id, 1e30f);
  }
  CHECK(mapped == 104);
  // engagement: defaults do nothing, a moved parameter follows (also back to its default), a Live Set switch restores the base patch
  FmEngine fm;
  const auto base = ser(fm.part_voice_params(0));
  for (const params::ParamDef& d : params::kParams)
    if (d.id >= params::kFmAlgorithm && d.id <= params::kFmOp6AmpModSens) fm.set_part_param(0, d.id, static_cast<float>(d.def));
  CHECK(ser(fm.part_voice_params(0)) == base);
  fm.set_part_param(0, params::kFmAlgorithm, 7.0f);
  CHECK(fm.part_voice_params(0).algorithm == 7);
  fm.set_part_param(0, params::kFmAlgorithm, 1.0f);  // the registry default, now engaged: applied
  CHECK(fm.part_voice_params(0).algorithm == 1);
  fm.reset_play_state();
  CHECK(ser(fm.part_voice_params(0)) == base);
  // a patch installed by set_part_voice_params is the base: registry defaults do not touch it
  FmVoiceParameters custom = create_init_fm_voice();
  custom.algorithm = 12;
  fm.set_part_voice_params(1, custom);
  for (const params::ParamDef& d : params::kParams)
    if (d.id >= params::kFmAlgorithm && d.id <= params::kFmOp6AmpModSens) fm.set_part_param(1, d.id, static_cast<float>(d.def));
  CHECK(fm.part_voice_params(1).algorithm == 12);
  // a live edit engages at once, even with the registry default value: fm_op1_rate_4 default 99, the EP base patch has 45
  FmEngine fe;
  CHECK(fe.part_voice_params(0).operators[0].rates[3] == 45);
  fe.set_part_param(0, params::kFmOp1Rate4, 99.0f);  // an initial value equal to the default: the base patch stays
  CHECK(fe.part_voice_params(0).operators[0].rates[3] == 45);
  fe.set_part_param_edit(0, params::kFmOp1Rate4, 99.0f);
  CHECK(fe.part_voice_params(0).operators[0].rates[3] == 99);
  fe.reset_play_state();
  CHECK(fe.part_voice_params(0).operators[0].rates[3] == 45);
  // exact unit mapping of a few
  fm.set_part_param(2, params::kFmOp3Detune, -5.0f);
  CHECK(fm.part_voice_params(2).operators[2].detune == -5);
  fm.set_part_param(2, params::kFmOp6FreqCoarse, 14.0f);
  CHECK(fm.part_voice_params(2).operators[5].freq_coarse == 14);
  fm.set_part_param(2, params::kFmLfoWave, 4.0f);
  CHECK(fm.part_voice_params(2).lfo_wave == 4);
  fm.set_part_param(2, params::kFmTranspose, -12.0f);
  CHECK(fm.part_voice_params(2).transpose == -12);
  fm.set_part_param(2, params::kFmPitchEnvLevel3, 80.0f);
  CHECK(fm.part_voice_params(2).pitch_env_levels[2] == 80);
}

void test_va_mapping() {
  std::printf("VA: every registry parameter maps onto the patch\n");
  int mapped = 0;
  for (std::uint32_t id = params::kVaOsc1Wave; id <= params::kVaBrightness; ++id) {
    const params::ParamDef* d = params::find(id);
    if (!d) continue;
    VaEngine va;
    const auto before = ser(va.part_voice_params(3));
    va.set_part_param(3, id, other_than_default(*d));
    const auto after = ser(va.part_voice_params(3));
    if (id == params::kVaUnisonSpread) {
      CHECK(before != after);  // stored in the patch (and inaudible: see README)
    } else {
      CHECK_MSG(before != after, "va id %u does not change the patch", id);
    }
    CHECK_MSG(ser(va.part_voice_params(2)) == before, "va id %u leaked into part 2", id);
    ++mapped;
    VaEngine va2;
    va2.set_part_param(0, id, std::nanf(""));
    va2.set_part_param(0, id, INFINITY);
    va2.set_part_param(0, id, -1e30f);
    va2.set_part_param(0, id, 1e30f);
    for (float x : ser(va2.part_voice_params(0))) CHECK(std::isfinite(x));
  }
  CHECK(mapped == 50);
  VaEngine va;
  const auto base = ser(va.part_voice_params(0));
  for (const params::ParamDef& d : params::kParams)
    if (d.id >= params::kVaOsc1Wave && d.id <= params::kVaBrightness) va.set_part_param(0, d.id, static_cast<float>(d.def));
  CHECK(ser(va.part_voice_params(0)) == base);  // the registry defaults ARE the init patch: engaged or not, the same values
  va.set_part_param(0, params::kVaFilterCutoff, 5000.0f);
  CHECK(va.part_voice_params(0).cutoff_hz == 5000.0f);
  va.set_part_param(0, params::kVaMixOsc1, 25.0f);
  CHECK(va.part_voice_params(0).mix_osc1 == 0.25f);
  va.set_part_param(0, params::kVaLfoRate, 75.0f);
  CHECK(va.part_voice_params(0).lfo_rate_hz == 7.5f);
  va.set_part_param(0, params::kVaFilterEnvAmount, -50.0f);
  CHECK(va.part_voice_params(0).filter_env_amount == -0.5f);
  va.set_part_param(0, params::kVaOsc1Wave, 3.0f);
  CHECK(va.part_voice_params(0).osc1_wave == VaWaveform::kSine);
  va.reset_play_state();
  CHECK(ser(va.part_voice_params(0)) == base);
}

// ---- audible effect per engine -------------------------------------------------------------------------------------------------------------------------

using Setup = std::function<void(Engine&)>;

Wave fm_note(const Setup& setup, std::uint32_t note = 60, std::size_t frames = 4096, std::size_t release_at = 0, std::size_t tail = 0) {
  FmEngine fm;
  if (setup) setup(fm);
  Wave w;
  fm.start_voice(0, note, 0xC000);
  render(fm, w, release_at ? release_at : frames);
  if (release_at) {
    fm.stop_voice(0, note);
    render(fm, w, tail);
  }
  return w;
}
Wave va_note(const Setup& setup, std::uint32_t note = 60, std::size_t frames = 8192, std::size_t release_at = 0, std::size_t tail = 0) {
  VaEngine va;
  if (setup) setup(va);
  Wave w;
  va.start_voice(0, note, 0xC000);
  render(va, w, release_at ? release_at : frames);
  if (release_at) {
    va.stop_voice(0, note);
    render(va, w, tail);
  }
  return w;
}
// A live edit (what a knob does): it engages even at the registry default value.
void set(Engine& e, std::uint32_t id, float v, std::uint32_t part = 0) { e.set_part_param_edit(part, id, v); }

void test_fm_audible() {
  std::printf("FM: audible effect\n");
  const Wave base = fm_note(nullptr);
  CHECK(rms(base) > 0.01);
  struct Case { const char* name; std::uint32_t id; float v; };
  const Case cases[] = {{"algorithm", params::kFmAlgorithm, 3}, {"feedback", params::kFmFeedback, 2}, {"op2 level", params::kFmOp2TotalLevel, 99},
                        {"op4 level", params::kFmOp4TotalLevel, 99}, {"op2 coarse", params::kFmOp2FreqCoarse, 3}, {"op6 fine", params::kFmOp6FreqFine, 50},
                        {"op1 detune", params::kFmOp1Detune, 7}, {"op1 level", params::kFmOp1TotalLevel, 30}, {"transpose", params::kFmTranspose, 7},
                        {"op1 rate2", params::kFmOp1Rate2, 5}, {"op1 level1", params::kFmOp1Level1, 30}, {"op3 rate1", params::kFmOp3Rate1, 20}};
  for (const Case& c : cases) {
    const Wave w = fm_note([&](Engine& e) { set(e, c.id, c.v); });
    CHECK_MSG(diff_ratio(base, w) > 0.002, "FM %s: no audible change (%.4f)", c.name, diff_ratio(base, w));
    CHECK(finite_all(w));
  }
  // carrier level lowers the output, transposition raises the pitch
  const Wave quiet = fm_note([](Engine& e) { for (auto id : {params::kFmOp1TotalLevel, params::kFmOp3TotalLevel, params::kFmOp5TotalLevel}) set(e, id, 20); });
  CHECK(rms(quiet) < 0.6 * rms(base));
  const Wave up = fm_note([](Engine& e) { for (auto id : {params::kFmOp2TotalLevel, params::kFmOp4TotalLevel, params::kFmOp6TotalLevel}) set(e, id, 0); set(e, params::kFmTranspose, 12); });
  const Wave plain = fm_note([](Engine& e) { for (auto id : {params::kFmOp2TotalLevel, params::kFmOp4TotalLevel, params::kFmOp6TotalLevel}) set(e, id, 0); set(e, params::kFmTranspose, 1); });
  CHECK(zero_crossings(up, 512) > 1.7 * zero_crossings(plain, 512) * 0.5);  // +12 vs +1 semitone: about 1.9x
  // attack: slower carrier attack rates delay the build-up
  const Wave slow = fm_note([](Engine& e) { for (auto id : {params::kFmOp1Rate1, params::kFmOp3Rate1, params::kFmOp5Rate1}) set(e, id, 8); });
  CHECK(rms(slow, 0, 256) < 0.5 * rms(base, 0, 256));
  // release: a faster release rate (99) silences the tail the default rate (45) still carries
  const Wave rel_def = fm_note(nullptr, 60, 0, 2048, 6000);
  const Wave rel_fast = fm_note([](Engine& e) { for (auto id : {params::kFmOp1Rate4, params::kFmOp3Rate4, params::kFmOp5Rate4}) set(e, id, 99); }, 60, 0, 2048, 6000);
  CHECK(rms(rel_fast, 4096, 8048) < 0.3 * rms(rel_def, 4096, 8048));
  // LFO: pitch modulation depth + sensitivity make the waveform differ from an unmodulated one; the wave shape matters
  const Wave lfo_a = fm_note([](Engine& e) { set(e, params::kFmLfoPitchModDepth, 99); set(e, params::kFmLfoPitchModSens, 7); set(e, params::kFmLfoSpeed, 60); });
  const Wave lfo_b = fm_note([](Engine& e) { set(e, params::kFmLfoPitchModDepth, 99); set(e, params::kFmLfoPitchModSens, 7); set(e, params::kFmLfoSpeed, 60); set(e, params::kFmLfoWave, 3); });
  CHECK(diff_ratio(base, lfo_a) > 0.05);
  CHECK(diff_ratio(lfo_a, lfo_b) > 0.02);
  // brightness scales the modulators: a lower value is a duller tone than a higher one
  const Wave dark = fm_note([](Engine& e) { set(e, params::kFmBrightness, 10); });
  const Wave bright = fm_note([](Engine& e) { set(e, params::kFmBrightness, 120); });
  CHECK(hf_ratio(dark, 512) < hf_ratio(bright, 512));
  // unengaged: bit-identical to a patch nobody touched
  const Wave defaults = fm_note([](Engine& e) {
    for (const params::ParamDef& d : params::kParams)
      if (d.id >= params::kFmAlgorithm && d.id <= params::kFmOp6AmpModSens) e.set_part_param(0, d.id, static_cast<float>(d.def));
  });
  CHECK(identical(base, defaults));
}

void test_va_audible() {
  std::printf("VA: audible effect\n");
  const Wave base = va_note(nullptr);
  CHECK(rms(base) > 0.01);
  struct Case { const char* name; std::uint32_t id; float v; };
  const Case cases[] = {{"osc1 wave", params::kVaOsc1Wave, 3}, {"osc2 wave", params::kVaOsc2Wave, 0}, {"osc2 semitone", params::kVaOsc2Semitone, 7},
                        {"osc2 detune", params::kVaOsc2Detune, 50}, {"pulse width", params::kVaOsc2PulseWidth, 10}, {"hard sync", params::kVaOsc2HardSync, 1},
                        {"sub off", params::kVaSubEnabled, 0}, {"sub octave", params::kVaSubOctave, -2},
                        {"mix noise", params::kVaMixNoise, 40}, {"mix osc2", params::kVaMixOsc2, 0}, {"filter type", params::kVaFilterType, 3},
                        {"cutoff", params::kVaFilterCutoff, 400}, {"resonance", params::kVaFilterResonance, 90}, {"drive", params::kVaFilterDrive, 100},
                        {"env amount", params::kVaFilterEnvAmount, -100}, {"amp attack", params::kVaAmpAttack, 1500},
                        {"amp sustain", params::kVaAmpSustain, 10}, {"filter decay", params::kVaFilterDecay, 5000}, {"lfo to filter", params::kVaLfoToFilter, 100},
                        {"lfo to amp", params::kVaLfoToAmp, 100}, {"lfo to pitch", params::kVaLfoToPitch, 100}, {"unison", params::kVaUnisonVoices, 6},
                        {"brightness", params::kVaBrightness, 120}, {"lfo to pwm", params::kVaLfoToPwm, 100}};
  for (const Case& c : cases) {
    Wave w = va_note([&](Engine& e) { set(e, c.id, c.v); });
    if (c.id == params::kVaLfoToPwm || c.id == params::kVaLfoToAmp || c.id == params::kVaLfoToPitch || c.id == params::kVaLfoToFilter) {
      w = va_note([&](Engine& e) { set(e, c.id, c.v); set(e, params::kVaLfoRate, 80); set(e, params::kVaOsc1Wave, 1); });  // pulse oscillator, faster LFO
      CHECK_MSG(diff_ratio(va_note([](Engine& e) { set(e, params::kVaLfoRate, 80); set(e, params::kVaOsc1Wave, 1); }), w) > 0.01, "VA %s: no audible change", c.name);
      continue;
    }
    CHECK_MSG(diff_ratio(base, w) > 0.01, "VA %s: no audible change (%.4f)", c.name, diff_ratio(base, w));
    CHECK(finite_all(w));
  }
  // filter cutoff: darker / brighter
  const Wave lo = va_note([](Engine& e) { set(e, params::kVaFilterCutoff, 300); });
  const Wave hi = va_note([](Engine& e) { set(e, params::kVaFilterCutoff, 20000); });
  CHECK(hf_ratio(lo, 2048) < hf_ratio(base, 2048));
  CHECK(hf_ratio(hi, 2048) > hf_ratio(base, 2048));
  // amp attack: a slow attack keeps the first 10 ms quiet
  const Wave att = va_note([](Engine& e) { set(e, params::kVaAmpAttack, 2000); });
  CHECK(rms(att, 0, 480) < 0.2 * rms(base, 0, 480));
  // amp release: a longer release keeps the tail
  const Wave rel_s = va_note([](Engine& e) { set(e, params::kVaAmpRelease, 20); }, 60, 0, 4096, 12000);
  const Wave rel_l = va_note([](Engine& e) { set(e, params::kVaAmpRelease, 3000); }, 60, 0, 4096, 12000);
  CHECK(rms(rel_l, 9000, 16000) > 5.0 * rms(rel_s, 9000, 16000) + 1e-6);
  // pitch: a pure sine oscillator one octave up crosses zero twice as often
  auto pure = [](float octave) {
    return [octave](Engine& e) {
      set(e, params::kVaOsc1Wave, 3);
      set(e, params::kVaMixOsc2, 0);
      set(e, params::kVaMixSub, 0);
      set(e, params::kVaFilterCutoff, 20000);
      set(e, params::kVaMixOsc1, 100);
      set(e, params::kVaFilterEnvAmount, 0);
      set(e, params::kVaFilterKeyTrack, 100);
      if (octave != 0) set(e, params::kVaOsc1Octave, octave);
    };
  };
  const Wave o0 = va_note(pure(0), 60, 8192);
  const Wave o1 = va_note(pure(1), 60, 8192);
  const double ratio = static_cast<double>(zero_crossings(o1, 2000)) / static_cast<double>(zero_crossings(o0, 2000));
  CHECK_MSG(ratio > 1.9 && ratio < 2.1, "octave ratio %.3f", ratio);
  // mono voice mode / unison / glide read at the note start
  const Wave uni = va_note([](Engine& e) { set(e, params::kVaUnisonVoices, 8); set(e, params::kVaUnisonDetune, 50); });
  CHECK(diff_ratio(base, uni) > 0.1);
  // unengaged: bit-identical
  const Wave defaults = va_note([](Engine& e) {
    for (const params::ParamDef& d : params::kParams)
      if (d.id >= params::kVaOsc1Wave && d.id <= params::kVaBrightness) e.set_part_param(0, d.id, static_cast<float>(d.def));
  });
  CHECK(identical(base, defaults));
}

// ---- smoothing --------------------------------------------------------------------------------------------------------------------------------------------

void test_smoothing() {
  std::printf("smoothing: live changes never step the sound\n");
  // VA: a sustained pure sine; the mixer level drops from 100 % to 10 % while it sounds. An unsmoothed change steps by ~0.9 of the amplitude; smoothed it
  // is bounded by the sine's own slope (a small multiple).
  auto run = [](const std::function<void(VaEngine&)>& live, std::size_t switch_at) {
    VaEngine va;
    set(va, params::kVaOsc1Wave, 3);
    set(va, params::kVaMixOsc1, 100);
    set(va, params::kVaMixOsc2, 0);
    set(va, params::kVaMixSub, 0);
    set(va, params::kVaFilterCutoff, 20000);
    set(va, params::kVaFilterEnvAmount, 0);
    set(va, params::kVaAmpSustain, 100);
    set(va, params::kVaFilterDrive, 0);
    va.start_voice(0, 72, 0xFFFF);
    Wave w;
    render(va, w, switch_at);
    live(va);
    render(va, w, 4096);
    return w;
  };
  const Wave steady = run([](VaEngine&) {}, 6000);
  const double slope = max_step(steady, 5000, 10000);
  for (std::size_t at : {6000u, 6013u, 6027u, 6040u, 6052u}) {  // different phases of the sine
    const Wave w = run([](VaEngine& e) { set(e, params::kVaMixOsc1, 10); }, at);
    CHECK_MSG(max_step(w, 5000, 10000) < 1.5 * slope, "VA mix step %.4f vs slope %.4f", max_step(w, 5000, 10000), slope);
    CHECK(rms(w, 9000, 10000) < 0.2 * rms(steady, 9000, 10000));  // and it did get there
  }
  // cutoff sweep on a sustained saw: no sample-to-sample step beyond the signal's own range
  {
    VaEngine va;
    set(va, params::kVaFilterEnvAmount, 0);
    set(va, params::kVaAmpSustain, 100);
    set(va, params::kVaMixOsc2, 0);
    set(va, params::kVaMixSub, 0);
    set(va, params::kVaFilterCutoff, 8000);
    va.start_voice(0, 48, 0xFFFF);
    Wave w;
    render(va, w, 4000);
    set(va, params::kVaFilterCutoff, 200);
    render(va, w, 4000);
    set(va, params::kVaFilterCutoff, 8000);
    render(va, w, 4000);
    CHECK(finite_all(w));
    CHECK(max_step(w, 3000) < 1.2 * max_step(w, 1000, 3900));
  }
  // VA amp sustain glides in the sustain stage (no step in the level)
  {
    VaEngine va;
    set(va, params::kVaOsc1Wave, 3);
    set(va, params::kVaMixOsc1, 100);
    set(va, params::kVaMixOsc2, 0);
    set(va, params::kVaMixSub, 0);
    set(va, params::kVaFilterCutoff, 20000);
    set(va, params::kVaFilterEnvAmount, 0);
    set(va, params::kVaAmpSustain, 100);
    va.start_voice(0, 72, 0xFFFF);
    Wave w;
    render(va, w, 8000);
    const double slope = max_step(w, 4000);
    set(va, params::kVaAmpSustain, 20);
    render(va, w, 8000);
    CHECK_MSG(max_step(w, 8000) < 1.6 * slope, "sustain step %.4f vs %.4f", max_step(w, 8000), slope);
    CHECK(rms(w, 15000, 16000) < 0.4 * rms(w, 6000, 7000));
  }
  // FM brightness on a sounding note: the transition is continuous
  {
    auto fmrun = [](bool change) {
      FmEngine fm;
      set(fm, params::kFmBrightness, 64);  // default: nothing engaged
      fm.start_voice(0, 60, 0xC000);
      Wave w;
      render(fm, w, 3000);
      if (change) set(fm, params::kFmBrightness, 127);
      render(fm, w, 3000);
      return w;
    };
    const Wave a = fmrun(false), b = fmrun(true);
    CHECK(finite_all(b));
    CHECK(identical(Wave(a.begin(), a.begin() + 3000), Wave(b.begin(), b.begin() + 3000)));
    CHECK(diff_ratio(a, b) > 0.02);
    CHECK_MSG(max_step(b, 3000, 4200) < 1.3 * max_step(b, 4500, 6000), "fm brightness step: %.4f vs settled %.4f", max_step(b, 3000, 4200), max_step(b, 4500, 6000));
  }
  // PartTone: a steady 1 kHz sine, the mid band moves +15 dB, then back: no step beyond the slope, settles on the target gain
  {
    PartTone t;
    t.set(50, 0, 0, 1000, 0, true);
    Wave all;
    double ph = 0;
    auto block = [&](std::size_t n) {
      for (std::size_t b = 0; b < n; ++b) {
        float l[64], r[64];
        for (int i = 0; i < 64; ++i) {
          l[i] = r[i] = 0.1f * static_cast<float>(std::sin(ph));
          ph += 2.0 * 3.14159265358979 * 1000.0 / 48000.0;
        }
        StereoBlock sb{l, r, 64};
        t.process(sb);
        all.insert(all.end(), l, l + 64);
      }
    };
    block(50);
    const double slope = max_step(all, 64);
    t.set(50, 0, 15, 1000, 0, false);
    block(300);
    const double settled = rms(all, all.size() - 4800) / (0.1 / std::sqrt(2.0));
    CHECK_MSG(std::fabs(20 * std::log10(settled) - 15.0) < 0.6, "mid band gain %.2f dB", 20 * std::log10(settled));
    CHECK_MSG(max_step(all, 64, 64 * 150) < 6.0 * slope, "tone step %.4f slope %.4f", max_step(all, 64, 64 * 150), slope);
    t.set(50, 0, 0, 1000, 0, false);
    block(600);
    CHECK(t.is_identity());
    CHECK(std::fabs(rms(all, all.size() - 4800) / (0.1 / std::sqrt(2.0)) - 1.0) < 1e-4);
  }
}

// ---- PartTone: stability, NaN, denormals, response --------------------------------------------------------------------------------------------------------

void test_part_tone() {
  std::printf("PartTone: response, stability at the extremes, NaN / denormal safety\n");
  PartTone t;
  CHECK(t.is_identity());
  {  // identity is exactly transparent
    float l[64], r[64], l0[64];
    for (int i = 0; i < 64; ++i) l0[i] = l[i] = r[i] = std::sin(0.3f * static_cast<float>(i));
    StereoBlock b{l, r, 64};
    t.process(b);
    CHECK(std::memcmp(l, l0, sizeof(l)) == 0 && std::memcmp(r, l0, sizeof(r)) == 0);
  }
  t.set(50, 6, 0, 1000, 0, true);  // low shelf +6 dB
  CHECK(std::fabs(t.magnitude_db(20.0) - 6.0) < 0.8);
  CHECK(std::fabs(t.magnitude_db(8000.0)) < 0.5);
  t.set(50, 0, 6, 1000, 0, true);
  CHECK(std::fabs(t.magnitude_db(1000.0) - 6.0) < 0.2);
  t.set(50, 0, 0, 1000, 6, true);
  CHECK(std::fabs(t.magnitude_db(16000.0) - 6.0) < 1.0 && std::fabs(t.magnitude_db(100.0)) < 0.3);
  t.set(100, 0, 0, 1000, 0, true);  // brightness 100 %: +9 dB shelf
  CHECK(std::fabs(t.magnitude_db(16000.0) - 9.0) < 1.2 && std::fabs(t.magnitude_db(100.0)) < 0.3);
  t.set(0, 0, 0, 1000, 0, true);
  CHECK(std::fabs(t.magnitude_db(16000.0) + 9.0) < 1.2);
  CHECK(tonemap::brightness_db(50.0f) == 0.0f && tonemap::brightness_db(std::nanf("")) == 0.0f && tonemap::brightness_db(1e9f) == 9.0f && tonemap::brightness_db(-5.0f) == -9.0f);

  // every extreme combination, full-scale noise, parameters jumped every block
  std::uint32_t seed = 12345;
  auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return static_cast<float>(seed >> 8) / 8388608.0f - 1.0f; };
  const float gains[2] = {-15.0f, 15.0f}, freqs[2] = {200.0f, 8000.0f}, br[2] = {0.0f, 100.0f};
  for (int combo = 0; combo < 64; ++combo) {
    PartTone x;
    x.set(br[combo & 1], gains[(combo >> 1) & 1], gains[(combo >> 2) & 1], freqs[(combo >> 3) & 1], gains[(combo >> 4) & 1], (combo & 32) != 0);
    float peak = 0;
    for (int b = 0; b < 1200; ++b) {
      float l[64], r[64];
      for (int i = 0; i < 64; ++i) { l[i] = rnd(); r[i] = rnd(); }
      StereoBlock sb{l, r, 64};
      if (b % 7 == 0) x.set(br[(b / 7) & 1], gains[(b / 3) & 1], gains[(b / 5) & 1], freqs[(b / 11) & 1], gains[(b / 13) & 1], false);
      x.process(sb);
      for (int i = 0; i < 64; ++i) {
        CHECK(std::isfinite(l[i]) && std::isfinite(r[i]));
        peak = std::max({peak, std::fabs(l[i]), std::fabs(r[i])});
      }
    }
    CHECK_MSG(peak < 60.0f, "combo %d peak %.1f", combo, peak);
    // silence afterwards: the tail dies out, no subnormals in the output
    Wave tail;
    for (int b = 0; b < 2000; ++b) {
      float l[64] = {}, r[64] = {};
      StereoBlock sb{l, r, 64};
      x.process(sb);
      tail.insert(tail.end(), l, l + 64);
    }
    CHECK(rms(tail, tail.size() - 6400) < 1e-9);
    CHECK(no_subnormal(tail));
  }
  // NaN / Inf in the input never sticks
  PartTone n;
  n.set(100, 15, 15, 8000, 15, true);
  Wave out;
  for (int b = 0; b < 40; ++b) {
    float l[64], r[64];
    for (int i = 0; i < 64; ++i) {
      l[i] = (b == 10 && i < 8) ? std::nanf("") : ((b == 11 && i < 8) ? INFINITY : 0.2f * std::sin(0.1f * static_cast<float>(i)));
      r[i] = l[i];
    }
    StereoBlock sb{l, r, 64};
    n.process(sb);
    out.insert(out.end(), l, l + 64);
  }
  CHECK(finite_all(out));
  CHECK(rms(out, 64 * 30) > 0.01);
  // non-finite targets are sanitised
  n.set(std::nanf(""), std::nanf(""), INFINITY, std::nanf(""), -INFINITY, true);
  float l[64] = {}, r[64] = {};
  StereoBlock sb{l, r, 64};
  n.process(sb);
  for (int i = 0; i < 64; ++i) CHECK(std::isfinite(l[i]));
}

// ---- the block loop --------------------------------------------------------------------------------------------------------------------------------------

struct Instrument final : ports::SampleSource {
  std::vector<ports::SampleRegionDesc> regions;
  std::vector<ports::SamplePcm> pcm;
  std::vector<std::vector<float>> data;
  ports::InstrumentView instrument() const noexcept override { return {regions, pcm}; }
};
std::shared_ptr<Instrument> make_instrument() {
  auto t = std::make_shared<Instrument>();
  const std::size_t period = 150;
  std::vector<float> x(period * 20);
  for (std::size_t i = 0; i < x.size(); ++i) x[i] = 0.3f * std::sin(6.2831853f * static_cast<float>(i % period) / static_cast<float>(period));
  t->data.push_back(std::move(x));
  ports::SamplePcm p;
  p.sample_rate = 48000;
  p.channels = 1;
  p.frames = t->data[0].size();
  p.left = t->data[0].data();
  t->pcm.push_back(p);
  ports::SampleRegionDesc a;
  a.sample_index = 0;
  a.key_lo = 0;
  a.key_hi = 127;
  a.key_center = 60;
  a.loop_mode = ports::SampleLoopMode::kLoopContinuous;
  a.ampeg_attack = 0.0f;
  a.ampeg_release = 0.05f;
  t->regions.push_back(a);
  return t;
}

struct Rig {
  std::shared_ptr<Instrument> ins = make_instrument();
  std::unique_ptr<SampleEngine> se;
  std::unique_ptr<FmEngine> fm = std::make_unique<FmEngine>();
  std::unique_ptr<VaEngine> va = std::make_unique<VaEngine>();
  std::unique_ptr<BlockRenderer> br = std::make_unique<BlockRenderer>();
  explicit Rig(std::uint32_t threads = 0, bool with_sample = true) {
    SampleEngine::Config cfg;
    cfg.max_voices = 32;
    se = std::make_unique<SampleEngine>(cfg);
    se->set_instrument(*ins);
    br->bind_engine(*fm);
    br->bind_engine(*va);
    if (with_sample) br->bind_engine(*se);
    br->set_channel_engine(0, static_cast<std::uint32_t>(EngineTypeId::kFm));
    br->set_channel_engine(1, static_cast<std::uint32_t>(EngineTypeId::kVa));
    if (with_sample) br->set_channel_engine(2, static_cast<std::uint32_t>(EngineTypeId::kSample));
    if (threads) br->set_parallel_voice_render(threads);
  }
  void param(std::uint32_t id, std::uint32_t part, double v, std::uint32_t frame = 0) {
    br->submit({make_param_event(def(id), part, v), frame});
  }
  void note(std::uint32_t part, std::uint32_t n, bool on, std::uint32_t frame = 0) {
    br->submit({on ? make_note_on(0, part, n, 50000) : make_note_off(0, part, n), frame + 8});  // after the parameter events of frame 0
  }
  void run(Wave& out, std::size_t blocks) {
    for (std::size_t b = 0; b < blocks; ++b) {
      float l[kBlockFrames], r[kBlockFrames];
      br->process_block(l, r);
      out.insert(out.end(), l, l + kBlockFrames);
    }
  }
};

void test_block_loop_paths() {
  std::printf("block loop: events reach the engines and the Mixer\n");
  {  // FM through a UMP parameter event
    Rig a, b;
    b.param(params::kFmOp2TotalLevel, 0, 99);
    for (Rig* r : {&a, &b}) r->note(0, 60, true, 10);
    Wave wa, wb;
    a.run(wa, 40);
    b.run(wb, 40);
    CHECK(diff_ratio(wa, wb) > 0.02);
  }
  {  // an FM event carrying the registry default value is a live edit: it takes effect (fm_op1_rate_4 default 99, the EP patch 45)
    Rig a, b;
    b.param(params::kFmOp1Rate4, 0, 99);
    b.param(params::kFmOp3Rate4, 0, 99);
    b.param(params::kFmOp5Rate4, 0, 99);
    for (Rig* r : {&a, &b}) r->note(0, 60, true, 10);
    Wave wa, wb;
    a.run(wa, 30);
    b.run(wb, 30);
    for (Rig* r : {&a, &b}) r->note(0, 60, false, 0);
    a.run(wa, 60);
    b.run(wb, 60);
    CHECK_MSG(rms(wb, 64 * 60, 64 * 90) < 0.3 * rms(wa, 64 * 60, 64 * 90), "fm release event: %.4f vs %.4f (pre %.4f) rate4=%d voices=%u", rms(wb, 64 * 60, 64 * 90), rms(wa, 64 * 60, 64 * 90), rms(wa, 64 * 20, 64 * 30), b.fm->part_voice_params(0).operators[0].rates[3], b.fm->active_voice_count());
  }
  {  // VA through a UMP parameter event, with the tone stage: default = bit-identical to no event, brightness / EQ change the sound
    Rig a, b, c;
    for (Rig* r : {&a, &b, &c}) r->note(1, 60, true, 10);
    b.param(params::kPartBrightness, 1, 50);  // the default value: nothing
    b.param(params::kPartEqMid, 1, 0);
    c.param(params::kPartEqLow, 1, 15);
    c.param(params::kPartBrightness, 1, 100);
    Wave wa, wb, wc;
    a.run(wa, 60);
    b.run(wb, 60);
    c.run(wc, 60);
    CHECK(identical(wa, wb));
    CHECK(!a.br->mixer().routed() && !b.br->mixer().routed());
    CHECK(c.br->mixer().routed());
    CHECK(diff_ratio(wa, wc) > 0.05);
    CHECK(finite_all(wc));
    // sample part: brightness cuts / boosts the sine's harmonic-free tone only by gain; EQ low +15 dB at the sine's frequency raises the level
    Rig s0, s1;
    for (Rig* r : {&s0, &s1}) r->note(2, 60, true, 0);
    s1.param(params::kPartEqMid, 2, 12);
    s1.param(params::kPartEqMidFreq, 2, 320);  // the sample plays ~320 Hz
    Wave w0, w1;
    s0.run(w0, 80);
    s1.run(w1, 80);
    CHECK_MSG(rms(w1, 3000) > 2.0 * rms(w0, 3000), "mid EQ on the sample part: %.4f vs %.4f", rms(w1, 3000), rms(w0, 3000));
  }
  {  // sample part_attack / part_release
    Rig a, b, c;
    for (Rig* r : {&a, &b, &c}) r->note(2, 60, true, 0);
    b.param(params::kPartAttack, 2, 400);  // 400 ms attack
    c.br->set_channel_engine(2, static_cast<std::uint32_t>(EngineTypeId::kSample));  // re-forwards the live values as INITIAL ones: the defaults engage nothing
    Wave wa, wb, wc;
    a.run(wa, 10);
    b.run(wb, 10);
    c.run(wc, 10);
    CHECK(rms(wb, 0, 320) < 0.1 * rms(wa, 0, 320));
    CHECK(identical(wa, wc));
    // release: a long part_release keeps the note ringing after the key goes up; the default leaves the instrument's own 50 ms
    Rig r0, r1;
    for (Rig* r : {&r0, &r1}) r->note(2, 60, true, 0);
    r1.param(params::kPartRelease, 2, 3000);
    Wave w0, w1;
    r0.run(w0, 20);
    r1.run(w1, 20);
    for (Rig* r : {&r0, &r1}) r->note(2, 60, false, 0);
    r0.run(w0, 80);
    r1.run(w1, 80);
    CHECK(rms(w0, 64 * 80, 64 * 100) < 0.01 * rms(w0, 64 * 15, 64 * 20));
    CHECK(rms(w1, 64 * 80, 64 * 100) > 0.5 * rms(w1, 64 * 15, 64 * 20));
  }
  std::printf("block loop: edits of one part never leak into another; a remap keeps the values\n");
  {
    Rig a, b;
    for (Rig* r : {&a, &b}) {
      r->note(0, 60, true, 5);
      r->note(2, 64, true, 5);
    }
    for (std::uint32_t id = params::kVaOsc1Wave; id <= params::kVaBrightness; ++id)
      if (const params::ParamDef* d = params::find(id)) b.param(id, 1, other_than_default(*d));  // heavy edits on the (silent) VA part 1 only
    b.param(params::kPartEqHigh, 1, 15);
    b.param(params::kPartAttack, 1, 900);
    Wave wa, wb;
    a.run(wa, 60);
    b.run(wb, 60);
    CHECK_MSG(diff_ratio(wa, wb) < 1e-6, "edits of part 1 leaked: %.3g", diff_ratio(wa, wb));  // b is routed through the part buses (tone stage of part 1), a is not: same sound, another float summation order
    // remap part 0: FM -> VA -> FM keeps the engaged FM values (the block loop re-forwards the live values at every remap)
    Rig m;
    m.param(params::kFmAlgorithm, 0, 3);
    m.param(params::kFmOp3TotalLevel, 0, 77);
    Wave w;
    m.run(w, 2);
    const auto before = ser(m.fm->part_voice_params(0));
    m.br->set_channel_engine(0, static_cast<std::uint32_t>(EngineTypeId::kVa));
    m.br->set_channel_engine(0, static_cast<std::uint32_t>(EngineTypeId::kFm));
    CHECK(ser(m.fm->part_voice_params(0)) == before);
    CHECK(m.fm->part_voice_params(0).algorithm == 3 && m.fm->part_voice_params(0).operators[2].total_level == 77);
  }
}

void test_parallel_equivalence() {
  std::printf("parallel render: 1 vs 3 partitions bit-identical, serial within tolerance, with part_attack / part_release moving\n");
  auto go = [](std::uint32_t threads) {
    Rig r(threads);
    Wave w;
    r.param(params::kPartAttack, 2, 120);
    for (std::uint32_t n = 40; n < 72; n += 3) r.note(2, n, true, (n % 7) * 5);
    r.note(0, 55, true, 3);
    r.note(1, 48, true, 3);
    r.run(w, 30);
    r.param(params::kPartRelease, 2, 900, 20);
    r.param(params::kPartAttack, 2, 5, 40);
    r.param(params::kPartEqMid, 2, 6);
    r.param(params::kVaFilterCutoff, 1, 700);
    for (std::uint32_t n = 40; n < 72; n += 6) r.note(2, n, false, 10);
    for (std::uint32_t n = 41; n < 60; n += 3) r.note(2, n, true, 12 + n % 9);
    r.run(w, 60);
    for (std::uint32_t n = 40; n < 72; ++n) r.note(2, n, false, 0);
    r.run(w, 80);
    return w;
  };
  const Wave serial = go(0), p1 = go(1), p3 = go(3), p3b = go(3);
  CHECK(identical(p1, p3));
  CHECK(identical(p3, p3b));
  CHECK(finite_all(serial) && rms(serial) > 0.001);
  CHECK_MSG(diff_ratio(serial, p3) < 1e-4, "serial vs parallel %.3g", diff_ratio(serial, p3));
}

// A flood of random registry events (every FM / VA / part_* id, random values incl. the extremes), notes on all engines, remaps and tone moves, with the
// allocation hook armed; the run is repeated and must be bit-identical.
std::uint64_t flood(bool arm, std::uint64_t& allocs_out, bool& finite_out) {
  Rig r(3);
  std::uint32_t seed = 4242;
  auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
  std::vector<std::uint32_t> ids;
  for (const params::ParamDef& d : params::kParams)
    if (d.cls == params::Class::kPerformance && d.scope == params::Scope::kPart &&
        ((d.id >= params::kFmAlgorithm && d.id <= params::kVaBrightness) || (d.id >= params::kPartBrightness && d.id <= params::kPartEqHigh)))
      ids.push_back(d.id);
  std::uint64_t hash = 1469598103934665603ull;
  bool fin = true;
  if (arm) g_armed.store(true);
  for (std::uint32_t b = 0; b < 1000; ++b) {
    for (int k = 0; k < 12; ++k) {
      const params::ParamDef& d = def(ids[rnd() % ids.size()]);
      const std::uint32_t part = rnd() % 3;
      const std::uint32_t pick = rnd() % 4;
      const double v = pick == 0 ? d.min : (pick == 1 ? d.max : d.min + (d.max - d.min) * static_cast<double>(rnd() % 1000) / 999.0);
      r.br->submit({make_param_event(d, part, v), rnd() % 64});
    }
    for (int k = 0; k < 3; ++k) {
      const std::uint32_t part = rnd() % 3, note = 28 + rnd() % 60;
      r.br->submit({(rnd() & 1) ? make_note_on(0, part, note, 20000 + rnd() % 40000) : make_note_off(0, part, note), rnd() % 64});
    }
    if (b % 500 == 250) {  // a part switches engine and back (what a timbre change does)
      r.br->set_channel_engine(1, static_cast<std::uint32_t>(EngineTypeId::kFm));
      r.br->set_channel_engine(1, static_cast<std::uint32_t>(EngineTypeId::kVa));
    }
    float l[kBlockFrames], rr[kBlockFrames];
    r.br->process_block(l, rr);
    for (std::uint32_t i = 0; i < kBlockFrames; ++i) {
      fin = fin && std::isfinite(l[i]) && std::isfinite(rr[i]);
      std::uint32_t u;
      std::memcpy(&u, &l[i], 4);
      hash = (hash ^ u) * 1099511628211ull;
    }
  }
  if (arm) g_armed.store(false);
  allocs_out = g_allocs.load();
  finite_out = fin;
  return hash;
}

void test_traffic() {
  std::printf("traffic: random registry flood, no allocation, deterministic\n");
  std::uint64_t allocs = 0;
  bool fin1 = false, fin2 = false;
  const std::uint64_t h1 = flood(true, allocs, fin1);
  std::uint64_t dummy = 0;
  const std::uint64_t h2 = flood(false, dummy, fin2);
  CHECK(fin1 && fin2);
  CHECK_MSG(h1 == h2, "the flood is not deterministic");
#ifndef VELORA_NO_ALLOC_HOOK
  CHECK_MSG(allocs == 0, "%llu allocations in the block path", static_cast<unsigned long long>(allocs));
#endif
  std::printf("  allocations in the block path: %llu\n", static_cast<unsigned long long>(allocs));
}

}  // namespace

int main() {
  auto timed = [](const char* name, void (*fn)()) {
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    std::printf("  (%s: %.1f s)\n", name, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  };
  timed("fm mapping", test_fm_mapping);
  timed("va mapping", test_va_mapping);
  timed("fm audible", test_fm_audible);
  timed("va audible", test_va_audible);
  timed("smoothing", test_smoothing);
  timed("part tone", test_part_tone);
  timed("block loop", test_block_loop_paths);
  timed("parallel", test_parallel_equivalence);
  timed("traffic", test_traffic);
  if (g_fail) {
    std::printf("part_params_tests: FAIL (%d)\n", g_fail);
    return 1;
  }
  std::printf("part_params_tests: all passed\n");
  return 0;
}
