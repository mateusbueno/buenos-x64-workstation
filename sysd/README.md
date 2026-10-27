# sysd

Role: the host process that **owns the system configuration** (AR17, AR39, AD-12) and serves it to `engine` and `ui` at start and on change. Foundation of Stories
7.7 (clock source), 3.6 (velocity curves), 3.11 (pedal calibration) and 8.9 (Boot into): they add behaviour that consumes the published config; this foundation
adds the owner, the file, the channel and the client.

- `velora-sysd` (C++20, `sysd/src/main.cpp`): `velora-sysd --data-dir DIR [--socket PATH] [--duration SECONDS] [--slot-good]`. Becomes a systemd unit on the
  board (example below). Only sysd writes the config.
- `velora_sysd_service` (static library): `ConfigStore` (the file), `ConfigService` (the ws.v1 protocol as a state machine, no sockets), `ConfigServer` (the
  Unix domain socket).
- Engine side: `engine/host` `SysdConfigClient` (`velora/host/sysd_config_client.hpp`), wired into `engine-host` behind `--sysd-socket PATH`.
- Dev CLI: `velora-sysdctl [--socket PATH] get | set FIELD VALUE... | watch [COUNT]` (`tools/sysdctl/`).
- **TCP bridge** (Foundation: Velora Studio device connection; `BridgeServer`, `PairingWindow`, `PairedStore`): the one door through which Velora Studio reaches the unit
  (below, "TCP bridge"). Enabled with `--bridge-port`.

## Ownership and file layout

One file, `<data>/config/sysd_config.pb` (dir created 0755, file 0644, owned by the sysd user per AD-9), holding `velora.sysd.v1.SysdConfig`
(`proto/sysd_config.proto`): `schema_version`, `revision` and the ws.v1 `Config`. `<data>/config/.writer.lock` is an exclusive `flock` held for the life of the
process: a second sysd on the same directory refuses to start. Writes use the data root's atomic replace (`liveset::atomic_replace_file`: temp file, fsync,
rename, directory fsync) with its stage hook for fault injection.

| Situation | Behaviour |
|---|---|
| no file (first start) | defaults served, revision 0, **the file is created on the first change only** |
| `schema_version` N | loaded |
| `schema_version` N-1 | loaded; never rewritten on read. A SetConfig keeps the N-1 stamp (so a rollback to the previous firmware still reads it) until the Story 8.5 migration runner (`ConfigStore::migrate()`, `migration_unit()`; called at start and to be called again on the RAUC mark-good event) rewrites it as N, only after the slot probe says good |
| too old / unknown major / oversize / unreadable | defaults + a logged typed reason (`too_old` / `too_new` / `too_large` / `unreadable`); the file is untouched on read. Before the first write replaces it, a hard link keeps its bytes as `sysd_config.pb.<reason>` (`.N` suffix when that name exists: an aside is never overwritten; rename is the fallback where links are unsupported). The primary stays in place until the atomic replace succeeds, so a failed replace or a crash between the two steps leaves it untouched |
| a pedal range of 1..255 counts (written before Story 3.11's minimum span) | the file is **not** voided: that pedal is read as uncalibrated (min = max = 0, polarity kept) with a logged note and everything else loads; the span rule applies to patches (the merged result), not to loading |
| corrupt bytes / invalid values | defaults + logged reason (`corrupt`, `invalid_content`); the bad file is renamed `sysd_config.pb.corrupt` (`.1`, `.2`, ... for later ones: two corruptions in a row keep both) |
| kill at any write stage | the replace is an atomic rename, so the next start reads the old file or the new one, never a half-written one (tests kill at each stage, simulated and with real process kills). A crash may leave a stale `sysd_config.pb.tmp`, which the next write replaces; this does not protect against media corruption, which loads as `corrupt` (above) |

Defaults: clock source INTERNAL, velocity curve NORMAL, pedals uncalibrated (min = max = 0, polarity unspecified), Boot into LAST_SOUND, no default Live Set.
Unknown fields (at the file level, in `Config`, in a pedal) are kept on every rewrite. Fields absent from an older file are filled with the defaults in memory.

**Persist failure policy** (chosen and fixed): a change that cannot be persisted is **rejected** with `ERROR_CODE_STORAGE_FULL` (ENOSPC) or
`ERROR_CODE_STORAGE_FAILED` (`details.reason`), and nothing changes in memory or in the config file (a failed write removes its own temp file; a leftover aside link from the too-new/too-old path is kept). The one exception is a rename that happened
whose directory fsync failed: the change is **applied** and the reply and pushes carry `not_durable = true` (as Story 4.6 does for setlists).

## ws.v1 messages (additive, in `proto/ws_v1.proto`)

Framing, Hello and the N / N-1 version rule are the engine control server's (4-byte big-endian length, `ClientMessage` / `ServerMessage`, largest frame 65536
bytes; HelloAck feature `config.v1`). The socket is created 0600.

