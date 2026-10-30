# IP and Reference Material (modeled keys)

Preliminary research from 2026-10-30, for CAP-24 and CAP-26. It is not a legal opinion. Patent status comes from Google Patents, which marks its status as an assumption. Claim summaries were not checked word for word, except US5468906A.

## Development references for the modeled electric piano

Nothing here ships. The final comparison uses the founder's own recordings.

| Source | Instrument | License | Format | Use |
|---|---|---|---|---|
| [tim.kahn, C_S Fender Rhodes Mark II](https://freesound.org/people/tim.kahn/sounds/65762/) | 73 Stage tine, through a tube preamp, 53 sounds | CC BY 4.0, checked on one sound | Mono, 44.1 kHz, 16-bit | Tine development reference |
| [OldBassMan, Wurlitzer 200a](https://freesound.org/people/OldBassMan/sounds/88000/) | 200A reed, through a transformer and tube preamp | CC BY 4.0, checked on one sound | Mono, 48 kHz, 24-bit | Reed development reference |
| [jRhodes3d](https://github.com/sfzinstruments/jlearman.jRhodes3d) | 1977 Mark I Stage 73 | Samples CC BY-NC 4.0 | FLAC, up to 5 layers | Not used |

- Check the license of each sound before use, because Freesound licenses are per sound.
- Both CC BY sources pass through a preamp, so their tone is not the bare pickup signal.
- jRhodes3d stays out. CC BY-NC grants reproduction only for purposes not "directed towards commercial advantage", and product development is hard to fit under that. It needs the author's written permission to come back.

## Patent search

### Brazil (INPI pePI, searched 2026-10-30)

No electronic piano resonance or modeling filing was found in Brazil.

| Search | Result |
|---|---|
| Class G10H (electronic instruments), all applicants | 32 filings. None covers piano resonance or piano modeling |
| Abstract contains "piano" | 22 filings. None covers electronic sound generation |
| Title contains "piano" | 15 filings. None covers resonance or modeling. PI 7707284-7 "Piano eletronico digital" dates from 1977 |
| G10H, Yamaha | 1 filing: PI 9501409-8, tone signal generator, filed 1995 |
| G10H, Casio | 1 filing: BR 11 2021 002733 5, keyboard instrument, class G10H 1/32 |
| G10H, Roland and Kawai and Korg | 0 filings |
| Modartt, Kawai, Korg, any class | 0 filings |

Limits: unpublished applications cannot be seen, because filings stay secret for up to 18 months. Multi-word abstract and title searches returned broad, unrelated matches, so only the single-word and class searches above count.

### Other countries (Google Patents, by holder)

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
- No search found a patent on tine or reed physical modeling.

**Limits**
- Status, expiry, and family data come from Google Patents, and most claims were read only in summary.
- Holder searches used the terms "piano resonance". They are a first pass, not a clearance.
- Resonance design was not compared claim by claim with any patent.
