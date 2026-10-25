---
title: 'Story 3.10: Panel reader on the SoC and the Mac virtual panel'
type: 'feature'
created: '2026-10-25'
status: 'done'
baseline_revision: '9e6b23e3c0585982f5e7cf6ed7c141bd4884b0f9'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/mcu/common/MCU-ID-MAP.md']
warnings: []
deferred:
  - summary: >-
      No SoC input thread / UART wiring for the panel reader; on engine-host only the MIDI control-surface path is live and VirtualPanel::on_key (keyboard keys) has no live key source.
    evidence: |-
      Reader and key API are tested; wiring belongs to the board bring-up and a ui/ws key-injection path.
    severity: medium
  - summary: >-
      ui WsClient panel_event listener (decode, device_ns/1000) is untested and nothing registers a listener yet; relative-encoder encodings other than two's complement are unsupported in the mapping file.
    evidence: |-
      Close when the ui story consumes PanelEvent.
    severity: low
---

<intent-contract>

## Intent

**Problem:** Panel frames (wheels, pedals, faders, encoders, buttons) from the panel MCU never reach the engine or the UI: there is no panel reader on the SoC, and no way to develop Epic 4 panel behaviour on the Mac without hardware (FR5, FR43, AR16, AR10, AR41).

**Approach:** Add a panel reader core in `engine/host` (input-edge-thread logic, no hardware I/O) that reuses the keybed reader's tick→device-time drift tracking (Story 3.5), turns CONTROL frames into UMP for the RT plane and BUTTON frames into `PanelEvent` messages pushed to the UI over ws.v1; plus a Mac virtual panel that turns a mapping file (keyboard keys and a USB MIDI control surface's faders/knobs/buttons) into the same MCU frames fed to the same reader.

## Boundaries & Constraints

**Always:** Reuse the existing frame library (`mcu_frame.hpp`, ids in `mcu_ids.hpp`/ids.json) and the `DriftClock`/TIME_SYNC logic of `keybed_reader` (factor shared code rather than copying). UMP reaches the RT plane only through the existing host event queue (`StampedUmp` with host_ns). Pitch wheel → MIDI 2.0 pitch bend and mod wheel → modulation (CC1), sent to EVERY part whose keyboard is on, at their in-block offset (the reader learns the keyboard-on channel set through an injected, thread-safe provider the control thread updates). Fader and encoder CONTROL frames are published to the RT plane as raw panel control events carrying control ID and value or delta (reuse the panel-control UMP encoding already in `engine/core/include/velora/core/ump.hpp`/block_renderer if one exists; otherwise add the smallest additive encoding there); binding to drawbars/volumes/parameters stays as in Epic 4. Pedal CONTROL frames are published as raw panel control events (sustain/expression/switch semantics are Story 3.11). BUTTON frames become a `PanelEvent` (button ID, pressed/released, device time ns) delivered to subscribed ui clients via an additive ws.v1 server push (new `ServerMessage` body number, schema grows by addition only) and are never relayed through sysd. Frame errors/seq gaps are counted; processing continues. Virtual panel frames carry device-time ticks derived from an injected clock so tests are deterministic; the mapping file is data (JSON): key name or MIDI CC/note → control/button ID plus scaling.

**Never:** No UART/SPI/real hardware I/O, no pedal calibration/polarity (3.11), no LED output (3.12), no binding of faders/encoders (Epic 4), no change to the MCU frame protocol, no sysd involvement.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Pitch wheel | CONTROL frame for pitch wheel | pitch bend UMP at device time to every keyboard-on part channel | none |
| Mod wheel | CONTROL frame for mod wheel | CC1 modulation UMP to every keyboard-on part | none |
| No keyboard-on parts | wheel moves | no UMP emitted | none |
| Fader / encoder | CONTROL frame | raw panel control UMP with ID and value/delta | unknown ID dropped and counted |
| Pedal | CONTROL frame | raw panel control event with ID/value | none |
| Button | BUTTON frame pressed/released | one PanelEvent (id, pressed, device ns) pushed to ws.v1 subscribers; nothing to RT | none |
| Bad frame | CRC error / seq gap | counted, reader continues | counted |
| Time conversion | drifting simulated panel MCU | converted times follow Story 3.5 accuracy, never reordered | none |
| Virtual panel key | mapped keyboard key / MIDI CC / note | same frames as the MCU would send, reader output identical | unmapped input ignored |
| Mapping file | malformed JSON or unknown control id | rejected with an error naming the entry; no partial apply | error |

</intent-contract>

## Code Map

