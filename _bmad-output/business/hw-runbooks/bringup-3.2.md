# DRAFT FOR FOUNDER — does not satisfy the listed stories' acceptance criteria

Runbook for Story 3.2 (prototype board bring-up). Prepared 2026-10-19. Nothing here has been run on hardware. No SoM vendor, part number, board file, connector, GPIO or register is named, because the repo names none for the prototype; every such item is "TBD from vendor docs".

Path shorthand (all relative to `_bmad-output/`):
- EPICS = `planning-artifacts/epics.md`
- HWI = `planning-artifacts/architecture/architecture-buenos-x64-workstation-2026-10-17/HARDWARE-INTERFACES.md`
- SPINE = `planning-artifacts/architecture/architecture-buenos-x64-workstation-2026-10-17/ARCHITECTURE-SPINE.md`
- MAP = `planning-artifacts/architecture/architecture-buenos-x64-workstation-2026-10-17/MCU-ID-MAP.md`

## 1. Scope and acceptance criteria (source: EPICS:1998-2023)

Covers AR9 (partial), AR3 (partial), AR24 (partial), NFR38, NFR41, NFR43, EXT10 dependency (EPICS:2004). Runs on: Linux build host or ARM64 Linux VM (image) plus the prototype board (EPICS:2006). Blocked by Story 11.7, target month 7 (EPICS:2008).

| AC | Statement (condensed from the story) | EPICS line |
|---|---|---|
| AC1 | Yocto machine for the prototype board added, dev image built; boots Linux 6.18 LTS PREEMPT_RT with the Epic 1 RT config on mainline drivers for audio, USB, PCIe and display; a check script lists each peripheral present or missing | 2012-2014 |
| AC2 | 7" 1024x600 panel and touch work; NVMe detected at PCIe Gen3 x4; USB-C enumerates in device role; USB-A host supplies >=900 mA; both MCU links (UART or SPI+IRQ) pass a frame loopback with the Story 3.1 frame library; MIDI UART present; tier ID strap and serial EEPROM readable | 2015-2018 |
| AC2b | No Wi-Fi or Bluetooth interface exists | 2019 |
| AC3 | Story 1.1 cyclictest load profile runs 1 h; max latency on isolated cores within the Story 1.1 threshold | 2021-2023 |

Story 1.1 threshold: max latency on every isolated core <=100 us (EPICS:788-815, AC2; full profile: cyclictest SCHED_FIFO one thread per isolated core at 1.333 ms interval, rtla timerlat, synthetic DSP load ~70% of period on the 3 isolated cores, fio O_DIRECT random 64 KiB reads on NVMe at >=400 MB/s, stress-ng on the rest). The kernel config fragment and boot args are committed under `tools/spikes/rt-latency/` per Story 1.1 (that directory does not exist in the repo yet: confirm it was produced before using it).

## 2. Preconditions and equipment

Entry gates (all must be true, otherwise stop):
1. Story 11.7 delivered: SoM vendor and part number, board files, tier-ID strap method (GPIO or EEPROM), serial label and EEPROM location (EPICS:2004, 2008; HWI:139-148, items 1-3). Record status: ____ (TBD from vendor docs).
2. Story 3.1 frame library available (`mcu/common/`, protocol 1.0, wire byte 0x10; MAP:1-10) so the loopback test has something to link.
3. Epic 1 RT kernel config and boot arguments available (Story 1.1 output).
4. Prototype board is an unfused unit (no secure-boot fuses programmed). This story does not require fusing; do not fuse in this runbook.

Equipment (generic categories; specific models TBD):
- Linux build host or ARM64 Linux VM on the Mac with the Yocto toolchain (Yocto 6.0 "Wrynose", SPINE:428-440).
- Prototype board plus the partner's board files (TBD from vendor docs); power supply per board documentation (TBD from vendor docs).
- Serial console access via the internal debug UART header (HWI:135 says internal header only; pinout TBD from vendor docs).
- 7" 1024x600 DSI panel with capacitive touch as fitted (part numbers TBD, HWI:144).
- NVMe M.2 2280 SSD, PCIe Gen3 x4 slot (HWI:121); candidate SSD choice is open (HWI:122), record model used.
- USB-C cable to a host computer; USB-A load able to draw >=900 mA (a current-measuring load or a bus-powered SSD plus meter; instrument TBD).
- Both MCUs (keybed and panel) fitted on the board, or the board's MCU links wired to something that answers frames for the loopback; MIDI DIN connectors or a UART loopback for the MIDI UART (31,250 baud, HWI:115-117).
- Multimeter, and an RF check method for "no radio" (interface listing is sufficient for AC2b).

## 3. Ordered steps

