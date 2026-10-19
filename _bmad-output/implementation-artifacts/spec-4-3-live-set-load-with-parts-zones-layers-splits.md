---
title: 'Story 4.3: Live Set load with 8 parts, zones, layers and splits'
type: 'feature'
created: '2026-10-19'
status: 'done'
baseline_revision: '76391a4118422996dfb6634564f40e85cb8deb9a'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-4-context.md'
  - '{project-root}/engine/core/README.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem and approach:** Story 4.3 "Live Set load with 8 parts, zones, layers and splits" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~2593; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

**Constraints:** implement in `engine/host` (control-thread side) a `LiveSetLoader` that takes a validated `LiveSet` (Story 4.2 `LiveSetStore`/`velora.liveset.v1`) and builds a new snapshot generation for the engine: for each part resolve its timbre `(library_id, instrument_key)` through the Story 1.14 `LibraryResolver` (typed result; unresolved library or instrument => that part is silent with a "missing library" state, never substituted; an empty timbre on an organ part is fine), load the instrument data into RAM through the existing sample-source/bundle loader (`engine/host/src/sample_bundle_loader.*`; heads/streaming from Stories 1.16/1.17 are board work and NOT in scope: the Mac path loads whole instruments in RAM; keep a clear seam `HeadProvider`/loader interface so 1.16 can plug in; document) , build one engine configuration per part (engine type from `EngineData`: Sample, Organ bound; FM/VA reserved types => part silent with "engine not available", never substituted), apply the part's insert chain, send levels, output Main/Aux, volume and engine sound parameters (`param_id` values) through the existing `ControlPlane` snapshot mechanism (Story 2.1/2.7) — extend the snapshot's per-part structure with whatever is missing (low key, high key, octave, transpose, keyboard on/off, volume, engine type, instrument handle/pointer, load state) additively; **swap in the generation only when every part's instrument is resident** (all-or-nothing publication; a load that fails halfway publishes nothing and returns a typed error; the previous generation keeps playing); sound-engine routing: the renderer must run each part's engine instance(s) with that part's instrument (SampleEngine currently binds per channel/engine instance with one instrument — add per-part sample engine instances or per-channel instruments as the design needs; document the decision; keep the existing Story 1.15 null tests bit-exact for the single-instrument path); **zones/layers/splits:** each part with `keyboard on` sounds only if the played key — the key *as played*, i.e. before the part's octave/transpose are applied (octave/transpose apply to pitch, NOT to the zone test) — is inside [low_key, high_key]: overlapping zones layer and adjacent zones split; implement the input-side routing in `engine/core` (BlockRenderer or a new `PartRouter` stage in the serial phase) that takes the incoming UMP note-on/off (channel 0..15 from the keyboard source; define the "keyboard channel" convention: the physical keyboard events arrive on a keyboard channel and are fanned out to parts by zone; document how it relates to the existing channel==part addressing used by param events) and generates per-part note events with transposed pitch; **note-off consistency**: a note-off must go to exactly the parts that received the note-on (track per-key the set of parts it was fanned to, so a Live Set/zone/keyboard-flag change while the key is held does not hang or orphan notes); a part with the keyboard **off** receives no new notes while its sound keeps releasing any notes held before; sustain pedal and CC fan out to all keyboard-on parts; pitch bend / mod wheel fan-out is left as pass-through to all parts (document); also generate octave/transpose clamping to 0..127 (notes that transpose out of range are dropped, with a counter); organ parts use the same fan-out; arbiter: the whole Live Set's reservations (FX budget, organ+rotary, floors) are computed from the loaded parts and applied via `VoiceArbiter::reserve` at load (the sample floor 256 needs ≥ 320 slot capacity per existing rules — keep whatever the arbiter requires; if a Live Set has no sample part, the floor is zero); render tests (offline, deterministic): a **piano + pad layer** (two sample parts, overlapping full-range zones, both sound), a **bass/piano split at B2/C3** (low zone ends at B2 = MIDI 47, high zone starts at C3 = MIDI 48; verify with notes on both sides and exactly at the boundary), a part with the **keyboard off** (silent for new notes, releases held notes after it was switched off while a note was held), transpose/octave pitch checks (zone test uses the played key), missing-library part silent while others play, FM part silent, atomic publication test (fault injection: second part fails to load => no generation published, old one still active), the Pro-sized 16-part Live Set loads only where the tier allows (part_count passed in; v1=8) — use small synthetic bundles built with `BundleWriter` and the synthetic SFZ fixtures from the librarian tests as library content in a temp library root laid out per Story 1.13/1.14 (`<root>/lib/<library_id>/current`). Add a thin CLI/engine-host option `--live-set FILE --library-root DIR` that loads a Live Set at start (macOS dev app) and a `velora-offline-render --live-set FILE --library-root DIR` mode so Live Sets render offline from MIDI (deterministic) with a committed golden of a layer+split Live Set (null-tested −100 dB, two renders bit-identical). RTSan test: the fan-out and note tracking under dense traffic with live zone/keyboard-flag changes (published via new generations).

