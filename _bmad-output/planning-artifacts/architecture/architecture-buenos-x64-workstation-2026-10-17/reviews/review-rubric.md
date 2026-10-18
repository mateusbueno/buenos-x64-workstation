---
reviews: ../ARCHITECTURE-SPINE.md
lens: good-spine rubric (divergence points, enforceable rules, safe deferrals, capability coverage, operational envelope)
date: 2026-10-17
inputs:
  - ../ARCHITECTURE-SPINE.md
  - ../.memlog.md
  - ../../../../specs/spec-arm64-stage-workstation/SPEC.md (+ companions, + .memlog CAP-19)
excluded: HARDWARE-INTERFACES.md (listed companion, not yet written; absence not flagged)
---

# Rubric Review — ARCHITECTURE-SPINE (ARM64 Stage Workstation)

## Verdict

The realtime core is a strong spine. The single-writer, RT-plane, hexagonal-core and license rules are well chosen and mostly enforceable. It is **not yet build-ready as a consistency contract** for three reasons:

1. The central AD-1/AD-2 split leaves open which state class each parameter belongs to, and every client will hit that question.
2. Nothing governs version skew across the units that ship and roll back on their own: the desktop app, firmware A/B slots, MCU firmware and library bundles.
3. The operational envelope is only sketched in the Structural Seed. Security operations, factory provisioning and update delivery are irreversible once units are fused and shipped, and they have no rule.

Rubric scores:

| # | Criterion | Result |
|---|---|---|
| 1 | Fixes the real divergence points between units | Partial. Process, engine, IPC and dataset seams are covered. The gaps are the parameter class, version skew, edge-conversion semantics (MCU → UMP) and voice-buffer ownership in parallel render |
| 2 | Every Rule is enforceable and prevents its divergence | Mostly. AD-3 is unenforceable as written, AD-6 is ambiguous on output buffers, and the AD-7 wording reads as a cap |
| 3 | Nothing Deferred can let units diverge | Fails on one item. "Partner licensing … AD-14 already supports it" is not true without a device-identity registry that has to be captured at the factory now. The boot-sequencing deferral hides an engine↔librarian boot dependency |
| 4 | Covers every spec capability (CAP-1..19) | Yes, nominally. CAP-1..19 are all mapped. CAP-12 (delivery to shipped units), CAP-14 (bulk transfer protocol), CAP-18 (contact timing through UMP) and CAP-17 (catalog/bundle crash consistency) have thin governance |
| 5 | Operational envelope decided, deferred or open | Fails. Environments and CI appear only as a seed diagram. There are no ADs, deferrals or open questions for update delivery, signing-key custody, dev/prod key separation, anti-rollback, device registry, RMA, observability metrics or CI gates |

---

## Findings

Severity: **critical** = irreversible or a cross-unit break that ships; **high** = two units will very likely diverge; **medium** = probable friction or rework; **low** = wording or hygiene.

### F1 — Security operations and factory provisioning have no rule (critical)

- **Where:** AD-14, Structural Seed (Factory station), Deferred ("Partner licensing … AD-14 already supports it").
- **Problem:**
  - The seed says the factory station does "OTP fuse + key provisioning". Nothing decides:
    - who generates the per-device key;
    - whether its public half or ID is recorded anywhere;
    - who holds the secure-boot, RAUC-signing and Ed25519 vendor private keys, and how;
    - whether dev units and dev images use keys separate from production;
    - whether OTP anti-rollback counters are used, and how they interact with RAUC A/B rollback;
    - how a leaked vendor key is revoked or rotated (the firmware holds one public key).
  - Partner licensing works off-device: a content key gets wrapped for a specific unit. That needs a vendor-side registry of device identities, captured when the unit is fused. Units shipped without that registry entry can never receive partner libraries. The deferral therefore lets the factory station and the future licensing server diverge permanently.
  - RMA is uncovered. Replacing the SoM changes the device key, and every wrapped factory library on the sealed NVMe becomes unplayable.
