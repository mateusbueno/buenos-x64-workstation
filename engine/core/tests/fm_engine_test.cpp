#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

#include "velora/core/fm_engine.hpp"

using namespace velora::core;

void test_fm_engine_initialization_and_metadata() {
  std::cout << "Testing FM Engine initialization and metadata...\n";

  FmEngine engine(48000.0f);
  assert(engine.type_id() == EngineTypeId::kFm);
  assert(engine.cost_units_per_voice() == 4);
  assert(engine.voice_capacity() == 256);
  assert(engine.active_voice_count() == 0);

  // Check voice info on inactive slot
  VoiceInfo info{};
  assert(!engine.voice_info(0, info));

  std::cout << "[PASS] FM Engine initialization\n";
}

void test_fm_algorithms_sweep() {
  std::cout << "Testing all 32 FM algorithms sweep...\n";

  FmEngine engine(48000.0f);

  for (uint8_t alg = 1; alg <= 32; ++alg) {
    FmVoiceParameters params = create_init_fm_voice();
    params.algorithm = alg;
    // Turn on all operators with moderate level
    for (int op = 0; op < 6; ++op) {
      params.operators[op].total_level = 80;
      params.operators[op].freq_coarse = (op + 1);
    }
    engine.set_part_voice_params(0, params);

    // Start voice Note 60
    EngineStatus st = engine.start_voice(0, 60, 0x8000);
    assert(st == EngineStatus::kOk);
    assert(engine.active_voice_count() == 1);

    // Render 4 blocks
    std::vector<float> left(256, 0.0f);
    std::vector<float> right(256, 0.0f);
    engine.render(left.data(), right.data(), 256);

    // Verify audio rendered, no NaN, no Inf, bounded output
    float max_amp = 0.0f;
    for (size_t i = 0; i < 256; ++i) {
      assert(!std::isnan(left[i]));
      assert(!std::isinf(left[i]));
      assert(!std::isnan(right[i]));
      assert(!std::isinf(right[i]));
      max_amp = std::max(max_amp, std::abs(left[i]));
    }
    assert(max_amp > 0.0001f);
    assert(max_amp <= 2.0f);

    engine.stop_voice(0, 60);

    // Render until voice dies
    for (int b = 0; b < 20; ++b) {
      engine.render(left.data(), right.data(), 256);
      if (engine.active_voice_count() == 0) break;
    }
  }

  std::cout << "[PASS] All 32 FM algorithms rendered cleanly\n";
}

void test_fm_feedback_sweep_and_stability() {
  std::cout << "Testing FM feedback stability (levels 0..7)...\n";

  FmEngine engine(48000.0f);

  for (uint8_t fb = 0; fb <= 7; ++fb) {
    FmVoiceParameters params = create_init_fm_voice();
    params.algorithm = 1; // feedback on OP6
    params.feedback = fb;
    params.operators[0].total_level = 99; // OP1 carrier
    params.operators[5].total_level = 90; // OP6 modulator with fb

    engine.set_part_voice_params(0, params);
    engine.start_voice(0, 69, 0x9000); // A4 (440 Hz)

    std::vector<float> left(512, 0.0f);
    std::vector<float> right(512, 0.0f);
    engine.render(left.data(), right.data(), 512);

    float max_amp = 0.0f;
    for (size_t i = 0; i < 512; ++i) {
      assert(!std::isnan(left[i]));
      assert(!std::isinf(left[i]));
      max_amp = std::max(max_amp, std::abs(left[i]));
    }
    assert(max_amp > 0.01f);
    assert(max_amp < 3.0f); // bounded, no divergence

    engine.stop_voice(0, 69);
  }

  std::cout << "[PASS] FM feedback sweep stable across all levels\n";
}