**Never:** implement scene recall (4.4), seamless Live Set switching/background load/preload (4.5, 4.7), heads/streaming/locked RAM (1.16/1.17), setlists, UI, MIDI input hardware paths, or change existing goldens.


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 4.3 (~line 2593) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 4.3 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Result notes

- Implemented as described in engine/core/README.md, engine/host/README.md and tools/offline-render/README.md (Story 4.3 sections).
- **Limitation: 16 parts.** `core::params::kPartCount` is 8. A Pro 16-part Live Set is rejected with `kPartCountUnsupported` (and a 16-part file on a v1 tier with `kInvalidLiveSet`); it does not load on this build.
- Pitch bend, mod wheel and every controller other than the sustain pedal are ignored events in Live Set mode (no engine reads them yet); they are not passed to the parts.
- The loader must not run while the audio callback renders (`LiveSetLoaderOptions::is_rendering`, refusal `kNotQuiesced`); background load and seamless handover are Stories 4.5/4.7.


## Review Triage Log

### 2026-10-19 — Review pass (2 reviewers: edge-case hunter + verification gap)
- `[high]` `patch` audio-thread use-after-free risks: (1) leaving Live Set mode left per-part bank pointers into freed banks, the channel→engine mapping and transposed fanned notes; (2) voices that started before a pool-exhaustion mid-note never referenced their generation; (3) release-trigger regions looked up in a rebound bank with the old note's state — banks cleared, mapping restored, router notes released, generation bound on every applied note, bank recorded per held note; sentinel-owner lifetime tests; mutation checks of two fixes
- `[medium]` `patch` router stored engine ids in uint8_t (kTestTone 0xF000 truncated) and released a shared (part,pitch) on the first key release — full uint32 ids, refcount per (part, pitch, engine)
- `[medium]` `patch` loader: late publish failure cleared instead of restoring the previous reservation; loading during rendering unguarded; non-finite scalars; engine-host published the dev config before a failing --live-set — exact restore, `is_rendering` guard (`kNotQuiesced`), clamping, ordering fixed; kBusy path tested
- `[medium]` `patch` untested paths: engine-remap parameter sync, pedal-up after keyboard-off, waiver flags with non-zero floors — tests added
- `[low]` `defer` Pro 16-part Live Sets are rejected with `kPartCountUnsupported` (engine carries 8 parts, `params::kPartCount`) — widening is a larger change; pitch bend/mod wheel/non-pedal CCs remain ignored events in Live Set mode (no engine reads them yet); loader NaN/out-of-range sanitising unreachable behind validation and untested; reservation must run while audio is quiesced (background load/handover is Stories 4.5/4.7); default arbiter budget and 340-voice sample capacity are placeholders until board calibration; the organ generator runs for all 8 parts

## Auto Run Result

Status: done (Pro 16-part loading deferred). `LiveSetLoader` (control thread) resolves timbres through the Story 1.14 resolver, loads whole instruments into RAM behind a `HeadProvider` seam, builds all-or-nothing snapshot generations, applies insert chains/sends/outputs/volume and arbiter reservations; `PartRouter` fans keyboard events out by zone (layers and splits; zone test on the played key, octave/transpose on pitch; keyboard-off parts keep releasing; note-off consistency; sustain fan-out); per-part `SampleBank`s let each part have its own instrument; `engine-host --live-set/--library-root` and `velora-offline-render --live-set` with a committed layer+split golden (−100 dB). Verification: macOS ctest 345 pass (2 env-gated skips), Docker gcc 344 and clang-20 RTSan 354, purity and notices checks pass. Follow-up review recommended: false.
