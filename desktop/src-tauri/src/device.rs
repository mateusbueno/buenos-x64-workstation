// The Velora Studio device client (Foundation: Velora Studio device connection).
//
// A blocking TCP client of the sysd bridge (`sysd/README.md`, "TCP bridge") on its own worker thread. It speaks the same ws.v1 schema as the on-device
// clients, framed as the Unix sockets are (4-byte big-endian length + protobuf, largest frame 65536 bytes). Standard library networking only: nothing here
// is OS specific, so Studio keeps building for macOS, Windows and Linux.
//
// Lifecycle (one worker per `connect`):
//   connect (TCP) -> Hello {client_id, client_version, features} -> HelloAck {device info, negotiated features, paired}
//     paired   -> Subscribe (engine State pushes) and keep the session alive (a GetState every 20 s; the bridge closes idle connections; no frame for 50 s = dead peer)
//     unpaired -> if a pairing token is stored for THIS device id (HelloAck.device_id) it is sent in a second Hello on the same connection (a token is never sent
//                 to a device id it was not issued by); otherwise stay connected: `pair()` sends a PairRequest, the unit answers with a pairing token (256
//                 random bits, shown to nobody), which is stored per device id (<app data>/device_pairing.json, mode 0600) and presented in later Hellos
//   the unit drops / the bridge restarts -> status `disconnected`, reconnect with exponential backoff (50 ms doubling to 2 s), Hello again, Subscribe again
//   a protocol the unit does not speak (UNSUPPORTED_VERSION) -> status `version-mismatch` with the details; no retry storm, `connect` again to retry
//
// Status values (also the `device://status` event payload, `DeviceStatus.status`): "disconnected" | "connecting" | "unpaired" | "paired" |
// "version-mismatch". The latest engine State is kept (`latest_state()`) and every State message (reply or push) is emitted as `device://state`.
// The physical USB link is the trust boundary (no TLS): the pairing code is a physical-presence check, not cryptography.
use crate::ws_v1::{
    client_message::Body as CBody, library_client_message::Body as LBody, library_server_message::Body as LSBody, server_message::Body as SBody, ClientMessage, ErrorCode, Hello,
    LibraryClientMessage, LibraryRequest, LibraryServerMessage, ListInstruments, ListLibraries, ServerMessage,
};
use prost::Message;
use std::collections::HashMap;
use std::io::{ErrorKind, Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc, Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

pub const MAX_FRAME: usize = 65536;
/// Rows asked per Library API page (a reply must fit one 65536-byte frame; the librarian also keeps its own reply budget and says `more`).
const LIBRARY_PAGE: u32 = 200;
/// What the timbre picker takes from a unit at most (a ceiling for the list, not a library limit: `truncated` tells the page).
pub const MAX_PICKER_TIMBRES: usize = 5000;
const MAX_PICKER_LIBRARIES: usize = 1000;

enum LibFail {
    TooLarge,
    Gone,
    Other(String),
}
pub const PROTOCOL_VERSION: u32 = 1;
pub const FEATURES: [&str; 6] = ["params.v1", "snapshots.v1", "subscribe.v1", "liveset.v1", "config.v1", "library.v1"];
const BACKOFF_START: Duration = Duration::from_millis(50);
const BACKOFF_MAX: Duration = Duration::from_millis(2000);
const KEEPALIVE: Duration = Duration::from_secs(20);
/// No frame at all from the unit for this long (more than twice the keepalive, which the bridge answers) means a dead peer: the connection is dropped.
const DEAD_AFTER: Duration = Duration::from_secs(50);

#[derive(serde::Serialize, Clone, Copy, Debug, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum LinkStatus {
    Disconnected,
    Connecting,
    Unpaired,
    Paired,
    VersionMismatch,
}

impl LinkStatus {
    pub fn as_str(&self) -> &'static str {
        match self {
            LinkStatus::Disconnected => "disconnected",
            LinkStatus::Connecting => "connecting",
            LinkStatus::Unpaired => "unpaired",
            LinkStatus::Paired => "paired",
            LinkStatus::VersionMismatch => "version-mismatch",
        }
    }
}

#[derive(serde::Serialize, Clone, Debug, PartialEq)]
pub struct DeviceStatus {
    pub status: LinkStatus,
    /// A device address was given (`connect`): the UI follows this status instead of the offline simulation.
    pub configured: bool,
    pub host: String,
    pub port: u16,
    pub device_id: Option<String>,
    pub model: Option<String>,
    pub firmware_version: Option<String>,
    pub device_version: Option<String>,
    pub features: Vec<String>,
    /// Why the link is in this status when that is useful (version mismatch details, last error).
    pub detail: Option<String>,
    /// Set when the computer id or a pairing token could not be saved (the next launch would have to pair again). Never contains a secret.
    pub persist_error: Option<String>,
}

impl DeviceStatus {
    fn new() -> Self {
        DeviceStatus {
            status: LinkStatus::Disconnected,
            configured: false,
            host: String::new(),
            port: 0,
            device_id: None,
            model: None,
            firmware_version: None,
            device_version: None,
            features: vec![],
            detail: None,
            persist_error: None,
        }
    }
}

/// Receives `("device://status", DeviceStatus as JSON)` and `("device://state", State as JSON)`.
pub type EventSink = Arc<dyn Fn(&str, serde_json::Value) + Send + Sync>;

type Reply = Result<ServerMessage, String>;

enum Cmd {
    Request { msg: ClientMessage, reply: mpsc::Sender<Reply> },
}

struct Shared {
    status: Mutex<DeviceStatus>,
    state: Mutex<Option<serde_json::Value>>,
    sink: EventSink,
    client_id: Mutex<String>,
    client_version: String,
    token_dir: Mutex<Option<std::path::PathBuf>>,
}

impl Shared {
    fn set_status(&self, f: impl FnOnce(&mut DeviceStatus)) {
        let snapshot = {
            let mut s = self.status.lock().unwrap();
            let before = s.clone();
            f(&mut s);
            if *s == before {
                return;
            }
            s.clone()
        };
        (self.sink)("device://status", serde_json::to_value(&snapshot).unwrap_or(serde_json::Value::Null));
    }

    fn set_link(&self, link: LinkStatus, detail: Option<String>) {
        self.set_status(|s| {
            s.status = link;
            s.detail = detail;
            if link == LinkStatus::Disconnected || link == LinkStatus::Connecting || link == LinkStatus::VersionMismatch {
                s.features.clear();
            }
        });
    }

    fn publish_state(&self, msg: &ServerMessage) {
        if let Some(SBody::State(st)) = &msg.body {
            if let Ok(v) = serde_json::to_value(st) {
                *self.state.lock().unwrap() = Some(v.clone());
                (self.sink)("device://state", v);
            }
        }
    }
}

struct Worker {
    stop: Arc<AtomicBool>,
    handle: JoinHandle<()>,
    tx: mpsc::Sender<Cmd>,
}

pub struct DeviceClient {
    shared: Arc<Shared>,
    worker: Mutex<Option<Worker>>,
}

impl DeviceClient {
    pub fn new(client_id: String, client_version: String, sink: EventSink) -> Self {
        DeviceClient {
            shared: Arc::new(Shared { status: Mutex::new(DeviceStatus::new()), state: Mutex::new(None), sink, client_id: Mutex::new(client_id), client_version, token_dir: Mutex::new(None) }),
            worker: Mutex::new(None),
        }
    }

    /// Replaces the computer id used in the next Hello (the app data dir is only known once Tauri has started).
    pub fn set_client_id(&self, id: String) {
        *self.shared.client_id.lock().unwrap() = id;
    }

    /// Where pairing tokens are kept (`<dir>/device_pairing.json`). Without it a token lives only as long as this run.
    pub fn set_token_dir(&self, dir: std::path::PathBuf) {
        *self.shared.token_dir.lock().unwrap() = Some(dir);
    }

    pub fn set_persist_error(&self, why: Option<String>) {
        self.shared.set_status(|s| s.persist_error = why);
    }

    pub fn status(&self) -> DeviceStatus {
        self.shared.status.lock().unwrap().clone()
    }

    pub fn latest_state(&self) -> Option<serde_json::Value> {
        self.shared.state.lock().unwrap().clone()
    }

    /// Starts (or restarts) the worker toward host:port. Returns at once; follow `device://status`.
    pub fn connect(&self, host: &str, port: u16) {
        self.stop_worker();
        *self.shared.state.lock().unwrap() = None;
        self.shared.set_status(|s| {
            let keep = s.persist_error.take();
            *s = DeviceStatus::new();
            s.persist_error = keep;
            s.configured = true;
            s.host = host.to_string();
            s.port = port;
        });
        let stop = Arc::new(AtomicBool::new(false));
        let (tx, rx) = mpsc::channel();
        let shared = self.shared.clone();
        let stop2 = stop.clone();
        let host = host.to_string();
        let handle = std::thread::Builder::new()
            .name("velora-device".into())
            .spawn(move || run_worker(shared, stop2, rx, host, port))
            .expect("spawn device worker");
        *self.worker.lock().unwrap() = Some(Worker { stop, handle, tx });
    }

    /// Stops the worker and leaves the status `disconnected` (still `configured`).
    pub fn disconnect(&self) {
        self.stop_worker();
        self.shared.set_link(LinkStatus::Disconnected, None);
    }

    fn stop_worker(&self) {
        let w = self.worker.lock().unwrap().take();
        if let Some(w) = w {
            w.stop.store(true, Ordering::SeqCst);
            drop(w.tx);
            let _ = w.handle.join();
        }
    }

    /// Sends one ws.v1 command and waits for its reply (`request_id` is assigned here and restored on the reply: the caller's own id, if any, is kept).
    /// A typed error from the unit or the bridge is an `Ok(ServerMessage)` whose body is `Error`; `Err` is a transport problem (not connected, timeout).
    pub fn request(&self, msg: ClientMessage, timeout: Duration) -> Reply {
        let tx = {
            let g = self.worker.lock().unwrap();
            match g.as_ref() {
                Some(w) => w.tx.clone(),
                None => return Err("not connected".to_string()),
            }
        };
        let (rtx, rrx) = mpsc::channel();
        tx.send(Cmd::Request { msg, reply: rtx }).map_err(|_| "not connected".to_string())?;
        match rrx.recv_timeout(timeout) {
            Ok(r) => r,
            Err(mpsc::RecvTimeoutError::Timeout) => Err("timeout".to_string()),
            Err(mpsc::RecvTimeoutError::Disconnected) => Err("not connected".to_string()),
        }
    }

    /// One Library API request through the bridge's library allow-list (`LibraryRequest{payload}`), read-only calls only. Typed failures are told apart because the timbre
    /// list reacts to two of them: a reply too big for one ws.v1 frame (ask for fewer rows) and a library that is gone (skip it).
    fn library_call(&self, body: LBody) -> Result<LSBody, LibFail> {
        let inner = LibraryClientMessage { request_id: 1, body: Some(body) }; // the bridge maps the inner id to its own and back
        let msg = ClientMessage { request_id: 0, body: Some(CBody::Library(LibraryRequest { payload: inner.encode_to_vec() })), ..Default::default() };
        let reply = self.request(msg, Duration::from_secs(10)).map_err(LibFail::Other)?;
        match reply.body {
            Some(SBody::LibraryReply(r)) => {
                let m = LibraryServerMessage::decode(&r.payload[..]).map_err(|e| LibFail::Other(format!("the library reply does not parse: {}", e)))?;
                match m.body {
                    Some(LSBody::Error(e)) => {
                        if e.code == crate::ws_v1::LibraryErrorCode::UnknownLibrary as i32 {
                            Err(LibFail::Gone)
                        } else {
                            let reason = e.details.get("reason").cloned().unwrap_or_default();
                            Err(LibFail::Other(format!("library error {}{}", e.code, if reason.is_empty() { String::new() } else { format!(" ({})", reason) })))
                        }
                    }
                    Some(b) => Ok(b),
                    None => Err(LibFail::Other("the library reply is empty".to_string())),
                }
            }
            Some(SBody::Error(e)) => {
                if e.details.get("reason").map(|r| r == "library_reply_too_large").unwrap_or(false) {
                    Err(LibFail::TooLarge)
                } else {
                    Err(LibFail::Other(describe_error(&e)))
                }
            }
            _ => Err(LibFail::Other("unexpected reply to a library request".to_string())),
        }
    }

    /// Story 9.11: every timbre installed on the unit, for the Part Editor's "+ Add timbre" picker: the libraries (ListLibraries) and each library's instruments
    /// (ListInstruments), paged, as `{ timbres: [{library_id, library_name, instrument_key, name, category}], libraries, total, truncated }`. Read-only (nothing is
    /// assigned, no revision moves). At most `max_timbres` are returned (`truncated` says more exist); a page that does not fit one frame is asked again with half the rows; a library
    /// that vanishes between the two calls is skipped; a failure of any other kind is the error (never a partial list presented as complete).
    pub fn list_timbres(&self, max_timbres: usize) -> Result<serde_json::Value, String> {
        let mut libraries: Vec<crate::ws_v1::LibraryInfo> = Vec::new();
        let mut offset = 0u32;
        let mut limit = LIBRARY_PAGE;
        loop {
            match self.library_call(LBody::ListLibraries(ListLibraries { offset, limit })) {
                Ok(LSBody::LibraryList(l)) => {
                    let n = l.libraries.len() as u32;
                    libraries.extend(l.libraries);
                    if !l.more || n == 0 || libraries.len() >= MAX_PICKER_LIBRARIES {
                        break;
                    }
                    offset += n;
                }
                Ok(_) => return Err("unexpected reply to ListLibraries".to_string()),
                Err(LibFail::TooLarge) if limit > 1 => limit = (limit / 2).max(1),
                Err(LibFail::TooLarge) => return Err("a library entry does not fit one frame".to_string()),
                Err(LibFail::Gone) => return Err("the library list is not available".to_string()),
                Err(LibFail::Other(e)) => return Err(e),
            }
        }
        let mut timbres: Vec<serde_json::Value> = Vec::new();
        let mut total: u64 = 0;
        let mut truncated = false;
        'libs: for lib in libraries.iter() {
            if lib.instrument_count == 0 {
                continue;
            }
            let mut offset = 0u32;
            let mut limit = LIBRARY_PAGE;
            loop {
                match self.library_call(LBody::ListInstruments(ListInstruments { library_id: lib.library_id.clone(), offset, limit })) {
                    Ok(LSBody::InstrumentList(l)) => {
                        if l.library_id != lib.library_id {
                            return Err("the instrument list is for another library".to_string()); // never attribute a timbre to the wrong library
                        }
                        total += l.instruments.len() as u64;
                        let n = l.instruments.len() as u32;
                        for i in l.instruments {
                            if timbres.len() >= max_timbres {
                                truncated = true;
                                break 'libs;
                            }
                            timbres.push(serde_json::json!({
                                "library_id": lib.library_id,
                                "library_name": lib.name,
                                "instrument_key": i.instrument_key,
                                "name": i.name,
                                "category": i.category,
                            }));
                        }
                        if !l.more || n == 0 {
                            break;
                        }
                        offset += n;
                    }
                    Ok(_) => return Err("unexpected reply to ListInstruments".to_string()),
                    Err(LibFail::TooLarge) if limit > 1 => limit = (limit / 2).max(1),
                    Err(LibFail::TooLarge) => return Err("an instrument entry does not fit one frame".to_string()),
                    Err(LibFail::Gone) => break, // removed between the two calls: nothing to offer from it
                    Err(LibFail::Other(e)) => return Err(e),
                }
            }
        }
        Ok(serde_json::json!({ "timbres": timbres, "libraries": libraries.len(), "total": total, "truncated": truncated }))
    }

    /// PairRequest with the code the unit shows. Ok when paired (the client then subscribes by itself); Err with the typed code otherwise
    /// ("PAIRING_CODE_INVALID (4 attempts left)", "PAIRING_CLOSED (expired)", ...).
    pub fn pair(&self, code: &str, computer_name: &str) -> Result<(), String> {
        let msg = ClientMessage {
            request_id: 0,
            body: Some(CBody::PairRequest(crate::ws_v1::PairRequest { code: code.to_string(), computer_name: computer_name.to_string() })),
            ..Default::default()
        };
        let reply = self.request(msg, Duration::from_secs(5))?;
        match reply.body {
            Some(SBody::PairReply(r)) if r.paired => Ok(()),
            Some(SBody::Error(e)) => Err(describe_error(&e)),
            _ => Err("unexpected reply to PairRequest".to_string()),
        }
    }
}

