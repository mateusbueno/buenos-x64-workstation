// Prevents additional console window on Windows in release, DO NOT REMOVE!!
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

// Generated ws.v1 protobuf types from proto/ws_v1.proto.
// Contains NO device-only code (pure protocol schema & data types).
pub mod ws {
    pub mod v1 {
        include!(concat!(env!("OUT_DIR"), "/ws.v1.rs"));
    }
}

pub mod velora {
    pub mod setlist {
        pub mod v1 {
            include!(concat!(env!("OUT_DIR"), "/velora.setlist.v1.rs"));
        }
    }
    pub mod liveset {
        pub mod v1 {
            include!(concat!(env!("OUT_DIR"), "/velora.liveset.v1.rs"));
        }
    }
    pub mod backup {
        pub mod v1 {
            include!(concat!(env!("OUT_DIR"), "/velora.backup.v1.rs"));
        }
    }
}

// Flat aliases for convenience
pub use ws::v1 as ws_v1;
pub use velora::setlist::v1 as setlist_v1;
pub use velora::liveset::v1 as liveset_v1;
pub use velora::backup::v1 as backup_v1;

pub mod backup_restore;

use backup_restore::*;
use std::path::Path;
use std::sync::Mutex;
use tauri::State;

#[derive(serde::Serialize, Clone)]
pub struct ConnectionInfo {
    pub connected: bool,
    pub device_name: Option<String>,
    pub transport: Option<String>,
    pub has_unsaved_edits: bool,
}

#[derive(serde::Serialize)]
pub struct BackupInfo {
    pub last_backup_timestamp: Option<String>,
}

pub struct AppState {
    pub connection: Mutex<ConnectionInfo>,
    pub device: Mutex<DeviceState>,
    pub last_backup_time: Mutex<Option<String>>,
}

fn make_default_device_state() -> DeviceState {
    DeviceState {
        device_model: "Velora 73".to_string(),
        firmware_version: "1.2.0".to_string(),
        setlists: setlist_v1::SetlistLibrary {
            format_version: 1,
            tags: vec![
                setlist_v1::SetlistTag {
                    id: "tag-1".to_string(),
                    name: "Live".to_string(),
                },
                setlist_v1::SetlistTag {
                    id: "tag-2".to_string(),
                    name: "Rehearsal".to_string(),
                },
            ],
            setlists: vec![
                setlist_v1::Setlist {
                    id: "sl-wedding".to_string(),
                    name: "Wedding Repertoire".to_string(),
                    live_set_ids: vec!["ls-perfect".to_string(), "ls-grand".to_string()],
                    tag_ids: vec!["tag-1".to_string()],
                    format_version: 1,
                },
            ],
        },
        assign_default: setlist_v1::AssignDefault {
            format_version: 1,
            assign: vec![100, 101, 102, 103, 104, 105, 106, 107],
        },
        favorites: vec!["ls-perfect".to_string()],
        live_sets: vec![
            liveset_v1::LiveSet {
                format_version: 1,
                id: "ls-perfect".to_string(),
                name: "Perfect".to_string(),
                tempo: 72.0,
                parts: vec![
                    liveset_v1::Part {
                        volume: 0.8,
                        timbre: Some(liveset_v1::TimbreRef {
                            library_id: "c82b9b7e-9669-424a-9ef8-e0eef0437aa8".to_string(),
                            instrument_key: "pianos/concert_grand.sfz".to_string(),
                        }),
                        ..Default::default()
                    },
                    liveset_v1::Part {
                        volume: 0.4,
                        timbre: Some(liveset_v1::TimbreRef {
                            library_id: "d19c3b8f-1234-4567-89ab-cdef01234567".to_string(),
                            instrument_key: "synths/warm_pad.sfz".to_string(),
                        }),
                        ..Default::default()
                    },
                ],
                scenes: vec![],
                assign: vec![100, 101, 102],
                ..Default::default()
            },
            liveset_v1::LiveSet {
                format_version: 1,
                id: "ls-grand".to_string(),
                name: "Concert Grand Solo".to_string(),
                tempo: 120.0,
                parts: vec![
                    liveset_v1::Part {
                        volume: 1.0,
                        timbre: Some(liveset_v1::TimbreRef {
                            library_id: "c82b9b7e-9669-424a-9ef8-e0eef0437aa8".to_string(),
                            instrument_key: "pianos/concert_grand.sfz".to_string(),
                        }),
                        ..Default::default()
                    },
                ],
                scenes: vec![],
                assign: vec![100, 101, 102],
                ..Default::default()
            },
        ],
        unsaved_edits: false,
    }
}

#[tauri::command]
fn get_connection_status(state: State<AppState>) -> ConnectionInfo {
    state.connection.lock().unwrap().clone()
}

