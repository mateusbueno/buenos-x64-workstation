---
title: 'Story 7.7: Clock source setting and DIN/USB clock inputs (Mac logic)'
type: 'feature'
created: '2026-10-26'
status: 'done'
baseline_revision: '5f13c800d8339daa973a00e81212d2e9859d6181'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/sysd/README.md', '{project-root}/engine/core/include/velora/core/clock_follower.hpp']
warnings: []
deferred:
  - summary: >-
      Not done: 10-minute by-ear arp check on hardware, DIN UART driver (tagging hook documented), automated test of CoreMIDI source-name matching (--din-source), smoke test only checks log lines (behaviour covered by clock_source_tests); ShellView::set_clock_lost has no app caller (7.7/7.8 Top Bar follows PerformanceView tempo label).
    evidence: |-
      engine/host/README.md, sysd/README.md.
    severity: medium
  - summary: >-
      Events already in the renderer's pending_ for the block being built can survive a source switch (at most one block); a request while sysd is absent fails immediately with a notice instead of being held.
    evidence: |-
      Documented.
    severity: low
---

<intent-contract>

## Intent

**Problem:** The RT clock follower (Story 7.6) and `BlockRenderer::set_clock_source` exist, but nothing connects the player's Clock source choice (Internal / DIN / USB) to the engine, and incoming MIDI clock is not filtered by port: with DIN selected, clock on USB must be ignored and vice versa (FR76, AR39 partial, AR17, UX-DR65 partial).

**Approach:** (1) The engine consumes the sysd config (`SysdConfigClient` callback from the foundation) and applies `clock_source` to the renderer without interrupting sound (applied at the next block through the existing atomic setter; no voices touched). (2) MIDI ingest tags every incoming UMP with its input port kind (`kUsb` for CoreMIDI/USB MIDI sources, `kDin` for DIN sources) when stamping it on arrival in device time, and forwards only the selected port's realtime clock messages (0xF8/0xFA/0xFB/0xFC) to the follower; clock from the other port is dropped (counted); non-clock events from both ports pass as before. On the Mac the source kind comes from the CoreMIDI source (USB or virtual = USB; a `--din-source NAME` option or mapping marks one source as DIN so DIN can be exercised on the Mac), the board's DIN UART adapter is not implemented here (document the tagging hook). (3) The Settings › MIDI & Clock › Clock source Segmented Control (Internal / DIN / USB; the row/section exist in `ui/src/screens/settings_view.cpp`) writes the value through a `SysdConfigSender` seam to sysd (`SetConfig` patch with only clock_source), and shows the value received from sysd; a change takes effect without interrupting sound.

## Boundaries & Constraints

**Always:** Reuse: the sysd foundation (`sysd/`, `engine/host/include/velora/host/sysd_config_client.hpp`, `proto/ws_v1.proto` ClockSource/ConfigPatch, `tools/sysdctl` client library), `core::ClockSource`, `BlockRenderer::set_clock_source`, `ClockFollower`, `MidiIngest`/`StampedUmp`, `coremidi_input.cpp`. Stamp-on-arrival stays in the input edge thread; only the selected port's clock is forwarded; the RT plane receives clock only through the existing SPSC queue. Source selection must also gate Start/Continue/Stop. Switching source resets/re-locks the follower cleanly (no double trigger, state `clock_lost` consistent: switching to Internal clears it). A missing sysd keeps the last applied value (default Internal) and never blocks audio or the control tick. The ui sender is injectable (like the LED sender) with a real implementation using the sysdctl blocking client library on a background thread (non-blocking for the UI loop; errors reported as a notice, not a crash); the ui keeps its local SystemConfig clock_source in sync with what sysd reports (single source of truth = sysd; the local JSON field becomes a cache). Config persists in sysd, so it survives restart (test via the sysd store).

**Never:** No change to the follower algorithm or 7.6 behaviour, no DIN UART driver, no Top Bar display work (Story 7.8, already done reads clock_source from the engine state: keep that working, verify its source of truth now follows the sysd value), no other settings, no sysd protocol changes beyond what exists (clock_source is already in Config/ConfigPatch).

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Select DIN | config clock_source=DIN, clock on DIN | follower driven by DIN clock | none |
| Select DIN, clock on USB | same | USB clock ignored and counted | none |
| Select USB, clock on DIN | clock_source=USB | DIN clock ignored | none |
| Internal | clock_source=INTERNAL | any external clock ignored, follower idle, clock_lost clear | none |
| Change at runtime | USB→DIN while notes sound and arp runs | no voice cut, no glitch, no double trigger; new source locks | none |
| Both ports send | DIN selected, both send | only DIN reaches the follower | none |
| Start/Stop/Continue | on the unselected port | ignored | none |
| sysd absent | engine starts without sysd | Internal until sysd appears, then applied | no crash |
| ui change | Segmented Control to USB | SetConfig patch {clock_source=USB} sent; UI shows the sysd-reported value | send failure → notice, UI reverts to last known |
| Persistence | restart sysd and engine | selected source restored from sysd | none |

