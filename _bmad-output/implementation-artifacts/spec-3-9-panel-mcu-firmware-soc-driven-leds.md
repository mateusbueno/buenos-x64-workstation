# Spec 3.9: Panel MCU firmware — SoC-driven LEDs

**Story:** `3-9-panel-mcu-firmware-soc-driven-leds`
**Status:** Done
**Covers:** AR41 (LEDs), FR111 (partial: MCU side), UX-DR50, UX-DR51, UX-DR52, NFR36 (partial: firmware side of LED contrast)

## Summary

Implemented the portable C11 panel MCU LED driver under `mcu/panel/` (`panel_leds.h` / `panel_leds.c`):

1. **Strictly SoC-Driven LED State (`mcu_panel_leds_init`, `mcu_panel_leds_on_frame`):**
   - At boot, all 23 panel LED entries (toggle state LEDs, part/scene LED strips, and 8 encoder rings with 11 segments each) initialize to `0` PWM duty and remain off until commanded by an incoming `LED_SET` frame from the SoC.
   - The MCU never modifies any LED autonomously.
2. **Flicker-Free PWM & Brightness Scaling (`mcu_panel_leds_set_global_brightness`):**
   - Enforces a minimum PWM carrier frequency of `>= 20 kHz` (`MCU_PANEL_MIN_PWM_HZ = 20000`) and scales per-LED brightness (`MCU_LED_BRIGHTNESS_STRONG = 255`, `MCU_LED_BRIGHTNESS_WEAK = 38` ~15%, `MCU_LED_BRIGHTNESS_OFF = 0`) by the global brightness tier (`Low = 35%`, `Mid = 68%`, `High = 100%`) into 16-bit PWM duty (`0..65535`).
3. **Encoder Ring Value Arcs:**
   - When the SoC sends a value arc (`arg` `0..255`) for `MCU_LED_ENCODER_RING_1..8`, lit segments receive the scaled arc PWM duty and all remaining unlit segments are strictly `0` (fully off, never weak-lit).
4. **50 klx Contrast Verification (`mcu_panel_leds_evaluate_contrast_50klx`):**
   - Evaluates measured on:off luminance under a 50 klx lamp against the `>= 10:1` contrast target.
