# Hardware Platform

## SoC: Rockchip RK3588 (chosen)

- 4× Cortex-A76 + 4× Cortex-A55, up to 32 GB of RAM, PCIe Gen3 x4 for NVMe.
- One SoC covers both tiers: v1 with 16 GB and Pro with 32 GB.
- Milestone 1 runs on a dev board (e.g. ROCK 5B+ 16 GB). Production uses an industrial SoM with a written 7–10 year longevity commitment, never a hobby board.
- Milestone 1 validates two risks first:
  1. PREEMPT_RT on mainline with real audio. The only data point so far is a vendor test on a 6.1 kernel.
  2. A MIPI DSI panel on mainline. HDMI landed around 6.13. If DSI fails, the fallback is an internal HDMI panel.
- Secure boot plus OTP/OP-TEE key storage are required by `library-format.md`.

## Fallbacks (only if RT validation fails)

| Option | Why not first choice |
|---|---|
| Raspberry Pi CM5 | PCIe Gen2 x1 (~350–400 MB/s measured) is marginal at the 32f/96k worst case, and its 16 GB maximum cannot cover Pro |
| Intel N150/N355 (x86_64) | Mature PREEMPT_RT and lowest technical risk, but it gives up the ARM64 thesis and draws more power |

## Rejected SoCs

| SoC | Reason |
|---|---|
| NXP i.MX 95 / 8M Plus | A55/A53-only CPU, too weak for 256 voices plus organ |
| CIX P1 (CD8180) | Mainline is immature, power draw is high, few SoMs. Watch it for a later Pro |
| MediaTek Genio 720 | Only 2 big cores and a 16 GB maximum |
| Qualcomm QCS6490 | Closed ecosystem, NDA documentation, heavy BSP |

## Keybed

- Fatar 73SW (semi-weighted), from the partner's stock of ~100 units. Assumed waterfall profile, E to E: E1–E7 with C4 = MIDI 60, 43 white and 30 black keys.
- Key geometry (23.6 mm white key pitch, 1013.4 mm for the 73 keys) comes from Fatar's 61 TP/9 drawing until the 73SW drawing arrives. Full dimensions are in `mechanical.md`.
- A dedicated scan MCU (e.g. STM32 or RP2350) scans at ≥10 kHz. It derives attack and release velocity from contact timing, reads aftertouch if present, and sends timestamped events to the SoC over UART/SPI in <1 ms.
- Candidate MCU dev kit for both MCUs (founder decision 2026-10-19, not final until the partners confirm by month 7, Story 11.7): ST Nucleo-G474RE (STM32G4; many ADC channels for the panel, native dual-bank flash, on-board ST-LINK, ST longevity programme). ESP32 is excluded because v1 has no radio (NFR41). RP2350 is the fallback for the keybed only, since its ADC count would force external multiplexers on the panel.
- The keybed model is a calibration table, so a different Fatar model needs no code change.
- Resupply beyond the first ~100 units needs Fatar OEM terms (open question).

## Control surface and display (UI option A, refined by UX)

Layout and legends are in the adopted `DESIGN.md` and `EXPERIENCE.md`. Legends are English only. Control sizes and proposed positions are in `mechanical.md`.

- **Faders:** 9 dual-function faders: drawbars in DRAWBARS mode, parts 1–8 plus REV in MIXER mode. Non-motorized in v1, with pickup. Motorized faders are Pro only. 45 mm travel, 20 mm between centres.
- **Encoders:** 8 endless encoders with LED rings, Ø20 mm knobs, 32 mm between centres. Fixed functions are DELAY, BRIGHTNESS, ATTACK, RELEASE, LOW, MID, MID FREQ, HIGH. In ASSIGN they follow the Live Set's map. A VALUE encoder scrolls lists and changes values; it has a push switch (push-click), which on Performance opens the Part surface of the knob-target part. The VALUE knob is Ø35 mm and knurled.
- **MASTER:** an analog level potentiometer in the output path with a Ø20 mm knob. The panel MCU does not read it.
- **Toggle buttons:** 3, each with one LED per state: DRAWBARS/MIXER, PARTS/SCENES, and PART/ALL/ASSIGN (3 states).
- **Button and LED size:** buttons are 14 × 9 mm, and LEDs are Ø3 mm diffused. Knobs and fader caps are machined anodized aluminum.
- **Buttons:** 8 part/scene buttons, LIVE SET ◀ ▶, SCENE ◀ ▶, EXIT · MENU · WRITE (MENU opens the Launcher), ROTARY STOP and S/F (slow/fast). An optional Panel guard setting (default Off) makes LIVE SET ◀ ▶ act only after a 300 ms hold; the long-press threshold is a user setting (400 / 500 / 800 ms, default 500 ms).
- **Pro tier (16 parts):** two banks of 8 (A/B) toggled by a bank button; part buttons and faders act on the active bank. Details are deferred to Pro.
- **Wheels and footswitch:** pitch and mod wheels in an 80 mm block left of the keybed. The footswitch advances the scene by default, and can be set to previous scene, next Live Set or rotary speed.
- **Display:** 7-inch capacitive touchscreen, native 1024×600 landscape. Luminance, glass, LED and legend floors are in `HARDWARE-INTERFACES.md`.

## Audio path

- RK3588 I2S/TDM feeds a TI PCM3168A codec through the mainline ASoC drivers (`rockchip-i2s-tdm`, `pcm3168a`), plus a board machine driver.
- The codec is the clock master, with its own 48 kHz-family crystal. The SoC PLL must not clock the audio.
- USB-C runs in device mode as a UAC2 audio gadget plus a MIDI gadget. Nord is the only major brand without a USB audio interface.
- An internal USB audio chip (e.g. XMOS) was rejected, because it puts the xHCI stack in the critical latency path.

## v1 I/O

- 4 balanced ¼" TRS outputs: 2 main and 2 aux.
- Headphone output with its own amplifier, on a ¼" TRS jack on the front face under the wheel block.
- 1 stereo line input: 2 × ¼" (L/R) plus 1 × 3.5 mm on the rear panel.
- Rear panel order, seen from behind, left to right: DC IN and POWER, USB-A, USB-C, MIDI OUT, MIDI IN, the 3 pedal jacks, LINE IN, AUX OUT, MAIN OUT. MAIN sits nearest the bass end.
- USB-C (device) for audio, MIDI, and the companion-app protocol. It exposes no serial console or mass storage.
- USB-A (host) for importing libraries from an external drive.
- MIDI DIN in and out.
- Pedals: continuous sustain, expression, and 1 switch.
- No Bluetooth and no Wi-Fi.

## Storage and power

- NVMe (up to 1 TB in v1), sealed inside the unit.
- Read-only root filesystem, A/B firmware partitions with rollback, journaled data partition.
- An external off-the-shelf power supply that complies with Inmetro, plugged into a locking 5.5 × 2.5 mm DC jack on the rear panel. Voltage and current are still open.
- A soft power button on the rear panel, next to DC IN.
