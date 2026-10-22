# Clean-Room Record for Velora Virtual-Analog (VA) Subtractive Engine (Story 12.6)

## 1. Compliance and Provenance Statement
In accordance with NFR28 and NFR29 of the Velora System Architecture:
1. **Zero GPL Code**: No GPL, LGPL, or copyleft source code (including but not limited to Helm, Surge, Vital, ZynAddSubFX, OB-Xd, Yoshimi, or any open-source synth derivatives) was inspected, referenced, consulted, read, copied, or given as context to the implementing agent or engineers.
2. **Clean-Room Design**: The mathematical formulations, digital filter models, anti-aliased oscillators, and envelope algorithms are developed entirely from first principles, publicly available academic peer-reviewed literature, and standard signal processing textbooks.
3. **Generic Terminology Only**: In compliance with NFR29, no proprietary third-party trademarks, model numbers, or manufacturer brand names are used in code, comments, parameter names, or binary artifacts. The engine is referred to generically as "virtual-analog" or "subtractive synthesizer".

## 2. Theoretical Foundations and Academic Literature
The implementation is grounded in the following published scientific and engineering works:

### Band-Limited / Anti-Aliased Oscillator Generation
- **Välimäki, V., & Huovilainen, A. (2006)**: *"Antialiased Oscillator Algorithms Based on Polynomial Transition Regions"*, IEEE Transactions on Speech and Audio Processing, Vol. 14, No. 4, pp. 1054–1066.
  - Derives the Polynomial Band-Limited Step (PolyBLEP) correction function for reducing aliasing in digital oscillators with step discontinuities (sawtooth and pulse/square waves).
  - Residual polynomial correction for phase offset $t = \frac{\phi}{\Delta\phi}$:
    $$\text{BLEP}(t) = \begin{cases} 2t - t^2 - 1, & 0 \le t < 1 \\ t^2 + 2t + 1, & -1 < t < 0 \\ 0, & \text{otherwise} \end{cases}$$
- **Välimäki, V., & Huovilainen, A. (2007)**: *"Oscillator and Filter Algorithms for Virtual Analog Synthesis"*, Computer Music Journal, Vol. 31, No. 2, pp. 19–37.
  - Comprehensive survey of band-limited digital oscillator techniques and filter modeling.
- **Stilson, T., & Smith, J. O. (1996)**: *"Alias-Free Digital Synthesis of Classic Analog Waveforms"*, Proceedings of the International Computer Music Conference (ICMC), pp. 332–335.
- **Lane, J., Hoory, D., Martinez, E., & Wang, P. (1997)**: *"Modeling Analog Synthesis with DSPs"*, Computer Music Journal, Vol. 21, No. 4, pp. 23–41.
- **Nam, J., Välimäki, V., Abel, J. S., & Smith, J. O. (2010)**: *"Efficient Antialiasing of Nonlinearities Using Polynomial Approximations"*, IEEE Transactions on Audio, Speech, and Language Processing.

### Zero-Delay Feedback (ZDF) State-Variable Filters
- **Zavalishin, Vadim (2012 / rev 2020)**: *"The Art of VA Filter Design: A Theoretical and Practical Guide to the Calculation of Non-Linear Analog-Emulating Audio Filters"*, Native Instruments technical report / public publication.
  - Formulates the Topology-Preserving Transform (TPT) for zero-delay feedback (ZDF) analog filter emulation without artificial unit-delay feedback latency.
  - Uses bilinear transform with trapezoidal integration to eliminate algebraic loops instantaneously:
    - Prewarped frequency parameter: $g = \tan\left(\frac{\pi f_c}{F_s}\right)$
    - Damping coefficient: $R = \frac{1}{2Q} = 1 - \text{resonance} \cdot 0.99$
    - High-pass: $v_0 = \frac{x - 2R s_1 - g s_1 - s_2}{1 + 2R g + g^2}$
    - Band-pass: $v_1 = s_1 + g v_0$
    - Low-pass: $v_2 = s_2 + g v_1$
    - State register update: $s_1 \leftarrow s_1 + 2g v_0$, $s_2 \leftarrow s_2 + 2g v_1$
  - Multi-pole cascading: 24 dB/octave slope via two cascaded 2-pole SVF sections with staggered damping factors.
  - Non-linear saturation: soft saturation $\tanh(x)$ or saturating feedback ensuring bounded, stable self-oscillation without numerical explosion, NaN, or denormals.

