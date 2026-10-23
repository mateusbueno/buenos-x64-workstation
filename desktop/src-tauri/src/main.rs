// Prevents additional console window on Windows in release, DO NOT REMOVE!!
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

// Generated ws.v1 protobuf types from proto/ws_v1.proto.
// Contains NO device-only code (pure protocol schema & data types).
pub mod ws_v1 {
    include!(concat!(env!("OUT_DIR"), "/ws.v1.rs"));
}

#[derive(serde::Serialize)]
pub struct ConnectionInfo {
    pub connected: bool,
    pub device_name: Option<String>,
    pub transport: Option<String>,
}

#[derive(serde::Serialize)]
pub struct BackupInfo {
    pub last_backup_timestamp: Option<String>,
}

#[tauri::command]
fn get_connection_status() -> ConnectionInfo {
    ConnectionInfo {
        connected: false,
        device_name: None,
        transport: None,
    }
}

#[tauri::command]
fn get_backup_status() -> BackupInfo {
    BackupInfo {
        last_backup_timestamp: None,
    }
}

fn main() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            get_connection_status,
            get_backup_status,
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

    #[test]
    fn test_offline_initial_state() {
        let conn = super::get_connection_status();
        assert!(!conn.connected);
        assert!(conn.device_name.is_none());

        let backup = super::get_backup_status();
        assert!(backup.last_backup_timestamp.is_none());
    }
}
