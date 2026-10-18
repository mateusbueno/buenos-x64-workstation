---
type: architecture-review
lens: adversary (compliant-but-incompatible unit pairs)
target: ../ARCHITECTURE-SPINE.md
context: ../../../../specs/spec-arm64-stage-workstation/SPEC.md (+ companions)
date: 2026-10-17
---

# Adversarial Review: ARM64 Stage Workstation Architecture Spine

## Verdict

The paradigm is sound: single-writer control plane, an RT-owned performance plane, a hexagonal core, and one-writer-per-dataset. But the spine fixes *who* writes and *which transport* to use. It does not fix the *shape*, *identity*, *ordering* or *lifetime* of what crosses those boundaries. I found 14 holes. In each one, two units can each follow every AD to the letter and still fail to work together. Six of them are likely to bite during Milestone 1 or the first integration of ui, engine and librarian, so close those before stories are written.

Method: for each hole, I name two units built independently (different people, or the same founder months apart with an AI agent that sees only the spine). I show that both are AD-compliant, then show the incompatibility and a concrete failure scenario. Each hole ends with a proposed new or tightened AD.

Severity: **Critical** means silent wrong sound or data loss in normal use. **High** means it breaks a CAP success test, or forces a rewrite at integration. **Medium** means a real bug class with a workaround. **Low** means friction.

---

## Pre-finding: the binding companion is missing

AD-16 says both MCUs speak "the frame protocol in `HARDWARE-INTERFACES.md`", and the spine lists that file under `companions:`. It does not exist anywhere in the repository. Until it does, the keybed MCU, the panel MCU and the engine reader are bound to nothing, and every hole below about MCUs (H3, H6, H5) is wide open. **Action:** write `HARDWARE-INTERFACES.md`, or drop the reference and move the frame rules into the spine.

---

## H1. Library identity vs. bundle version vs. re-import (Critical)

**Pairs:** librarian (on-device import) vs. desktop app (formatlib conversion on the Mac); librarian vs. engine (resolution); factory-content pipeline vs. librarian.

**Both comply:**
- AD-10 says programs reference "library UUID + instrument key, stable within that library".
- The conventions say "Library = UUIDv7, set at import or creation and never reassigned".
- AD-11 says "an update means a new bundle and a catalog switch".

**Clashes:**
1. **Update semantics.** One implementer reads AD-11 as *new bundle = new library UUID*, because "never reassigned" means a new import gets a new ID. Another keeps the UUID and swaps the bundle. Under the first reading, every patch referencing the library goes silent after an update. That is the exact failure AD-10 says it prevents.
2. **Re-import.** A user deletes a 300 GB SFZ library and re-imports it from the same drive. Import mints a fresh UUIDv7, so every saved program now shows "missing library". AD-10 lists "re-imported" as something it prevents, but nothing in the rules makes it so.
3. **Who mints.** The desktop app converts the SFZ on the computer (formatlib) and uploads a bundle. Both the desktop app ("creation") and the librarian ("import") can claim the right to mint the UUID. If the same conversion is uploaded twice, you get two UUIDs or a collision, depending on who wins.
4. **Factory and partner libraries.** These must have the *same* ID on every unit, or factory programs cannot reference them. UUIDv7 "set at import" gives a different ID per unit.
5. **Instrument key derivation is unspecified.** The librarian numbers instruments 0..N in directory-walk order. Desktop formatlib v1.3 walks in a different order. The "same" library converted on two paths gives the same keys to different instruments, so a patch silently plays the wrong instrument. That is worse than silent.
6. **Engine lookup with no paths.** AD-10 bans paths in messages. AD-9 lists the catalog's readers as "ui, desktop app, via the librarian API only", so the engine is *not* a catalog reader. So how does the engine turn a library UUID into a bundle on disk? Each team will invent a directory convention, and the two will differ.