- **Fix:** add **AD-22 Provisioning and key custody**:
  - (a) The device key pair is generated in OP-TEE at the factory station. The device ID and public key are exported to a vendor device registry (append-only and backed up) before the unit leaves the station.
  - (b) There are three key sets with disjoint roots: dev, factory-test and production. Dev images refuse production-signed bundles and the reverse.
  - (c) Production signing keys live offline or in an HSM/YubiHSM, under a documented two-person or founder-only ceremony.
  - (d) The firmware holds a vendor key list with an index, so keys can be rotated by firmware update. Decide on the anti-rollback counter policy.
  - (e) RMA: a SoM swap re-wraps factory libraries from the registry, or the factory content is re-installed.
  - Add an open question on HSM choice and cost. Rewrite the Deferred line as "licensing server deferred; device registry captured from unit #1".

### F2 — Parameter class (structural vs performance) is not decided per parameter (high)

- **Where:** AD-1, AD-2, AD-8, AD-21.
- **Problem:**
  - AD-2 makes "knobs, drawbars, CC" performance state owned by RT. AD-1 makes programs, parts and FX routing structural, written by the control thread.
  - Most parameters can be reached both ways: reverb size, filter cutoff, drawbar 3 and arpeggiator rate can each be set by a panel knob (an event to RT) and by a touchscreen edit or desktop app (a command to the control thread).
  - With no rule, `ui` sends a command and the control thread bakes a new snapshot, while RT still holds a live override from the knob. One parameter ends up with two owners, which is the divergence AD-1/AD-2 exist to prevent.
  - AD-21 classifies tempo and transport, but arpeggiator pattern, rate and on/off are unclassified.
  - Snapshot swap on program change does not say whether performance state resets to the new program's stored values.
- **Fix:**
  - Add to AD-2: "Every parameter has exactly one class, declared in a parameter registry in `proto/` (id, class, range, UMP mapping). Performance-class parameters are changed only by events into the RT queue, whatever the source; ui and desktop edits are translated to the same events. Structural-class parameters are changed only by control-thread commands. On snapshot swap, RT re-seeds performance state from the snapshot's stored values."
  - Classify the arpeggiator parameters explicitly in AD-21.

### F3 — No version-skew or rollback compatibility rules (high)

- **Where:** AD-12, AD-13, AD-16, AD-18; formatlib.
- **Problems (each one is a separately released or separately rolled-back unit):**
  1. **Desktop app ↔ firmware.** The desktop app is released independently and talks to many firmware versions. `ws.v1` has no wire-compatibility rule (additive-only fields, no renumbering, reserved tags) and no capability or version handshake on connect.
  2. **A/B rollback ↔ forward migration.** AD-12 migrates data forward on load. AD-18 and CAP-17 require booting back into the previous firmware. The data partition, holding the SQLite catalog schema and migrated programs, is shared across slots. After a rollback, the old firmware meets a newer catalog schema. Protobuf unknown-field preservation does not help SQLite.
  3. **Library bundle format.** Bundles are written by both `librarian` and the desktop app through formatlib, but AD-12 does not cover the bundle format. A newer desktop app can produce bundles that older firmware cannot read.
  4. **MCU firmware ↔ SoC.** AD-16 has the SoC flash MCU firmware from the A/B image. Nothing covers protocol version negotiation, rollback (slot B rolls back but the MCU keeps the newer firmware), or a power cut mid-flash (CAP-17 covers only "firmware update"; a bricked keybed MCU is a bricked instrument).
