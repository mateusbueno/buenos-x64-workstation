# SFZ scope for the reference corpus (Story 13.1)

**Status: PROPOSED, awaiting founder ratification.** This record was written by an unattended run. It records the ordering already
laid out in Epic 13 as the proposed decision. It is not a founder decision, and nothing here may be cited as approved until the
founder ratifies it (see "Ratification" at the end). No importer, engine, corpus audio or tolerance was changed to produce it.

## Why a decision is needed

FR18 describes a deliberate v1 opcode subset. FR20 requires that every reference-corpus library imports and plays with mapping intact.
Story 5.4 ran the corpus on real libraries: 9 of 10 fetched libraries are rejected on import because they use opcodes outside the
Story 5.1 subset; only VSCO 2 CE imports (75 of 75 files) (Story 5.4 spec, `warnings` and Acceptance status). Both requirements cannot
hold unchanged, so each opcode tier needs a verdict before Stories 13.2-13.9 start.

## Evidence base

- Story 5.4 spec (`_bmad-output/implementation-artifacts/spec-5-4-reference-corpus-imports-and-plays-with-mapping-intact.md`),
  `deferred` entry 1 (opcode families by number of files, "each is a feature of the engine as well, not only the importer"),
  entry 3 (Pianobook packs) and the Review Triage Log (VSCO null-test causes).
- `tools/corpus/manifest.json` (10 included entries, 6 excluded; library ids used below) and `tools/corpus/README.md`.
- Per-library rejected-opcode histograms are produced by `python3 tools/corpus/corpus.py run` (`unsupported_histogram` in
  `report.json`; not committed). Counts below are import-issue lines from the run of 2026-10-27 recorded in
  `build/corpus-report-rest/report.json`, so they measure how often an opcode appears, not how many files it blocks, unless the text says "files".
- `formatlib/src/sfz_import.cpp` (read-only): the importer rejects unsupported opcodes, headers and values as `unsupported_feature`
  issues and allow-lists inert opcodes (`ampeg_dynamic`).

## Verdicts

Verdict values: **in scope** (will be implemented, story named), **deferred** (decided later on measured need), **out of scope**
(never imported; files skipped). Each tier has exactly one verdict.