#[tauri::command]
fn set_connection_status(
    connected: bool,
    has_unsaved_edits: bool,
    state: State<AppState>,
) -> ConnectionInfo {
    let mut conn = state.connection.lock().unwrap();
    conn.connected = connected;
    conn.device_name = if connected {
        Some("Velora 73".to_string())
    } else {
        None
    };
    conn.transport = if connected {
        Some("USB".to_string())
    } else {
        None
    };
    conn.has_unsaved_edits = has_unsaved_edits;

    let mut dev = state.device.lock().unwrap();
    dev.unsaved_edits = has_unsaved_edits;

    conn.clone()
}

#[tauri::command]
fn get_backup_status(state: State<AppState>) -> BackupInfo {
    BackupInfo {
        last_backup_timestamp: state.last_backup_time.lock().unwrap().clone(),
    }
}

#[tauri::command]
fn get_current_device_state(state: State<AppState>) -> DeviceState {
    state.device.lock().unwrap().clone()
}

#[tauri::command]
fn create_backup(file_path: String, state: State<AppState>) -> Result<backup_v1::VeloraBackup, String> {
    let dev = state.device.lock().unwrap();
    let now = chrono_now();
    let backup = create_backup_from_state(&dev, now.clone());

    save_backup_file(Path::new(&file_path), &backup)?;

    let mut last_backup = state.last_backup_time.lock().unwrap();
    *last_backup = Some(now);

    Ok(backup)
}

#[tauri::command]
fn read_backup_file_cmd(file_path: String) -> Result<backup_v1::VeloraBackup, String> {
    let data = std::fs::read(&file_path).map_err(|e| format!("Failed to read file: {}", e))?;
    read_backup_bytes(&data)
}

#[tauri::command]
fn save_backup_file_cmd(file_path: String, backup: backup_v1::VeloraBackup) -> Result<(), String> {
    save_backup_file(Path::new(&file_path), &backup)
}

#[tauri::command]
fn get_restore_counts_cmd(
    file_path: String,
    mode: String,
    state: State<AppState>,
) -> Result<RestoreCounts, String> {
    let data = std::fs::read(&file_path).map_err(|e| format!("Failed to read file: {}", e))?;
    let backup = read_backup_bytes(&data)?;
    let dev = state.device.lock().unwrap();
    let restore_mode = match mode.to_lowercase().as_str() {
        "replace" => RestoreMode::Replace,
        _ => RestoreMode::Merge,
    };
    Ok(compute_restore_counts(&dev, &backup, restore_mode))
}

#[tauri::command]
fn perform_restore_cmd(
    file_path: String,
    mode: String,
    state: State<AppState>,
) -> Result<RestoreExecutionResult, String> {
    let data = std::fs::read(&file_path).map_err(|e| format!("Failed to read file: {}", e))?;
    let backup = read_backup_bytes(&data)?;

    let mut dev = state.device.lock().unwrap();
    let restore_mode = match mode.to_lowercase().as_str() {
        "replace" => RestoreMode::Replace,
        _ => RestoreMode::Merge,
    };

    let result = execute_restore(&dev, &backup, restore_mode);
    *dev = result.resulting_state.clone();

    Ok(result)
}

fn chrono_now() -> String {
    use std::time::{SystemTime, UNIX_EPOCH};
    let now = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_secs();
    let secs = now % 60;
    let mins = (now / 60) % 60;
    let hours = (now / 3600) % 24;
    format!("2026-10-23T{:02}:{:02}:{:02}Z", hours, mins, secs)
}

fn main() {
    let app_state = AppState {
        connection: Mutex::new(ConnectionInfo {
            connected: false,
            device_name: None,
            transport: None,
            has_unsaved_edits: false,
        }),
        device: Mutex::new(make_default_device_state()),
        last_backup_time: Mutex::new(None),
    };

    tauri::Builder::default()
        .manage(app_state)
        .invoke_handler(tauri::generate_handler![
            get_connection_status,
            set_connection_status,
            get_backup_status,
            get_current_device_state,
            create_backup,
            read_backup_file_cmd,
            save_backup_file_cmd,
            get_restore_counts_cmd,
            perform_restore_cmd,
        ])
        .run(tauri::generate_context!())
        .expect("error while running Velora Studio application");
}

#[cfg(test)]
mod tests {
    use super::ws_v1::*;

    #[test]
    fn test_ws_v1_types_linked() {
        let hello = Hello {
            version: ProtocolVersion::ProtocolVersion1 as u32,
            features: vec!["snapshots.v1".to_string(), "params.v1".to_string()],
            client_name: "Velora Studio".to_string(),
        };
        assert_eq!(hello.version, 1);
        assert_eq!(hello.client_name, "Velora Studio");
        assert_eq!(ErrorCode::UnsupportedVersion as i32, 1);
        assert_eq!(ErrorCode::HandshakeRequired as i32, 2);
    }
}
