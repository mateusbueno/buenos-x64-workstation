# DRAFT FOR FOUNDER — does not satisfy the listed stories' acceptance criteria

Runbook for Stories 3.3 (keybed MCU firmware), 3.4 (dual-bank bootloader and SoC reflash), 3.8 (panel MCU inputs) and 3.9 (panel MCU LEDs). Prepared 2026-10-19. Nothing here has been run on hardware. The MCU family, dev kit, bootloader approach, encoder/fader/LED part numbers and keybed contact count are not in the repo (HWI:139-148 items 2, 3, 8; SPINE:519 "Partners choose by month 7"); they are "TBD from vendor docs" below, and no flashing or debug commands are given for them.

Path shorthand (relative to `_bmad-output/`): EPICS = `planning-artifacts/epics.md`; HWI, SPINE, MAP as in the bring-up runbook (`planning-artifacts/architecture/architecture-buenos-x64-workstation-2026-10-17/`). Repo code: `mcu/common/` (Story 3.1 frame library, `ids.json`, `gen_ids.py`).

## 1. Acceptance criteria (source lines in EPICS)

Story 3.3 (EPICS:2025-2054; blocked by 11.6 and 11.7, EPICS:2035; runs on MCU dev kit + 73SW keybed harness, then prototype)
- 3.3-AC1 `mcu/keybed/` firmware scans every contact (2 or 3 per key) at >=10 kHz, timestamps each change with a free-running tick >=1 MHz, one `KEY_CONTACT` frame per change (key, contact, closed/open); MCU computes no velocity, holds no calibration (2039-2042)
- 3.3-AC2 At power-up on link-up the MCU sends `HELLO` (fw version, protocol version, board ID, keybed contact count) and answers every `TIME_SYNC` with its tick (2044-2046)
- 3.3-AC3 Logic analyser on a contact line and the link: 10,000 contact changes incl. all keys at once; contact change to frame complete on the wire <=200 us for every event; seq has no gaps (2048-2050)
- 3.3-AC4 Review shows no duty other than key scanning (2052-2054)

Story 3.4 (EPICS:2056-2085; blocked by 11.7, EPICS:2066; runs on MCU dev kit + dev board over UART, then prototype)
- 3.4-AC1 `mcu/bootloader/`, write-protected after factory programming; starts the newest bank passing its integrity check, falls back to the other bank otherwise (2070-2072)
- 3.4-AC2 `sysd` flashing tool: reads `HELLO` version; if it differs from the packaged firmware, sends `ENTER_BOOTLOADER` (or uses BOOT/RESET GPIO pair), writes the inactive bank, verifies, marks active, resets; versions match means no action (2074-2077)
- 3.4-AC3 50 power cuts at random points during flashing on the dev kit: MCU boots old or new firmware every time and the next flash succeeds (2079-2081)
- 3.4-AC4 Production configuration: no external connector can flash it, SWD/JTAG disabled per production option bytes (2083-2085)

Story 3.8 (EPICS:2175-2203; blocked by 11.7 and 11.8, EPICS:2185)
- 3.8-AC1 `mcu/panel/` encoder turn sends `CONTROL` with encoder ID and signed relative step delta; no missed or reversed steps at the encoder's rated maximum speed (2189-2191)
- 3.8-AC2 9 faders, 2 wheels, 3 pedal inputs: sampled >=10 bits, filtered, reported on change at >=500 Hz with a deadband suppressing idle jitter (2193-2195)
- 3.8-AC3 Every button incl. VALUE push: debounced `BUTTON` frame (ID, pressed/released) per edge; no double events in a 5,000-press bounce test (2197-2199)
- 3.8-AC4 Power-up with pedals plugged in: polarity (NO/NC, and continuous pedal direction) detected and reported in `HELLO` or a `CONTROL` status message (2201-2203)

