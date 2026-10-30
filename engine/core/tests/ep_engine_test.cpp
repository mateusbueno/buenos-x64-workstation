#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#include "velora/core/ep_engine.hpp"

using namespace velora::core;

void test_ep_engine_initialization_and_metadata() {
  std::cout << "Testing EP Engine initialization and metadata...\n";

  EpEngine engine(48000.0f);
  assert(engine.type_id() == EngineTypeId::kModeledEP);
  assert(static_cast<uint32_t>(engine.type_id()) == 5);
  assert(engine.cost_units_per_voice() == 4);
  assert(engine.voice_capacity() == 64);
  assert(engine.active_voice_count() == 0);

  // Check voice info on inactive slot
  VoiceInfo info{};
  assert(!engine.voice_info(0, info));

  std::cout << "[PASS] EP Engine initialization\n";
}

void test_ep_tuning_sweep_e1_to_e7() {
  std::cout << "Testing EP tuning across E1 (28) to E7 (100)...\n";

  EpEngine engine(48000.0f);
  const float sample_rate = 48000.0f;

  // Selected note checkpoints across all 6 octaves from E1 to E7
  const uint32_t test_notes[] = {28, 40, 52, 64, 69, 76, 88, 100};

  for (uint32_t note : test_notes) {
    float expected_freq = 440.0f * std::pow(2.0f, (static_cast<float>(note) - 69.0f) / 12.0f);

    engine.reset_play_state();
    EngineStatus st = engine.start_voice(0, note, 0x8000);  // velocity 64
    assert(st == EngineStatus::kOk);
    assert(engine.active_voice_count() == 1);

    // Warm up slightly past the initial hammer transient (50 ms)
    const uint32_t warmup_frames = static_cast<uint32_t>(sample_rate * 0.05f);
    std::vector<float> warmup_l(warmup_frames, 0.0f);
    std::vector<float> warmup_r(warmup_frames, 0.0f);
    engine.render(warmup_l.data(), warmup_r.data(), warmup_frames);

    // Capture 0.5s of sustained tone
    const uint32_t record_frames = static_cast<uint32_t>(sample_rate * 0.5f);
    std::vector<float> rec_l(record_frames, 0.0f);
    std::vector<float> rec_r(record_frames, 0.0f);
    engine.render(rec_l.data(), rec_r.data(), record_frames);

    // Count positive-going zero crossings with refractory period to reject harmonic ripple
    int zero_crossings = 0;
    int first_crossing = -1;
    int last_crossing = -1;
    const int min_interval = static_cast<int>(0.6f * (sample_rate / expected_freq));

    for (uint32_t i = 1; i < record_frames; ++i) {
      if (rec_l[i - 1] <= 0.0f && rec_l[i] > 0.0f) {
        if (last_crossing >= 0 && (static_cast<int>(i) - last_crossing) < min_interval) {
          continue;  // Skip harmonic ripple
        }
        zero_crossings++;
        if (first_crossing < 0) first_crossing = static_cast<int>(i);
        last_crossing = static_cast<int>(i);
      }
    }

    assert(zero_crossings > 10);
    assert(last_crossing > first_crossing);

    float measured_period = static_cast<float>(last_crossing - first_crossing) / static_cast<float>(zero_crossings - 1);
    float measured_freq = sample_rate / measured_period;

    // Tolerance check: within 0.1 semitone (10 cents) for zero crossings with overtones
    float cents_error = std::abs(1200.0f * std::log2(measured_freq / expected_freq));
    if (cents_error > 8.0f) {
      std::cerr << "Note " << note << " expected: " << expected_freq << " Hz, measured: "
                << measured_freq << " Hz, cents error: " << cents_error << "\n";
    }
    assert(cents_error <= 8.0f);

    engine.stop_voice(0, note);
  }

  std::cout << "[PASS] Tuning across keys E1 to E7 verified within tolerance\n";
}

