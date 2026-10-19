# DRAFT FOR FOUNDER — does not satisfy Story 11.8 acceptance criteria

Preparation template for the panel, display, LED and legend specification to be agreed with hardware partners (Ladiani Velora 73). Prepared 2026-10-19. Nothing here is signed off or measured. Blank cells are values the repo does not fix; the partner and founder decide them. No partner names, part numbers or measurements are invented.

Source key (paths relative to repo root):
- **DESIGN** = `_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/DESIGN.md`
- **HWI** = `_bmad-output/planning-artifacts/architecture/architecture-buenos-x64-workstation-2026-10-17/HARDWARE-INTERFACES.md`
- **HWP** = `_bmad-output/specs/spec-arm64-stage-workstation/hardware-platform.md`
- **EPICS** = `_bmad-output/planning-artifacts/epics.md`

## 1. Panel and legend specification

| # | Parameter | Value fixed by repo | Source | Partner / founder to fill |
|---|---|---|---|---|
| 1.1 | Layout | Panel layout A: left lockup, MASTER, ROTARY STOP, S/F, wheels below; mixer zone 9 faders (1-8 + REV) + DRAWBARS/MIXER toggle, 8 part/scene buttons + PARTS/SCENES toggle; centre 7" display; navigation column LIVE SET ◀ ▶, SCENE ◀ ▶, VALUE encoder, EXIT · MENU · WRITE; right 8 encoders in two rows + PART/ALL/ASSIGN toggle | DESIGN:609 ; mockup `ux-designs/ux-buenos-x64-workstation-2026-10-18/mockups/panel-v2.html` (draws MASTER as an encoder; 2-LED knob toggle is superseded by 3-state) | Render package version/date handed over: ____ |
| 1.2 | Legend language | English only; uppercase; rotary controls read ROTARY; no third-party trademarks | DESIGN:194-197, DESIGN:682 ; HWP:38 ; EPICS:282 (NFR29) | — |
| 1.3 | Legend cap height | >= 2.5 mm ("final mm with the hardware partner") | DESIGN:197, DESIGN:706 ; HWI:58 ; EPICS:295 (NFR36) | Final cap height (mm): ____ |
| 1.4 | Legend letter-spacing | 0.17em | DESIGN:194-196 | Typeface for legends (stand-in is Gill Sans, final face pending): ____ |
| 1.5 | Legend colour | `panel-legend` #3b413d | DESIGN:53 | Ink product / Pantone / RAL ref: ____ |
| 1.6 | Legend contrast | >= 5:1 against the darkest aluminium stop (DESIGN table lists 5.06:1 on alu-low) | DESIGN:706, DESIGN:562 | Measured on sample: ____ |
| 1.7 | Legend band | Matte or bead-blasted band; never legends on bare specular aluminium | DESIGN:706, DESIGN:681 | Band treatment and gloss units (GU): ____ |
| 1.8 | Legend method | Open: engraving vs silkscreen | DESIGN:699 ; EPICS:6327+ (Story 11.8 AC1) | Method: ____ ; ink: ____ ; reason: ____ ; wear/abrasion test method: ____ |
| 1.9 | Aluminium finish | Brushed; must hide wear (Motif XS is the anti-reference) | DESIGN:518, DESIGN:681 | Alloy / finish / anodising: ____ ; scratch test: ____ |
| 1.10 | Aluminium render colours | alu-high #dadddf, alu-mid #c3c7ca, alu-low #b1b5b8 (render references, not production values) | DESIGN:49-51, DESIGN:543 | Production colour match tolerance (dE): ____ |
| 1.11 | Panel ink / button cap / edge / lit edge / fader track | panel-ink #2b322e; button-cap #e9ecea; button-edge #9aa19c; button-edge-lit #4b6f58; fader-track #7b827e | DESIGN:52, 54-57 | Material and colour match: ____ |
| 1.12 | MASTER control type | Open: knob vs fader (panel v2 draws an encoder); decision needs a recorded reason | DESIGN:699 ; EPICS:6327+ (AC1) | Type: ____ ; reason: ____ |
| 1.13 | Dark-stage plan | Open, choose one: tactile cues only / backlit navigation column + toggles / USB-A lamp port | DESIGN:706 ; EPICS (Story 11.8 AC1) | Chosen option: ____ ; cost/impact: ____ |

