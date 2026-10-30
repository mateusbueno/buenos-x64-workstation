# Clean-Room Record for Velora Modeled Electric Piano Engine (Story 14.1)

## 1. Compliance and Provenance Statement

In accordance with NFR56, NFR57, and NFR29 of the Velora System Architecture:

1. **Zero GPL Code / Clean-Room Provenance**:
   - No GPL, LGPL, copyleft, proprietary, or reverse-engineered source code (including but not limited to Dexed, MSFA, setBfree, Pianoteq, Lounge Lizard, EP-MK1, MDA EPiano, Csound epiano opcodes, or any open-source or commercial EP implementations) was inspected, referenced, decompiled, disassembled, consulted, read, copied, or given as context to the implementing agent or engineers.
   - No competing product was disassembled, reverse engineered, or given to the implementing agent or engineers as context.
   - The digital signal processing algorithms, physical simulation mechanics, and electroacoustic transducer formulations were derived solely from first principles, published peer-reviewed academic literature, expired public-domain historical patents, and physical/acoustic measurements of instruments owned or directly recorded by the project founder.

2. **Generic Terminology Only (NFR29)**:
   - In strict compliance with NFR29, no proprietary third-party trademarks ("Rhodes", "Wurlitzer", "Fender", "Hohner", "Pianoteq", "Lounge Lizard", or any brand names) appear in source code identifiers, comments, parameter definitions, user interface strings, or binary artifacts.
   - The engine is designated generically as "Modeled Electric Piano" (`kEngineModeledEP`). The two physical instrument types are designated generically as "Tine" (electromagnetic variable reluctance pickup) and "Reed" (electrostatic variable capacitance pickup).

3. **Development Audio References vs. Production Shipped Content**:
   - Development reference audio files (from Freesound contributors tim.kahn and OldBassMan) were individually checked and verified under Creative Commons Attribution 4.0 International (CC BY 4.0).
   - Audio references with Non-Commercial clauses (such as jRhodes3d, CC BY-NC 4.0) are strictly excluded and dropped from development and production.
   - **Zero Shipped Reference Audio**: Development reference audio is utilized strictly offline for parameter calibration, curve fitting, and objective spectral comparison. No third-party reference audio files, wavetables, or sample assets are vendored, packaged, or shipped within firmware binaries or factory content bundles.
   - Production validation relies on direct founder recordings and blind ABX listening panels.

4. **Lifecycle and Review Gating (NFR56, EXT16)**:
   - This clean-room record is maintained directly in `engine/core/engines/ep/CLEAN-ROOM.md`, kept versioned with the code, and must be reviewed and re-verified before any later Epic 14 merge.
   - A preliminary patent search has been documented in `_bmad-output/specs/spec-arm64-stage-workstation/ip-and-references.md`.
   - In accordance with NFR56, neither the tine nor reed model ships commercially until formal legal freedom-to-operate clearance is confirmed by external patent counsel prior to Epic 14 release gating (Story 14.13).

---

## 2. Theoretical Foundations and Academic Literature

The mathematical formulation of the modeled electric piano is founded on published scientific, acoustic, and mechanical literature:

### A. Cantilever Beam Vibration & Euler-Bernoulli Theory
The vibrating element in both tine and reed electric pianos is a cantilever beam with fixed-free boundary conditions.

- **Euler-Bernoulli Beam Equation**:
  Transverse vibration $y(x,t)$ of a uniform isotropic elastic beam is governed by:
  $$EI \frac{\partial^4 y(x,t)}{\partial x^4} + \rho A \frac{\partial^2 y(x,t)}{\partial t^2} = 0$$
  where $E$ is Young's modulus, $I$ is the area moment of inertia, $\rho$ is mass density, and $A$ is beam cross-sectional area.

- **Boundary Conditions (Fixed at $x = 0$, Free at $x = L$)**:
  - Clamped root ($x = 0$): $y(0,t) = 0$, $\left.\frac{\partial y}{\partial x}\right|_{x=0} = 0$
  - Free tip ($x = L$): $\left.\frac{\partial^2 y}{\partial x^2}\right|_{x=L} = 0$ (bending moment is zero), $\left.\frac{\partial^3 y}{\partial x^3}\right|_{x=L} = 0$ (shear force is zero)

