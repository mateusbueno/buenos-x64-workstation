# Acceptance Tests

## Sound fidelity

| Engine | Method | Pass |
|---|---|---|
| Sample | Null test: the same notes rendered offline by the reference renderer (e.g. sfizz) and by the unit | Residual below a defined dB tolerance. Automated |
| Organ, objective | Scripted MIDI sequences with identical drawbar, percussion, and rotary settings, played on the reference and on the unit. Compare spectrum, key click, percussion decay, and leakage | Within tolerances derived from the reference |
| Organ, subjective | Blind ABX with a panel of 3–5 professional keyboardists | The panel cannot reliably tell the two apart, or the mean rating is ≥4/5 |
| FM | Null test against a reference implementation, used only as a test oracle and never shipped | Residual below tolerance |

**Organ references:**
- The Hammond M-solo, which is MIDI-driven, sends and receives CC/NRPN, and is used for automated A/B renders.
- Recordings of a real tonewheel B-3 through a real rotary speaker cabinet. Recommended.

## SFZ v1 opcode subset

- **In:**
  - Headers: `<global>`, `<master>`, `<group>`, `<region>`, `<control>`.
  - Key and velocity ranges, `pitch_keycenter`, tune/transpose, volume/pan, offset.
  - Round robin (`seq_length`/`seq_position`, `lorand`/`hirand`).
  - Loops (`loop_mode`, `loop_start`/`loop_end`).
  - `ampeg_*`, release triggers, keyswitches (`sw_*`), CC crossfades (`xfin`/`xfout`).
  - `#define`/`#include`, `default_path`, basic filters (`fil_type`, cutoff, resonance).
- **Out:** scripting and effect buses.
- SF2 is imported into the same internal model.

## Reference corpus (free libraries)

Salamander Grand Piano, VSCO2, Sonatina Symphonic Orchestra, Karoryfer libraries, and a Pianobook selection. Each must import and play with its mapping intact.

## Robustness

- 24 h stress test at 256 voices with the SSD near full: zero dropouts.
- 100 random power cuts during firmware update and during library import: the unit boots every time, and no library is partially imported.
- Boot: the Boot into target (each of the 3 options) playable ≤10 s with a 1 TB library.
- Seamless switching (CAP-20): automated renders switch scene, in-setlist Live Set, setlist, and a non-preloaded Live Set during held, sustained and releasing notes. Pass: no gap or truncation in the output.
