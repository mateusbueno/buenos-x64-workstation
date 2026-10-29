# Velora 73 — USB-C Composite Device Enumeration Matrix

Documenting USB-C peripheral role enumeration, in-box driver binding, interface descriptors, and security invariants per Story 9.1 (FR36, FR37, FR55, AR9, AR38, NFR24, AD-13).

---

## 1. Device Identifiers & Security Invariants

- **Vendor ID (VID)**: `0x1209` (pid.codes open-source hardware registry)
- **Product ID (PID)**: `0x5673` ("V73" — Velora 73 Synthesizer & Stage Piano)
- **Device Class**: `0x00` (Defined at interface level for composite device)
- **Device Protocol**: `0x00`
- **Product String**: `Velora 73`
- **Manufacturer String**: `Velora`
- **Serial Number**: Derived dynamically from `/etc/machine-id` (first 16 hex characters)
- **Power Configuration**: Self-powered / Bus-reporting (`MaxPower: 500 mA`)

### Invariant & Storage Boundary (NFR24, AR38)
- **NO Mass Storage (`f_mass_storage`)**: The internal NVMe SSD and eMMC are **never** exposed as USB mass storage or block devices over USB under any circumstance.
- **NO Serial Console (`f_acm` / `f_serial`)**: Debug console is strictly routed to the internal hardware UART2 header on the board; no virtual serial port or shell is exposed on the user-facing USB-C port.
- **Verified by CI**: `tools/ci/check_usb_gadget.py` strictly checks and gates every commit to ensure no unauthorized functions can be added to the ConfigFS composite definition.

---

## 2. Composite Interface Layout

The device exposes **exactly three** standard USB functions in Configuration 1 (`c.1`):

| Interface # | Function | USB Class / Subclass | Linux Kernel Driver | Purpose |
|---|---|---|---|---|
| **0, 1** | **UAC2 Audio** | Audio (`0x01` / `0x02`) | `f_uac2` | 4-channel 48 kHz / 24-bit capture (Main L/R + Aux L/R), asynchronous mode with explicit feedback endpoint. |
| **2, 3** | **MIDI** | Audio / Midistreaming (`0x01` / `0x03`) | `f_midi2` (or `f_midi`) | USB MIDI 2.0 UMP with MIDI 1.0 alternate setting for backwards compatibility. Bidirectional note, control, and SysEx exchange. |
| **4, 5** | **CDC-NCM** | Communications (`0x02` / `0x0D`) | `f_ncm` | High-speed peer-to-peer Ethernet link for Velora Studio communication over `ws.v1`. |

---

## 3. Host Operating System Enumeration Matrix

| Feature | macOS (14 Sonoma, 15 Sequoia) | Windows 11 (23H2, 24H2) | Linux (Ubuntu 24.04 LTS / Debian 12) |
|---|---|---|---|
| **Driver Installation Required?** | **None** (Driverless / In-box) | **None** (Driverless / In-box) | **None** (Driverless / In-box) |
| **UAC2 Audio Interface** | Native CoreAudio (`AppleUSBAudio.kext`). Discovers as `"Velora 73"` 4-channel input device at 48.0 kHz. | In-box `USBAudio2.sys`. Discovers under *Sound, video and game controllers* as `"Velora 73"`. | ALSA `snd-usb-audio`. Exposes `hw:CARD=Velora73,DEV=0`. |
| **Audio Latency & Drift** | Hardware explicit feedback endpoint locks host frame clock to the internal audio master clock. | Stable isochronous stream using feedback endpoint with zero buffer underruns. | ALSA DLL follower synchronizes sample rates without sample rate conversion drops. |
| **MIDI 2.0 / 1.0 Interface** | CoreMIDI native driver. Exposes bidirectional UMP endpoint `"Velora 73 MIDI"`. Fallback to MIDI 1.0 on older hosts. | Microsoft USB MIDI 2.0 driver (`usbmidi2.sys`) on Windows 11; falls back to `wdma_usb.sys` MIDI 1.0. | ALSA RawMIDI (`snd-usbmidi-lib`). Supports UMP or standard MIDI 1.0 endpoints. |
| **CDC-NCM Ethernet** | Discovers as native USB Ethernet network interface (`en*`). DHCP / Link-local autoconfiguration. | In-box `UsbNcm.sys`. Discovers under *Network adapters* as `"Velora 73 NCM"`. | In-box `cdc_ncm.ko`. Discovers as `usb0` interface. |
| **Velora Studio Discovery** | Velora Studio connects directly over peer-to-peer link to `sysd` on port 50051. | Velora Studio connects directly over peer-to-peer link to `sysd` on port 50051. | Velora Studio connects directly over peer-to-peer link to `sysd` on port 50051. |
| **Mass Storage Visibility** | **Hidden** (Zero block devices or disk volumes exposed). | **Hidden** (Zero disk drives or Explorer drive letters). | **Hidden** (Zero `/dev/sd*` or `/dev/nvme*` nodes exposed). |
