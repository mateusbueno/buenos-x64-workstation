# MCU frame protocol and ID map, version 1.0

Generated from `mcu/common/ids.json` by `mcu/common/gen_ids.py`. Do not edit by hand. Published to the hardware partners (HARDWARE-INTERFACES.md section 5 says the software team owns these maps).

- Document version: **1.0**
- Protocol version: **1.0 (wire byte 0x10)**
- Source hash (SHA-256 of the canonical ids.json): `4e81bc5d0a8e8aa51b060aaf2c4e58ae184417c19abcc4aa5af42ce85c99c665`
- IDs are stable and never reused. A retired ID stays listed in `retired_ids` of the source file.
- Each namespace (controls, buttons, LEDs) has its own ID space; 0 is never assigned.

## 1. Frame format

All multi-byte fields are little-endian. One format for both MCUs and the SoC, both directions.

| Offset | Size | Field | Note |
|---|---|---|---|
| 0 | 1 | Sync | Fixed `0xA5` |
| 1 | 1 | Protocol version | High nibble major, low nibble minor |
| 2 | 1 | Type | See section 3 |
| 3 | 1 | Length | Payload bytes, 0..64 |
| 4 | 2 | Seq | Increments by 1 per frame, wraps at 65536 |
| 6 | 4 | Tick | MCU tick at the event (free-running, at least 1 MHz) |
| 10 | n | Payload | Layout fixed per type, section 3 |
| 10+n | 2 | CRC-16 | CRC-16/CCITT-FALSE over every byte from version to the end of the payload, stored little-endian |

Total frame size is 12 + n bytes (at most 76).

## 2. CRC

CRC-16/CCITT-FALSE: polynomial `0x1021`, initial value `0xFFFF`, no input or output reflection, no final XOR.
Check value: the CRC of the ASCII string `123456789` is `0x29B1`.

## 3. Types and payloads

| Type | Code | Direction | Length | Payload |
|---|---|---|---|---|
| `HELLO` | 1 | MCU to SoC | 5 | `fw_version` u16 (major in the high byte, minor in the low byte), `protocol_version` u8, `board_id` u8 (1 keybed MCU, 2 panel MCU), `keybed_contacts` u8 (2 or 3; 0 on the panel MCU) |
| `KEY_CONTACT` | 2 | MCU to SoC | 3 | `key` u8 (0 = lowest key), `contact` u8 (0-based, in closing order), `state` u8 (1 closed, 0 open) |
| `AFTERTOUCH` | 3 | MCU to SoC | 3 | `target` u8 (`0xFF` = channel pressure, otherwise the key index), `value` u16 |
| `CONTROL` | 4 | MCU to SoC | 3 | `control_id` u8 (section 6), `value` u16: unsigned absolute value for faders, wheels and pedals; signed 16-bit two's complement step delta for encoders |
| `BUTTON` | 5 | MCU to SoC | 2 | `button_id` u8 (section 7), `state` u8 (1 pressed, 0 released); includes the VALUE push switch |
| `LED_SET` | 6 | SoC to MCU | 3 | `led_id` u8 (section 8), `brightness` u8 (0 off, 255 full), `arg` u8: colour index for single LEDs (0 = default colour), fill position 0..255 for encoder rings |
| `TIME_SYNC` | 7 | SoC to MCU to SoC | 9 | `kind` u8 (0 request, 1 reply), `token` u32 (chosen by the SoC in the request, echoed in the reply), `tick` u32 (reply: the MCU tick when the reply was built; request: 0) |
| `ENTER_BOOTLOADER` | 8 | SoC to MCU | 4 | `magic` u32 = `0x544F4F42` (bytes `42 4F 4F 54`) |

A frame whose length differs from the table, or whose type is unknown, is rejected by the decoder and counted. Typed pack/unpack also rejects out-of-range fields: `board_id` in {1, 2}, `keybed_contacts` in {0, 2, 3}, `state`/`pressed` in {0, 1}, control values within the range of the control's ID entry (faders, wheels, pedals unsigned at their resolution; encoder deltas -127..127), and unknown control, button or LED IDs.

## 4. Version policy

The version byte has the major version in the high nibble and the minor version in the low nibble.
The current version is 1.0 (wire byte `0x10`). A receiver whose current major is N accepts frames of major N (any minor)
and major N-1 (any minor, when N-1 is at least 1; major 0 is never valid). Every other major is rejected with a typed
error and a version-mismatch counter. Bump the minor for compatible additions (new types are dropped and counted by older
receivers) and the major for any incompatible change. The sync, length and CRC rules are the same in every major, so a
receiver can always skip a frame of a rejected major.

## 5. Decoder behaviour (reference implementation `mcu/common`)

The streaming decoder consumes one byte at a time and keeps a fixed buffer. On noise, a bad length, a CRC mismatch or a
truncated frame it discards the false sync byte and rescans the bytes it already holds for the next `0xA5`, so a real frame
that started inside a false one is still delivered. It never delivers a frame whose CRC is wrong. Counters: CRC errors,
sequence gaps and lost frames, version mismatches, framing errors, resyncs, dropped bytes, bad lengths, unknown types.

