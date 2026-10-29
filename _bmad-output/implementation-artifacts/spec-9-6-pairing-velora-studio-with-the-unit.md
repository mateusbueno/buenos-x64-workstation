---
title: 'Story 9.6: Pairing Velora Studio with the unit'
type: 'feature'
created: '2026-10-29'
status: 'done'
baseline_revision: '04c142270928929e70e4bb387fa44b361bbcf259'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/sysd/include/velora/sysd/pairing.hpp'
  - '{project-root}/sysd/src/pairing.cpp'
  - '{project-root}/sysd/include/velora/sysd/bridge_server.hpp'
  - '{project-root}/sysd/src/bridge_server.cpp'
  - '{project-root}/ui/include/velora/ui/screens/settings_view.hpp'
  - '{project-root}/ui/src/screens/settings_view.cpp'
  - '{project-root}/desktop/src-tauri/src/device.rs'
  - '{project-root}/desktop/src/app.js'
  - '{project-root}/sysd/PAIRING.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists need to safely authorize only approved computers (running Velora Studio) to modify their instrument. Pairing requires reading a random 6-digit code on the Velora physical screen and typing it into Velora Studio. The instrument's Settings menu must provide management of paired computers (listing paired computers, last connected timestamp, Forget action) and an Action Sheet that displays the active pairing code and closes when complete, expired (2 min), or cancelled (EXIT), without interrupting active sound rendering. Studio must present an accessible 6-digit pairing code dialog with focus trap, exact error reporting on invalid codes (max 5 attempts lockout), title bar status updates, and automatic persistence of long-term token secrets.

**Approach:**
1. Document pairing cryptography and trust model in `sysd/PAIRING.md` (physical USB trust boundary, ephemeral 6-digit code, SHA-256 token hashing, constant-time comparison, 5-attempt lockout, 2-minute window expiry).
2. Protocol & Daemon Integration (`proto/ws_v1.proto`, `sysd`):
   - Add additive messages for pairing management over UDS: `StartPairingWindow`, `CancelPairingWindow`, `ListPairedComputers`, `ForgetPairedComputer`, and replies `PairingWindowReply`, `PairedComputersReply`.
   - Wire `PairingWindow` and `PairedStore` into `sysd`'s `ConfigService` / `ConfigServer` so local clients (device UI) can initiate pairing windows and manage the paired computer list.
   - When a computer attempts pairing, update active computer name; when paired, push pairing window closure.
3. Device UI (`ui/`):
   - In `SettingsView` (Section: Computer / Velora Studio):
     - "Pair new computer" action button.
     - List of paired computers (display name, formatted last connected time) with "Forget" button per row.
     - Status line: "USB Audio: 48 kHz 24-bit · MIDI 2.0 / 1.0 ready".
   - When "Pair new computer" is activated:
     - Open `ActionSheet` on device showing eyebrow ("Computer · Velora Studio"), title ("Pair new computer"), 6-digit code in large tabular numbers, active computer name once client asks, and `EXIT · Cancel`.
     - 2-minute timeout timer; closes on EXIT or upon successful pairing.
     - Sound is completely unaffected (runs on UI thread, audio RT plane remains uninterrupted).
   - "Forget" button removes the computer from `PairedStore`. If forgotten computer reconnects, it is rejected with `NOT_PAIRED`.
4. Velora Studio Frontend (`desktop/`):
   - Provide `PairDialog` / `PairDialogDom` with accessible modal dialog:
     - Heading: "Enter the code shown on Velora 73" (or detected model name).
     - 6-digit input field with numeric inputmode and autofocus.
     - Tab/Shift+Tab focus trap, focus restoration to previously active element on close.
     - Enter confirms, Esc cancels.
     - Wrong code shows "That code doesn't match. Check the Velora screen."
     - Lockout after 5 wrong codes: pairing window closes, displays error that pairing must be restarted on the device.
     - On success: exchanges long-term token, stores token, Title Bar displays "● VELORA 73 · CONNECTED VIA USB · PAIRED" in ok color.
5. Localization:
   - Add all required UI keys in `locale/en.json`, `locale/es.json`, `locale/pt-BR.json`, and `desktop/src/catalogs.js`.

## Boundaries & Constraints

**Always:**
- Sound rendering must NEVER be interrupted during pairing window, Action Sheet navigation, or token exchange.
- Pairing codes must be compared in constant time (`constant_time_equals`).
- After 5 wrong attempts, pairing window closes permanently until restarted by user on the device.
- Pairing tokens must remain secret (stored as SHA-256 hex on device, stored in mode 0600 on client, never exposed in logs or telemetry).
- Forgotten computers must be refused with `NOT_PAIRED` on subsequent connections.
- Focus trap and focus restoration must be strictly maintained in the Studio pairing modal dialog.
- All new UI strings must be localized across `en`, `es`, and `pt-BR`.

**Never:**
- Never interrupt audio or drop MIDI messages during pairing window operations.
- Never allow an unpaired client to execute engine or library commands (only `Hello` and `PairRequest` allowed).
- Never allow a forgotten client token to authenticate.
- Never write tokens in plain text in logs or diagnostics.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Open Pairing Window | User taps "Pair new computer" on device Settings | Action Sheet opens with 6-digit code and 2 min countdown; sysd pairing window opens | If entropy unavailable, fails closed and displays error |
| Client requests pairing | Studio submits code and computer name | Action Sheet displays computer name; code verified in constant time | Wrong code increments wrong count, returns `PAIRING_CODE_INVALID` with attempts left |
| 5 Wrong Codes | Studio submits 5 invalid codes | Window closes; sysd returns `PAIRING_CLOSED` (reason: attempts); Studio shows lockout message | Device Action Sheet closes; user must reopen on device |
| Window Expiry | 2 minutes elapse without valid pairing | Window closes; sysd returns `PAIRING_CLOSED` (reason: expired) | Device Action Sheet closes |
| Successful Pairing | Studio submits correct 6-digit code | 256-bit token issued; paired computer stored in sysd; device Action Sheet closes; Studio title bar shows "● VELORA 73 · CONNECTED VIA USB · PAIRED" in ok | None |
| Reconnect Paired | Computer connects with saved token in subsequent session | Authenticated immediately without pairing code prompt | None |
| Forget Computer | User taps "Forget" on device paired list | Computer removed from `PairedStore`; list updates | If computer attempts to send commands, gets `NOT_PAIRED` |
| Cancel on Device | User presses EXIT while pairing Action Sheet is open | Pairing window closes in sysd; Action Sheet closes; sound keeps playing | None |

</intent-contract>