- **Eigenvalue Characteristic Equation & Inharmonic Partials**:
  Separation of variables yields spatial eigenfunctions $\phi_n(x)$ subject to:
  $$\cos(\beta_n L) \cosh(\beta_n L) = -1$$
  The positive roots $\beta_n L$ evaluate to:
  $$\beta_1 L \approx 1.87510, \quad \beta_2 L \approx 4.69409, \quad \beta_3 L \approx 7.85476, \quad \beta_4 L \approx 10.99554$$
  Modal natural frequencies for an ideal uniform cantilever beam:
  $$f_n = \frac{(\beta_n L)^2}{2\pi L^2} \sqrt{\frac{EI}{\rho A}}$$
  The partial ratios relative to the fundamental mode $f_1$ are inherently inharmonic:
  $$r_1 = 1.000, \quad r_2 = \left(\frac{\beta_2}{\beta_1}\right)^2 \approx 6.267, \quad r_3 = \left(\frac{\beta_3}{\beta_1}\right)^2 \approx 17.548, \quad r_4 = \left(\frac{\beta_4}{\beta_1}\right)^2 \approx 34.387$$

- **Mass Loading (Tuning Spring) & Resonator Coupling**:
  - A tuning spring of mass $m_s$ positioned at distance $x_s$ from the root introduces a localized inertial perturbation. By Rayleigh-Ritz perturbation theory:
    $$\frac{\Delta f_n}{f_n} \approx -\frac{m_s \phi_n^2(x_s)}{2 M_{\text{beam}}}$$
    Because $\phi_1(x)$ grows monotonically toward the tip while higher modes have internal nodes, sliding the tuning clip shifts the fundamental frequency $f_1$ far more sensitively than the high-frequency clang partials.
  - In tine instruments, the tine root is coupled to a massive tuned tone bar (tuning fork configuration), providing a mechanical reactive load that reflects vibratory energy back into the tine, supporting a prolonged fundamental decay while rapidly draining high inharmonic clang partials.

- **Published Literature**:
  - **Fletcher, N. H., & Rossing, T. D. (1998)**: *The Physics of Musical Instruments* (2nd ed.), Springer-Verlag. Chapter 2: Sound Waves in Solids; Chapter 3: Bars and Beams (Cantilever bars, mass loading, internal friction).
  - **Morse, P. M., & Ingard, K. U. (1968)**: *Theoretical Acoustics*, McGraw-Hill. Transverse vibrations of stiff rods and boundary conditions.
  - **Bilbao, Stefan (2009)**: *Numerical Sound Synthesis: Finite Difference Schemes and Simulation in Musical Acoustics*, John Wiley & Sons. Discretization of stiff beam ODEs and energy-preserving numerical methods.
  - **Graff, Karl F. (1975)**: *Wave Motion in Elastic Solids*, Ohio State University Press.

---

### B. Non-Linear Hammer Impact Mechanics (Hertzian Contact)
The acoustic excitation arises from an elastomeric or felt-covered hammer striking the cantilever beam near its root or node.

- **Hammer Dynamic Equation**:
  $$m_h \frac{d^2 y_h(t)}{dt^2} = -F_h(t)$$
  where $m_h$ is effective hammer mass and $y_h(t)$ is hammer displacement.

- **Hertzian Contact Compliance Law**:
  The contact force $F_h(t)$ between hammer and beam at striking coordinate $x_h$ is non-linear and unilateral:
  $$F_h(t) = \begin{cases} k_h \cdot \left[ y_h(t) - y_b(x_h, t) \right]^p + r_h \cdot \left[ y_h(t) - y_b(x_h, t) \right]^p \frac{d}{dt}\left[ y_h(t) - y_b(x_h, t) \right], & y_h(t) > y_b(x_h, t) \\ 0, & y_h(t) \le y_b(x_h, t) \end{cases}$$
  where $k_h$ is hammer stiffness, $p$ is non-linear elasticity exponent ($1.5 \le p \le 2.5$, typically $p \approx 2.0$), and $r_h$ represents contact felt hysteresis/viscoelastic loss.

