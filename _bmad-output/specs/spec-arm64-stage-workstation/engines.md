# Engines and Effects

| Engine | Release | Guaranteed voice floor | Role |
|---|---|---|---|
| Sample (streaming) | v1, mandatory | ≥256 stereo | The differentiator |
| Tonewheel organ | v1, mandatory | Full polyphony, never stolen | Expected baseline |
| 6-operator FM | firmware after v1 | ≥128 | Expected baseline |
| Virtual analog | firmware after v1 | ≥64 | Expected baseline |

## Voice pool

- The pool is dynamic and budgeted by CPU. Each engine has a guaranteed floor, and any surplus is shared.
- Stealing priority: protect the lowest held note and notes sustained by the pedal. Steal newer, quieter voices first.

## Organ specifics

- 9 drawbars, mapped to the 9 dual-function faders in DRAWBARS mode.
- Percussion, key click, vibrato/chorus scanner, tonewheel leakage.
- Multi-contact key onset driven by the timing of the keybed's contacts. The reference behaviour is the Hammond M-solo's Virtual Multi-Contact.
- The rotary speaker (slow/fast/stop with ramping, panel buttons ROTARY STOP and S/F) is part of the organ voice path and is also available as an effect.

## Effects (v1, mandatory)

Rotary speaker, reverb, delay, chorus/phaser, EP amp/drive, EQ, compressor. They share the CPU budget with the engines and must not break the latency or dropout targets.

## Rules

- Every engine is clean-room or licensed. GPL references (Dexed/MSFA, setBfree) must not ship; using them only as test oracles is fine. sfizz (BSD-2) may serve as the SFZ parser and reference renderer.
- Marketing, UI and panel names are generic: "6-operator FM", "tonewheel organ", "rotary speaker" (panel: ROTARY). Never "Leslie", "Hammond" or "DX7".
- The engine API, patch format, and UI are built for all four engines from day one.
