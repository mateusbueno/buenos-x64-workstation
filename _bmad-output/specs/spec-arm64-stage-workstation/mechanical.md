# Mechanical Envelope

The v1 instrument's physical dimensions, as decided by the founder on 2026-10-23 for technical drawing rev. 00. All values are in mm. Values marked **TBD** have no source yet. Values marked **to confirm** come from a reference that may not match the final part.

The drawing (PDF, one DXF per view, SVG sheets and a zoom/pan viewer) and its generator are in `tools/drawings/velora73/`. Change a value here and in its `model.py` together.

## Conventions

- Origin O is the front-left corner of the enclosure seen from above. X runs right, Y runs back (depth), and Z runs up from the underside of the bottom plate, feet excluded.
- Note names follow C4 = MIDI 60 (middle C). The 73 keys span E1–E7: 43 white and 30 black. The Cs are C2 to C7.
- Drawings use first-angle projection (ABNT). General tolerances follow ISO 2768-mK unless a dimension says otherwise.

## Keybed

The key geometry was measured from Fatar's drawing "Insieme tastiera 61 TP/9 C+ after touch" (drawing code 94163200, rev. 11/06/02, scale 1:1). It was checked against that drawing's own 848.2 and 869.00 dimensions. Fatar sizes keys the same way across its range, but that drawing is for the 61-key TP/9, not the 73SW. Every value below therefore holds until the 73SW drawing arrives (Story 11.6).

| Item | Value |
|---|---|
| White key pitch (width + gap) | 23.6 (22.2 + 1.4) |
| Octave | 165.2 (7 × 23.6) |
| White key length, total / in front of the top-panel edge | 160.0 / 139.4 |
| White key head (wide front part) | 48.0 |
| Black key width, base / top | 12.2 / 8.2 |
| Black key length, total / visible | 110.0 / 89.4 |
| Black key front, from the white key front | 50.0 |
| Black key front chamfer | 3.5 |
| Black key clearance to the white keys, each side | 1.35 (slot 14.9) |
| Black key left edge from the C left edge: C#, D#, F#, G#, A# | 14.55, 42.65, 84.27, 111.21, 138.15 |
| Keybed underside to white key top | 47.5 |
| Black key top above white key top | 10.0 |
| Key travel at the white key front | 10.0 |
| 73 keys: 43 × 23.6 − 1.4 | 1013.4 |
| Keybed frame beyond the keys, each side | 10.4 (**to confirm**; 61 TP/9 value) |
| Keybed width with frame | 1034.2 (**to confirm**) |
| Keybed depth from the white key front | 161.5 (**to confirm**) |
| Waterfall profile, E to E | assumed (**to confirm**, Story 11.6) |
| Waterfall face height | **TBD** |
| Keybed fixing holes | **TBD** (from the 73SW drawing) |

## Enclosure

| Item | Value |
|---|---|
| Width | 1164.2 = 25 (end cheek) + 80 (wheel block) + 1034.2 (keybed) + 25 (end cheek) |
| Depth | 304.4 = 15 (front lip) + 139.4 (keys) + 130 (panel zone, **TBD**) + 20 (rear border). It is provisional because the panel zone depends on the display glass. |
| Height, without feet | 68.7 = 1.2 (bottom) + 47.5 (keybed) + 10.0 (black key) + 8.0 (clearance, **TBD**) + 2.0 (top). It is provisional because it depends on the clearance. |
| Feet | 4 rubber feet Ø30 × 10, M4 screw, centres 50 from each corner |
| Corners | R3 |
| Top panel | Aluminum 2.0, brushed, a finish that hides wear. Flat (not angled). |
| Top panel front flange | 5 down from the top surface (proposal) |
| Bottom | Steel 1.2, painted |
| End cheeks | 25 wide each, tough plastic (resin **TBD**) |
| Wheel block | 80 wide, between the left end cheek and the keybed, holding pitch and mod wheels |
| Front lip | 15 deep; height and material **TBD** |
| Rear panel | Material **TBD** |
| Knobs and fader caps | Machined anodized aluminum; REV cap distinct |
| Weight (reference) | ~20 kg (Story 11.18) |

