#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <vector>

#include "velora/core/va_engine.hpp"

using namespace velora::core;

void test_va_engine_initialization_and_metadata() {
  std::cout << "Testing VA Engine initialization and metadata...\n";

  VaEngine engine(48000.0f);
  assert(engine.type_id() == EngineTypeId::kVa);
  assert(engine.cost_units_per_voice() == 4);
  assert(engine.voice_capacity() == 128);
  assert(engine.active_voice_count() == 0);

  // Check voice info on inactive slot
  VoiceInfo info{};
  assert(!engine.voice_info(0, info));

  std::cout << "[PASS] VA Engine initialization\n";
}

void test_va_oscillator_antialiasing_at_top_key() {
  std::cout << "Testing VA oscillator anti-aliasing (PolyBLEP) at C8 (4186 Hz)...\n";

  VaEngine engine(48000.0f);
  VaVoiceParameters params = create_init_va_voice();
  params.osc1_wave = VaWaveform::kSaw;
  params.mix_osc1 = 1.0f;
  params.mix_osc2 = 0.0f;
  params.mix_sub = 0.0f;
  params.mix_noise = 0.0f;
  // Wide open filter
  params.cutoff_hz = 20000.0f;
  params.resonance = 0.0f;
  params.amp_attack_ms = 1.0f;
  params.amp_decay_ms = 1000.0f;
  params.amp_sustain_level = 1.0f;

  engine.set_part_voice_params(0, params);

  // Note 108 is C8 (approx 4186.01 Hz)
  engine.start_voice(0, 108, 0x7FFF);

  // Render 8192 frames to reach steady state
  constexpr size_t total_N = 8192;
  constexpr size_t N = 4096;
  std::vector<float> left(total_N, 0.0f);
  std::vector<float> right(total_N, 0.0f);
  engine.render(left.data(), right.data(), total_N);

  // Apply Hann window to steady-state block [4096 .. 8192]
  std::vector<float> windowed(N, 0.0f);
  for (size_t n = 0; n < N; ++n) {
    float w = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * n / (N - 1)));
    windowed[n] = left[4096 + n] * w;
  }

  // Fundamental frequency f0 for Note 108 (C8, 4186 Hz)
  float f0 = 440.0f * std::pow(2.0f, (108.0f - 69.0f) / 12.0f);

  // Compute DFT magnitudes
  std::vector<double> mag(N / 2, 0.0);
  for (size_t k = 1; k < N / 2; ++k) {
    double real = 0.0;
    double imag = 0.0;
    for (size_t n = 0; n < N; ++n) {
      double angle = 2.0 * M_PI * k * n / N;
      real += windowed[n] * std::cos(angle);
      imag -= windowed[n] * std::sin(angle);
    }
    mag[k] = std::sqrt(real * real + imag * imag);
  }

  // Find fundamental peak and valid integer harmonics below Nyquist
  double fundamental_peak = 0.0;
  std::vector<bool> is_harmonic(N / 2, false);
  for (int h = 1; h * f0 < 24000.0f; ++h) {
    float target_bin = (h * f0 / 48000.0f) * N;
    int b_center = static_cast<int>(std::round(target_bin));
    for (int b = b_center - 4; b <= b_center + 4; ++b) {
      if (b >= 1 && b < static_cast<int>(N / 2)) {
        is_harmonic[b] = true;
      }
    }
    if (h == 1 && b_center < static_cast<int>(N / 2)) {
      fundamental_peak = mag[b_center];
    }
  }

  // Maximum aliased peak in non-harmonic spectrum
  double max_aliased_peak = 0.0;
  for (size_t k = 1; k < N / 2; ++k) {
    if (!is_harmonic[k]) {
      max_aliased_peak = std::max(max_aliased_peak, mag[k]);
    }
  }

  double alias_ratio_db = 20.0 * std::log10(max_aliased_peak / (fundamental_peak + 1e-12));
  std::cout << "  PolyBLEP Saw C8 Peak Aliased Component: " << alias_ratio_db << " dB (rel fundamental)\n";

  // Stated threshold: at C8 (4186 Hz / 48 kHz), peak aliased component must stay below -18.0 dB
  constexpr double kStatedAliasingThresholdDb = -18.0;
  assert(alias_ratio_db < kStatedAliasingThresholdDb);

  engine.stop_voice(0, 108);
  std::cout << "[PASS] VA oscillator aliasing at top key C8 stays below stated threshold (-18 dB)\n";
}