**Proposed rule (AD-10, tightened; new AD-10a):**
> - A library has a **`library_id`** (UUID, stable across versions and re-imports) and each published bundle has a **`bundle_id`** (content hash of the signed manifest). Programs store `library_id` + `instrument_key` only.
> - `library_id` is minted by exactly one function in formatlib: for factory/partner content it is assigned by the vendor and carried in the signed manifest; for user imports it is a UUIDv5 over a normalized source fingerprint (root folder name + sorted relative paths + sizes), so re-importing the same source yields the same id. The desktop app and librarian both call this function; neither mints ids any other way.
> - `instrument_key` is a formatlib-defined deterministic function of the source (SFZ: normalized relative path of the `.sfz`; SF2: `bank:preset`), versioned with the bundle format. Enumeration order never defines a key.
> - On-disk layout is defined by formatlib: `/data/lib/<library_id>/<bundle_id>/…`, with `/data/lib/<library_id>/current` an atomically swapped symlink. This is the engine's only resolution mechanism. `LibraryPublished{library_id, bundle_id}` is the only notification.
> - An instrument key that disappears in a new bundle resolves as "missing instrument" (AD-10 silent-part rule), never as a different instrument.

---

## H2. Structural vs. performance parameter boundary (Critical)

**Pairs:** ui (touchscreen parameter edit) vs. panel MCU knob path; desktop app editor vs. ui; engine control thread vs. RT plane on "save as current".

**Both comply:**
- AD-1 says programs and parts are written only by the control thread, through commands.
- AD-2 says knobs, drawbars and CC are performance state owned by RT and arrive as UMP.
- AD-8 lists "desktop app" as a UMP input source.

**Clashes:**
1. **Same parameter, two paths.** Filter cutoff is a stored program value (structural: it is in the program) and also a panel knob (performance). The ui developer sends `SetParam{cutoff}` to the control thread, which rebuilds the snapshot. The panel knob goes as CC to RT. The desktop app sends UMP. The live value now has three writers. AD-1 and AD-2 each claim the cutoff, and nothing says which list wins.
2. **Snapshot clobber.** A player rides the cutoff knob (RT live value = 80), then renames part 3 on the touchscreen. The control thread publishes a new snapshot with the stored cutoff = 40. One engine author re-initializes performance values from every snapshot, and the sound jumps. Another author never reads snapshot values after program load, so a ui edit of the cutoff is ignored. Both readings are AD-compliant.
3. **"Save as current" race.** AD-2 has the control thread *mirror* RT changes for save. The player moves the drawbars and presses Save within one block. The control thread serializes its mirror before draining the outbound queue, so the saved program has the old drawbars.
4. **Outbound queue overflow.** "RT threads publish *every* performance change." Nine drawbars swept, plus aftertouch and pitch bend, plus 8 parts, fill any fixed SPSC queue. RT must not block (AD-3), so it drops events, and the mirror is wrong forever. The spine says nothing about what happens on drop.
5. **Physical position after program change.** On program load, the stored drawbars (888000000) differ from the physical sliders (000000888). Jump, pickup and ignore are all compliant behaviours.

**Proposed rule (new AD-2a, Parameter registry):**
> - Every engine/FX/part parameter is declared once in a generated **parameter registry** (`proto/params`) with: stable `param_id`, class `structural` or `performance`, range, and default. The registry is the only source for ui, desktop, MIDI mapping and engines.
> - A `performance` param has exactly one live value, in the RT plane. Every client that changes it (ui, desktop, panel, MIDI) sends a UMP-equivalent event to RT; none send it as a control-thread command. The snapshot carries performance params only as **initial values** tagged with `program_generation`; RT applies them only when `program_generation` changes, never on other structural swaps.
> - RT publishes performance state as a **last-value table** (per param, atomic writes or seqlock, plus a change counter), not a queue. Discrete events (notes, transport) use the queue; queue overflow sets a `resync` flag that makes the control thread re-read the whole table.
> - Save: the control thread posts a fence token to RT, waits for its echo on the outbound path, then reads the last-value table. No save serializes a mirror older than the fence.
> - Program-change pickup behaviour for physical controls is one system setting (sysd), applied in the panel reader, not per engine.

---

## H3. Clock and timestamp domains (High)

**Pairs:** keybed MCU vs. panel MCU; keybed reader vs. sample engine vs. organ engine (layered parts); sysd USB MIDI vs. engine; arpeggiator vs. MIDI-clock follower.