void test_ep_rt_safety_and_render_blocks() {
  std::cout << "Testing RT-safety and block rendering...\n";

  EpEngine engine(48000.0f);
  engine.start_voice(0, 60, 0x8000);

  std::vector<float> left(64, 0.0f);
  std::vector<float> right(64, 0.0f);

  // Render 500 blocks of 64 frames
  for (int i = 0; i < 500; ++i) {
    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    engine.render(left.data(), right.data(), 64);

    for (int j = 0; j < 64; ++j) {
      assert(!std::isnan(left[j]));
      assert(!std::isinf(left[j]));
      assert(!std::isnan(right[j]));
      assert(!std::isinf(right[j]));
      assert(std::abs(left[j]) <= 2.5f);
      assert(std::abs(right[j]) <= 2.5f);
    }
  }

  VoiceInfo info{};
  assert(engine.voice_info(0, info));
  assert(info.channel == 0);
  assert(info.state == VoiceState::kHeld);
  assert(info.note == 60);

  std::cout << "[PASS] RT-safety block rendering verified\n";
}

void test_ep_voice_stealing_and_fade() {
  std::cout << "Testing voice stealing and fade...\n";

  EpEngine engine(48000.0f, 4);  // Small capacity of 4 voices

  for (uint32_t n = 60; n < 64; ++n) {
    assert(engine.start_voice(0, n, 0x8000) == EngineStatus::kOk);
  }
  assert(engine.active_voice_count() == 4);

  // Test begin_steal_fade
  assert(engine.begin_steal_fade(0, 32));
  // Already fading -> subsequent call returns false
  assert(!engine.begin_steal_fade(0, 32));

  std::vector<float> left(64, 0.0f);
  std::vector<float> right(64, 0.0f);
  engine.render(left.data(), right.data(), 64);

  // 5th note triggers automatic slot stealing
  assert(engine.start_voice(0, 64, 0x8000) == EngineStatus::kOk);
  assert(engine.active_voice_count() <= 4);

  std::cout << "[PASS] Voice stealing and fade verified\n";
}

void test_ep_aliasing_at_top_key_and_velocity() {
  std::cout << "Testing aliasing suppression at top key (E7, note 100) and top velocity (127)...\n";

  EpEngine engine(48000.0f);
  engine.start_voice(0, 100, 0xFFFF);  // Note 100 @ velocity 127

  const uint32_t frames = 2048;
  std::vector<float> left(frames, 0.0f);
  std::vector<float> right(frames, 0.0f);
  engine.render(left.data(), right.data(), frames);

  // Measure peak audio signal
  float peak = 0.0f;
  for (uint32_t i = 256; i < frames; ++i) {
    peak = std::max(peak, std::abs(left[i]));
  }
  assert(peak > 0.001f);

  // Aliasing test: verify signal has clean bounded dynamics without divergent high-frequency energy
  float high_diff_energy = 0.0f;
  for (uint32_t i = 257; i < frames; ++i) {
    float diff = left[i] - left[i - 1];
    high_diff_energy += diff * diff;
  }
  high_diff_energy /= static_cast<float>(frames - 257);

  // High diff energy is strictly bounded due to band-limiting
  assert(high_diff_energy < 0.5f);

  std::cout << "[PASS] Aliasing suppression at top key verified\n";
}