- **Velocity-Dependent Pulse Width & Spectral Brightness**:
  At higher striking velocities $v_h$, peak compression $\Delta y_{\max}$ increases and contact duration $T_c \propto v_h^{-(p-1)/(p+1)}$ contracts. This contraction expands the frequency spectrum of the excitation pulse, injecting proportionally more energy into higher beam modes ($r_2, r_3, r_4$) at higher MIDI velocities. This achieves smooth, monotonic timbre dynamics across velocity 1–127 without velocity-split sample switching.

- **Published Literature**:
  - **Hertz, Heinrich (1882)**: *"Über die Berührung fester elastischer Körper"*, Journal für die reine und angewandte Mathematik, 92, pp. 156–171.
  - **Hunt, K. H., & Crossley, F. R. E. (1975)**: *"Coefficient of Restitution for Collision of Dynamic Systems"*, ASME Journal of Applied Mechanics, 42(2), pp. 440–445.
  - **Chaigne, A., & Askenfelt, A. (1994)**: *"Numerical simulations of piano strings. I. Physical model"*, Journal of the Acoustical Society of America, 95(2), pp. 1112–1118.
  - **Suzuki, H. (1987)**: *"Vibration analysis of a hammer-string contact"*, J. Acoust. Soc. Am., 81(Suppl 1), S40.

---

### C. Transducer & Pickup Modeling

#### 1. Tine: Electromagnetic Variable Reluctance Pickup
- **Physical Structure**: Permanent magnetic cylinder surrounded by a coil of $N$ wire turns, oriented coaxially toward the vibrating ferromagnetic tine tip.
- **Magnetic Reluctance and Flux**:
  The total reluctance $\mathcal{R}(y)$ of the magnetic path varies inversely with the air gap distance $d(t) = d_0 - y(t)$:
  $$\Phi(t) = \frac{\mathcal{F}_{\text{magnet}}}{\mathcal{R}_{\text{core}} + \mathcal{R}_{\text{gap}}(y(t))} \approx \Phi_0 \left[ 1 + c_1 \frac{y(t) - y_0}{d_0} + c_2 \left(\frac{y(t) - y_0}{d_0}\right)^2 + c_3 \left(\frac{y(t) - y_0}{d_0}\right)^3 \right]$$
  where $d_0$ is quiescent horizontal tip-to-pole distance, and $y_0$ is vertical offset alignment.
- **Faraday's Law of Induction**:
  Induced electromotive force (EMF) $\mathcal{E}(t)$ is proportional to the time rate of change of magnetic flux:
  $$\mathcal{E}(t) = -N \frac{d\Phi}{dt} = -N \frac{\partial \Phi}{\partial y} \frac{dy}{dt}$$
- **Non-Linear Transduction Function**:
  $$\mathcal{E}(t) \propto v_{\text{tip}}(t) \cdot \frac{1}{\left[d_0 - (y_{\text{tip}}(t) - y_0)\right]^2}$$
  - Symmetrical positioning ($y_0 = 0$): predominantly odd harmonic distortion.
  - Asymmetrical positioning ($y_0 \ne 0$): produces strong even harmonics (octave overtone generation) alongside odd harmonics.
  - Large tip excursions close to the pole face produce natural magnetic compression and the characteristic sharp aggressive attack bark.
- **Published Literature**:
  - **Horton, C. W. (1946)**: *"Theory of the magnetic pickup"*, Journal of the Acoustical Society of America, 18(1), pp. 157–164.
  - **Lemarquand, G., & Lemarquand, V. (2007)**: *"Analytical calculation of magnetic fields in electromagnetic transducers"*, IEEE Transactions on Magnetics, 43(8), pp. 3373–3379.
  - **Smith, Julius O. (2010)**: *Physical Audio Signal Processing*, W3K Publishing. Chapter: Variable-Reluctance Pickups.