Story 3.9 (EPICS:2205-2233; blocked by 11.7 and 11.8, EPICS:2215)
- 3.9-AC1 `LED_SET` sets the addressed LED (toggle state LEDs, part/scene LED strips, encoder ring segments); MCU never changes any LED on its own, all LEDs off at boot until the first command (2219-2221)
- 3.9-AC2 Strong, weak (~15% PWM) or off per LED and global brightness Low/Mid/High; no visible flicker (PWM >=20 kHz or equivalent) (2223-2225)
- 3.9-AC3 Encoder ring value arc: lit segments show the arc, the rest fully off, never weak-lit (2227-2229)
- 3.9-AC4 Prototype panel under a 50 klx lamp: LED on:off contrast measured at High brightness, recorded against the >=10:1 target, passed to the hardware partner if it fails (2231-2233)

Spec points these rely on: keybed table HWI:33-44; panel table HWI:46-58; frame format HWI:60-84 and MAP:11-46 (sync 0xA5, version byte 0x10, little-endian, CRC-16/CCITT-FALSE poly 0x1021 init 0xFFFF check 0x29B1 for "123456789", MAP:28-31); bootloader rules HWI:86-90; AD-16 SPINE:238-246.

Known wording differences to resolve before the tests, not decided here: story 3.8-AC2 says >=10 bits while MAP:73-99 lists faders, wheels and pedals as 12-bit with range 0..4095 (an MCU with a coarser ADC shifts left, MAP:73); the 3.8 test should use the MAP range as the reference and state which was used. Encoder step range in MAP is -127..127, 8 bits.

## 2. Preconditions and equipment

Entry gates (stop if any is false):
1. Story 11.7 delivered: MCU family and bootloader approach for both MCUs (HWI:143 item 3). Status: ____. Until then the founder's candidate is the ST Nucleo-G474RE (STM32G4), see hardware-platform.md; the steps below use it as the dev kit but nothing may be frozen before 11.7.
2. Story 11.6 delivered: 73SW contact count (2 or 3) and aftertouch presence (HWI:142 item 2; EPICS:2035). Status: ____
3. Story 11.8 delivered (panel stories): panel, display, LED and legend specs; encoder and fader part numbers; encoder rated maximum speed (EPICS:2185, 2191). Rated speed value: ____ (TBD from vendor docs).
4. Story 3.1 library and ID maps built and passing host tests (`mcu/common/`); `MCU-ID-MAP.md` version 1.0, source hash `4e81bc5d...5c99c665` (MAP:7) matches the firmware build.
5. For 3.4 and later: Story 3.2 (bring-up) done so the dev board / prototype provides the SoC end of the link, or a dev board with UART link (EPICS:2064).

Equipment (categories; models TBD):
- Candidate MCU dev kit: ST Nucleo-G474RE (STM32G4). Its on-board ST-LINK is the programmer and debug probe (USB cable, drag-and-drop or STM32CubeProgrammer). Two kits are needed, one for the keybed and one for the panel. Exact option bytes, bootloader memory map and dual-bank layout: TBD from ST reference manual RM0440.
- 73SW keybed with harness (partner stock; EPICS:2033) and its connector adapter to the dev kit (TBD).
- Panel harness: encoders, 9 faders, 2 wheels, 3 pedal inputs with real pedals of each type (sustain/half-pedal, expression, switch; HWI:55), buttons, LEDs (TBD).
- Logic analyser or scope with enough channels to capture a contact line and the link at once; sample rate must resolve well below 200 us (instrument TBD).
- Adjustable programmable power supply or relay to cut power at random instants (for 3.4; method TBD); random delay generator script.
- Lux-calibrated lamp able to reach 50 klx, and a light meter/camera method for on:off contrast (EPICS:2231; measurement method TBD, HWI:145).
- Mechanical or motorized means to turn an encoder at its rated max speed (TBD).
- SoC side: dev board or prototype running the frame library, with `sysd` flashing tool when 3.4 AC2 is tested.

## 3. Ordered steps

### 3.3 Keybed MCU firmware

