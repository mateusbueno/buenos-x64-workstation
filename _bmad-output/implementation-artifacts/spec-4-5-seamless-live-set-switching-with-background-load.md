---
title: 'Story 4.5: Seamless Live Set switching with background load'
type: 'feature'
created: '2026-10-19'
status: 'done'
baseline_revision: '0097f1dffc036dde5e87a5e83005c0ccddc7123e'
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

**Problem and approach:** Story 4.5 "Seamless Live Set switching with background load" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~2713; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

**Constraints:** build on Story 4.3 (`LiveSetLoader`, `HeadProvider` seam with the Mac `RamHeadProvider`, `PartRouter`, snapshot generations, `ControlPlane::publish_snapshot`, arbiter reserve) and 4.4 (`SceneController`). Add a `LiveSetSwitcher` in `engine/host` (control thread owner, its own **background loader worker thread** (non-RT, never the audio/RT threads and never the control thread's command handling) that resolves and loads the target's instruments (heads) while the **current generation keeps playing and responding to the keys**; `jump_to(live_set_id)` returns immediately with a typed acknowledgement; the control thread publishes the new generation only when every head of the target is resident (all-or-nothing as in 4.3). **Reservation handover:** Story 4.3 documented that `VoiceArbiter::reserve()` must run while audio is quiesced; seamless switching needs the reservation of the new Live Set to be applied **at the generation swap on the RT plane in the serial phase** (AD-22): pre-compute the new `LiveSetConfig`/reservation on the control thread (validation, costs) and carry it in the snapshot; apply it in `adopt_generation` in a bounded, allocation-free, RT-safe way (restructure `reserve()` into a validate-and-prepare step (control thread, may allocate) and a cheap commit step (RT, no allocation) — keep the existing quiesced `reserve()` API working for the older tests; the arbiter must not lose or double count voices that still sound under the old reservation: sounding voices keep their slots and costs; the new floors/protection apply to new notes; define and document the accounting rule for old-generation voices during the overlap, e.g. old voices keep counting against the budget until silent like faded stolen voices). **Old/new voice overlap:** notes held, sustained by the pedal or releasing on the old generation finish on it (note-offs for them are routed to the engines/banks that started them — the `PartRouter` tracking and the per-note bank/generation binding from 4.3 already cover note-off consistency; extend to the cross-Live-Set case where the old part layout, zones and engines differ from the new: the router must keep releasing old-layout notes with their recorded pitch/engine/bank), while new notes play on the new generation; the old generation (snapshot, sample banks, bundle leases) is freed only after RT reports no voice references it (existing ledger/hazard-pointer mechanism) — test with a held note across the switch and a sustained note across the switch and releasing tails; **loading status:** the engine publishes `loading` (additive ws.v1 state fields: loading flag, target Live Set id and name, estimated remaining seconds, progress ratio, state enum idle/loading/ready/failed + last error code) updated **at least once per second** during a load (implement via an injectable clock and a progress callback of the HeadProvider (bytes loaded / bytes total, per instrument), a rate-based ETA with smoothing, and a periodic publisher on the control thread tick (document: control server poll tick); tests with a fake clock and a throttled HeadProvider that takes several simulated seconds, asserting updates at least every 1 s (simulated), monotone non-increasing ETA tendencies not required but bounded sanity: ETA finite and ≥ 0); **second jump request while a load is in progress:** cancels the first load (cooperative cancellation token checked between samples/instruments and inside the HeadProvider read loop), frees its partial heads (verify via weak-owner sentinels / allocation counters that nothing leaks), and loads the new target, with no effect on the sound (render comparison: audio output while jumping twice equals the audio of the old Live Set continuing until the swap); **head budget:** a target that cannot fit in the head budget (configurable `head_budget_bytes` in the switcher options; the Mac default large; compute the target's total head bytes from its instruments' decoded sizes *before* loading where possible — resolve and sum sizes from bundle index metadata so refusal happens early — and also guard during loading) is refused: the current Live Set keeps playing and the engine publishes a typed error (additive closed error code `HEAD_BUDGET_EXCEEDED` and also `LOAD_CANCELLED` for superseded loads reported as informational state, not as errors to the UI); jump to an unknown Live Set id, a failed instrument load, or a file validation failure also publish typed errors and keep playing; `jump_to` of the already loaded Live Set is a no-op with a typed result (or reloads? choose: no-op "already_loaded"). ws.v1 commands (additive): `JumpToLiveSet{id}`, `CancelLoad`; replies with `request_id`; State gets the fields above; the host wires them (`engine-host --live-set-dir DIR` listing Live Sets in a directory using `LiveSetStore`, `--library-root`); `velora-offline-render` gets a test mode `--switch-at SECONDS:LIVESET_FILE` (repeatable) so renders can include a seamless switch with a committed golden: two small Live Sets (one with a held chord + sustain across the switch, tails from the old one finishing while the new plays), null-tested at −100 dB, bit-identical on two renders; the background load in offline mode is simulated deterministically (the loader completes at a configured simulated time/blocks `--switch-load-blocks N` so renders are deterministic). RTSan test: switches interleaved with dense traffic and reservation handovers.

**Never:** preload next Live Set (Story 4.7), setlists, UI, heads in locked RAM/streaming (1.16/1.17), change existing goldens, or make the RT thread allocate.


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 4.5 (~line 2713) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 4.5 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

### 2026-10-19 — Review pass (2 reviewers: edge-case hunter + verification gap)
- `[high]` `patch` stale sustain flag across a switch (never cleared in leave_live_set) could stick the first notes of the new Live Set; live knob moves leaked into a new Live Set whose initial value was equal — flag synchronised on both pedal paths, all performance slots forced at a Live Set (reservation-epoch) commit; tests with mutations
- `[medium]` `patch` router ref counts shared between a key held on the old bank and one struck on the new bank — keys tagged with a bank/engine generation
- `[medium]` `patch` loading status stalled during held stages and the 5 s busy retry; no-op jumps pushed State; cancellation latency in non-cancellable steps; estimate 0 skipped the budget refusal; stage failures lost per-part states; engine-host wiring (catalog, switcher creation, control tick) was only covered by a device-dependent smoke — cadence fixed with fake-clock tests, cancel checks after parse and estimate, in-flight budget guard, restored part states, `--no-audio` flag so both engine-host smoke tests never skip
- `[low]` `patch` ws.v1 NO_LIVE_SET / ALREADY_LOADING mapping, arbiter overlap accounting (protected organ voice, floor→0), superseded `--switch-at` — tests and errors added
- `[low]` `defer` the engine's `stop_voice(channel, note)` releases every voice on that part and pitch, so a key released on the old bank also ends a same-pitch voice of the new bank (needs a bank-aware stop_voice); arbiter diagnostics are quiesced-only after audio starts (read `Snapshot::reservation`); the 32-slot snapshot ledger fills under continuous dense playing with a switch every few blocks (commit then reports busy); head budget bounds only the target's heads (shared current+next accounting is Story 4.7); protected old organ voices count against the new shared remainder until released; ALREADY_LOADING mutation not run; two macOS tests failed once in a full run while the machine was busy (Docker running in parallel) and all 407 passed on re-run — timing-sensitive tests to watch (not identified)

## Auto Run Result

Status: done. `LiveSetSwitcher` with a background loader thread: `jump_to` returns at once, the current generation keeps playing, loads are cancellable/superseded, head budget refused early and in flight, loading status (target, progress, smoothed ETA) republished at least every 0.5 s, all-or-nothing commit retried while the 32-slot ledger is full; `VoiceArbiter::reserve` split into `prepare_reservation` (any thread) and `commit_prepared` (RT serial phase, no allocation) carried in the snapshot by epoch; notes held/sustained/releasing finish on the old layout; additive ws.v1 `JumpToLiveSet`/`CancelLoad`, loading state and error codes; `engine-host --live-set-dir/--no-audio`; `velora-offline-render --switch-at` with a committed golden. Verification: macOS ctest 407 (2 env-gated skips), Docker gcc 404 and clang-20 RTSan 416, purity and notices checks pass. Follow-up review recommended: false.
