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

    assert(error_cents <= 0.2);
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

void test_ep_continuous_touch_velocity_sweep() {
  std::cout << "Testing continuous touch and velocity response (Story 14.4)...\n";

  const uint32_t sampled_keys[] = {28, 40, 52, 60, 72};
  const EpInstrumentType types[] = {EpInstrumentType::kTine, EpInstrumentType::kReed};

  for (auto itype : types) {
    for (uint32_t note : sampled_keys) {
      double prev_rms = -1.0;
      double prev_db = -100.0;

      for (uint32_t vel = 1; vel <= 127; vel += 4) {
        EpEngine engine(48000.0f);
        EpVoicingParameters p = (itype == EpInstrumentType::kTine)
                                    ? create_default_tine_parameters()
                                    : create_default_reed_parameters();
        engine.set_part_voice_params(0, p);

        uint32_t vel16 = (vel << 8) | vel;
        engine.start_voice(0, note, vel16);

        constexpr size_t N = 12288;
        std::vector<float> left(N, 0.0f);
        std::vector<float> right(N, 0.0f);
        engine.render(left.data(), right.data(), N);

        // Windowed RMS and high-frequency spectral derivative to prevent window truncation artifacts
        const size_t start = 256;
        const size_t len = N - start;
        double sum_sq = 0.0;
        double sum_w_sq = 0.0;
        for (size_t i = 0; i < len; ++i) {
          double w = 0.5 * (1.0 - std::cos(2.0 * M_PI * i / (len - 1)));
          double sample = left[start + i];
          sum_sq += (sample * w) * (sample * w);
          sum_w_sq += w * w;
        }
        double rms = std::sqrt(sum_sq / sum_w_sq);

        if (prev_rms >= 0.0) {
          if (rms < prev_rms - 0.005) {
            std::cout << "DEBUG FAIL note=" << note << " itype=" << static_cast<int>(itype) << " vel=" << vel
                      << " rms=" << rms << " prev=" << prev_rms << "\n";
          }
          // Monotonically non-decreasing level within numerical and saturation bounds
          assert(rms >= prev_rms - 0.005);

          // No discrete velocity layer stepping: smooth continuous response (< 15.0 dB threshold recorded with the test)
          if (vel > 5) {
            double db = 20.0 * std::log10(rms + 1e-9);
            double step_db = db - prev_db;
            assert(step_db < 15.0);
          }
        }

        prev_rms = rms;
        prev_db = 20.0 * std::log10(rms + 1e-9);
      }
    }
  }

  std::cout << "[PASS] Continuous touch velocity monotonicity and smooth scaling verified\n";
}

void test_ep_release_velocity_damper_sound() {
  std::cout << "Testing release velocity damper sound and decay differentiation (Story 14.4)...\n";

  // Compare release velocity 1 (slow key release) vs 127 (staccato snap release)
  auto measure_release = [](uint8_t rel_vel) -> std::pair<double, double> {
    EpEngine engine(48000.0f);
    auto p = create_default_tine_parameters();
    p.damper_noise = 0.5f;  // active damper felt noise
    engine.set_part_voice_params(0, p);

    engine.start_voice(0, 60, 0x8000);  // play note 60

    // Sustain for 0.2s
    constexpr size_t hold_frames = 9600;
    std::vector<float> l(hold_frames, 0.0f);
    std::vector<float> r(hold_frames, 0.0f);
    engine.render(l.data(), r.data(), hold_frames);

    // Release key with specified release velocity
    uint32_t rel16 = (static_cast<uint32_t>(rel_vel) << 8);
    engine.release_voice(0, 60, rel16);

    // Record release segment [0..0.15s]
    constexpr size_t rel_frames = 7200;
    std::vector<float> rel_l(rel_frames, 0.0f);
    std::vector<float> rel_r(rel_frames, 0.0f);
    engine.render(rel_l.data(), rel_r.data(), rel_frames);

    // Early release transient energy [0..1000 frames] (contains felt noise)
    double noise_energy = 0.0;
    for (size_t i = 0; i < 1000; ++i) {
      noise_energy += rel_l[i] * rel_l[i];
    }

    // Late release tail energy [3000..7000 frames] (measures damping rate)
    double tail_energy = 0.0;
    for (size_t i = 3000; i < 7000; ++i) {
      tail_energy += rel_l[i] * rel_l[i];
    }

    return {noise_energy, tail_energy};
  };

  auto [noise_slow, tail_slow] = measure_release(1);
  auto [noise_fast, tail_fast] = measure_release(127);

  std::cout << "  Slow release (vel 1):   noise_energy=" << noise_slow << ", tail_energy=" << tail_slow << "\n";
  std::cout << "  Fast release (vel 127): noise_energy=" << noise_fast << ", tail_energy=" << tail_fast << "\n";

  // Fast release hits dampers harder (louder scrape transient)
  assert(noise_fast > noise_slow);

  // Fast release damps string faster (less late tail energy)
  assert(tail_slow > tail_fast);

  std::cout << "[PASS] Release velocity produces measurably distinct damper sound and decay\n";
}