| # | Step | Expected observable result | AC |
|---|---|---|---|
| 1 | Connect the 73SW harness to the dev kit; record contact count per key | Contact count 2 or 3 noted and equals the `keybed_contacts` the firmware will report (MAP:37) | 3.3-AC1 |
| 2 | Build `mcu/keybed/` from the Story 3.1 library; flash the dev kit by the vendor method (TBD) | Firmware runs; link up to a host/SoC reader | 3.3-AC1 |
| 3 | Measure scan period on a spare GPIO toggle or equivalent firmware-exposed signal | Each contact sampled at >=10 kHz (period <=100 us) | 3.3-AC1 |
| 4 | Check tick source frequency | Free-running tick >=1 MHz; value in `tick` field advances accordingly | 3.3-AC1 |
| 5 | Press and release individual keys, each contact in turn | One `KEY_CONTACT` per change with correct key index (0 = lowest), contact index (in closing order), state 1/0 (MAP:38) | 3.3-AC1 |
| 6 | Review source: no velocity computation, no calibration tables | Reviewer note recorded | 3.3-AC1, AC4 |
| 7 | Reset the MCU and bring the link up; capture bytes | `HELLO` arrives with fw version, protocol version 0x10, board_id 1, contact count (MAP:37) | 3.3-AC2 |
| 8 | SoC sends `TIME_SYNC` requests (kind 0, token); capture replies | Each request gets a reply echoing the token with the current tick (MAP:43) | 3.3-AC2 |
| 9 | Attach logic analyser to one contact line and the link TX; run 10,000 contact changes including all keys pressed at once | For every event: contact edge to last bit of the frame <=200 us; analyser export retained | 3.3-AC3 |
| 10 | Decode the captured link stream with the Story 3.1 decoder | seq has no gaps; CRC errors 0 | 3.3-AC3 |
| 11 | Repeat steps 2-10 on the prototype board once Story 3.2 is done | Same results on the real board | 3.3 (runs-on) |
| 12 | Review firmware for any task other than key scanning (aftertouch is a separate story) | None found | 3.3-AC4 |

### 3.4 Bootloader and reflash