- **Fix:**
  - Extend AD-12 to cover programs, config, catalog and bundle format. Every format carries a version. Readers accept N and N−1 at minimum. Schema migrations that cannot be reversed run only after RAUC marks the new slot good, and the catalog stays rebuildable from bundles.
  - Add to AD-13: `ws.v1` evolves additively only. Breaking changes mean `ws.v2`. A connect handshake exchanges firmware version, schema version and feature flags, and the desktop app refuses or degrades on mismatch.
  - Add to AD-16: each MCU has an immutable bootloader with dual-bank or recovery mode, and a protocol version in the hello frame. On every boot the SoC flashes the MCU when its version does not match the running slot's bundled firmware, so rollback is automatic.

### F4 — Update delivery to field units has no rule (high)

- **Where:** AD-18, CAP-5, CAP-6, CAP-12; the spec says no Wi-Fi or Bluetooth.
- **Problem:** RAUC A/B makes updates safe, but nothing says how a bundle reaches a shipped unit with no radio: over the desktop app, a USB-A stick, or both. The spine also does not say which process receives it and hands it to RAUC, how update and factory-library content are distributed (vendor download site, channels such as beta and stable), or whether factory-library updates use the same path. FM and VA (CAP-5/6) exist on v1 units only through this path. Left open, the desktop app, sysd and librarian can each grow their own update intake.
- **Fix:** add an AD, or extend AD-18:
  - Updates arrive only as RAUC bundles signed with the production key, through two intakes: the desktop app over the AD-13 protocol, and a USB-A stick detected by `sysd`.
  - `sysd` is the sole update intake and RAUC caller.
  - Factory-library updates are new immutable bundles taken in through `librarian` (AD-11), never inside RAUC.
  - Defer the vendor distribution site and the channel policy, with a trigger: before the first post-launch update.

### F5 — Parallel render: output buffers and voice allocation are ambiguous (high)

- **Where:** AD-5, AD-6, AD-7.
- **Problem:**
  - AD-5 renders the voices of every part in parallel across workers. AD-6 lets a voice write "its own … output buffer".
  - If engine authors read that as the part bus, two workers summing voices of the same part race. That is exactly the high-polyphony-only race AD-6 claims to prevent.
  - Voice allocation and stealing from the shared pool (CAP-3) is not assigned to a phase. Workers that allocate concurrently would race on the pool.
  - AD-7's "within its guaranteed floor" reads as a cap, but floors are minimums.
- **Fix:**
  - AD-6: each worker accumulates into its own per-part scratch buffers, which are summed into the part bus after the barrier. No voice writes a shared bus.
  - AD-5: each block runs a serial pre-phase on one RT thread to apply events, run the arpeggiators and allocate or steal voices. Then comes parallel voice render, the barrier, per-part FX and the mix.
  - AD-7: change the wording to "is guaranteed at least its floor; surplus is allocated by the pool".

### F6 — Edge-conversion semantics MCU → UMP are underspecified (medium)

- **Where:** AD-8, AD-16, CAP-18.
- **Problems:**
  - **Multi-contact onset (CAP-18).** The organ engine needs per-key contact timing. AD-8 says engines consume only UMP, but does not say how contact timing travels in UMP. Options include per-note controllers / note attributes, or an extra per-note controller per contact closure. The keybed reader and the organ engine will each invent an encoding.
  - **Velocity curve location.** Calibration lives in sysd config, but nothing says whether the curve is applied in the MCU, in the keybed reader or in the engine. The MCU and engine teams can double-apply it or skip it.
  - **Clock domains.** MCU timestamps are in the MCU clock. The rules do not cover how they map to device-monotonic nanoseconds, or how those nanoseconds map to sample frames, given that the codec crystal (master) drifts against CLOCK_MONOTONIC. The keybed reader, the DIN/USB MIDI adapters and the arpeggiator need one mapping.
- **Fix:**
  - Add to AD-8: the MCUs send raw contact timestamps only. The keybed reader in engine is the only place that applies calibration and velocity curves and converts to UMP. Contact timing travels as a defined UMP per-note controller, with its registry index in `proto/`.
  - The RT plane owns one time-mapping function (a DLL tracking the codec frame counter against CLOCK_MONOTONIC), and every adapter uses it.
  - MCU timestamps are converted at the reader using a sync frame defined in HARDWARE-INTERFACES.md.

