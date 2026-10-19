# mcu/common

Role: the portable MCU frame protocol shared by the keybed MCU, the panel MCU and the SoC (Story 3.1), and the single
source of the control, button and LED ID maps. Later siblings (`mcu/keybed/`, `mcu/panel/`, `mcu/bootloader/`) are other stories.

Dependency rule: portable C11 only. No OS, no heap, no libc allocation, no stdio, no floating point in the library; every
buffer is owned by the caller (the decoder keeps one fixed buffer inside its own struct). It must build for the MCU
targets and on the host with `-Wall -Wextra -Wpedantic -Wconversion` clean. `mcu/common` is not `engine/core` and may not
depend on anything; `engine/host` wraps it (`velora/host/mcu_frame.hpp`).

## Files

- `include/mcu/mcu_frame.h`, `src/mcu_frame.c` -- CRC-16/CCITT-FALSE (check `"123456789"` = `0x29B1`), encoder (returns the
  length or a typed negative `mcu_status`), typed payload pack/unpack for the 8 frame types, streaming decoder.
- `ids.json` -- the one source of every control, button and LED ID (name, kind, resolution bits, range). IDs are stable and
  never reused: retire one by adding it to `retired_ids` and removing it from the list.
- `gen_ids.py` -- stdlib generator: `mcu/mcu_ids.h` (C), `velora/host/mcu_ids.hpp` (C++ constexpr tables) and
  `MCU-ID-MAP.md`, written into the build dir (`build/generated/mcu`) at build time.
- `MCU-ID-MAP.md` -- the committed, versioned partner document (frame format, CRC, payloads, version policy, the three ID tables).
  An identical copy is published under `_bmad-output/planning-artifacts/architecture/architecture-buenos-x64-workstation-2026-10-17/`.

## Changing the ID map or the protocol

1. Edit `ids.json` (bump `document_version`; bump `protocol_version` and `MCU_PROTOCOL_VERSION` together for protocol changes;
   the C++ wrapper `static_assert`s they match).
2. `python3 mcu/common/gen_ids.py --write-published` rewrites both committed `MCU-ID-MAP.md` copies.
3. ctest `mcu_id_map_up_to_date` fails on duplicate IDs or names, missing fields, an ID reused after retirement, or a stale published copy.

## Version policy

The version byte is major (high nibble) and minor (low nibble). Current: `0x10` (1.0). A receiver at major N accepts
majors N and N-1 (any minor); major 0 is never valid, so at 1.x only major 1 is accepted. Everything else is rejected with
`MCU_EV_VERSION_REJECTED` and the `version_mismatches` counter. `MCU_PROTOCOL_VERSION` is the single constant to bump.

## Decoder notes

Byte-at-a-time: `mcu_decoder_feed()` returns the first event; after any event keep calling `mcu_decoder_poll()` until it
returns `MCU_EV_NONE` (or use `mcu_decoder_push()`, which does that). On noise, a bad length, a CRC mismatch or a truncated
frame it drops the false sync byte and rescans the bytes it already holds, so a real frame that began inside a false one is
still delivered; a false header claiming a long payload delays the following frames by at most `MCU_MAX_FRAME` (76) bytes
(about 250 us at 3 Mbaud); call `mcu_decoder_flush_stale()` after an idle gap to abandon such a stalled candidate and rescan.
Sequence accounting (every CRC-valid frame consumes a seq, rejected ones included; `delta = seq - (last+1)` mod 65536:
0 in order, above 0x8000 a duplicate or reorder counted in `seq_reorders` and not as a loss, otherwise a gap of `delta`
frames; a delivered HELLO resets the expectation with no gap). `mcu_decoder_init` replaces a current version with major 0
by the default. `bytes_in` is 64-bit; the other counters are 32-bit and wrap at 2^32 events. Typed `pack_*`/`unpack_*` check documented
ranges (see the header and `MCU-ID-MAP.md`) and never touch the frame's version, seq or tick.
Build: `-DVELORA_WERROR=OFF` disables `-Werror` for the library (default ON). Counters: see `mcu_decoder_counters`.

## Tests

`mcu_frame_c_test` (plain C11), `mcu_frame_test` (CRC, goldens for every type, round trips with boundary values, chunking,
noise, truncation, bad CRC, resync, seq gaps with wrap, version boundaries, length-vs-type), `mcu_fuzz_test`,
`mcu_id_map_up_to_date`, `mcu_id_map_selftest`. The fuzz test is labelled `slow;fuzz`, deterministic, and checks the decoder
against an independent reference parser. Its budget comes from `VELORA_FUZZ_BYTES` (default 50,000,000). The hour-equivalent
at 3 Mbaud is 3e6 x 3600 / 8 = 1.35e9 bytes:

    VELORA_FUZZ_BYTES=1350000000 ctest --test-dir build -R mcu_fuzz_test --output-on-failure
