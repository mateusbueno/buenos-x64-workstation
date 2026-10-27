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
  disabled placeholder with a tooltip. Setlist editing is the Setlists view below (Story 9.13). The category of a part comes from the bundle's `category` file (formatlib), so a protected library whose bundle has none shows no category.

Try it without a unit: serve `desktop/src` over http with a stub for `window.__TAURI__` (`core.invoke` answering `device_status`, `device_state` and `device_request`; `event.listen` for
`device://status` / `device://state`) and open it in a browser; this is how the page was checked visually (stage and studio themes, pt-BR, keyboard, deltas, error banner, offline fallback).

## Setlists view (Story 9.13)

While the unit is **paired**, SETLISTS > **Manage setlists** (sidebar, disabled with the offline hint otherwise) opens the Setlists view in the main area: the setlists on the left
(name, tag chips, Live Set count, missing count, a **LOADED** marker) and, for the selected one, its Live Sets in order on the right (name, a `missing` flag for entries whose Live Set no
longer exists: kept, flagged, removable, never in the way of reordering). Choosing a Live Set in the sidebar, or **Close**, goes back to the Live Set view.

- **What it does.** Create / rename / delete / reorder setlists; add (from All Live Sets), remove and reorder Live Sets inside a setlist; move a Live Set to another setlist or position;
  create a Live Set (8 empty parts, one scene) and optionally add it to the selected setlist; create / rename / delete tags and set a setlist's tags (at most 8). Every change is the **same
  ws.v1 control-thread command the unit's screen sends** (`CreateSetlist`, `RenameSetlist`, `ReorderSetlists`, `DeleteSetlist`, `AddLiveSetToSetlist`, `RemoveLiveSetFromSetlist`,
  `ReorderLiveSetsInSetlist`, `CreateLiveSet`, `CreateTag`, `RenameTag`, `DeleteTag`, `SetSetlistTags`; `GetSetlists` reads) and is persisted by the engine exactly as the unit persists it.
  It never sends `LoadSetlist`, `UnloadSetlist`, a step, a Jump, a Recall, a Write or `DeleteLiveSet`: **removing a Live Set from a setlist never deletes the Live Set**, and deleting a
  setlist never deletes its Live Sets. Every message it sends has a row in the bridge's message table (`liveset.v1`, routed to the engine; the bridge serves that table only to a paired computer), and `setlists_view.test.js` checks that; the stand-in engine used by the tests also throws on any other message name, so every test asserts at run time that no Load / Step / Jump / Recall / Write was sent.
- **Optimistic, rolled back, always converged.** Reorders, renames, removals and tag changes show at once, are sent one command at a time (a queue), and are restored from a snapshot when
  the reply is an error (typed, shown inline: limit reached with its numbers, name refused, order no longer valid, ...). After the last queued change the library is **read again**
  (`GetSetlists` with the selected setlist and All Live Sets), so the view always ends on the engine's order. A read reply that arrives while a change is pending is dropped. The error of a
  refused command stays until the next change succeeds (a good re-read clears only a read error). While open, `State.setlists.library_revision` / the loaded setlist move the view to a
  throttled re-read (the selection is kept while the setlist still exists), so a change made on the unit shows up.
- **Full permutations.** `ReorderSetlists` carries all setlist ids and `ReorderLiveSetsInSetlist` the whole ordered list, never a partial one (the engine refuses those with
  `INVALID_ORDER`; the tests send one on purpose).
