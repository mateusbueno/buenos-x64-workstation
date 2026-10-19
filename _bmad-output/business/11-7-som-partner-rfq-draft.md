# DRAFT FOR FOUNDER — does not satisfy Story 11.7 acceptance criteria

Preparation draft for Story 11.7 (SoM longevity commitment and board-level partner deliverables). Prepared 2026-10-19. Nothing here is a supplier statement. No SoM vendor, part number, price, MOQ or lead time is named, because the repo names none for production (only a dev board example). All such fields are blank.

Path shorthand used below:
- EPICS = `_bmad-output/planning-artifacts/epics.md`
- HWP = `_bmad-output/specs/spec-arm64-stage-workstation/hardware-platform.md`
- SPEC = `_bmad-output/specs/spec-arm64-stage-workstation/SPEC.md`
- HWI = `_bmad-output/planning-artifacts/architecture/architecture-buenos-x64-workstation-2026-10-17/HARDWARE-INTERFACES.md`
- SPINE = `_bmad-output/planning-artifacts/architecture/architecture-buenos-x64-workstation-2026-10-17/ARCHITECTURE-SPINE.md`

## 1. Acceptance Criteria (source: EPICS:6305-6325)

Covers EXT10 (items 1, 3; tier-ID strap, serial label and EEPROM, factory programming fixture, codec-loopback test points), NFR38, NFR41 (EPICS:6311). Runs on: business (EPICS:6313).

- AC1 Given the requirement for an industrial RK3588 SoM (16 GB v1, 32 GB Pro, pin-compatible, no radio), when the SoM is selected with the hardware partner, then the vendor, part numbers (v1 and Pro) and a signed letter committing to 7-10 years of supply are stored (EPICS:6317-6319).
- AC1b And the record confirms the SoM variant has no Wi-Fi/Bluetooth and the BSP supports mainline Linux 6.18 LTS for audio, USB, PCIe and display paths (HW-§2), or lists exceptions for the software team (EPICS:6320).
- AC2 Given the carrier board design, when the partner delivers the board-level decisions, then a document names: the MCU family and bootloader approach for both MCUs (due by month 7), the tier-ID strap (GPIO or EEPROM) and how it reads v1 vs Pro, the serial number label and EEPROM location, the factory programming fixture plan for secure-boot fusing, and the codec-loopback test points or fixture (EPICS:6322-6324).
- AC2b And each item is marked delivered / pending with a date, and pending items past their due date are escalated (EPICS:6325).

## 2. What the repo already says (evidence)

| # | Fact | Source (file:line) |
|---|------|--------------------|
| 1 | RK3588 is the chosen SoC; one SoC covers v1 with 16 GB and Pro with 32 GB | HWP:3-6 |
| 2 | Milestone 1 runs on a dev board (e.g. ROCK 5B+ 16 GB); production uses an industrial SoM with a written 7-10 year longevity commitment, never a hobby board. (ROCK 5B+ is a dev-board example only, not a SoM choice) | HWP:7 |
| 3 | Industrial SoM, written longevity commitment 7-10 years, 16 GB v1 / 32 GB Pro, pin-compatible | HWI:27; SPEC:103; EPICS:300 (NFR38) |
| 4 | No radio: no Wi-Fi or Bluetooth on the SoM variant, no RF parts on the carrier; avoids Anatel homologation | HWI:28; EPICS:303 (NFR41); HWP:64 |
| 5 | Tier-ID strap (GPIO or EEPROM) readable at boot, distinguishing v1 from Pro | HWI:29; EPICS:357 (AR24, tier profile read from board ID / OTP at boot) |
| 6 | SoM BSP must support mainline Linux 6.18 LTS; vendor-kernel-only parts must not be the audio, USB, PCIe or display paths | HWI:30 |
| 7 | Thermal: 24 h at full audio load (3 A76 cores at 100%) without throttling, closed chassis, 40 C ambient | HWI:31 |
| 8 | Milestone 1 risks: PREEMPT_RT on mainline (only data point is a vendor test on 6.1); MIPI DSI on mainline; fallback CM5 / Intel N-series only if RT/display validation fails | HWP:8-11, HWP:13-18; EPICS:300 |
| 9 | Audio: RK3588 I2S/TDM to TI PCM3168A codec via mainline `rockchip-i2s-tdm` and `pcm3168a`; codec is clock master on its own crystal (24.576 MHz) | HWP:50-51; EPICS:302 (NFR40) |
| 10 | Two MCUs: keybed MCU and panel MCU | SPINE:243 |
| 11 | MCU family and bootloader approach chosen by partners by month 7 | EPICS:401 (AR47); HWI:143 (item 3) |
| 12 | Each MCU needs an immutable bootloader with two firmware banks; a power cut while flashing must leave the old bank bootable | HWI:88 |
| 13 | SoC must put each MCU in bootloader mode and flash it over the same link, or over a dedicated BOOT/RESET GPIO pair | HWI:89 |
| 14 | Secure boot: the factory station fuses the RK3588 OTP secure-boot keys; plan a programming fixture | HWI:133; HWP:11 |
| 15 | JTAG/SWD disabled or fused off on production SoC and MCUs; debug UART internal header only | HWI:134-135 |
| 16 | Factory test image tests every port, both MCUs, keybed contacts, codec loopback (output to line in), display and touch, NVMe; provide test points or a fixture for codec loopback | HWI:136; EPICS:330 (AR3) |
| 17 | Device registry: each unit's serial is linked to its SoM ID; provide a readable serial number (label and EEPROM) | HWI:137 |
| 18 | Power-fail: supercapacitor hold-up >=150 ms; `POWER_FAIL` GPIO asserted to the SoC >=100 ms before rails drop; provide measured timing | HWI:127-128; SPINE:263 |
| 19 | Partner return list items: 1 SoM vendor/part/longevity; 3 MCU family and bootloader; 5 POWER_FAIL timing and hold-up; 6 codec reference design measurements (latency, noise); 7 thermal test | HWI:141-147 (section 14) |
| 20 | EXT10 definition (full list including tier-ID strap, serial label + EEPROM, factory programming fixture) | EPICS:416 |
| 21 | Related stories that depend on these answers: board bring-up (SoM part number, board files, tier-ID strap, serial EEPROM), bootloader (MCU family) | EPICS:2004 (Covers line); EPICS:2062 (Covers line); EPICS:2327 (codec, Covers line) |

