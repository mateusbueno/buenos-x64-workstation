---
title: 'Story 7.3: Swing, latch and arpeggiator performance behaviour'
type: 'feature'
created: '2026-10-21'
status: 'done'
baseline_revision: '7b5405b6d4ee2c92eb683d97d63ac1c466cbdd18'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-7-context.md'
  - '{project-root}/engine/core/README.md'
  - '{project-root}/tools/offline-render/README.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Story 7.3 in `_bmad-output/planning-artifacts/epics.md` (`### Story 7.3`, ~line 4666): the arpeggiator from Story 7.2 does not yet shift odd steps by Swing %, keep patterns running with Latch On across chord replacements, hold notes in the pattern while the sustain pedal (CC 64) is down with Latch Off, or guarantee seamless continuation and untruncated gate completion across scene and Live Set switches.

**Approach:** extend `Arpeggiator` and `BlockRenderer` in `engine/core` with exact Swing % step-pair grid timing (`50%` straight up to `75%`), Latch On/Off state and chord-replacement rules, sustain-pedal (`CC 64`) pattern holding when Latch is Off (without deferring the arpeggiator's own step Note Offs in `VoiceArbiter`), and seamless scene/Live Set switching where already-sounding arpeggiated notes finish their gate on their original generation/engine while the pattern continues on the musical grid (or cleanly ends when the new Live Set has Arp Off). Expose optional `:SWING:LATCH` fields on `velora-offline-render --arp` and verify all four acceptance criteria with unit, offline-render, and RTSan tests.

## Boundaries & Constraints

**Always:**
- **Swing % (`50.0f..75.0f`, `50%` = straight):**
  - Every step pair consists of an even step (pair index `0`, aligned to an even multiple of `step_ticks`: `(grid_tick / step_ticks) % 2 == 0`) and an odd step (pair index `1`, `(grid_tick / step_ticks) % 2 != 0`).
  - Even steps fire on their exact unshifted grid tick `T_even`.
  - Odd steps (every second step of the rate) are delayed from `T_even` so they land at `T_swung = T_even + llround(2.0 * step_ticks * (swing_percent / 100.0))` ticks (or exact rational tick offset `2 * step_ticks * swing_percent / 100`, mapped to the exact frame via `TransportClock::frame_of_tick` — at `50%`, offset is `1.0 * step_ticks` = unshifted `T_even + step_ticks`; at `75%`, offset is `1.5 * step_ticks`, i.e. delayed by `0.5 * step_ticks` = half a step, which is `(swing_percent - 50%) / 100%` of the `2 * step_ticks` step pair). Compute the exact swung tick/units in integer/rational math on `TransportClock` and round to the nearest frame (`frame_of_tick` or rational unit offset) so every swung Note On lands on the exact computed frame.
  - Step duration (`step_frames`) used for `gate_percent` of an even step is the distance from the even step's frame to the swung odd step's frame, and for an odd step is the distance from the swung odd step's frame to the next even step's frame (minimum 1 frame), so Gate `< 100%` scales naturally with the swung step length and never inadvertently overlaps the delayed/shortened step unless Gate `>= 100%`.
- **Latch On (`ArpConfig::latch == true`):**
  - When the player releases keys (`NoteOff`), latched notes remain in the arpeggiator's pattern; when *all* physical keys are released (`physically_held_count == 0`), the pattern keeps running with the last held notes.
  - Once all physical keys have been released (all held notes are latched-only, not physically held), the first new `NoteOn` (or new chord played at the same frame) **replaces** all latched notes with the new chord while keeping the pattern running continuously on the musical grid (resetting `step_index_ = 0` for the new chord, or continuing on the grid without a gap or double trigger). While at least one key is still physically held, pressing additional keys adds to the chord.
  - Switching **Latch Off** (`latch: true -> false` via `set_config` / `Snapshot`) while no keys are physically held (or for any notes only held by latch) ends the latched pattern / removes unheld latched notes at the **next step** on the grid (specifically: the currently sounding step note finishes its normal gate length, and at the next grid step boundary where no keys are physically held, no new step is triggered and the latched notes are cleared; if some keys are still physically held, only the non-physically-held latched notes are dropped from the pool at the next step).
- **Sustain pedal (`CC 64`) with Latch Off:**
  - When the sustain pedal is down (`CC 64 >= 64`, `0x80000000` in 32-bit UMP) on a part with Arp On and Latch Off, releasing keys (`NoteOff`) keeps those sustained notes in the arpeggiator's pattern until the sustain pedal is released (`CC 64 < 64`).
  - While Arp is On on a part, `VoiceArbiter::set_sustain(part, ...)` must **not** hold the arpeggiator's own generated step `NoteOff` events (otherwise every arpeggiator step would accumulate sustained voices in the engine instead of playing rhythmic gated steps!). Instead, the arpeggiator itself consumes the part's sustain-pedal state to hold the input pattern notes until pedal-up (and if Latch is On when the pedal is released, the notes stay latched). When the pedal is released with Latch Off, any notes not physically held are removed from the arpeggiator (and if no notes remain held, active sounding step notes end and the pattern stops). Document and test this behavior.
- **Scene and Live Set switching while an arpeggiator is running:**
  - When a scene recall or seamless Live Set switch happens while an arpeggiator is running on a part:
    - Any arpeggiated step notes already sounding at the switch boundary **finish their gate length on their original generation/engine** (never truncated or cut at the switch block boundary; their scheduled `off_frame` still emits `NoteOff` to the `note_engine_` that started them, holding the old generation reference via `bind_generation` until the voice finishes).
    - If the part continues with Arp On in the new scene/Live Set (including across a Live Set switch when keys remain held or latched), the arpeggiator pattern **continues on the musical grid without a gap or double trigger** (`leave_live_set` is only called when leaving Live Set mode altogether for plain mode, NOT on a Live Set -> Live Set switch; on a Live Set -> Live Set switch where the player holds keys across the switch, preserve the held keys and running grid phase so the pattern continues seamlessly on the new generation's instrument).
    - Even if the new scene or Live Set has Arp Off on that part (or changes the part's instrument), any arpeggiated note already sounding at the switch block finishes its gate length on its original generation rather than being cut early at frame 0 of the new generation.
- **Offline renderer (`tools/offline-render`):**
  - Extend `--arp` in `velora-offline-render` to accept `P:MODE:RATE:OCTAVES:GATE[:SWING[:LATCH]]` (`SWING` `50..75`, default `50`; `LATCH` `on|off`, default `off`), keeping backward compatibility with the 5-field Story 7.2 syntax.

**Never:**
- Do not add `proto/params.json` registry IDs or `live_set.proto` serialization yet (that is Story 7.4).
- Do not break any Story 7.1 or Story 7.2 tests or committed goldens.
- Do not allocate or lock on the RT thread.

</intent-contract>

## Code Map

- `engine/core/include/velora/core/arpeggiator.hpp`, `engine/core/src/arpeggiator.cpp` -- add Swing % step-pair tick/frame calculation, Latch state (`physically_held`, `latched`, `latch_off_pending_` to end latched notes at the next step), sustain-pedal state (`set_sustain(bool down)` or `CC 64` UMP handling), and non-truncating transition handling so already-sounding step notes finish their `off_frame` gate on generation/scene/Live-Set changes
- `engine/core/include/velora/core/block_renderer.hpp`, `engine/core/src/block_renderer.cpp` -- route sustain pedal (`CC 64`) to `arps_[p]` while preventing `VoiceArbiter` from deferring arpeggiator-generated step Note Offs; preserve held/running arp state and in-flight step gate completions across scene recalls and seamless Live Set switches (`adopt_generation`)
- `engine/core/tests/arpeggiator_test.cpp`, `engine/core/tests/rtsan_arpeggiator_test.cpp` -- tests for Swing % (50%, 62.5%, 75%) exact frame positions, Latch On (release all keys, new chord replacement, Latch Off ending at next step), Sustain pedal with Latch Off, and seamless scene / Live Set switch continuation without note truncation, gap, or double trigger
- `tools/offline-render/src/render_main.cpp`, `tools/offline-render/src/render.hpp`, `tools/offline-render/src/render.cpp`, `tools/offline-render/tests/arp_test.cpp`, `tools/offline-render/CMakeLists.txt` -- extend `--arp` with optional `:SWING[:LATCH]`, add offline seamless-switching and swing/latch/sustain verification in `arp_test.cpp` and CLI error tests in `CMakeLists.txt`
- `engine/core/README.md`, `tools/offline-render/README.md`, `_bmad-output/implementation-artifacts/sprint-status.yaml` -- document Swing, Latch, sustain pedal, and seamless switching behaviour; mark `7-3` done

## Tasks & Acceptance

**Execution:**
- `engine/core/include/velora/core/arpeggiator.hpp` and `engine/core/src/arpeggiator.cpp` -- implement Swing % (`50..75%`) odd-step delay and step-length gate scaling, Latch On/Off (keep running on release, new chord after full release replaces latched notes, Latch Off ends latched notes at next step), sustain pedal (`CC 64`) holding with Latch Off, and non-truncating step gate completion across transitions -- core Story 7.3 arpeggiator performance rules
- `engine/core/include/velora/core/block_renderer.hpp` and `engine/core/src/block_renderer.cpp` -- wire sustain pedal (`CC 64`) to `arps_[p]` without deferring arp-generated step `NoteOff`s in `VoiceArbiter`, and preserve running arp grid continuity and in-flight sounding step gates across scene recalls and seamless Live Set switches -- glitch-free stage performance
- `engine/core/tests/arpeggiator_test.cpp` and `engine/core/tests/rtsan_arpeggiator_test.cpp` -- unit and renderer tests for Swing %, Latch On/Off, sustain pedal with Latch Off, and scene/Live Set seamless switching (verifying old-generation voice completion for full gate length and gap-free/double-trigger-free grid continuation) -- verify all core ACs
- `tools/offline-render/src/render_main.cpp`, `tools/offline-render/src/render.{hpp,cpp}`, `tools/offline-render/tests/arp_test.cpp`, `tools/offline-render/CMakeLists.txt` -- support optional `:SWING[:LATCH]` on `--arp`, add offline render tests for swing, latch, sustain, and seamless switching without truncation -- verify offline render ACs
- `engine/core/README.md`, `tools/offline-render/README.md`, `_bmad-output/implementation-artifacts/sprint-status.yaml` -- document sustain/latch/swing/switching rules and update sprint status `7-3` to `done`

**Acceptance Criteria:**
- Given Swing % (50% = straight, up to 75%), when the arpeggiator runs, then every second step of the rate is delayed by the swing amount of the step pair, exactly on the computed frame.
- Given Latch On, when the player releases all keys, then the pattern keeps running with the last held notes; a new chord played after all keys were released replaces the latched notes; Latch Off ends latched notes at the next step.
- Given the sustain pedal down with Latch Off, when keys are released, then the arpeggiator keeps the sustained notes in its pattern until the pedal is released (behaviour documented and tested).
- Given an arpeggiator running on a part, when a scene or Live Set switch happens, then notes already sounding finish on their original generation with their gate length, the pattern continues on the musical grid without a gap or double trigger, and the seamless-switching render test shows no truncation.

## Verification

**Commands:**
- `cmake -S . -B build && cmake --build build -j8 && ctest --test-dir build --output-on-failure` -- expected: all pass
- `python3 tools/ci/check_core_deps.py` -- expected: pass

## Review Triage Log

- **Live Set sustain pedal (`CC 64`) test coverage:** Patched. Added a test case in `engine/core/tests/arpeggiator_test.cpp` where `live_set = true` and `CC 64` is sent to verify that `arps_[p]` holds notes on listening parts (`router_.pedal_down_target(p)`) without `VoiceArbiter` deferring arp step note-offs.
- **First grid tick with Swing off-zero:** Patched. Added an assertion in `test_swing_timing_and_gate_scaling` starting at a frame `f > 0` falling between an unswung odd tick and its swung position (at frame 7000 at 120 BPM with 75% swing) confirming it triggers at the swung tick (9000).
- **Sustain state sync when `!any_proc`:** Patched. In `BlockRenderer::apply()` and `apply_keyboard_sustain()`, updated `arps_[ch].set_sustain_pedal(down)` so that enabling Arp while pedal is down holds notes.
- **Sustain pedal in `tools/offline-render/src/render.cpp`:** Patched. In `control` lambda, verified that `CC 64` is passed if `opt.routing.arp[e.channel].enabled` is true OR if any entry in `opt.routing.arp_switches` enables arp on that channel.

## Auto Run Result

- **Summary:** Implemented Swing % (`50.0%..75.0%`) exact step-pair grid timing and step-duration gate scaling, Latch On (pattern continues across physical key release, new chord replaces latched notes, Latch Off drops latched notes at the next step), sustain pedal (`CC 64`) holding of arpeggiated patterns when Latch is Off without VoiceArbiter deferral of arp-generated step Note Offs, and seamless scene / Live Set switching where already-sounding notes complete their natural gate length without truncation or double triggers. Extended `--arp` in `tools/offline-render` with optional `:SWING[:LATCH]`.
- **Files changed:**
  - `engine/core/include/velora/core/arpeggiator.hpp`, `engine/core/src/arpeggiator.cpp` — added Swing % calculations, Latch tracking and chord replacement, sustain pedal state, and non-truncating transition handling.
  - `engine/core/include/velora/core/block_renderer.hpp`, `engine/core/src/block_renderer.cpp` — routed sustain pedal without VoiceArbiter deferral of arp steps; preserved running arp grid continuity and sounding step gates across scene recalls and Live Set switches.
  - `engine/core/tests/arpeggiator_test.cpp`, `engine/core/tests/rtsan_arpeggiator_test.cpp` — unit and RTSan traffic tests for swing timing, latch, sustain pedal, and seamless switching.
  - `tools/offline-render/src/render_main.cpp`, `tools/offline-render/src/render.{hpp,cpp}`, `tools/offline-render/tests/arp_test.cpp`, `tools/offline-render/CMakeLists.txt` — extended `--arp` options and added offline render tests for swing, latch, sustain, and seamless switching.
  - `engine/core/README.md`, `tools/offline-render/README.md`, `_bmad-output/implementation-artifacts/sprint-status.yaml` — updated documentation and sprint status.
- **Review findings breakdown:** 4 patches applied (4 low/medium), 0 deferred.
- **Follow-up review recommendation:** `false`.
- **Verification:** `cmake --build build -j8`, `ctest --test-dir build -R "(arpeggiator|arp_|transport_clock)" --output-on-failure` (38/38 passed), `python3 tools/ci/check_core_deps.py` passed.