## 2. Controls and tactile cues

| # | Parameter | Value fixed by repo | Source | To fill |
|---|---|---|---|---|
| 2.1 | WRITE button | Concave cap or raised ring | DESIGN:707 | Which: ____ |
| 2.2 | MENU button | Centre dot; sits between EXIT and WRITE; same cap and edge as navigation buttons, no LED | DESIGN:707, DESIGN:662 | Dot size/height: ____ |
| 2.3 | VALUE encoder | >= 30 mm, knurled, with push switch (push-click) | DESIGN:707 ; HWP:41 ; HWI:50 | Diameter/knurl: ____ ; part no.: ____ |
| 2.4 | VALUE push switch reporting | Reported in the panel MCU frame as a debounced `BUTTON` press/release with its own button ID | HWI:50, HWI:79 | Switch part no., debounce measured: ____ |
| 2.5 | Parameter encoders | 8 endless encoders, each with an LED ring; pointer line on the knob | HWI:50 ; HWP:41 ; DESIGN:707 | Part no.: ____ ; detent/torque: ____ |
| 2.6 | Faders | 9 dual-function faders, dark cap on grey track, non-motorized in v1; >= 10-bit, >= 500 Hz update rate | DESIGN:667, DESIGN:499-501 ; HWI:51 ; EPICS:305 (NFR43), EPICS:488 (UX-DR53) | Fader part no./travel/feel: ____ |
| 2.7 | REV fader cap | Distinct from faders 1-8 | DESIGN:707 ; EPICS:488 | Distinguishing feature: ____ |
| 2.8 | Part/scene buttons | 8 buttons with an LED strip above each; nubs on buttons 1 and 5 | DESIGN:707, DESIGN:665 | Nub size: ____ |
| 2.9 | Toggle buttons | 3 toggles (DRAWBARS/MIXER, PARTS/SCENES, PART/ALL/ASSIGN 3-state); one legend above/below with one LED per state; slightly taller than momentary buttons | DESIGN:664, DESIGN:707 ; HWI:53 ; HWP:42 | Height difference (mm): ____ |
| 2.10 | Other buttons | LIVE SET ◀ ▶, SCENE ◀ ▶, EXIT, MENU, WRITE, ROTARY STOP, S/F | HWP:43 ; HWI:53 | Switch type/actuation force: ____ |
| 2.11 | Wheels, pedals | Pitch and mod wheels >= 10-bit | HWI:52 | Part no.: ____ (out of scope of 11.8 unless founder adds) |

## 3. LEDs

| # | Parameter | Value fixed by repo | Source | To fill |
|---|---|---|---|---|
| 3.1 | Type | Diffused, >= 3 mm | DESIGN:705 ; HWI:54 (diffused) | LED part no.: ____ ; colour (nm / CCT): ____ |
| 3.2 | On:off contrast | >= 10:1 at 50 klx ambient (example target >= 150 mcd at full drive) | DESIGN:705 ; HWI:54 ; EPICS:295 | Measured: ____ |
| 3.3 | Weak level | ~15% PWM, fixed | DESIGN:705 | Measured luminance at 15%: ____ |
| 3.4 | User brightness | 3 steps driven by firmware (Settings: Low / Mid / High / Follow screen) | DESIGN:705 ; EXPERIENCE (`ux-designs/.../EXPERIENCE.md`:69) | Measured step luminance: ____ / ____ / ____ |
| 3.5 | States and render colours | on #9fbaa8, weak #7f9a88, lens-off #7d8581; encoder rings never weak-lit (rest of ring is unlit lens) | DESIGN:58-60, DESIGN:666 | Real LED colours vs render: ____ |
| 3.6 | Control | SoC commands only (`LED_SET`: ID, brightness/colour); MCU never sets LED state | HWI:54, HWI:80 | — |

## 4. Display