## Panel controls

Control positions are a **proposal** to validate with the hardware partner. They follow panel layout A in `DESIGN.md`.

| Control | Size |
|---|---|
| MASTER | Analogue level potentiometer in the output path (not read by the panel MCU), Ø20 knob |
| 9 faders | 45 travel, 20 between centres; cap size **TBD** |
| 8 encoders with LED rings | Ø20 knob, 32 between centres in both directions, LED ring Ø28 |
| VALUE encoder | Ø35 knob, knurled, push switch |
| Buttons (17) | 14 × 9; part/scene buttons 20 between centres, LED Ø3 11.5 behind each |
| Toggle buttons (3) | 14 × 9, one Ø3 LED per state |
| Display | 7" 1024×600, active area 153.6 × 90.0 (from 0.150 mm/px), centred on the instrument; glass, cut-out and module **TBD** |
| Pitch and mod wheels | Diameter and width **TBD** |
| Height of knobs, caps and buttons above the top panel | **TBD** |

## Connectors

Seen from the rear, left to right: DC IN and POWER · USB-A, USB-C · MIDI OUT, MIDI IN · PEDAL SUSTAIN, EXPRESSION, SWITCH · LINE IN L, R (¼") and 3.5 mm · AUX OUT R, L · MAIN OUT R, L. MAIN sits nearest the bass end. All rear connector centres are at Z = 35. Cut-out sizes depend on the part numbers (**TBD**).

| Connector | Choice |
|---|---|
| Power | External off-the-shelf Inmetro-compliant supply, locking 5.5 × 2.5 DC plug on the rear panel; voltage and current **TBD** |
| Power button | Soft power button on the rear panel, next to DC IN |
| Headphones | ¼" TRS on the front face, under the wheel block |
| Line input | 2 × ¼" (L/R) plus 1 × 3.5 mm stereo on the rear panel; how the two feed ADC 1–2 (switched or summed) is **TBD** |

## Coordinates (proposal)

Origin O. Rear connectors sit on the rear face (Y = 304.4), feet on the bottom face (Z = 0).

| ID | Element | X | Y | Z | Note |
|---|---|---|---|---|---|
| 9.1 | Wheel PITCH | 50 | 85 | — | size TBD |
| 9.2 | Wheel MOD | 80 | 85 | — | size TBD |
| 13 | MASTER | 55 | 215 | — | knob Ø20 |
| 14.16 | ROTARY STOP | 110 | 215 | — | button 14×9 |
| 14.17 | ROTARY S/F | 135 | 215 | — | button 14×9 |
| 15.1 | DRAWBARS / MIXER | 215 | 245 | — | 2 LEDs |
| 10.1 | Fader 1 | 255 | 245 | — | centre of 45 travel |
| 10.2 | Fader 2 | 275 | 245 | — | centre of 45 travel |
| 10.3 | Fader 3 | 295 | 245 | — | centre of 45 travel |
| 10.4 | Fader 4 | 315 | 245 | — | centre of 45 travel |
| 10.5 | Fader 5 | 335 | 245 | — | centre of 45 travel |
| 10.6 | Fader 6 | 355 | 245 | — | centre of 45 travel |
| 10.7 | Fader 7 | 375 | 245 | — | centre of 45 travel |
| 10.8 | Fader 8 | 395 | 245 | — | centre of 45 travel |
| 10.9 | Fader REV | 415 | 245 | — | centre of 45 travel |
| 15.2 | PARTS / SCENES | 215 | 180 | — | 2 LEDs |
| 14.1 | Part/scene 1 | 255 | 180 | — | LED at Y=191.5 |
| 14.2 | Part/scene 2 | 275 | 180 | — | LED at Y=191.5 |
| 14.3 | Part/scene 3 | 295 | 180 | — | LED at Y=191.5 |
| 14.4 | Part/scene 4 | 315 | 180 | — | LED at Y=191.5 |
| 14.5 | Part/scene 5 | 335 | 180 | — | LED at Y=191.5 |
| 14.6 | Part/scene 6 | 355 | 180 | — | LED at Y=191.5 |
| 14.7 | Part/scene 7 | 375 | 180 | — | LED at Y=191.5 |
| 14.8 | Part/scene 8 | 395 | 180 | — | LED at Y=191.5 |
| 17 | Display (centre of active area) | 582.1 | 219.4 | — | active area 153.6×90.0 |
| 14.9 | LIVE SET ◀ | 715 | 262 | — | button 14×9 |
| 14.10 | LIVE SET ▶ | 735 | 262 | — | button 14×9 |
| 14.11 | SCENE ◀ | 715 | 240 | — | button 14×9 |
| 14.12 | SCENE ▶ | 735 | 240 | — | button 14×9 |
| 14.13 | EXIT | 705 | 172 | — | button 14×9 |
| 14.14 | MENU | 725 | 172 | — | button 14×9 |
| 14.15 | WRITE | 745 | 172 | — | button 14×9 |
| 12 | VALUE (push) | 725 | 205 | — | knob Ø35 |
| 11.1 | Encoder DELAY | 840 | 241 | — | knob Ø20, LED ring Ø28 |
| 11.2 | Encoder BRIGHTNESS | 872 | 241 | — | knob Ø20, LED ring Ø28 |
| 11.3 | Encoder ATTACK | 904 | 241 | — | knob Ø20, LED ring Ø28 |
| 11.4 | Encoder RELEASE | 936 | 241 | — | knob Ø20, LED ring Ø28 |
| 11.5 | Encoder LOW | 840 | 209 | — | knob Ø20, LED ring Ø28 |
| 11.6 | Encoder MID | 872 | 209 | — | knob Ø20, LED ring Ø28 |
| 11.7 | Encoder MID FREQ | 904 | 209 | — | knob Ø20, LED ring Ø28 |
| 11.8 | Encoder HIGH | 936 | 209 | — | knob Ø20, LED ring Ø28 |
| 15.3 | PART / ALL / ASSIGN | 990 | 225 | — | 3 LEDs |
| 19 | PHONES ¼" TRS | 65 | 0 | 7.5 | front face |
| 25 | DC IN (locking 5.5 × 2.5) | 1084.2 | 304.4 | 35 | rear face |
| 26 | POWER | 1059.2 | 304.4 | 35 | rear face |
| 24 | USB-A (host) | 1004.2 | 304.4 | 35 | rear face |
| 23 | USB-C (device) | 979.2 | 304.4 | 35 | rear face |
| 22.1 | MIDI OUT | 924.2 | 304.4 | 35 | rear face |
| 22.2 | MIDI IN | 894.2 | 304.4 | 35 | rear face |
| 21.1 | PEDAL SUSTAIN | 834.2 | 304.4 | 35 | rear face |
| 21.2 | PEDAL EXPRESSION | 809.2 | 304.4 | 35 | rear face |
| 21.3 | PEDAL SWITCH | 784.2 | 304.4 | 35 | rear face |
| 20.1 | LINE IN L ¼" | 724.2 | 304.4 | 35 | rear face |
| 20.2 | LINE IN R ¼" | 699.2 | 304.4 | 35 | rear face |
| 20.3 | LINE IN 3.5 mm | 674.2 | 304.4 | 35 | rear face |
| 18.3 | AUX OUT R | 614.2 | 304.4 | 35 | rear face |
| 18.4 | AUX OUT L | 589.2 | 304.4 | 35 | rear face |
| 18.1 | MAIN OUT R | 529.2 | 304.4 | 35 | rear face |
| 18.2 | MAIN OUT L | 504.2 | 304.4 | 35 | rear face |
| 32.1 | Foot / M4 hole | 50 | 50 | 0 | bottom face |
| 32.2 | Foot / M4 hole | 1114.2 | 50 | 0 | bottom face |
| 32.3 | Foot / M4 hole | 50 | 254.4 | 0 | bottom face |
| 32.4 | Foot / M4 hole | 1114.2 | 254.4 | 0 | bottom face |