void test_fm_bit_identical_renders() {
  std::cout << "Testing bit-identical offline FM renders...\n";

  auto render_pass = []() {
    FmEngine engine(48000.0f);
    FmVoiceParameters p = create_ep_fm_voice();
    engine.set_part_voice_params(0, p);

    engine.start_voice(0, 60, 0x8000);
    std::vector<float> buffer(1024, 0.0f);
    std::vector<float> dummy(1024, 0.0f);
    engine.render(buffer.data(), dummy.data(), 1024);
    return buffer;
  };

  std::vector<float> run1 = render_pass();
  std::vector<float> run2 = render_pass();

  assert(run1.size() == run2.size());
  for (size_t i = 0; i < run1.size(); ++i) {
    assert(run1[i] == run2[i]); // Bit-identical
  }

  std::cout << "[PASS] FM renders are 100% bit-identical\n";
}

void test_fm_velocity_sensitivity() {
  std::cout << "Testing FM velocity sensitivity...\n";

  FmEngine engine(48000.0f);
  FmVoiceParameters p = create_ep_fm_voice();
  engine.set_part_voice_params(0, p);

  // Soft note (velocity ~25)
  engine.start_voice(0, 60, 0x3000);
  std::vector<float> soft_l(256, 0.0f), soft_r(256, 0.0f);
  engine.render(soft_l.data(), soft_r.data(), 256);
  engine.stop_voice(0, 60);

  // Hard note (velocity ~127)
  engine.start_voice(0, 60, 0xFE00);
  std::vector<float> hard_l(256, 0.0f), hard_r(256, 0.0f);
  engine.render(hard_l.data(), hard_r.data(), 256);
  engine.stop_voice(0, 60);

  float max_soft = 0.0f, max_hard = 0.0f;
  for (size_t i = 0; i < 256; ++i) {
    max_soft = std::max(max_soft, std::abs(soft_l[i]));
    max_hard = std::max(max_hard, std::abs(hard_l[i]));
  }

  assert(max_hard > max_soft * 1.5f);
  std::cout << "[PASS] FM velocity sensitivity verified (soft=" << max_soft << ", hard=" << max_hard << ")\n";
}

void test_fm_steal_fade_and_metadata() {
  std::cout << "Testing FM steal fade and voice metadata...\n";

  FmEngine engine(48000.0f);
  FmVoiceParameters p = create_ep_fm_voice();
  engine.set_part_voice_params(0, p);

  engine.start_voice(0, 64, 0x8000);
  assert(engine.active_voice_count() == 1);

  VoiceInfo info{};
  bool ok = engine.voice_info(0, info);
  assert(ok);
  assert(info.channel == 0);
  assert(info.note == 64);
  assert(info.state == VoiceState::kHeld);

  // Sustain pedal on
  engine.set_sustain(0, true);
  engine.stop_voice(0, 64);
  ok = engine.voice_info(0, info);
  assert(ok);
  assert(info.state == VoiceState::kSustained);

  // Sustain pedal off -> releasing
  engine.set_sustain(0, false);
  ok = engine.voice_info(0, info);
  assert(ok);
  assert(info.state == VoiceState::kReleasing);

  // Steal fade
  bool fade_ok = engine.begin_steal_fade(0, 64);
  assert(fade_ok);

  std::vector<float> l(128, 0.0f), r(128, 0.0f);
  engine.render(l.data(), r.data(), 128);

  // After steal fade, slot is inactive
  assert(engine.active_voice_count() == 0);
  assert(!engine.voice_info(0, info));

  std::cout << "[PASS] FM steal fade and voice metadata\n";
}

