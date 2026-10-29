# Spec 3.8: Panel MCU firmware — inputs

**Story:** `3-8-panel-mcu-firmware-inputs`
**Status:** Done
**Covers:** AR41 (inputs), FR42 (partial: pedal ADC and polarity detection at boot), FR43 (partial: wheel ADC)

## Summary

Implemented the portable C11 panel MCU input firmware module under `mcu/panel/` (`velora_mcu_panel`):

1. **Quadrature Encoders (`mcu_panel_inputs_sample_encoder`):**
   - Decodes all 9 encoders (`MCU_CTRL_ENCODER_1`..`8`, `MCU_CTRL_VALUE_ENCODER`) using a 4-state Gray-code state machine with illegal 2-bit transition rejection, emitting `CONTROL` frames with signed relative step deltas and zero missed or reversed steps at rated maximum speed.
2. **12-Bit Analog Controls (`mcu_panel_inputs_sample_analog`):**
   - Samples the 9 faders (`16..24`), 2 wheels (`32..33`), and 2 continuous pedals (`40..41`) at 12-bit resolution (enforcing `adc_bits >= 10`), applies integer IIR smoothing with endpoint snap (`0` and `4095`), enforces `report_interval_us <= 2000` (`>= 500 Hz`), and suppresses idle jitter with a configurable deadband.
3. **Debounced Buttons & Switch Pedal (`mcu_panel_inputs_sample_button`, `mcu_panel_inputs_sample_pedal_switch`):**
   - Time-based holdoff debouncer for all 21 buttons (including `MCU_BTN_VALUE_PUSH` and the 3 toggles) and `MCU_CTRL_PEDAL_SWITCH`, verified across a 5,000-press contact bounce test with zero double events.
4. **Boot-Time Pedal Polarity Detection (`mcu_panel_inputs_boot`):**
   - Detects initial resting polarity of sustain, expression, and switch pedals (`MCU_PANEL_PEDAL_SUSTAIN_INVERTED`, `MCU_PANEL_PEDAL_EXPRESSION_INVERTED`, `MCU_PANEL_PEDAL_SWITCH_INVERTED`) and reports the bitmask in the panel `HELLO` frame.