void test_ep_tuning_accuracy_peak_spectrum() {
  std::cout << "Testing EP fundamental frequency accuracy within 0.1 cents via peak spectrum...\n";

  const uint32_t test_notes[] = {28, 69, 100};
  constexpr float Fs = 48000.0f;

  for (uint32_t note : test_notes) {
    EpEngine engine(Fs);
    double target_f = 440.0 * std::pow(2.0, (static_cast<double>(note) - 69.0) / 12.0);

    engine.start_voice(0, note, 0x7FFF);

    constexpr size_t total_frames = 48000;
    std::vector<float> left(total_frames, 0.0f);
    std::vector<float> right(total_frames, 0.0f);
    engine.render(left.data(), right.data(), total_frames);

    constexpr size_t M = 16384;
    const size_t start = 24000;
    std::vector<double> windowed(M, 0.0);
    for (size_t n = 0; n < M; ++n) {
      double w = 0.5 * (1.0 - std::cos(2.0 * M_PI * n / (M - 1)));
      windowed[n] = static_cast<double>(left[start + n]) * w;
    }

    auto spectrum_power = [&](double f) -> double {
      double re = 0.0;
      double im = 0.0;
      for (size_t n = 0; n < M; ++n) {
        double phase = 2.0 * M_PI * f * static_cast<double>(n) / static_cast<double>(Fs);
        re += windowed[n] * std::cos(phase);
        im += windowed[n] * std::sin(phase);
      }
      return re * re + im * im;
    };

    double a = target_f - 1.0;
    double b = target_f + 1.0;
    const double gr = (std::sqrt(5.0) - 1.0) / 2.0;
    double c = b - gr * (b - a);
    double d = a + gr * (b - a);

    for (int iter = 0; iter < 40; ++iter) {
      if (spectrum_power(c) > spectrum_power(d)) {
        b = d;
        d = c;
        c = b - gr * (b - a);
      } else {
        a = c;
        c = d;
        d = a + gr * (b - a);
      }
    }

    double measured_freq = (b + a) / 2.0;
    double error_cents = std::abs(1200.0 * std::log2(measured_freq / target_f));
    double rel_error = std::abs(measured_freq - target_f) / target_f;

    std::cout << "  Note " << note << " Target: " << target_f << " Hz, Measured: "
              << measured_freq << " Hz, Error: " << error_cents << " cents (rel: " << rel_error << ")\n";

    assert(error_cents < 0.1);
    assert(rel_error < 0.0006);
  }

  std::cout << "[PASS] Tuning across keys E1 to E7 verified within 0.1 cents\n";
}

void test_ep_aliasing_dft_below_minus_45_dbfs() {
  std::cout << "Testing EP aliasing at top key E7 (MIDI 100) and top velocity (127) below -45 dBFS...\n";

  EpEngine engine(48000.0f);
  engine.start_voice(0, 100, 0xFFFF);

  constexpr size_t total_N = 8192;
  constexpr size_t N = 4096;
  std::vector<float> left(total_N, 0.0f);
  std::vector<float> right(total_N, 0.0f);
  engine.render(left.data(), right.data(), total_N);

  std::vector<float> windowed(N, 0.0f);
  for (size_t n = 0; n < N; ++n) {
    float w = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * n / (N - 1)));
    windowed[n] = left[4096 + n] * w;
  }

  float f0 = 440.0f * std::pow(2.0f, (100.0f - 69.0f) / 12.0f);

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

  std::vector<bool> is_harmonic(N / 2, false);
  for (int h = 1; h * f0 < 24000.0f; ++h) {
    float target_bin = (h * f0 / 48000.0f) * N;
    int b_center = static_cast<int>(std::round(target_bin));
    for (int b = b_center - 4; b <= b_center + 4; ++b) {
      if (b >= 1 && b < static_cast<int>(N / 2)) {
        is_harmonic[b] = true;
      }
    }
  }

  const double full_scale_mag = static_cast<double>(N) / 4.0;
  double max_aliased_peak = 0.0;
  for (size_t k = 1; k < N / 2; ++k) {
    if (!is_harmonic[k]) {
      max_aliased_peak = std::max(max_aliased_peak, mag[k]);
    }
  }

  double alias_dbfs = 20.0 * std::log10(max_aliased_peak / full_scale_mag + 1e-12);
  std::cout << "  E7 Velocity 127 Peak Aliased Component: " << alias_dbfs << " dBFS\n";

  assert(alias_dbfs < -45.0);
  std::cout << "[PASS] EP aliasing at top key E7 remains below -45 dBFS\n";
}

void test_ep_midi2_pitch_bend() {
  std::cout << "Testing EP MIDI 2.0 pitch bend...\n";

  EpEngine engine(48000.0f);
  assert(engine.set_part_pitch_bend(0, 0x80000000u));
  assert(std::abs(engine.part_pitch_bend(0) - 0.0f) < 1e-4f);

  assert(engine.set_part_pitch_bend(0, 0u));
  assert(std::abs(engine.part_pitch_bend(0) - (-2.0f)) < 1e-4f);

  assert(engine.set_part_pitch_bend(0, 0xFFFFFFFFu));
  assert(std::abs(engine.part_pitch_bend(0) - 2.0f) < 1e-4f);

  assert(engine.set_part_pitch_bend(3, 0xC0000000u));
  assert(std::abs(engine.part_pitch_bend(3) - 1.0f) < 1e-4f);
  assert(std::abs(engine.part_pitch_bend(0) - 2.0f) < 1e-4f);

  std::cout << "[PASS] EP MIDI 2.0 pitch bend verified\n";
}

