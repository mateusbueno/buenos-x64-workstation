---
title: 'Story 4.28: On-screen Keyboard'
type: 'feature'
created: '2026-10-22'
status: 'implemented'
baseline_revision: '6de477e'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-4-context.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/DESIGN.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/EXPERIENCE.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/mockups/on-screen-keyboard.html'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists entering or editing names, searches, or tags need an intuitive, accessible on-screen keyboard (OSK) supporting their locale's layout (EN, PT-BR with Ç/accents, ES with Ñ/accents), touch targets ≥56 px, with Done and physical button support, without ever blocking sound or obscuring the Top Bar or the input field.

**Approach:**
1. Implement `OnScreenKeyboard` in `ui/include/velora/ui/components/on_screen_keyboard.hpp` and `ui/src/components/on_screen_keyboard.cpp`:
   - Positioning: lower content area (width 1024, height 276 at y = 324), leaving Top Bar (0..56) and field area (56..140) visible.
   - Keys: 4 rows of keys, each ≥ 56 px (rendered height 58 px) on `Theme::current().raised` with `line_strong` border, `r.sm` corners.
   - Key layouts:
     - EN: QWERTY layout.
     - PT-BR: QWERTY with direct Ç key, dead/popover accents (á, à, â, ã, é, ê, í, ó, ô, õ, ú).
     - ES: QWERTY with direct Ñ key, accents (á, é, í, ó, ú, ü).
   - Done key: accent-filled (`Theme::current().accent`, on-accent text).
   - Functions:
     - `open(initial_text, locale)`
     - `close()`
     - `set_text(text)` / `text()`
     - `on_text_changed`, `on_confirm`, `on_cancel`
   - Physical panel mapping:
     - `MCU_BTN_WRITE` confirms (equivalent to Done).
     - `MCU_BTN_EXIT` closes keeping the typed text.
     - Keybed playing continues unimpeded.
2. Unit tests in `ui/tests/on_screen_keyboard_test.cpp`:
   - Verify keyboard opens without covering Top Bar or field.
   - Verify key touch targets are ≥56 px.
   - Verify layout switching (EN, PT-BR with Ç, ES with Ñ).
   - Verify typing, space, backspace, and accents.
   - Verify WRITE/Done confirms and EXIT closes.

## Boundaries & Constraints

**Always:**
- Strict adherence to `DESIGN.md` and `on-screen-keyboard.html`.
- Zero audio allocations or locks on RT plane.
- Localized strings from catalog only (`locale/*.json`).
- Touch targets ≥ 56 px (`Theme::spacing().touch_target_min`).

</intent-contract>

## Tasks & Acceptance

- [ ] Add localization keys for OSK to `locale/*.json`
- [ ] Create `OnScreenKeyboard` class with key layouts for EN, PT-BR, ES
- [ ] Implement key styling with ≥56 px targets, accent-filled Done key
- [ ] Implement text manipulation (type, backspace, space, shift, accents)
- [ ] Connect panel WRITE and EXIT events
- [ ] Add unit test suite in `ui/tests/on_screen_keyboard_test.cpp`
- [ ] Verify build, CI linters, and 100% test pass
