---
title: 'Story 2.18: Design tokens, Stage and Studio themes and the contrast gate'
type: 'feature'
created: '2026-10-22'
status: 'done'
baseline_revision: '45666f0766efea5ec85ea751d8d9b897645163a8'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-2-context.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/DESIGN.md'
  - '{project-root}/_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/EXPERIENCE.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** UI styling (colors, typography, dimensions, spacing, elevation) must not be hard-coded in individual widgets or screens. Without a single token source, themes drift, contrast can regress below accessibility standards, and runtime theme switching (Stage vs Studio) is error-prone.

**Approach:**
1. Implement a pluggable token generator under `tools/tokens/generate_tokens.py` that parses the YAML frontmatter of `DESIGN.md`.
2. Generate the LVGL theme module (`ui/include/velora/ui/theme.hpp` and `ui/src/theme.cpp`) providing:
   - Stage (dark) and Studio (light) palettes.
   - Theme-invariant drawbar, LED, panel, and logo colors.
   - Typography tokens, radius tokens, and spacing scale.
   - LVGL styles for containers, buttons, labels, and borders.
   - Runtime theme switching (`ThemeManager::set_theme(...)`) that restyles all active widgets immediately without restart.
3. CI Lint: `tools/ci/check_no_color_literals.py` that fails the build if color literals exist in `ui/` outside generated theme files.
4. CI Contrast Gate: `tools/tokens/contrast_check.py` enforcing WCAG 2.x relative luminance (>= 4.5:1 for text pairs, >= 3:1 for meaningful boundaries, and rejecting opacity on text).

## Boundaries & Constraints

**Always:**
- Single source of truth for tokens is `DESIGN.md` frontmatter.
- Theme generator architecture must be pluggable (supporting future Velora Studio CSS export).
- Contrast gate must run in CI/CTest.
- No color literals in UI code outside theme files.
- Restyling must occur cleanly at runtime.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Generator run | Run `generate_tokens.py` | Emits `ui/include/velora/ui/theme.hpp` and `ui/src/theme.cpp` | Fails if frontmatter malformed or missing required tokens |
| Contrast check | Run `contrast_check.py` | Computes contrast ratios for all pairs in Stage and Studio; reports PASS | Non-zero exit with failing pairs listed if ratio < 4.5:1 (text) or < 3:1 (boundary) |
| Color literal lint | Run `check_no_color_literals.py` | Scans `ui/src` and `ui/include`; reports PASS | Non-zero exit if raw hex / color literal detected |
| Runtime theme switch | `theme_manager.set_theme(ThemeId::Studio)` | Active screen and widget styles update instantly; values and labels preserved | No-op if same theme requested |

</intent-contract>

## Tasks & Acceptance

- [x] Create `tools/tokens/generate_tokens.py` with pluggable emitters (`LvglEmitter`, `CssEmitter`)
- [x] Create `tools/tokens/contrast_check.py` with `--self-test`
- [x] Create `tools/ci/check_no_color_literals.py` with `--self-test`
- [x] Generate `ui/include/velora/ui/theme.hpp` and `ui/src/theme.cpp`
- [x] Implement `ThemeManager` in `velora_ui` and integrate theme styles into `ShellView`
- [x] Add runtime theme toggle test `ui/tests/theme_test.cpp`
- [x] Wire generator and checks into CMakeLists.txt and CTest
- [x] Verify all tests pass (100%)