Not found in the repo (leave blank, do not assume): any SoM vendor name or module part number, any price, MOQ, lead time, longevity letter wording from a vendor, MCU family choice (examples only: "e.g. STM32 or RP2350", HWP:32), tier-ID mechanism choice, EEPROM part or location, fixture design, codec-loopback test point locations.

## 3. Request-for-quotation / commitment letter template

> To: [SoM vendor — NAME / ADDRESS TO BE FILLED, selected with the hardware partner]
> Cc: [Hardware partner contact]
> From: [Founder name, company, address, tax ID]
> Date: [____]
> Subject: Supply and longevity commitment request — industrial Rockchip RK3588 SoM, 16 GB and 32 GB variants
>
> Dear [____],
>
> We are developing a 73-key stage workstation instrument based on the Rockchip RK3588. Production will use an industrial SoM. We need a written commitment before we select a module. Please answer each point in writing:
>
> A. Module and variants
> 1. Module name and part number for the 16 GB variant. [____]
> 2. Module name and part number for the 32 GB variant. [____]
> 3. Are the 16 GB and 32 GB variants pin-compatible (same connector, same pinout, same carrier board)? List any differences. [____]
> 4. Confirm that the variants we would buy carry no Wi-Fi or Bluetooth radio (no radio populated, no radio on module). [____]
>
> B. Longevity
> 5. A signed letter committing to supply of both part numbers for 7 to 10 years from [date]. State the exact end-of-supply date for each. [____]
> 6. Policy and notice period for end-of-life and last-time-buy. [____]
> 7. Policy for silicon revision or component changes (change notification, requalification). [____]
> 8. Does the commitment depend on Rockchip's own supply commitment for the RK3588? Please state the upstream term. [____]
>
> C. Software
> 9. BSP status for mainline Linux 6.18 LTS for each of: audio (I2S/TDM), USB (including device mode), PCIe (NVMe), display (MIPI DSI and HDMI). For each, state mainline / vendor-kernel-only / partial. [____]
> 10. List any exception that needs a vendor kernel. [____]
> 11. Documentation and schematic access for the carrier board design, and under what terms (NDA, fee). [____]
>
> D. Commercial
> 12. Unit price at [quantity bands to be filled], MOQ, lead time, payment terms, warranty. [____]
>
> E. Factory
> 13. Support for secure-boot key fusing (RK3588 OTP) in a factory fixture: tooling, documentation. [____]
> 14. SoM identifier readable by software (unique ID) for serial-to-SoM registry. [____]
>
> Please reply by [date]. This letter is a request for information, quotation and commitment and creates no purchase obligation.
>
> Sincerely, [name, title, contact]

### Companion request to the hardware partner (carrier board decisions)