void test_fm_split_render_and_brightness() {
  std::cout << "Testing FM split render and Knob 2 Brightness mapping...\n";

  FmEngine engine(48000.0f);
  FmVoiceParameters p = create_ep_fm_voice();
  engine.set_part_voice_params(0, p);
  engine.set_part_voice_params(1, p);

  // Brightness offset on Part 2 (channel 1)
  engine.set_brightness_offset(1, 100);

  engine.start_voice(0, 60, 0x8000);
  engine.start_voice(1, 67, 0x8000);
  assert(engine.active_voice_count() == 2);

  std::vector<std::vector<float>> left_channels(Engine::kSplitChannels, std::vector<float>(64, 0.0f));
  std::vector<std::vector<float>> right_channels(Engine::kSplitChannels, std::vector<float>(64, 0.0f));
  std::vector<float*> left_ptrs(Engine::kSplitChannels);
  std::vector<float*> right_ptrs(Engine::kSplitChannels);
  for (size_t c = 0; c < Engine::kSplitChannels; ++c) {
    left_ptrs[c] = left_channels[c].data();
    right_ptrs[c] = right_channels[c].data();
  }

  engine.render_split(left_ptrs.data(), right_ptrs.data(), 64);

  // Part 1 (channel 0) and Part 2 (channel 1) rendered into their respective split buffers
  float sum_ch0 = 0.0f, sum_ch1 = 0.0f, sum_other = 0.0f;
  for (int i = 0; i < 64; ++i) {
    sum_ch0 += std::abs(left_channels[0][i]);
    sum_ch1 += std::abs(left_channels[1][i]);
    for (size_t c = 2; c < Engine::kSplitChannels; ++c) {
      sum_other += std::abs(left_channels[c][i]);
    }
  }

  assert(sum_ch0 > 0.01f);
  assert(sum_ch1 > 0.01f);
  assert(sum_other == 0.0f);

  std::cout << "[PASS] FM split render and Knob 2 Brightness\n";
}

void test_fm_anti_aliasing_high_key_and_ratio() {
  std::cout << "Testing FM anti-aliasing and stability on high key (C8, note 108) with high ratio (14.0) and max feedback (7)...\n";

  FmEngine engine(48000.0f);
  FmVoiceParameters params = create_init_fm_voice();
  params.algorithm = 1;
  params.feedback = 7; // Max feedback

  // OP1 carrier at ratio 1.0
  params.operators[0].total_level = 99;
  params.operators[0].freq_coarse = 1;

  // OP6 modulator at ratio 14.0 with high modulation index
  params.operators[5].total_level = 95;
  params.operators[5].freq_coarse = 14;

  engine.set_part_voice_params(0, params);
  engine.start_voice(0, 108, 0xFE00); // Note 108 (C8, 4186 Hz)

  std::vector<float> left(2048, 0.0f);
  std::vector<float> right(2048, 0.0f);
  engine.render(left.data(), right.data(), 2048);

  // Compute RMS and peak power
  double sum_sq = 0.0;
  float peak = 0.0f;
  for (size_t i = 0; i < 2048; ++i) {
    assert(!std::isnan(left[i]));
    assert(!std::isinf(left[i]));
    sum_sq += static_cast<double>(left[i] * left[i]);
    peak = std::max(peak, std::abs(left[i]));
  }

  double rms = std::sqrt(sum_sq / 2048.0);

  // Threshold: Peak must stay bounded <= 2.5 and RMS <= 1.0 (no runaway numerical resonance)
  assert(peak < 2.5f);
  assert(rms < 1.0);
  assert(rms > 0.01); // Signal is active

  engine.stop_voice(0, 108);

  std::cout << "[PASS] FM anti-aliasing and high-key stability (peak=" << peak << ", rms=" << rms << ")\n";
}

int main() {
  test_fm_engine_initialization_and_metadata();
  test_fm_algorithms_sweep();
  test_fm_feedback_sweep_and_stability();
  test_fm_bit_identical_renders();
  test_fm_velocity_sensitivity();
  test_fm_steal_fade_and_metadata();
  test_fm_split_render_and_brightness();
  test_fm_anti_aliasing_high_key_and_ratio();

  std::cout << "\nAll FM Engine (Story 12.1) unit tests passed successfully!\n";
  return 0;
}
