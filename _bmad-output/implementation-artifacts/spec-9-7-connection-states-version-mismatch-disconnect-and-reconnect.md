---
title: 'Story 9.7: Connection states: version mismatch, disconnect and reconnect'
type: 'feature'
created: '2026-10-29'
status: 'done'
baseline_revision: '3a56caf9430db88589bf4ae2a2a0614828ce26b1'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/desktop/src/app.js'
  - '{project-root}/desktop/src/index.html'
  - '{project-root}/desktop/src/style.css'
  - '{project-root}/desktop/src/catalogs.js'
  - '{project-root}/desktop/src-tauri/src/device.rs'
  - '{project-root}/desktop/src-tauri/src/main.rs'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists need immediate, unambiguous feedback in Velora Studio when connection state changes (cable disconnect, device restart, protocol or feature version differences), so they never edit stale data or wonder whether changes reached the instrument. Disconnections must display a non-modal Studio Banner with a Reconnect action while the app seamlessly switches to offline mode; reconnects must perform a full state snapshot fetch, clear the disconnect banner, and announce "Connected" through a polite live region. Version and feature mismatches must hide unsupported features and provide informative guidance without crashing.

**Approach:**
1. **Title Bar & Indicator:**
   - On disconnect: within 2 s of status transition, Title Bar status text updates to "Velora 73 disconnected" (`studio.conn.disconnected`), indicator turns to accent/muted.
   - On reconnect: Title Bar returns to "● VELORA 73 · CONNECTED VIA USB · PAIRED" (or current model name in ok).
2. **Studio Banners (Top of Main Working Area):**
   - Provide a dedicated, non-modal `ConnectionBannerView` component rendered at the top of the main area:
     - **Disconnect banner:** Displays warning icon, message ("Velora 73 disconnected. USB connection was interrupted. Offline editing mode is active; live sync paused."), and "Reconnect" action button. Clicking "Reconnect" invokes `window.veloraDevice.connect(host, port)` and displays connecting state.
     - **Version Mismatch banner:** Displays when protocol/feature version differs ("Velora 73 runs {dev_ver}; this Velora Studio needs {app_ver} for some features. Update firmware / Update app."), hiding unsupported features.
   - Dismisses automatically when the connection is restored.
3. **Accessibility & Polite Live Region Announcements (UX-DR91):**
   - On disconnect: announces "Velora 73 disconnected" via polite live region (`#live-region`).
   - On reconnect: announces "Connected" via polite live region.
4. **State Resynchronization (AR25):**
   - When reconnecting after an outage, Studio re-fetches device status and state, refreshes setlists, part strips, and scenes with fresh unit data, and clears any stale in-flight banners.
5. **Localization:**
   - Define all required strings across `en`, `es`, `pt-BR` in `locale/*.json` and `desktop/src/catalogs.js`.

## Boundaries & Constraints

**Always:**
- Disconnect banner must be non-modal and persistent until connection is restored.
- Clicking "Reconnect" must trigger immediate reconnection attempt without page reload.
- State resynchronization on reconnect must completely replace in-memory view state with fresh unit snapshot.
- All announcements must go through polite live region (`aria-live="polite"`).
- Strict design token fidelity (no raw color literals).

**Never:**
- Never block offline editing or modal dialogs when disconnected.
- Never display stale unsaved flags for edits that were pending when cable was pulled.
- Never retry in an unconstrained storm on version mismatch.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Cable Pulled / Device Offline | `st.status = "disconnected"` | Title bar reads "Velora 73 disconnected"; Studio Banner shows disconnect notice with "Reconnect" button; live region announces "Velora 73 disconnected"; switches to offline fallback | Graceful offline mode |
| Click Reconnect Button | User clicks "Reconnect" on banner | Calls `connect(host, port)`; button disabled / shows connecting state | If still unreachable, stays disconnected |
| Reconnect Succeeded | `st.status = "paired"` | Banner removed; Title bar updates to paired; Live region announces "Connected"; fresh Live Set snapshot requested and rendered | None |
| Version Mismatch | `st.status = "version-mismatch"` | Title bar shows mismatch; Studio Banner shows firmware/app version update prompt; live edit disabled; features missing on device hidden | No retry storm |
| Rapid Disconnect / Reconnect Flap | Multiple status events within 500ms | Banners and live region update idempotently without duplicate banners or memory leaks | Throttled / debounced updates |

</intent-contract>
