# Epic 1 Context: Deep library with stage latency

<!-- Generated from planning artifacts. Regenerate with compile-epic-context if planning docs change. -->

## Goal

Prove the core product claim on an RK3588 dev board with a USB MIDI keyboard: a sample library far larger than RAM (up to 1 TB) plays at 256 stereo voices with ≤5 ms key-to-audio latency and zero dropouts over 24 h. The epic opens with the PREEMPT_RT, audio-output, MIPI DSI and secure-boot spikes (go/no-go and fallback decisions on data), builds the platform-independent engine, lossless library format, streaming and voice pool, and ends with the Milestone 1 investor demo (month 6). Development is Mac-first: stories tagged "Runs on: Mac" start on an Apple-silicon Mac (same ARM64/NEON ISA); latency and xrun budgets are accepted only on the dev board.

## Stories

- Story 1.1: PREEMPT_RT latency spike on mainline RK3588 under load
- Story 1.2: Audio output bring-up on the dev board
- Story 1.3: Monorepo skeleton with host build and CI
- Story 1.4: Minimal Yocto dev image for the dev board
- Story 1.5: MIPI DSI 7" panel spike on mainline RK3588
- Story 1.6: Secure-boot chain selection and signed image on an unfused board
- Story 1.7: Fused dev board boots signed and derives a per-device key
- Story 1.8: Engine core block loop and engine interface
- Story 1.9: Offline renderer and null-test harness
- Story 1.10: Engine plays live on the developer's Mac (macOS host adapters)
- Story 1.11: Engine runs live on the dev board
- Story 1.12: formatlib lossless block-indexed bundle
- Story 1.13: Import the demo SFZ library subset into bundles
- Story 1.14: 1 TB library resolved from the index without scanning
- Story 1.15: Sample engine plays RAM-resident instruments and passes the sfizz null test
- Story 1.16: Instrument heads decoded into locked RAM at load
- Story 1.17: Streaming from NVMe ahead of the playhead
- Story 1.18: Parallel voice render on the three isolated cores
- Story 1.19: Voice pool arbiter with the sample floor
- Story 1.20: Timestamped event path from a USB MIDI keyboard
- Story 1.21: In-RAM diagnostics counters and latency histogram
- Story 1.22: 24-hour soak gate on the dev board
- Story 1.23: Milestone 1 investor demo proving NFR1–NFR4

## Requirements & Constraints

- Latency: key-press-to-audio ≤5 ms under full polyphony (64-frame buffer at 48 kHz); ≥256 simultaneous stereo sample voices (a floor); libraries up to 1 TB indexed on NVMe; 16 GB RAM with ~4–6 GB reserved for heads; zero xruns in a 24 h run at 256 voices with SSD near full. Heads must cover the qualified SSD's measured p99.99 read latency plus margin.
- Engine is a 64-bit userspace process on mainline Linux (6.18 LTS) PREEMPT_RT, no out-of-tree RT patches; runs internally at 48 kHz on 3 isolated Cortex-A76 cores (`isolcpus`, `nohz_full`, fixed governor).
- RT threads never allocate, lock, block, do disk I/O or page-fault; no swap, no core dumps. During performance the disk only reads.
- Audio is lossless: decoded output is bit-identical to source; never resample or requantize; float is stored uncompressed.
- Null test against sfizz with a dB residual tolerance, automated in CI. GPL/oracle software (sfizz renderer etc.) only as unshipped test oracles; permissive code (sfizz BSD-2 parser, libFLAC) allowed.
- No GPL-3/LGPL-3 in the image (`INCOMPATIBLE_LICENSE`); flac/metaflac CLIs and libsndfile excluded; license manifest per build.
- Main audio goes over I2S to the codec; USB audio is demo-only fallback, never the product main output.
- Secure boot: dev signing root separate from factory-test/production; per-device key derived in OP-TEE; keys never on disk. Fusing is irreversible (use a sacrificial board).
- Fallbacks: CM5 / Intel N-series if RT validation fails; internal HDMI panel if DSI on mainline fails.
- Release gates in CI: host unit tests, null tests, RealtimeSanitizer, Yocto build with license manifest, 24 h dev-board soak with zero xruns.
- Mac timing numbers are labelled "host, not representative".
- Story 1.13 is blocked by Story 11.12 (demo libraries, target month 5); SSD qualification (p99.99) feeds head sizing.