impl Drop for DeviceClient {
    fn drop(&mut self) {
        self.stop_worker();
    }
}

pub fn describe_error(e: &crate::ws_v1::Error) -> String {
    let code = ErrorCode::try_from(e.code).map(|c| c.as_str_name().trim_start_matches("ERROR_CODE_").to_string()).unwrap_or_else(|_| format!("CODE_{}", e.code));
    let mut extra: Vec<String> = e.details.iter().map(|(k, v)| format!("{}={}", k, v)).collect();
    extra.sort();
    if extra.is_empty() {
        code
    } else {
        format!("{} ({})", code, extra.join(", "))
    }
}

/// 32 random bytes (or `n`) from the OS CSPRNG as lowercase hex. Fails closed: there is no fallback generator.
pub fn random_hex(n: usize) -> Result<String, String> {
    let mut b = vec![0u8; n];
    getrandom::fill(&mut b).map_err(|e| format!("the OS random source is unavailable: {}", e))?;
    Ok(b.iter().map(|x| format!("{:02x}", x)).collect())
}

/// A fresh random computer id, for a run that has no place to keep one.
pub fn random_client_id() -> Result<String, String> {
    Ok(format!("studio-{}", random_hex(16)?))
}

fn valid_client_id(s: &str) -> bool {
    !s.is_empty() && s.len() <= 64 && s.chars().all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_' || c == '.')
}

#[cfg(unix)]
fn write_private(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    use std::os::unix::fs::OpenOptionsExt;
    // Written beside the final name and renamed, with mode 0600 from the first byte.
    let tmp = path.with_extension("tmp");
    let _ = std::fs::remove_file(&tmp);
    {
        let mut f = std::fs::OpenOptions::new().write(true).create_new(true).mode(0o600).open(&tmp)?;
        f.write_all(bytes)?;
        f.sync_all()?;
    }
    std::fs::rename(&tmp, path)
}

#[cfg(not(unix))]
fn write_private(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    // Windows: the file lives under the user's own app data directory, whose ACL is already the user's.
    let tmp = path.with_extension("tmp");
    std::fs::write(&tmp, bytes)?;
    std::fs::rename(&tmp, path)
}

/// The stable id of this computer, kept in `<dir>/device_client_id` (created on first use, mode 0600). Err when it could not be created or saved: the caller
/// then runs with `random_client_id()` and reports the error (so a failing disk is visible, not a silently new id at every launch).
pub fn load_or_create_client_id(dir: &Path) -> Result<String, String> {
    let path = dir.join("device_client_id");
    match std::fs::read_to_string(&path) {
        Ok(s) if valid_client_id(s.trim()) => return Ok(s.trim().to_string()),
        Ok(_) => {} // unreadable content: replaced below
        Err(e) if e.kind() == ErrorKind::NotFound => {}
        Err(e) => return Err(format!("cannot read the computer id: {}", e)),
    }
    let id = random_client_id()?;
    std::fs::create_dir_all(dir).map_err(|e| format!("cannot create the app data folder: {}", e))?;
    write_private(&path, id.as_bytes()).map_err(|e| format!("cannot save the computer id: {}", e))?;
    Ok(id)
}

fn token_path(dir: &Path) -> std::path::PathBuf {
    dir.join("device_pairing.json")
}

/// The pairing token issued by the unit with this device id, if any. A token is never offered to another device id.
pub fn load_token(dir: &Path, device_id: &str) -> Option<String> {
    let v: serde_json::Value = serde_json::from_slice(&std::fs::read(token_path(dir)).ok()?).ok()?;
    let t = v.get(device_id)?.as_str()?;
    if t.len() == 64 && t.chars().all(|c| c.is_ascii_hexdigit()) {
        Some(t.to_string())
    } else {
        None
    }
}

pub fn save_token(dir: &Path, device_id: &str, token: &str) -> Result<(), String> {
    let mut map: serde_json::Map<String, serde_json::Value> = std::fs::read(token_path(dir))
        .ok()
        .and_then(|b| serde_json::from_slice::<serde_json::Value>(&b).ok())
        .and_then(|v| v.as_object().cloned())
        .unwrap_or_default();
    map.insert(device_id.to_string(), serde_json::Value::String(token.to_string()));
    std::fs::create_dir_all(dir).map_err(|e| format!("cannot create the app data folder: {}", e))?;
    let bytes = serde_json::to_vec(&serde_json::Value::Object(map)).map_err(|e| e.to_string())?;
    write_private(&token_path(dir), &bytes).map_err(|e| format!("cannot save the pairing token: {}", e))
}

// ---- framing -------------------------------------------------------------------------------------------------------------------------------------

fn write_frame(stream: &mut TcpStream, msg: &ClientMessage) -> std::io::Result<()> {
    let body = msg.encode_to_vec();
    if body.len() > MAX_FRAME {
        return Err(std::io::Error::new(ErrorKind::InvalidInput, "message larger than one ws.v1 frame"));
    }
    let mut frame = Vec::with_capacity(4 + body.len());
    frame.extend_from_slice(&(body.len() as u32).to_be_bytes());
    frame.extend_from_slice(&body);
    stream.write_all(&frame)
}

struct FrameReader {
    buf: Vec<u8>,
}

impl FrameReader {
    fn new() -> Self {
        FrameReader { buf: Vec::new() }
    }

    fn extract(&mut self) -> std::io::Result<Option<Vec<u8>>> {
        if self.buf.len() < 4 {
            return Ok(None);
        }
        let len = u32::from_be_bytes([self.buf[0], self.buf[1], self.buf[2], self.buf[3]]) as usize;
        if len > MAX_FRAME {
            return Err(std::io::Error::new(ErrorKind::InvalidData, "frame above 65536 bytes"));
        }
        if self.buf.len() < 4 + len {
            return Ok(None);
        }
        let frame = self.buf[4..4 + len].to_vec();
        self.buf.drain(..4 + len);
        Ok(Some(frame))
    }

    /// One read attempt bounded by the stream's read timeout. Ok(None): nothing complete yet.
    fn poll(&mut self, stream: &mut TcpStream) -> std::io::Result<Option<ServerMessage>> {
        loop {
            if let Some(f) = self.extract()? {
                return ServerMessage::decode(f.as_slice()).map(Some).map_err(|e| std::io::Error::new(ErrorKind::InvalidData, e.to_string()));
            }
            let mut chunk = [0u8; 16384];
            match stream.read(&mut chunk) {
                Ok(0) => return Err(std::io::Error::new(ErrorKind::UnexpectedEof, "connection closed")),
                Ok(n) => self.buf.extend_from_slice(&chunk[..n]),
                Err(e) if e.kind() == ErrorKind::WouldBlock || e.kind() == ErrorKind::TimedOut => return Ok(None),
                Err(e) if e.kind() == ErrorKind::Interrupted => continue,
                Err(e) => return Err(e),
            }
        }
    }
}

// ---- worker --------------------------------------------------------------------------------------------------------------------------------------

enum End {
    Stopped,
    /// The link dropped; the bool is true when a HelloAck had been received (the backoff only resets then).
    Lost(String, bool),
    Mismatch,
}

fn run_worker(shared: Arc<Shared>, stop: Arc<AtomicBool>, rx: mpsc::Receiver<Cmd>, host: String, port: u16) {
    let mut backoff = BACKOFF_START;
    let mut announce_connecting = true;
    while !stop.load(Ordering::SeqCst) {
        if announce_connecting {
            shared.set_link(LinkStatus::Connecting, None);
        }
        let stream = connect_tcp(&host, port);
        match stream {
            Ok(s) => match run_session(&shared, &stop, &rx, s) {
                End::Stopped => break,
                End::Mismatch => {
                    // Nothing to retry until the user connects again (or updates): answer queued requests and wait for the stop.
                    while !stop.load(Ordering::SeqCst) {
                        refuse_requests(&rx, "version mismatch");
                        std::thread::sleep(Duration::from_millis(20));
                    }
                    return;
                }
                End::Lost(why, established) => {
                    shared.set_link(LinkStatus::Disconnected, Some(why));
                    if established {
                        backoff = BACKOFF_START; // only a link that got as far as a HelloAck counts as working
                    }
                    announce_connecting = true;
                }
            },
            Err(e) => {
                shared.set_link(LinkStatus::Disconnected, Some(e));
                announce_connecting = false; // repeated failures stay "disconnected" instead of flickering
            }
        }
        // Backoff, answering requests that arrive meanwhile.
        let until = Instant::now() + jitter(backoff);
        while Instant::now() < until && !stop.load(Ordering::SeqCst) {
            refuse_requests(&rx, "not connected");
            std::thread::sleep(Duration::from_millis(10));
        }
        backoff = (backoff * 2).min(BACKOFF_MAX);
    }
    shared.set_link(LinkStatus::Disconnected, None);
}

/// 50%..100% of `d`, so a fleet of Studios does not retry in lockstep (not security relevant: a time-seeded hash is enough).
fn jitter(d: Duration) -> Duration {
    use std::collections::hash_map::RandomState;
    use std::hash::{BuildHasher, Hasher};
    let mut h = RandomState::new().build_hasher();
    h.write_u64(1);
    let f = 0.5 + (h.finish() % 1000) as f64 / 2000.0;
    d.mul_f64(f)
}

fn refuse_requests(rx: &mpsc::Receiver<Cmd>, why: &str) {
    while let Ok(Cmd::Request { reply, .. }) = rx.try_recv() {
        let _ = reply.send(Err(why.to_string()));
    }
}

fn connect_tcp(host: &str, port: u16) -> Result<TcpStream, String> {
    let addrs = (host, port).to_socket_addrs().map_err(|e| format!("cannot resolve {}:{}: {}", host, port, e))?;
    let mut last = format!("no address for {}:{}", host, port);
    for a in addrs {
        match TcpStream::connect_timeout(&a, Duration::from_millis(1500)) {
            Ok(s) => return Ok(s),
            Err(e) => last = format!("cannot connect to {}: {}", a, e),
        }
    }
    Err(last)
}

/// Reads frames until a HelloAck / Error, bounded to 4 s.
fn read_hello_reply(stop: &AtomicBool, reader: &mut FrameReader, stream: &mut TcpStream) -> Result<ServerMessage, End> {
    let deadline = Instant::now() + Duration::from_secs(4);
    loop {
        if stop.load(Ordering::SeqCst) {
            return Err(End::Stopped);
        }
        if Instant::now() > deadline {
            return Err(End::Lost("no HelloAck".to_string(), false));
        }
        match reader.poll(stream) {
            Ok(Some(m)) => return Ok(m),
            Ok(None) => continue,
            Err(e) => return Err(End::Lost(format!("hello: {}", e), false)),
        }
    }
}

fn hello_message(shared: &Shared, request_id: u64, token: &str) -> ClientMessage {
    ClientMessage {
        request_id,
        body: Some(CBody::Hello(Hello {
            version: PROTOCOL_VERSION,
            features: FEATURES.iter().map(|s| s.to_string()).collect(),
            client_name: "Velora Studio".to_string(),
            client_version: shared.client_version.clone(),
            client_id: shared.client_id.lock().unwrap().clone(),
            pairing_token: token.to_string(),
        })),
        ..Default::default()
    }
}

