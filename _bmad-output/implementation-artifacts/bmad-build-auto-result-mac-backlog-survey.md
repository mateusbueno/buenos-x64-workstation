---
status: blocked
---

# BMad Build Auto Result

Status: blocked
Blocking condition: dirty working tree (21 uncommitted paths on `main`: modified `ui/` sources, `tools/tokens/generate_tokens.py`, untracked `font_manager.*`, `ui/**/fonts/`, `ui/tests/full_navigation_suite_test.cpp`). The workflow requires a clean tree and a fitting branch before planning; this is someone's in-flight work, so it was not committed or stashed automatically.

## Survey: backlog stories that can advance on the Mac

Source: `planning-artifacts/mac-first-plan.md` crossed with non-done entries in `implementation-artifacts/sprint-status.yaml`.

### Fully on Mac (no hardware)
- 4.14, 4.15 design stories (HTML mockups); 4.19 is in `review`
- 9.13 Setlists view (Studio), 9.14 Library mgmt + heavy SFZ conversion, 9.22 Studio accessibility audit, 9.8 Live Set view
- 5.4 Reference corpus import (blocker 11.12 is done)
- 2.13 / 2.14 organ A/B and ABX (blocked by business stories 11.13 / 11.14)

### Start on Mac, finish on board (Mac part only)
- Engine: 1.18 parallel voice render (correctness), 1.21 diagnostics counters, 1.16 / 1.17 logic prototypes, 4.1 tier profile, 4.7 preload logic
- Keybed/panel readers: 3.5, 3.6, 3.7, 3.10, 3.11, 3.12
- Clock: 7.7 clock source setting, 7.9 offline tests
- Studio/device: 9.6, 9.7, 9.9, 9.10, 9.11, 9.12, 9.15 (protocol), 9.18, 9.19, 9.20 (mocked sysd), 9.21, 9.3 (bridge logic on localhost)
- Boot/update: 8.1 (mocked RAUC), 8.4, 8.5, 8.8, 8.9, 8.12, 6.11/6.12/6.13 (need ARM64 Linux VM)

### Board/hardware/business only (skip)
Remaining epic 1, 3.x firmware, 3.13-3.20, 5.9, 5.15, 6.6/6.7/6.10/6.14-6.17, 8.2, 8.6, 8.10, 8.11, 8.13, 8.14, 12.14, epic 11.