### Subtractive Synth Architecture and Modulation
- **Smith, Julius O. (2011)**: *"Introduction to Digital Filters with Audio Applications"*, W3K Publishing.
- **Pirkle, Will (2014)**: *"Designing Software Synthesizer Plug-Ins in C++"*, Focal Press.
  - Real-time modulation routing, exponential ADSR state machines, and portamento/glide smoothing algorithms.

## 3. Mathematical Specifications

### 1. Sampling Rate & Oscillators
- Audio rate $F_s = 48{,}000\text{ Hz}$.
- Normalized phase $\phi \in [0.0, 1.0)$, step per sample $\Delta\phi = \frac{f}{F_s}$.
- **Sawtooth**:
  $$y_{\text{saw}}[n] = (2\phi - 1) - \text{PolyBLEP}(\phi, \Delta\phi)$$
- **Pulse / Square (with Pulse Width $w \in [0.01, 0.99]$)**:
  $$y_{\text{pulse}}[n] = \left(\phi < w \,?\, 1.0 : -1.0\right) + \text{PolyBLEP}(\phi, \Delta\phi) - \text{PolyBLEP}(\phi - w, \Delta\phi)$$
- **Triangle**:
  Symmetric triangle with smoothed corners using integrated PolyBLEP or polynomial shaping:
  $$y_{\text{tri}}[n] = 2.0 \cdot |2.0 \cdot \phi - 1.0| - 1.0$$
- **Sine**:
  $$y_{\text{sin}}[n] = \sin(2\pi \phi)$$
- **Hard Sync**:
  When Master oscillator (Osc 1) wraps past 1.0, Slave oscillator (Osc 2) is reset to $\phi_2 \cdot \frac{f_2}{f_1}$, with a PolyBLEP step inserted at the sync discontinuity.
- **Noise Generator**:
  Uniform pseudo-random 32-bit xorshift generator producing white noise, with optional 1st-order IIR pink filter ($H(z) = \frac{1 - 0.5z^{-1}}{1 - 0.95z^{-1}}$).

### 2. Multi-Mode ZDF Filter
- Multi-mode outputs: Low-Pass (12 dB / 24 dB), Band-Pass (12 dB), High-Pass (12 dB).
- Cutoff frequency $f_c \in [20\text{ Hz}, 20000\text{ Hz}]$.
- Key tracking: $f_c' = f_c \cdot 2^{(\text{note} - 60) \cdot \frac{\text{keytrack}}{12}}$.
- Envelope modulation: $f_c'' = f_c' \cdot 2^{\text{env\_amount} \cdot \text{env\_val} \cdot 5}$.
- Knob 2 Brightness: adjusts base cutoff logarithmically around nominal value.
- Self-oscillation: stable, bounded sine output when resonance $Q \to \infty$.

### 3. Dual ADSR Envelopes
- Four phases: Attack, Decay, Sustain, Release.
- Attack: exponential or quasi-linear rise from 0 to 1.
- Decay: exponential decay toward Sustain level ($0.0 \le S \le 1.0$).
- Release: exponential decay toward 0.0 upon Note-Off.
- Velocity sensitivity: scales maximum attack peak.

### 4. LFO & Modulation
- Waveforms: Sine, Triangle, SawUp, SawDown, Square, Sample & Hold.
- Rate: $0.1\text{ Hz}$ to $50\text{ Hz}$.
- Destinations: Pitch (vibrato), Filter Cutoff, Amplitude, Pulse Width.
