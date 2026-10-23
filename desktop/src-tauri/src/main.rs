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

#[derive(serde::Serialize, Clone, Debug)]
pub struct ConnectionInfo {
    pub connected: bool,
    pub device_name: Option<String>,
    pub transport: Option<String>,
    pub has_unsaved_edits: bool,
    pub firmware_version: String,
    pub has_fm_va: bool,
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
    let fw = "1.4.0".to_string();
    let has_fm_va = check_has_fm_va(&fw);

    DeviceState {
        device_model: "Velora 73".to_string(),
        firmware_version: fw,
        has_fm_va,
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
                    // Part 1: Concert Grand (Sample)
                    liveset_v1::Part {
                        volume: 0.72,
                        timbre: Some(liveset_v1::TimbreRef {
                            library_id: "c82b9b7e-9669-424a-9ef8-e0eef0437aa8".to_string(),
                            instrument_key: "pianos/concert_grand.sfz".to_string(),
                        }),
                        engine: Some(liveset_v1::EngineData {
                            engine_type_id: 1,
                            params: vec![],
                        }),
                        ..Default::default()
                    },
                    // Part 2: Glass Keys (FM)
                    liveset_v1::Part {
                        volume: 0.65,
                        timbre: Some(liveset_v1::TimbreRef {
                            library_id: "e9f01234-5678-9abc-def0-123456789abc".to_string(),
                            instrument_key: "epianos/glass_keys.syx".to_string(),
                        }),
                        engine: Some(liveset_v1::EngineData {
                            engine_type_id: 3, // FM
                            params: vec![
                                liveset_v1::ParamEntry { param_id: 1000, value: 5.0 },  // Algorithm 5
                                liveset_v1::ParamEntry { param_id: 1001, value: 4.0 },  // Feedback 4
                                liveset_v1::ParamEntry { param_id: 1019, value: 64.0 }, // Brightness 64
                                liveset_v1::ParamEntry { param_id: 1028, value: 99.0 }, // OP1 level 99
                                liveset_v1::ParamEntry { param_id: 1042, value: 78.0 }, // OP2 level 78
                                liveset_v1::ParamEntry { param_id: 1056, value: 99.0 }, // OP3 level 99
                                liveset_v1::ParamEntry { param_id: 1070, value: 62.0 }, // OP4 level 62
                                liveset_v1::ParamEntry { param_id: 1084, value: 90.0 }, // OP5 level 90
                                liveset_v1::ParamEntry { param_id: 1098, value: 41.0 }, // OP6 level 41
                            ],
                        }),
                        ..Default::default()
                    },
                    // Part 3: Grand Piano (Sample)
                    liveset_v1::Part {
                        volume: 0.58,
                        timbre: Some(liveset_v1::TimbreRef {
                            library_id: "c82b9b7e-9669-424a-9ef8-e0eef0437aa8".to_string(),
                            instrument_key: "pianos/grand_piano.sfz".to_string(),
                        }),
                        engine: Some(liveset_v1::EngineData {
                            engine_type_id: 1,
                            params: vec![],
                        }),
                        ..Default::default()
                    },
                    // Part 4: Warm Saw Pad (VA)
                    liveset_v1::Part {
                        volume: 0.55,
                        timbre: Some(liveset_v1::TimbreRef {
                            library_id: "d19c3b8f-1234-4567-89ab-cdef01234567".to_string(),
                            instrument_key: "synths/warm_saw_pad.va".to_string(),
                        }),
                        engine: Some(liveset_v1::EngineData {
                            engine_type_id: 4, // VA
                            params: vec![
                                liveset_v1::ParamEntry { param_id: 1200, value: 0.0 },  // Osc 1 Saw
                                liveset_v1::ParamEntry { param_id: 1205, value: 1.0 },  // Osc 2 Square
                                liveset_v1::ParamEntry { param_id: 1231, value: 74.0 }, // Cutoff 74
                                liveset_v1::ParamEntry { param_id: 1232, value: 22.0 }, // Resonance 22
                                liveset_v1::ParamEntry { param_id: 1260, value: 0.0 },  // Poly
                                liveset_v1::ParamEntry { param_id: 1266, value: 64.0 }, // Brightness 64
                            ],
                        }),
                        ..Default::default()
                    },
                    // Part 5: Strings (Sample)
                    liveset_v1::Part {
                        volume: 0.58,
                        timbre: Some(liveset_v1::TimbreRef {
                            library_id: "f1234567-89ab-cdef-0123-456789abcdef".to_string(),
                            instrument_key: "strings/strings.sfz".to_string(),
                        }),
                        engine: Some(liveset_v1::EngineData {
                            engine_type_id: 1,
                            params: vec![],
                        }),
                        ..Default::default()
                    },
                    // Part 6: Soft Bass (Sample)
                    liveset_v1::Part {
                        volume: 0.64,
                        timbre: Some(liveset_v1::TimbreRef {
                            library_id: "f1234567-89ab-cdef-0123-456789abcdef".to_string(),
                            instrument_key: "bass/soft_bass.sfz".to_string(),
                        }),
                        engine: Some(liveset_v1::EngineData {
                            engine_type_id: 1,
                            params: vec![],
                        }),
                        ..Default::default()
                    },
                    // Part 7: Brass Stab (Sample)
                    liveset_v1::Part {
                        volume: 0.50,
                        timbre: Some(liveset_v1::TimbreRef {
                            library_id: "f1234567-89ab-cdef-0123-456789abcdef".to_string(),
                            instrument_key: "brass/brass_stab.sfz".to_string(),
                        }),
                        engine: Some(liveset_v1::EngineData {
                            engine_type_id: 1,
                            params: vec![],
                        }),
                        ..Default::default()
                    },
                    // Part 8: Choir (Sample)
                    liveset_v1::Part {
                        volume: 0.44,
                        timbre: Some(liveset_v1::TimbreRef {
                            library_id: "f1234567-89ab-cdef-0123-456789abcdef".to_string(),
                            instrument_key: "voice/choir.sfz".to_string(),
                        }),
                        engine: Some(liveset_v1::EngineData {
                            engine_type_id: 1,
                            params: vec![],
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
                        engine: Some(liveset_v1::EngineData {
                            engine_type_id: 1,
                            params: vec![],
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
    firmware_version: Option<String>,
    state: State<AppState>,
) -> ConnectionInfo {
    let mut conn = state.connection.lock().unwrap();
    let mut dev = state.device.lock().unwrap();
    if let Some(fw) = firmware_version {
        dev.firmware_version = fw.clone();
        dev.has_fm_va = check_has_fm_va(&fw);
        conn.firmware_version = fw;
        conn.has_fm_va = dev.has_fm_va;
    }
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
    dev.unsaved_edits = has_unsaved_edits;

    conn.clone()
}

#[tauri::command]
fn set_firmware_version(version: String, state: State<AppState>) -> ConnectionInfo {
    let mut conn = state.connection.lock().unwrap();
    let mut dev = state.device.lock().unwrap();
    dev.firmware_version = version.clone();
    dev.has_fm_va = check_has_fm_va(&version);
    conn.firmware_version = version;
    conn.has_fm_va = dev.has_fm_va;
    conn.clone()
}

#[tauri::command]
fn update_part_param(
    live_set_id: String,
    part_index: usize,
    param_id: u32,
    value: f32,
    state: State<AppState>,
) -> Result<(), String> {
    let mut dev = state.device.lock().unwrap();
    let ls = dev
        .live_sets
        .iter_mut()
        .find(|l| l.id == live_set_id)
        .ok_or_else(|| format!("LiveSet {} not found", live_set_id))?;
    if part_index >= ls.parts.len() {
        return Err(format!("Part index {} out of range", part_index));
    }
    let part = &mut ls.parts[part_index];
    if let Some(ref mut engine) = part.engine {
        if let Some(entry) = engine.params.iter_mut().find(|p| p.param_id == param_id) {
            entry.value = value;
        } else {
            engine.params.push(liveset_v1::ParamEntry { param_id, value });
        }
    } else {
        part.engine = Some(liveset_v1::EngineData {
            engine_type_id: 1,
            params: vec![liveset_v1::ParamEntry { param_id, value }],
        });
    }
    dev.unsaved_edits = true;
    let mut conn = state.connection.lock().unwrap();
    conn.has_unsaved_edits = true;
    Ok(())
}

#[tauri::command]
fn write_to_device(live_set_id: String, state: State<AppState>) -> Result<(), String> {
    let mut dev = state.device.lock().unwrap();
    let _ls = dev
        .live_sets
        .iter()
        .find(|l| l.id == live_set_id)
        .ok_or_else(|| format!("LiveSet {} not found", live_set_id))?;
    dev.unsaved_edits = false;
    let mut conn = state.connection.lock().unwrap();
    conn.has_unsaved_edits = false;
    Ok(())
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
    let dev = make_default_device_state();
    let app_state = AppState {
        connection: Mutex::new(ConnectionInfo {
            connected: true,
            device_name: Some("Velora 73".to_string()),
            transport: Some("USB".to_string()),
            has_unsaved_edits: false,
            firmware_version: dev.firmware_version.clone(),
            has_fm_va: dev.has_fm_va,
        }),
        device: Mutex::new(dev),
        last_backup_time: Mutex::new(None),
    };

    tauri::Builder::default()
        .manage(app_state)
        .invoke_handler(tauri::generate_handler![
            get_connection_status,
            set_connection_status,
            set_firmware_version,
            update_part_param,
            write_to_device,
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
    use super::*;
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

    #[test]
    fn test_default_device_has_fm_and_va_parts() {
        let dev = make_default_device_state();
        assert_eq!(dev.firmware_version, "1.4.0");
        assert!(dev.has_fm_va);

        let ls = &dev.live_sets[0];
        assert_eq!(ls.parts.len(), 8);

        // Part 2 (index 1) is FM engine (engine_type_id: 3)
        let fm_part = &ls.parts[1];
        let fm_engine = fm_part.engine.as_ref().expect("FM engine data present");
        assert_eq!(fm_engine.engine_type_id, 3);
        // Algorithm 5
        let algo = fm_engine.params.iter().find(|p| p.param_id == 1000).expect("algorithm param");
        assert_eq!(algo.value, 5.0);

        // Part 4 (index 3) is VA engine (engine_type_id: 4)
        let va_part = &ls.parts[3];
        let va_engine = va_part.engine.as_ref().expect("VA engine data present");
        assert_eq!(va_engine.engine_type_id, 4);
        // Cutoff 74
        let cutoff = va_engine.params.iter().find(|p| p.param_id == 1231).expect("cutoff param");
        assert_eq!(cutoff.value, 74.0);
    }

    #[test]
    fn test_firmware_mismatch_detection() {
        // Firmware 1.4.0 or above supports FM/VA
        assert!(check_has_fm_va("1.4.0"));
        assert!(check_has_fm_va("1.4.1"));
        assert!(check_has_fm_va("2.0.0"));

        // Firmware below 1.4.0 (such as 1.2.0 launch firmware) lacks FM/VA
        assert!(!check_has_fm_va("1.2.0"));
        assert!(!check_has_fm_va("1.0.0"));
        assert!(!check_has_fm_va("0.9.5"));
    }

    #[test]
    fn test_fm_va_parameter_update_and_write_persistence() {
        let dev = make_default_device_state();
        let app_state = AppState {
            connection: Mutex::new(ConnectionInfo {
                connected: true,
                device_name: Some("Velora 73".to_string()),
                transport: Some("USB".to_string()),
                has_unsaved_edits: false,
                firmware_version: dev.firmware_version.clone(),
                has_fm_va: dev.has_fm_va,
            }),
            device: Mutex::new(dev),
            last_backup_time: Mutex::new(None),
        };

        // Initially no unsaved edits
        assert!(!app_state.connection.lock().unwrap().has_unsaved_edits);
        assert!(!app_state.device.lock().unwrap().unsaved_edits);

        // Update FM Algorithm to 16 on Part 2 (index 1)
        {
            let mut dev = app_state.device.lock().unwrap();
            let ls = &mut dev.live_sets[0];
            let part = &mut ls.parts[1];
            let engine = part.engine.as_mut().unwrap();
            let algo = engine.params.iter_mut().find(|p| p.param_id == 1000).unwrap();
            algo.value = 16.0;
            dev.unsaved_edits = true;
            let mut conn = app_state.connection.lock().unwrap();
            conn.has_unsaved_edits = true;
        }

        // Live edits are active
        assert!(app_state.connection.lock().unwrap().has_unsaved_edits);
        assert!(app_state.device.lock().unwrap().unsaved_edits);

        // "Write to Velora" persists edits and clears unsaved_edits flag
        {
            let mut dev = app_state.device.lock().unwrap();
            dev.unsaved_edits = false;
            let mut conn = app_state.connection.lock().unwrap();
            conn.has_unsaved_edits = false;
        }
        assert!(!app_state.connection.lock().unwrap().has_unsaved_edits);
        assert!(!app_state.device.lock().unwrap().unsaved_edits);

        // Value was preserved
        let dev = app_state.device.lock().unwrap();
        let algo = dev.live_sets[0].parts[1].engine.as_ref().unwrap()
            .params.iter().find(|p| p.param_id == 1000).unwrap();
        assert_eq!(algo.value, 16.0);
    }
}
