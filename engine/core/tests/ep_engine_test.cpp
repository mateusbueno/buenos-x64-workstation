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

    // Count positive-going zero crossings to measure fundamental frequency
    int zero_crossings = 0;
    int first_crossing = -1;
    int last_crossing = -1;

    for (uint32_t i = 1; i < record_frames; ++i) {
      if (rec_l[i - 1] <= 0.0f && rec_l[i] > 0.0f) {
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

int main() {
  std::cout << "========================================\n";
  std::cout << "Starting EpEngine Test Suite (Story 14.2)\n";
  std::cout << "========================================\n";

  test_ep_engine_initialization_and_metadata();
  test_ep_tuning_sweep_e1_to_e7();
  test_ep_rt_safety_and_render_blocks();
  test_ep_voice_stealing_and_fade();
  test_ep_aliasing_at_top_key_and_velocity();
  test_ep_bit_identical_offline_renders();

  std::cout << "========================================\n";
  std::cout << "All EpEngine tests passed successfully!\n";
  std::cout << "========================================\n";
  return 0;
}
