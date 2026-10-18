---
review: reconcile (spec → spine)
target: ../ARCHITECTURE-SPINE.md (+ ../.memlog.md)
input: ../../../../specs/spec-arm64-stage-workstation/ (SPEC.md, 8 companions, .memlog.md)
date: 2026-10-17
---

# Reconcile Review — Spec vs Architecture Spine

## Verdict

The spine covers the spec's main structure well: process isolation, RT safety, I2S clock master, A/B updates, atomic imports, content protection, the license boundary, the one-schema IPC and the arpeggiator clock. But it **drops several quiet requirements that decide whether CAP-1/3/4 and the 24 h zero-dropout target can pass**:

- the voice-pool policy (stealing priorities, CPU-budgeted floors, organ never stolen)
- head sizing against tail latency
- the lossless and bit-identical rule, which collides with 32-bit float sources
- the vendor content pipeline

It also has a few internal contradictions: disk writes during performance, the USB MIDI path through `sysd`, and the keybed reader under AD-3. None of this needs a new paradigm. Every item can be fixed by amending an AD or adding one.

Severity: **H** = a CAP success criterion or hard constraint has no architectural home, or the spine contradicts it. **M** = the rule is implied but not stated, so builders will diverge. **L** = wording, traceability or verification.

---

## 1. Dropped or contradicted requirements (ranked)

### R1 [H] Voice pool policy is missing. AD-7 contradicts "organ never stolen"

- **Spec:**
  - CAP-3 says stealing never drops the lowest held note or pedal-sustained notes first, and steals newer, quieter voices first.
  - engines.md says the pool is "dynamic and budgeted by CPU", each engine has a guaranteed floor, and surplus is shared.
  - CAP-4 and engines.md say the organ has "full polyphony, never stolen".
- **Spine:**
  - The CAP-3 row maps to AD-1/5/6/7. None of those ADs states a stealing order, a cost model or an arbiter.
  - AD-7 says every engine "allocates voices only from the shared pool, within its guaranteed floor". Read literally, that puts the organ in the pool where it can be stolen, and it caps engines *at* their floor, while the spec says floors are minimums plus a shared surplus.
- **Missing:**
  - Who arbitrates. It must be the RT plane, before the parallel render.
  - The unit of the budget. The spec says CPU cost, not voice count, so each engine needs a per-voice cost estimate.
  - What happens when the sum of the floors plus FX plus the arpeggiator exceeds the 3 isolated cores.
  - The stealing order.
  - The organ's exemption, as a fixed reserved budget outside stealing.
- **Fix:** add **AD-22 Voice pool arbitration**:
  - One RT-plane arbiter owns the pool.
  - The budget is CPU cost units per engine voice, measured on target.
  - Floors are reserved and the surplus is shared.
  - The organ (and rotary) run on a fixed reservation that is never stolen.
  - Stealing order: newest and quietest first. The lowest held note and pedal-sustained notes are protected.
  - The effect budget is reserved before the voice budget.
  - Reword AD-7 to "allocates from the shared pool through the arbiter; floor is a guaranteed minimum".

### R2 [H] Lossless and bit-identical rule is missing, and 32-bit float conflicts with libFLAC

- **Spec:**
  - The constraint "sample data is never lossy-compressed".
  - CAP-1 says "decoded output is bit-identical to the source".
  - library-format and performance-targets specify sources up to 32-bit float / 96 kHz, a FLAC-style codec, and allow libFLAC.
- **Spine:** formatlib holds a "lossless codec". Stack lists libFLAC 1.5.0. No AD states the lossless or bit-identical rule.
- **Conflict:** libFLAC encodes integer PCM only. It cannot losslessly encode 32-bit float.
- **Unstated:**
  - Whether import may resample (44.1 kHz corpora such as VSCO2) or requantize. Either would break "bit-identical to source" and the sfizz null test.
- **Fix:** amend AD-11, or add a formatlib rule:
  - The codec is lossless only. Float blocks use a dedicated lossless float path, or are stored uncompressed and only encrypted.
  - Import never resamples or requantizes. It keeps the source rate and depth, and the engine applies the rate ratio at playback.
  - `librarian` checks a decode round-trip for bit-identity before the atomic publish. This gives "verifies it" in AD-11 a definition.

