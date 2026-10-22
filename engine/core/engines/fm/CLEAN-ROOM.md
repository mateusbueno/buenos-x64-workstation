# Clean-Room Record for Velora 6-Operator FM Engine (Story 12.1)

## 1. Compliance and Provenance Statement
In accordance with NFR28 and NFR29 of the Velora System Architecture:
1. **Zero GPL Code**: No GPL, LGPL, or copyleft source code (including Dexed, MSFA, Hexter, or any derivatives) was inspected, referenced, consulted, read, copied, or given as context to the implementing agent or engineers.
2. **Clean-Room Design**: The mathematical formulation and digital signal processing algorithms are developed entirely from first principles and publicly available academic literature, expired patents, and standard engineering textbooks.
3. **No Proprietary Trademarks**: No third-party trademarks ("DX7" or manufacturer brand names) are used in source code, identifiers, comments, or binaries. The engine is referred to generically as "6-operator FM".

## 2. Theoretical Foundations and Academic Literature
The implementation is based on the following public domain and scientific works:
- **Chowning, John M. (1973)**: *"The Synthesis of Complex Audio Spectra by Means of Frequency Modulation"*, Journal of the Audio Engineering Society (JAES), Vol. 21, No. 7, pp. 526–534.
  - Formulates the basic carrier-modulator phase modulation equation:
    $$y[n] = A \cdot \sin(\omega_c n + I \cdot \sin(\omega_m n))$$
  - Demonstrates that sinusoidal phase modulation yields Bessel function-distributed sideband spectra:
    $$y[n] = A \sum_{k=-\infty}^{\infty} J_k(I) \sin((\omega_c + k\omega_m)n)$$
- **Expired Patents**:
  - US Patent 4,018,121 (Chowning / Stanford University, 1977, expired): Phase modulation waveform generator.
  - US Patent 4,178,822 (Yamaha, 1979, expired): Digital phase modulation musical instrument.
  - US Patent 4,249,447 (Yamaha, 1981, expired): Operator feedback loop implementation using running average of previous output samples.
- **Textbooks**:
  - Roads, Curtis (1996): *"The Computer Music Tutorial"*, MIT Press. Chapter 5: Modulation Synthesis.
  - Smith, Julius O. (2011): *"Spectral Audio Signal Processing"*, W3K Publishing.
  - Bristow-Bovey, Peter (1984): Published algorithms for 6-operator phase modulation topologies.

## 3. Mathematical Specifications
1. **Sampling Rate & Phase Accumulator**:
   - Audio rate $F_s = 48{,}000\text{ Hz}$.
   - Normalized phase $\phi \in [0.0, 1.0)$, incremented per sample by $\Delta \phi = \frac{f_{\text{op}}}{F_s}$.
   - Sine calculation: computed using direct sinusoidal evaluation or high-precision 2048-point Taylor/polynomial-interpolated lookup.
2. **Feedback Operator**:
   - Feedback is applied to designated feedback operators using the average of the last two output samples:
     $$\tilde{y}[n] = \frac{y[n-1] + y[n-2]}{2}$$
     $$\phi_{\text{mod}}[n] = \phi[n] + \beta_{\text{fb}} \cdot \tilde{y}[n]$$
   - $\beta_{\text{fb}}$ is scaled exponentially from level 0 (0.0) to level 7 ($\approx \pi$).
3. **Operator Rate-Level Envelopes**:
   - 4-stage piecewise envelope (Rates $R_1, R_2, R_3, R_4 \in [0, 99]$, Levels $L_1, L_2, L_3, L_4 \in [0, 99]$).
   - Rate $R$ translates to an exponential time constant or slope:
     $$\Delta t \propto 2^{(R - 99) / 8}$$
   - Key-on advances through $S_1 \to S_2 \to S_3$ and holds at $L_3$; Key-off triggers transition $S_4$ towards $L_4$.
4. **Keyboard & Velocity Scaling**:
   - Keyboard rate scaling: higher notes accelerate envelope rates per octave.
   - Keyboard level scaling: piecewise curve around selectable breakpoint note with linear/exponential left/right curves.
   - Velocity sensitivity: exponential modulation index scaling $I_{\text{vel}} = 2^{((v - 127) / 127) \cdot S_{\text{vel}}}$.