| Client message (body no.) | Reply | Notes |
|---|---|---|
| `Hello` (10) | `HelloAck` | first message; unsupported version closes the connection |
| `GetConfig` (55) | `ConfigReply {config, revision}` | |
| `SetConfig {patch}` (56) | `ConfigReply {config, revision, not_durable}` or `Error` | `ConfigPatch` has only the fields to change, **field-wise**: `PedalPatch {optional min, max, polarity}` and `BootIntoPatch {optional mode, default_live_set_id}` change only the fields present, so a patch with only a polarity, only min or max, only the Live Set id or only the mode never clears the others. The merged result is validated as a whole (min < max, DEFAULT_LIVE_SET needs an id); unknown fields inside a patch are dropped silently. Validated as a whole, applied atomically, persisted, `revision + 1`, then pushed. An identical patch answers the current config, changes nothing and pushes nothing |
| `SubscribeConfig {enable}` (57) | `Ack`, then (enable) a `ConfigReply` push at once | afterwards a `ConfigReply` push (`request_id` 0) to every subscriber, the requester included, after each change |

`ServerMessage.config_reply` is body 25. The engine's own control server answers the three config messages with `ERROR_CODE_INVALID_COMMAND` (it does not serve
them; sysd answers every engine command the same way): the servers are not mixed.

Typed errors (nothing changes, nothing is pushed): unknown / unspecified enum value -> `UNKNOWN_ENUM_VALUE` (`details.field`); pedal min or max above 4095 ->
`OUT_OF_RANGE`; pedal `min >= max` (0/0 means uncalibrated), a calibrated pedal whose `max - min` is under 256 counts (`details.reason = span_too_small`, `details.min_span`; Story 3.11), a bad Live Set id (a patch's `default_live_set_id` must be empty or a canonical lowercase UUID, the Live Set store's id format; Story 8.9) or Boot into DEFAULT_LIVE_SET
without a UUID id -> `INVALID_CONFIG` (new, 41; `details.field`, `details.reason`); an empty patch -> `INVALID_COMMAND` (`reason=empty_patch`); persist failures as above.

`revision` is persisted and strictly increases with every change; SetConfig is serialised under one mutex (single writer), so two concurrent SetConfig never
interleave. Every reply and push is produced inside the call that made the change, so subscribers get the config well within 100 ms (the service tests assert
"before `handle()` returns"; the socket test measures it).

## The engine client

`SysdConfigClient` runs its own thread: connect, Hello, `SubscribeConfig`, store the latest `Config`; reconnect with exponential backoff (50 ms doubling to 2 s)
while sysd is absent or gone. It never blocks the RT plane or the control tick: the control thread calls `tick()` (a short mutex for a copy) and the **callback
runs there**, once per revision, with the revision guard (a push not newer than the last of the connection is ignored; the first config of a new connection is
delivered when it differs from the last-known one, so a sysd that restarted without its file is not hidden by an old high revision; several revisions between two
ticks deliver the latest). Without sysd the engine runs on `default_config()`; on disconnect it keeps the last-known config (`config()`).