| # | Parameter | Value fixed by repo | Source | To fill |
|---|---|---|---|---|
| 4.1 | Size / resolution | 7" MIPI DSI, native 1024x600, landscape | HWI:103 ; HWP:46 ; EPICS:305 | Panel part no.: ____ |
| 4.2 | Touch | Capacitive, I2C, controller with a mainline Linux driver (examples named in repo: Goodix, FocalTech families) | HWI:105 | Controller part no.: ____ ; mainline driver name/kernel version: ____ |
| 4.3 | Luminance | >= 1000 cd/m2 max | HWI:104 ; DESIGN:704 ; EPICS:295 | Measured max: ____ |
| 4.4 | Minimum luminance | Backlight PWM-dimmable by SoC to <= 5 cd/m2 (UI range 5-100%) | HWI:106 ; DESIGN:704 | Measured min: ____ |
| 4.5 | Cover glass | Optically bonded anti-glare + anti-reflection, <= 2% reflectance | HWI:104 ; DESIGN:704 | Measured reflectance: ____ |
| 4.6 | Touch target context | Min touch target 56 px (~8 mm) | DESIGN:219, DESIGN:599 | Touch accuracy at glove/edge: ____ |
| 4.7 | HDMI fallback | Board should provide a footprint/variant if DSI bring-up on mainline fails | HWI:103 | Footprint present: ____ |

## 5. Sample-acceptance measurement checklist

Floors come from the sources above. "Corrective action" must be named and dated for any miss, before first playable units (month 12) (EPICS Story 11.8 AC3; go-to-market timeline `_bmad-output/specs/spec-arm64-stage-workstation/go-to-market.md`, Timeline table).

| # | Measurement | Floor | Instrument / method (partner to state) | Result | Pass? | Corrective action + date |
|---|---|---|---|---|---|---|
| M1 | Display max luminance | >= 1000 cd/m2 | ____ | | | |
| M2 | Display min luminance at lowest backlight | <= 5 cd/m2 | ____ | | | |
| M3 | Cover-glass reflectance | <= 2% | ____ | | | |
| M4 | Native resolution and touch-controller identification | 1024x600; mainline driver | ____ | | | |
| M5 | LED on:off contrast at 50 klx ambient (each LED type: toggle, part/scene, encoder ring) | >= 10:1 | ____ | | | |
| M6 | Weak-level luminance at ~15% PWM, and the 3 user brightness steps | ~15%; 3 steps | ____ | | | |
| M7 | Legend contrast vs darkest aluminium (measured, not from render) | >= 5:1 | ____ | | | |
| M8 | Legend cap height (caliper/microscope) | >= 2.5 mm | ____ | | | |
| M9 | Legend surface is matte / bead-blasted (gloss reading) | matte | ____ | | | |
| M10 | Legend and finish wear after abrasion test | legends legible; finish hides wear | ____ | | | |
| M11 | VALUE encoder diameter, knurl, push-click present | >= 30 mm; push switch | ____ | | | |
| M12 | VALUE push reaches the panel MCU as a `BUTTON` frame | press/release events | ____ | | | |
| M13 | Tactile cues present: WRITE concave/ring, MENU dot, pointer lines, REV cap distinct, nubs on part buttons 1 and 5, toggles taller | per DESIGN:707 | visual + blind-touch trial | | | |
| M14 | Fader caps dark on grey track; non-motorized | per DESIGN:667 | ____ | | | |
| M15 | Dark-stage plan verified (blind-touch trial in dark; backlight/lamp port if chosen) | chosen plan works | ____ | | | |

Wording for the final report requirements is in EPICS Story 11.8 (three Given/When/Then blocks): panel spec signed off; display report with DSI + touch part numbers; encoder and LED report with part numbers.

## 6. Open items for the founder

- Items marked blank in 1.3, 1.8, 1.12, 1.13 are the decisions Story 11.8 AC1 requires.
- Partner names, sample dates and sign-off signatures: to confirm (not in repo).
- Whether the render package needs a refreshed panel v3 mockup (MASTER type, 3-state toggle) before handoff: to confirm.
