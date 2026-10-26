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
`OUT_OF_RANGE`; pedal `min >= max` (0/0 means uncalibrated), a calibrated pedal whose `max - min` is under 256 counts (`details.reason = span_too_small`, `details.min_span`; Story 3.11), a bad Live Set id (empty, or 1..64 characters of `[A-Za-z0-9_-]`) or Boot into DEFAULT_LIVE_SET
without an id -> `INVALID_CONFIG` (new, 41; `details.field`, `details.reason`); an empty patch -> `INVALID_COMMAND` (`reason=empty_patch`); persist failures as above.

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

## Deployment and limits (read before relying on it)

- The socket is created **0600**: `engine` and `ui` must run as the sysd user, or the deployment must decide a group arrangement (not done here).
- The `revision` restarts at 0 whenever sysd serves defaults (no file, or an unusable one); clients treat the first config of a connection as new when it differs
  from what they hold, which covers it, but a revision is not a global clock.
- The N-1 stamp path in `ConfigStore::apply()` and `migrate()` both consult `slot_good`; they must stay consistent (one probe, one meaning).
- Config values received by the client are **not validated** on the engine side: sysd validates, but consumers (Stories 3.6, 3.11, 7.7, 8.9) must treat what they
  read defensively (unknown enum values, a calibration that is not min < max, a Live Set id that no longer exists).
- Deferred on purpose: the persist (fsync) runs on the server's poll thread; the Live Set id format stays `[A-Za-z0-9_-]{1,64}` until Story 8.9 tightens it to a
  UUID. (Story 3.11 fixed the minimum span of a pedal calibration: 256 counts, validated by sysd and again by the engine, which treats a narrower range as uncalibrated.)
- `velora-sysdctl` waits up to 10 s per request (`VELORA_SYSDCTL_TIMEOUT_MS` overrides).

## Not covered here

- Behaviour exists for clock source (7.7), velocity curve (3.6) and pedals (3.11); Boot into (8.9) is only logged by the engine wiring.
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