**Both comply:**
- AD-8 says every event carries a "device-monotonic nanosecond timestamp".
- AD-16 says "MCU-side timestamps".
- AD-2 says events are "applied at the next block".
- AD-21 says note generators are "sample-accurate".
- AD-17 says the codec crystal is the audio clock.

**Clashes:**
1. **Three clocks, no mapping.** The keybed MCU stamps in its own µs counter, the panel MCU in another, and the SoC's CLOCK_MONOTONIC runs from a third crystal. Audio frames run from a fourth (the codec). The spine never says who converts MCU time to device time, or device time to frames. The keybed reader adds a fixed offset taken at boot. The panel reader stamps on receipt. They drift by ~50 ppm, about 180 ms per hour. "Sustain pedal up" (panel) and "note-off" (keybed) arrive in the wrong order and notes hang or cut. On a 24 h gig box this is guaranteed.
2. **Block-start vs. sample-accurate.** AD-2 says "applied at the next block". AD-21 says "sample-accurately". The sample engine applies at block start. The organ engine converts the timestamp into an in-block offset. A key that plays a piano and an organ layered then has up to 1.33 ms of random flam between the layers, and the jitter adds to the 5 ms budget.
3. **Timestamp → frame policy.** One engine schedules at `frame(ts)` (late events get clamped). Another schedules at `frame(ts) + fixed_delay` (constant latency, zero jitter). Both are "applying timestamped events". Null tests against the offline renderer (AD-19) differ by whichever policy the renderer picked.
4. **Desktop and USB MIDI.** AD-8 has "the desktop app" stamp at the edge. If the desktop stamps with host time, that is a fourth unrelated clock.

**Proposed rule (AD-8, tightened; new AD-8a, Time base):**
> - Device time = `CLOCK_MONOTONIC_RAW` ns on the SoC. Each MCU link runs a sync exchange (defined in `HARDWARE-INTERFACES.md`) and the SoC-side reader converts MCU ticks to device time with continuous offset+drift estimation. MCUs never send device time; external sources (DIN, USB MIDI, desktop) are stamped on arrival by the engine-side reader, never by the sender.
> - The RT plane owns the single **device-time → frame** mapping (a DLL fed once per block by the ALSA hw pointer and `CLOCK_MONOTONIC_RAW`). No engine or processor reads a clock.
> - Every event is scheduled at `frame = map(ts) + D_sched` with one system-wide constant `D_sched` (≤ one block), delivered to engines as an **in-block frame offset**. Events later than their frame are applied at offset 0 and counted. All engines and processors honour the offset; "next block" application is forbidden.
> - The offline renderer uses the same scheduling function.

---

## H4. Voice-pool accounting and allocation phase (High)

**Pairs:** sample engine vs. FM engine vs. organ engine (different authors and dates); pool vs. arpeggiator.

**Both comply:**
- AD-7 says engines "allocate voices only from the shared pool, within its guaranteed floor".
- AD-5 says "voices of every part render in parallel across the RT workers".
- AD-6 says a voice touches only its own state.
- `engines.md` says the pool is "budgeted by CPU".

**Clashes:**
1. **Unit of a voice.** The sample engine counts 1 stereo voice = 1. The FM author counts 1 voice = 1 note of 6 operators, which costs ~3× a sample voice. A "budgeted by CPU" pool that counts slots will let FM starve the CPU. A pool that counts CPU makes "≥128 FM voices" a CPU claim no one has checked against the 256 sample voices plus organ plus FX on 3 cores. Floors are stated as counts, while the pool is stated as CPU.
2. **The organ breaks AD-6.** A tonewheel organ has 91 *shared* tonewheels that all keys read, plus leakage between them. An author who follows AD-6 strictly must either make the whole organ one "voice" per part, which makes pool accounting meaningless ("never stolen" becomes an untracked CPU hog), or put the tonewheel generator in per-part shared state "after the barrier". That is too late, because voices need its output *before* the barrier. The rule has no pre-voice phase.
3. **Who steals.** "Protect the lowest held note" — lowest in the part, in the engine, or in the whole performance? Engine A implements stealing inside the engine. The pool implements it globally. A bass split on the sample engine plus a pad on VA gives different survivors depending on whose rule runs.
4. **Allocation inside the parallel render.** If two workers allocate from the pool at the same time, they race. AD-3 allows only SPSC and atomics, and a lock-free multi-consumer allocator is not SPSC. Nothing places allocation in a serial phase.
5. **Stealing fade-out.** A voice being stolen needs a ~2–5 ms fade. Does it still count against the floor? One author says yes, another says no, and floors are violated during bursts.
6. **Streaming capacity.** Sample voices also consume stream-buffer slots and NVMe bandwidth. A pool that grants CPU slots can still start a voice the I/O threads cannot feed, and the result is a dropout.