### R3 [H] Head sizing, the streaming design driver and the decode/verify location are not stated

- **Spec:**
  - performance-targets: about 4–6 GB reserved for heads on 16 GB.
  - "Tail latency matters more than bandwidth (p99.99 during SSD GC). Heads must cover the worst-case read, not the average."
  - library-format: heads are decoded to PCM at load, so an attack never waits on decode.
- **Spine:** AD-3 says only "heads and stream buffers live in mlocked, pre-faulted memory".
- **Missing:**
  - The head-length rule. It must cover the measured worst-case read plus the refill pipeline at the maximum transpose and 96 kHz.
  - The head budget per tier, and what "loaded" means against a 1 TB library. Only the heads of parts in the current performance or set are resident.
  - Where streamed blocks are lossless-decoded. It must be the I/O threads, never RT.
  - Where per-block hashes are checked. Checking 1 TB at load is impossible. The manifest signature should be checked at load and block hashes on the I/O threads. A mismatch should give a silent block plus a fault event, never a crash.
  - Page-cache policy. O_DIRECT, or equivalent, so page cache from the librarian or UI cannot evict the stream or stall it.
  - `RLIMIT_MEMLOCK` and a no-swap/no-overcommit policy.
- **Fix:** add **AD-23 Streaming budget**, or amend AD-3, with these rules and with "Milestone 1 measures p99.99 read latency on the chosen SSD model near full; head length is derived from it".

### R4 [H] Internal contradictions with AD-18 ("disk read-only during performance") and AD-2/AD-3/AD-8

1. **Logging writes during performance.**
   - The Logging convention puts the journald ring on the data partition. That writes continuously while playing and contradicts AD-18.
   - The scheduled `fstrim` timer, SQLite WAL checkpoints and favorite or recent-use writes in the catalog are not addressed either. Each can cause NVMe tail stalls, which threatens the 24 h zero-dropout target with the SSD near full.
   - Fix: journald goes to volatile RAM and is persisted only on save, idle or shutdown (or on demand through the desktop app). `fstrim` and checkpoints run only when idle or at shutdown. Imports during a performance are throttled with I/O priority, or forbidden.
2. **USB MIDI routed through `sysd` → control thread.**
   - The paradigm diagram shows `SYS -- "config, power-fail, MIDI USB" --> CT`.
   - Notes are performance state owned by RT (AD-2), and AD-8 converts every source at the edge. Routing USB MIDI through another process and the non-RT control thread adds latency and jitter and breaks AD-2.
   - Fix: `engine/host` reads the USB MIDI gadget's ALSA UMP endpoint directly, as it does for DIN. `sysd` only *configures* the gadget.
3. **Keybed/panel reader labelled "RT" in the diagram, while AD-3 forbids RT threads from doing syscalls and I/O.**
   - The reader must do UART/SPI reads.
   - Fix: define the reader as a high-priority SCHED_FIFO *edge thread* outside AD-3's "RT audio threads", which hands off only through SPSC. State that AD-3 applies to the audio render threads. The UAC2 gadget writer is a similar non-RT edge thread with ASRC.

### R5 [H] Vendor content pipeline and production security hygiene have no home

- **Spec:**
  - The founder supplies the factory library content.
  - Factory and partner libraries are encrypted, with a per-device key wrapping the content key.
  - library-format says factory and partner libraries "are encrypted on import".
  - CAP-9 says copying a library to another unit fails, and a custom OS cannot boot to read keys.
  - The production firmware has no console and no mass storage.