Sequence accounting: seq is 16-bit and wraps. Every CRC-valid frame consumes a seq, including frames that are then rejected
(unknown type, wrong length, unsupported major), so a minor-version addition creates no spurious gap. With `delta = (seq - (last + 1)) mod 65536`:
`delta == 0` is in order; `delta > 0x8000` is a duplicate or out-of-order frame, counted as a reorder and not as a loss
(the expectation does not move); otherwise `delta` frames were lost (one gap). A delivered `HELLO` resets the expectation
without recording a gap, because an MCU reboot restarts seq at 0. A caller that can tell the link went idle calls
`mcu_decoder_flush_stale()` to abandon a candidate frame stalled on a bogus length.

## 6. Controls (CONTROL frame)

Encoders report a signed step delta per event (no absolute value, no pickup). Faders, wheels and pedals report the unsigned value at the stated resolution; an MCU with a coarser ADC shifts its reading left to the stated resolution. The pitch wheel rests at 2048.

| ID | Name | Kind | Bits | Range | Legend |
|---|---|---|---|---|---|
| 1 (`0x01`) | `ENCODER_1` | encoder | 8 | -127..127 | Encoder 1 |
| 2 (`0x02`) | `ENCODER_2` | encoder | 8 | -127..127 | Encoder 2 |
| 3 (`0x03`) | `ENCODER_3` | encoder | 8 | -127..127 | Encoder 3 |
| 4 (`0x04`) | `ENCODER_4` | encoder | 8 | -127..127 | Encoder 4 |
| 5 (`0x05`) | `ENCODER_5` | encoder | 8 | -127..127 | Encoder 5 |
| 6 (`0x06`) | `ENCODER_6` | encoder | 8 | -127..127 | Encoder 6 |
| 7 (`0x07`) | `ENCODER_7` | encoder | 8 | -127..127 | Encoder 7 |
| 8 (`0x08`) | `ENCODER_8` | encoder | 8 | -127..127 | Encoder 8 |
| 9 (`0x09`) | `VALUE_ENCODER` | encoder | 8 | -127..127 | VALUE |
| 16 (`0x10`) | `FADER_1` | fader | 12 | 0..4095 | Fader 1 |
| 17 (`0x11`) | `FADER_2` | fader | 12 | 0..4095 | Fader 2 |
| 18 (`0x12`) | `FADER_3` | fader | 12 | 0..4095 | Fader 3 |
| 19 (`0x13`) | `FADER_4` | fader | 12 | 0..4095 | Fader 4 |
| 20 (`0x14`) | `FADER_5` | fader | 12 | 0..4095 | Fader 5 |
| 21 (`0x15`) | `FADER_6` | fader | 12 | 0..4095 | Fader 6 |
| 22 (`0x16`) | `FADER_7` | fader | 12 | 0..4095 | Fader 7 |
| 23 (`0x17`) | `FADER_8` | fader | 12 | 0..4095 | Fader 8 |
| 24 (`0x18`) | `FADER_9` | fader | 12 | 0..4095 | Fader 9 |
| 32 (`0x20`) | `WHEEL_PITCH` | wheel | 12 | 0..4095 | Pitch wheel (centre 2048) |
| 33 (`0x21`) | `WHEEL_MOD` | wheel | 12 | 0..4095 | Mod wheel |
| 40 (`0x28`) | `PEDAL_SUSTAIN` | pedal | 12 | 0..4095 | Sustain pedal (continuous, half-pedal) |
| 41 (`0x29`) | `PEDAL_EXPRESSION` | pedal | 12 | 0..4095 | Expression pedal |
| 42 (`0x2A`) | `PEDAL_SWITCH` | pedal_switch | 1 | 0..1 | Pedal switch (0 open, 1 closed after polarity detect) |

Retired IDs (never reuse): none.

## 7. Buttons (BUTTON frame)

Debounced press (1) and release (0). Toggle buttons report press and release like any button; the SoC owns the toggle state and drives one LED per state.