| # | Tier | Verdict | Story | Reason |
|---|------|---------|-------|--------|
| 1 | Importer rules (value rules for out-of-range note values, `<curve>` header stored inert, inert opcodes) | in scope | 13.2 | Smallest effort, no engine change. Salamander is rejected on `lokey`/`hikey` invalid values and `<curve>`; kamoepiano301 on `lokey=c-2` and five `<curve>` headers; Osiris, Weresax, Headroom and Steel Drum also carry `<curve>` (Story 5.4 deferred 1 and 3; report histograms). A harmless value should not reject a whole library, and sfizz's own fallback is the specification. Risk: low (importer only); `ampeg_dynamic` stays inert only until `*_oncc` lands (13.5). |
| 2 | Note-on scalars (`amp_veltrack`, `amp_velcurve_N`, `amplitude`, `group_volume`, `ampeg_vel2*`, `offset_random`, `delay`, velocity crossfades) | in scope | 13.4 | Widest reach by files: `amp_velcurve_<N>` in 33 of 39 Meatbass and 27 of 48 Bear Sax files; `fil_*`, `group_volume`, `gain_cc<N>` in 289-373 Sonatina files each (Story 5.4 deferred 1). Evaluated once at note-on, so it fits the render thread without per-block state. Effort: medium (bundle `format_version` minor bump, bit-identical existing renders). Risk: medium (velocity mapping must match sfizz; checked by null test). |
| 3 | Trigger and release modes (`end`, `trigger=first/legato/release_key`, `rt_decay`) | in scope | 13.3 | Manthey Klaviano is rejected on `end` (153 regions, inert in 152 and truncating in one: `samples/70_p1.wav`) and `trigger=release_key`; the importer maps no such mode and the engine has no sample end (Story 5.4 deferred 3). Salamander also uses `rt_decay`. Effort: small to medium. Risk: low to medium (held-key state on the control path, no render-thread allocation). |
| 4 | CC at voice start (`locc`/`hicc` gates, `*_oncc`/`*cc` and `*_curvecc` on envelope, offset, amplitude, pan, tune, `set_cc`/`set_hdcc`, pseudo-CCs) | in scope | 13.5 | The largest blocker: nearly every Karoryfer, Salamander, Osiris and Headroom file uses it, kamoepiano301 uses velocity-as-CC (`cutoff_oncc131` x56, `cutoff_curvecc131` x61) (Story 5.4 deferred 1 and 3). Sampled at voice start only; continuous per-block modulation is a documented limit and such probes stay out of null picks with a manifest reason. Effort: large. Risk: high (touches the per-part CC model and the bundle format); this is the critical-path story for most libraries. |
| 5 | Choke groups (`group`, `off_by`, `off_mode`, `off_time`, `polyphony`, `note_polyphony`) | in scope | 13.6 | About 25 of 48 Bear Sax, 21 of 39 Meatbass and 135 of 747 Sonatina files (Story 5.4 deferred 1); also Osiris, Headroom, Weresax, Salamander. Needs voice-pool interaction (cutting, fast release) under existing stealing rules. Effort: medium. Risk: medium (parallel render must stay bit-identical; sfizz's stealing and `off_mode` timing may diverge from the engine and must be documented if so). |
| 6 | Pitch bend and CC-triggered voices (`bend_up`/`bend_down`/`bend_step`, `on_locc`/`on_hicc`, `hikey=-1`) | in scope | 13.7 | Scarypiano, Weresax, Meatbass reject on `bend_up`/`bend_down`; Salamander's pedal-action region needs a controller-triggered voice and is rejected on `lokey=-1`/`hikey=-1` ("a region that never plays by key", Story 5.4 deferred 1). Effort: medium. Risk: medium (voice started from a controller, smoothing without allocation). Depends on 13.2 and 13.5. |
| 7 | Per-voice filter (`cutoff_oncc`/`cutoff_cc`, `fil_keytrack`, `fil_keycenter`, `fil_veltrack`, `resonance_cc`, second filter) | in scope | 13.8 | `fil_keytrack/keycenter/veltrack` and `gain_cc<N>` appear in 289-373 Sonatina files each; kamoepiano301 and Headroom Piano open the filter with touch (Story 5.4 deferred 1 and 3). Per-voice coefficients cost CPU on the target, so the budget must be measured first. Effort: large. Risk: high (NFR8 voice floors, filter stability at extremes, null-test tolerance). Depends on 13.5. |
| 8 | Region EQ (`eqN_*`) | deferred | 13.9 (conditional) | Needed by Sonatina (`eq1_bw` etc.) and Karoryfer files, but those files are also blocked by the other tiers; the real remaining need is only measurable after 13.5-13.8 land. Effort: medium per band, CPU cost per voice unknown. Decide after 13.8 from the corpus report: if files remain blocked only by this tier, move it to in scope; otherwise mark it out of scope and keep the affected files skipped in the manifest with the reason. |
| 9 | LFO/EG modulation (`lfoNN_*`, `egNN_*`, `amplfo_*`, `pitcheg`, `fileg`) | deferred | 13.9 (conditional) | Needed by Sonatina (`amplfo_*` 256, `lfo01_*`), Meatbass and Bear Sax (`eg06_time1_oncc*` in the thousands of issue lines), Weresax (`lfo*`) (Story 5.4 deferred 1). Largest effort and risk of all tiers (modulation matrix, LFO phase, smoothing, CPU cost per voice, 1 versus 3 worker partitions bit-identical). A design note against sfizz must precede any code (Epic 13, Story 13.9). Same decision rule as tier 8. |

Related item that is not an opcode tier: **random-layer parity** (`lorand`/`hirand`, Story 13.10) is in scope. It does not widen the
opcode subset. It decides how the 19 of 75 VSCO files that cannot be null-tested against sfizz are verified (Story 5.4 triage, cause 6),
without changing the production random generator.

## Effect on each library

This table follows Epic 13's own mapping; it is a plan, not a result.

| Library | Blocked today by tiers | Becomes importable after |
|---------|------------------------|--------------------------|
| VSCO 2 CE | none (imports 75 of 75) | already; 13.10 adds random-layer checks |
| Manthey Klaviano | 3 (`end`, `release_key`) | 13.3 |
| kamoepiano301 | 1, 3-4, 7 | 13.5, 13.8 |
| Scarypiano, Osiris, Steel Drum | 1, 2, 4, 5, 6 | Epic 13 names 13.5 for these; the histograms also show bend (tier 6), groups (tier 5) and velocity curves (tier 2), so expect 13.4, 13.6 and 13.7 as well |
| Salamander | 1, 4, 5, 6 | 13.7 (plus 13.2, 13.5) |
| Headroom Piano | 4, 5, 7 | 13.8 |
| Meatbass, Bear Sax, Weresax | 2, 4, 5, 6, 8-9 | after 13.9's decision |
| Sonatina | 2, 4, 5, 7, 8-9 | after 13.9's decision |

## Rule for out-of-scope tiers

If a tier is, or later becomes, **out of scope** (tiers 8 and 9 if 13.9 is cancelled):

1. A library file that needs it is skipped by the importer with `unsupported_features` naming the offending opcodes (FR22). The
   rest of the batch imports.
2. The corpus manifest lists that file under `skip_files` with the same reason (`path` + `reason` naming the tier and opcodes).
3. The manifest lists **only** files skipped for tiers marked out of scope. A file that fails import for any other reason is a
   failure of the corpus job, never a manifest entry. A stale `skip_files` entry fails the job (existing rule, README).
4. The Epic 5 corpus requirement is restated as "imports and plays with mapping intact for every file in a supported tier".

At the time of writing no tier is out of scope, so no file is added to `skip_files`. The 9 libraries that fail import today keep
failing honestly until their tiers land; hiding them in the manifest would break rule 3.

## Consequences

- FR18 is amended in `epics.md` to name the subset by tier (the v1 subset plus tiers 1-7, with tiers 8-9 conditional).
- FR20 is restated in `epics.md` as above.
- Epic 13 ordering: 13.2, 13.3, 13.4 and 13.6 may start in parallel (blocked only by this record); 13.5 needs 13.2 and 13.4; 13.7 needs
  13.2 and 13.5; 13.8 needs 13.5; 13.9 needs the decision after 13.8; 13.10 is independent. No story is cancelled by the proposal.
- Founder-owned inputs remain outside this decision: three further Pianobook packs (2 of at least 5 delivered) and the licence
  confirmation for Pianobook packs and for Sonatina (Sampling Plus 1.0) (Story 5.4 deferred 3 and 4).

## Ratification

Awaiting the founder. To ratify: change the Status line to "ratified by <name>, <date>", or to amend, change a verdict and update
`epics.md` (FR18, Epic 13) to match. Until then the proposal governs planning only; no story is cancelled on its strength.