## How a dependent story adds its behaviour (no protocol change)

Clock source 7.7, velocity curve 3.6, pedal calibration 3.11 and Boot into 8.9 already have their fields in `Config` (INTERNAL / DIN / USB; NORMAL / LIGHT /
HEAVY / FIXED; sustain and expression `{min, max, polarity}` on the 12-bit pedal range; `boot_into {mode, default_live_set_id}`). A story:

1. consumes them in the `SysdConfigClient` callback (`engine/host/macos/main.cpp` currently only logs them) or reads `client.config()` on the control thread;
2. if it needs a new field, adds it to `Config` **and** `ConfigPatch` in `proto/ws_v1.proto` with the next free number (numbers never reused, `reserved` when one
   is retired), its validation in `validate_patch()` / `valid_config()` (`sysd/src/config_store.cpp`) and its default in `default_config()` and `normalized()`.
   Additive fields need no `schema_version` bump and no migration; bump `kSchemaVersion` only for a change an old reader cannot survive, with a unit in the
   migration runner;
3. adds a CLI field to `velora-sysdctl` if it wants one.

## Clock source (Story 7.7): how `clock_source` flows

`clock_source` (INTERNAL / DIN / USB, default INTERNAL) is the first setting with behaviour. sysd stays the single owner and the single source of truth:

- **ui -> sysd**: Settings > MIDI & Clock > Clock source asks through the injectable `SysdConfigSender` (`ui/include/velora/ui/sysd_config_sender.hpp`); the real
  one (`ui --sysd-socket PATH`) uses the blocking `velora_sysdctl_client` on a worker thread (the UI loop never waits), sends a `SetConfig` patch with only
  `clock_source`, subscribes to config pushes and reports what sysd says. The control shows the value sysd reported; the local `SystemConfig` JSON field is only
  a cache of it. A request that cannot be delivered or is refused comes back as a notice and the control returns to the last reported value. Without
  `--sysd-socket` the ui keeps the old local behaviour.
- **sysd -> engine**: the `SysdConfigClient` callback (control thread) calls `host::apply_clock_source`: the MIDI ingest's port filter first, then
  `BlockRenderer::set_clock_source` (an atomic; the RT plane resets the follower at its next block; no voice is touched). Until sysd answers, or when it is absent,
  the engine stays on INTERNAL (or on the last applied value); an unknown value is not applied.
- The ui sender's destructor joins its worker, which is bounded by the connect / reply timeouts (2 s) plus a 100 ms poll, so closing the ui can take up to that long
  while sysd is hung.
- **Persistence**: the value lives in `sysd_config.pb`, so it is restored on restart (tests: `sysd_sender_test`, `engine_host_sysd_smoke`).

## Velocity curve (Story 3.6): how `velocity_curve` flows

`velocity_curve` (NORMAL default / LIGHT / HEAVY / FIXED) is set from the dev CLI and takes effect for the next note, without restarting the engine:

```
velora-sysdctl --socket /run/velora/sysd.sock set velocity_curve light     # normal | light | heavy | fixed
velora-sysdctl --socket /run/velora/sysd.sock get
```

- **sysd -> engine**: the `SysdConfigClient` callback (control thread) calls `host::apply_velocity_curve`, which stores the curve in a `VelocityCurveHolder`; the holder pushes it to
  every attached `KeybedReader` (`set_velocity_curve`, an atomic store, safe against the reader running on its own thread). The reader applies it once, after the keybed calibration
  table, when it computes the next Note On velocity; a sounding note is untouched. An unknown value is not applied (the last curve is kept). `engine-host` has no keybed link yet: it
  owns the holder, and the link wiring will attach its reader (the Settings UI row is Story 10.2).