#### 2. Reed: Electrostatic Variable Capacitance Pickup
- **Physical Structure**: A vibrating conductive reed blade moving inside a fixed polarizing pickup plate (comb), biased with a DC polarizing voltage $V_0$.
- **Variable Capacitance**:
  $$C(t) = \frac{\epsilon_0 A}{d_0 - y(t)}$$
  where $\epsilon_0$ is air permittivity, $A$ is pickup plate face area, and $d_0$ is nominal electrode gap.
- **Displacement Current**:
  The audio signal is the displacement current $i(t) = \frac{dQ}{dt}$:
  $$i(t) = \frac{d}{dt}\left[ C(t) V_0 \right] = V_0 \frac{d C(t)}{dt} = V_0 \frac{\epsilon_0 A}{\left[d_0 - y(t)\right]^2} \frac{dy(t)}{dt}$$
- **Power Series Expansion & Non-Linear Bite**:
  Expanding in terms of normalized displacement $\xi(t) = \frac{y(t)}{d_0} < 1$:
  $$i(t) \approx \frac{V_0 C_0}{d_0} \frac{dy}{dt} \left[ 1 + 2\xi(t) + 3\xi^2(t) + 4\xi^3(t) + \dots \right]$$
  - The quadratic term ($2\xi$) introduces strong second-harmonic drive proportional to instantaneous displacement.
  - As excursion increases during fortissimo strikes, high-order polynomial terms rise exponentially, producing the asymmetric clipping, grit, and reed bite characteristic of electrostatic pianos.
- **Published Literature**:
  - **Hunt, Frederick V. (1954)**: *Electroacoustics: The Analysis of Transduction, and Its Historical Background*, Harvard Monographs in Applied Science.
  - **Rossi, Mario (1988)**: *Acoustics and Electroacoustics*, Artech House.

---

### D. Damper & Key Release Mechanics
- **Damper Contact Damping**:
  When a key is released, a felt pad descends onto the vibrating tine or reed, adding heavy mechanical damping:
  $$m_{\text{beam}} \ddot{y} + \left( c_{\text{internal}} + c_{\text{damper}} \right) \dot{y} + k_{\text{beam}} y = 0$$
- **Release Velocity Modulation**:
  The effective damping coefficient $c_{\text{damper}}$ is modulated by note-off velocity:
  - Rapid release (high velocity): abrupt damping with a sharp felt-choke thud.
  - Slow release (low velocity): gradual damping with transient frictional scrape noise before vibration ceases.
- **Continuous Half-Pedal Law**:
  The continuous sustain pedal input $p_{\text{pedal}} \in [0.0, 1.0]$ smoothly modulates damper contact distance:
  $$c_{\text{damper}}(p) = c_{\text{damper, max}} \cdot \max\left(0, 1.0 - \frac{p_{\text{pedal}}}{p_{\text{threshold}}}\right)$$
- **Published Literature**:
  - **Askenfelt, A., & Jansson, E. V. (1991)**: *"From touch to string vibrations. II: The motion of the key and hammer"*, J. Acoust. Soc. Am., 90(5), pp. 2383–2393.

---

### E. Expired Historical Patents (Public Domain Foundations)
The fundamental mechanical and electrical principles of electromechanical tine and reed pianos reside in the public domain under expired patents:

1. **Harold B. Rhodes (Tine Pianos)**:
   - **US Patent 2,972,274** (filed 1954, issued Feb 21, 1961, expired 1978): *"Electric Piano"* — Asymmetrical tuning fork, cantilever tine and tone bar assembly.
   - **US Patent 3,244,800** (filed 1962, issued Apr 5, 1966, expired 1983): *"Piano Key Action and Tone Generator"*.
   - **US Patent 3,453,923** (filed 1965, issued Jul 8, 1969, expired 1986): *"Electric Piano Tone Generation Using Cantilever Tines"*.
2. **Benjamin F. Miessner & Wurlitzer (Reed Pianos)**:
   - **US Patent 2,942,512** (filed 1955, issued Jun 28, 1960, expired 1977): *"Electric Piano Tone Generator"* — Vibrating reeds excited by piano action, electrostatic capacitive pickups.
   - **US Patent 3,038,364** (filed 1957, issued Jun 12, 1962, expired 1979): *"Capacitive Musical Instrument Pick-Up"*.
   - **US Patent 3,260,784** (filed 1961, issued Jul 12, 1966, expired 1983): *"Electronic Musical Instruments"*.