| # | Step | Expected observable result | AC |
|---|---|---|---|
| 1 | Record board revision, serial, SoM identifier, BSP version, and photograph the board | Entry recorded in the evidence table | all |
| 2 | Add the Yocto machine for the prototype from the partner's board files and build the dev image (Stories 1.4/1.5 flow; layout per Story 6.11 if already merged) | Build completes; image artifacts exist; license manifest generated (SPINE:455) | AC1 |
| 3 | Write the image to the boot medium by the SoM's documented method (TBD from vendor docs) | Board powers on, bootloader output on debug UART | AC1 |
| 4 | Boot; capture `uname -a`, kernel command line, and `/sys/kernel/realtime` | Kernel 6.18.x with PREEMPT_RT; `/sys/kernel/realtime` reads 1; isolcpus/nohz_full/rcu_nocbs present as in Story 1.1 | AC1 |
| 5 | Run the check script (to be written as part of this story) | One line per peripheral: audio, USB, PCIe, display, touch, NVMe, MCU links, MIDI UART, tier strap, EEPROM; each present or missing | AC1 |
| 6 | Display and touch: confirm picture at 1024x600 and touch events from the whole panel | Native 1024x600 reported; touches register; if DSI fails on mainline, note the internal-HDMI fallback exists (HWI:103) and log it as a deviation, not a pass | AC2 |
| 7 | NVMe: read link speed and width from the PCIe device | Gen3 x4 reported; NVMe detected | AC2 |
| 8 | USB-C: connect to a host computer | Enumerates in device role (peripheral role set in the device tree, HWI:110) | AC2 |
| 9 | USB-A: attach the load and draw current | Port supplies >=900 mA without dropout (HWI:113); record measured current and voltage | AC2 |
| 10 | MCU links: run the frame loopback with the Story 3.1 library on each link (keybed, panel) | Frames round-trip with CRC valid and no seq gaps; decoder counters (CRC errors, gaps) read 0; record link type per MCU (UART >=3 Mbaud or SPI+IRQ, HWI:43) | AC2 |
| 11 | MIDI UART: confirm device node present at 31,250 baud (loopback if the board offers one) | UART present; loopback bytes match | AC2 |
| 12 | Tier ID strap and serial EEPROM: read both | Strap value distinguishes v1 from Pro (AD-27, SPINE:351-356); serial EEPROM returns the label serial (HWI:137). Reading mechanism TBD from vendor docs | AC2 |
| 13 | List network interfaces and radios | No Wi-Fi or Bluetooth interface exists | AC2b |
| 14 | Run the Story 1.1 load profile for 1 h with cyclictest/rtla on the isolated cores | Max latency per isolated core <=100 us; keep histogram and any rtla trace >50 us | AC3 |
| 15 | Archive logs, check-script output and kernel config with the board serial | Evidence complete | all |

## 4. Risks and irreversible steps

- Nothing in this runbook is irreversible: the board must stay unfused. If anyone proposes programming OTP/fuses during bring-up, stop and use the gate in the 6.14-6.17 runbook instead.
- Electrical risk on first power-up of a new carrier board: follow the partner's power-up procedure (TBD from vendor docs); use a current-limited supply if the board documentation allows it.
- Gate "do on sacrificial unit first": not applicable to fusing here, but flashing a boot medium or SPI NOR with a wrong image can make an early prototype need recovery. Use the first prototype as the practice unit; do not run on the only unit if recovery access (maskrom or equivalent) is unknown. TBD from vendor docs.
- A failure at step 6 (DSI) or step 14 (RT latency) is a decision for the founder, not for the runner: Story 1.1 states the CM5 / Intel N-series fallback applies if Milestone-1 RT/display validation fails (EPICS:300, NFR38).

## 5. Entry / exit criteria mapped to ACs

Entry: section 2 gates 1-4 all ticked.
Exit (story-level, for the founder to judge): every row below PASS with evidence attached; any FAIL carries a written reason and is reported to the hardware partner (HWI:139-148). This document by itself never satisfies the story.

| AC | Exit evidence required |
|---|---|
| AC1 | Steps 2-5 outputs: image build log, `uname -a`, `/sys/kernel/realtime`, check-script output |
| AC2 | Steps 6-12 outputs, one per item |
| AC2b | Step 13 interface list |
| AC3 | Step 14 histograms and max per isolated core |

## 6. Evidence table (blank)

| Step | AC | Date | Operator | Board rev / serial | Result (PASS/FAIL/not present) | Measured value | File / capture reference | Notes |
|---|---|---|---|---|---|---|---|---|
| 1 | all | | | | | | | |
| 2 | AC1 | | | | | | | |
| 3 | AC1 | | | | | | | |
| 4 | AC1 | | | | | | | |
| 5 | AC1 | | | | | | | |
| 6 | AC2 | | | | | | | |
| 7 | AC2 | | | | | | | |
| 8 | AC2 | | | | | | | |
| 9 | AC2 | | | | | | | |
| 10 (keybed link) | AC2 | | | | | | | |
| 10 (panel link) | AC2 | | | | | | | |
| 11 | AC2 | | | | | | | |
| 12 (strap) | AC2 | | | | | | | |
| 12 (EEPROM) | AC2 | | | | | | | |
| 13 | AC2b | | | | | | | |
| 14 | AC3 | | | | | | | |
