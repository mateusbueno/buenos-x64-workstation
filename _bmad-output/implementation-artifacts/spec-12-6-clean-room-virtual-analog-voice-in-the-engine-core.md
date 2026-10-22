---
title: 'Story 12.6: Clean-room virtual-analog voice in the engine core'
type: 'feature'
created: '2026-10-22'
status: 'done'
baseline_revision: '29a847d'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/engine/core/include/velora/core/engine.hpp'
  - '{project-root}/engine/core/include/velora/core/block_renderer.hpp'
  - '{project-root}/engine/core/include/velora/core/ump.hpp'
  - '{project-root}/ui/include/velora/ui/screens/part_view.hpp'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** The Velora workstation requires a clean-room virtual-analog (VA) subtractive synthesizer engine in `engine/core` that renders classic analog synth sounds (leads, pads, basses, brass, polysynths) with high fidelity, anti-aliased oscillators, zero-delay feedback resonant multi-mode filters, flexible modulation, unison and glide, without relying on or vendoring any copyleft/GPL synth code.

**Approach:** Implement `VaEngine` and `VaVoice` in `engine/core/engines/va/` and `engine/core/include/velora/core/` / `engine/core/src/va_engine.cpp`:
1. **Clean-room Record (`engine/core/engines/va/CLEAN-ROOM.md`)**:
   - Explicitly cite published academic DSP literature:
     - Välimäki & Huovilainen (2006, 2007) on antialiased oscillator algorithms (PolyBLEP).
     - Zavalishin (2012) on Topology-Preserving Transform (TPT) / Zero-Delay Feedback (ZDF) linear state-variable filter (SVF) topology.
     - Stilson & Smith (1996) and Lane et al. (1997) on digital virtual analog modeling.
   - Affirm zero GPL/LGPL or third-party synth code was inspected, copied, or consulted.
   - Strictly generic terminology: "virtual analog", "subtractive synthesizer", "state-variable filter".
2. **Anti-Aliased Oscillators (`va_polyblep.hpp` / `va_oscillator.hpp`)**:
   - Osc 1 and Osc 2: Sawtooth, Pulse with variable Pulse Width (PWM 0..100%), Triangle, Sine.
   - Anti-aliasing via Polynomial Band-Limited Step (PolyBLEP) correction at step discontinuities to keep aliasing below -50 dB at C8 (4186 Hz) under 48 kHz sampling rate.
   - Sub-oscillator: 1 or 2 octaves below Osc 1 (Square or Sine wave).
   - Noise generator: xorshift pseudo-random white noise and pink noise filtering.
   - Hard sync: Osc 2 hard-synced to Osc 1 phase reset with BLEP discontinuity correction.
   - Detune & pitch: Coarse semitones (-24..+24), fine detune (-50..+50 cents), octave shifts (-2..+2).
3. **Mixer**:
   - Independent level mixing for Osc 1, Osc 2, Sub, and Noise (0..100%).
4. **Resonant Multi-Mode Filter (`va_filter.hpp`)**:
   - Zero-delay feedback (ZDF) state-variable filter topology (Zavalishin TPT).
   - Modes: Low-pass (LP 12 dB/oct and LP 24 dB/oct cascade), High-pass (HP 12 dB/oct), Band-pass (BP 12 dB/oct).
   - Cutoff: 20 Hz to 20 kHz with key tracking (0..100%), filter envelope modulation (-100%..+100%), LFO modulation, and Brightness offset (Knob 2).
   - Resonance (Q): 0.0 to self-oscillation with soft non-linear saturation preventing divergence and ensuring stable bounded output without NaN/denormals.
   - Drive: variable saturation.
5. **Envelopes (`va_envelope.hpp`)**:
   - Dual ADSR envelopes: Amp ADSR and Filter ADSR.
   - Attack, Decay, Sustain (0..100%), Release times with exponential decay/release curves.
   - Velocity sensitivity scaling.
6. **LFO (`va_lfo.hpp`)**:
   - Waveforms: Sine, Triangle, SawUp, SawDown, Square, Sample & Hold (S&H).
   - Frequency: 0.1 Hz to 50 Hz, onset delay, key sync option.
   - Destinations: Pitch (vibrato), Filter cutoff (wah/sweep), Amp (tremolo), PWM.
7. **Voice Performance & Modes**:
   - Voice modes: Polyphonic, Mono, Legato.
   - Glide (Portamento): constant-time or constant-rate glide; modes for always vs legato-only glide.
   - Unison: 1 to 8 unison sub-voices with detune spread and stereo panning spread.
8. **Real-Time Safety & Arbiter Integration**:
   - Zero heap allocation or locks in block rendering.
   - 64-frame block processing.
   - Capacity of 128 voices, cost rating of 4 units per voice.
   - Steal metadata reporting (`VoiceInfo`) and anti-click steal fade ramp (`begin_steal_fade`).
9. **DSP Acceptance Tests (`va_engine_test.cpp`)**:
   - Objective DSP tests verifying aliasing < -50 dB at C8, filter cutoff/resonance accuracy, self-oscillation stability, ADSR timings, and bit-identical golden render reproducibility.

## Boundaries & Constraints

**Always:**
- Clean-room: derived solely from academic DSP papers and textbook equations.
- RT-safety: zero dynamic heap allocation, zero locks, zero blocking operations in `render()`.
- Generic terminology: Virtual Analog / Subtractive Synthesizer.
- Compatible with Velora `Engine` and `VoiceArbiter` interfaces.

**Never:**
- Reference or copy GPL synth code (Helm, Surge, Dexed, Vital, OB-Xd, etc.).
- Use trademarked analog synthesizer brand names.
- Allocate memory during audio rendering.

</intent-contract>