- `engine/host/include/velora/host/keybed_reader.hpp`, `src/keybed_reader.cpp` -- DriftClock/TIME_SYNC logic to share (factor into a common header/source, keybed tests must keep passing).
- `engine/host/include/velora/host/mcu_frame.hpp`, `mcu_ids.hpp`, `mcu/common/MCU-ID-MAP.md`, `ids.json` -- Control/Button/TimeSync/Hello frames and control/button ID maps (wheels, pedals, faders, encoders, buttons).
- `engine/core/include/velora/core/ump.hpp`, `src/block_renderer.cpp`, `src/arpeggiator.cpp` -- existing UMP helpers and existing panel-control handling (kControl hits); pitch bend/CC1 helpers.
- `engine/host/include/velora/host/midi_ingest.hpp`, `render_driver.hpp`, `src/fader_bindings.cpp` -- StampedUmp/host queue and how 4.12/4.13 consume panel controls today.
- `proto/ws_v1.proto`, `engine/host/src/control_server.cpp`, `control_plane.cpp` -- add PanelEvent push and subscriber broadcast (pattern: other pushed messages with request_id 0); `ui/include/velora/ui/panel_event.hpp`, `ui/src/ws_client.cpp` -- UI-side PanelEvent shape to match (optionally deliver pushed PanelEvent into the existing listener path).
- `engine/host/macos/coremidi_input.cpp`, `main.cpp` -- MIDI control surface input and optional `--virtual-panel <map.json>` wiring; `ui/src/virtual_panel.cpp` -- existing keycode→PanelEvent map to mirror in the mapping file.
- New: `engine/host/include/velora/host/panel_reader.hpp`, `src/panel_reader.cpp`; `virtual_panel_map.hpp`, `src/virtual_panel_map.cpp` (+ default `engine/host/dev/virtual-panel.json`); `tests/panel_reader_test.cpp`, `tests/virtual_panel_map_test.cpp`.

## Tasks & Acceptance

**Execution:**
- shared drift clock -- factor DriftClock out of keybed_reader for reuse -- AR16 for both links
- `engine/host/{include,src}/panel_reader.*` -- frame ingest, wheel→pitch bend/mod UMP to keyboard-on parts, raw panel control events, BUTTON→PanelEvent sink, counters
- `proto/ws_v1.proto`, `control_server.cpp`/`control_plane.cpp` -- additive PanelEvent push to subscribers
- `engine/host/{include,src}/virtual_panel_map.*`, `engine/host/dev/virtual-panel.json`, macOS wiring -- mapping file → MCU frames into the reader (keyboard keys via an API entry point; MIDI control surface via CoreMIDI)
- tests -- every matrix row; render test via `BlockRenderer` showing pitch bend and modulation reaching ALL keyboard-on parts; PanelEvent push over the UDS (control test pattern); virtual panel end-to-end (mapped input → frames → reader output identical to hand-built frames)

**Acceptance Criteria:**
- Given wheel CONTROL frames, then they become pitch bend / modulation UMP at device time for every keyboard-on part, and a render test shows both reaching all of them.
- Given fader/encoder CONTROL frames, then raw panel control events with ID and value/delta reach the RT plane.
- Given BUTTON frames, then each is delivered to ui as a PanelEvent (id, pressed/released, device time) and never through sysd.
- Given the Mac virtual panel with a mapping file, then keyboard keys and a MIDI control surface produce the same frames and the same downstream behaviour as hardware.

## Spec Change Log

## Review Triage Log

### 2026-10-25 — Review pass
- verdicts: 4 layers, ~45 findings — patches 8 groups, deferred 2, rest rejected
- findings:
  - `[high]` `patch` shutdown use-after-free (panel_push vs control destruction); wake-fd race in push_panel_event — fixed
  - `[medium]` `patch` button release could be dropped/miscounted; wheels/pedals shed near full queue; stale pitch bend on mask change / Live Set switch / MCU restart; unknown button ids; sink step clamp — fixed + tests
  - `[medium]` `patch` untested Live Set pitch-bend routing, update_from, FM/VA bend scaling — tests added
  - `[medium]` `defer` SoC thread/UART and key source not wired; WsClient listener untested
  - `[low]` `reject` virtual panel consuming mapped MIDI from real devices (opt-in flag), set_map held buttons, far-future tick clamp, magic-number nits, default key map ergonomics

## Verification

**Commands:**
- `cmake --build build -j8 && ctest --test-dir build -R "panel|keybed|mcu|control|fader|rt_traffic" --output-on-failure` -- expected: all pass
- `python3 tools/ci/check_core_deps.py` -- expected: exit 0

## Auto Run Result

Summary: PanelReader (wheels→pitch bend/CC1 to keyboard-on parts, raw panel controls, BUTTON→PanelEvent), shared DriftClock/TimeSyncLink, ws.v1 PanelEvent push, virtual panel mapping file + MCU frame generator + CoreMIDI sink, FM/VA pitch bend.
Verification: full ctest passes except 4 pre-existing failures (tokens_verify, protected_keyless_test, sysex_receiver_test, library_resolver_test); core deps check passes.
Residual risks: no SoC link wiring; RTSan not run; keyboard-key source not wired in engine-host.