**Proposed rule (AD-7, tightened; new AD-5a, Block phases and pool):**
> - Each RT block runs fixed phases: (1) **serial**: drain input queues, run event processors (AD-21), and do all voice allocation/stealing; (2) **parallel per part**: shared per-part generators (e.g. tonewheel bank), the only pre-voice shared state, written before voices read it; (3) **parallel**: voice render (AD-6); (4) barrier; (5) per-part FX; (6) sends/master.
> - The pool is the **sole** allocator and stealer. Engines declare per-voice **cost units** (measured CPU per block on the target, plus stream slots for streaming voices) and expose per-voice steal metadata (part, note, level estimate, sustained, age). Engines never choose victims.
> - Floors in `engines.md` are expressed and enforced in cost units, reserved at performance load (admission control). A performance whose floors cannot all be reserved loads with a reported degradation, never with an over-subscription.
> - A voice in steal-fade counts against its engine until silent. The organ reserves a fixed cost per active organ part and is outside stealing.
> - Stealing scope is the whole performance; "lowest held note" is evaluated per part.

---

## H5. Input/output topology and link ownership (High)

**Pairs:** sysd (USB MIDI gadget) vs. engine (RT input); engine panel reader vs. ui (buttons, LEDs); sysd (MCU flashing, calibration) vs. engine reader (owns the UART/SPI).

**Both comply:**
- In the diagram, sysd carries "MIDI USB" to the **control thread** over protobuf/UDS.
- AD-3 allows only SPSC queues into RT.
- AD-4 puts the keybed/panel reader inside `engine`.
- AD-16 says sysd flashes the MCUs and calibration lives in sysd config.

**Clashes:**
1. **USB MIDI through the control thread.** sysd sends USB MIDI over UDS to the non-RT control thread. That puts a socket, a protobuf decode and a non-RT thread in the note path. The latency and jitter fail CAP-2, and the control thread becomes a forwarder of performance state, which AD-2 says it never owns.
2. **MIDI DIN has no home.** Nothing in the spine says where MIDI DIN is read.
3. **Multi-producer SPSC.** The keybed reader, the panel reader, DIN, USB and arp feedback all feed RT. If an implementer reads the diagram as "one SPSC queue into RT", multiple producers will corrupt it. If they build several queues, no merge order is defined.
4. **Outbound MIDI is unowned.** MIDI DIN out, USB MIDI out, MIDI clock out and Local Off are not in the spine at all.
5. **Panel buttons are commands, not UMP.** "Program up", "Save", "Favorite" and "Arp on" come from the panel MCU. Under AD-8 they become UMP in engine. The ui expects to own navigation. Two units each implement "Program up", and the program advances twice.
6. **LEDs have three writers.** LED state comes from the ui (menu state), the engine (rotary fast/slow) and RT (arp/tempo blink). Only engine owns the link.
7. **Shared serial link.** sysd flashing or calibrating the panel MCU needs the same serial link the engine reader holds open. Two processes will open one UART.