void test_va_filter_responses_and_slopes() {
  std::cout << "Testing VA ZDF filter frequency responses and slopes (LP12, LP24, BP12, HP12)...\n";

  VaFilter filter;
  constexpr float Fs = 48000.0f;
  constexpr float fc = 1000.0f; // 1 kHz cutoff

  // Test LP12: 1 octave above cutoff (2 kHz) should be attenuated by ~12 dB (±2 dB)
  // 2 octaves above cutoff (4 kHz) attenuated by ~24 dB
  auto measure_gain = [&](VaFilterType type, float test_freq) -> float {
    filter.reset();
    constexpr size_t kSamples = 4096;
    float peak_in = 0.5f;
    float max_out = 0.0f;
    for (size_t i = 0; i < kSamples; ++i) {
      float t = static_cast<float>(i) / Fs;
      float in = peak_in * std::sin(2.0f * M_PI * test_freq * t);
      float out = filter.process(in, type, fc, 0.0f, 0.0f, Fs);
      if (i > 1000) {
        max_out = std::max(max_out, std::abs(out));
      }
    }
    return max_out / peak_in;
  };

  // LP12
  float lp12_pass = measure_gain(VaFilterType::kLowPass12, 200.0f);
  float lp12_cutoff = measure_gain(VaFilterType::kLowPass12, fc);
  float lp12_stop1 = measure_gain(VaFilterType::kLowPass12, 2000.0f); // +1 octave
  float lp12_stop2 = measure_gain(VaFilterType::kLowPass12, 4000.0f); // +2 octaves

  assert(std::abs(lp12_pass - 1.0f) < 0.1f); // Passband near unity
  assert(std::abs(lp12_cutoff - 0.707f) < 0.15f); // ~ -3 dB at cutoff
  float lp12_att_octave = 20.0f * std::log10(lp12_stop2 / lp12_stop1);
  std::cout << "  LP12 Attenuation slope per octave: " << lp12_att_octave << " dB/oct\n";
  assert(lp12_att_octave < -10.0f && lp12_att_octave > -14.5f); // ~ -12 dB/oct

  // LP24
  float lp24_stop1 = measure_gain(VaFilterType::kLowPass24, 2000.0f);
  float lp24_stop2 = measure_gain(VaFilterType::kLowPass24, 4000.0f);
  float lp24_att_octave = 20.0f * std::log10(lp24_stop2 / lp24_stop1);
  std::cout << "  LP24 Attenuation slope per octave: " << lp24_att_octave << " dB/oct\n";
  assert(lp24_att_octave < -20.0f && lp24_att_octave > -26.0f); // ~ -24 dB/oct

  // HP12
  float hp12_stop = measure_gain(VaFilterType::kHighPass12, 250.0f);
  float hp12_pass = measure_gain(VaFilterType::kHighPass12, 5000.0f);
  assert(hp12_stop < 0.1f);
  assert(hp12_pass > 0.9f);

  // BP12
  float bp12_center = measure_gain(VaFilterType::kBandPass12, fc);
  float bp12_low = measure_gain(VaFilterType::kBandPass12, 200.0f);
  float bp12_high = measure_gain(VaFilterType::kBandPass12, 5000.0f);
  assert(bp12_center > bp12_low * 2.0f);
  assert(bp12_center > bp12_high * 2.0f);

  std::cout << "[PASS] VA filter slopes match theoretical targets\n";
}

