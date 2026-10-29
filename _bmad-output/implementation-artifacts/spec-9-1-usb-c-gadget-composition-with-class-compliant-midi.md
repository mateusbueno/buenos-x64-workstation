# Story 9.1: USB-C Gadget Composition with Class-Compliant MIDI

## Summary
Implemented USB-C peripheral composite gadget composition and security validation for Velora 73 per Story 9.1, FR36, FR37, FR55, AR9, AR38, NFR24, and AD-13:
- **Composite ConfigFS setup script (`sysd/scripts/velora-usb-gadget.sh`)**:
  - Configures fixed USB identifiers: `idVendor=0x1209`, `idProduct=0x5673`, `bcdDevice=0x0100`, `bcdUSB=0x0200`.
  - Exposes exactly three functions in configuration `c.1`:
    1. **UAC2 Audio (`functions/uac2.usb0`)**: 4-channel 48 kHz / 24-bit capture (stereo main + stereo aux) in asynchronous mode with explicit feedback endpoint.
    2. **MIDI (`functions/midi2.usb0` / `functions/midi.usb0`)**: MIDI 2.0 UMP with MIDI 1.0 alternate setting for driverless DAW connection.
    3. **CDC-NCM Ethernet (`functions/ncm.usb0`)**: High-speed peer-to-peer network link for Velora Studio communication.
  - Generates per-unit serial from `/etc/machine-id` or system boot ID.
  - Binds dynamically to the SoC's primary UDC driver.
- **Security & Storage Invariant Guard (`tools/ci/check_usb_gadget.py`)**:
  - CI test verifying that no mass-storage (`mass_storage`, `f_mass_storage`) or ACM/serial console (`acm`, `f_acm`, `f_serial`) functions are configured.
  - Ensures internal NVMe storage is never exposed as a USB block device (NFR24).
  - Includes `--self-test` suite.
- **Cross-Platform Enumeration Matrix (`sysd/USB-MATRIX.md`)**:
  - Documents driverless in-box enumeration across macOS (CoreAudio / CoreMIDI / Apple NCM), Windows 11 (USBAudio2.sys / usbmidi2.sys / UsbNcm.sys), and Linux (snd-usb-audio / snd-usbmidi-lib / cdc_ncm).
- **C++ Inspection & Service Integration (`sysd/include/velora/sysd/usb_gadget_manager.h`, `sysd/src/usb_gadget_manager.cpp`, `sysd/tests/usb_gadget_test.cpp`)**:
  - Exposes `UsbGadgetManager` in `sysd` to inspect gadget status, active functions, and enforce absence of storage/serial endpoints.

## Verification
- `check_usb_gadget` and `check_usb_gadget_selftest` pass in CTest.
- `sysd_usb_gadget_test` unit tests pass with 100% assertions satisfied.
- Clean-tree purity and lints pass.