### F7 — AD-3 is not enforceable as written (medium)

- **Where:** AD-3.
- **Problem:** "RT threads never … make syscalls" cannot literally hold. The audio driver thread must wait on ALSA (poll or ioctl), and the AD-5 barrier needs either a futex or a spin. Nothing enforces the rule either: no sanitizer, no lint, no test.
- **Fix:**
  - Name the single allowed blocking point: the ALSA period wait in the RT driver thread. Workers sync with a spin barrier on isolated cores.
  - Enforce the rule in CI with Clang RealtimeSanitizer (`[[clang::nonblocking]]` on the render entry points) in host builds of `engine_core` and the offline renderer. Add a device soak test that counts page faults (`getrusage` minflt/majflt) and xruns.

### F8 — Content keys: librarian and desktop roles contradict AD-14 (medium)

- **Where:** AD-9 (Keys row), AD-14, library-format.md, Structural Seed (`formatlib` holds crypto and is linked into the desktop app).
- **Problem:**
  - library-format.md says factory and partner libraries are "encrypted on import". The AD-9 keys row and AD-14 give key access only to `engine`.
  - Nothing says who wraps a content key to the device key when a partner bundle arrives over USB-A or the desktop app: librarian through an OP-TEE TA, or the vendor server.
  - `formatlib`, with its crypto, ships inside the desktop app. A desktop build that contains the decrypt path is an extraction aid.
- **Fix:**
  - State that protected bundles arrive already encrypted, and that the device never encrypts content.
  - Content-key wrapping and unwrapping happens only inside an OP-TEE TA. `librarian` may only ask the TA to verify and store a wrapped key. `engine` may only ask it to unwrap.
  - The desktop build of formatlib is compiled without the decrypt path, enforced by a build flag checked in CI.

### F9 — Observability and CI gates are not decided (medium)

- **Where:** Consistency Conventions (Logging), Structural Seed (CI).
- **Problem:**
  - The memlog approved "diagnostics exported via desktop app opt-in", but the spine leaves it out.
  - The spine does not define the product's key health metrics as shared counters: xruns, stream underruns, p99.99 read latency (named as the design driver in performance-targets.md) and voice steals. It does not cover crash dumps, or the time source for wall-clock logs on a unit with no network or RTC policy.
  - CI is a box on a diagram. It has no gates for AD-15 license scanning, null tests, RTSan, schema compatibility (F3) or a hardware-in-the-loop latency/dropout regression on a dev board.
- **Fix:**
  - Add an AD or convention: `engine` exposes a fixed set of counters in `ws.v1` (`xrun_count`, `stream_underrun_count`, `io_latency_histogram`, `voice_steal_count`). `sysd` collects logs and minidumps into the journald ring, and exports them only through the desktop app after user consent.
  - CI gates on every merge: host unit and null tests, RTSan, `buf breaking` against the last release's proto, a Yocto license manifest check, and a nightly HIL soak on the RK3588 dev board.

### F10 — Engine boot depends on librarian (medium)

- **Where:** AD-4, AD-9, AD-10, Deferred "Boot sequencing details".
- **Problem:** Programs reference library UUIDs (AD-10). The engine learns bundle locations only through the "library published" message from librarian, and the catalog is librarian-only. So at boot, and after a librarian crash, the engine cannot resolve the default program without librarian. That breaks the AD-4 isolation intent and puts librarian, a low-priority process, on the CAP-16 critical path. This is a structural seam, not a sequencing detail, so it should not sit under Deferred.
- **Fix:** add to AD-10/AD-11: published bundles live at a deterministic location derived from the library UUID (internal only; paths still never appear in patches or messages). The engine resolves UUIDs directly, read-only, without librarian. Librarian's publish message is only a notification.

