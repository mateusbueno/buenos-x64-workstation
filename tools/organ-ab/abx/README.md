# organ-ab/abx: reference-recording capture and blind ABX panel (Stories 11.14 and 2.14; EXT7, NFR20, NFR21)

Sources: `_bmad-output/planning-artifacts/epics.md` Story 11.14 (lines 6474-6494), Story 2.14 (1774-1797),
NFR20/21 (267-268), EXT7 (413); `_bmad-output/specs/spec-arm64-stage-workstation/acceptance-tests.md:9`.

Repo thresholds (cite, do not invent): Story 2.14 requires **>= 16 trials per listener** with randomised order, listener
blind to source, same playback system, loudness-matched excerpts; pass = **no listener above the p < 0.05 binomial
threshold, OR mean rating >= 4/5** (epics.md ~1789-1797). The tool takes these as required arguments, no defaults.
Blanks marked `____` are to be filled by the founder; none are invented here.

## Part 1. Reference-recording capture protocol (Story 11.14 AC1)

Goal: 48 kHz / 24-bit (or higher) recordings of a real tonewheel B-3 (or other tonewheel console) through a real rotary
cabinet, with the same mic setup throughout. Recommended, not blocking (EXT7).

Session record (fill before recording):
| Item | Value |
|---|---|
| Studio / date / engineer | ____ |
| Console (make, model, serial, last service date) | ____ |
| Cabinet (model, e.g. Leslie type) | ____ |
| Sample rate / bit depth (>= 48 kHz / 24-bit) | ____ |
| Usage rights document stored at | ____ |

Mic / line setup (choose and document; keep identical for every take):
- Option A, cabinet mics: horn mic(s) ____ (model, distance ____, angle ____), drum/bass mic ____ (model, distance ____), room mic(s) ____.
- Option B, console line out (dry, no cabinet): ____ (DI model, level ____). Useful as the NFR20 dry A/B against the M-solo.
- Option C, both A and B simultaneously on separate tracks (preferred: allows rotary null analysis).
- Preamp / interface / gain staging ____; photos of every position and a position diagram stored at ____.
- Reference tone / slate at start of each take; no processing, no limiter on recording chain.

Test script (each take logs registration, rotary state, and either the MIDI file name or the manual note list):
1. Single notes across the range (suggest ____ notes, e.g. C1..C7 step ____) on **each drawbar alone**, rotary stop.
2. Standard registrations (list ____, e.g. `88 8000 000`, `00 8800 000`, `88 8600 000`), percussion off.
3. Percussion variants: on/off, soft/normal, fast/slow decay, 2nd/3rd harmonic, per registration ____.
4. Key click: with/without (instrument setting ____), staccato and legato notes.
5. Vibrato/chorus modes V1/V2/V3/C1/C2/C3 and off, on a fixed registration ____.
6. Rotary: **stop**, **slow**, **fast**, and **ramps** (slow->fast and fast->slow, with a marker for the switch moment), on fixed held chords and single notes.
7. Playing excerpts for the ABX (Part 2): ____ short musical excerpts, ~____ s each, covering chords, melodic lines, rotary changes.

Levels: play at a consistent key velocity/touch ____; record at peak ~____ dBFS with >= ____ dB headroom; note the console
volume/expression position per take. Logging may reuse the `tools/organ-ab` MIDI/CSV conventions (do not edit them).

Panel recruitment (Story 11.14 AC2): 3-5 named keyboardists with organ experience; written agreement covering date window
(first playable units: ____), compensation ____, consent to anonymised results; contact data held under the Story 11.3 LGPD rules.

## Part 2. ABX panel protocol (Story 2.14; NFR21)

Parameters (set by founder before the session): listeners `N` = ____ (3-5 per NFR21), trials per listener `T` = ____
(>= 16 per Story 2.14), alpha = ____ (repo: 0.05), mean-rating threshold = ____ (repo: 4).