- **Persistence**: the value lives in `sysd_config.pb`, so after a reboot sysd serves it again and the engine re-applies it on connect (test: `sysd_socket_tests`, `test_velocity_curve_via_sysd`,
  real sysd on a temp dir, restarted). Without sysd the engine runs on Normal.
- Details of the curves, the calibration and the release velocity: `engine/host/README.md`.

## Pedals (Story 3.11): how `sustain` / `expression` flow

`sustain` and `expression` (`{min, max, polarity}` on the raw 12-bit pedal range) have behaviour now. Polarity: UNSPECIFIED and NORMALLY_OPEN keep the raw direction (pressed =
raw rises), NORMALLY_CLOSED inverts it (pressed = raw falls); `0 / 0` is uncalibrated (the full 0..4095). The footswitch pedal has no sysd field: its wiring is the engine flag
`--switch-polarity open|closed|auto` until Story 3.8's detection supplies it. How the engine maps, emits and calibrates: `engine/host/README.md`, "Pedals".

```
velora-sysdctl --engine-socket /run/velora/engine.sock calibrate-pedal sustain        # press the pedal fully, release, press Enter
velora-sysdctl --engine-socket /run/velora/engine.sock calibrate-pedal expression --seconds 8   # finish after 8 s instead of waiting for Enter
velora-sysdctl --socket /run/velora/sysd.sock set sustain ,,closed                    # only the polarity (normally closed); min / max are kept
velora-sysdctl --socket /run/velora/sysd.sock set sustain 500,3800                    # a range by hand (min < max, span >= 256)
```

- **Measurement stays on the engine** (it owns the panel reader): `calibrate-pedal` talks to the **engine's** ws.v1 socket (`--engine-socket`), not to sysd. The engine writes the
  result to sysd with a `SetConfig` patch holding only that pedal's `min` and `max`, so the stored polarity survives a recalibration.
- sysd validates the whole config as always: a degenerate range is refused with `INVALID_CONFIG` / `span_too_small` and nothing changes (the engine also refuses it first, typed, before writing).
- **sysd -> engine**: the config callback (control thread) calls `host::apply_pedal_config`, which stores the calibration and polarity lock-free in the reader's `PedalSettings`; the input thread uses
  them from the next pedal frame (a held pedal is re-mapped at once), no restart. **Persistence** is sysd's file (tests: `sysd_pedal_calibration_test`, real sysd in a temp dir, restarted).
- sysd absent: the engine runs uncalibrated in the raw direction; a calibration then ends `SYSD_UNAVAILABLE` and stores nothing.

## Boot into (Story 8.9): how `boot_into` flows

`boot_into {mode, default_live_set_id}` (LAST_SOUND default / START_OF_SETLIST / DEFAULT_LIVE_SET) says where the unit starts after power-on.

- **ui -> sysd**: Settings > Display & Language > Boot into (Segmented Control: Last sound used / Start of setlist / Default Live Set) asks through the same `SysdConfigSender`
  (`request_boot_into`): the first two are a patch with only `boot_into.mode`; Default Live Set opens a List Row picker of All Live Sets (the engine answers `GetSetlists{include_all_live_sets}` with `all_live_sets`, asked when the picker opens and when the setlist library revision changes; the Live Set that sounds is marked NOW PLAYING and the current default carries a "Default" meta) and the choice is one patch with the
  mode and the id, saved at once. The row shows the value sysd reports; a request that cannot be delivered or is refused is a notice and the row goes back.
- **sysd -> engine**: at start only (a change applies at the next boot). `engine-host --data-dir` reads the first config with a **bounded wait**
  (`boottarget::kSysdBootWaitMs`, 1500 ms, a short-lived client of its own); without an answer in time the boot uses the defaults (LAST_SOUND) and is logged; "never stalls" holds up to that bound, which adds to the boot time when sysd is absent (Story 8.10) and a sysd that starts after the engine boots LAST_SOUND (order the units with `After=velora-sysd.service`). The
  engine's long-lived client still delivers the config to its callback afterwards.