void test_ep_bit_identical_offline_renders() {
  std::cout << "Testing bit-identical offline rendering for key and velocity sweep...\n";

  auto render_pass = []() -> std::vector<float> {
    EpEngine engine(48000.0f);
    std::vector<float> out(48000 * 2, 0.0f);
    std::vector<float> left(64, 0.0f);
    std::vector<float> right(64, 0.0f);

    uint32_t write_idx = 0;
    // Play a sequence of keys with varying velocities
    for (uint32_t n = 40; n <= 80; n += 5) {
      engine.start_voice(0, n, (n * 3) << 8);
      for (int b = 0; b < 20; ++b) {
        std::fill(left.begin(), left.end(), 0.0f);
        std::fill(right.begin(), right.end(), 0.0f);
        engine.render(left.data(), right.data(), 64);
        for (int i = 0; i < 64 && write_idx < out.size(); ++i) {
          out[write_idx++] = left[i];
        }
      }
      engine.stop_voice(0, n);
    }
    return out;
  };

  std::vector<float> run1 = render_pass();
  std::vector<float> run2 = render_pass();

  assert(run1.size() == run2.size());
  for (size_t i = 0; i < run1.size(); ++i) {
    assert(run1[i] == run2[i]);  // Exact bit-identical float comparison
  }

  std::cout << "[PASS] Two independent render runs are exactly bit-identical\n";
}

void test_ep_reed_voicing_and_tuning() {
  std::cout << "Testing EP Reed voicing parameters and tuning across E1 to E7 (Story 14.3)...\n";

  EpEngine engine(48000.0f);
  auto reed_params = create_default_reed_parameters();
  assert(reed_params.instrument_type == EpInstrumentType::kReed);
  engine.set_part_voice_params(0, reed_params);
  assert(engine.part_voice_params(0).instrument_type == EpInstrumentType::kReed);

  const uint32_t test_notes[] = {28, 40, 52, 64, 69, 76, 88, 100};
  constexpr float sample_rate = 48000.0f;

  for (uint32_t note : test_notes) {
    float expected_freq = 440.0f * std::pow(2.0f, (static_cast<float>(note) - 69.0f) / 12.0f);

    engine.reset_play_state();
    engine.set_part_voice_params(0, reed_params);
    EngineStatus st = engine.start_voice(0, note, 0x8000);
    assert(st == EngineStatus::kOk);
    assert(engine.active_voice_count() == 1);

    const uint32_t warmup_frames = static_cast<uint32_t>(sample_rate * 0.05f);
    std::vector<float> warmup_l(warmup_frames, 0.0f);
    std::vector<float> warmup_r(warmup_frames, 0.0f);
    engine.render(warmup_l.data(), warmup_r.data(), warmup_frames);

    const uint32_t record_frames = static_cast<uint32_t>(sample_rate * 0.5f);
    std::vector<float> rec_l(record_frames, 0.0f);
    std::vector<float> rec_r(record_frames, 0.0f);
    engine.render(rec_l.data(), rec_r.data(), record_frames);

    int zero_crossings = 0;
    int first_crossing = -1;
    int last_crossing = -1;
    const int min_interval = static_cast<int>(0.6f * (sample_rate / expected_freq));

    for (uint32_t i = 1; i < record_frames; ++i) {
      if (rec_l[i - 1] <= 0.0f && rec_l[i] > 0.0f) {
        if (last_crossing >= 0 && (static_cast<int>(i) - last_crossing) < min_interval) {
          continue;
        }
        zero_crossings++;
        if (first_crossing < 0) first_crossing = static_cast<int>(i);
        last_crossing = static_cast<int>(i);
      }
    }

    assert(zero_crossings > 10);
    assert(last_crossing > first_crossing);

    float measured_period = static_cast<float>(last_crossing - first_crossing) / static_cast<float>(zero_crossings - 1);
    float measured_freq = sample_rate / measured_period;
    float cents_error = std::abs(1200.0f * std::log2(measured_freq / expected_freq));

    assert(cents_error <= 8.0f);
    engine.stop_voice(0, note);
  }

  std::cout << "[PASS] EP Reed tuning across E1 to E7 verified\n";
}