fn run_session(shared: &Arc<Shared>, stop: &Arc<AtomicBool>, rx: &mpsc::Receiver<Cmd>, mut stream: TcpStream) -> End {
    let _ = stream.set_nodelay(true);
    let _ = stream.set_read_timeout(Some(Duration::from_millis(20)));
    let _ = stream.set_write_timeout(Some(Duration::from_secs(3)));
    // The first Hello carries NO token: the token is only ever offered to the device id that issued it, and that id is in the HelloAck.
    if let Err(e) = write_frame(&mut stream, &hello_message(shared, 1, "")) {
        return End::Lost(format!("hello: {}", e), false);
    }
    let mut reader = FrameReader::new();
    let mut ack = match read_hello_reply(stop, &mut reader, &mut stream) {
        Ok(m) => m,
        Err(e) => return e,
    };
    let mut paired;
    let mut tried_token = false;
    loop {
        match ack.body {
            Some(SBody::HelloAck(a)) => {
                if a.version != PROTOCOL_VERSION {
                    shared.set_link(LinkStatus::VersionMismatch, Some(format!("the unit answered protocol {}, Studio speaks {}", a.version, PROTOCOL_VERSION)));
                    return End::Mismatch;
                }
                paired = a.paired;
                shared.set_status(|s| {
                    s.device_id = Some(a.device_id.clone()).filter(|v| !v.is_empty());
                    s.model = Some(a.model.clone()).filter(|v| !v.is_empty());
                    s.firmware_version = Some(a.firmware_version.clone()).filter(|v| !v.is_empty());
                    s.device_version = Some(a.device_version.clone()).filter(|v| !v.is_empty());
                    s.features = a.features.clone();
                    s.status = if a.paired { LinkStatus::Paired } else { LinkStatus::Unpaired };
                    s.detail = None;
                });
                if !paired && !tried_token {
                    // A token stored for THIS device id proves the pairing: present it in a second Hello (the bridge allows exactly one).
                    let dir = shared.token_dir.lock().unwrap().clone();
                    if let (Some(dir), false) = (dir, a.device_id.is_empty()) {
                        if let Some(tok) = load_token(&dir, &a.device_id) {
                            tried_token = true;
                            if let Err(e) = write_frame(&mut stream, &hello_message(shared, 2, &tok)) {
                                return End::Lost(format!("hello: {}", e), true);
                            }
                            ack = match read_hello_reply(stop, &mut reader, &mut stream) {
                                Ok(m) => m,
                                Err(End::Lost(w, _)) => return End::Lost(w, true),
                                Err(e) => return e,
                            };
                            continue;
                        }
                    }
                }
                break;
            }
            Some(SBody::Error(e)) if e.code == ErrorCode::UnsupportedVersion as i32 => {
                shared.set_link(LinkStatus::VersionMismatch, Some(describe_error(&e)));
                return End::Mismatch;
            }
            Some(SBody::Error(e)) => return End::Lost(describe_error(&e), false),
            _ => return End::Lost("the first reply was not a HelloAck".to_string(), false),
        }
    }

    let mut next_id: u64 = 3;
    let mut pending: HashMap<u64, Option<mpsc::Sender<Reply>>> = HashMap::new();
    let send_internal = |stream: &mut TcpStream, pending: &mut HashMap<u64, Option<mpsc::Sender<Reply>>>, next_id: &mut u64, body: CBody| -> std::io::Result<()> {
        let id = *next_id;
        *next_id += 1;
        pending.insert(id, None);
        write_frame(stream, &ClientMessage { request_id: id, body: Some(body), ..Default::default() })
    };
    if paired {
        if let Err(e) = send_internal(&mut stream, &mut pending, &mut next_id, CBody::Subscribe(crate::ws_v1::Subscribe { enable: true })) {
            return End::Lost(format!("subscribe: {}", e), true);
        }
    }
    let mut last_sent = Instant::now();
    let mut last_frame = Instant::now();
    loop {
        if stop.load(Ordering::SeqCst) {
            fail_pending(&mut pending, "disconnected");
            return End::Stopped;
        }
        // Requests from the application.
        while let Ok(Cmd::Request { mut msg, reply }) = rx.try_recv() {
            let id = next_id;
            next_id += 1;
            msg.request_id = id;
            match write_frame(&mut stream, &msg) {
                Ok(()) => {
                    pending.insert(id, Some(reply));
                    last_sent = Instant::now();
                }
                Err(e) => {
                    let _ = reply.send(Err(format!("send failed: {}", e)));
                    fail_pending(&mut pending, "connection lost");
                    return End::Lost(format!("send: {}", e), true);
                }
            }
        }
        match reader.poll(&mut stream) {
            Ok(Some(mut m)) => {
                last_frame = Instant::now();
                shared.publish_state(&m);
                if m.request_id != 0 {
                    if let Some(slot) = pending.remove(&m.request_id) {
                        if let Some(SBody::PairReply(p)) = &mut m.body {
                            // The pairing token is a secret: keep it (per device id), and never hand it to the application or a log.
                            let token = std::mem::take(&mut p.pairing_token);
                            if p.paired && !token.is_empty() {
                                let dev = shared.status.lock().unwrap().device_id.clone();
                                let dir = shared.token_dir.lock().unwrap().clone();
                                let saved = match (dir, dev) {
                                    (Some(dir), Some(dev)) => save_token(&dir, &dev, &token),
                                    _ => Err("no app data folder: the pairing lasts until Studio closes".to_string()),
                                };
                                shared.set_status(|s| s.persist_error = saved.err());
                            }
                            if p.paired && !paired {
                                paired = true;
                                shared.set_status(|s| {
                                    s.status = LinkStatus::Paired;
                                    s.detail = None;
                                });
                                if let Err(e) = send_internal(&mut stream, &mut pending, &mut next_id, CBody::Subscribe(crate::ws_v1::Subscribe { enable: true })) {
                                    fail_pending(&mut pending, "connection lost");
                                    return End::Lost(format!("subscribe: {}", e), true);
                                }
                            }
                        }
                        if let Some(tx) = slot {
                            let _ = tx.send(Ok(m));
                        }
                    }
                } else if let Some(SBody::Error(e)) = &m.body {
                    // A pushed error (request_id 0): the unit side dropped; the connection closes right after and the reconnect re-subscribes.
                    shared.set_status(|s| s.detail = Some(describe_error(e)));
                }
            }
            Ok(None) => {}
            Err(e) => {
                fail_pending(&mut pending, "connection lost");
                return End::Lost(e.to_string(), true);
            }
        }
        if last_frame.elapsed() >= DEAD_AFTER {
            fail_pending(&mut pending, "connection lost");
            return End::Lost("the unit stopped answering".to_string(), true);
        }
        if last_sent.elapsed() >= KEEPALIVE {
            // Keeps the bridge's idle timeout away (and proves the peer is alive); the State reply refreshes the stored state.
            if let Err(e) = send_internal(&mut stream, &mut pending, &mut next_id, CBody::GetState(crate::ws_v1::GetState {})) {
                fail_pending(&mut pending, "connection lost");
                return End::Lost(format!("keepalive: {}", e), true);
            }
            last_sent = Instant::now();
        }
    }
}

fn fail_pending(pending: &mut HashMap<u64, Option<mpsc::Sender<Reply>>>, why: &str) {
    for (_, slot) in pending.drain() {
        if let Some(tx) = slot {
            let _ = tx.send(Err(why.to_string()));
        }
    }
}

