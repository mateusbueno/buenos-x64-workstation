---
title: 'Story 3.11: Pedals with half-pedal calibration and expression (Mac math, sysd config and render test)'
type: 'feature'
created: '2026-10-26'
status: 'done'
baseline_revision: 'ed587ef6b912489d157fc4efeb7c0a6ac4b267c4'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/sysd/README.md', '{project-root}/engine/host/include/velora/host/panel_reader.hpp', '{project-root}/mcu/common/MCU-ID-MAP.md']
warnings: []
deferred:
  - summary: >-
      AC wording vs design: "half-pedal damping at mid travel" holds only below half travel — the arbiter's CC64 hold threshold stays at half (MIDI convention), so at exactly 50% the note switches from maximum damping to held (documented design limitation; revisit with a real pedal). Real pedals on the prototype (ADC range, noise, NO/NC wiring) not measured.
    evidence: |-
      engine/host/README.md; render tests at CC 32/40/56/63.
    severity: high
  - summary: >-
      Follow-ups: async Finish with a WRITING result (set_config blocks the control thread up to ~400 ms), PanelReader::poll() has no dedicated timer in engine-host (the trailing pedal value waits for the next packet/frame; set_tick wiring untested), switch polarity only through --switch-polarity (no sysd field), single-spike outlier filter for calibration, Settings UI (10.2), boot polarity detection (3.8).
    evidence: |-
      Documented in the READMEs.
    severity: medium
---

<intent-contract>

## Intent

**Problem:** Pedal CONTROL frames from the panel MCU (sustain, expression, switch) reach the RT plane only as raw panel control events (Story 3.10): pedals are not normalised for polarity, not calibrated, and sustain/expression/switch semantics do not exist yet (FR42, AR39 partial).

**Approach:** In the host panel path add a pedal processor: (1) polarity: sustain, expression and switch report the same direction (pressed = higher) for any supported pedal — the polarity setting (`PedalPolarity` in sysd config, per pedal; detection at boot is Story 3.8 and does not exist, so polarity comes from the config and an `AUTO`/unspecified value means the raw direction); NO and NC switch pedals and two continuous sustain pedals of opposite direction are normalised; (2) calibration: a command-driven routine (CLI for now; the Settings row is Story 10.2) where the player presses the sustain pedal fully and releases it; the measured min/max (12-bit raw 0..4095) are stored in the sysd config (`PedalCalibration`), and the pedal maps over its calibrated range to the full continuous sustain range 0..max so mid travel gives mid value; same routine for expression; (3) sustain output: a MIDI 2.0 CC64 (32-bit value, continuous) UMP in device time to every keyboard-on part, and the sample engine plays half-pedal damping at mid travel (investigate what `SampleEngine` / the voice arbiter do with CC64 today: binary threshold? add a continuous half-pedal damping behaviour with a documented law — e.g. release rate scaled by (1 - pedal) between the pedal-down threshold and fully up — minimally and additively, default behaviour for binary CC64 unchanged); (4) expression: continuous CC11 UMP in device time to keyboard-on parts; (5) switch: a published pedal event (the existing raw panel control event plus the ws.v1 PanelEvent push if applicable) that later functions (footswitch actions, Story 4.36) consume — normalised for polarity so pressed is always "closed".

## Boundaries & Constraints

**Always:** Reuse the Story 3.10 panel reader (`PanelReader`, `KeyboardOnParts`, `make_panel_control_event`, reserved raw-control path), the sysd config foundation (PedalCalibration/PedalPatch, SysdConfigClient callback, `velora-sysdctl`), existing UMP helpers (make CC UMP; check ump.hpp for MIDI 2.0 control change helpers used by wheels CC1). The calibration routine is a command: add additive ws.v1 messages (`StartPedalCalibration{pedal}`, `CancelPedalCalibration`, result via a reply/State field) handled by the engine's control plane (control thread) which collects raw min/max while the session runs (RT/panel-reader publishes raw values to the control thread through a lock-free last-value, no locks on RT), and on finish writes the result to sysd with a SysdConfigClient write path (add `set`/patch capability to the client: its own thread, retry-free, typed result) — OR implement the routine inside the `velora-sysdctl` tool by watching raw values over a ws.v1 stream; choose the simpler design that keeps raw measurement on the engine, document it. Calibration validation: min < max with a minimum span (reject a degenerate span below 256 counts, noted as deferred item of the sysd review; update sysd validation accordingly), uncalibrated (0/0) pedals use the full 0..4095 range. Mapping is clamped, monotonic, and has a small deadband/hysteresis at both ends (document constants) so a resting pedal does not jitter and the fully-up pedal reads exactly 0 and fully-down exactly max (test). The output rate is limited: only emit a CC when the quantised 7-bit-equivalent... NO: emit on change of the 32-bit mapped value but throttle to at most one UMP per pedal per audio block (coalesce to the latest), since the queue has limited slots. Keep pedal events in the note headroom lane (never shed with controllers) as in 3.10. All pedal logic on the input thread/control side, never allocating on RT. Existing 3.10/4.12/4.13/4.36 tests stay green.