**Proposed rule (AD-4, tightened; new AD-8b, Ingress/egress):**
> - All realtime MIDI/UMP ingress and egress (keybed, panel, DIN, USB MIDI gadget via ALSA UMP) is read and written by **engine-side I/O threads**, never relayed through sysd, ui or the control thread. sysd only configures the gadget.
> - Each producer gets its own SPSC queue into RT; phase 1 (AD-5a) merges them by device timestamp.
> - The panel reader splits panel input into (a) performance UMP to RT and (b) **`PanelEvent` commands** to the ui (navigation, save, favorites). The panel reader alone performs neither kind of interpretation for the other side.
> - LEDs are driven only by the engine panel writer from a merged LED state: ui sends `SetLedState` commands for its LEDs; engine-owned LEDs are listed in the registry.
> - The engine reader is the sole runtime owner of MCU links. Flashing and raw calibration happen through engine commands (`ReleaseLink`/`RawStream`) or before `engine` starts (systemd ordering), never by sysd opening the device concurrently.
> - Outbound MIDI (DIN, USB, clock out, Local Off) is emitted by RT through an outbound UMP queue to the engine I/O thread.

---

## H6. Multi-contact onset and velocity have no representation (High)

**Pairs:** keybed MCU firmware vs. keybed reader vs. organ engine vs. sample engine; sysd velocity curves vs. MCU velocity.

**Both comply:**
- AD-8 says engines consume only UMP.
- CAP-18 requires the organ onset to follow per-contact timing.
- `hardware-platform.md` says the MCU "derives attack and release velocity".
- AD-9 says velocity curves live in sysd config.

**Clashes:**
1. **UMP has no field for contact times.** A UMP Note On carries a single velocity, so contact 1/2/3 closure times have nowhere to go. The organ author invents a per-note assignable controller, the reader author invents SysEx, and the MCU author sends a velocity only. The organ fails CAP-18, or engine and reader integrate with incompatible vendor encodings.
2. **When note-on fires.** An organ must sound at the *first* contact. A piano measures velocity between contacts and fires at the *last*. The reader picks one moment. The other engine is either late, by contact travel time of several ms, which eats CAP-2, or has no velocity.
3. **Double velocity curve.** The MCU applies a curve to compute velocity. The engine applies the sysd curve again. Or neither does. All three are compliant.
4. **2 vs 3 contacts and aftertouch** are unknown (SPEC Open Questions), so the encoding must cover both cases.

**Proposed rule (new AD-16a, Keybed event semantics):**
> - The keybed MCU sends **raw** per-contact closure/open times (MCU ticks) per key; it never applies curves. Velocity derivation and the sysd velocity curve are applied once, in the SoC keybed reader.
> - The reader emits, per key: a vendor **per-note controller `contact_k`** (UMP MIDI 2.0 per-note assignable, numbers fixed in the registry) at each contact closure, then a **Note On** at the velocity-measuring contact with 16-bit velocity; release likewise with release velocity. The note number is fixed at the first contact.
> - Engines that need early onset (organ) start on `contact_1` and treat the later Note On as velocity confirmation; others ignore `contact_k`. Contact count is a calibration field.

---

## H7. Snapshot, head and key lifetime vs. sounding voices (High)

**Pairs:** control thread (snapshot reclaim, head unload, key unwrap) vs. RT voice code; librarian (garbage-collects old bundles) vs. engine streaming I/O.

**Both comply:**
- AD-1 says old snapshots are "reclaimed off the RT thread".
- AD-14 says keys are in memory "only while a library is loaded".
- AD-11 says a published bundle is never modified, but says nothing about deletion.

**Clashes:**
1. **Program change with held notes.** A voice holds raw pointers into snapshot N, its region map and its head PCM. The control thread swaps to snapshot N+1 and frees snapshot N "off the RT thread" once the atomic swap completes. A note still sustaining from program N then reads freed memory. A seamless program change (sound continues through held notes and sustain), which players expect, guarantees this.
2. **Unload too early.** The control thread unloads library L, frees its heads and zeroes its content key, while voices from L are still in their release tail. The I/O threads then decrypt with a zeroed key and play noise.
3. **Old bundle deleted under the engine.** The librarian switches the catalog to bundle B2 and deletes B1. If the engine opens per-sample files lazily by path, it fails. If it holds fds, disk space is not reclaimed until restart. Neither side knows the other's policy.
4. **Heads not ready.** A snapshot that references heads that are not yet loaded gets swapped in, and voices start with no head (AD-3 forbids waiting).

