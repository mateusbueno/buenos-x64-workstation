# proto

Role: the single owner of parameter ids and of the `ws.v1` wire schema (Story 2.1).

- `params.json` -- the parameter registry definition (JSON, parsed with the Python 3 standard library: no new dependency).
  Every parameter has a stable numeric `id` (1..16383, 14 bits: a UMP assignable controller carries it as bank + index),
  a `name`, a `class` (`structural` | `performance`, mandatory), a `type` (`int` | `uint` | `float` | `bool` | `enum`),
  a `scope` (`part` = one value per part (8), `master`, `insert` = per insert slot), `min`/`max`/`default`, a `unit` and an optional
  MIDI `cc` (performance + part scope only). Enum parameters name an entry of `enums`; their range is `0..count-1` and the default
  is a value name. Declared here only what Epic 2 needs: tonewheel organ (drawbars 1-9, percussion, vibrato/chorus, key click,
  leakage), rotary speed, inserts (type, bypass, 4 params), send levels, master EQ/compressor, per-part output, timbre reference.
- **Ids are never reused.** To retire a parameter delete it from `params` and add its id to `reserved`; the generator fails if a
  parameter ever takes a reserved id.
- `gen_params.py` -- deterministic generator (same input, same bytes). Emits `--core` (C++ header for `engine/core`, numeric tables,
  no names), `--host` (C++ header for `engine/host`, names and enum value names) and `--proto` (`ws_v1_params.proto`, the `ws.v1`
  `ParamId` enum and one enum per registry enum). It fails, naming the problem, on: duplicate id, duplicate name, missing or unknown
  class, reserved id reuse, ids out of 1..16383, bad type/scope, range/default inconsistency, unknown enum, duplicate or reserved CC.
  Nothing is written when validation fails.
- `params.cmake` -- `velora_params_output(core|host|proto <var>)` registers the generation as a build step of the calling target,
  so an invalid `params.json` fails the build. Generated files live in `build/generated/` and are not committed.
- `ws_v1.proto` -- the `ws.v1` schema (package `ws.v1`, `PascalCase` messages, `snake_case` fields; it only grows by addition,
  numbers are never reused). Framing: 4-byte big-endian length + protobuf, largest frame 65536 bytes. First message on every
  connection is `Hello` (version + features); errors are `{code, message, details}` with `code` from the closed `ErrorCode` enum.
- `tests/test_gen_params.py` -- ctest `params_generator_tests`: runs the generator on bad definitions and checks determinism.

## Parameter classes

| class | who owns the value | how it changes | path to RT |
|---|---|---|---|
| structural | the control thread (snapshot generation) | `ws.v1` structural commands | immutable `Snapshot`, one atomic pointer swap |
| performance | RT (last-value table) | `ws.v1` `SetPerformanceParam` or MIDI CC | one timestamped UMP |

Performance UMP mapping (`engine/core/include/velora/core/param_events.hpp`): MIDI 2.0 assignable controller (message type 4,
status 0x3), group 0, channel = part, bank = `id >> 7`, index = `id & 0x7F`, 32-bit value normalised over the parameter range.
MIDI CC numbers map through `cc` (drawbars 1-9 = CC 70-78, rotary speed 79, percussion 80, vibrato/chorus 81, key click 82, leakage 83, percussion harmonic/decay/volume 84/85/86 (value < 64 = first enum entry), vibrato/chorus type 87 (the 32-bit range is split evenly over v1..c3: index = round(value/127 * 5) for a 7-bit CC),
reverb send 91, delay send 94); the CC's channel is the part (0..7).

## Key contact events (`contact_k`, Story 2.5)

Not a registry parameter: the multi-contact keybed's per-note vendor controller, defined in `engine/core/include/velora/core/contact_events.hpp`
and documented in `engine/core/ORGAN.md`. MIDI 2.0 assignable **per-note** controller (message type 4, status 0x1), group 0:
`word0 = 4<<28 | 1<<20 | channel<<16 | note<<8 | k`, `word1 = 0xFFFFFFFF` (contact k closed) or `0` (open); a value of 0x80000000 and above reads as
closed. `k` is the contact index, 1..3 (1..2 on a two-contact keybed); the wire accepts 1..8, other per-note controller indices are not `contact_k` and are ignored; `channel` is the part, `note` 0..127. One event per contact
transition, timestamped like every other UMP; contact 1 closes first on the way down and opens last on the way up, and a Note On
follows the closures (Note On/Off-only sources simply send no contact events). This is the format the Epic 3 keybed reader emits; it
is distinct from the performance-parameter events above (status 0x3, not per-note).

## Toolchain

`protoc` + `libprotobuf` (Homebrew `protobuf` on the Mac; `protobuf-compiler` + `libprotobuf-dev` in the Linux image
`tools/ci/docker/Dockerfile`). protobuf is used only in `engine/host`, here and in tools; `engine/core` has no protobuf dependency.