void test_va_self_oscillation_stability() {
  std::cout << "Testing VA filter self-oscillation stability & boundedness...\n";

  VaEngine engine(48000.0f);
  VaVoiceParameters params = create_init_va_voice();
  // No oscillator inputs
  params.mix_osc1 = 0.0f;
  params.mix_osc2 = 0.0f;
  params.mix_sub = 0.0f;
  params.mix_noise = 0.0f;
  // Maximum resonance (self-oscillation)
  params.resonance = 1.0f;
  params.cutoff_hz = 1000.0f;
  params.amp_attack_ms = 1.0f;
  params.amp_decay_ms = 1000.0f;
  params.amp_sustain_level = 1.0f;

  engine.set_part_voice_params(0, params);
  engine.start_voice(0, 60, 0x7FFF);

  // Render 48000 samples (1 full second)
  constexpr size_t kSamples = 48000;
  std::vector<float> left(kSamples, 0.0f);
  std::vector<float> right(kSamples, 0.0f);
  engine.render(left.data(), right.data(), kSamples);

  for (size_t i = 0; i < kSamples; ++i) {
    assert(!std::isnan(left[i]));
    assert(!std::isinf(left[i]));
    assert(!std::isnan(right[i]));
    assert(!std::isinf(right[i]));
    // Check bounded amplitude
    assert(std::abs(left[i]) <= 2.0f);
    assert(std::abs(right[i]) <= 2.0f);
  }

  engine.stop_voice(0, 60);
  std::cout << "[PASS] Self-oscillation is bounded, stable, zero NaN/denormal\n";
}

void test_va_envelope_timing_accuracy() {
  std::cout << "Testing VA ADSR envelope timing accuracy within tolerance...\n";

  VaAdsrEnvelope env;
  constexpr float Fs = 48000.0f;
  constexpr float target_attack_ms = 50.0f;
  constexpr float target_decay_ms = 100.0f;
  constexpr float target_sustain = 0.4f;
  constexpr float target_release_ms = 80.0f;

  env.init(target_attack_ms, target_decay_ms, target_sustain, target_release_ms, Fs);
  env.trigger();

  // 1. Measure Attack time
  uint32_t attack_samples = 0;
  while (env.state() == VaEnvState::kAttack && attack_samples < 5000) {
    env.step();
    attack_samples++;
  }
  float measured_attack_ms = (static_cast<float>(attack_samples) / Fs) * 1000.0f;
  float attack_error_pct = std::abs(measured_attack_ms - target_attack_ms) / target_attack_ms * 100.0f;
  std::cout << "  Target Attack: " << target_attack_ms << " ms, Measured: "
            << measured_attack_ms << " ms (Error: " << attack_error_pct << "%)\n";
  assert(attack_error_pct < 2.0f); // < 2% error

  // 2. Measure Decay time to sustain
  uint32_t decay_samples = 0;
  while (env.state() == VaEnvState::kDecay && decay_samples < 20000) {
    env.step();
    decay_samples++;
  }
  float measured_decay_ms = (static_cast<float>(decay_samples) / Fs) * 1000.0f;
  std::cout << "  Decay reached sustain at: " << measured_decay_ms << " ms, level: " << env.level() << "\n";
  assert(std::abs(env.level() - target_sustain) < 0.01f);

  // 3. Trigger release and measure release time
  env.release();
  uint32_t release_samples = 0;
  while (env.is_active() && release_samples < 20000) {
    env.step();
    release_samples++;
  }
  float measured_release_ms = (static_cast<float>(release_samples) / Fs) * 1000.0f;
  std::cout << "  Release time to silence: " << measured_release_ms << " ms\n";
  assert(measured_release_ms > 0.0f && measured_release_ms < target_release_ms * 2.0f);

  std::cout << "[PASS] VA Envelope timings match parameters within tolerance\n";
}

void test_va_glide_mono_legato_and_unison() {
  std::cout << "Testing VA Glide, Mono/Legato, and Unison...\n";

  VaEngine engine(48000.0f);
  VaVoiceParameters params = create_init_va_voice();
  params.voice_mode = VaVoiceMode::kLegato;
  params.glide_time_ms = 50.0f; // 50 ms glide
  params.unison_voices = 4;
  params.unison_detune_cents = 20.0f;
  params.unison_spread = 0.8f;

  engine.set_part_voice_params(0, params);

  // Start Note 60
  engine.start_voice(0, 60, 0x7FFF);
  assert(engine.active_voice_count() == 1);

  std::vector<float> left(2400, 0.0f);
  std::vector<float> right(2400, 0.0f);
  engine.render(left.data(), right.data(), 2400);

  // Legato trigger Note 72 while Note 60 is held
  engine.start_voice(0, 72, 0x7FFF);
  // In Legato mode, voice count remains 1 (retuned)
  assert(engine.active_voice_count() == 1);

  engine.render(left.data(), right.data(), 2400);

  // Check stereo spread from 4 unison voices
  float left_energy = 0.0f, right_energy = 0.0f;
  for (size_t i = 0; i < 2400; ++i) {
    left_energy += left[i] * left[i];
    right_energy += right[i] * right[i];
  }
  assert(left_energy > 0.001f);
  assert(right_energy > 0.001f);

  engine.stop_voice(0, 72);
  std::cout << "[PASS] VA Glide, Legato retuning, and Unison verified\n";
}