---

## 3. Mathematical Specifications & Real-Time DSP Formulation

### 1. Sampling Rate & Block Processing
- Audio rate $F_s = 48{,}000\text{ Hz}$.
- Real-time block size: 64 frames ($1.333\text{ ms}$ processing quantum).
- Real-time safety (AD-3): Zero heap allocations, zero system calls, zero lock contention in render path.

### 2. Resonator Bank (Modal Cantilever Model)
Each voice models a bank of 4 modal second-order resonant filters corresponding to the beam's lowest 4 natural modes:
- Fundamental frequency $f_1$: calculated from MIDI note $k \in [28, 100]$ (E1 to E7) with standard equal-temperament tuning:
  $$f_1(k) = 440 \cdot 2^{(k - 69) / 12}$$
- Mode partial ratios:
  $$f_2 = r_2 f_1, \quad f_3 = r_3 f_1, \quad f_4 = r_4 f_1$$
  where ideal cantilever ratios ($1.000, 6.267, 17.548, 34.387$) are adjusted per register by the tuning spring dispersion factor.
- Modal Discrete Filter Formulation:
  For each mode $m \in \{1, 2, 3, 4\}$:
  $$y_m[n] = 2 e^{-\alpha_m / F_s} \cos\left(\frac{2\pi f_m}{F_s}\right) y_m[n-1] - e^{-2\alpha_m / F_s} y_m[n-2] + b_m F_h[n]$$
  where:
  - $\alpha_m$ is modal damping rate: $\alpha_m = \alpha_{\text{base}} + \alpha_{\text{damping}}(m) + \zeta_{\text{damper}}[n]$
  - $b_m$ is mode excitation coefficient determined by modal shape $\phi_m(x_h)$ at hammer strike position $x_h$.

### 3. Hammer Strike Integration
- Input velocity $v \in [1, 127]$ maps to initial hammer velocity:
  $$v_h = v_{\max} \cdot \left(\frac{v}{127}\right)^\gamma, \quad \gamma \approx 1.4$$
- The hammer-beam contact force $F_h[n]$ is computed over the duration of contact $T_c$:
  $$F_h[n] = k_h \max\left(0, y_h[n] - y_{\text{beam}}[n]\right)^p$$
- Hammer hardness voicing parameter modulates stiffness $k_h$ and exponent $p$.

### 4. Non-Linear Transducer Synthesis
- **Tine Transducer**:
  Tip velocity $v_{\text{tip}}[n] = \sum_{m} \dot{y}_m[n]$ is modulated by variable reluctance:
  $$s_{\text{tine}}[n] = g_{\text{pu}} \cdot \frac{v_{\text{tip}}[n]}{\left(1.0 - \frac{y_{\text{tip}}[n] - y_0}{d_0}\right)^2}$$
  with soft saturation limiting extreme excursions.
- **Reed Transducer**:
  Tip velocity is modulated by electrostatic polynomial expansion:
  $$\xi[n] = \frac{y_{\text{tip}}[n]}{d_0}$$
  $$s_{\text{reed}}[n] = g_{\text{pu}} \cdot v_{\text{tip}}[n] \cdot \left(1.0 + 2\xi[n] + 3\xi^2[n]\right)$$

### 5. Voicing Parameters
The engine exposes 5 mechanical and electroacoustic voicing parameters per timbre:
1. **Hammer Hardness** ($k_h$, impulse width): Controls attack brilliance and high-mode excitation.
2. **Pickup Position** ($d_0, y_0$): Controls odd vs. even harmonic balance and non-linear bark depth.
3. **Pickup Level** ($g_{\text{pu}}$): Controls pre-transducer gain and overall sensitivity.
4. **Damper Noise**: Level of mechanical felt release scrape and damping friction.
5. **Attack Noise**: Mechanical strike transient impulse amplitude.

Amp, drive, EQ, chorus, and phaser are handled outside the voice core by the existing fixed effect topologies.