void test_ep_render_split_multi_part() {
  std::cout << "Testing EP multi-part render_split and voice isolation (Story 14.3)...\n";

  EpEngine engine(48000.0f);
  engine.set_part_voice_params(0, create_default_tine_parameters());
  engine.set_part_voice_params(1, create_default_reed_parameters());

  assert(engine.start_voice(0, 60, 0x7FFF) == EngineStatus::kOk);
  assert(engine.start_voice(1, 64, 0x7FFF) == EngineStatus::kOk);

  VoiceInfo vi0{}, vi1{};
  assert(engine.voice_info(0, vi0));
  assert(engine.voice_info(1, vi1));
  assert(vi0.note == 60);
  assert(vi0.channel == 0);
  assert(vi1.note == 64);
  assert(vi1.channel == 1);

  constexpr size_t kFrames = 128;
  std::vector<std::vector<float>> part_l(16, std::vector<float>(kFrames, 0.0f));
  std::vector<std::vector<float>> part_r(16, std::vector<float>(kFrames, 0.0f));

  // Pre-seed unused part 2 to verify additive accumulation (no clobbering)
  for (size_t i = 0; i < kFrames; ++i) {
    part_l[2][i] = 0.42f;
    part_r[2][i] = 0.42f;
  }

  float* left_ptrs[16];
  float* right_ptrs[16];
  for (size_t p = 0; p < 16; ++p) {
    left_ptrs[p] = part_l[p].data();
    right_ptrs[p] = part_r[p].data();
  }

  engine.render_split(left_ptrs, right_ptrs, kFrames);

  float max_p0 = 0.0f;
  for (size_t i = 0; i < kFrames; ++i) max_p0 = std::max(max_p0, std::abs(part_l[0][i]));
  assert(max_p0 > 0.001f);

  float max_p1 = 0.0f;
  for (size_t i = 0; i < kFrames; ++i) max_p1 = std::max(max_p1, std::abs(part_l[1][i]));
  assert(max_p1 > 0.001f);

  // Unused Part 2 pre-existing audio is preserved intact
  for (size_t i = 0; i < kFrames; ++i) {
    assert(std::abs(part_l[2][i] - 0.42f) < 1e-6f);
    assert(std::abs(part_r[2][i] - 0.42f) < 1e-6f);
  }

  // Untouched parts 3..15 remain strictly zero
  for (size_t p = 3; p < 16; ++p) {
    for (size_t i = 0; i < kFrames; ++i) {
      assert(part_l[p][i] == 0.0f);
      assert(part_r[p][i] == 0.0f);
    }
  }

  std::cout << "[PASS] EP multi-part render_split isolation and additive accumulation verified\n";
}

int main() {
  std::cout << "========================================\n";
  std::cout << "Starting EpEngine Test Suite (Stories 14.2 & 14.3)\n";
  std::cout << "========================================\n";

  test_ep_engine_initialization_and_metadata();
  test_ep_tuning_sweep_e1_to_e7();
  test_ep_tuning_accuracy_peak_spectrum();
  test_ep_rt_safety_and_render_blocks();
  test_ep_voice_stealing_and_fade();
  test_ep_aliasing_at_top_key_and_velocity();
  test_ep_aliasing_dft_below_minus_45_dbfs();
  test_ep_midi2_pitch_bend();
  test_ep_bit_identical_offline_renders();
  test_ep_reed_voicing_and_tuning();
  test_ep_render_split_multi_part();

  std::cout << "========================================\n";
  std::cout << "All EpEngine tests passed successfully!\n";
  std::cout << "========================================\n";
  return 0;
}