> To: [Hardware partner]. Please deliver a board-level decisions document, with each item marked delivered or pending and a date, covering:
> 1. MCU family and bootloader approach for the keybed MCU and the panel MCU (due by month 7). [____]
> 2. Tier-ID strap: GPIO or EEPROM, and how software reads v1 vs Pro at boot. [____]
> 3. Serial number label and EEPROM location. [____]
> 4. Factory programming fixture plan for secure-boot fusing. [____]
> 5. Codec-loopback test points or fixture (output to line in). [____]
> Reference requirements: HWI sections 2, 7 and 13 (HWI:27-31, HWI:88-89, HWI:133-137).

## 4. Question list mapped one-to-one to the Acceptance Criteria

| AC | Question | Answer (blank) | Letter item |
|----|----------|----------------|-------------|
| AC1 | Which vendor? | | To / section A |
| AC1 | Part number, 16 GB (v1)? | | A1 |
| AC1 | Part number, 32 GB (Pro)? | | A2 |
| AC1 | Pin-compatible between the two? | | A3 |
| AC1 | Signed 7-10 year supply letter stored? End-of-supply dates? | | B5 |
| AC1b | Variant has no Wi-Fi / Bluetooth? | | A4 |
| AC1b | Mainline 6.18 LTS BSP: audio | | C9 |
| AC1b | Mainline 6.18 LTS BSP: USB | | C9 |
| AC1b | Mainline 6.18 LTS BSP: PCIe | | C9 |
| AC1b | Mainline 6.18 LTS BSP: display | | C9 |
| AC1b | Exceptions listed for the software team? | | C10 |
| AC2 | MCU family (both MCUs) | | Companion 1 |
| AC2 | Bootloader approach (both MCUs) and due date (month 7) | | Companion 1 |
| AC2 | Tier-ID strap: GPIO or EEPROM; how v1 vs Pro is read | | Companion 2 |
| AC2 | Serial number label and EEPROM location | | Companion 3 |
| AC2 | Factory programming fixture plan for secure-boot fusing | | Companion 4 (and D/E 13) |
| AC2 | Codec-loopback test points or fixture | | Companion 5 |
| AC2b | Each item marked delivered / pending with date; escalation of overdue items | | Tracking table |

## 5. Required written commitments (blank)

| # | Commitment | Source document | Date | Signed by | Stored at | Done |
|---|------------|-----------------|------|-----------|-----------|------|
| 1 | SoM vendor name | | | | | |
| 2 | Part number v1 (16 GB) | | | | | |
| 3 | Part number Pro (32 GB) | | | | | |
| 4 | Pin-compatibility statement v1/Pro | | | | | |
| 5 | Signed 7-10 year supply letter (end dates) | | | | | |
| 6 | End-of-life / last-time-buy notice policy | | | | | |
| 7 | No-radio confirmation for the variants | | | | | |
| 8 | Mainline 6.18 LTS support statement: audio / USB / PCIe / display, plus exceptions | | | | | |
| 9 | MCU family and bootloader approach, both MCUs | | | | | |
| 10 | Tier-ID strap decision and v1/Pro read method | | | | | |
| 11 | Serial label and EEPROM location | | | | | |
| 12 | Factory programming fixture plan (secure-boot fusing) | | | | | |
| 13 | Codec-loopback test points or fixture | | | | | |

## 6. Tracking table (delivered / pending, escalation)

| # | Item | Owner | Due | Status (delivered / pending) | Date delivered | Escalated (date) | Notes |
|---|------|-------|-----|------------------------------|----------------|------------------|-------|
| 1 | SoM vendor selected | Founder + hardware partner | | | | | |
| 2 | RFQ letter sent to SoM vendor | Founder | | | | | |
| 3 | Part numbers v1 and Pro | SoM vendor | | | | | |
| 4 | Signed 7-10 year letter | SoM vendor | | | | | |
| 5 | No-radio confirmation | SoM vendor | | | | | |
| 6 | Mainline 6.18 LTS BSP statement and exceptions | SoM vendor | | | | | |
| 7 | MCU family and bootloader approach | Hardware partner | month 7 (EPICS:401) | | | | |
| 8 | Tier-ID strap | Hardware partner | | | | | |
| 9 | Serial label and EEPROM location | Hardware partner | | | | | |
| 10 | Factory programming fixture plan | Hardware partner | | | | | |
| 11 | Codec-loopback test points / fixture | Hardware partner | | | | | |
| 12 | Hand exceptions and decisions to software team | Founder | | | | | |
| 13 | Confirm Story 11.7 criteria against stored documents | Founder | | | | | |