- **Resolution** (`velora/host/boot_target.hpp`, pure, deterministic; the Boot-into pointer of Story 8.8 is the other input): LAST_SOUND = the pointer's Live Set and scene with its setlist
  loaded; START_OF_SETLIST = the first Live Set of the pointer's setlist at scene 1 with that setlist loaded; DEFAULT_LIVE_SET = `default_live_set_id` at scene 1 with no setlist
  loaded. A missing, deleted or invalid target, no / corrupt pointer, a deleted or empty setlist all end on the first Live Set of All Live Sets (the stepping order), each reason
  logged (`boot target: ...`); an empty library gets an empty new Live Set. A missing library is not a fallback: the Live Set loads and the affected parts show Missing library.
- **Pointer**: the tracker starts after the target is applied and the boot position is its baseline (`BootIntoTracker::seed`), so the stored pointer is not overwritten by it; only
  a real user change makes it dirty. Details and tests: `engine/host/README.md` (Boot target).

## Deployment and limits (read before relying on it)

- The socket is created **0600**: `engine` and `ui` must run as the sysd user, or the deployment must decide a group arrangement (not done here).
- The `revision` restarts at 0 whenever sysd serves defaults (no file, or an unusable one); clients treat the first config of a connection as new when it differs
  from what they hold, which covers it, but a revision is not a global clock.
- The N-1 stamp path in `ConfigStore::apply()` and `migrate()` both consult `slot_good`; they must stay consistent (one probe, one meaning).
- Config values received by the client are **not validated** on the engine side: sysd validates, but consumers (Stories 3.6, 3.11, 7.7, 8.9) must treat what they
  read defensively (unknown enum values, a calibration that is not min < max, a Live Set id that no longer exists).
- Deferred on purpose: the persist (fsync) runs on the server's poll thread. (Story 8.9 tightened `default_live_set_id` to a UUID: patches refuse anything else; a legacy
  `[A-Za-z0-9_-]{1,64}` value already stored still loads, and with DEFAULT_LIVE_SET it loads as LAST_SOUND with a logged note, the id kept. Story 3.11 fixed the minimum span of a pedal calibration: 256 counts, validated by sysd and again by the engine, which treats a narrower range as uncalibrated.)
- `velora-sysdctl` waits up to 10 s per request (`VELORA_SYSDCTL_TIMEOUT_MS` overrides).

## Not covered here

- Behaviour exists for clock source (7.7), velocity curve (3.6), pedals (3.11) and Boot into (8.9).
- The ui `SystemConfig` JSON (`ui/src/system_config.cpp`: theme, language, brightness, ...) is **not migrated**: it stays in the ui for now. Moving it into
  `SysdConfig` (and the ui reading it from sysd) is a follow-up.
- The `ui` as a client of sysd (this foundation delivers the engine client and the CLI).
- No systemd unit file in the tree, no TCP / USB bridge, no pairing, no RAUC / MCU duties of sysd. The migration runner's mark-good probe is not wired to RAUC
  (`--slot-good` is a dev flag; without it the migration is deferred).

Example unit (documented, not shipped):

```ini
[Unit]
Description=Velora system config service (sysd)
[Service]
User=sysd
ExecStart=/usr/bin/velora-sysd --data-dir /data --socket /run/velora/sysd.sock
RuntimeDirectory=velora
Restart=on-failure
```

## Tests

`sysd_config_tests` (store: defaults, validation, load policy, N / N-1 / too old / unknown major, corrupt, unknown-field round trip, migration, persist failures,
kill at each stage simulated and by real kills, single writer), `sysd_service_tests` (protocol with a fake loop, concurrent SetConfig), `sysd_socket_tests`
(real UDS in a temp dir: framing and limits, latency, stale / live socket, the engine client with a late-starting sysd, reconnect, revision guard, never
blocking), `control_tests` (the engine refuses the config messages), `engine_host_sysd_smoke` (real `engine-host`, `velora-sysd`, `velora-sysdctl`).