- **Missing in the spine:**
  - **A vendor packaging tool:** encrypt, sign with the Ed25519 vendor key, and handle custody of the vendor private key. There is no `tools/` entry, and there is no environment for it in the structural seed.
  - **How a content key ends up wrapped by a device key that never leaves OTP/OP-TEE.** The two options are a factory-station install with per-unit wrapping, or a vendor-wrapped transport key that OP-TEE re-wraps on import. The spec's "encrypted on import" wording implies on-device encryption, which would mean plaintext vendor content on the USB drive. This needs a decision.
  - **Factory library pre-install** at the factory station, which today does only OTP fusing and key provisioning.
  - **Dev and factory-test images signed with different keys** from production, so a dev image with SSH cannot boot on a fused production unit. Otherwise "custom OS cannot boot" is defeated by our own image.
  - **Core dumps disabled and no swap in production.** Content keys live in engine memory, and a core dump or swap would put them on disk. That contradicts AD-14 "never touch disk".
  - **A USB gadget composition rule:** production allows UAC2 + MIDI + NCM only, and never `f_mass_storage`. AD-13 covers SSH only.
  - **Untrusted USB-A media handling:** mount read-only with `nosuid,nodev,noexec`, parse inside the low-privilege `librarian`, and use a filesystem allow-list (exFAT, NTFS3, ext4, and HFS+/APFS?).
- **Fix:**
  - Amend AD-14 with the key-wrap flow, core-dump and swap policy, and image-signing separation.
  - Add a `tools/content-pack/` module and a "Vendor content" environment to the structural seed.
  - Amend AD-13 with the gadget composition and the USB-A mount policy.

### R6 [M] One software base v1/Pro is not made concrete

- **Spec:**
  - v1 and Pro use one software base. The sequencer is a module, not a fork.
  - v1 has 8 parts and Pro has 16.
  - v1 has 16 GB and Pro has 32 GB, with heads sized accordingly.
- **Spine:** the scope line mentions the shared Pro base, and the sequencer is deferred as an AD-1 client.
- **Missing:**
  - How the tier is identified: OTP or board ID, read by `sysd`.
  - That part count, head budget and voice budget are *runtime tier parameters* from one image, not compile-time branches.
  - How the sequencer module is enabled per tier.
- **Fix:** add a short AD, "Tier is a runtime profile". Snapshot structures are sized for 16 parts and capped by the profile.

### R7 [M] CAP-19 is not in SPEC.md

- The spine binds CAP-19 and AD-21 governs it.
- CAP-19 appears only as the last spec `.memlog.md` entry. SPEC.md has no CAP-19 intent or success criterion, so nothing makes it testable.
- It is also v1 scope added to a date-driven release with one developer.
- **Fix:** update SPEC.md with CAP-19 intent and success. For example: per-part arpeggiator, modes, sync to internal tempo or MIDI clock from DIN and USB, jitter tolerance, and behaviour with sustain. Then re-confirm v1 scope.

### R8 [M] Multi-contact organ onset vs "engines consume only UMP" (AD-8)

- **Spec:** CAP-18 says organ onset follows the timing *between each key's contacts*, like the M-solo's Virtual Multi-Contact.
- **Spine:** AD-8 says engines consume only UMP. A plain UMP note-on carries velocity and an attribute, not per-contact closure times.
- **Fix:** amend AD-8 with a defined encoding. Two options:
  - A per-note assignable controller event per contact closure, with timestamps.
  - Note-on attribute data carrying the contact interval.
- Also state that release velocity rides on note-off velocity, that aftertouch is polyphonic or channel pressure when present, and that half-pedal sustain uses continuous CC64 resolution.

### R9 [M] Audio routing gaps for CAP-11 / CAP-13

- **Line input.** CAP-11 requires every listed port to work on a production unit, and the stereo line input is listed. The spine has no input path from the codec ADC to the engine mix or monitor. Decide v1 behaviour, for example a play-along mix into the main and headphone outputs at a fixed gain, and give it a home in the render model.
- **Aux outputs.** The render model in AD-5 is "per-part FX → master mix". It has no routing of a part to aux 1/2 and no send/return buses. A shared reverb or delay as a send is the CPU-sane choice for 8 parts. Amend AD-5 with an output and bus routing rule: part → {main, aux}, sends → shared FX → main.
- **USB channel set.** "A DAW records its outputs" implies the UAC2 channel set mirrors main + aux. Pin that down, with the details in HW-INTERFACES.
- **UAC2 clocking.** AD-17 says "resampled **or** slaved". Pick one: an async feedback endpoint slaved to the codec, or ASRC on a non-RT thread.

