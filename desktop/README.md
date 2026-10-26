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