- **Move to another setlist is two commands.** `AddLiveSetToSetlist` (target, at the start with `index: 0`, or at the end with no index) **then** `RemoveLiveSetFromSetlist` (source). The
  engine has no atomic move (the unit's setlist screen, `ui/src/screens/setlist_view.cpp`, has no cross-setlist move to copy), so if the Add succeeds and the Remove fails the Live Set is in **both** setlists (never in neither); the
  view says so ("partial move") and the re-read shows exactly that. A Live Set the target already holds is listed disabled when the view knows it (it knows the selected setlist and the ones it
  moved into); otherwise the engine's `DUPLICATE_LIVE_SET_IN_SETLIST` is shown.
- **Names.** Trimmed client-side, then 1..64 UTF-8 **bytes**, valid text, no control characters (the store's rule); limits 100 setlists, 64 tags, 256 Live Sets per setlist, 8 tags per setlist
  (`LIMITS` in `setlists_view.js`). Violations of the name rule are shown in the dialog and never sent; the other limits are checked from what was last read and the engine stays the authority
  (its typed error is shown). Names from the device are drawn with `textContent` only.
- **Keyboard.** Both lists are `role="listbox"` with a roving tabindex: Up / Down / Home / End move the focus, **Enter / Space select** (a setlist shows its Live Sets; a Live Set becomes the target
  of Move to... and Remove), **Alt+Up / Alt+Down move the focused row** (setlist or Live Set), F2 renames a setlist, Delete or Backspace (the Delete key of a Mac's main keyboard) deletes a setlist / removes a Live Set (both ask first, Cancel has the
  focus; the focus then lands on the row now at that place, or on the list's button when it is empty), **Alt+M** (or the context-menu key / Shift+F10) opens **Move to...** on a Live Set, a labelled `role="menu"` (Up / Down / Home / End, Enter, Escape returns the focus): positions of
  this setlist, then "Setlist: at the start / at the end" for every other setlist. **Add Live Set...** is the same kind of menu over the Live Sets not yet in the setlist. Dialogs
  (`role="dialog"`, `aria-modal`) trap Tab and close on Escape. Every result is announced in the polite live region ("Moved Song X to position 3 of 12", "Moved X to Y", "Already first", ...);
  errors are a `role="alert"`.
- **Mouse.** HTML5 drag and drop: every row is `draggable` and has a handle glyph labelled "Drag to reorder X" (a `role="img"` label, not a control: **Alt+Up / Alt+Down and Move to... are the real non-mouse equivalents**); a 2 px accent line shows the gap the row would land in (before /
  after the row under the pointer), and a Live Set dropped on another setlist's row (dashed outline) moves it to the end of that setlist. A drop on the gap next to itself sends nothing. A render
  that arrives under a drag in progress is deferred until it ends.
- **Layout and strings.** Sizes in rem, token colours, the global 2 px focus ring; rows wrap and the two columns become one when the main area is narrower than two 22 rem columns (written to avoid horizontal scroll; **not verified at 200 % text**). Every string is a `studio.setlists.*` key in en / es / pt-BR (`locale/*.json`, embedded in `src/catalogs.js`; `tests/catalogs.test.js` checks that every key the two modules
  use exists in all three).
- **Code.** `src/setlists_view.js` (pure: limits, name validation, ordering and permutation helpers, `moveTargets`, request builders, `parseReply`, `errorMessage`, `SetlistsController`),
  `src/setlists_dom.js` (`SetlistsView`: lists, keyboard, drag and drop, menus, dialogs, tag editor), wired in `src/app.js` (`setlistsCtl`, `openSetlistsView`). Tests: `cd desktop && node --test tests`
  (`setlists_view.test.js` against `tests/support/fake_engine.js`, a stand-in with the store's limits and typed errors; `setlists_dom.test.js` with a DOM shim: keyboard moves, drag simulation,
  hostile names, dialogs, tags, disabled state); `cargo test setlist` (the page's JSON for every command maps onto the real `ClientMessage`, and the replies come back as the JSON it parses);
  `ctest -R engine_host_bridge` (the real engine through the real bridge: create, rename, reorder, tag, add / remove, create Live Set with the Rust client and the C++ client, then **an engine restart
  on the same data directory** and a read-back; it fails when the engine lists fewer than two setlists, so an empty read can not pass).
- **Deferred.** Loading / stepping a setlist and Live Set editing stay out of this view (Stories 9.9-9.12). No undo. A Live Set's membership in setlists other than the ones this session has
  read is not known up front (the engine's duplicate error covers it). Drag and drop is mouse only (touch / pen drags rely on the webview's HTML5 support); the keyboard and Move to... are the
  equivalent for everything else. Tags have no colour (the store has none).

Smoke-viewed (not a full visual check) in the browser pane at about 800 px wide, dark theme, against a stub: the list layout, Alt+Down with its live-region text, the Move to menu and the tag dialog. Not checked in the real Tauri webview, in the light theme or at 200 % text. To repeat it without a unit: serve `desktop/src` over http with a stub for `window.__TAURI__` whose `device_request` is answered by `tests/support/fake_engine.js` (paired status, a State with
`setlists.library_revision`), open it, then SETLISTS > Manage setlists.

- **Tauri and drag and drop.** On Windows, Tauri's native file-drop handler (`dragDropEnabled`, on by default) swallows HTML5 drag events in the webview, so mouse reordering needs
  `"dragDropEnabled": false` on the window in `tauri.conf.json` (not set here: it is a Windows-only concern and the keyboard path works everywhere). Not tested on Windows.
- **Also deferred (Story 9.13 review):** a warning when the setlist being deleted is the loaded one on the unit; `select()` clears the shown entries before its read, so a failed read leaves an
  empty list (with the error) until the next refresh; `inert` / `aria-hidden` on the page behind a dialog or menu (focus is trapped, screen-reader virtual cursors are not); the `dragleave`
  flicker of the drop indicator in WebKit; a right-click context menu (the keyboard has Alt+M / the context-menu key); bidirectional and zero-width characters in names (the store accepts them,
  the view draws them as given); two announcements in one tick (the live region keeps only the last).


## Part Editor with Studio Knobs (Story 9.9)

> **MOST KNOBS ARE STORED BUT NOT AUDIBLE YET (the main deferred engine gap).** The engine stores every registry part parameter (`BlockRenderer::apply_param`) and calls
> `Engine::set_part_param`, but only `OrganEngine` overrides it: `FmEngine`, `VaEngine` and `SampleEngine` ignore it, and ids 402..408 (`part_brightness`, `part_attack`, `part_release`,
> `part_eq_*`) are consumed by **no** engine. So edits to FM / VA / sample knobs and the whole **Amp envelope + EQ** section change the *stored* value, and what the read-back
> (`GetPerformanceValues`) confirms is that stored value, **not the sound**. What takes effect: **organ parameters, the sends, the output, the part volume, the insert chain and ASSIGN.**
> Each inert knob carries a tooltip / `aria-description` ("No audible effect on this engine yet") driven by `INERT_BY_ENGINE` in `src/part_editor.js` (a small table: remove a row when an
> engine starts consuming those parameters), and the Amp envelope + EQ section says so in a note. Which engine owns which parameters is for the engine stories to decide.

While the unit is **paired** and a part is selected in the Live Set view, the area below the strips is the real Part Editor (offline / unpaired, the simulated editor of Stories 9.5 / 12.13 is unchanged). Four hairline-divided
sections, no menus, no tabs: **Sound** (the part volume, then the engine's cards: organ drawbars / percussion / vibrato / tone / rotary, FM algorithm / global / LFO / pitch env / OP 1..6, VA oscillator 1 / oscillator 2 / sub / noise /
mixer / filter / filter envelope / LFO / voice), **Amp envelope + EQ** (the envelope polyline drawn from attack / decay / sustain / release, then the EQ; a VA part shows its own `va_amp_*` envelope here and not the generic
attack / release, which are hidden for VA), **Effects inserts + sends** (the insert chain with bypass, reorder, remove and "+ Add insert"; reverb / delay sends; output), **Zone & keyboard · Arpeggiator · Assign** (the zone, the
arpeggiator, and ASSIGN · KNOBS (THIS LIVE SET): K1..K8 each a select of the assignable parameters). The parameters come from the generated registry: `src/params_registry.js` is generated from `proto/params.json`
(`python3 proto/gen_params.py --defs proto/params.json --desktop desktop/src/params_registry.js`; `ctest -R params_generator_tests` fails when it is stale) and `proto/README.md` ("Part Editor grouping") says where each parameter
shows. A parameter added to the registry **is displayed** with no UI code (`tests/part_editor.test.js` adds one to a registry fixture); whether the unit *consumes* it is a separate matter (see the box above).

- **Studio Knobs.** Every parameter is an ARIA `slider` (name, `aria-valuemin` / `max` / `now`, `aria-valuetext` with the unit: "+3.5 dB", "300 ms", "62 %", "On", "Hall"; the visible label and value text are `aria-hidden`
  so a screen reader says each once). Keys on a focused knob (not with Ctrl / Cmd / Alt): Up / Right +1 step, Down / Left -1, **Shift = 10 steps**, PageUp / PageDown the large step, Home / End min / max, Space toggles a switch,
  **Enter opens a value entry** (Enter applies, Escape cancels; a value out of range or not a number is told in the editor's polite live region and next to the knob, and the knob keeps its value; units, a decimal comma,
  "1.5 s", "2k" and enum labels are understood). Pointer: press and drag (up / right raises; the full range is 200 px, Shift makes it 5 times finer, and pressing or releasing Shift mid-drag carries on from the current value;
  switches, enums and small ranges take 24 px a step). **The wheel changes the knob that has the focus** (scrolling the page over other knobs never changes a value) and accumulates `deltaY` (one step per 100 px, so a trackpad's
  momentum does not sweep the range). Steps: integers 1, floats about 1 % of the range on a 1-2-5 grid, enums 1, and **time / frequency knobs with a wide range (1..5000 ms, 20..20000 Hz, 200..8000 Hz) a logarithmic grid of 100
  points** so a 5 ms attack and a 100 Hz cutoff are reachable by keys and drag. **Deviation from the spec:** Tab visits every knob (one tab stop per knob, in the order drawn) instead of a roving order, because the arrow
  keys change values. Read-only controls are `aria-readonly` with a tooltip reason; a pending value is `aria-busy` with a dashed track (not by colour alone).
- **Sending.** Every change is a ws.v1 edit carrying `base_revision` (Story 9.10: the Live Set revision, `State.edits.live_set_revision`, the view was LOADED at; the engine refuses a stale one with `STALE_REVISION` and the editor shows "Changed on the Velora since you opened it · Reload"), mapped to the command table in `proto/README.md` ("Edits from
  Studio"). A performance edit is **queued** for the RT plane (Ack = queued; it is applied at the next audio block when audio runs). Sends are coalesced per knob: at most one request in flight per knob, at least 30 ms between two (**140 ms for
  structural edits**: volume, sends, output, insert chain, ASSIGN, each of which publishes a generation and a State push, so a drag cannot fill the 32-slot generation ledger), the latest value wins, and switching part or Live Set
  drops what is still queued. A send knob sends **both** `SetPerformanceParam` (the live value) and `SetSendLevels` (persists the initial level and marks the Live Set dirty) and **shows the live value** (so a CC or encoder move shows).
  The knob shows the new value at once (pending) and then **what the unit holds**: a performance edit is confirmed by reading the live values back (`GetPerformanceValues`, float32 noise rounded); a read-back that differs is read
  again after about 150 ms and 500 ms before it is believed, and the knob then shows what the unit holds with a notice ("did not apply the change"); a structural edit is confirmed by the next State (if none arrives in 0.8 s the State's value is shown, with a notice when it differs). **Under `engine-host --no-audio` there is no render thread, so every performance edit is
  queued and never applied: the knob snaps back after the re-reads and says so.** A typed error from the unit (or a transport failure) puts the knob back, reads the live values again, and shows the unit's message next to the knob
  and in the live region. Performance edits are not saved: the Write flow is Story 9.12 (Write to Velora stays disabled, with its text updated); the stale-revision handling and the live-edit Footer are Story 9.10 (below).
  Frame headroom: a `State` or `PerformanceValues` at 16 parts carries about 190 part parameters x 16 parts of `ParamValue` (about 30-35 KB estimated, not measured) against the 64 KiB ws.v1 frame, so the margin is
  real but not large; measure it before adding per-part fields.
- **Live edit state, stale edits, cable pulled (Story 9.10).** The Footer follows `State.edits` (`src/live_edit_state.js`): `● LIVE EDIT ON · CHANGES PLAY ON THE VELORA` while edits are in the unit's memory or Studio
  edits arrived within 3 s, plus `UNSAVED: SCENE n` (the Write Scene scope is available) or `UNSAVED: LIVE SET` (a timbre change, or no scene to write to). The editor keeps the Live Set revision it was **loaded at**
  (first State about the shown Live Set, or after Reload) and sends it as `base_revision`; Studio's own edits never move it, anything done on the unit does. A refused edit (`STALE_REVISION`) drops every optimistic
  value, stops queued edits, makes the knobs read-only and shows "Changed on the Velora since you opened it · Reload"; Reload reads the State again (and the Live Set view), takes its revision as the new base. A link
  that drops resets the editor (a late answer from the dead link is ignored); the unit keeps the applied edits with the diamond until Write or power-off, and after reconnect the Live Set view reads them back as unsaved.
  Not detected: a second Studio computer editing the same Live Set (it also carries `base_revision`); the banner appears on the first refused edit, not when the unit changes. The device Status Bar shows `STUDIO` while
  edits arrive (Performance view; 3 s idle expiry in the engine).
- **Only the Live Set that sounds is editable.** The engine has one live graph, so a Live Set that is shown but not playing (or an engine this firmware does not have) is read-only with a banner and a reason on every knob.
  A Pro unit's 16 parts: the editor edits whichever part is selected, the strips show the first 8 (the existing 16-part note). **ASSIGN (A4):** a part-scope parameter assigned to K1..K8 lands on `State.encoders.selected_part` (the unit's
  selected part; `SelectPart` is denied over the bridge), not necessarily on the part being edited here.
- **Deferred (no engine command yet; shown read-only with the reason "The Velora cannot take this edit from Studio yet", listed in `model.deferred`):** the arpeggiator (`arp_on` .. `arp_latch`; the State does not carry
  their values, so they read "—"), low key, high key, keyboard on/off, octave and transpose (low / high key and keyboard on/off are shown from `PartState`; octave and transpose are not in the State, placeholders use the unit's
  ranges: transpose -12..12, octave -3..3). The minimal additive commands that would make them editable: `SetPartZone { part, optional low_key, high_key, octave, transpose, keyboard_on }` and `SetPartArp { part, arp_* }` as
  structural commands, with `PartState` gaining `octave`, `transpose` and the arpeggiator fields. The timbre (`timbre_ref`) is hidden (Story 9.11). Master-scope parameters (global reverb, delay, EQ, compressor, tempo) are not part
  parameters and are not in this editor.
- **Layout.** Sizes in rem, token colours only, the global 2 px focus ring. The sections are a CSS grid `repeat(auto-fit, minmax(min(100%, 15.1rem), 1fr))` (rem: it grows with the text size) plus one column when the editor is at most
  45em of its own font wide; `columnsFor` mirrors it and `tests/part_editor.test.js` computes it for several widths. Nothing relies on `overflow-x: hidden` (the knob grids are `auto-fill` columns of 7.5 rem and every grid item is
  `min-width: 0`). Measured in the browser pane (1400 px window, editor 1129 px wide, FM and organ parts) with the root font at 16 / 24 / 32 px: 4 / 2 / 1 columns and no element past the editor's right edge (`scrollWidth == clientWidth`); the 1280 px window (editor about 1024 px) was computed by `columnsFor`, not measured. Every string is a locale key in
  en / es / pt-BR (`studio.part_editor.*`, `studio.knob.*`, `studio.group.*`, `studio.param.*`, `studio.enum.*`; labels the registry already points at are the existing `fm.*` / `va.*` keys, now embedded in `src/catalogs.js`);
  `tests/part_editor_dom.test.js` checks that every key the modules use exists in all three and that no two sliders share an accessible name. Text that came from the unit (part names, error messages) is only ever `textContent`.
- **Code.** `src/part_editor.js` (pure: registry views, knob rules, value text and entry, the command table `commandKind` / `requests`, `INERT_BY_ENGINE`, `EditSender` (coalescing), `PartEditorController` (optimistic values,
  confirm, revert), `buildPartEditorModel`, `envelopePolyline`, `columnsFor`), `src/part_editor_dom.js` (`PartEditorView`: the DOM, in-place updates that keep focus, focus restored by stable id after a rebuild, no rebuild under a drag),
  wired in `src/app.js` (`partEditorCtl`, `renderRealPartDetails`). Tests: `cd desktop && node --test tests` (`part_editor.test.js` against `tests/support/fake_unit.js`, a stand-in for the engine's edit commands with typed errors,
  RT latency / no-render modelling (`rtDelay`, `noRender`), initial vs live send levels and per-engine audibility (`consumes`); `part_editor_dom.test.js` with a DOM shim with real focus loss on removal and pointer capture: ARIA,
  every key, drag, wheel, value entry, read-only, hostile strings, focus after rebuilds); `cargo test part_editor` (the page's JSON for every edit, with `base_revision`, maps onto the real `ClientMessage`);
  `ctest -R "control|sysd_bridge"` (the engine judges `base_revision`, STALE_REVISION, the revision bumps, STUDIO expiry; the bridge forwards it); `node --test tests/live_edit_state.test.js` (Footer scope, stale + Reload, races, cable pulled); `ctest -R engine_host_bridge_studio_smoke` (the real engine through the real bridge with the Rust client).
- **What the smoke does and does not prove.** It proves the commands the editor sends are accepted over the real bridge (a performance edit is acked and `GetPerformanceValues` lists the parameter; bad values are typed errors; the
  sends, volume and insert chain edits show up in the State). It does **not** prove an effect on the sound: the smoke engine runs `--no-audio` (no render thread), and the engines consume few parameters anyway (box above).
  That the RT plane applies a queued edit is covered only by the C++ control tests (`control_test`, `test_performance_path`).
- **Not verified.** The real Tauri webview, the light theme, a screen reader, a real unit, and the 1280 px window. Browser check without a unit: `python3 desktop/tests/support/serve_mock.py` serves `desktop/` with a paired fake unit
  (`tests/support/mock_device.js`) and `http://127.0.0.1:8765/src/index.html` shows the real Part Editor (select a part strip; `window.__mockUnit.log` lists what was sent).