// ---- tests ---------------------------------------------------------------------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ws_v1::{Ack, Error as WsError, HelloAck, PairReply, State};
    use std::net::TcpListener;
    use std::sync::atomic::AtomicUsize;

    fn token_for(id: &str) -> String {
        let mut t = String::new();
        while t.len() < 64 {
            t.push_str(&format!("{:02x}", (id.len() * 7 + t.len()) as u8));
        }
        t
    }

    /// A fake bridge: Hello / pairing / GetState / Subscribe like sysd's, on 127.0.0.1.
    #[allow(dead_code)]
    struct FakeBridge {
        port: u16,
        stop: Arc<AtomicBool>,
        conns: Arc<Mutex<Vec<TcpStream>>>,
        hellos: Arc<AtomicUsize>,
        subscribes: Arc<AtomicUsize>,
        tokens: Arc<Mutex<HashMap<String, String>>>,
        hello_log: Arc<Mutex<Vec<String>>>,
        mode: Arc<Mutex<Mode>>,
        thread: Option<JoinHandle<()>>,
    }

    #[derive(Clone, Copy, PartialEq)]
    enum Mode {
        Normal,
        UnsupportedVersion,
        WrongProtocolInAck,
    }

    fn send_server(s: &mut TcpStream, m: &ServerMessage) {
        let body = m.encode_to_vec();
        let mut f = (body.len() as u32).to_be_bytes().to_vec();
        f.extend_from_slice(&body);
        let _ = s.write_all(&f);
    }

    fn state_msg(rev: u64, scene: i32) -> ServerMessage {
        ServerMessage { request_id: 0, revision: rev, body: Some(SBody::State(State { revision: rev, current_scene: scene, ..Default::default() })) }
    }

    impl FakeBridge {
        fn start(port: u16, preset_paired: &[&str], mode: Mode) -> FakeBridge {
            Self::start_dev(port, preset_paired, mode, "velora-test")
        }

        /// `preset_paired` computers hold the token `token_for(id)`.
        fn start_dev(port: u16, preset_paired: &[&str], mode: Mode, device_id: &str) -> FakeBridge {
            let listener = TcpListener::bind(("127.0.0.1", port)).expect("bind");
            let port = listener.local_addr().unwrap().port();
            listener.set_nonblocking(true).unwrap();
            let stop = Arc::new(AtomicBool::new(false));
            let conns: Arc<Mutex<Vec<TcpStream>>> = Arc::new(Mutex::new(vec![]));
            let hellos = Arc::new(AtomicUsize::new(0));
            let subscribes = Arc::new(AtomicUsize::new(0));
            let tokens = Arc::new(Mutex::new(preset_paired.iter().map(|s| (s.to_string(), token_for(s))).collect::<HashMap<_, _>>()));
            let hello_log = Arc::new(Mutex::new(Vec::<String>::new()));
            let device_id = device_id.to_string();
            let mode = Arc::new(Mutex::new(mode));
            let (st, cs, hs, ss, ps, md, hl, dv) = (stop.clone(), conns.clone(), hellos.clone(), subscribes.clone(), tokens.clone(), mode.clone(), hello_log.clone(), device_id.clone());
            let thread = std::thread::spawn(move || {
                let mut workers: Vec<JoinHandle<()>> = vec![];
                while !st.load(Ordering::SeqCst) {
                    match listener.accept() {
                        Ok((s, _)) => {
                            s.set_nonblocking(false).unwrap();
                            s.set_read_timeout(Some(Duration::from_millis(20))).unwrap();
                            cs.lock().unwrap().push(s.try_clone().unwrap());
                            let (st2, hs2, ss2, ps2, md2, hl2, dv2) = (st.clone(), hs.clone(), ss.clone(), ps.clone(), md.clone(), hl.clone(), dv.clone());
                            workers.push(std::thread::spawn(move || serve(s, st2, hs2, ss2, ps2, md2, hl2, dv2)));
                        }
                        Err(_) => std::thread::sleep(Duration::from_millis(5)),
                    }
                }
                for w in workers {
                    let _ = w.join();
                }
            });
            FakeBridge { port, stop, conns, hellos, subscribes, tokens, hello_log, mode, thread: Some(thread) }
        }

        fn push_state(&self, rev: u64, scene: i32) {
            for c in self.conns.lock().unwrap().iter_mut() {
                send_server(c, &state_msg(rev, scene));
            }
        }

        fn shutdown(&mut self) {
            self.stop.store(true, Ordering::SeqCst);
            for c in self.conns.lock().unwrap().iter() {
                let _ = c.shutdown(std::net::Shutdown::Both);
            }
            if let Some(t) = self.thread.take() {
                let _ = t.join();
            }
        }
    }

    impl Drop for FakeBridge {
        fn drop(&mut self) {
            self.shutdown();
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn serve(mut s: TcpStream, stop: Arc<AtomicBool>, hellos: Arc<AtomicUsize>, subscribes: Arc<AtomicUsize>, paired: Arc<Mutex<HashMap<String, String>>>, mode: Arc<Mutex<Mode>>, hello_log: Arc<Mutex<Vec<String>>>, device_id: String) {
        let mut client_id = String::new();
        let mut is_paired = false;
        let mut wrong = 0;
        let mut last = Vec::<u8>::new();
        let _ = &mut last;
        // Frames can arrive back to back: keep one reader buffer for the connection.
        let mut buf: Vec<u8> = Vec::new();
        loop {
            // pull one frame from buf or the socket
            let msg = loop {
                if buf.len() >= 4 {
                    let len = u32::from_be_bytes([buf[0], buf[1], buf[2], buf[3]]) as usize;
                    if buf.len() >= 4 + len {
                        let m = ClientMessage::decode(&buf[4..4 + len]).unwrap();
                        buf.drain(..4 + len);
                        break Some(m);
                    }
                }
                if stop.load(Ordering::SeqCst) {
                    break None;
                }
                let mut c = [0u8; 4096];
                match s.read(&mut c) {
                    Ok(0) => break None,
                    Ok(n) => buf.extend_from_slice(&c[..n]),
                    Err(e) if e.kind() == ErrorKind::WouldBlock || e.kind() == ErrorKind::TimedOut => continue,
                    Err(_) => break None,
                }
            };
            let Some(m) = msg else { return };
            let id = m.request_id;
            let err = |code: ErrorCode, d: &[(&str, &str)]| ServerMessage {
                request_id: id,
                revision: 0,
                body: Some(SBody::Error(WsError { code: code as i32, message: "x".into(), details: d.iter().map(|(k, v)| (k.to_string(), v.to_string())).collect() })),
            };
            match m.body {
                Some(CBody::Hello(h)) => {
                    hellos.fetch_add(1, Ordering::SeqCst);
                    client_id = h.client_id.clone();
                    let md = *mode.lock().unwrap();
                    if md == Mode::UnsupportedVersion {
                        send_server(&mut s, &err(ErrorCode::UnsupportedVersion, &[("reason", "too_new"), ("supported", "1")]));
                        return;
                    }
                    hello_log.lock().unwrap().push(format!("{}|{}|{}", device_id, client_id, h.pairing_token));
                    // Paired only by the TOKEN issued to this id; the id alone proves nothing (and a Hello may be sent again to present the token).
                    is_paired = is_paired || paired.lock().unwrap().get(&client_id).map(|t| *t == h.pairing_token && !t.is_empty()).unwrap_or(false);
                    send_server(
                        &mut s,
                        &ServerMessage {
                            request_id: id,
                            revision: 0,
                            body: Some(SBody::HelloAck(HelloAck {
                                version: if md == Mode::WrongProtocolInAck { 9 } else { h.version },
                                features: h.features.clone(),
                                revision: 0,
                                device_version: "1".into(),
                                firmware_version: "9.9.9".into(),
                                device_id: device_id.clone(),
                                model: "Velora Test".into(),
                                paired: is_paired,
                            })),
                        },
                    );
                }
                Some(CBody::PairRequest(p)) => {
                    if is_paired {
                        send_server(&mut s, &ServerMessage { request_id: id, revision: 0, body: Some(SBody::PairReply(PairReply { paired: true, computer_id: client_id.clone(), pairing_token: String::new() })) });
                    } else if p.code == "123456" {
                        is_paired = true;
                        let tok = token_for(&format!("{}-issued", client_id));
                        paired.lock().unwrap().insert(client_id.clone(), tok.clone());
                        send_server(&mut s, &ServerMessage { request_id: id, revision: 0, body: Some(SBody::PairReply(PairReply { paired: true, computer_id: client_id.clone(), pairing_token: tok })) });
                    } else {
                        wrong += 1;
                        let left = (5 - wrong).to_string();
                        send_server(&mut s, &err(ErrorCode::PairingCodeInvalid, &[("attempts_left", left.as_str())]));
                    }
                }
                Some(CBody::GetState(_)) if is_paired => send_server(&mut s, &ServerMessage { request_id: id, revision: 7, body: Some(SBody::State(State { revision: 7, current_scene: 3, ..Default::default() })) }),
                Some(CBody::Subscribe(_)) if is_paired => {
                    subscribes.fetch_add(1, Ordering::SeqCst);
                    send_server(&mut s, &ServerMessage { request_id: id, revision: 7, body: Some(SBody::Ack(Ack { not_durable: false })) });
                    send_server(&mut s, &state_msg(7, 3));
                }
                Some(CBody::Library(r)) if is_paired => send_server(&mut s, &fake_library_reply(id, &r.payload)),
                Some(_) if is_paired => send_server(&mut s, &ServerMessage { request_id: id, revision: 0, body: Some(SBody::Ack(Ack { not_durable: false })) }),
                _ => send_server(&mut s, &err(ErrorCode::NotPaired, &[])),
            }
        }
    }

    /// The fake librarian behind the fake bridge (Story 9.11): "Grand" (3 instruments), "Strings" (250: two pages at the client's page size), "Vanished" (listed, then
    /// UNKNOWN_LIBRARY), "Empty" (0). An instrument page over 100 rows is "too large" like a reply that does not fit one ws.v1 frame.
    fn fake_library_reply(id: u64, payload: &[u8]) -> ServerMessage {
        use crate::ws_v1::{InstrumentInfo, InstrumentList, LibraryError, LibraryErrorCode, LibraryInfo, LibraryList, LibraryReply};
        let lm = LibraryClientMessage::decode(payload).unwrap();
        let wrap = |body: LSBody| ServerMessage {
            request_id: id,
            revision: 0,
            body: Some(SBody::LibraryReply(LibraryReply { payload: LibraryServerMessage { request_id: lm.request_id, body: Some(body) }.encode_to_vec() })),
        };
        let libs = [("lib-grand", "Grand", 3u32), ("lib-strings", "Strings", 250), ("lib-gone", "Vanished", 5), ("lib-empty", "Empty", 0)];
        match lm.body {
            Some(LBody::ListLibraries(l)) => {
                let limit = if l.limit == 0 { 500 } else { l.limit } as usize;
                let rows: Vec<LibraryInfo> = libs.iter().skip(l.offset as usize).take(limit).map(|(i, n, c)| LibraryInfo { library_id: i.to_string(), name: n.to_string(), instrument_count: *c, ..Default::default() }).collect();
                let more = (l.offset as usize + rows.len()) < libs.len();
                wrap(LSBody::LibraryList(LibraryList { libraries: rows, total: libs.len() as u32, offset: l.offset, more }))
            }
            Some(LBody::ListInstruments(l)) => {
                let Some((lid, _, count)) = libs.iter().find(|(i, _, _)| *i == l.library_id) else {
                    return wrap(LSBody::Error(LibraryError { code: LibraryErrorCode::UnknownLibrary as i32, message: "no such library".into(), ..Default::default() }));
                };
                if *lid == "lib-gone" {
                    return wrap(LSBody::Error(LibraryError { code: LibraryErrorCode::UnknownLibrary as i32, message: "no such library".into(), ..Default::default() }));
                }
                let limit = if l.limit == 0 { 500 } else { l.limit };
                if limit > 100 {
                    return ServerMessage {
                        request_id: id,
                        revision: 0,
                        body: Some(SBody::Error(WsError { code: ErrorCode::Internal as i32, message: "too big".into(), details: [("reason".to_string(), "library_reply_too_large".to_string())].into_iter().collect() })),
                    };
                }
                let end = (l.offset + limit).min(*count);
                let rows: Vec<InstrumentInfo> = (l.offset..end).map(|k| InstrumentInfo { instrument_key: format!("{}-k{}", lid, k), name: format!("{} {}", lid, k), category: 1 + (k % 17), ..Default::default() }).collect();
                wrap(LSBody::InstrumentList(InstrumentList { library_id: l.library_id.clone(), instruments: rows, total: *count, offset: l.offset, more: end < *count }))
            }
            _ => wrap(LSBody::Error(LibraryError { code: LibraryErrorCode::InvalidCommand as i32, message: "not on the allow-list".into(), ..Default::default() })),
        }
    }

    type Events = Arc<Mutex<Vec<(String, serde_json::Value)>>>;

    fn client_with_events(id: &str) -> (DeviceClient, Events) {
        let events: Events = Arc::new(Mutex::new(vec![]));
        let e2 = events.clone();
        let sink: EventSink = Arc::new(move |name, v| e2.lock().unwrap().push((name.to_string(), v)));
        (DeviceClient::new(id.to_string(), "0.0.1-test".to_string(), sink), events)
    }

    fn wait_for(f: impl Fn() -> bool) -> bool {
        let end = Instant::now() + Duration::from_secs(8);
        while Instant::now() < end {
            if f() {
                return true;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        false
    }

    fn statuses(ev: &Events) -> Vec<String> {
        ev.lock().unwrap().iter().filter(|(n, _)| n == "device://status").map(|(_, v)| v["status"].as_str().unwrap().to_string()).collect()
    }

    fn state_events(ev: &Events) -> usize {
        ev.lock().unwrap().iter().filter(|(n, _)| n == "device://state").count()
    }

    fn tmpdir(tag: &str) -> std::path::PathBuf {
        let d = std::env::temp_dir().join(format!("velora-studio-test-{}-{}-{}", std::process::id(), tag, random_hex(4).unwrap()));
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    /// A client that already holds the token the fake issued to `id` for device `device_id`.
    fn with_token(c: &DeviceClient, dir: &std::path::Path, device_id: &str, id: &str) {
        c.set_token_dir(dir.to_path_buf());
        save_token(dir, device_id, &token_for(id)).unwrap();
    }

    fn get_state() -> ClientMessage {
        ClientMessage { request_id: 0, body: Some(CBody::GetState(crate::ws_v1::GetState {})), ..Default::default() }
    }

    #[test]
    fn unpaired_then_pair_then_state_events() {
        let b = FakeBridge::start(0, &[], Mode::Normal);
        let (c, ev) = client_with_events("studio-test-1");
        let dir = tmpdir("t1");
        c.set_token_dir(dir.clone());
        assert!(!c.status().configured);
        c.connect("127.0.0.1", b.port);
        assert!(wait_for(|| c.status().status == LinkStatus::Unpaired));
        let st = c.status();
        assert!(st.configured && st.device_id.as_deref() == Some("velora-test") && st.model.as_deref() == Some("Velora Test") && st.firmware_version.as_deref() == Some("9.9.9"));
        assert!(st.features.contains(&"pairing.v1".to_string()) || !st.features.is_empty());
        // Before pairing a command is NOT_PAIRED (a typed error reply, not a transport error).
        let r = c.request(get_state(), Duration::from_secs(3)).unwrap();
        match r.body {
            Some(SBody::Error(e)) => assert_eq!(e.code, ErrorCode::NotPaired as i32),
            other => panic!("expected NOT_PAIRED, got {:?}", other),
        }
        assert_eq!(state_events(&ev), 0);
        // A wrong code is a typed error; the right one pairs, the client subscribes by itself and the State arrives as an event.
        let e = c.pair("000000", "Test Mac").unwrap_err();
        assert!(e.contains("PAIRING_CODE_INVALID") && e.contains("attempts_left=4"), "{}", e);
        assert_eq!(c.status().status, LinkStatus::Unpaired);
        c.pair("123456", "Test Mac").unwrap();
        assert!(wait_for(|| c.status().status == LinkStatus::Paired));
        // The token the unit issued is kept for THIS device id only, mode 0600, and never appears in a status or event payload.
        let tok = load_token(&dir, "velora-test").expect("token stored");
        assert_eq!(tok.len(), 64);
        assert!(load_token(&dir, "velora-other").is_none());
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(std::fs::metadata(dir.join("device_pairing.json")).unwrap().permissions().mode() & 0o777, 0o600);
        }
        assert!(!serde_json::to_string(&c.status()).unwrap().contains(&tok));
        assert!(!format!("{:?}", ev.lock().unwrap()).contains(&tok));
        assert!(c.status().persist_error.is_none());
        assert!(wait_for(|| state_events(&ev) >= 1));
        assert_eq!(c.latest_state().unwrap()["current_scene"], 3);
        assert_eq!(b.subscribes.load(Ordering::SeqCst), 1);
        // A push from the unit becomes an event and the latest state.
        b.push_state(8, 5);
        assert!(wait_for(|| c.latest_state().map(|s| s["current_scene"] == 5).unwrap_or(false)));
        // request/response: the reply to GetState is the State.
        let r = c.request(get_state(), Duration::from_secs(3)).unwrap();
        assert!(matches!(r.body, Some(SBody::State(ref s)) if s.current_scene == 3));
        let seq = statuses(&ev);
        assert_eq!(&seq[..3], &["disconnected".to_string(), "connecting".to_string(), "unpaired".to_string()], "{:?}", seq);  // configured, connecting, unpaired
        assert!(seq.contains(&"paired".to_string()));
        // A pair request through the generic path never hands the token to the caller either.
        c.disconnect();
        assert_eq!(c.status().status, LinkStatus::Disconnected);
        assert!(c.request(get_state(), Duration::from_millis(200)).is_err());
        // Connecting again: the first Hello has no token, a second one presents the stored token, and the computer is paired without a code.
        c.connect("127.0.0.1", b.port);
        assert!(wait_for(|| c.status().status == LinkStatus::Paired));
        let log = b.hello_log.lock().unwrap().clone();
        let last_two = &log[log.len() - 2..];
        assert!(last_two[0].ends_with("studio-test-1|") && last_two[1].ends_with(&format!("studio-test-1|{}", tok)), "{:?}", last_two);
        c.disconnect();
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_token_is_never_sent_to_a_different_device_id() {
        let b = FakeBridge::start_dev(0, &["studio-test-6"], Mode::Normal, "velora-other");
        let (c, _ev) = client_with_events("studio-test-6");
        let dir = tmpdir("t6");
        with_token(&c, &dir, "velora-test", "studio-test-6"); // issued by "velora-test", not by the unit we now connect to
        c.connect("127.0.0.1", b.port);
        assert!(wait_for(|| c.status().status == LinkStatus::Unpaired));
        std::thread::sleep(Duration::from_millis(300));
        let log = b.hello_log.lock().unwrap().clone();
        assert_eq!(log.len(), 1, "{:?}", log); // only the token-less Hello
        assert!(log[0].ends_with("studio-test-6|"));
        assert_eq!(c.status().status, LinkStatus::Unpaired);
        c.disconnect();
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_known_id_without_its_token_is_unpaired() {
        // The unit knows "studio-test-7" but this client has no token: unpaired (not subscribed, commands refused).
        let b = FakeBridge::start(0, &["studio-test-7"], Mode::Normal);
        let (c, _ev) = client_with_events("studio-test-7");
        c.connect("127.0.0.1", b.port);
        assert!(wait_for(|| c.status().status == LinkStatus::Unpaired));
        assert_eq!(b.subscribes.load(Ordering::SeqCst), 0);
        // A stale / wrong token: still unpaired.
        c.disconnect();
        let dir = tmpdir("t7");
        c.set_token_dir(dir.clone());
        save_token(&dir, "velora-test", &"0".repeat(64)).unwrap();
        c.connect("127.0.0.1", b.port);
        assert!(wait_for(|| c.status().status == LinkStatus::Unpaired));
        std::thread::sleep(Duration::from_millis(200));
        assert_eq!(c.status().status, LinkStatus::Unpaired);
        assert_eq!(b.subscribes.load(Ordering::SeqCst), 0);
        c.disconnect();
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn already_paired_computer_subscribes_at_once_and_reconnects_after_the_bridge_restarts() {
        let mut b = FakeBridge::start(0, &["studio-test-2"], Mode::Normal);
        let port = b.port;
        let (c, ev) = client_with_events("studio-test-2");
        let dir = tmpdir("t2");
        with_token(&c, &dir, "velora-test", "studio-test-2");
        c.connect("127.0.0.1", port);
        assert!(wait_for(|| c.status().status == LinkStatus::Paired));
        assert!(wait_for(|| state_events(&ev) >= 1));
        // The unit goes away: disconnected, not a crash; requests fail cleanly.
        b.shutdown();
        assert!(wait_for(|| c.status().status == LinkStatus::Disconnected));
        assert!(c.request(get_state(), Duration::from_millis(500)).is_err());
        let before = state_events(&ev);
        // It comes back on the same port: the client reconnects by itself, says Hello again and re-subscribes.
        let b2 = FakeBridge::start(port, &["studio-test-2"], Mode::Normal);
        assert!(wait_for(|| c.status().status == LinkStatus::Paired));
        assert!(wait_for(|| state_events(&ev) > before));
        assert_eq!(b2.hellos.load(Ordering::SeqCst), 2); // the token-less Hello and the one with the token
        assert_eq!(b2.subscribes.load(Ordering::SeqCst), 1);
        let seq = statuses(&ev);
        assert!(seq.iter().filter(|s| *s == "paired").count() >= 2, "{:?}", seq);
        assert!(seq.contains(&"disconnected".to_string()));
        c.disconnect();
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn version_mismatch_is_a_status_not_a_crash_and_does_not_retry_in_a_storm() {
        let b = FakeBridge::start(0, &[], Mode::UnsupportedVersion);
        let (c, ev) = client_with_events("studio-test-3");
        c.connect("127.0.0.1", b.port);
        assert!(wait_for(|| c.status().status == LinkStatus::VersionMismatch));
        let d = c.status().detail.unwrap();
        assert!(d.contains("UNSUPPORTED_VERSION") && d.contains("too_new"), "{}", d);
        std::thread::sleep(Duration::from_millis(400));
        assert_eq!(b.hellos.load(Ordering::SeqCst), 1);
        assert!(c.request(get_state(), Duration::from_millis(500)).is_err());
        assert!(statuses(&ev).contains(&"version-mismatch".to_string()));
        c.disconnect();
        // A bridge answering another protocol in its HelloAck is a mismatch too.
        let b2 = FakeBridge::start(0, &[], Mode::WrongProtocolInAck);
        c.connect("127.0.0.1", b2.port);
        assert!(wait_for(|| c.status().status == LinkStatus::VersionMismatch));
    }

    #[test]
    fn no_device_stays_disconnected_and_can_be_stopped() {
        // A port nothing listens on: bind then drop to get a free one.
        let port = TcpListener::bind(("127.0.0.1", 0)).unwrap().local_addr().unwrap().port();
        let (c, ev) = client_with_events("studio-test-4");
        c.connect("127.0.0.1", port);
        assert!(wait_for(|| c.status().status == LinkStatus::Disconnected && c.status().detail.is_some()));
        assert!(c.request(get_state(), Duration::from_millis(300)).is_err());
        assert!(c.status().configured);
        // The status events do not flicker on repeated failures: connecting once, disconnected, no pairing / paired.
        std::thread::sleep(Duration::from_millis(500));
        let seq = statuses(&ev);
        assert!(!seq.contains(&"paired".to_string()) && !seq.contains(&"unpaired".to_string()));
        assert!(seq.iter().filter(|s| *s == "connecting").count() == 1, "{:?}", seq);
        // The device appears later: the client connects without being asked again.
        let dir = tmpdir("t4");
        with_token(&c, &dir, "velora-test", "studio-test-4");
        let b = FakeBridge::start(port, &["studio-test-4"], Mode::Normal);
        assert!(wait_for(|| c.status().status == LinkStatus::Paired));
        drop(b);
        c.disconnect();
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn request_ids_are_the_clients_and_replies_are_matched() {
        let b = FakeBridge::start(0, &["studio-test-5"], Mode::Normal);
        let (c, _ev) = client_with_events("studio-test-5");
        let dir = tmpdir("t5");
        with_token(&c, &dir, "velora-test", "studio-test-5");
        c.connect("127.0.0.1", b.port);
        assert!(wait_for(|| c.status().status == LinkStatus::Paired));
        let c = Arc::new(c);
        let mut hs = vec![];
        for _ in 0..8 {
            let c2 = c.clone();
            hs.push(std::thread::spawn(move || {
                for _ in 0..10 {
                    let r = c2.request(get_state(), Duration::from_secs(3)).unwrap();
                    assert!(matches!(r.body, Some(SBody::State(_))));
                    assert!(r.request_id >= 2);
                }
            }));
        }
        for h in hs {
            h.join().unwrap();
        }
    }

    #[test]
    fn client_id_is_random_created_once_private_and_reused() {
        let dir = tmpdir("cid");
        let a = load_or_create_client_id(&dir).unwrap();
        assert!(a.starts_with("studio-") && a.len() <= 64 && valid_client_id(&a));
        assert_eq!(a, load_or_create_client_id(&dir).unwrap());
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(std::fs::metadata(dir.join("device_client_id")).unwrap().permissions().mode() & 0o777, 0o600);
        }
        assert_ne!(random_client_id().unwrap(), random_client_id().unwrap());
        std::fs::write(dir.join("device_client_id"), "bad id with spaces").unwrap();
        let b = load_or_create_client_id(&dir).unwrap();
        assert_ne!(b, "bad id with spaces");
        // A write error is an error, never a silently new id: the app data "folder" is a file.
        let file = dir.join("not-a-dir");
        std::fs::write(&file, "x").unwrap();
        assert!(load_or_create_client_id(&file).is_err());
        assert!(save_token(&file, "d", &"a".repeat(64)).is_err());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn timbre_list_pages_halves_a_too_large_page_and_skips_a_vanished_library() {
        let b = FakeBridge::start(0, &["studio-lib"], Mode::Normal);
        let (c, _ev) = client_with_events("studio-lib");
        let dir = tmpdir("lib");
        with_token(&c, &dir, "velora-test", "studio-lib");
        c.connect("127.0.0.1", b.port);
        assert!(wait_for(|| c.status().status == LinkStatus::Paired));
        let v = c.list_timbres(MAX_PICKER_TIMBRES).unwrap();
        let t = v["timbres"].as_array().unwrap();
        assert_eq!(t.len(), 253, "3 + 250 (the vanished library is skipped, the empty one asked for nothing)");
        assert_eq!(v["libraries"], 4);
        assert_eq!(t[0]["library_id"], "lib-grand");
        assert_eq!(t[0]["library_name"], "Grand");
        assert_eq!(t[0]["instrument_key"], "lib-grand-k0");
        assert_eq!(t[3]["library_id"], "lib-strings");
        assert_eq!(t[252]["instrument_key"], "lib-strings-k249");
        assert_eq!(v["truncated"], false);
        // every timbre keeps its own library: no key of the Strings library is listed under another one
        assert!(t.iter().all(|x| x["instrument_key"].as_str().unwrap().starts_with(x["library_id"].as_str().unwrap())));
        // a cap is a cap, and says so
        let v = c.list_timbres(10).unwrap();
        assert_eq!(v["timbres"].as_array().unwrap().len(), 10);
        assert_eq!(v["truncated"], true);
        // the unit going away is an error, not an empty list
        drop(b);
        c.disconnect();
        assert!(c.list_timbres(10).is_err());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn zone_and_arp_commands_deserialize_with_base_revision() {
        let mz: ClientMessage = serde_json::from_value(serde_json::json!({
            "base_revision": 42,
            "body": {
                "SetPartZone": {
                    "part": 1,
                    "low_key": 36,
                    "high_key": 84,
                    "octave": -1,
                    "transpose": 2,
                    "keyboard_on": false
                }
            }
        })).unwrap();
        assert_eq!(mz.base_revision, 42);
        assert!(matches!(mz.body, Some(CBody::SetPartZone(ref z)) if z.part == 1 && z.low_key == Some(36) && z.high_key == Some(84) && z.octave == Some(-1) && z.transpose == Some(2) && z.keyboard_on == Some(false)));

        let ma: ClientMessage = serde_json::from_value(serde_json::json!({
            "base_revision": 43,
            "body": {
                "SetPartArp": {
                    "part": 2,
                    "arp_on": true,
                    "arp_mode": 1,
                    "arp_rate": 3,
                    "arp_octaves": 2,
                    "arp_gate": 120,
                    "arp_swing": 60,
                    "arp_latch": true
                }
            }
        })).unwrap();
        assert_eq!(ma.base_revision, 43);
        assert!(matches!(ma.body, Some(CBody::SetPartArp(ref a)) if a.part == 2 && a.arp_on == Some(true) && a.arp_mode == Some(1) && a.arp_rate == Some(3) && a.arp_octaves == Some(2) && a.arp_gate == Some(120.0) && a.arp_swing == Some(60.0) && a.arp_latch == Some(true)));
    }

    #[test]
    fn timbre_assignment_and_library_requests_map_to_the_wire() {
        // the picker's assignment is an ordinary ws.v1 edit (SetPartTimbre with base_revision), sent as the page's JSON through device_request
        let m: ClientMessage = serde_json::from_value(serde_json::json!({"base_revision": 12, "body": {"SetPartTimbre": {"part": 2, "library_id": "lib-a", "instrument_key": "k1"}}})).unwrap();
        assert_eq!(m.base_revision, 12);
        assert!(matches!(m.body, Some(CBody::SetPartTimbre(ref t)) if t.part == 2 && t.library_id == "lib-a" && t.instrument_key == "k1" && t.engine_type_id.is_none()));
        // a library request carries the Library API message as opaque bytes
        let inner = LibraryClientMessage { request_id: 1, body: Some(LBody::ListLibraries(ListLibraries { offset: 0, limit: 200 })) };
        let back = LibraryClientMessage::decode(&inner.encode_to_vec()[..]).unwrap();
        assert!(matches!(back.body, Some(LBody::ListLibraries(ref l)) if l.limit == 200));
    }

    #[test]
    fn json_requests_deserialize_with_defaults() {
        // The shape `device_request` accepts from the UI: a ClientMessage as JSON, absent fields defaulted.
        let m: ClientMessage = serde_json::from_value(serde_json::json!({"body": {"GetState": {}}})).unwrap();
        assert!(matches!(m.body, Some(CBody::GetState(_))));
        let m: ClientMessage = serde_json::from_value(serde_json::json!({"body": {"RecallScene": {"index": 2}}})).unwrap();
        assert!(matches!(m.body, Some(CBody::RecallScene(ref r)) if r.index == 2));
    }

    #[test]
    fn live_set_view_messages_map_to_json_and_survive_the_wire() {
        use crate::ws_v1::{LiveSetViewPart, LiveSetViewReply, LiveSetViewScene, PartRunState, PartState};
        // The request the UI sends: GetLiveSetView as JSON, absent fields defaulted.
        let m: ClientMessage = serde_json::from_value(serde_json::json!({"body": {"GetLiveSetView": {"live_set_id": "abc"}}})).unwrap();
        assert!(matches!(m.body, Some(CBody::GetLiveSetView(ref g)) if g.live_set_id == "abc"));
        // The reply: nothing in it is a u64 (the JSON numbers of the webview are doubles); ids are strings, counts and keys u32, volume f32.
        let reply = ServerMessage {
            request_id: 5,
            revision: 12,
            body: Some(SBody::LiveSetViewReply(LiveSetViewReply {
                live_set_id: "ls-1".into(),
                name: "Perfect".into(),
                dirty: true,
                sounding: false,
                scenes: (0..8).map(|i| LiveSetViewScene { index: i, name: if i < 2 { format!("S{}", i) } else { String::new() }, empty: i >= 2 }).collect(),
                parts: vec![
                    LiveSetViewPart { index: 0, on: true, engine_type_id: 1, timbre_name: "Concert Grand".into(), category: 1, volume: 0.75, low_key: 36, high_key: 96, ..Default::default() },
                    LiveSetViewPart { index: 2, on: false, engine_type_id: 2, category: 3, organ_registration: "88 8600 000".into(), high_key: 127, ..Default::default() },
                    LiveSetViewPart { index: 3, on: true, engine_type_id: 1, missing_library: true, ..Default::default() },
                    LiveSetViewPart { index: 6, on: true, engine_type_id: 1, empty: true, ..Default::default() },
                ],
                part_count: 4,
                scene_count: 2,
                factory: false,
            })),
        };
        let bytes = reply.encode_to_vec();
        let back = ServerMessage::decode(bytes.as_slice()).unwrap();
        assert_eq!(back, reply);
        let j = serde_json::to_value(&back).unwrap();
        let v = &j["body"]["LiveSetViewReply"];
        assert_eq!(v["name"], "Perfect");
        assert_eq!(v["dirty"], true);
        assert_eq!(v["scenes"].as_array().unwrap().len(), 8);
        assert_eq!(v["scenes"][1]["name"], "S1");
        assert_eq!(v["scenes"][5]["empty"], true);
        assert_eq!(v["parts"][0]["timbre_name"], "Concert Grand");
        assert_eq!(v["parts"][0]["volume"], 0.75);
        assert_eq!(v["parts"][1]["organ_registration"], "88 8600 000");
        assert_eq!(v["parts"][2]["missing_library"], true);
        assert_eq!(v["parts"][3]["empty"], true);
        // No u64 field exists in the reply: every number in the JSON is exact in a double (the largest values here are 127 and 12).
        fn all_numbers_small(v: &serde_json::Value) -> bool {
            match v {
                serde_json::Value::Number(n) => n.as_f64().map(|f| f.abs() < 9007199254740992.0).unwrap_or(false),
                serde_json::Value::Array(a) => a.iter().all(all_numbers_small),
                serde_json::Value::Object(o) => o.values().all(all_numbers_small),
                _ => true,
            }
        }
        assert!(all_numbers_small(&j));
        // The State fields Studio follows from pushes (additive PartState fields) are present in the State JSON, and a State from an older unit (no such fields) defaults.
        let st = State {
            parts: vec![PartState { part: 1, volume: 0.5, keyboard_on: true, low_key: 24, high_key: 100, engine_type_id: 3, part_state: PartRunState::MissingLibrary as i32, view_fields: true, ..Default::default() }],
            ..Default::default()
        };
        let sj = serde_json::to_value(&st).unwrap();
        assert_eq!(sj["parts"][0]["volume"], 0.5);
        assert_eq!(sj["parts"][0]["keyboard_on"], true);
        assert_eq!(sj["parts"][0]["view_fields"], true);
        assert_eq!(sj["parts"][0]["part_state"], PartRunState::MissingLibrary as i32);
        let old: State = serde_json::from_value(serde_json::json!({"parts": [{"part": 0}]})).unwrap();
        assert_eq!(old.parts[0].volume, 0.0);
        assert!(!old.parts[0].keyboard_on);
        assert!(!old.parts[0].view_fields); // an older unit's State: Studio does not overlay such a part
        // A typed error reply (unknown Live Set) is an ordinary ServerMessage whose body is Error.
        let e = ServerMessage { request_id: 6, revision: 0, body: Some(SBody::Error(crate::ws_v1::Error { code: ErrorCode::NoSuchLiveSet as i32, ..Default::default() })) };
        assert_eq!(serde_json::to_value(&e).unwrap()["body"]["Error"]["code"], ErrorCode::NoSuchLiveSet as i32);
    }

    /// Story 9.12: the Write to Velora command, as the JSON the page builds (write_to_velora.js: `{ base_revision?, body: { WriteLiveSet: { scope, new_name? } } }`), maps onto the
    /// device's own WriteLiveSet (scope is the enum number 1 / 2 / 3, `new_name` only for Save as new), and the answers it reads (WriteReply, and the typed refusals with their
    /// details) come back as the JSON it parses.
    #[test]
    fn write_to_velora_messages_map_to_json() {
        use crate::ws_v1::{WriteReply, WriteScope};
        let parse = |v: serde_json::Value| -> ClientMessage { serde_json::from_value(v).expect("the page's JSON must map onto a ClientMessage") };
        let m = parse(serde_json::json!({"base_revision": 42, "body": {"WriteLiveSet": {"scope": 2}}}));
        assert_eq!(m.base_revision, 42);
        assert!(matches!(m.body, Some(CBody::WriteLiveSet(ref w)) if w.scope == WriteScope::WholeLiveSet as i32 && w.new_name.is_none()));
        let m = parse(serde_json::json!({"body": {"WriteLiveSet": {"scope": 3, "new_name": "Ballad take"}}}));
        assert_eq!(m.base_revision, 0); // an older unit: nothing is judged
        assert!(matches!(m.body, Some(CBody::WriteLiveSet(ref w)) if w.scope == WriteScope::SaveAsNew as i32 && w.new_name.as_deref() == Some("Ballad take")));
        let m = parse(serde_json::json!({"body": {"WriteLiveSet": {"scope": 1}}}));
        assert!(matches!(m.body, Some(CBody::WriteLiveSet(ref w)) if w.scope == WriteScope::Scene as i32));
        // the reply the page reads
        let reply = ServerMessage {
            request_id: 9,
            revision: 31,
            body: Some(SBody::WriteReply(WriteReply {
                scope: WriteScope::SaveAsNew as i32,
                live_set_id: "a".into(),
                written_live_set_id: "b".into(),
                written_name: "Ballad take".into(),
                scene: -1,
                written_count: 2,
                current_dirty: true,
                remaining_changes: 2,
                setlist_index: -1,
                ..Default::default()
            })),
        };
        let j = serde_json::to_value(&reply).unwrap();
        let w = &j["body"]["WriteReply"];
        assert_eq!(w["scope"], 3);
        assert_eq!(w["written_name"], "Ballad take");
        assert_eq!(w["current_dirty"], true);
        assert_eq!(w["nothing_to_write"], false);
        assert_eq!(w["remaining_changes"], 2);
        assert_eq!(j["revision"], 31);
        // the refusals write_to_velora.js reacts to: name in use (details name + suggested_name), scope unavailable (details reason), storage full, stale revision
        for (c, n) in [(ErrorCode::NameInUse, 34), (ErrorCode::WriteScopeUnavailable, 35), (ErrorCode::StorageFull, 36), (ErrorCode::StaleRevision, 47), (ErrorCode::UpstreamUnavailable, 44), (ErrorCode::InvalidName, 25), (ErrorCode::StorageFailed, 33)] {
            assert_eq!(c as i32, n);
        }
        let err = ServerMessage {
            request_id: 1,
            revision: 0,
            body: Some(SBody::Error(WsError {
                code: ErrorCode::NameInUse as i32,
                message: "name in use".into(),
                details: [("name".to_string(), "Perfect".to_string()), ("suggested_name".to_string(), "Perfect 2".to_string())].into_iter().collect(),
            })),
        };
        let j = serde_json::to_value(&err).unwrap();
        assert_eq!(j["body"]["Error"]["code"], 34);
        assert_eq!(j["body"]["Error"]["details"]["suggested_name"], "Perfect 2");
    }

    /// Story 9.9: every edit the Part Editor sends, as the JSON the page builds (part_editor.js `requests`, plus `base_revision` on the envelope), maps onto the real
    /// ClientMessage (the enum fields are numbers, `part` of a master ParamValue is absent), and a message without `base_revision` still parses (0 = not provided).
    #[test]
    fn part_editor_edits_map_to_json() {
        let parse = |v: serde_json::Value| -> ClientMessage { serde_json::from_value(v).expect("the page's JSON must map onto a ClientMessage") };
        let m = parse(serde_json::json!({"base_revision": 42, "body": {"SetPerformanceParam": {"param_id": 102, "part": 3, "value": 5.0}}}));
        assert_eq!(m.base_revision, 42);
        assert!(matches!(m.body, Some(CBody::SetPerformanceParam(ref p)) if p.param_id == 102 && p.part == 3 && p.value == 5.0));
        assert_eq!(parse(serde_json::json!({"body": {"GetState": {}}})).base_revision, 0);
        assert!(matches!(parse(serde_json::json!({"body": {"SetSendLevels": {"part": 2, "reverb": 0.5, "delay": 0.25}}})).body, Some(CBody::SetSendLevels(ref s)) if s.part == 2 && s.reverb == 0.5 && s.delay == 0.25));
        assert!(matches!(parse(serde_json::json!({"body": {"SetPartOutput": {"part": 1, "output": 1}}})).body, Some(CBody::SetPartOutput(ref s)) if s.part == 1 && s.output == 1));
        assert!(matches!(parse(serde_json::json!({"body": {"SetPartVolume": {"part": 0, "volume": 0.4}}})).body, Some(CBody::SetPartVolume(ref s)) if s.volume == 0.4));
        assert!(matches!(parse(serde_json::json!({"body": {"SetMasterParams": {"values": [{"param_id": 530, "value": 1.0}]}}})).body, Some(CBody::SetMasterParams(ref s)) if s.values.len() == 1 && s.values[0].part.is_none()));
        let chain = parse(serde_json::json!({"body": {"SetInsertChain": {"part": 0, "slots": [{"type": 2, "bypass": true, "params": [0.1, 0.2, 0.3, 0.4, 0.5]}, {"type": 4, "bypass": false, "params": [0.5, 0.5, 0.5, 0.5, 0.0]}]}}}));
        assert!(matches!(chain.body, Some(CBody::SetInsertChain(ref s)) if s.slots.len() == 2 && s.slots[0].r#type == 2 && s.slots[0].bypass && s.slots[0].params.len() == 5 && s.slots[1].r#type == 4));
        assert!(matches!(parse(serde_json::json!({"body": {"SetAssignSlot": {"target": 1, "slot": 3, "param_id": 100}}})).body, Some(CBody::SetAssignSlot(ref s)) if s.target == 1 && s.slot == 3 && s.param_id == 100));
        assert!(matches!(parse(serde_json::json!({"body": {"ClearAssignSlot": {"target": 1, "slot": 3}}})).body, Some(CBody::ClearAssignSlot(ref s)) if s.target == 1 && s.slot == 3));
        assert!(matches!(parse(serde_json::json!({"body": {"GetPerformanceValues": {}}})).body, Some(CBody::GetPerformanceValues(_))));
        // the live values come back as JSON the page reads: param_id and part numbers, value a float
        let reply = ServerMessage {
            request_id: 4,
            revision: 12,
            body: Some(SBody::PerformanceValues(crate::ws_v1::PerformanceValues { values: vec![crate::ws_v1::ParamValue { param_id: 102, part: Some(3), value: 5.0 }], ..Default::default() })),
        };
        let j = serde_json::to_value(&reply).unwrap();
        assert_eq!(j["body"]["PerformanceValues"]["values"][0]["param_id"], 102);
        assert_eq!(j["body"]["PerformanceValues"]["values"][0]["part"], 3);
        assert_eq!(j["body"]["PerformanceValues"]["values"][0]["value"], 5.0);
        assert_eq!(j["revision"], 12);
        // the edit error codes part_editor.js / fake_unit.js use
        for (c, n) in [(ErrorCode::UnknownParam, 6), (ErrorCode::OutOfRange, 7), (ErrorCode::InvalidPart, 8), (ErrorCode::WrongClass, 9), (ErrorCode::InvalidSlot, 11), (ErrorCode::Busy, 13), (ErrorCode::FxBudgetExceeded, 15), (ErrorCode::NotAssignable, 37)] {
            assert_eq!(c as i32, n);
        }
    }

    /// Story 9.13: every setlist command Studio's Setlists view sends, as the JSON the page builds (setlists_view.js `req`), maps onto the real ClientMessage (absent `index`
    /// = append), and the replies it reads (SetlistsReply with tag ids and missing counts, CreatedReply, a typed Error with details) come back as the JSON it parses.
    #[test]
    fn setlist_messages_map_to_json() {
        use crate::ws_v1::{CreatedReply, SetlistEntry, SetlistSummary, SetlistTagInfo, SetlistsReply};
        let parse = |v: serde_json::Value| -> ClientMessage { serde_json::from_value(v).expect("the page's JSON must map onto a ClientMessage") };
        assert!(matches!(parse(serde_json::json!({"body": {"CreateSetlist": {"name": "A"}}})).body, Some(CBody::CreateSetlist(ref m)) if m.name == "A"));
        assert!(matches!(parse(serde_json::json!({"body": {"RenameSetlist": {"id": "s", "name": "B"}}})).body, Some(CBody::RenameSetlist(ref m)) if m.id == "s" && m.name == "B"));
        assert!(matches!(parse(serde_json::json!({"body": {"ReorderSetlists": {"ids": ["b", "a"]}}})).body, Some(CBody::ReorderSetlists(ref m)) if m.ids == vec!["b", "a"]));
        assert!(matches!(parse(serde_json::json!({"body": {"DeleteSetlist": {"id": "s"}}})).body, Some(CBody::DeleteSetlist(ref m)) if m.id == "s"));
        assert!(matches!(parse(serde_json::json!({"body": {"AddLiveSetToSetlist": {"setlist_id": "s", "live_set_id": "l"}}})).body, Some(CBody::AddLiveSetToSetlist(ref m)) if m.index.is_none()));
        assert!(matches!(parse(serde_json::json!({"body": {"AddLiveSetToSetlist": {"setlist_id": "s", "live_set_id": "l", "index": 0}}})).body, Some(CBody::AddLiveSetToSetlist(ref m)) if m.index == Some(0)));
        assert!(matches!(parse(serde_json::json!({"body": {"RemoveLiveSetFromSetlist": {"setlist_id": "s", "live_set_id": "l"}}})).body, Some(CBody::RemoveLiveSetFromSetlist(_))));
        assert!(matches!(parse(serde_json::json!({"body": {"ReorderLiveSetsInSetlist": {"setlist_id": "s", "live_set_ids": ["x", "y"]}}})).body, Some(CBody::ReorderLiveSetsInSetlist(ref m)) if m.live_set_ids.len() == 2));
        assert!(matches!(parse(serde_json::json!({"body": {"CreateLiveSet": {"name": "L"}}})).body, Some(CBody::CreateLiveSet(_))));
        assert!(matches!(parse(serde_json::json!({"body": {"CreateTag": {"name": "T"}}})).body, Some(CBody::CreateTag(_))));
        assert!(matches!(parse(serde_json::json!({"body": {"RenameTag": {"id": "t", "name": "U"}}})).body, Some(CBody::RenameTag(_))));
        assert!(matches!(parse(serde_json::json!({"body": {"DeleteTag": {"id": "t"}}})).body, Some(CBody::DeleteTag(_))));
        assert!(matches!(parse(serde_json::json!({"body": {"SetSetlistTags": {"setlist_id": "s", "tag_ids": ["t"]}}})).body, Some(CBody::SetSetlistTags(ref m)) if m.tag_ids == vec!["t"]));
        assert!(matches!(parse(serde_json::json!({"body": {"GetSetlists": {"setlist_id": "s", "include_all_live_sets": true}}})).body, Some(CBody::GetSetlists(ref m)) if m.include_all_live_sets));
        // Replies.
        let reply = ServerMessage {
            request_id: 3,
            revision: 9,
            body: Some(SBody::SetlistsReply(SetlistsReply {
                setlists: vec![SetlistSummary { id: "s1".into(), name: "Friday".into(), tag_ids: vec!["t1".into()], live_set_count: 3, missing_count: 1, loaded: true }],
                tags: vec![SetlistTagInfo { id: "t1".into(), name: "Gig".into() }],
                status: Some(crate::ws_v1::SetlistStatus { available: true, loaded_setlist_id: "s1".into(), library_revision: 41, ..Default::default() }),
                detail_setlist_id: "s1".into(),
                entries: vec![SetlistEntry { live_set_id: "l1".into(), name: "Alpha".into(), missing: false }, SetlistEntry { live_set_id: "l2".into(), name: String::new(), missing: true }],
                all_live_sets: vec![SetlistEntry { live_set_id: "l1".into(), name: "Alpha".into(), missing: false }],
            })),
        };
        let back = ServerMessage::decode(reply.encode_to_vec().as_slice()).unwrap();
        assert_eq!(back, reply);
        let j = serde_json::to_value(&back).unwrap();
        let v = &j["body"]["SetlistsReply"];
        assert_eq!(v["setlists"][0]["tag_ids"][0], "t1");
        assert_eq!(v["setlists"][0]["live_set_count"], 3);
        assert_eq!(v["setlists"][0]["missing_count"], 1);
        assert_eq!(v["setlists"][0]["loaded"], true);
        assert_eq!(v["tags"][0]["name"], "Gig");
        assert_eq!(v["status"]["library_revision"], 41);
        assert_eq!(v["status"]["loaded_setlist_id"], "s1");
        assert_eq!(v["detail_setlist_id"], "s1");
        assert_eq!(v["entries"][1]["missing"], true);
        assert_eq!(v["all_live_sets"][0]["live_set_id"], "l1");
        let created = serde_json::to_value(ServerMessage { request_id: 4, revision: 0, body: Some(SBody::CreatedReply(CreatedReply { id: "new".into(), name: "N".into(), not_durable: true })) }).unwrap();
        assert_eq!(created["body"]["CreatedReply"]["id"], "new");
        assert_eq!(created["body"]["CreatedReply"]["not_durable"], true);
        let mut details = std::collections::HashMap::new();
        details.insert("limit_name".to_string(), "tags_per_setlist".to_string());
        details.insert("limit".to_string(), "8".to_string());
        let e = serde_json::to_value(ServerMessage {
            request_id: 5,
            revision: 0,
            body: Some(SBody::Error(crate::ws_v1::Error { code: ErrorCode::LimitReached as i32, message: "limit".into(), details })),
        })
        .unwrap();
        assert_eq!(e["body"]["Error"]["code"], ErrorCode::LimitReached as i32);
        assert_eq!(e["body"]["Error"]["code"], 29);
        assert_eq!(e["body"]["Error"]["details"]["limit_name"], "tags_per_setlist");
        // the ErrorCode numbers setlists_view.js switches on
        for (c, n) in [(ErrorCode::NoSuchLiveSet, 20), (ErrorCode::NoSuchSetlist, 22), (ErrorCode::NoSuchTag, 23), (ErrorCode::DuplicateLiveSetInSetlist, 24), (ErrorCode::InvalidName, 25), (ErrorCode::LimitReached, 29), (ErrorCode::LiveSetInUse, 30), (ErrorCode::InvalidOrder, 31), (ErrorCode::LiveSetNotInSetlist, 32), (ErrorCode::StorageFailed, 33)] {
            assert_eq!(c as i32, n);
        }
    }

    /// Against a REAL bridge (sysd --bridge-port ... with a real engine-host). Skipped unless VELORA_TEST_BRIDGE=host:port and VELORA_TEST_PAIRING_CODE are set.
    #[test]
    fn real_bridge_round_trip() {
        let Ok(addr) = std::env::var("VELORA_TEST_BRIDGE") else {
            eprintln!("real_bridge_round_trip: skipped (VELORA_TEST_BRIDGE not set)");
            return;
        };
        let code = std::env::var("VELORA_TEST_PAIRING_CODE").unwrap_or_default();
        let (host, port) = addr.rsplit_once(':').unwrap();
        let (c, ev) = client_with_events("studio-real-bridge-test");
        let dir = tmpdir("real");
        c.set_token_dir(dir.clone());
        c.connect(host, port.parse().unwrap());
        assert!(wait_for(|| matches!(c.status().status, LinkStatus::Paired | LinkStatus::Unpaired)));
        if c.status().status == LinkStatus::Unpaired {
            c.pair(&code, "Studio cargo test").unwrap();
        }
        assert!(wait_for(|| c.status().status == LinkStatus::Paired));
        assert!(wait_for(|| state_events(&ev) >= 1), "no State event from the real engine");
        let r = c.request(get_state(), Duration::from_secs(5)).unwrap();
        assert!(matches!(r.body, Some(SBody::State(ref s)) if s.parts.len() >= 8));
        // Story 9.8: the read-only Live Set view from a real engine: list the Live Sets (GetSetlists with All Live Sets), then read each back. Zero Live Sets or an Error
        // body fails the test (the C++ smoke creates one first, and an engine with a data directory makes its own).
        let lists = c
            .request(ClientMessage { request_id: 0, body: Some(CBody::GetSetlists(crate::ws_v1::GetSetlists { setlist_id: String::new(), include_all_live_sets: true })), ..Default::default() }, Duration::from_secs(5))
            .unwrap();
        let l = match lists.body {
            Some(SBody::SetlistsReply(ref l)) => l,
            ref other => panic!("expected SetlistsReply, got {:?}", other),
        };
        assert!(!l.all_live_sets.is_empty(), "the engine lists no Live Set: the smoke must create one first");
        {
            for first in l.all_live_sets.iter() {
                let rev_before = c.latest_state().map(|s| s["revision"].clone());
                let r = c
                    .request(ClientMessage { request_id: 0, body: Some(CBody::GetLiveSetView(crate::ws_v1::GetLiveSetView { live_set_id: first.live_set_id.clone() })), ..Default::default() }, Duration::from_secs(5))
                    .unwrap();
                match r.body {
                    Some(SBody::LiveSetViewReply(ref v)) => {
                        assert_eq!(v.live_set_id, first.live_set_id);
                        assert_eq!(v.name, first.name);
                        assert_eq!(v.scenes.len(), 8);
                        assert!(v.parts.len() >= 8 && v.part_count as usize == v.parts.len());
                        assert!(v.parts.iter().all(|p| p.volume >= 0.0 && p.volume <= 1.0));
                        eprintln!("real_bridge_round_trip: view of {:?}: {} parts, {} scenes stored, dirty {}", v.name, v.parts.len(), v.scene_count, v.dirty);
                    }
                    other => panic!("expected LiveSetViewReply, got {:?}", other),
                }
                // Read-only: asking for a view moved nothing.
                assert_eq!(c.latest_state().map(|s| s["revision"].clone()), rev_before);
            }
        }
        real_setlists_flow(&c);
        real_part_edit_flow(&c);
        if std::env::var("VELORA_TEST_TIMBRES").is_ok() {
            real_timbre_flow(&c);
        }
        real_write_flow(&c);
        eprintln!("real_bridge_round_trip: status {:?}", c.status());
        c.disconnect();
        let _ = std::fs::remove_dir_all(&dir);
    }
    /// Story 9.9 against a REAL bridge and engine (called by real_bridge_round_trip with its paired client): the edits the Part Editor sends, as the page's JSON with
    /// `base_revision` (Story 9.10: the engine refuses a stale one). A performance edit is acked (queued for the RT plane) and the live values are readable; the structural
    /// edits (sends, volume, insert chain) publish a State that shows them; a bad value is a typed error. The ASSIGN slot commands answer an Ack, or NO_LIVE_SET when the smoke
    /// engine has no Live Set loaded.
    fn real_part_edit_flow(c: &DeviceClient) {
        let call = |v: serde_json::Value| -> serde_json::Value {
            let m: ClientMessage = serde_json::from_value(v).expect("page JSON maps onto a ClientMessage");
            serde_json::to_value(c.request(m, Duration::from_secs(5)).expect("a reply")).unwrap()
        };
        // Story 9.10: the base_revision of an edit is the Live Set revision (State.edits.live_set_revision), read fresh (an edit WITHOUT base_revision moves it)
        let rev = || call(serde_json::json!({"body": {"GetState": {}}}))["body"]["State"]["edits"]["live_set_revision"].as_u64().unwrap_or(0);
        let live = |param: u64, part: u64| -> Option<f64> {
            let r = call(serde_json::json!({"body": {"GetPerformanceValues": {}}}));
            r["body"]["PerformanceValues"]["values"].as_array()?.iter().find(|v| v["param_id"] == param && v["part"] == part).and_then(|v| v["value"].as_f64())
        };
        // a performance edit: acked, then the live value is the one sent (the RT plane applies it a block later)
        let r = call(serde_json::json!({"base_revision": rev(), "body": {"SetPerformanceParam": {"param_id": 102, "part": 0, "value": 5.0}}}));
        assert!(r["body"].get("Ack").is_some(), "expected Ack, got {}", r);
        // The smoke engine runs --no-audio: no render thread applies the queued event, so the live value stays what it was (the C++ control tests cover RT applying it). What is
        // checked here is the read-back path the Part Editor uses to confirm a knob: GetPerformanceValues lists the parameter for the part.
        let before = live(102, 0);
        assert!(before.is_some(), "GetPerformanceValues does not list drawbar_8 for part 0");
        // Story 9.10: an edit built on an older Live Set revision is STALE_REVISION (47) with both revisions in the details, and changes nothing; an edit without base_revision
        // moves the revision (a change from elsewhere), so the one built on the revision before it is stale in turn
        let now = rev();
        assert!(now >= 1, "State.edits.live_set_revision missing");
        let stale = call(serde_json::json!({"base_revision": now + 1, "body": {"SetPerformanceParam": {"param_id": 102, "part": 0, "value": 4.0}}}));
        assert_eq!(stale["body"]["Error"]["code"], 47, "expected STALE_REVISION, got {}", stale);
        assert_eq!(stale["body"]["Error"]["details"]["current_revision"], now.to_string());
        assert_eq!(live(102, 0), before, "a stale edit changed nothing");
        let r = call(serde_json::json!({"body": {"SetPerformanceParam": {"param_id": 102, "part": 0, "value": 5.0}}}));
        assert!(r["body"].get("Ack").is_some(), "expected Ack, got {}", r);
        assert!(rev() > now, "an edit without base_revision did not move the revision");
        assert_eq!(call(serde_json::json!({"base_revision": now, "body": {"SetPerformanceParam": {"param_id": 102, "part": 0, "value": 5.0}}}))["body"]["Error"]["code"], 47);
        // a value outside the registry range, an unknown parameter and a bad part are typed errors (the knob reverts and shows the message)
        assert_eq!(call(serde_json::json!({"base_revision": rev(), "body": {"SetPerformanceParam": {"param_id": 102, "part": 0, "value": 9.0}}}))["body"]["Error"]["code"], 7);
        assert_eq!(call(serde_json::json!({"body": {"SetPerformanceParam": {"param_id": 99999, "part": 0, "value": 1.0}}}))["body"]["Error"]["code"], 6);
        assert_eq!(call(serde_json::json!({"body": {"SetPerformanceParam": {"param_id": 102, "part": 99, "value": 1.0}}}))["body"]["Error"]["code"], 8);
        assert_eq!(live(102, 0), before, "a refused edit changed nothing");
        // the structural edits: each publishes a generation and the State shows it
        let part0 = |field: &str| c.latest_state().map(|s| s["parts"][0][field].clone());
        let r = call(serde_json::json!({"base_revision": rev(), "body": {"SetSendLevels": {"part": 0, "reverb": 0.5, "delay": 0.25}}}));
        assert!(r["body"].get("Ack").is_some(), "expected Ack, got {}", r);
        assert!(wait_for(|| part0("send_reverb") == Some(serde_json::json!(0.5)) && part0("send_delay") == Some(serde_json::json!(0.25))), "the State does not show the sends");
        let r = call(serde_json::json!({"base_revision": rev(), "body": {"SetPartVolume": {"part": 0, "volume": 0.5}}}));
        assert!(r["body"].get("Ack").is_some(), "expected Ack, got {}", r);
        assert!(wait_for(|| part0("volume") == Some(serde_json::json!(0.5))), "the State does not show the volume");
        let chain = serde_json::json!([{"type": 1, "bypass": false, "params": [0.5, 0.5, 0.5, 0.5, 0.0]}]);
        let r = call(serde_json::json!({"base_revision": rev(), "body": {"SetInsertChain": {"part": 0, "slots": chain}}}));
        assert!(r["body"].get("Ack").is_some(), "expected Ack, got {}", r);
        assert!(wait_for(|| part0("inserts").map(|i| i.as_array().map(|a| a.len()) == Some(1)).unwrap_or(false)), "the State does not show the insert chain");
        let r = call(serde_json::json!({"body": {"SetInsertChain": {"part": 0, "slots": []}}}));
        assert!(r["body"].get("Ack").is_some(), "expected Ack, got {}", r);
        assert_eq!(call(serde_json::json!({"body": {"SetPartVolume": {"part": 0, "volume": 1.5}}}))["body"]["Error"]["code"], 7);
        // ASSIGN: SetAssignSlot / ClearAssignSlot target the current Live Set
        let r = call(serde_json::json!({"base_revision": rev(), "body": {"SetAssignSlot": {"target": 1, "slot": 1, "param_id": 102}}}));
        assert!(r["body"].get("Ack").is_some() || r["body"]["Error"]["code"] == 17, "expected Ack or NO_LIVE_SET, got {}", r);
        assert_eq!(call(serde_json::json!({"body": {"SetAssignSlot": {"target": 1, "slot": 9, "param_id": 102}}}))["body"]["Error"]["code"].as_u64().map(|c| c == 11 || c == 17), Some(true));
        eprintln!("real_part_edit_flow: ok");
    }

    /// Story 9.11 against a REAL librarian, bridge and engine (the C++ smoke starts a librarian-service over the fixture libraries and sets VELORA_TEST_TIMBRES): the picker's list
    /// (Library API through the bridge's allow-list), the assignment as the page sends it (SetPartTimbre with base_revision) on an empty part, the rebuilt Live Set read back
    /// (GetLiveSetView: the chosen timbre's name, dirty, a timbre change in the summary), a stale assignment refused, and a timbre whose library is not there (the engine takes the
    /// reference and the part reads "missing library" with NO name: never another timbre).
    fn real_timbre_flow(c: &DeviceClient) {
        let call = |v: serde_json::Value| -> serde_json::Value {
            let m: ClientMessage = serde_json::from_value(v).expect("page JSON maps onto a ClientMessage");
            serde_json::to_value(c.request(m, Duration::from_secs(10)).expect("a reply")).unwrap()
        };
        let state = || call(serde_json::json!({"body": {"GetState": {}}}))["body"]["State"].clone();
        let list = c.list_timbres(MAX_PICKER_TIMBRES).expect("the timbre list through the bridge");
        let timbres = list["timbres"].as_array().unwrap();
        assert!(!timbres.is_empty(), "the librarian lists no timbre: the fixture libraries were not found");
        assert_eq!(list["truncated"], false);
        assert!(timbres.iter().all(|t| !t["library_id"].as_str().unwrap().is_empty() && !t["instrument_key"].as_str().unwrap().is_empty() && !t["name"].as_str().unwrap().is_empty()));
        let wire = serde_json::to_string(&list).unwrap();
        assert!(!wire.contains("source_path"), "a device path must not reach Studio");
        let st = state();
        assert_eq!(st["live_set_loaded"], true, "the smoke engine has no Live Set loaded");
        let live_id = st["live_set_id"].as_str().unwrap().to_string();
        let empties: Vec<u64> = st["parts"].as_array().unwrap().iter().filter(|p| p["part_state"] == 1).map(|p| p["part"].as_u64().unwrap()).collect();
        assert!(empties.len() >= 2, "need two empty parts, got {:?}", empties);
        let (p1, p2) = (empties[0], empties[1]);
        let part_state = |part: u64| -> Option<u64> { c.latest_state().and_then(|s| s["parts"][part as usize]["part_state"].as_u64()) };
        let settle = |part: u64, want: u64| {
            let end = Instant::now() + Duration::from_secs(30);
            while Instant::now() < end {
                let s = state();
                if part_state(part) == Some(want) && s["loading"]["loading"] != true {
                    return true;
                }
                std::thread::sleep(Duration::from_millis(50));
            }
            false
        };
        let rev = || state()["edits"]["live_set_revision"].as_u64().unwrap();
        // assign: a Studio structural edit with base_revision; the unit rebuilds the part
        let pick = &timbres[0];
        let lib = pick["library_id"].as_str().unwrap();
        let key = pick["instrument_key"].as_str().unwrap();
        let name = pick["name"].as_str().unwrap();
        let base = rev();
        let r = call(serde_json::json!({"base_revision": base, "body": {"SetPartTimbre": {"part": p1, "library_id": lib, "instrument_key": key}}}));
        assert!(r["body"].get("Ack").is_some(), "expected Ack, got {}", r);
        assert!(settle(p1, 0), "the part did not become READY after the timbre was assigned");
        assert_eq!(rev(), base, "a Studio edit does not move the revision (the rebuild and the restore are not foreign changes)");
        let st = state();
        assert_eq!(st["edits"]["has_timbre_edit"], true);
        assert_eq!(st["edits"]["current_dirty"], true);
        let view = call(serde_json::json!({"body": {"GetLiveSetView": {"live_set_id": live_id}}}));
        let part = &view["body"]["LiveSetViewReply"]["parts"].as_array().unwrap().iter().find(|p| p["index"].as_u64().unwrap_or(0) == p1).cloned().expect("the part is in the view");
        assert_eq!(part["empty"], false);
        assert_eq!(part["missing_library"], false);
        assert_eq!(part["timbre_name"], name, "the view shows the timbre that was picked");
        // an assignment built on an older revision is refused and changes nothing
        let r = call(serde_json::json!({"base_revision": base + 5, "body": {"SetPartTimbre": {"part": p2, "library_id": lib, "instrument_key": key}}}));
        assert_eq!(r["body"]["Error"]["code"], 47, "expected STALE_REVISION, got {}", r);
        assert_eq!(part_state(p2), Some(1), "a refused assignment left the part empty");
        // a bad part and a malformed reference are typed errors, not an assignment
        assert_eq!(call(serde_json::json!({"base_revision": rev(), "body": {"SetPartTimbre": {"part": 99, "library_id": lib, "instrument_key": key}}}))["body"]["Error"]["code"], 8);
        // the library vanished between the list and the assign: the unit takes the reference and the part is "missing library" with no name
        let gone = "00000000-0000-4000-8000-00000000dead";
        let r = call(serde_json::json!({"base_revision": rev(), "body": {"SetPartTimbre": {"part": p2, "library_id": gone, "instrument_key": "nothing"}}}));
        assert!(r["body"].get("Ack").is_some(), "expected Ack, got {}", r);
        assert!(settle(p2, 2), "the part did not read MISSING_LIBRARY");
        let view = call(serde_json::json!({"body": {"GetLiveSetView": {"live_set_id": live_id}}}));
        let part = &view["body"]["LiveSetViewReply"]["parts"].as_array().unwrap().iter().find(|p| p["index"].as_u64().unwrap_or(0) == p2).cloned().unwrap();
        assert_eq!(part["missing_library"], true);
        assert_eq!(part["timbre_name"], "", "a missing library never shows another timbre's name");
        assert_eq!(part["empty"], false);
        eprintln!("real_timbre_flow: ok ({} timbres listed)", timbres.len());
    }

    /// Story 9.12 against a REAL bridge and engine (called by real_bridge_round_trip with its paired client, after the edits and timbre flows left the Live Set with unsaved
    /// edits): the Write command the device's own Write Dialog sends, as the page's JSON with base_revision. A Write on a stale revision is refused and writes nothing; Save as new
    /// with the Live Set's own name is NAME_IN_USE with a suggested free name, and with that name creates a NEW Live Set while the sounding one keeps its edits; Scene is refused
    /// with `timbre_change` when the edits include a timbre change; Whole Live Set clears the unsaved state on the engine (State.edits.current_dirty / dirty_live_set_ids) without
    /// moving live_set_revision, so Studio's next edit on the same base still works; a repeated Write says nothing_to_write. (Storage full needs the store's test hook, which the
    /// engine binary does not expose: it is covered by engine/host/tests/edit_session_test.cpp test_storage_full.)
    fn real_write_flow(c: &DeviceClient) {
        let call = |v: serde_json::Value| -> serde_json::Value {
            let m: ClientMessage = serde_json::from_value(v).expect("page JSON maps onto a ClientMessage");
            serde_json::to_value(c.request(m, Duration::from_secs(15)).expect("a reply")).unwrap()
        };
        let state = || call(serde_json::json!({"body": {"GetState": {}}}))["body"]["State"].clone();
        let st = state();
        assert_eq!(st["live_set_loaded"], true, "the smoke engine has no Live Set loaded");
        assert_eq!(st["edits"]["available"], true, "the engine runs no edit session");
        let rev = || state()["edits"]["live_set_revision"].as_u64().unwrap();
        let writes = || state()["edits"]["writes_done"].as_u64().unwrap();
        let settle = |f: &dyn Fn(&serde_json::Value) -> bool| {
            let end = Instant::now() + Duration::from_secs(30);
            while Instant::now() < end {
                let s = state();
                if f(&s) && s["loading"]["loading"] != true {
                    return true;
                }
                std::thread::sleep(Duration::from_millis(50));
            }
            false
        };
        assert!(settle(&|_| true));
        // an unsaved Studio edit (a structural volume edit with the revision the view was loaded at)
        let base = rev();
        let r = call(serde_json::json!({"base_revision": base, "body": {"SetPartVolume": {"part": 0, "volume": 0.37}}}));
        assert!(r["body"].get("Ack").is_some(), "expected Ack, got {}", r);
        assert!(settle(&|s| s["edits"]["current_dirty"] == true));
        let st = state();
        let has_timbre = st["edits"]["has_timbre_edit"] == true;
        let items: Vec<String> = st["edits"]["change_summary"].as_array().unwrap().iter().map(|i| i["key"].as_str().unwrap().to_string()).collect();
        assert!(items.iter().any(|k| k == "part.volume"), "the change list does not say the volume changed: {:?}", items);
        assert_eq!(rev(), base, "a Studio edit does not move the revision");
        assert!(st["edits"]["dirty_live_set_ids"].as_array().unwrap().iter().any(|i| *i == st["live_set_id"]));
        let before = writes();
        // a Write on a view older than the unit's Live Set is refused (like an edit) and writes nothing; one with no scope is INVALID_COMMAND
        let stale = call(serde_json::json!({"base_revision": base + 7, "body": {"WriteLiveSet": {"scope": 2}}}));
        assert_eq!(stale["body"]["Error"]["code"], 47, "expected STALE_REVISION, got {}", stale);
        assert_eq!(stale["body"]["Error"]["details"]["current_revision"], base.to_string());
        assert_eq!(writes(), before);
        assert_eq!(state()["edits"]["current_dirty"], true, "a refused Write changed nothing");
        assert_eq!(call(serde_json::json!({"base_revision": base, "body": {"WriteLiveSet": {"scope": 0}}}))["body"]["Error"]["code"], 5);
        // Save as new: the Live Set's own name is taken (the device's NAME_IN_USE with a free name); the free name creates a NEW Live Set and the current one keeps its edits
        let own = st["live_set_name"].as_str().unwrap().to_string();
        let taken = call(serde_json::json!({"base_revision": base, "body": {"WriteLiveSet": {"scope": 3, "new_name": own}}}));
        assert_eq!(taken["body"]["Error"]["code"], 34, "expected NAME_IN_USE, got {}", taken);
        let suggested = taken["body"]["Error"]["details"]["suggested_name"].as_str().expect("a suggested name").to_string();
        assert_ne!(suggested, own);
        assert_eq!(writes(), before, "a refused name wrote nothing");
        let copy = call(serde_json::json!({"base_revision": base, "body": {"WriteLiveSet": {"scope": 3, "new_name": suggested}}}));
        let w = &copy["body"]["WriteReply"];
        assert!(w.is_object(), "expected WriteReply, got {}", copy);
        assert_eq!(w["scope"], 3);
        assert_eq!(w["written_name"], suggested.as_str());
        assert_ne!(w["written_live_set_id"], st["live_set_id"]);
        assert_eq!(w["current_dirty"], true, "the sounding Live Set keeps its unsaved edits after Save as new");
        assert_eq!(state()["edits"]["current_dirty"], true);
        assert_eq!(writes(), before + 1);
        assert_eq!(rev(), base, "Save as new does not move live_set_revision");
        let listed = call(serde_json::json!({"body": {"GetSetlists": {"setlist_id": "", "include_all_live_sets": true}}}));
        assert!(listed["body"]["SetlistsReply"]["all_live_sets"].as_array().unwrap().iter().any(|l| l["name"] == suggested.as_str()), "the new Live Set is not listed");
        // Scene: refused with the device's reason when the edits include a timbre change (the timbre flow ran before this one when the librarian is there)
        let scene = call(serde_json::json!({"base_revision": base, "body": {"WriteLiveSet": {"scope": 1}}}));
        if has_timbre {
            assert_eq!(scene["body"]["Error"]["code"], 35, "expected WRITE_SCOPE_UNAVAILABLE, got {}", scene);
            assert_eq!(scene["body"]["Error"]["details"]["reason"], "timbre_change");
            assert_eq!(writes(), before + 1);
        } else {
            assert!(scene["body"].get("WriteReply").is_some() || scene["body"]["Error"]["code"] == 35, "unexpected Scene answer {}", scene);
        }
        // Whole Live Set: written; the unsaved state is gone on the engine, live_set_revision did not move, and the next edit on the same base is accepted
        let before_whole = writes();
        let whole = call(serde_json::json!({"base_revision": base, "body": {"WriteLiveSet": {"scope": 2}}}));
        let w = &whole["body"]["WriteReply"];
        assert!(w.is_object(), "expected WriteReply, got {}", whole);
        assert_eq!(w["scope"], 2);
        assert_eq!(w["nothing_to_write"], false);
        assert_eq!(w["current_dirty"], false);
        assert_eq!(w["remaining_changes"], 0);
        assert!(w["written_count"].as_u64().unwrap() >= 1);
        assert!(settle(&|s| s["edits"]["current_dirty"] == false));
        let after = state();
        assert_eq!(after["edits"]["has_timbre_edit"], false);
        assert!(after["edits"]["dirty_live_set_ids"].as_array().unwrap().is_empty(), "the engine still lists dirty Live Sets: {}", after["edits"]["dirty_live_set_ids"]);
        assert_eq!(after["edits"]["writes_done"].as_u64().unwrap(), before_whole + 1);
        assert_eq!(after["edits"]["live_set_revision"].as_u64().unwrap(), base, "a Write is not a change of the Live Set");
        // the pushed State says the same (what the Studio footer and the diamond read)
        assert!(wait_for(|| c.latest_state().map(|s| s["edits"]["current_dirty"] == false).unwrap_or(false)), "the pushed State still says unsaved");
        // nothing left: the same Write says so and touches nothing
        let again = call(serde_json::json!({"base_revision": base, "body": {"WriteLiveSet": {"scope": 2}}}));
        assert_eq!(again["body"]["WriteReply"]["nothing_to_write"], true, "got {}", again);
        assert_eq!(writes(), before_whole + 1);
        // and Studio's view is still current: an edit on the same base is accepted
        let r = call(serde_json::json!({"base_revision": base, "body": {"SetPartVolume": {"part": 0, "volume": 0.5}}}));
        assert!(r["body"].get("Ack").is_some(), "expected Ack after the Write, got {}", r);
        eprintln!("real_write_flow: ok (timbre edit present: {})", has_timbre);
    }

    /// Story 9.13 against a REAL bridge and engine (called by real_bridge_round_trip with its paired client: the bridge's pairing window closes at the first pairing, so
    /// there is one client): the commands of the Setlists view, sent as the page's JSON, then read back. The setlist it leaves behind ("Studio Smoke Setlist": Studio
    /// Smoke B then Studio Smoke A, tagged "Studio Smoke Tag") is what the C++ smoke checks after restarting the engine.
    fn real_setlists_flow(c: &DeviceClient) {
        // JSON in, JSON out: exactly what window.veloraDevice.request does.
        let call = |v: serde_json::Value| -> serde_json::Value {
            let m: ClientMessage = serde_json::from_value(v).expect("page JSON maps onto a ClientMessage");
            serde_json::to_value(c.request(m, Duration::from_secs(5)).expect("a reply")).unwrap()
        };
        let created = |v: serde_json::Value| -> String {
            let r = call(v);
            r["body"]["CreatedReply"]["id"].as_str().unwrap_or_else(|| panic!("expected CreatedReply, got {}", r)).to_string()
        };
        let ok = |v: serde_json::Value| {
            let r = call(v);
            assert!(r["body"].get("Ack").is_some(), "expected Ack, got {}", r);
        };
        let read = |setlist: &str| call(serde_json::json!({"body": {"GetSetlists": {"setlist_id": setlist, "include_all_live_sets": true}}}))["body"]["SetlistsReply"].clone();
        let la = created(serde_json::json!({"body": {"CreateLiveSet": {"name": "Studio Smoke A"}}}));
        let lb = created(serde_json::json!({"body": {"CreateLiveSet": {"name": "Studio Smoke B"}}}));
        let sl = created(serde_json::json!({"body": {"CreateSetlist": {"name": "Studio Smoke Draft"}}}));
        let other = created(serde_json::json!({"body": {"CreateSetlist": {"name": "Studio Smoke Other"}}}));
        ok(serde_json::json!({"body": {"AddLiveSetToSetlist": {"setlist_id": sl, "live_set_id": la}}}));
        ok(serde_json::json!({"body": {"AddLiveSetToSetlist": {"setlist_id": sl, "live_set_id": lb, "index": 0}}})); // at index 0
        let r = read(&sl);
        assert_eq!(r["entries"].as_array().unwrap().iter().map(|e| e["name"].as_str().unwrap()).collect::<Vec<_>>(), vec!["Studio Smoke B", "Studio Smoke A"]);
        // a duplicate and an unknown setlist are typed errors the page shows inline
        let dup = call(serde_json::json!({"body": {"AddLiveSetToSetlist": {"setlist_id": sl, "live_set_id": la}}}));
        assert_eq!(dup["body"]["Error"]["code"], 24);
        let unknown = call(serde_json::json!({"body": {"RenameSetlist": {"id": "no-such", "name": "x"}}}));
        assert_eq!(unknown["body"]["Error"]["code"], 22);
        // the full permutation: Live Sets, then setlists
        ok(serde_json::json!({"body": {"ReorderLiveSetsInSetlist": {"setlist_id": sl, "live_set_ids": [la, lb]}}}));
        ok(serde_json::json!({"body": {"ReorderLiveSetsInSetlist": {"setlist_id": sl, "live_set_ids": [lb, la]}}}));
        let bad = call(serde_json::json!({"body": {"ReorderLiveSetsInSetlist": {"setlist_id": sl, "live_set_ids": [lb]}}}));
        assert_eq!(bad["body"]["Error"]["code"], 31); // never partial
        let all_ids: Vec<String> = read("")["setlists"].as_array().unwrap().iter().map(|s| s["id"].as_str().unwrap().to_string()).collect();
        assert!(all_ids.len() >= 2, "the engine lists the setlists just created");
        let mut rev = all_ids.clone();
        rev.reverse();
        ok(serde_json::json!({"body": {"ReorderSetlists": {"ids": rev}}}));
        assert_eq!(read("")["setlists"][0]["id"], serde_json::json!(rev[0]));
        // a cross-setlist move as Studio does it: Add to the target, then Remove from the source
        ok(serde_json::json!({"body": {"AddLiveSetToSetlist": {"setlist_id": other, "live_set_id": la}}}));
        ok(serde_json::json!({"body": {"RemoveLiveSetFromSetlist": {"setlist_id": sl, "live_set_id": la}}}));
        assert_eq!(read(&other)["entries"][0]["name"], "Studio Smoke A");
        ok(serde_json::json!({"body": {"AddLiveSetToSetlist": {"setlist_id": sl, "live_set_id": la}}}));
        ok(serde_json::json!({"body": {"RemoveLiveSetFromSetlist": {"setlist_id": other, "live_set_id": la}}}));
        // tags
        let tag = created(serde_json::json!({"body": {"CreateTag": {"name": "Studio Smoke Tag draft"}}}));
        ok(serde_json::json!({"body": {"RenameTag": {"id": tag, "name": "Studio Smoke Tag"}}}));
        ok(serde_json::json!({"body": {"SetSetlistTags": {"setlist_id": sl, "tag_ids": [tag]}}}));
        ok(serde_json::json!({"body": {"RenameSetlist": {"id": sl, "name": "Studio Smoke Setlist"}}}));
        ok(serde_json::json!({"body": {"DeleteSetlist": {"id": other}}}));
        let r = read(&sl);
        let mine = r["setlists"].as_array().unwrap().iter().find(|s| s["id"] == serde_json::json!(sl)).expect("the setlist is listed");
        assert_eq!(mine["name"], "Studio Smoke Setlist");
        assert_eq!(mine["tag_ids"], serde_json::json!([tag]));
        assert_eq!(mine["live_set_count"], 2);
        assert_eq!(mine["missing_count"], 0);
        assert!(r["tags"].as_array().unwrap().iter().any(|t| t["name"] == "Studio Smoke Tag"));
        assert!(!r["setlists"].as_array().unwrap().iter().any(|s| s["id"] == serde_json::json!(other)));
        assert_eq!(r["entries"].as_array().unwrap().iter().map(|e| e["name"].as_str().unwrap()).collect::<Vec<_>>(), vec!["Studio Smoke B", "Studio Smoke A"]);
        // the Live Sets are still there after the removals (a removal never deletes a Live Set)
        assert!(r["all_live_sets"].as_array().unwrap().iter().any(|l| l["name"] == "Studio Smoke A"));
        eprintln!("real_bridge_setlists_round_trip: ok, {} setlists", r["setlists"].as_array().unwrap().len());
    }
}