## TCP bridge (Velora Studio device connection; Story 9.3 Mac/localhost logic, pairing-lite of Story 9.6, relay of Story 9.7)

Velora Studio speaks the same ws.v1 schema as the on-device clients, framed on **one TCP port** over the USB NCM link (4-byte big-endian length + protobuf,
largest frame 65536 bytes, exactly the Unix socket framing). `velora-sysd` bridges that port to the unit's services. The Mac build runs it on localhost.
This is the unit's only network door; the rules below are what protect it.

```
velora-sysd --data-dir DIR --bridge-port 7410 [--bind 127.0.0.1] --engine-socket /run/velora/engine.sock [--librarian-socket /run/velora/librarian.sock] \
            [--pairing-code CODE | --dev-bypass-pairing]
```

| Flag | Meaning |
|---|---|
| `--bridge-port PORT` | enables the bridge; `0` picks an ephemeral port (printed: `sysd: bridge ws.v1 on HOST:PORT ...`). Must be a number 0..65535, anything else is refused. Without it nothing listens on TCP |
| `--bind ADDR` | IPv4 or IPv6 literal, default `127.0.0.1` (the Mac). The NCM interface address is board work |
| `--engine-socket`, `--librarian-socket` | the upstream Unix sockets; a missing one answers `UPSTREAM_UNAVAILABLE` |
| `--pairing-code CODE` | **DEV / TEST ONLY**: a fixed code, at least 6 digits (shorter or non-digit codes are refused). Logs a loud warning |
| `--dev-bypass-pairing` | **DEV HOST ONLY**: every client is paired at Hello, the model is `DEV HOST`. Logs a loud warning. Never on a unit |
| `--i-know-dev-flags-on-a-network` | the two dev flags above are **refused** when `--bind` is not a loopback address (127.0.0.0/8, ::1) unless this override is given |

The dev flags are runtime flags of the same binary: **nothing in the build stops a production image from passing them**. The guard is the loopback check above plus the loud
warning; the board image's unit file must not pass them (a build-time exclusion is deferred to the board work).

**Handshake.** `Hello {version, features[], client_name, client_version, client_id, pairing_token}` -> `HelloAck {version, features, device_version, firmware_version, device_id,
model, paired}`. `features` is the **negotiated set**: the client's list intersected with the device features (`params.v1`, `snapshots.v1`, `subscribe.v1`, `liveset.v1`,
`config.v1`, `library.v1`) plus `pairing.v1`. Version rule as the engine's control server (N / N-1; another major is `UNSUPPORTED_VERSION` and the connection closes). The
`client_id` only **names** the computer (it is kept only when it is 1..64 of `A-Z a-z 0-9 - _ .`, so it can never inject into a log or the paired file); it is not a credential.

**Pairing.** A computer is paired only by `PairRequest {code, computer_name}` with the code the unit shows. The reply `PairReply {paired, computer_id, pairing_token}` carries a
**pairing token: 256 random bits (hex) from `getentropy`, returned once**; the unit stores only its SHA-256 (`<data>/bridge/paired_computers.txt`, mode 0600, dir 0700, atomic
replace; up to 32 computers, the least recently connected is evicted at the cap). From then on a `Hello` pairs only when `client_id` is known **and** SHA-256(`pairing_token`)
equals the stored hash (constant-time compare). A known id without the token, with a wrong token, or with another computer's token is `HelloAck{paired=false}` and **nothing is
written to disk** for it. A client learns the unit's `device_id` from the first `HelloAck` and may then send **one** more `Hello` on the same connection carrying the token it holds
for that device id; any later `Hello` is `INVALID_COMMAND`. The pairing window opens at start with the code, lasts **2 minutes**, closes after **5 wrong codes** (a global
counter: `PAIRING_CODE_INVALID` with `details.attempts_left`, then `PAIRING_CLOSED` with `details.reason` = `attempts` | `expired` | `not_open`) and **closes at the first
successful pairing** (one code pairs one computer). The code is compared in constant time; the clock is injected (tests do not sleep).
Code strength: a 6-digit code is about 20 bits, protected by the 5 global attempts per window and the 2-minute life, not by its entropy. The code is printed on stdout (journald on a
unit): a dev convenience until the unit's own pairing UI (Story 9.6) shows it on the display only. There is **no reopen path** on a unit before Story 9.6; a closed window
stays closed until `velora-sysd` restarts. Because the attempts are global, anyone who can reach the port can burn them and deny pairing: a per-source cooldown is deferred
(Story 9.6). Randomness is `getentropy` only and **fails closed**: with no entropy the bridge does not start (there is no `rand()` fallback anywhere). A corrupt paired file (or an
old v1 one without tokens) reads as an empty list **and a new device id**, logged: every stored token was bound to the old id, so computers pair again.

