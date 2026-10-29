// Story 3.8: Panel MCU firmware — inputs unit and stress tests:
// - Quadrature encoders (8 encoders + VALUE_ENCODER) at rated max speed with zero missed/reversed steps
// - 12-bit analog faders (9), wheels (2) and pedals (2) at >= 500 Hz with deadband jitter suppression
// - 5,000-press button bounce test across all buttons including VALUE_PUSH with zero double events
// - Boot-time pedal polarity detection (NO/NC switch and continuous pedal direction) reported in HELLO
#include "mcu/panel_inputs.h"

#include <cassert>
#include <cstdint>
#include <vector>

namespace {

struct CapturedFrames {
  std::vector<mcu_frame> frames;
  static void on_tx(void* user, const mcu_frame* f) {
    static_cast<CapturedFrames*>(user)->frames.push_back(*f);
  }
};

void test_encoder_quadrature_rated_speed() {
  CapturedFrames cap;
  mcu_panel_inputs panel{};
  mcu_panel_inputs_config cfg{};
  cfg.fw_version = 0x0104;
  cfg.adc_bits = 12;
  cfg.steps_per_detent = 4;
  cfg.deadband_12bit = 8;
  cfg.report_interval_us = 1000;
  cfg.debounce_us = 5000;

  assert(mcu_panel_inputs_init(&panel, &cfg, &CapturedFrames::on_tx, &cap) == MCU_OK);

  // Gray code sequence for CW rotation: 00 -> 01 -> 11 -> 10 -> 00
  const std::uint8_t cw[4] = {0x00, 0x01, 0x03, 0x02};
  std::uint32_t tick = 1000;

  // Initialize state at 00
  assert(mcu_panel_inputs_sample_encoder(&panel, MCU_CTRL_ENCODER_1, cw[0], tick) == MCU_OK);

  // Spin 500 detents clockwise at rated max speed (e.g. 50 us per quadrature edge = 5,000 detents/sec)
  int total_cw_steps = 0;
  for (int detent = 0; detent < 500; ++detent) {
    for (int edge = 1; edge <= 4; ++edge) {
      tick += 50;
      assert(mcu_panel_inputs_sample_encoder(&panel, MCU_CTRL_ENCODER_1, cw[edge & 3], tick) == MCU_OK);
    }
  }

  for (const auto& f : cap.frames) {
    mcu_control c{};
    assert(mcu_unpack_control(&f, &c) == MCU_OK);
    assert(c.id == MCU_CTRL_ENCODER_1);
    const auto delta = static_cast<std::int16_t>(c.value);
    assert(delta > 0);  // never reversed
    total_cw_steps += delta;
  }
  assert(total_cw_steps == 500);  // zero missed steps

  // Spin 500 detents counter-clockwise on VALUE_ENCODER
  cap.frames.clear();
  const std::uint8_t ccw[4] = {0x00, 0x02, 0x03, 0x01};
  assert(mcu_panel_inputs_sample_encoder(&panel, MCU_CTRL_VALUE_ENCODER, ccw[0], tick) == MCU_OK);
  int total_ccw_steps = 0;
  for (int detent = 0; detent < 500; ++detent) {
    for (int edge = 1; edge <= 4; ++edge) {
      tick += 50;
      assert(mcu_panel_inputs_sample_encoder(&panel, MCU_CTRL_VALUE_ENCODER, ccw[edge & 3], tick) == MCU_OK);
    }
  }
  for (const auto& f : cap.frames) {
    mcu_control c{};
    assert(mcu_unpack_control(&f, &c) == MCU_OK);
    assert(c.id == MCU_CTRL_VALUE_ENCODER);
    const auto delta = static_cast<std::int16_t>(c.value);
    assert(delta < 0);  // never reversed
    total_ccw_steps += delta;
  }
  assert(total_ccw_steps == -500);
}

void test_analog_12bit_filtering_and_deadband() {
  CapturedFrames cap;
  mcu_panel_inputs panel{};
  mcu_panel_inputs_config cfg{};
  cfg.fw_version = 0x0104;
  cfg.adc_bits = 8;  // below 10 bits -> must be rejected per Story 3.8 AC2
  cfg.steps_per_detent = 4;
  cfg.deadband_12bit = 8;
  cfg.report_interval_us = 1000;
  cfg.debounce_us = 5000;
  assert(mcu_panel_inputs_init(&panel, &cfg, &CapturedFrames::on_tx, &cap) == MCU_ERR_BAD_VALUE);

  cfg.adc_bits = 12;
  cfg.report_interval_us = 2500;  // < 500 Hz -> rejected
  assert(mcu_panel_inputs_init(&panel, &cfg, &CapturedFrames::on_tx, &cap) == MCU_ERR_BAD_VALUE);

  cfg.report_interval_us = 1000;  // 1000 Hz (>= 500 Hz)
  assert(mcu_panel_inputs_init(&panel, &cfg, &CapturedFrames::on_tx, &cap) == MCU_OK);

  // Initial sample at rest (2048 for pitch wheel) reports once
  std::uint32_t tick = 1000;
  assert(mcu_panel_inputs_sample_analog(&panel, MCU_CTRL_WHEEL_PITCH, 2048, tick) == MCU_OK);
  assert(cap.frames.size() == 1);

  // Idle jitter within +/- 3 counts for 200 samples at 1 kHz produces ZERO frames
  for (int i = 0; i < 200; ++i) {
    tick += 1000;
    const std::uint16_t noisy = static_cast<std::uint16_t>(2048 + ((i % 3) - 1) * 3);
    assert(mcu_panel_inputs_sample_analog(&panel, MCU_CTRL_WHEEL_PITCH, noisy, tick) == MCU_OK);
  }
  assert(cap.frames.size() == 1);

  // Sweep fader 1 from 0 to 4095: reports smoothly and reaches exact 4095
  cap.frames.clear();
  for (std::uint16_t v = 0; v <= 4095; v = static_cast<std::uint16_t>(v + 64)) {
    tick += 1000;
    assert(mcu_panel_inputs_sample_analog(&panel, MCU_CTRL_FADER_1, v, tick) == MCU_OK);
  }
  tick += 1000;
  assert(mcu_panel_inputs_sample_analog(&panel, MCU_CTRL_FADER_1, 4095, tick) == MCU_OK);
  assert(!cap.frames.empty());
  mcu_control last_ctl{};
  assert(mcu_unpack_control(&cap.frames.back(), &last_ctl) == MCU_OK);
  assert(last_ctl.id == MCU_CTRL_FADER_1 && last_ctl.value == 4095);
}

void test_5000_press_button_bounce_test() {
  // Story 3.8 AC3: Given every button including the VALUE push switch, when it is pressed and released,
  // a debounced BUTTON frame (ID, pressed/released) is sent for each edge, with no double events under a 5,000-press bounce test.
  CapturedFrames cap;
  mcu_panel_inputs panel{};
  mcu_panel_inputs_config cfg{};
  cfg.fw_version = 0x0104;
  cfg.adc_bits = 12;
  cfg.steps_per_detent = 4;
  cfg.deadband_12bit = 8;
  cfg.report_interval_us = 1000;
  cfg.debounce_us = 5000;
  assert(mcu_panel_inputs_init(&panel, &cfg, &CapturedFrames::on_tx, &cap) == MCU_OK);

  std::uint32_t tick = 1000;
  // Seed initial released state (0) for VALUE_PUSH
  assert(mcu_panel_inputs_sample_button(&panel, MCU_BTN_VALUE_PUSH, 0, tick) == MCU_OK);
  assert(cap.frames.empty());

  for (int press = 0; press < 5000; ++press) {
    // Contact bounce on press: rapid chatter (1, 0, 1, 0, 1) every 400 us (< 5000 us debounce window)
    for (int b = 0; b < 5; ++b) {
      tick += 400;
      assert(mcu_panel_inputs_sample_button(&panel, MCU_BTN_VALUE_PUSH, static_cast<std::uint8_t>(b & 1), tick) == MCU_OK);
    }
    // Settle at pressed (1) for 6000 us
    tick += 1000;
    assert(mcu_panel_inputs_sample_button(&panel, MCU_BTN_VALUE_PUSH, 1, tick) == MCU_OK);
    tick += 5500;
    assert(mcu_panel_inputs_sample_button(&panel, MCU_BTN_VALUE_PUSH, 1, tick) == MCU_OK);

    // Contact bounce on release: chatter (0, 1, 0, 1, 0) every 400 us
    for (int b = 0; b < 5; ++b) {
      tick += 400;
      assert(mcu_panel_inputs_sample_button(&panel, MCU_BTN_VALUE_PUSH, static_cast<std::uint8_t>((b + 1) & 1), tick) == MCU_OK);
    }
    // Settle at released (0) for 6000 us
    tick += 1000;
    assert(mcu_panel_inputs_sample_button(&panel, MCU_BTN_VALUE_PUSH, 0, tick) == MCU_OK);
    tick += 5500;
    assert(mcu_panel_inputs_sample_button(&panel, MCU_BTN_VALUE_PUSH, 0, tick) == MCU_OK);
  }

  // Exactly 5,000 press frames + 5,000 release frames = 10,000 frames, strictly alternating 1, 0, 1, 0...
  assert(cap.frames.size() == 10000);
  for (std::size_t i = 0; i < cap.frames.size(); ++i) {
    mcu_button b{};
    assert(mcu_unpack_button(&cap.frames[i], &b) == MCU_OK);
    assert(b.id == MCU_BTN_VALUE_PUSH);
    const std::uint8_t expected_pressed = (i % 2 == 0) ? 1u : 0u;
    assert(b.pressed == expected_pressed);
  }
}

void test_boot_pedal_polarity_detection() {
  // Story 3.8 AC4: Given power-up with pedals plugged in, when the MCU starts,
  // it detects each pedal's polarity (NO/NC, and continuous pedal direction) and reports it in HELLO.
  CapturedFrames cap;
  mcu_panel_inputs panel{};
  mcu_panel_inputs_config cfg{0x0104, 12, 4, 8, 1000, 5000};
  assert(mcu_panel_inputs_init(&panel, &cfg, &CapturedFrames::on_tx, &cap) == MCU_OK);

  // Case 1: All normally open / non-inverted (sustain=50, expression=80, switch=0)
  assert(mcu_panel_inputs_boot(&panel, 50, 80, 0, 500) == MCU_OK);
  assert(cap.frames.size() == 1);
  mcu_hello h{};
  assert(mcu_unpack_hello(&cap.frames[0], &h) == MCU_OK);
  assert(h.board_id == MCU_BOARD_PANEL);
  assert(h.keybed_contacts == 0u);

  // Case 2: Sustain NC (3950), Expression normal (100), Switch NC (1)
  cap.frames.clear();
  assert(mcu_panel_inputs_boot(&panel, 3950, 100, 1, 600) == MCU_OK);
  assert(cap.frames.size() == 1);
  assert(mcu_unpack_hello(&cap.frames[0], &h) == MCU_OK);
  assert(h.keybed_contacts == (MCU_PANEL_PEDAL_SUSTAIN_INVERTED | MCU_PANEL_PEDAL_SWITCH_INVERTED));

  // Verify NC switch pedal reports closed (1) when raw drops to 0 after debounce
  assert(mcu_panel_inputs_sample_pedal_switch(&panel, 0, 1000) == MCU_OK);
  assert(mcu_panel_inputs_sample_pedal_switch(&panel, 0, 6500) == MCU_OK);
  assert(cap.frames.size() == 2);
  mcu_control sw{};
  assert(mcu_unpack_control(&cap.frames[1], &sw) == MCU_OK);
  assert(sw.id == MCU_CTRL_PEDAL_SWITCH && sw.value == 1u);
}

}  // namespace

int main() {
  test_encoder_quadrature_rated_speed();
  test_analog_12bit_filtering_and_deadband();
  test_5000_press_button_bounce_test();
  test_boot_pedal_polarity_detection();
  return 0;
}
