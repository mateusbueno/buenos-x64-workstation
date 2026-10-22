# 6-Operator FM SysEx Parameter Mapping (Story 12.1 & 12.3)

This document maps all 155 packed parameters of the classic 32-algorithm, 6-operator FM voice format (128-byte packed / 143-byte single dump format) into the Velora `FmVoiceParameters` structure. Every field has an exact 1:1 representation with no unmapped parameters.

## Operator Parameters (x6 Operators, Ordered OP6 down to OP1)

| Parameter Name | SysEx Range | Velora Struct Field | Type | Description |
|----------------|-------------|---------------------|------|-------------|
| Operator R1 (Attack Rate) | 0..99 | `operators[i].rates[0]` | uint8_t | Rate of stage 1 |
| Operator R2 (Decay 1 Rate) | 0..99 | `operators[i].rates[1]` | uint8_t | Rate of stage 2 |
| Operator R3 (Decay 2 Rate) | 0..99 | `operators[i].rates[2]` | uint8_t | Rate of stage 3 |
| Operator R4 (Release Rate) | 0..99 | `operators[i].rates[3]` | uint8_t | Rate of stage 4 |
| Operator L1 (Stage 1 Level) | 0..99 | `operators[i].levels[0]` | uint8_t | Target level of stage 1 |
| Operator L2 (Stage 2 Level) | 0..99 | `operators[i].levels[1]` | uint8_t | Target level of stage 2 |
| Operator L3 (Sustain Level) | 0..99 | `operators[i].levels[2]` | uint8_t | Sustain level of stage 3 |
| Operator L4 (Release Level) | 0..99 | `operators[i].levels[3]` | uint8_t | End level of stage 4 |
| Keyboard Breakpoint | 0..99 (A-1..C7) | `operators[i].kbd_breakpoint` | uint8_t | Center note for level scaling |
| Left Scale Depth | 0..99 | `operators[i].left_depth` | uint8_t | Depth below breakpoint |
| Right Scale Depth | 0..99 | `operators[i].right_depth` | uint8_t | Depth above breakpoint |
| Left Scale Curve | 0..3 | `operators[i].left_curve` | uint8_t | 0: -LIN, 1: -EXP, 2: +EXP, 3: +LIN |
| Right Scale Curve | 0..3 | `operators[i].right_curve` | uint8_t | 0: -LIN, 1: -EXP, 2: +EXP, 3: +LIN |
| Keyboard Rate Scaling | 0..7 | `operators[i].kbd_rate_scaling` | uint8_t | Envelope acceleration by pitch |
| Amplitude Mod Sensitivity | 0..3 | `operators[i].amp_mod_sens` | uint8_t | Sensitivity to LFO AM |
| Key Velocity Sensitivity | 0..7 | `operators[i].velocity_sens` | uint8_t | Output level sensitivity to velocity |
| Operator Output Level | 0..99 | `operators[i].total_level` | uint8_t | Master level (carrier vol / mod index) |
| Frequency Mode | 0..1 | `operators[i].fixed_freq_mode` | bool | 0 = Ratio (tracked), 1 = Fixed (Hz) |
| Frequency Coarse | 0..31 | `operators[i].freq_coarse` | uint8_t | Ratio integer multiplier or fixed octave |
| Frequency Fine | 0..99 | `operators[i].freq_fine` | uint8_t | Fractional multiplier 0.00..0.99 |
| Frequency Detune | 0..14 | `operators[i].detune` | int8_t | Cent offset (-7..+7, 7 = center 0) |

## Global Voice Parameters

| Parameter Name | SysEx Range | Velora Struct Field | Type | Description |
|----------------|-------------|---------------------|------|-------------|
| Pitch Env Rate 1 | 0..99 | `pitch_env_rates[0]` | uint8_t | Pitch envelope rate 1 |
| Pitch Env Rate 2 | 0..99 | `pitch_env_rates[1]` | uint8_t | Pitch envelope rate 2 |
| Pitch Env Rate 3 | 0..99 | `pitch_env_rates[2]` | uint8_t | Pitch envelope rate 3 |
| Pitch Env Rate 4 | 0..99 | `pitch_env_rates[3]` | uint8_t | Pitch envelope rate 4 |
| Pitch Env Level 1 | 0..99 | `pitch_env_levels[0]` | uint8_t | Pitch level 1 (50 = zero pitch offset) |
| Pitch Env Level 2 | 0..99 | `pitch_env_levels[1]` | uint8_t | Pitch level 2 |
| Pitch Env Level 3 | 0..99 | `pitch_env_levels[2]` | uint8_t | Pitch level 3 |
| Pitch Env Level 4 | 0..99 | `pitch_env_levels[3]` | uint8_t | Pitch level 4 |
| Algorithm | 0..31 (1..32) | `algorithm` | uint8_t | Operator routing algorithm 1..32 |
| Feedback | 0..7 | `feedback` | uint8_t | Feedback strength on designated operator |
| Oscillator Key Sync | 0..1 | `osc_key_sync` | bool | Reset operator phase on Note On |
| LFO Speed | 0..99 | `lfo_speed` | uint8_t | LFO frequency 0.06..50 Hz |
| LFO Delay | 0..99 | `lfo_delay` | uint8_t | Delay before LFO onset |
| LFO Pitch Mod Depth | 0..99 | `lfo_pitch_mod_depth` | uint8_t | LFO -> Pitch modulation depth |
| LFO Amp Mod Depth | 0..99 | `lfo_amp_mod_depth` | uint8_t | LFO -> Amplitude modulation depth |
| LFO Key Sync | 0..1 | `lfo_key_sync` | bool | Reset LFO phase on note onset |
| LFO Waveform | 0..5 | `lfo_wave` | uint8_t | 0: Tri, 1: SawDown, 2: SawUp, 3: Sqr, 4: Sine, 5: S&H |
| LFO Pitch Mod Sensitivity | 0..7 | `lfo_pitch_mod_sens` | uint8_t | Global pitch sensitivity to LFO PM |
| Transpose | 0..48 | `transpose` | int8_t | Semitone transpose (-24..+24, 24 = C3) |
| Voice Name | 10 chars ASCII | `name[10]` | char[11] | 10-character sanitized name string |

## Velora Extension Parameters

| Parameter Name | Range | Velora Struct Field | Description |
|----------------|-------|---------------------|-------------|
| Brightness Offset | 0..127 | `brightness_offset` | Direct Knob 2 control (offsets modulator levels) |