**Proposed rule (AD-1, tightened; new AD-11a, Leases):**
> - Snapshot, head set and content key form one **generation**, reclaimed by epoch: RT publishes the oldest generation any live voice references (per block); the control thread frees a generation only when RT has passed it. Voices pin the generation they started in.
> - A snapshot is swapped in only after all heads it references are resident; until then the previous snapshot stays live (the part keeps its old sound or is silent, per the AD-10 state).
> - The engine opens all files of a bundle at library load and holds them; it reports in-use `bundle_id`s to the librarian (`BundleLease`). The librarian deletes a superseded bundle only when no lease holds it.

---

## H8. Versioning gaps beyond patches (High)

**Pairs:** desktop app vX vs. firmware vY; desktop formatlib vs. device formatlib; firmware slot A vs. slot B (RAUC rollback); MCU firmware vs. SoC frame parser; librarian SQLite schema across slots.

**Both comply:** AD-12 versions programs, performances and config with forward migration only. AD-13 pins package `ws.v1`.

**Gaps:**
1. **Desktop/firmware skew.** A desktop app on 2.11 talks to newer firmware that added an error code to the "closed enum", and the app crashes or mis-handles it. No handshake exists.
2. **Bundle format version.** The desktop app's newer formatlib writes a bundle format that the device's formatlib cannot read. AD-12 does not cover bundles.
3. **Rollback is a downgrade.** RAUC rolls back from B to A. Programs saved by B (`schema_version` N+1, an FM part from an engine A does not have) must open on A. AD-12 is forward-only, so A refuses them or drops the FM part on the next save. Config and the SQLite catalog migrated by B are unreadable on A.
4. **MCU firmware vs. slot.** Slot B flashed MCU firmware with frame protocol v2. Rollback to A, which parses v1, leaves the keybed dead.

**Proposed rule (AD-12, tightened; new AD-13a):**
> - Every connection (UDS and NCM) starts with `Hello{protocol_major, protocol_minor, capabilities[]}`; majors must match, minors negotiate down. Every enum has `UNSPECIFIED = 0` and receivers map unknown values to a generic code, never fail.
> - Bundles carry `format_version`; the device advertises its supported range in `Hello`; the desktop writes only within it; the librarian rejects out-of-range bundles before publish.
> - Downgrade safety: a firmware loads newer-version programs read-only-compatible (unknown fields preserved, unknown engines load the part silent as "missing engine", per AD-10) and never re-saves them in a lossy way; config and catalog migrations keep the previous schema readable by the previous slot (expand-only migrations), or keep a pre-migration copy per slot.
> - On every boot sysd checks MCU protocol versions against the running slot and reflashes on mismatch, before engine opens the links.

---

## H9. IPC state sync and concurrent edits (Medium-High)

**Pairs:** ui vs. desktop app (both editing); sysd bridge vs. control thread (response routing); ui vs. engine after an engine restart.

**Both comply:** Commands carry a "client-generated `request_id`". State is "full-state snapshots plus deltas".

**Clashes:**
1. **request_id collisions.** The ui and two desktop connections bridged by sysd all start their `request_id`s at 1. The control thread replies "request 7 failed", and sysd or the ui routes it to the wrong client.
2. **Unsequenced deltas.** A client subscribes, receives a delta before the snapshot, or loses one across a sysd reconnect. It applies deltas to the wrong base, and the ui shows drawbar or part state that differs from the sound. That is the exact failure AD-2 says it prevents.
3. **Engine restart.** The engine crashes and restarts. The ui keeps applying new deltas to its old full state.
4. **Lost update.** The desktop app loads program P, the user edits P on the device, and the desktop then sends a full `SaveProgram{P}`. The device edit is gone with no conflict reported.

**Proposed rule (Conventions → Commands, tightened):**
> - Request identity is `(connection_id, request_id)`; sysd assigns `connection_id` per bridged client and routes on it.
> - Every published state stream carries `(epoch, seq)`; `epoch` changes on publisher restart; snapshots carry `seq`; a delta applies only if `seq == last+1`, otherwise the client re-subscribes for a snapshot.
> - Write commands on persistent entities carry `base_revision`; the writer rejects stale writes with `CONFLICT`. Partial-field commands are preferred to whole-object replace.

---

## H10. Program vs. catalog ownership; program ID collisions (Medium)

