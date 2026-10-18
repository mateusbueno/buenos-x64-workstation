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

- Fatar 73SW (semi-weighted), from the partner's stock of ~100 units. Assumed waterfall profile, E to E.
- A dedicated scan MCU (e.g. STM32 or RP2350) scans at ≥10 kHz. It derives attack and release velocity from contact timing, reads aftertouch if present, and sends timestamped events to the SoC over UART/SPI in <1 ms.
- The keybed model is a calibration table, so a different Fatar model needs no code change.
- Resupply beyond the first ~100 units needs Fatar OEM terms (open question).

## Control surface and display (UI option A)

- One knob per function for piano, samples, and effects, plus 9 physical sliders as drawbars.
- A 7-inch touchscreen for library search, categories, and favorites.

## Audio path

- RK3588 I2S/TDM feeds a TI PCM3168A codec through the mainline ASoC drivers (`rockchip-i2s-tdm`, `pcm3168a`), plus a board machine driver.
- The codec is the clock master, with its own 48 kHz-family crystal. The SoC PLL must not clock the audio.
- USB-C runs in device mode as a UAC2 audio gadget plus a MIDI gadget. Nord is the only major brand without a USB audio interface.
- An internal USB audio chip (e.g. XMOS) was rejected, because it puts the xHCI stack in the critical latency path.

## v1 I/O

- 4 balanced ¼" TRS outputs: 2 main and 2 aux.
- Headphone output with its own amplifier.
- 1 stereo line input.
- USB-C (device) for audio, MIDI, and the companion-app protocol. It exposes no serial console or mass storage.
- USB-A (host) for importing libraries from an external drive.
- MIDI DIN in and out.
- Pedals: continuous sustain, expression, and 1 switch.
- No Bluetooth and no Wi-Fi.

## Storage and power

- NVMe (up to 1 TB in v1), sealed inside the unit.
- Read-only root filesystem, A/B firmware partitions with rollback, journaled data partition.
- An off-the-shelf power supply that complies with Inmetro.
