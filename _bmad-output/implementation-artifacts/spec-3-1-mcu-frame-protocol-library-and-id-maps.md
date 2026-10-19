---
title: 'Story 3.1: Versioned MCU frame protocol library and control/LED ID maps'
type: 'feature'
created: '2026-10-19'
status: 'done'
baseline_revision: '001a0e3494a1c9090eab1e54f55183d5950eb6e5'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-3-context.md'
  - '{project-root}/engine/core/README.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem and approach:** Story 3.1 "Versioned MCU frame protocol library and control/LED ID maps" from `_bmad-output/planning-artifacts/epics.md` (starts at line ~1752; read the whole story there, its Given/When/Then acceptance criteria are authoritative and are copied by reference, not paraphrased).

**Constraints:** create the new top-level `mcu/common/` (the Structural Seed dir `mcu/`: later `mcu/keybed/`, `mcu/panel/`, `mcu/bootloader/` are other stories — create only `mcu/common/` now with a README stating role and dependency rule) as a **portable C library (C11, no OS, no heap, no libc allocation, no stdio in the library itself; fixed-size buffers owned by the caller; no floating point)** that compiles for the MCU target and on the host, plus a **C++ wrapper in `engine/host`** (`velora/host/mcu_frame.hpp`) for the SoC side; implement exactly the frame layout of `_bmad-output/planning-artifacts/architecture/architecture-buenos-x64-workstation-2026-10-17/HARDWARE-INTERFACES.md` §5 (sync 0xA5, protocol version, type, length, 16-bit seq, 32-bit tick, payload, CRC-16/CCITT over version..payload, all little-endian) with the types HELLO, KEY_CONTACT, AFTERTOUCH, CONTROL, BUTTON, LED_SET, TIME_SYNC, ENTER_BOOTLOADER and their payload encodings (define each payload layout in the header and the published document: HELLO = firmware version, protocol version, board ID, keybed contact count; KEY_CONTACT = key index, contact index, closed/open; AFTERTOUCH = channel or per-key + 16-bit value; CONTROL = control ID + 16-bit value or signed step delta; BUTTON = button ID + pressed/released; LED_SET = LED ID + brightness/colour; TIME_SYNC = request/reply with the current tick; ENTER_BOOTLOADER) — pick a CRC-16/CCITT variant explicitly (CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no xorout) and document it with the standard check value for "123456789" (0x29B1); an **encoder** (no heap, caller buffer, returns length or a typed error for buffer too small / payload too long), a **streaming decoder** (byte-at-a-time state machine with a fixed internal buffer; resynchronises on the next valid sync byte after noise/truncation/bad CRC; never reads out of bounds; never delivers a corrupt frame; counters: CRC errors, sequence gaps (seq is 16-bit wrapping; a gap means seq != last+1; report lost count), version mismatches, framing errors/resyncs, dropped bytes), version policy: protocol version byte; **unknown major version rejected with a typed error and a version-mismatch counter; N−1 accepted** — define version as a single byte with a documented major/minor split (e.g. high nibble major, low nibble minor; accepted = same major with minor ≤ current, or the previous major as N−1? pick one rule that matches "accepts N and N−1" and document and test the boundaries; current version = 1 per the hardware doc "Starts at 1"; make constants easy to bump). **ID maps generated from one source file:** `mcu/common/ids.json` (or yaml-subset/JSON) defining every control (8 encoders, VALUE encoder, 9 faders, 2 wheels (pitch, mod), 3 pedals (sustain, expression, switch)), button (8 part/scene buttons; LIVE SET ◀ ▶; SCENE ◀ ▶; EXIT; MENU; WRITE; VALUE push; ROTARY STOP; ROTARY S/F; 3 toggle buttons DRAWBARS/MIXER, PARTS/SCENES, PART/ALL/ASSIGN) and LED (toggle state LEDs: 2+2+3 states, 8 part/scene LEDs, 8 encoder rings; follow HARDWARE-INTERFACES §4 and the UX DESIGN.md/EXPERIENCE.md panel for names) each with one stable numeric ID, a name, type, resolution (bits) and range; a generator script (`mcu/common/gen_ids.py`, Python stdlib) emitting a C header (`mcu_ids.h`), a C++ constexpr table for the host, and a **versioned Markdown document** `mcu/common/MCU-ID-MAP.md` (generated, committed or generated into the build and also copied under `_bmad-output/planning-artifacts/architecture/architecture-buenos-x64-workstation-2026-10-17/` as the published partner document — choose one consistent approach: generated at build time into the build dir AND a committed copy under `mcu/common/` that a ctest verifies is up to date), with the build/ctest failing on duplicate IDs, missing fields, or a stale published document; keep IDs never reused. Tests (host, bare-main style like the rest): encode/decode round trip for every type including boundary values, golden frame bytes (hard-coded hex of at least one frame per type to pin the wire format and CRC), CRC known-answer, streaming decoder fed one byte at a time and in random chunking, noise before/between frames, truncated frames, bad CRC (delivers nothing, counts), seq gap counting including 16-bit wrap, version rejection and N−1 acceptance, payload length vs type mismatch, max payload size, back-to-back frames, resync after a false sync byte inside a payload (a 0xA5 byte in payload must not break resync correctness), and a **1-hour fuzz run on the Mac** as a separate ctest labelled `slow`/fuzz (iteration count based on time or a large fixed count that takes about an hour wall-clock? — instead use a deterministic-seed fuzz that processes the equivalent of 1 hour of 3 Mbaud traffic in bytes (≈ 1.35 GB) in fixed-size chunks of random garbage mixed with valid frames and mutated frames, with ASan/UBSan in the Docker job where available; keep the default ctest run short by making the fuzz test read an iteration count from an env var `VELORA_FUZZ_BYTES` with a small default (e.g. 50 MB) and document how to run the full hour-equivalent; invariants: never crashes, never delivers a frame whose CRC is wrong, delivered frames equal the valid frames injected in order when injected frames are not corrupted by neighbours); the library must compile cleanly with -Wall -Wextra -Wpedantic -Wconversion for C11 (and as C++ for the wrapper), and with the `engine/core` purity unaffected (mcu/common is not engine/core). Add a small loopback helper in the C++ wrapper for Story 3.2/3.5 use (encode frames into a byte vector; stream decoder wrapper with callbacks or a vector of frames).