## Technical Decisions

- Process topology: systemd services `engine`, `ui`, `librarian`, `sysd`. `engine` solely owns the audio device, MCU links and USB MIDI (ALSA); streaming and decryption stay inside it. `librarian` is sandboxed, low priority, off audio cores.
- Platform-independent `engine/core` (only depends on `engine/ports` and the C++ standard library; C++20, CMake; builds on macOS and Linux). Host/OS adapters live under `engine/host/`. Other dirs: `tools/offline-render`, `tools/spikes`, `formatlib/`, `proto/`, `yocto/`. CI enforces the dependency rule.
- Block scheduler (64 frames, 48 kHz): serial phase (merge events, event processors, voice alloc/steal) → parallel shared generators → parallel voice render into per-worker scratch → barrier → sum/FX/sends/master. Voices write only their own state and worker scratch.
- One static engine interface; engines compiled in, no plugins. Declares per-voice cost units and steal metadata (start time, level, held/sustained/releasing); sound params addressed by `param_id`. Stable engine type IDs reserved for Sample, Organ, FM, VA; a request for an uncompiled type yields silence with typed `ENGINE_NOT_AVAILABLE`, never another engine.
- Voice pool arbiter is the sole allocator/stealer, runs in the serial phase: reserves FX budget, organ, then each engine's floor (sample ≥256 stereo), rest shared; protects lowest held and sustained notes, steals newer/quieter first; fading voices still count.
- Events: MIDI 2.0 UMP only. Device time is `CLOCK_MONOTONIC_RAW` ns; a DLL on the ALSA hw pointer maps device time to frames; events apply at their exact in-block offset after a fixed scheduling delay; the offline renderer uses the same mapping.
- RT discipline: lock-free SPSC queues, atomic swaps, mlocked pre-faulted arenas, only syscall is the ALSA period wait; RTSan in CI.
- Identity (only formatlib mints): `library_id` (vendor-assigned or UUIDv5 of source fingerprint), `bundle_id` (manifest hash), `instrument_key` (normalized `.sfz` relative path). Engine resolves only via `/data/lib/<library_id>/current → <bundle_id>/`, never scans; unresolved references play silent, never substituted. Readers accept bundle format versions N and N−1.
- Bundles are immutable: stage → bit-exact decode verify → fsync → rename → flip `current` → catalog row; startup reconcile removes orphans; engine holds `BundleLease`s on referenced bundles. Container is block-indexed, FLAC-style per-block coding via libFLAC.
- Generations: snapshot + resident heads form one generation; swapped in only when all heads are resident; freed only after RT reports no live reference.
- Streaming: io_uring + `O_DIRECT` reads, decode and block-hash on I/O edge threads; head budget from tier profile; boot opens `current` links and indexes only.
- Observability: RAM counters (xruns, steals, stream underruns) and block-time and event-to-onset latency histograms, updated atomically on RT threads; exported by the control thread over a Unix socket using versioned `ws.v1` protobuf (`proto/`) via the `velora-diag` dev CLI.
- Yocto 6.0 "Wrynose" LTS, systemd; image variants dev / factory-test / production (this epic: dev).

## Cross-Story Dependencies

- 1.1 gates the RK3588 choice; its kernel config, boot args, load profile and 100 µs pass threshold are reused by 1.4, 1.5 (and regression checks in 1.4/1.5).
- 1.2 output path and 1.4 image underpin 1.5, 1.11 and everything on the board; 1.6 → 1.7 secure-boot chain.
- 1.3 (skeleton, CI, RTSan) precedes 1.8–1.10; 1.8 core precedes 1.9, 1.10, 1.11, 1.15, 1.18, 1.19.
- 1.12 formatlib → 1.13 importer → 1.14, 1.15, 1.16, 1.17; 1.16/1.17 feed 1.18 and 1.19 toward 256 voices.
- 1.20 (event path) and 1.21 (counters) feed 1.22 (soak, uses 1.21 as oracle and 1.20 USB MIDI path) and 1.23 (demo pack aggregates 1.1, 1.2, 1.5, 1.7 reports plus latency and soak reports).
- Later epics extend this work: Epic 5 (full SFZ/import UX), Epic 3 (keybed, panel, DIN events), Epic 9 (Velora Studio diagnostics export).