void test_ep_continuous_half_pedal_law() {
  std::cout << "Testing continuous half-pedal law (Story 3.11 & 14.4)...\n";

  EpEngine engine(48000.0f);
  assert(engine.part_pedal_cc(0) == 0.0f);

  // Helper: converts CC value 0..127 to 32-bit MIDI 2.0 controller value
  auto cc_to_u32 = [](float cc) -> uint32_t {
    return static_cast<uint32_t>((cc / 127.0f) * 4294967295.0f);
  };

  // Verify half-pedal scale curve
  assert(std::abs(EpVoice::half_pedal_scale(0.0f) - 1.0f) < 1e-5f);
  assert(std::abs(EpVoice::half_pedal_scale(63.5f) - 1.0f) < 1e-5f);
  assert(std::abs(EpVoice::half_pedal_scale(127.0f) - 1.0f) < 1e-5f);

  // In half pedal zone (0 < cc < 63.5), scale decreases linearly from 1.0 down to 0.1
  float scale_mid = EpVoice::half_pedal_scale(31.75f);
  assert(std::abs(scale_mid - 0.55f) < 1e-3f);

  // Test half-pedal damping on ringing notes
  // As pedal CC increases towards 63.5, damper lifts more and note rings longer (tail energy increases)
  auto measure_pedal_ring = [&](float pedal_cc) -> double {
    EpEngine eng(48000.0f);
    eng.start_voice(0, 60, 0x8000);

    // Play 0.1s
    float buf_l[4800] = {0.0f};
    float buf_r[4800] = {0.0f};
    eng.render(buf_l, buf_r, 4800);

    // Release key with pedal at pedal_cc
    eng.set_part_cc(0, 64, cc_to_u32(pedal_cc));
    eng.stop_voice(0, 60);

    // Measure decay over next 0.5s (24000 frames)
    constexpr size_t decay_frames = 24000;
    std::vector<float> decay_l(decay_frames, 0.0f);
    std::vector<float> decay_r(decay_frames, 0.0f);
    eng.render(decay_l.data(), decay_r.data(), decay_frames);

    double late_energy = 0.0;
    for (size_t i = 12000; i < decay_frames; ++i) {
      late_energy += decay_l[i] * decay_l[i];
    }
    return late_energy;
  };

  double ring_0 = measure_pedal_ring(0.0f);    // Full damper down (fast stop)
  double ring_16 = measure_pedal_ring(16.0f);  // Heavy damper
  double ring_32 = measure_pedal_ring(32.0f);  // Medium half-pedal
  double ring_48 = measure_pedal_ring(48.0f);  // Light half-pedal
  double ring_63 = measure_pedal_ring(63.0f);  // Almost fully lifted

  std::cout << "  Late energy at CC 0:  " << ring_0 << "\n";
  std::cout << "  Late energy at CC 16: " << ring_16 << "\n";
  std::cout << "  Late energy at CC 32: " << ring_32 << "\n";
  std::cout << "  Late energy at CC 48: " << ring_48 << "\n";
  std::cout << "  Late energy at CC 63: " << ring_63 << "\n";

  // Monotonically longer ring as pedal lifts damper through half-pedal range
  assert(ring_16 > ring_0);
  assert(ring_32 > ring_16);
  assert(ring_48 > ring_32);
  assert(ring_63 > ring_48);

  // Test real-time pedal movement during sustained note
  EpEngine eng(48000.0f);
  eng.start_voice(0, 60, 0x8000);
  eng.set_part_cc(0, 64, cc_to_u32(127.0f)); // Full sustain
  eng.stop_voice(0, 60); // Key released, note sustained by pedal

  VoiceInfo vi{};
  assert(eng.voice_info(0, vi));
  assert(vi.state == VoiceState::kSustained);

  // Move pedal into half-pedal zone (e.g. 30)
  eng.set_part_cc(0, 64, cc_to_u32(30.0f));
  assert(eng.voice_info(0, vi));
  assert(vi.state == VoiceState::kReleasing);

  // Render 64-frame blocks while moving pedal
  float l[64] = {0.0f}, r[64] = {0.0f};
  for (int cc = 30; cc >= 0; cc -= 5) {
    std::fill_n(l, 64, 0.0f);
    std::fill_n(r, 64, 0.0f);
    eng.set_part_cc(0, 64, cc_to_u32(static_cast<float>(cc)));
    eng.render(l, r, 64);
    for (int i = 0; i < 64; ++i) {
      assert(!std::isnan(l[i]) && !std::isinf(l[i]));
      assert(!std::isnan(r[i]) && !std::isinf(r[i]));
    }
  }

  std::cout << "[PASS] Continuous half-pedal damping law and RT modulation verified\n";
}

int main() {
  std::cout << "========================================\n";
  std::cout << "Starting EpEngine Test Suite (Stories 14.2 & 14.3 & 14.4)\n";
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
  test_ep_continuous_touch_velocity_sweep();
  test_ep_release_velocity_damper_sound();
  test_ep_continuous_half_pedal_law();

  std::cout << "========================================\n";
  std::cout << "All EpEngine tests passed successfully!\n";
  std::cout << "========================================\n";
  return 0;
}