## Code Map

- `engine/host/include/velora/host/sysd_config_client.hpp`, `src/sysd_config_client.cpp`, `engine/host/macos/main.cpp` (`--sysd-socket` wiring; currently only logs the config) -- where the callback applies clock_source.
- `engine/host/include/velora/host/midi_ingest.hpp`, `src/midi_ingest.cpp`, `macos/coremidi_input.cpp` -- stamping, source identification, realtime clock UMP flow.
- `engine/core/include/velora/core/block_renderer.hpp` (`set_clock_source`, clock_lost state), `clock_follower.hpp`, `src/block_renderer.cpp` (lines ~431-440, 689-750, 955) -- RT application.
- `ui/src/screens/settings_view.cpp` (MIDI & Clock section), `ui/include/velora/ui/system_config.hpp` (ClockSourceMode), `ui/src/screens/performance_view.cpp` (clock display from engine state), `ui/src/ws_client.cpp`; `tools/sysdctl/*` (blocking client) -- ui side.
- Tests: `engine/core/tests/clock_follower_test.cpp`, `engine/host/tests/` (midi ingest/host tests), `ui/tests/epic10_settings_test.cpp`, `sysd/tests/*`, `engine/host/macos/engine_host_sysd_smoke.cpp`.

## Tasks & Acceptance

**Execution:**
- engine: apply sysd clock_source to the renderer on config callback (and on first config)
- midi ingest: port-kind tagging at stamp time, selected-port filtering of 0xF8/FA/FB/FC with drop counters; CoreMIDI source kind mapping (+ `--din-source`); hook documented for the board DIN adapter
- ui: Segmented Control writes via SysdConfigSender, shows sysd value, notice on failure; real sender using the sysdctl client on a worker thread
- tests: every matrix row (synthetic clock streams through the ingest + offline render of the arp following the selected port; runtime switch without cuts; sysd integration with a real sysd process in a temp dir for persistence; ui test with a fake sender); keep Story 7.6/7.8 tests green
- docs: engine/host/README.md, sysd/README.md (how clock_source flows)

**Acceptance Criteria:**
- Given the sysd config, then Clock source is Internal/DIN/USB (default Internal), stored with the config schema_version and delivered to the engine at start and on change.
- Given the Settings control, then a change takes effect without interrupting sound.
- Given DIN (or USB) selected, then the input edge thread stamps clock on arrival in device time and forwards only that port's clock to the follower; clock on the other port is ignored.
- The 10-minute by-ear arp check on hardware and the DIN UART are recorded as deferred.

## Spec Change Log

## Review Triage Log

### 2026-10-26 — Review pass (single focused reviewer)
- findings:
  - `[high]` `patch` stale old-port clock reached the freshly reset follower — dropped in RenderDriver at pop and before submit; clock group>0 doubled tick rate — group 0 only; source/epoch race in set_clock_source — single packed atomic
  - `[medium]` `patch` clock bypassed note headroom; wrong failure notice; ui startup/7.8 tempo label state; vacuous transport gating test; smoke claim overstated
  - `[low]` `defer` see deferred

## Verification

**Commands:**
- `cmake --build build -j8 && ctest --test-dir build -R "clock|midi|sysd|settings|arp|control|rt_traffic" --output-on-failure` -- expected: all pass
- `python3 tools/ci/check_core_deps.py` -- expected: exit 0

## Auto Run Result

Summary: sysd clock_source applied to the renderer (ingest filter first), port tagging (kUsb/kDin) and selected-port-only clock/transport forwarding with drop counters, CoreMIDI source mapping and --din-source, ui Settings control via SysdConfigSender (worker thread, revert + typed notices), PerformanceView/PartView follow the sysd value.
Verification: targeted tests (69) pass; full ctest passes except the 4 pre-existing failures; core deps check passes.