### R10 [M] Boot ≤10 s with 1 TB and 24 h stability lack explicit rules

- Boot sequencing is deferred, which is acceptable. But the spec's own rule, "favorites' heads load first, rest in background", is not captured. The no-scan-at-boot invariants are also missing:
  - Open the catalog only.
  - Verify signatures only for libraries being loaded.
  - Re-flash an MCU only on a version mismatch, never on every boot.
- No rule exists for xrun detection or counting, a stress harness, CPU frequency governor pinning or thermal headroom. Without those, "zero dropouts in 24 h" cannot be observed.
- The CAP-16 row in the map cites AD-4 only, but AD-18 also binds CAP-16.
- **Fix:**
  - Add the boot invariants to AD-18 or AD-4.
  - Add an xrun counter, published to the UI and logs, plus a 24 h soak target in CI on hardware, to the Structural Seed.

### R11 [M] Power-loss coverage has gaps (CAP-17)

- An MCU flash interrupted by a power cut could brick the keybed or panel MCU. MCUs need a ROM or immutable bootloader that the SoC can always reflash, with details in HW-INTERFACES.
- **Crash consistency between the bundle and the catalog.** A cut between the atomic rename and the SQLite insert leaves an orphan bundle. Add startup reconciliation: delete staging, and index or remove orphans.
- **Patch and config saves.** These need write-temp + fsync + rename, not just "journaled partition".

### R12 [L] Price, BOM and one-developer constraints are not reflected

- The spec ties the v1 price (R$ 17,990–19,990) to BOM and tax margin, and says "scope must fit one developer". The spine makes choices that add BOM cost (a second MCU, a supercap hold-up) and engineering surface:
  - four C++ processes
  - a Tauri app adding Rust and a web stack
  - two MCU firmwares
  - an OP-TEE trusted application
  - Yocto
- There is no rationale and no fallback recorded.
- **Fix:** add one line per cost-adding decision with its justification. Consider a note that the panel MCU may merge into the keybed MCU if timing allows, since AD-16's purpose is protecting keybed timing.

### R13 [L] Smaller traceability items

- **"No radio."** The spine is silent. Add an image rule: the kernel and image exclude the Bluetooth and Wi-Fi stacks and firmware. The board side goes to HW-INTERFACES.
- **No trademarks in user-facing text** ("Hammond", "DX7"). Add to AD-20 as a string-catalog lint.
- **sfizz license.** The spec's open question on the repository LICENSE and its bundled dependencies is unresolved, yet Stack pins sfizz 1.2.3. Mark it "pending license check" in Stack or AD-15.
- **USB MIDI 2.0 gadget on mainline.** The spine memlog marks this as an unverified assumption. Carry it, with a MIDI 1.0 fallback for older Windows hosts, into the spine as a Milestone-1 risk.
- **Yocto `INCOMPATIBLE_LICENSE = "GPL-3.0*"`.** Verify that it does not catch the GCC runtime (libstdc++/libgcc, GPL-3.0-with-GCC-exception) or need an exception entry.
- **The fallback SoC path** (CM5, Intel N-series). AD-14's OTP/OP-TEE and secure boot are RK3588-specific. Note that the crypto adapter would need a TPM or another root of trust there. The hexagonal ports already make this cheap.
- **Bulk desktop transfers.** These cross `desktop` → `sysd` bridge → `librarian`. Confirm that the bridge streams and does not buffer whole files, given a 300 GB+ library.
- **MIDI out.** The spine does not state what DIN and USB MIDI out carry (local keybed and panel as a controller, with local off). Add a line to AD-8.

---

## 2. Requirements that landed correctly

