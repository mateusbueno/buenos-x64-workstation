---
title: 'Story 11.19: Brand assets integration: final typeface, app icon and boot artwork'
type: 'feature'
created: '2026-10-29'
status: 'done'
baseline_revision: 'd16611c'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/desktop/src-tauri/icons/'
  - '{project-root}/desktop/src-tauri/tauri.conf.json'
  - '{project-root}/desktop/src/tokens.css'
  - '{project-root}/desktop/src/style.css'
  - '{project-root}/ui/assets/fonts/lato/'
  - '{project-root}/ui/include/velora/ui/font_manager.hpp'
  - '{project-root}/ui/src/font_manager.cpp'
  - '{project-root}/ui/src/screens/boot_view.cpp'
  - '{project-root}/THIRD_PARTY_NOTICES.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** The instrument UI and Velora Studio previously relied on stand-in placeholder assets (DejaVu Sans fonts, dummy 105-byte icons, and stand-in boot typography). The final assets (Lato OFL 1.1 typeface, geometric logo symbol, brand icons for macOS/Windows/Linux, and 72-frame boot animation with static frame) delivered in Stories 11.15 and 11.16 must be integrated so the product and app look like one finished, coherent brand.

**Approach:**
1. **Desktop Icons:** Replace dummy icons in `desktop/src-tauri/icons/` with the final assets (`ladiani.icns`, `ladiani.ico`, `linux/` PNG sizes) delivered in Story 11.16.
2. **Desktop Typeface:** Bundle `Lato-Regular.ttf`, `Lato-Bold.ttf`, and `OFL.txt` into `desktop/src/fonts/`. Define `@font-face` rules in `tokens.css` / `style.css` so Studio renders native Lato offline without network access.
3. **Device UI Typeface:** Generate `lato_regular_data` from `ui/assets/fonts/lato/Lato-Regular.ttf` and update `velora::ui::FontManager` to load Lato via `lv_tiny_ttf_create_data`, removing the DejaVu Sans placeholder.
4. **Boot Screen Artwork:** Integrate final boot symbol parameters and verify static frame under reduced motion.
5. **Verification:** Confirm all unit tests pass (LVGL simulator and UI tests, Tauri desktop tests, desktop node tests), contrast audits hold, and zero regressions occur.

## Boundaries & Constraints
- Zero raw color literals in CSS or theme files.
- Lato OFL 1.1 license and copyright notice must be bundled alongside the fonts.
- Desktop must continue functioning 100% offline without external font CDN dependencies.
- Screen layouts must accommodate accented capitals (`Á Ç Ê Õ Ñ`) without clipping.

## Acceptance Criteria
- [x] Delivered macOS (.icns), Windows (.ico), and Linux PNG icon assets are installed in `desktop/src-tauri/icons/`; dummy placeholder icons removed.
- [x] Lato Regular & Bold TTF files and license are bundled in `desktop/src/fonts/` with `@font-face` declarations.
- [x] Device `FontManager` loads Lato TrueType data into LVGL with token sizes (14, 18, 24).
- [x] Boot screen layout and reduced motion static frame match final design specifications.
- [x] All test suites (desktop node tests, cargo test, ctest ui tests) pass without regression.

</intent-contract>

## Review Triage Log

- **Blind Hunter Review:**
  - Verified no placeholder assets remain in `desktop/src-tauri/icons/`. Checked that `.icns`, `.ico`, and PNG assets match the final dimensions and formats.
  - Checked that `THIRD_PARTY_NOTICES.md` accurately documents the Lato SIL OFL 1.1 license and that `OFL.txt` is bundled alongside the desktop font files.
- **Edge Case Review:**
  - Audited color literals in `desktop/src/style.css` to eliminate raw hex/rgb literals; resolved in lint check.
  - Verified font loading fallback order in `desktop/src/tokens.css` (`'Lato', 'Gill Sans', Calibri, sans-serif`).
- **Verification Gap Review:**
  - Executed 18 LVGL UI tests in CTest; all 18 passed.
  - Executed 367 Node tests in `desktop/tests/*.test.js`; all 367 passed.
  - Executed 53 Rust unit tests in `desktop/src-tauri`; all 53 passed.
  - Audited format decrypt checks and private key checks; all passed.

