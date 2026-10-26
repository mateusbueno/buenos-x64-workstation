# Velora Studio (desktop)

Tauri app (`desktop/src` web UI, `desktop/src-tauri` Rust). `cd desktop/src-tauri && cargo test` runs the Rust tests.

## Device connection (Foundation: Velora Studio device connection)

Studio reaches a unit through the **sysd TCP bridge** (`sysd/README.md`, "TCP bridge"): ws.v1 on one TCP port, the same schema as the on-device clients. The client is
`src-tauri/src/device.rs` (blocking TCP on its own thread, standard library only, so it builds the same on macOS, Windows and Linux).

- **Configure**: set `VELORA_DEVICE=host:port` before starting the app, or call `window.veloraDevice.connect(host, port)` (Tauri command `device_connect`). Without it nothing is
  configured and the **simulated device stays the offline fallback** (the existing behaviour). Once configured, the title-bar indicator and the offline card follow the real status.
- **Status** (`device_status`, event `device://status`): `disconnected` | `connecting` | `unpaired` | `paired` | `version-mismatch`, plus the device info (model, firmware, device id,
  negotiated features). The client reconnects with backoff (50 ms doubling to 2 s) after the unit or the bridge drops and subscribes again; a protocol mismatch is a status, not a crash.
- **Pairing**: the first time, the unit shows a code; `device_pair(code, name)` (the UI shows a minimal in-page input for now, re-opened by clicking the connection indicator while
  "unpaired"; the real dialog is Story 9.6). The unit answers with a **pairing token** (never shown to the page, never logged): it is stored per **device id** in
  `<app data>/device_pairing.json` (mode 0600) and presented only to a unit that reports that same device id (a second Hello on the same connection). The computer id
  (`<app data>/device_client_id`, random from the OS CSPRNG, mode 0600) only names the computer. A failure to save either is shown as `persist_error` in the status, not hidden
  behind a new id at every launch. With no app data folder the id and token live for this run only.
- **State**: `device_state` (latest engine State as JSON) and the event `device://state` (every State received, reply or push). `window.veloraDevice.state / onState(cb)` is the store Stories
  9.8 (Live Set view) and 9.13 (Setlists view) read. `device_request` (and `window.veloraDevice.request`) sends any ws.v1 `ClientMessage` given as JSON (`{ "body": { "GetState": {} } }`,
  missing fields default) and resolves to the `ServerMessage` as JSON; a typed error from the unit is a normal reply whose body is `Error`.
- The editor still renders the simulated Live Sets while paired: the connection label says so and **Write to Velora is disabled** until Stories 9.8 / 9.13 map the real State.
- `device_request` is a **trusted-frontend capability**: any page in the webview can send what a paired computer may send. The webview therefore loads only Studio's bundled assets
  (strict CSP in `tauri.conf.json`: `default-src 'self'`, IPC only), and the bridge's message table is the second fence. JSON numbers are doubles in the webview: u64 values
  (revisions, ids) are exact below 2^53.
- The client drops a peer that sends nothing for 50 s (a keepalive every 20 s is answered), reconnects with jittered backoff (50 ms doubling to 2 s, reset only after a HelloAck).

Try it on the Mac (real engine, no audio): start `engine-host --no-audio --socket /tmp/e.sock`, then
`velora-sysd --data-dir /tmp/vd --bridge-port 7410 --engine-socket /tmp/e.sock --pairing-code 123456`, then `VELORA_DEVICE=127.0.0.1:7410 npm run dev` and enter `123456` when asked.
`VELORA_TEST_BRIDGE=127.0.0.1:7410 VELORA_TEST_PAIRING_CODE=123456 cargo test real_bridge_round_trip -- --nocapture` runs the client against such a bridge (the pairing window closes at the first
pairing, so restart `velora-sysd` to pair again). `ctest -R engine_host_bridge_studio_smoke` does all of this automatically (skipped with a message if cargo is missing).

## Live Set view (Story 9.8)

