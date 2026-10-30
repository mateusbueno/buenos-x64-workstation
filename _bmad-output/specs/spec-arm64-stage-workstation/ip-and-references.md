# IP and Reference Material (modeled keys)

Preliminary research from 2026-10-30, for CAP-24 and CAP-26. It is not a legal opinion. Patent status comes from Google Patents, which marks its status as an assumption. Claim summaries were not checked word for word, except US5468906A.

## Development references for the modeled electric piano

Nothing here ships. The final comparison uses the founder's own recordings.

| Source | Instrument | License | Format | Use | Status |
|---|---|---|---|---|---|
| [tim.kahn, C_S Fender Rhodes Mark II](https://freesound.org/people/tim.kahn/sounds/65762/) ([Pack 3957](https://freesound.org/people/tim.kahn/packs/3957/), 53 note recordings) | 73 Stage tine, through a tube preamp, 53 sounds | CC BY 4.0 (audited on individual sound pages) | Mono, 44.1 kHz, 16-bit | Tine development reference | Approved for development |
| [OldBassMan, Wurlitzer 200a](https://freesound.org/people/OldBassMan/sounds/88000/) ([Pack 5726](https://freesound.org/people/OldBassMan/packs/5726/), 60 note recordings) | 200A reed, through a transformer and tube preamp | CC BY 4.0 (audited on individual sound pages) | Mono, 48 kHz, 24-bit | Reed development reference | Approved for development |
| [jRhodes3d](https://github.com/sfzinstruments/jlearman.jRhodes3d) | 1977 Mark I Stage 73 | Samples CC BY-NC 4.0 | FLAC, up to 5 layers | Not used | Dropped (NC restriction) |

### License Audit and Attribution Details

1. **tim.kahn Rhodes Mark II recordings (Freesound Pack 3957)**:
   - License: Creative Commons Attribution 4.0 International (CC BY 4.0).
   - Verification: All constituent sounds in pack 3957 (`https://freesound.org/people/tim.kahn/packs/3957/`) were verified individually; each is released by author `tim.kahn` under CC BY 4.0.
   - Required Attribution: *"C_S Fender Rhodes Mark II" by tim.kahn (https://freesound.org/people/tim.kahn/packs/3957/), licensed under CC BY 4.0.*
   - Scope limit: Used strictly offline on developer workstations for model parameter extraction and spectral tuning (Story 14.6). Zero audio samples are vendored, included in firmware images, or shipped in customer packages. If offline tuning reports or comparison plots reference these sounds, this attribution must appear in the corresponding report.
2. **OldBassMan Wurlitzer 200a recordings (Freesound Pack 5726)**:
   - License: Creative Commons Attribution 4.0 International (CC BY 4.0).
   - Verification: All individual sound pages within pack 5726 (`https://freesound.org/people/OldBassMan/packs/5726/`) carry CC BY 4.0.
   - Required Attribution: *"Wurlitzer 200a" by OldBassMan (https://freesound.org/people/OldBassMan/packs/5726/), licensed under CC BY 4.0.*
   - Scope limit: Strictly offline development reference for reed vibration decay and electrostatic pickup non-linear behavior. No files bundled or shipped.
3. **jRhodes3d samples**:
   - License: CC BY-NC 4.0.
   - Status: Dropped completely. Because commercial product development conflicts with the Non-Commercial condition, no sample or derivative from jRhodes3d is permitted in the workspace or development pipeline without a separately negotiated commercial license.

## Patent search

### Brazil (INPI pePI, searched 2026-10-30)

No electronic piano resonance or electric piano physical modeling filing was found in Brazil.

| Search | Result |
|---|---|
| Class G10H (electronic instruments), all applicants | 32 filings. None covers piano resonance or electric piano modeling |
| Class G10H 1/0058 / G10H 7/08 (physical modeling) | 0 filings in Brazil |
| Class G10H 3/18 (electromagnetic pickup instruments) | 0 filings in Brazil |
| Abstract contains "piano" | 22 filings. None covers electronic sound generation |
| Title contains "piano" | 15 filings. None covers resonance or modeling. PI 7707284-7 "Piano eletronico digital" dates from 1977 |
| G10H, Yamaha | 1 filing: PI 9501409-8, tone signal generator, filed 1995 |
| G10H, Casio | 1 filing: BR 11 2021 002733 5, keyboard instrument, class G10H 1/32 |
| G10H, Roland and Kawai and Korg | 0 filings |
| Modartt, Kawai, Korg, any class | 0 filings |

Limits: unpublished applications cannot be seen, because filings stay secret for up to 18 months. Multi-word abstract and title searches returned broad, unrelated matches, so only the single-word and class searches above count.

### Electric piano physical modeling patent search

Focused search conducted on Google Patents and worldwide databases across classification codes **G10H 1/00** (including **G10H 1/0058** for physical modeling synthesis), **G10H 7/00** (including **G10H 7/08** for physical model sound synthesis from computation), and **G10H 3/18** / **G10H 3/06** (electromechanical instruments using strings/tines/reeds and electromagnetic/electrostatic pickups), with keywords *electric piano, tine, reed, physical modeling, cantilever, pickup, tone bar, variable reluctance, variable capacitance*:

#### Holder & Classification Search Summary

| Holder / Assignee | Search Queries / Classifications | Findings | Status & Risk Assessment |
|---|---|---|---|
| **Harold B. Rhodes / CBS** | G10H 3/18, G10H 1/00, "electric piano", "tine", "tone bar" | [US3244793A](https://patents.google.com/patent/US3244793A/en), [US3453372A](https://patents.google.com/patent/US3453372A/en), [US2972274A](https://patents.google.com/patent/US2972274A/en), [US3453923A](https://patents.google.com/patent/US3453923A/en) | Expired (1978–1986). Fundamental electromechanical tine & reluctance pickup physics reside in the public domain. |
| **B. F. Miessner / Wurlitzer** | G10H 3/18, G10H 1/00, "reed", "electrostatic pickup", "capacitive pickup" | [US1929027A](https://patents.google.com/patent/US1929027A/en), [US2942512A](https://patents.google.com/patent/US2942512A/en), [US3038364A](https://patents.google.com/patent/US3038364A/en), [US3260784A](https://patents.google.com/patent/US3260784A/en) | Expired (1950–1983). Fundamental electrostatic capacitive reed pickup principles reside in the public domain. |
| **Yamaha** | G10H 7/00, G10H 7/08, G10H 1/0058, "physical model", "waveguide", "electric piano" | [US5369217A](https://patents.google.com/patent/US5369217A/en), [US5521325A](https://patents.google.com/patent/US5521325A/en), [US5747713A](https://patents.google.com/patent/US5747713A/en), [US5468906A](https://patents.google.com/patent/US5468906A/en), [US8115092B2](https://patents.google.com/patent/US8115092B2/en) | Broad waveguide bar and non-linear contact patents expired (2012–2016). Active acoustic piano patent US8115092B2 (expires 2030) is specific to acoustic grand string/soundboard loop networks. Yamaha CP1/CP4/CP88 SCM is sample+filter, not cantilever ODE solver. Low risk. |
| **Roland** | G10H 7/00, G10H 1/00, "physical modeling", "electric piano", "pickup" | [US8629343B2](https://patents.google.com/patent/US8629343B2/en) (SuperNATURAL acoustic piano), [US5731533A](https://patents.google.com/patent/US5731533A/en) (COSM guitar/string) | Zero active patents found claiming cantilever beam modeling or electric piano reluctance/capacitance pickup emulation. Low risk. |
| **Korg** | G10H 7/00, G10H 1/00, "physical model", "electric piano", "reed", "tine" | [US6943286B2](https://patents.google.com/patent/US6943286B2/en) (STR-1 plucked string modeling) | STR-1 covers plucked strings. Korg EP-1 / SGX engines rely on Multi-Dimensional Synthesis (multi-sample playback with interpolation), not physical simulation. Zero patents found on EP beam modeling. Low risk. |
| **Modartt** | G10H 7/00, G10H 7/08, "modal", "soundboard", "electric piano" | [US7915515B2](https://patents.google.com/patent/US7915515B2/en) | Active (expires 2027-10-20). Claims precomputing damped partial coefficients from physical string/soundboard dimensions. Velora EP uses real-time state-space Euler-Bernoulli cantilever modal equations without soundboard partial matrix precomputation. Low risk. |
| **Applied Acoustics Systems (AAS)** | G10H 7/00, G10H 1/0058, all classes, "Lounge Lizard", "physical modeling" | 0 patents found in USPTO / WIPO / EPO registers | AAS relies on trade secret software and copyright rather than public utility patents. No blocking patent on cantilever beam ODE or pickup emulation. Low risk. |
| **Casio & Kawai** | G10H 7/00, G10H 1/00, "piano modeling", "electric piano" | [US9489933B2](https://patents.google.com/patent/US9489933B2/en) (Casio), [US10102837B1](https://patents.google.com/patent/US10102837) (Kawai) | Acoustic piano resonance and channel routing only. Zero EP physical modeling filings found. Low risk. |

#### Specific Patents Identified in Search

| Patent / Publication | Holder / Inventor | Subject | Status | Grant / Expiry Notes |
|---|---|---|---|---|
| [US3244793A](https://patents.google.com/patent/US3244793A/en) | H. B. Rhodes / CBS | Asymmetrical tuning fork and electromagnetic pickup electric piano | Expired | Granted 1966, expired 1983; foundation Rhodes mechanical/pickup patent |
| [US3453372A](https://patents.google.com/patent/US3453372A/en) | H. B. Rhodes / CBS | Tone generator assembly with tine and tone bar | Expired | Granted 1969, expired 1986; foundation Rhodes tine/tone bar assembly |
| [US1929027A](https://patents.google.com/patent/US1929027A/en) | B. F. Miessner | Acoustic-electric piano with electrostatic pickups | Expired | Granted 1933, expired 1950; foundation electrostatic pickup patent |
| [US2942512A](https://patents.google.com/patent/US2942512A/en) | C. W. Andersen / Wurlitzer | Electronic piano with free vibrating reeds and electrostatic pickup | Expired | Granted 1960, expired 1977; foundation Wurlitzer reed electric piano |
| [US5369217A](https://patents.google.com/patent/US5369217A/en) | Yamaha | Struck body physical model using digital filters and loop delay | Expired | Granted 1994, expired 2012 |
| [US5521325A](https://patents.google.com/patent/US5521325A/en) | Yamaha | Musical tone synthesis using waveguide model of vibrating bar | Expired | Granted 1996, expired 2014; broad bar waveguide claims |
| [US5747713A](https://patents.google.com/patent/US5747713A/en) | Stanford Univ. / Yamaha | Modeling of non-linear contact (hammer strike) via non-linear spring | Expired | Granted 1998, expired 2016; Hertzian contact modeling now public domain |
| [US8115092B2](https://patents.google.com/patent/US8115092B2/en) | Yamaha | Piano synthesis modeling damper, hammer, string, and body | Active | Granted 2012, expires 2030-06-06. Specific to acoustic grand string/soundboard coupling |
| [US7915515B2](https://patents.google.com/patent/US7915515B2/en) | Modartt | Partials computation for piano strings and soundboard | Active | Granted 2011, expires 2027-10-20. Restricted to string-soundboard modal networks |

Limits: Worldwide patent filings (USPTO, EPO, WIPO, INPI) remain confidential during the statutory 18-month secrecy window from priority date. Attorney review prior to release (Story 14.13) should include a renewed search across late-published applications. This preliminary search does not constitute a formal legal freedom-to-operate opinion.

#### Potential Patents for Attorney Review Prior to Release (Epic 14 Merge Gate)
1. **US8115092B2 (Yamaha, active to 2030)**:
   - Claims cover specific bidirectional coupled string-bridge networks.
   - Non-infringement argument: The Velora EP engine models non-propagating cantilever beam modal resonators and lumped variable-reluctance/variable-capacitance pickup transducers, containing no string-bridge loop networks.
2. **US7915515B2 (Modartt, active to 2027)**:
   - Claims cover precomputing damped partial coefficients from physical string/soundboard dimensions.
   - Non-infringement argument: Velora EP uses real-time state-space Euler-Bernoulli cantilever modal equations with non-linear electromagnetic/electrostatic pickup integration, not Modartt's soundboard partial computation equations.
3. **AAS (Applied Acoustics Systems) Portfolio**:
   - AAS utilizes lumped-mass and modal physical modeling for electric piano plugins (Lounge Lizard). No active patents claiming the basic Euler-Bernoulli cantilever equation with non-linear pickup equations were identified in US or international registers.
4. **Conclusion for Counsel**:
   - The mechanical physics of tines, reeds, and electroacoustic transducers (Rhodes and Wurlitzer mechanical patents) have long entered the public domain.
   - Fundamental waveguide/modal synthesis patents for bars and non-linear hammer strikes (Yamaha/Stanford) have expired.
   - Clean-room implementation based on open academic physics (Fletcher & Rossing, Bilbao, Chaigne & Doutaut) avoids encumbered proprietary techniques.

### Acoustic piano resonance patents (Google Patents, by holder)

| Patent | Holder | Subject | Status | Expiry | Brazil family |
|---|---|---|---|---|---|
| [US8754316B2](https://patents.google.com/patent/US8754316B2/en) | Yamaha | Per-pitch string signals into resonator loops, attenuation set by damper state | Active | 2032-04-17 | None listed |
| [US9478203B2](https://patents.google.com/patent/US9478203B2/en) | Yamaha | Per-key resonance circuits with panning by key | Active | 2035-01-29 | None listed |
| [CN110192244B](https://patents.google.com/patent/CN110192244B/en) (US10818278B2, unverified) | Yamaha | Delay loops tuned to string pitch, plus a higher loop | Active | 2038-01-12 | None listed |
| [JP7243116B2](https://patents.google.com/patent/JP7243116B2/en) (US10805475B2) | Yamaha | Resonance from combinations of held pitches | Active | 2038-10-05 | None listed |
| [US12039960B2](https://patents.google.com/patent/US20210074250A1/en) | Yamaha | Resonance with an added sound for damper operation across pitches | Active | 2041-04-27 | None listed |
| [US8115092B2](https://patents.google.com/patent/US8115092B2/en) | Yamaha | Piano synthesis from damper, hammer, string, and body models | Active | 2030-06-06 | Not confirmed |
| [US10102837B1](https://patents.google.com/patent/US10102837) | Kawai | Per-key resonance circuits grouped with delays | Active | 2038-04-10 | None listed |
| [US9489933B2](https://patents.google.com/patent/US9489933B2/en) | Casio | Table of resonance strength by pitch difference, sent as note-ons | Active | 2036-02-19 | None listed |
| [US11881196B2](https://patents.google.com/patent/US11881196B2/en) | Casio | Key excitation routed through channels feeding low registers | Active | 2042-04-08 | None listed |
| [US11094307B2](https://patents.google.com/patent/US11094307B2/en) | Casio | Excitation waveforms from recordings at several intensities, windowed | Active | 2039-10-02 | None listed |
| [US7915515B2](https://patents.google.com/patent/US7915515B2/en) | Modartt | Damped partials with coefficients computed ahead from string and soundboard parameters | Active | 2027-10-20 | None listed |

Expired or lapsed, per Google: [US7476797B2](https://patents.google.com/patent/US7476797B2/en) (Kawai), [US9818390B1](https://patents.google.com/patent/US9818390B1/en) (Roland), [US5468906A](https://patents.google.com/patent/US5468906A/en), [US5286915A](https://patents.google.com/patent/US5286915A/en) (Yamaha), and [FR2747496A1](https://patents.google.com/patent/FR2747496A1/en). [US7642445B2](https://patents.google.com/patent/US7642445B2/en) (Kawai) is listed as fee-lapsed, with conflicting dates.

**Findings**
- Resonance is a crowded field outside Brazil. Yamaha, Kawai, and Casio each hold active patents, so export needs its own clearance.
- The Brazil launch looks open, because neither INPI nor the family lists show a matching filing. Only the attorney's opinion can confirm that.
- Roland's search showed no active resonance-generation patent, but the query was narrow.
- Korg and Waseda University are worth a check for CAP-27: [JP7072165B2](https://patents.google.com/?q=JP7072165B2) covers mode decomposition, and JP2019158995A covers resonance generation. Their status was not checked.
- Electric piano physical modeling foundations (Rhodes, Wurlitzer, Miessner, early Yamaha/Stanford waveguide/Hertzian patents) have expired. Active patents from Yamaha and Modartt cover grand piano string/soundboard coupling networks, which do not read on cantilever tine/reed pickup models. Legal counsel review remains mandatory prior to merge and release.

**Limits**
- Status, expiry, and family data come from Google Patents, and most claims were read only in summary.
- Holder searches used the terms "piano resonance". They are a first pass, not a clearance.
- Resonance design was not compared claim by claim with any patent.