**Pairs:** engine control thread (writes programs) vs. librarian (owns catalog: search, categories, favorites, tags); factory content build vs. user saves; desktop program import vs. on-device programs.

**Both comply:** AD-9 gives programs and performances to the control thread, and favorites, tags and categories to the librarian.

**Clashes:**
1. **Program metadata has two owners.** CAP-15's "favorite with one touch" applies to programs as much as to instruments. Is a *program* favorite or category in the catalog (librarian) or in the program file (engine)? Both implementations are compliant. Search over programs needs an index that only the librarian has, but the librarian does not write programs. So either the engine notifies the librarian, or the librarian indexes files that another process writes, which no AD defines.
2. **Factory programs.** Their UUIDs are fixed in firmware. A user edits and saves factory program F. Under one reading the save keeps F's UUID on /data. A firmware update ships a new F with the same UUID, and the unit now has two programs with that ID.
3. **Program import.** A program imported from another unit via the desktop app has a UUID that may already exist locally, and it silently overwrites the local one.

**Proposed rule (AD-9, tightened):**
> - Programs/performances: content owned by the engine control thread; **all browse metadata (favorites, categories, tags, search index) for programs and instruments** owned by the librarian catalog, keyed by entity id. The engine emits `ProgramSaved{id, revision, name, engines[]}` and the librarian indexes from that event only.
> - Factory programs are read-only on rootfs; any edit saves as a new UUID with `derived_from`. Import of an existing UUID with different content mints a new UUID.

---

## H11. Write-discipline contradictions and power-fail path (Medium)

**Pairs:** sysd power-fail vs. librarian/engine writers; logging ring vs. AD-18; background indexing vs. "disk read-only during performance".

**Clashes:**
1. **Writes during a performance.** AD-18 says the disk is read-only during a performance, yet the logging convention writes a journald ring "on the data partition". Toggling a favorite writes SQLite WAL. Boot indexes "in the background" (performance-targets.md). Nothing defines when "a performance" starts or who declares it.
2. **sysd cannot freeze other processes' writes.** It can only FIFREEZE the filesystem, and then the librarian blocks mid-rename and the control thread blocks mid-save. Each must tolerate an indefinite block, and nothing says so.
3. **The fade path is too slow.** The power-fail signal travels sysd → UDS → control thread → RT. The fade latency then depends on the non-RT scheduler, within an unspecified supercap hold-up.

**Proposed rule (AD-18, tightened):**
> - Logs go to a RAM ring; persisted only on clean shutdown or explicit export. Allowed writes during play: explicit save, favorites/catalog edits (batched), imports the user started. Background indexing is read-only on bundles; its catalog writes are batched and paused while the voice count is above a threshold.
> - Power-fail: a dedicated engine thread waits on the GPIO and sets an atomic `fade` flag that RT honours within one block; sysd independently FIFREEZEs `/data` and flushes. All writers use write-to-temp + fsync + rename so a freeze at any point is safe.

---

## H12. Arpeggiator vs. future sequencer; clock-source ownership (Medium)

**Pairs:** arpeggiator (v1) vs. Pro sequencer; sysd config vs. RT for clock source; ui transport vs. RT.

**Both comply:**
- AD-21 says note processors run "per part" on RT, there is exactly one clock, and tempo/transport are performance state.
- The Deferred table says the sequencer "must be a client of AD-1".
- AD-9 says MIDI config is owned by sysd.

**Clashes:**
1. **Per-part vs. multi-track.** A sequencer is multi-part and records. It cannot be both a per-part RT processor (AD-21) and a client of AD-1 (the Deferred table). The song data owner is undefined. The arp interface will be built per part, so the sequencer's needs force a rewrite.
2. **Chain order.** Sequencer output → arp → engine, or arp output → sequencer recording? Undefined.
3. **Clock source has two owners.** Is "internal vs. MIDI clock from DIN/USB" a sysd MIDI config (AD-9) or RT performance state (AD-21)? Tempo is performance state, but it is also saved with a performance (structural). That is H2 again.
4. **Transport start.** A ui transport start goes as a command to the control thread, but transport is RT-owned. No path is defined.