### F11 — Crash consistency between catalog and bundle (low)

- **Where:** AD-11, CAP-17.
- **Problem:** Publishing is a rename plus a catalog row. A power cut between the two leaves an orphan bundle, or a catalog row pointing at nothing. A rename is also only durable after the parent directory is fsynced.
- **Fix:** publish order is (1) fsync the bundle and its directory, (2) rename, (3) fsync the parent, (4) commit the catalog row. On startup, librarian reconciles: catalog rows without a bundle are dropped, and bundles without a row are indexed or quarantined.

### F12 — Minor wording and coverage gaps (low)

- **Pairing:** pairing tokens and paired-host records are a persistent dataset missing from the AD-9 table. Give them to `sysd`.
- **AD-17:** "resampled or slaved" leaves the UAC2 clock policy open. Decide one (async feedback endpoint slaved to the codec is the usual choice), or add it to Open Questions. USB audio from the host into the unit (DAW playback) is not mentioned.
- **Part count:** state that the part count is a runtime constant read from the tier configuration, not a compile-time constant, so v1 (8) and Pro (16) share one base.
- **Deferred "Pro sequencer … must be a client of AD-1":** reword to "a client of AD-1 for pattern data and an AD-21 event processor on the RT plane". As written it reads as a contradiction.
- **Deferred "Sampling feature":** note that it will require amending AD-18 (disk writes during performance) and AD-9 (a new dataset owner), so the deferral is a recorded known amendment.
- **Missing Open Questions section:** the spine has none. Version-sensitive bets recorded only in the memlog belong there: the USB MIDI 2.0 gadget on mainline, RAUC/meta-rauc compatibility with Wrynose, and the HSM choice.

---

## Capability coverage check

| CAP | Mapped | Governance adequate? |
|---|---|---|
| 1 | yes | yes |
| 2 | yes | yes, pending F6 clock mapping |
| 3 | yes | partial (F5: allocation phase) |
| 4/5/6 | yes | yes; CAP-5/6 delivery depends on F4 |
| 7 | yes | partial (F12 part-count constant) |
| 8 | yes | yes |
| 9 | yes | partial (F1, F8) |
| 10 | yes | partial (F12 UAC2 clock policy) |
| 11 | yes | deferred to HARDWARE-INTERFACES.md (accepted) |
| 12 | yes | partial (F3, F4) |
| 13 | yes | yes |
| 14 | yes | partial: the bulk transfer framing for hundreds of GB through the sysd bridge (chunking, resume, integrity) is not fixed between the desktop app and librarian. Add it to AD-13 or defer it with a trigger |
| 15 | yes | yes |
| 16 | yes | partial (F10) |
| 17 | yes | partial (F3 MCU flash, F11) |
| 18 | yes | partial (F6 contact timing) |
| 19 | yes | partial (F2 arpeggiator parameter class) |

## Operational envelope check

| Dimension | Status in spine | Needed |
|---|---|---|
| Deployment and environments | Seed diagram only | Key separation per environment (F1) |
| Build/CI | Seed box only | Gates listed (F9) |
| Update | AD-18 mechanism only | Intake and distribution (F4); skew (F3) |
| Factory provisioning | One phrase in seed | AD-22 (F1) |
| Observability | Logging convention | Metrics, crash dumps, export path (F9) |
| Security operations | Absent | Key custody, rotation, anti-rollback, RMA (F1) |

## What is strong (keep as is)

- AD-1/AD-2/AD-3/AD-21 together give a clear RT contract, and AD-21 cleanly brings in the late CAP-19 addition.
- AD-9's sole-writer table and AD-11's immutable bundles leave little room for storage races.
- AD-15 is a real, tool-enforced rule (`INCOMPATIBLE_LICENSE`) with the reasoning recorded.
- AD-19 makes the acceptance null tests runnable off-device, which matters for a one-developer team.
- The dependency diagram is a usable rule (core → ports only).