While the unit is **paired**, the main area is the real Live Set the player chose in the sidebar (SETLISTS expands to the setlists and their Live Sets, plus "All Live Sets"): header (name in the Live Set
name style, a warn-coloured **◆** with `aria-label="Unsaved changes"` and a tooltip when the device holds unsaved edits for it), 8 scene tiles (the current one filled, missing ones dashed) and 8 part
strips (`PART n` + LED, timbre name, category in accent, level meter, `VOL n`, key range, the organ registration `88 8600 000` on organ parts, "+ Add timbre" on empty parts, "Missing library" with a
glyph in warn when the timbre's library is missing, and an `OFF` tag instead of the volume on parts that are off). Unpaired or offline, the simulated Live Sets and the offline card behave as before.

- **Read-only.** Selecting a Live Set sends `GetLiveSetView {live_set_id}` (ws.v1, `proto/README.md` "Story 9.8") and `GetSetlists` for the sidebar, and nothing else: never a Jump, Recall, Load or
  Write, so what the unit plays does not change. A refused or failed read shows an inline banner and keeps the previous view. **Write to Velora stays disabled** (writes arrive with Stories 9.10-9.12).
- **Live deltas.** The Live Set that sounds follows the `device://state` pushes in place: current scene, part on/off, volume, zone, the missing-library / engine-not-available state (additive
  `PartState` fields) and the ◆ (State.edits). Only the tiles that changed are touched. The update runs synchronously in the handler of the push; `lastDeltaMs` measures the view-model and the
  render callback only (not the device, the bridge or the webview's paint), and `tests/live_set_view.test.js` measures the view-model + diff cost with the real clock (microseconds per push; the
  asserted bound is a generous 10 ms against the 100 ms budget). **A State from an older unit** (without the per-part fields: `PartState.view_fields` is not set, and serde reads the rest as
  zeros) never overlays a part: the static view stands for the parts, while the scene, the sounding flag and the ◆ still follow. A State that changes what the view cannot follow (a timbre / engine /
  part-list / scene-name edit, `has_timbre_edit`, the true `change_count` (the summary lists only 32), the unsaved flag, a Write, a rename, a library revision, a jump to or away) makes the
  controller **read the view again**, through a throttled single-flight runner (one read at a time, at most one per 300 ms, trailing; a failed read is not retried until the signature
  changes). The signature is that of the State current when the shown reply's request was SENT, so a reply older than a structural change is followed by a read. A Live Set that does not
  sound is a static picture from `GetLiveSetView` (with the device's unsaved-edit overlay when it holds one). After an outage nothing from before it is replayed: the first push after the
  reconnect feeds the view. The sidebar reads `GetSetlists` through the same kind of runner (400 ms) when `State.setlists.library_revision` or the loaded setlist moves.
  JSON numbers are doubles: `revision` and `library_revision` (u64) are exact below 2^53, and the view reply itself has no u64.
- **Part states.** `Missing library` (any resolver failure of a sample timbre: the engine's `unavailable_reason` names it, Studio shows them all alike), `Engine not available` (State
  `part_state` 3: FM / VA in a firmware without the engine; warn, never a normal sounding strip), `+ Add timbre`, `OFF`.
- **Code.** `src/live_set_view.js` is a pure ES module (no DOM, no Tauri, no locale): `buildModel`, `applyState`, `diffModels`, `structuralSignature`, `moveFocus`, the sidebar row helpers and
  `LiveSetViewController` (select / onState / selectPart; stale replies are dropped). `src/app.js` draws it. Tests: `cd desktop && node --test tests` (also ctest `desktop_live_set_view_node_tests`
  when node is installed); the Rust JSON mapping of the new messages is covered by `cargo test live_set_view`.
- **Accessibility.** Both grids are `role="grid"` (one row of 8 cells) with a roving tabindex: Left / Right move and **wrap** (Left from the first goes to the last, Right from the last to the first),
  Home / End jump, Up / Down stay (one row). **Enter or Space on a part strip selects it** (aria-selected, the selection is announced in the polite live region); Enter on a scene does nothing (no
  scene recall from Studio). Sizes are in rem (body 13 px, labels 11 px at weight 600), the 2 px focus ring is the global one, colours are tokens only, and every string is a locale-catalog key
  (`studio.live_set.*`, `studio.sidebar.*`; en, es, pt-BR in `locale/*.json` and the catalogs embedded in `app.js`).
- **For Story 9.9 (Part Editor).** The selection is exposed as `window.veloraLiveSetView` (`liveSetId`, `selectedPart`, `model`, `onPartSelected(cb)`) and as the document event
  `velora:part-selected` (`detail: { liveSetId, part, model }`). The parameter cards under the strips are still the simulated editor; they follow the real selection's engine (FM / VA cards) only.
- **Deferred.** Accessibility: the ◆ tooltip (`title`) is mouse-only (its text is the `aria-label`'s sibling, not reachable by keyboard focus); the grids' `role="row"` elements use
  `display: contents`, which some WKWebView versions drop from the accessibility tree (to be checked on the target webview). `State.edits.dirty_live_set_ids` lists at most 128 Live Sets, so a Live
  Set that does not sound and is past that cap may lose its ◆. A Pro unit's 16 parts: the first 8 are shown with a note (the reply carries all 16: `LiveSetViewReply.part_count`); a 16-part Studio layout is a later story. Add timbre (Story 9.11) is a
  disabled placeholder with a tooltip. Setlist editing is Story 9.13. The category of a part comes from the bundle's `category` file (formatlib), so a protected library whose bundle has none shows no category.

Try it without a unit: serve `desktop/src` over http with a stub for `window.__TAURI__` (`core.invoke` answering `device_status`, `device_state` and `device_request`; `event.listen` for
`device://status` / `device://state`) and open it in a browser; this is how the page was checked visually (stage and studio themes, pt-BR, keyboard, deltas, error banner, offline fallback).