**Never:** No Settings UI row (10.2), no boot-time polarity detection (3.8), no footswitch function dispatch (4.36 exists in ui: do not change it), no hardware measurement, no changes to the MCU protocol, no change to the sysd schema except adding validation (fields exist).

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Normally-open vs normally-closed switch | polarity config NO / NC | pressed always reports closed (same direction) | none |
| Two continuous sustain pedals, opposite direction | polarity NORMAL / INVERTED | same direction after normalisation, same mapped range | none |
| Calibration run | press fully, release, finish | min/max stored in sysd (revision+1), mapping uses them | too small span → typed refusal, config unchanged |
| Mid travel | raw halfway between min and max | CC64 value ≈ half range; sample render shows half-pedal damping (release slower than up, faster than down) | none |
| Fully up / down | raw ≤ min / ≥ max (+deadband) | exactly 0 / max | none |
| Uncalibrated | min=max=0 | full 0..4095 range | none |
| Expression | raw moves | CC11 UMP to keyboard-on parts in device time | no keyboard-on parts → nothing emitted, counted |
| Switch | closed/open | published pedal event, normalised | none |
| Calibration cancelled / timeout | cancel or no movement | config unchanged | typed result |
| Config change at runtime | sysd pushes new calibration/polarity | applied from the next frame without restart | none |
| Queue pressure | near-full queue | pedal events not shed, coalesced to latest per block | none |
| sysd absent | no config | uncalibrated, raw direction | none |

## Code Map

- `engine/host/include/velora/host/panel_reader.hpp`, `src/panel_reader.cpp` (pedal controls at ~line 289, wheel path emits CC/pitch bend to keyboard-on parts, KeyboardOnParts), `mcu_ids.hpp` / MCU-ID-MAP.md (PEDAL_SUSTAIN 0x28, PEDAL_EXPRESSION 0x29, PEDAL_SWITCH 0x2A), `virtual_panel_map.*` (virtual panel can generate pedal frames).
- `engine/core/include/velora/core/ump.hpp`, `src/block_renderer.cpp` (CC routing: sustain pedal handling in the router/arbiter/arpeggiator (pedal_down_target, sustain CC64 handling in arpeggiator and voice arbiter), keyboard_pitch_bend_ replay logic as a pattern), `sample_engine.hpp/.cpp` (release rates), `voice_arbiter.*` (pedal-sustained voice state kSustained).
- `engine/host/include/velora/host/sysd_config_client.hpp` + src (subscriber only today), `engine/host/macos/main.cpp` (callback wiring pattern: apply_clock_source / apply_velocity_curve), `sysd/src/config_store.cpp` (check_pedal validation), `proto/ws_v1.proto` (PedalCalibration, PedalPolarity, Config), `tools/sysdctl/*`.
- `engine/host/src/control_plane.cpp` / `control_server.cpp` (add calibration commands), `ui/src/navigation_controller.cpp` (footswitch consumer MCU_CTRL_PEDAL_SWITCH) -- read only.
- Tests: `engine/host/tests/panel_reader_test.cpp`, `velocity_curve_test.cpp` (pattern for sysd-driven tests), `engine/core/tests/` (sample/voice arbiter), `tools/offline-render/tests` (render tests), `sysd/tests/*`.

## Tasks & Acceptance

**Execution:**
- pedal processor (polarity, calibration mapping, deadband, coalescing) in the host panel path with sysd config application (thread-safe, no restart)
- sustain CC64 / expression CC11 emission to keyboard-on parts, switch event; verify BlockRenderer routes CC64 continuous values (arbiter/arpeggiator unaffected for binary behaviour)
- sample engine/arbiter: continuous half-pedal damping (additive, documented law), render test proving mid-travel damping is between up and down
- calibration routine: ws.v1 commands + engine collection + SysdConfigClient write + CLI in velora-sysdctl (`calibrate-pedal sustain|expression` with prompts and result); sysd validation min span
- tests: all matrix rows incl. opposite-direction sustain pedals, NO/NC switches, calibration end to end with a real sysd in a temp dir and the virtual panel generating frames, persistence across restart, near-full queue
- docs: engine/host/README.md, sysd/README.md, proto/README.md

**Acceptance Criteria:**
- Given any supported pedal polarity, then sustain, expression and switch report the same direction (tests with two continuous sustain pedals and a normally-open and a normally-closed switch).
- Given the calibration routine run by command, then measured min/max are stored in the sysd config and the sustain pedal maps to the full continuous range, so a sample instrument plays half-pedal damping at mid travel (render test).
- Given the expression pedal, then it sends a continuous expression controller UMP to keyboard-on parts in device time.
- Given the switch pedal, then it is published as a pedal event that later functions consume.
- Real pedals on the prototype are recorded as deferred.

## Spec Change Log

## Review Triage Log

### 2026-10-26 — Review pass (single focused reviewer)
- findings:
  - `[high]` `patch` every normal pedal lift lengthened the release of all held notes (CC delivered before the deferred release) — reordered + render test; a legacy sysd file with a narrow pedal span voided the whole config — normalised on load; trailing coalesced pedal value could stay unsent (stuck sustain) — flushed on next frame/poll; lost pedal-up when the last keyboard-on part leaves
  - `[medium]` `patch` hold threshold literals duplicated (single constant now), probe race (packed atomic), run token for calibration sessions, control-thread block timeout lowered, README overstatements, dead branch/unused code
  - `[high]` `decision` A1: kept the arbiter hold threshold at half; documented that damping applies below half travel

## Verification

**Commands:**
- `cmake --build build -j8 && ctest --test-dir build -R "panel|pedal|sysd|sample|arpeggiator|arbiter|control|rt_traffic|offline" --output-on-failure` -- expected: all pass
- `python3 tools/ci/check_core_deps.py` -- expected: exit 0

## Auto Run Result

Summary: PedalProcessor (polarity, calibration mapping, deadband, per-block coalescing) in the panel path, CC64/CC11 to keyboard-on parts, switch event, continuous half-pedal damping in the sample engine, calibration commands (ws.v1 Start/Finish/Cancel with run token) with measurement on the engine and write to sysd (SysdConfigClient::set_config), velora-sysdctl calibrate-pedal, sysd span validation.
Verification: targeted tests (84) pass; full ctest passes except the 4 pre-existing failures; core deps and UI strings lint pass.