**Proposed rule (AD-21, tightened):**
> - Event-processor chain per part is fixed: input merge → [sequencer track output merges here] → arp → engines. Processors implement one `EventProcessor` port: `process(in_events, transport_view, out_events)` on phase 1 of AD-5a, no other state access.
> - Sequencer song data is a structural dataset owned by the control thread (AD-1/AD-9 row added); playback is an RT processor reading an immutable song snapshot; recording flows RT → outbound queue → control thread.
> - Clock source and its follow behaviour are performance params in the registry (AD-2a) with saved initial values; sysd holds only port enablement. Transport commands from any client are UMP system real-time / vendor events into RT.

---

## H13. FX topology and outputs (Low-Medium)

**Pairs:** organ engine (rotary in voice path) vs. FX module (rotary as effect); per-part FX vs. shared reverb; mixer vs. USB UAC2 channel map.

**Clashes:**
1. **Rotary has two homes.** `engines.md` puts the rotary in the organ voice path *and* in the FX. Two implementations then double-apply it, or use different parameter ids for the same rotary speed (the panel's "fast" button needs one target).
2. **Send/global FX.** A shared reverb across parts is not addressed. AD-5 lists only "per-part FX, then master mix".
3. **Output routing.** Routing to Main, Aux, headphones and the USB channels is undefined. FX authors and USB gadget authors will disagree on channel count and order.

**Proposed rule (AD-5, tightened):** The render graph is fixed: part voices → part insert FX (≤N) → part output bus (main/aux/USB-pair assignment, a structural param) → global send FX (reverb, delay) → master. The rotary is one FX module; the organ engine instantiates it as its default insert. The USB UAC2 channel map is defined in `HARDWARE-INTERFACES.md`.

---

## H14. Error, logging and ID smaller seams (Low)

- **Error codes:** they are a "closed enum", but nothing defines who adds codes or how old clients behave. This is covered by H8's `UNSPECIFIED`/unknown rule.
- **RT log queue overflow:** like H2, an RT log record is dropped silently. Add a dropped-records counter that the control thread logs.
- **UUIDv7 time source:** the unit has no RTC or network (no radio). UUIDv7 embeds wall-clock time, and that clock may read 1970 on every boot, so UUIDs sort wrongly and lose collision entropy in the time field. **Rule:** mint UUIDv7 from a persisted monotonic counter seeded by the last-seen max time (sysd), or use UUIDv4 for program IDs and stop relying on time order.

---

## Priority order for closing

| # | Hole | Severity | Bites when |
|---|---|---|---|
| H1 | Library identity / bundle version / instrument-key derivation / engine lookup | Critical | First re-import or library update; first desktop conversion |
| H2 | Structural vs. performance parameter registry, snapshot clobber, save fence | Critical | First ui knob edit plus a panel knob on the same param |
| H3 | Clock domains and in-block scheduling | High | First layered patch; first long gig (drift) |
| H4 | Pool units, block phases, organ shared generator, central stealing | High | Second engine (organ) joins the sample engine |
| H5 | Ingress/egress topology, panel commands vs. UMP, link ownership | High | USB MIDI latency test; first panel button |
| H6 | Multi-contact onset encoding, single velocity curve | High | Organ CAP-18 integration |
| H7 | Generation and lease lifetime of snapshot, heads, key, bundles | High | First program change with held notes |
| H8 | Handshake, bundle format version, rollback downgrade, MCU reflash | High | First RAUC rollback; first desktop release |
| H9 | Request routing, (epoch, seq), base_revision | Medium-High | Desktop app plus ui used together |
| H10 | Program metadata owner, factory/import UUIDs | Medium | Favorites UI build |
| H11 | Write-during-play list, power-fail fade path | Medium | Power-cut tests (CAP-17) |
| H12 | Processor chain, sequencer data owner, clock-source owner | Medium | Arp design (v1); otherwise the Pro sequencer forces a rewrite |
| H13 | FX graph and output map | Low-Med | FX build; USB audio |
| H14 | UUIDv7 without an RTC, log drops | Low | First boot with no clock |

Also: write `HARDWARE-INTERFACES.md`. It is cited as binding and missing, and H3, H5, H6 and H13 each need rules placed in it.