**Never:** write MCU firmware, bootloader, SoC readers, USB, or hardware code; allocate; use floating point in the C library; read GPL code.


</intent-contract>

## Code Map

- `_bmad-output/planning-artifacts/epics.md` Story 3.1 (~line 1752) -- the acceptance criteria, read-only
- `engine/core/`, `engine/host/`, `proto/`, `tools/offline-render/` -- the code base so far; read the READMEs first (engine/core/README.md, ORGAN.md when present, proto/README.md, engine/host/README.md, tools/offline-render/README.md); keep the existing style (bare-main tests, RTSan tests wired under VELORA_HAVE_RTSAN; macOS skips RTSan tests, only `tools/ci/docker-check.sh` runs them)
- `tools/ci/check_core_deps.py`, `tools/ci/check_third_party_notices.py`, `tools/ci/docker-check.sh` -- validations (CI is manual-only; validate locally)

## Tasks & Acceptance

**Execution:** implement every acceptance criterion of the story with tests for each Given/When/Then and each row of the matrix above; update the READMEs/docs touched; keep all existing tests and null-test goldens passing.

**Acceptance Criteria:** exactly those of Story 3.1 in `epics.md`, plus: macOS `ctest` passes and `tools/ci/docker-check.sh` ends ok (gcc and clang-20 RTSan).

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` -- expected: all pass on macOS
- `tools/ci/docker-check.sh` -- expected: ok (only place RTSan tests run)
- `python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py` -- expected: pass

## Review Triage Log

### 2026-10-19 — Review pass (2 reviewers: edge-case hunter + verification gap)
- `[medium]` `patch` seq accounting: frames rejected for unknown type/bad length/version consumed no seq (spurious gaps); duplicates counted as ~65535-frame loss; MCU reboot (new HELLO) produced a huge gap — every CRC-valid frame consumes a seq, duplicates/reorders get their own counter, a delivered HELLO resets the expectation
- `[medium]` `patch` stalled false header could hold back a real short frame; init accepted major 0; pack_* clobbered the caller's version/seq/tick; no range validation of board IDs, contact counts, 12-bit values, encoder deltas, unknown IDs — flush_stale, validation with typed errors from the generated range tables, init clamp, pack preserves header
- `[medium]` `patch` wrapper error paths, event mapping and every counter field untested; fuzz did not compare seq/resync/framing counters — tests rewritten, fuzz compares all counters
- `[medium]` `patch` partner document had duplicate section numbers (two sections 4 and 5) — renumbered 1-8 with a generator uniqueness/cross-reference check; ids.json shape errors raise IdError; protocol-version static assert in C; published-copy check tolerates a missing `_bmad-output/`; -Werror optional via VELORA_WERROR
- `[low]` `defer` payload layouts the hardware document left loose (LED_SET arg, TIME_SYNC layout, 12-bit fader/wheel/pedal resolution, encoder step ≤ ±127, board ids 1=keybed 2=panel, max payload 64 bytes) must be confirmed with the hardware partners before the first firmware; ASan/UBSan job not added to Docker; counters other than bytes_in wrap at 2^32 (documented); a false header delays following frames by up to 76 bytes (~250 µs at 3 Mbaud) unless flush_stale is used; `reverb_bench` failed once under Docker load and passed on re-run (timing benchmark flakiness to watch)

## Auto Run Result

Status: done. Portable C11 `mcu/common` library (no heap/OS/float): CRC-16/CCITT-FALSE (check 0x29B1), encoder, typed pack/unpack for the eight frame types, streaming decoder with resync, counters, version policy (major.minor nibbles, N and N−1), C++ wrapper in `engine/host` (`StreamDecoder`, `FrameWriter`, `Loopback`), ID maps generated from `ids.json` (23 controls, 21 buttons, 23 LEDs) into C/C++ headers and a versioned partner document (also published under the architecture folder), deterministic fuzz test (1.35 GB hour-equivalent passed in ≈114 s). Verification: macOS ctest all pass, Docker gcc and clang-20 RTSan ok, purity and notices checks pass. Follow-up review recommended: false.