Design: each trial presents A and B (one organ render, one reference, roles randomised per trial) and X (equal to A or
B, randomised). The listener answers "X is A or B" and rates A and B 1-5 for realism. Excerpts are cycled evenly. Per
listener the plan is balanced (half organ-as-A, half X=A) then shuffled, from a recorded seed.

Statistical rationale and binomial computation:
- Null hypothesis: the listener cannot tell them apart, so each answer is correct with probability 0.5.
- With `c` correct of `n` trials, the one-sided exact p-value is `P(X >= c) = sum_{k=c..n} C(n,k) / 2^n`. Exact integer
  arithmetic is used, no normal approximation.
- Example (T = 16): 12/16 gives p = 0.0384 (listener can tell, alpha 0.05), 11/16 gives p = 0.1051 (cannot). So the
  minimum evidence threshold at 16 trials is 12 correct; fewer trials make it impossible to reach significance
  (at n = 4, even 4/4 gives p = 0.0625), which is why Story 2.14 sets a floor of 16.
- Per-listener test is the story's rule. With 3-5 listeners each tested at alpha, the chance that at least one passes by
  luck alone is about 1-(1-alpha)^N (up to ~23% at N=5, alpha 0.05), which makes a "no listener significant" result
  lenient for the organ and is a known limitation; larger T reduces the lenience by raising power. The pooled p-value is
  reported for information (assumes independent, equally able listeners), not used in the verdict.
- A listener that scores significantly BELOW chance is flagged in the data (check for answer-key or labelling errors).
- Mean rating = mean of the 1-5 rating given to the organ side across all trials and listeners.

Blinding: the plan script writes `key.csv` (A/B sources, X, correct answer) separately from listener sheets, which hold
only listener id, trial, excerpt and blank answer/rating columns. The operator who plays audio holds the key; listeners
never see file names or source labels (use neutral names, strip metadata, identical lengths/fades). Give `key.csv` to the
analyst only after all sheets are returned. Record the seed in the session report.

Level matching: match loudness of every organ/reference pair (LUFS-I per excerpt, tolerance ____ LU, or peak
and RMS check) before the session; document method and measured offsets. Remove hidden cues (silence at head/tail,
sample-rate or bit-depth differences, latency offsets); align start times.

Playback chain: one chain for all listeners (same room ____, interface ____, monitors or headphones ____, level calibrated
to ____ dB SPL), listeners seated identically, no source identification possible from the player UI, listener sets own
repeat count within the limit ____ per trial. Collect LGPD consent before the session.

## Tool: `abx_session.py` (python3, stdlib only)

```
python3 abx_session.py --self-test
python3 abx_session.py plan --seed S --listeners N --trials T --excerpts e1,e2,... --out-dir DIR
   -> DIR/key.csv (hidden) and DIR/sheet_L01.csv ... (listener-facing; fill answer = A|B, rating_A, rating_B = 1..5)
python3 abx_session.py analyze --key DIR/key.csv --answers sheet_L01.csv sheet_L02.csv ... \
       --alpha 0.05 --min-trials 16 --rating-threshold 4
```
(`0.05`, `16`, `4` are the Story 2.14 values, shown as an example; the tool has no defaults.)
Exit code 0 = PASS, 1 = FAIL or INVALID. Verdict logic: INVALID if any answer is missing/malformed/duplicate or a
listener has fewer than `--min-trials` answered trials; otherwise PASS if no listener has p < alpha OR mean organ rating
>= threshold. The report lists per-listener correct/n/p and pooled p. Failed excerpts for engine follow-up (Story 2.14 AC)
are not auto-computed; inspect `key.csv` joined with answers for excerpts with high correct rates.

## AC mapping
| AC | Where |
|---|---|
| 11.14 AC1 recordings, script, logging, mic consistency, rights | Part 1 |
| 11.14 AC2 panel of 3-5, written consent, LGPD | Part 1 (recruitment) |
| 2.14 AC1 randomised, blind, >=16 trials, same playback, loudness-matched, rate + 1-5 rating | Part 2, `plan` |
| 2.14 AC2 binomial p<0.05 / mean >=4/5 verdict, report | `analyze`, binomial section |