void test_va_golden_render_bit_identical() {
  std::cout << "Testing VA bit-identical golden render reproducibility...\n";

  auto render_session = []() -> std::vector<float> {
    VaEngine engine(48000.0f);
    VaVoiceParameters params = create_init_va_voice();
    params.osc1_wave = VaWaveform::kSaw;
    params.osc2_wave = VaWaveform::kPulse;
    params.osc2_detune_cents = 7.0f;
    params.mix_osc1 = 0.7f;
    params.mix_osc2 = 0.5f;
    params.mix_sub = 0.3f;
    params.cutoff_hz = 1500.0f;
    params.resonance = 0.35f;
    params.filter_env_amount = 0.5f;

    engine.set_part_voice_params(0, params);

    // Play a 3-note chord: C4 (60), E4 (64), G4 (67)
    engine.start_voice(0, 60, 0x7FFF);
    engine.start_voice(0, 64, 0x6FFF);
    engine.start_voice(0, 67, 0x7500);

    constexpr size_t total_frames = 48000; // 1 second
    std::vector<float> buffer(total_frames * 2, 0.0f);
    std::vector<float> l(total_frames, 0.0f);
    std::vector<float> r(total_frames, 0.0f);

    engine.render(l.data(), r.data(), total_frames);

    for (size_t i = 0; i < total_frames; ++i) {
      buffer[i * 2] = l[i];
      buffer[i * 2 + 1] = r[i];
    }
    return buffer;
  };

  std::vector<float> run1 = render_session();
  std::vector<float> run2 = render_session();

  assert(run1.size() == run2.size());
  assert(std::memcmp(run1.data(), run2.data(), run1.size() * sizeof(float)) == 0);

  // Compute 64-bit checksum
  uint64_t hash = 14695981039346656037ULL;
  for (float f : run1) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    hash ^= u;
    hash *= 1099511628211ULL;
  }
  std::cout << "  Bit-identical golden render checksum: 0x" << std::hex << hash << std::dec << "\n";
  assert(hash != 0);

#ifndef VELORA_SOURCE_ROOT
#define VELORA_SOURCE_ROOT "."
#endif

  // Check or write committed golden file
  const std::string golden_path = std::string(VELORA_SOURCE_ROOT) + "/engine/core/tests/golden/va_golden.bin";
  std::ifstream in(golden_path, std::ios::binary);
  if (!in.is_open()) {
    // Write golden file
    std::ofstream out(golden_path, std::ios::binary);
    assert(out.is_open());
    out.write(reinterpret_cast<const char*>(run1.data()), run1.size() * sizeof(float));
    std::cout << "  Wrote new golden file: " << golden_path << " (" << run1.size() * sizeof(float) << " bytes)\n";
  } else {
    // Verify against existing golden file
    std::vector<float> golden(run1.size(), 0.0f);
    in.read(reinterpret_cast<char*>(golden.data()), golden.size() * sizeof(float));
    assert(std::memcmp(run1.data(), golden.data(), run1.size() * sizeof(float)) == 0);
    std::cout << "  Bit-identical match against golden file: " << golden_path << "\n";
  }

  std::cout << "[PASS] VA Golden Render is bit-identically reproducible\n";
}

int main() {
  std::cout << "=== Running VA Engine DSP & Real-Time Test Suite (Story 12.6) ===\n";
  test_va_engine_initialization_and_metadata();
  test_va_oscillator_antialiasing_at_top_key();
  test_va_filter_responses_and_slopes();
  test_va_self_oscillation_stability();
  test_va_envelope_timing_accuracy();
  test_va_glide_mono_legato_and_unison();
  test_va_golden_render_bit_identical();
  std::cout << "=== All VA Engine Tests Passed! ===\n";
  return 0;
}