| ID | Name | Kind | States | Legend |
|---|---|---|---|---|
| 1 (`0x01`) | `PART_SCENE_1` | button | 1 | Part/scene button 1 |
| 2 (`0x02`) | `PART_SCENE_2` | button | 1 | Part/scene button 2 |
| 3 (`0x03`) | `PART_SCENE_3` | button | 1 | Part/scene button 3 |
| 4 (`0x04`) | `PART_SCENE_4` | button | 1 | Part/scene button 4 |
| 5 (`0x05`) | `PART_SCENE_5` | button | 1 | Part/scene button 5 |
| 6 (`0x06`) | `PART_SCENE_6` | button | 1 | Part/scene button 6 |
| 7 (`0x07`) | `PART_SCENE_7` | button | 1 | Part/scene button 7 |
| 8 (`0x08`) | `PART_SCENE_8` | button | 1 | Part/scene button 8 |
| 16 (`0x10`) | `LIVE_SET_PREV` | button | 1 | LIVE SET ◀ |
| 17 (`0x11`) | `LIVE_SET_NEXT` | button | 1 | LIVE SET ▶ |
| 18 (`0x12`) | `SCENE_PREV` | button | 1 | SCENE ◀ |
| 19 (`0x13`) | `SCENE_NEXT` | button | 1 | SCENE ▶ |
| 20 (`0x14`) | `EXIT` | button | 1 | EXIT |
| 21 (`0x15`) | `MENU` | button | 1 | MENU |
| 22 (`0x16`) | `WRITE` | button | 1 | WRITE |
| 23 (`0x17`) | `VALUE_PUSH` | button | 1 | VALUE push switch |
| 24 (`0x18`) | `ROTARY_STOP` | button | 1 | ROTARY STOP |
| 25 (`0x19`) | `ROTARY_SF` | button | 1 | ROTARY S/F |
| 32 (`0x20`) | `TOGGLE_DRAWBARS_MIXER` | toggle | 2 | DRAWBARS/MIXER |
| 33 (`0x21`) | `TOGGLE_PARTS_SCENES` | toggle | 2 | PARTS/SCENES |
| 34 (`0x22`) | `TOGGLE_PART_ALL_ASSIGN` | toggle | 3 | PART/ALL/ASSIGN |

Retired IDs (never reuse): none.

## 8. LEDs (LED_SET frame)

The MCU never sets an LED on its own. `brightness` 0..255; `arg` is the colour index for single LEDs and the fill position (0..255) for rings. Toggle state LEDs: two for DRAWBARS/MIXER, two for PARTS/SCENES, three for PART/ALL/ASSIGN.

| ID | Name | Kind | Bits | Range | Legend |
|---|---|---|---|---|---|
| 1 (`0x01`) | `PART_SCENE_LED_1` | led | 8 | 0..255 | Part/scene button 1 LED |
| 2 (`0x02`) | `PART_SCENE_LED_2` | led | 8 | 0..255 | Part/scene button 2 LED |
| 3 (`0x03`) | `PART_SCENE_LED_3` | led | 8 | 0..255 | Part/scene button 3 LED |
| 4 (`0x04`) | `PART_SCENE_LED_4` | led | 8 | 0..255 | Part/scene button 4 LED |
| 5 (`0x05`) | `PART_SCENE_LED_5` | led | 8 | 0..255 | Part/scene button 5 LED |
| 6 (`0x06`) | `PART_SCENE_LED_6` | led | 8 | 0..255 | Part/scene button 6 LED |
| 7 (`0x07`) | `PART_SCENE_LED_7` | led | 8 | 0..255 | Part/scene button 7 LED |
| 8 (`0x08`) | `PART_SCENE_LED_8` | led | 8 | 0..255 | Part/scene button 8 LED |
| 16 (`0x10`) | `ENCODER_RING_1` | ring | 8 | 0..255 | Encoder 1 LED ring |
| 17 (`0x11`) | `ENCODER_RING_2` | ring | 8 | 0..255 | Encoder 2 LED ring |
| 18 (`0x12`) | `ENCODER_RING_3` | ring | 8 | 0..255 | Encoder 3 LED ring |
| 19 (`0x13`) | `ENCODER_RING_4` | ring | 8 | 0..255 | Encoder 4 LED ring |
| 20 (`0x14`) | `ENCODER_RING_5` | ring | 8 | 0..255 | Encoder 5 LED ring |
| 21 (`0x15`) | `ENCODER_RING_6` | ring | 8 | 0..255 | Encoder 6 LED ring |
| 22 (`0x16`) | `ENCODER_RING_7` | ring | 8 | 0..255 | Encoder 7 LED ring |
| 23 (`0x17`) | `ENCODER_RING_8` | ring | 8 | 0..255 | Encoder 8 LED ring |
| 32 (`0x20`) | `TOGGLE_LED_DRAWBARS` | led | 8 | 0..255 | DRAWBARS/MIXER state 0 (DRAWBARS) LED |
| 33 (`0x21`) | `TOGGLE_LED_MIXER` | led | 8 | 0..255 | DRAWBARS/MIXER state 1 (MIXER) LED |
| 34 (`0x22`) | `TOGGLE_LED_PARTS` | led | 8 | 0..255 | PARTS/SCENES state 0 (PARTS) LED |
| 35 (`0x23`) | `TOGGLE_LED_SCENES` | led | 8 | 0..255 | PARTS/SCENES state 1 (SCENES) LED |
| 36 (`0x24`) | `TOGGLE_LED_PART` | led | 8 | 0..255 | PART/ALL/ASSIGN state 0 (PART) LED |
| 37 (`0x25`) | `TOGGLE_LED_ALL` | led | 8 | 0..255 | PART/ALL/ASSIGN state 1 (ALL) LED |
| 38 (`0x26`) | `TOGGLE_LED_ASSIGN` | led | 8 | 0..255 | PART/ALL/ASSIGN state 2 (ASSIGN) LED |

Retired IDs (never reuse): none.
