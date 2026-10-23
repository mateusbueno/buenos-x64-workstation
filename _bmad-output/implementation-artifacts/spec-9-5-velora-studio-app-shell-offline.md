---
title: 'Story 9.5: Velora Studio app shell, offline'
type: 'feature'
created: '2026-10-23'
status: 'done'
baseline_revision: '023ab6e'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/desktop/src/index.html'
  - '{project-root}/desktop/src/style.css'
  - '{project-root}/desktop/src/app.js'
  - '{project-root}/desktop/src/tokens.css'
  - '{project-root}/desktop/src-tauri/Cargo.toml'
  - '{project-root}/desktop/src-tauri/tauri.conf.json'
  - '{project-root}/desktop/src-tauri/build.rs'
  - '{project-root}/desktop/src-tauri/src/main.rs'
  - '{project-root}/tools/tokens/generate_tokens.py'
  - '{project-root}/tools/tokens/contrast_check.py'
  - '{project-root}/tools/ci/check_no_color_literals.py'
  - '{project-root}/locale/en.json'
  - '{project-root}/locale/es.json'
  - '{project-root}/locale/pt-BR.json'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Velora Studio needs to run as a cross-platform desktop application (macOS, Windows, Linux) with a thin Rust core linking ws.v1 protobuf types, launch offline when no Velora unit is connected, enforce a 1280×800 minimum size, adhere to DESIGN.md tokens via generated CSS custom properties, satisfy the contrast gate and color-literal lint rules, respect OS theme and language with user overrides, support full visual Tab order and F6 region cycling with a 2px accent ring at 2px offset, and limit all transitions to ≤80ms when reduced motion is preferred.

**Approach:**
1. **Tauri 2.12 project in `desktop/`:**
   - Created `package.json`, `desktop/src-tauri/Cargo.toml`, `desktop/src-tauri/tauri.conf.json` with 1280×800 minimum size (`minWidth: 1280`, `minHeight: 800`).
   - In `desktop/src-tauri/build.rs`, generates `ws_v1_params.proto` from `proto/params.json` via `proto/gen_params.py` and compiles `proto/ws_v1.proto` using `prost_build`.
   - Links generated protobuf ws.v1 types into `pub mod ws_v1` in `desktop/src-tauri/src/main.rs` with no device-only code.
   - Tested and verified via `cargo test` and CMake test target `desktop_rust_test`.
2. **Design Tokens & Generator:**
   - Updated `tools/tokens/generate_tokens.py` to emit `desktop/src/tokens.css` alongside `ui/generated/tokens.css` with both Stage (`-dark`) and Studio (`-light`) sets, invariant tokens, typography, spacing, and radius.
   - Verified that `tokens_verify` passes.
3. **Contrast Gate & Color-Literal Lint:**
   - Extended `tools/ci/check_no_color_literals.py` to scan `desktop/`, banning raw hex colors or color function calls outside theme token files.
   - Extended `tools/tokens/contrast_check.py` to check `desktop/src/tokens.css`.
   - Both self-tests and CI tests pass.
4. **Offline UI Shell:**
   - Title Bar: "Velora Studio", connection status "No Velora connected" in muted, "Import", "Backup", and disabled Primary Button "Write to Velora".
   - Sidebar: SETLISTS, LIBRARY, DEVICE with offline placeholders and device navigation items.
   - Main area: Spacious empty state with offline explanation ("Connect your Velora via USB to sync setlists, live sets, and library, or manage backups and local settings").
   - Footer: Connection status and last backup ("No backup yet").
   - Sizes in `rem` with body ≥ 13px (0.8125rem) and section labels ≥ 11px at 600 weight (0.6875rem).
5. **Localization & Theme Reactivity:**
   - Studio strings added to `locale/en.json`, `locale/es.json`, `locale/pt-BR.json`.
   - Frontend detects OS language with fallback to EN and provides settings override.
   - Theme follows OS scheme live (`@media (prefers-color-scheme: light)`) with manual overrides (System / Stage / Studio) in settings.
6. **Accessibility & Keyboard:**
   - F6 cycles focus between Sidebar (`#sidebar-region`) → Main (`#main-region`) → Footer (`#footer-region`).
   - Tab navigates in visual order.
   - Focused controls show 2px accent ring at 2px offset (`outline: 2px solid var(--accent); outline-offset: 2px;`).
   - `prefers-reduced-motion: reduce` enforces transition and animation duration ≤ 80ms.

</intent-contract>