| Spec requirement | Spine home |
| --- | --- |
| UI isolated from audio; a UI crash never interrupts sound | AD-4, AD-1 (panel goes to engine directly) |
| Audio thread: no disk I/O, syscalls or page faults; heads in locked RAM | AD-3 (subject to R3 and R4.3) |
| Main output over I2S/TDM, codec clock master, USB never in the main path | AD-17 |
| 64-bit userspace on mainline PREEMPT_RT, no RTAI | Stack (6.18 LTS), AD-5 |
| Clean-room / no GPL engines; sfizz and libFLAC allowed; GPL only as oracle | AD-15 |
| 4-engine-ready API, patch format and UI from day one; engines by firmware | AD-7, AD-12, AD-18 |
| 8 parts v1 / 16 Pro; sequencer as a module | AD-1, Deferred (see R6) |
| Encryption design (AES-CTR per block, wrapped keys, Ed25519, secure boot); user imports unencrypted | AD-14 (see R5) |
| Atomic imports; A/B with rollback; read-only root; journaled data | AD-11, AD-18 |
| No mass storage; mediated desktop protocol; no SSH | AD-13 (see R5 for gadget composition) |
| Stable IDs, missing library → silent part | AD-10 |
| Null-test and offline renderer for acceptance | AD-19 |
| ≤5 ms latency, 64 frames @ 48 kHz | AD-5, AD-16, AD-17 |
| Catalog for search, categories and favorites | AD-9 |
| Arpeggiator with a single clock (CAP-19) | AD-21 (see R7) |
| PT-BR launch, export later | AD-20 |

---

## 3. Items destined for HARDWARE-INTERFACES.md (not spine gaps)

- **Audio codec:**
  - PCM3168A over TDM as clock master, with a 24.576 MHz crystal. The SoC PLL never clocks audio.
  - I2C control and the board machine driver.
  - Channel map for 2 main + 2 aux outputs.
  - ADC channels for the stereo line input.
  - Headphone amplifier and its source.
  - Codec filter latency in the 5 ms budget.
- **Keybed MCU:**
  - Frame protocol over UART ≥3 Mbaud or SPI.
  - ≥10 kHz scan.
  - Handling of 2 or 3 contacts and optional aftertouch.
  - Per-contact timestamps (feeds R8).
  - Scan plus transport under 1 ms.
  - How the MCU clock syncs to the SoC monotonic clock.
  - Calibration table format, so a different Fatar model needs no code change.
- **Panel MCU:**
  - Knobs, 9 drawbar sliders (resolution), buttons, LEDs.
  - Pedals: continuous sustain with half-pedal resolution, expression, one switch, polarity detection.
- **MCU flashing:** the SoC flashing path (SWD, ROM bootloader, reset and boot GPIOs). An unbrickable bootloader for power cuts during flashing (R11).
- **MIDI DIN in/out:** which UART, opto-isolation, and the thru/out source.
- **Display:**
  - 7" MIPI DSI panel, with an internal HDMI fallback.
  - I2C capacitive touch with a mainline driver.
  - Backlight control.
- **USB-C (device):**
  - RK3588 USB3 OTG in device mode.
  - Composite UAC2 + USB MIDI (2.0 with a 1.0 fallback) + CDC-NCM.
  - Channel count and rates; VID/PID.
  - No mass-storage function and no serial console.
- **USB-A (host):** USB3, with a VBUS current budget for bus-powered external SSDs.
- **Storage:**
  - M.2 2280 NVMe on PCIe Gen3 x4, up to 1 TB, sealed in the unit.
  - SSD qualification for p99.99 read latency near full, endurance and preferably power-loss protection (drives R3).
  - Thermal pad and heat sink.
- **Power:**
  - Supercap hold-up of about 100–200 ms, sized to the measured flush and fade time.
  - Power-fail GPIO timing.
  - Off-the-shelf PSU complying with Inmetro.
  - IEC 62368-1 safety and EMC.
- **Security hardware:**
  - Secure-boot OTP fusing and the device-key provisioning flow at the factory station.
  - JTAG disabled by fuse.
  - Debug UART header unpopulated or disabled in production.
- **SoM:**
  - Industrial RK3588 SoM with a written 7–10 year longevity commitment.
  - 16 GB (v1) and 32 GB (Pro).
  - Tier ID through OTP or board strap (feeds R6).
  - A variant with no wireless.
- **No radio:** no Bluetooth or Wi-Fi parts or antennas on the board.
- **Thermal:** sustained 24 h full load on 3 A76 cores with no throttling, and the fixed CPU frequency the software assumes.