**The message table (a total table, no default).** `BridgeServer::message_rules()` classifies **every** `ClientMessage` body: `{field, feature, route, unpaired_allowed}`. A
test walks the proto descriptor and fails when a body is added without a row. An unpaired client gets `NOT_PAIRED` for everything whose `unpaired_allowed` is false (that is
every row except `Hello` and `PairRequest`, and also unknown bodies and the empty message); a paired client gets `INVALID_COMMAND` for an unlisted body, `INVALID_COMMAND`
(`reason=not_available_over_bridge`) for a denied one, and `UNSUPPORTED_FEATURE` when the row's feature was not negotiated.

| Route | Rows |
|---|---|
| engine, `params.v1` (edits) | `set_insert_chain`, `set_send_levels`, `set_part_output`, `set_master_params`, `set_timbre_ref`, `set_performance_param`, `set_part_timbre`, `set_part_volume` |
| engine, `snapshots.v1` / `subscribe.v1` (state) | `get_state`, `get_performance_values`, `get_diagnostics` / `subscribe` |
| engine, `liveset.v1` (scenes, Live Sets, setlists, Write, the Live Set's ASSIGN map) | `recall_scene`, `next_scene`, `previous_scene`, `jump_to_live_set`, `cancel_load`, the setlist and tag commands, `create_live_set`, `delete_live_set`, `next_live_set`, `previous_live_set`, `get_setlists`, `get_live_set_view` (Story 9.8, read-only), `write_live_set` (Story 9.12: Velora Studio's Write to Velora sends the device's own command through this row, with `base_revision`; the engine refuses a stale one; no row was added), `set_assign_slot`, `clear_assign_slot` |
| config, `config.v1` (in process) | `get_config`, `set_config`, `subscribe_config` |
| library, `library.v1` | `library` (allow-listed, below) |
| **denied** (panel, pedals, encoders: hardware, not Studio's) | `set_led`, `move_fader`, `select_part`, `set_fader_mode`, `select_organ_part`, `set_encoder_mode`, `start_pedal_calibration`, `finish_pedal_calibration`, `cancel_pedal_calibration` |
| local | `hello`, `pair_request` (the only unpaired-allowed rows) |

Decisions: `set_performance_param` is an edit (Studio edits parameters live); the ASSIGN map belongs to the Live Set, so `set_assign_slot` / `clear_assign_slot` are allowed while
`set_encoder_mode` (a panel mode) is not. The engine's own control server also answers the two bridge-only bodies (`pair_request`, `library`) with `INVALID_COMMAND`.

**Library allow-list.** `LibraryRequest {payload}` carries a `LibraryClientMessage`; `BridgeServer::library_rules()` is a total table over its body too. Allowed (read / safe):
`subscribe`, `add_favorite`, `remove_favorite`, `get_favorites_and_recent`, `list_libraries`, `list_instruments`, `import_status`, `cancel_import`, `cancel_preflight`. Refused with
`INVALID_COMMAND` (`reason=library_message_not_available`) before the librarian sees them: a second `hello` (the bridge did the handshake), the engine-to-librarian reports
(`report_active`, `live_set_deleted`, `reconcile`, `set_playing`, `flush_recent`), `install_protected`, and everything that names absolute device paths (`import_request`,
`preflight_request`, and `list_preflight_rows`, which returns them). The import path policy (restricting sources to removable media) is deferred; until it exists the import is not
offered over the bridge. Story 9.11 changes the TABLE not at all (`list_libraries` and `list_instruments` were already allowed, `set_part_timbre` is an engine edit of `params.v1`): Velora Studio's timbre picker is built on those three. What changed is the reply: `LibraryInfo.source_path` (the absolute path a library was imported from, e.g. a removable drive) is cleared by the bridge in every `LibraryList` it relays (`test_library_forwarding`). Inner ids: `CancelPreflight.request_id` names one of the client's own requests and is translated to the upstream id of that request (none pending: 0).

**Forwarding.** Each paired session gets a connection of its own to the engine's control socket (so the engine keeps its per-client `Subscribe` state and its State pushes flow back
as they come) and, on first use, one to the librarian. The upstream sockets are connected non-blocking: an unreachable or backlogged one is `UPSTREAM_UNAVAILABLE`. `request_id` is
the client's: the bridge maps it to a fresh per-connection upstream id and back (two clients may use the same ids); the library message's inner `request_id` is mapped the same way
and the outer reply carries the client's id. A pending entry is dropped at its first reply (every allowed request has exactly one; pushes carry request_id 0); one that never gets a
reply is answered `UPSTREAM_UNAVAILABLE` (`reason=timeout`) after 30 s so the 256-entry cap cannot fill with dead requests. Pushes (`request_id` 0) are relayed untouched. The bridge never
interprets Live Set content. An upstream lost mid-session sends `UPSTREAM_UNAVAILABLE` (request_id 0) and closes the client connection, so the client reconnects and subscribes again.
A reply that does not fit one 65536-byte ws.v1 frame (library replies can be larger) is answered `INTERNAL` (`reason=reply_too_large`) when it answers a request; an oversize push is dropped and logged.

**Limits.** Frame cap 65536 (violation = `FRAME_TOO_LARGE` + close, counted), malformed protobuf = close, counted; at most 8 clients (more are closed at accept, counted) and at most 3
handshaken-but-unpaired sessions (`BUSY` at Hello); handshake timeout 5 s; idle timeout 120 s paired, 45 s unpaired (Studio keepalive every 20 s); at most 500 messages per second per
connection (`BUSY` above, close at 4x); at most 256 requests and 1 MiB queued toward one upstream; a slow client whose output queue passes 4 MiB is dropped (counted). A flood of
connections that never say Hello can hold the 8 slots for the 5 s handshake timeout each: acceptable on a USB link, noted.

**Not done here (recorded as deferred).** No TLS: the physical USB link is the trust boundary (architecture). The nftables single-port policy, the NCM interface and its link-local
addressing, the USB gadget, and the unit-side pairing lifecycle (showing the code on the display only, reopening the window, the paired computers list, Forget, a per-source
cooldown against burning the global attempts, rotating the code) are board work and Story 9.6's device section. Also deferred: the library import path policy (above), bidirectional /
control characters beyond the ASCII control range in computer names (names are for display only), and `ConfigService` holding its mutex across the config fsync (a slow disk can stall the
bridge thread for one `SetConfig`).

Tests: `sysd_bridge_tests` (every row of the matrix against fake upstream sockets, an injected clock, the shared config service, SHA-256 known vectors, the descriptor-totality test,
NOT_PAIRED for every table row) and `engine_host_bridge_smoke` (real `engine-host --no-audio` + `velora-sysd` + a TCP client). `engine_host_bridge_studio_smoke` runs the same smoke
plus the Velora Studio Rust client (`cargo test --offline real_bridge_round_trip`) against that real bridge and engine; it is skipped with a clear message (exit 77) when cargo is not
installed.
