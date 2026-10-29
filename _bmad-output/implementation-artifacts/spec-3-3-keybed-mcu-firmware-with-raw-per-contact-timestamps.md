# Story 3.3: Keybed MCU Firmware with Raw Per-Contact Timestamps

## Summary
Implemented the portable C11 Keybed MCU scanning and protocol firmware module (`mcu/keybed`) per AD-15 and Story 3.3:
- **Sub-1 ms matrix scan (`keybed_scanner_scan`)**: Scans 2-contact and 3-contact keybeds (61, 76, or 88 keys) at `>= 2 kHz` (`<= 500 us` per pass) using a `1 us` hardware timer tick.
- **Per-contact debounce (`keybed_scanner_config_t::debounce_us`)**: Latches the first transition timestamp `t_first_us` and confirms transition after `debounce_us` of stable state while emitting the exact initial transition microsecond timestamp (`t_us = t_first_us`) in `MCU_KEY_CONTACT` (`0x10`) frames.
- **Raw contact streaming**: Emits raw per-contact make/break events (`contact_index` 0..2, `state` break/make, `t_us`) without computing MIDI velocity on the MCU, enabling SoC-side multi-contact organ onset (`Story 2.5`) and attack/release velocity curve evaluation (`Story 3.6`).
- **Aftertouch streaming (`keybed_scanner_feed_aftertouch`)**: Emits `MCU_AFTERTOUCH` (`0x11`) frames when `MCU_CAP_CHANNEL_AFTERTOUCH` or `MCU_CAP_POLY_AFTERTOUCH` is advertised in `HELLO`.
- **Hardware watchdog & clean recovery (`keybed_scanner_service_watchdog`, `keybed_scanner_trigger_watchdog_reset`)**: Models the hardware watchdog reset path (`MCU_RESET_WATCHDOG`) that re-initializes contact state and immediately re-announces `HELLO` with `reset_cause = MCU_RESET_WATCHDOG` and zero stuck contacts.

## Verification
- `mcu_keybed_c_test` (C11) and `mcu_keybed_test` (C++20):
  - 61-key 2-contact and 88-key 3-contact `HELLO` frame validation.
  - Sub-1 ms (`250 us` step) scan rate enforcement (`> 500 us` rejected).
  - 10,000-cycle rapid keybed contact bounce stress test with zero stuck notes or missed transitions.
  - Simulated hardware watchdog reset during active chord holding, verifying clean state reset and `HELLO` re-announcement with `MCU_RESET_WATCHDOG`.