| # | Step | Expected observable result | AC |
|---|---|---|---|
| 13 | Define with the partner how the bootloader approach for the chosen family maps to two banks, integrity check and write protection (TBD from vendor docs) | Written note attached | 3.4-AC1 |
| 14 | Program bootloader plus firmware in bank A on a dev kit; boot | Bootloader starts bank A | 3.4-AC1 |
| 15 | Flash a newer valid image to bank B; boot | Newest bank passing integrity check starts (B) | 3.4-AC1 |
| 16 | Corrupt bank B (flip a byte) and reset | Bootloader falls back to bank A | 3.4-AC1 |
| 17 | From the SoC, run `sysd` flashing tool with a packaged image whose version differs from the `HELLO` version | Tool sends `ENTER_BOOTLOADER` (magic 0x544F4F42, MAP:44) or toggles BOOT/RESET GPIOs, writes inactive bank, verifies, marks active, resets MCU; new `HELLO` shows new version | 3.4-AC2 |
| 18 | Run the tool again with matching versions | Does nothing; no bootloader entry | 3.4-AC2 |
| 19 | Power-cut loop: 50 cuts at random points during flashing (random delay after flash start) on the dev kit; after each, restore power and record which firmware version boots, then re-run the flash | All 50: MCU boots old or new firmware; the next flash attempt succeeds | 3.4-AC3 |
| 20 | Production-option check: **irreversible, see gate**. Set the production option bytes (write protect, SWD/JTAG disable) on a sacrificial MCU only; then try external flashing/debug connection | No external connector can flash; SWD/JTAG probe cannot attach (TBD from vendor docs for the family's option bytes) | 3.4-AC4 |

### 3.8 Panel inputs

| # | Step | Expected observable result | AC |
|---|---|---|---|
| 21 | Build `mcu/panel/`; flash dev kit wired to panel harness; link up | `HELLO` with board_id 2, contact count 0 (MAP:37) | 3.8 |
| 22 | Turn each of the 8 encoders and the VALUE encoder slowly, then at rated max speed (value TBD) for a counted number of detents | `CONTROL` frames with control IDs 1-9 and signed deltas whose sum equals the detents turned; none missed or reversed (MAP:77-85) | 3.8-AC1 |
| 23 | Move each of 9 faders, 2 wheels, 3 pedal inputs through full travel | Values follow position (MAP range 0..4095, pitch wheel rests at 2048, MAP:73, 95); log resolution actually achieved | 3.8-AC2 |
| 24 | Measure report rate while moving a fader quickly | Updates >=500 Hz when changing (from timestamps in `tick`) | 3.8-AC2 |
| 25 | Leave all analog controls idle for a fixed period (duration TBD by the team) | No frames from idle jitter (deadband works) | 3.8-AC2 |
| 26 | Press and release each button, including VALUE push (ID 23, MAP:124) | One `BUTTON` per edge with correct ID and state | 3.8-AC3 |
| 27 | Run the 5,000-press bounce test (actuation method TBD) | Count of events equals 5,000 presses + 5,000 releases; zero doubles | 3.8-AC3 |
| 28 | Power up with each pedal type plugged in, with both polarities if the pedals allow | Polarity (NO/NC) and continuous direction reported in `HELLO` or `CONTROL` status; where the report is carried is not fixed in MAP (MAP:99 states `PEDAL_SWITCH` 0 open / 1 closed after polarity detect) and must be decided and documented | 3.8-AC4 |
| 29 | Repeat on the prototype board | Same results | 3.8 (runs-on) |

### 3.9 Panel LEDs

| # | Step | Expected observable result | AC |
|---|---|---|---|
| 30 | Power up with LED harness and no `LED_SET` sent | All LEDs off; none lights on its own | 3.9-AC1 |
| 31 | Send `LED_SET` for each LED ID in MAP section 8 (8 part/scene, 8 rings, 7 toggle LEDs; MAP:139-161) | Only the addressed LED responds, at the commanded brightness | 3.9-AC1 |
| 32 | Send strong, weak (~15% PWM) and off; set global user brightness Low, Mid, High (scheme owned by the SoC) | Three distinct levels; global levels scale them | 3.9-AC2 |
| 33 | Measure PWM frequency on an LED line; view with a high-speed camera or scope-based flicker check | PWM >=20 kHz or equivalent; no visible flicker | 3.9-AC2 |
| 34 | Send a value arc to an encoder ring (fill position 0..255 in `arg`, MAP:42) | Lit segments follow the arc; the rest are fully off, never weak-lit | 3.9-AC3 |
| 35 | Place the prototype panel under the 50 klx lamp, High brightness; measure LED on and off luminance | on:off ratio recorded against >=10:1 (HWI:54); if lower, record and hand to the hardware partner | 3.9-AC4 |

## 4. Risks and irreversible steps

- IRREVERSIBLE: production option bytes, bootloader write protection and SWD/JTAG disable on an MCU (3.4 step 20, EPICS:2070, 2085). A mistake may lock you out of the part. Gate: do this first on a sacrificial MCU or dev kit, confirm the recovery story and that the bootloader and banks still work afterwards, only then on any prototype MCU. Never on the only copy of a prototype.
- Bootloader is immutable by design: a bad bootloader image cannot be fixed in the field (HWI:88-90). Test bootloader builds on dev kits only until 3.4 steps 14-19 pass.
- 3.4 step 19 cuts power while flashing; use a dev kit, not a prototype board with a sealed NVMe or filesystem at risk. Cutting only the MCU's supply is the intent; cutting the whole board is a different risk (data partition) and is not part of this AC.
- 3.3 AC3 measures an electrical edge to wire time; ground and probe loading can corrupt the measurement. Record probe setup.
- Dependencies 11.6 / 11.7 / 11.8 may slip (target months 7 and 9, EPICS:2035, 2066, 2185): do not guess contact counts, encoder rated speeds or LED parts to proceed.

## 5. Entry / exit criteria mapped to ACs

Entry: section 2 gates 1-5 (stories 3.3 needs 1, 2, 4; 3.4 needs 1, 4, 5; 3.8 and 3.9 need 1, 3, 4).
Exit per story (for the founder to judge): all rows of that story's AC mapping PASS with evidence, or FAIL with a written note. Production-configuration evidence (3.4-AC4) comes only from a sacrificial unit.

| AC | Steps | Evidence needed |
|---|---|---|
| 3.3-AC1 | 1-6 | Scan period, tick rate, per-contact frames |
| 3.3-AC2 | 7-8 | `HELLO` capture, `TIME_SYNC` replies |
| 3.3-AC3 | 9-11 | Analyser export of 10,000 events, decoder gap count |
| 3.3-AC4 | 6, 12 | Review note |
| 3.4-AC1 | 13-16 | Boot trace per case |
| 3.4-AC2 | 17-18 | `sysd` log, `HELLO` before/after |
| 3.4-AC3 | 19 | 50-row table |
| 3.4-AC4 | 20 | Option-byte readout, failed attach/flash attempts |
| 3.8-AC1 | 22 | Delta sums per encoder, speed used |
| 3.8-AC2 | 23-25 | Resolution, report rate, idle trace |
| 3.8-AC3 | 26-27 | Event counts |
| 3.8-AC4 | 28 | Polarity reports |
| 3.9-AC1 | 30-31 | Boot-off photo, per-LED response |
| 3.9-AC2 | 32-33 | Levels, PWM frequency |
| 3.9-AC3 | 34 | Ring photos |
| 3.9-AC4 | 35 | Luminance numbers, ratio, partner handoff note |

## 6. Evidence table (blank)

| Step | AC | Date | Operator | Unit / dev kit ID | Firmware version / commit | Result | Measured value | File / capture reference | Notes |
|---|---|---|---|---|---|---|---|---|---|
| 3 | 3.3-AC1 | | | | | | | | |
| 4 | 3.3-AC1 | | | | | | | | |
| 5 | 3.3-AC1 | | | | | | | | |
| 7 | 3.3-AC2 | | | | | | | | |
| 8 | 3.3-AC2 | | | | | | | | |
| 9 | 3.3-AC3 | | | | | | max latency (us): | | |
| 10 | 3.3-AC3 | | | | | | gaps: | | |
| 12 | 3.3-AC4 | | | | | | | | |
| 15-16 | 3.4-AC1 | | | | | | | | |
| 17-18 | 3.4-AC2 | | | | | | | | |
| 19 (cuts 1-50) | 3.4-AC3 | | | | | | n boot old / new / fail: | | |
| 20 | 3.4-AC4 | | | | | | | | |
| 22 | 3.8-AC1 | | | | | | | | |
| 23-25 | 3.8-AC2 | | | | | | | | |
| 26-27 | 3.8-AC3 | | | | | | | | |
| 28 | 3.8-AC4 | | | | | | | | |
| 30-31 | 3.9-AC1 | | | | | | | | |
| 32-33 | 3.9-AC2 | | | | | | | | |
| 34 | 3.9-AC3 | | | | | | | | |
| 35 | 3.9-AC4 | | | | | | on:off ratio: | | |

## Founder decisions (2026-10-19)

- **Resolution (3.8-AC2):** use 12 bits, the MCU-ID-MAP contract; it satisfies the story's >=10 bits. Update the story wording to match when convenient.
- **Pedal polarity (3.8-AC4):** reported in `HELLO` (hardware property, rarely changes), not in `CONTROL`. Step 28 should expect polarity in `HELLO` only; the protocol spec needs a field for it.

## Candidate kit and who does what (2026-10-19)

- Firmware is written in this repository (`mcu/keybed/`, `mcu/panel/`, Stories 3.1, 3.3, 3.4, 3.8, 3.9). The person helping with the assembly does the physical work only: harness wiring, continuity checks with a multimeter, plugging in the USB cable and copying the firmware file with STM32CubeProgrammer.
- Before any firmware step the assembler runs the continuity checklist: every contact line of the keybed harness to its dev-kit pin, ground common, no shorts to 3.3 V or 5 V. Record results in the evidence table.
- First MCU bring-up (steps 2, 7) should be done together with someone who has flashed an MCU before, in person or on a call. Irreversible option-byte steps stay behind the sacrificial-unit gate.
