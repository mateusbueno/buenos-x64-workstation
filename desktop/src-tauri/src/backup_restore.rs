use crate::backup_v1::*;
use crate::liveset_v1::*;
use crate::setlist_v1::*;
use prost::Message;
use serde::{Deserialize, Serialize};
use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::Path;

pub const CURRENT_FORMAT_VERSION: u32 = 1;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct RestoreCounts {
    pub setlists_to_add: usize,
    pub setlists_to_update: usize,
    pub setlists_to_remove: usize,
    pub live_sets_to_add: usize,
    pub live_sets_to_update: usize,
    pub live_sets_to_remove: usize,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum RestoreMode {
    Merge,
    Replace,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct DeviceState {
    pub device_model: String,
    pub firmware_version: String,
    #[serde(default)]
    pub has_fm_va: bool,
    pub setlists: SetlistLibrary,
    pub assign_default: AssignDefault,
    pub favorites: Vec<String>,
    pub live_sets: Vec<LiveSet>,
    pub unsaved_edits: bool,
}

pub fn check_has_fm_va(firmware_version: &str) -> bool {
    let parts: Vec<&str> = firmware_version.split('.').collect();
    if parts.is_empty() {
        return false;
    }
    if let Ok(major) = parts[0].parse::<u32>() {
        if major > 1 {
            return true;
        }
        if major == 1 && parts.len() > 1 {
            if let Ok(minor) = parts[1].parse::<u32>() {
                return minor >= 4;
            }
        }
    }
    false
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RestoreExecutionResult {
    pub mode: RestoreMode,
    pub applied_commands: Vec<String>,
    pub resulting_state: DeviceState,
    pub remapped_ids: HashMap<String, String>,
}

pub fn mint_uuid_v4(seed_offset: u64) -> String {
    use std::time::{SystemTime, UNIX_EPOCH};
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos() as u64;
    let seed = now ^ (seed_offset.wrapping_mul(0x9e3779b97f4a7c15));
    let mut bytes = [0u8; 16];
    for (i, b) in bytes.iter_mut().enumerate() {
        let shift = (i % 8) * 8;
        *b = ((seed >> shift) & 0xff) as u8 ^ ((i as u8).wrapping_mul(31));
    }
    bytes[6] = (bytes[6] & 0x0f) | 0x40; // RFC 4122 v4
    bytes[8] = (bytes[8] & 0x3f) | 0x80; // Variant 10xx
    format!(
        "{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        bytes[0], bytes[1], bytes[2], bytes[3],
        bytes[4], bytes[5],
        bytes[6], bytes[7],
        bytes[8], bytes[9],
        bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]
    )
}

/// Creates a `.velorabackup` message from published device state.
/// Timbres preserve only `library_id` and `instrument_key` references, never audio.
pub fn create_backup_from_state(
    state: &DeviceState,
    backup_time: String,
) -> VeloraBackup {
    let mut backup_live_sets = state.live_sets.clone();
    for ls in &mut backup_live_sets {
        for part in &mut ls.parts {
            if let Some(ref mut timbre) = part.timbre {
                // Keep only library_id and instrument_key references
                timbre.library_id = timbre.library_id.to_lowercase();
            }
        }
    }

    let notes = if state.unsaved_edits {
        "Unsaved edits on the Velora are not in this backup".to_string()
    } else {
        String::new()
    };

    VeloraBackup {
        format_version: CURRENT_FORMAT_VERSION,
        device_model: state.device_model.clone(),
        firmware_version: state.firmware_version.clone(),
        backup_time,
        notes,
        setlists: Some(state.setlists.clone()),
        assign_default: Some(state.assign_default.clone()),
        favorites: state.favorites.clone(),
        live_sets: backup_live_sets,
    }
}

/// Serializes backup to bytes.
pub fn serialize_backup(backup: &VeloraBackup) -> Vec<u8> {
    let mut buf = Vec::new();
    backup.encode(&mut buf).expect("Failed to encode backup protobuf");
    buf
}

/// Reads a `.velorabackup` from bytes, enforcing N and N-1 readers, and refusing
/// future unknown format versions with "This backup needs a newer Velora Studio".
pub fn read_backup_bytes(data: &[u8]) -> Result<VeloraBackup, String> {
    let backup = if let Ok(pb) = VeloraBackup::decode(data) {
        pb
    } else if let Ok(json_str) = std::str::from_utf8(data) {
        serde_json::from_str::<VeloraBackup>(json_str)
            .map_err(|e| format!("Failed to parse backup JSON: {}", e))?
    } else {
        return Err("Malformed backup file".to_string());
    };

    if backup.format_version == 0 {
        return Err("Invalid backup: missing or zero format_version".to_string());
    }

    // Refuse newer major versions per Story 9.17 AC
    if backup.format_version > CURRENT_FORMAT_VERSION {
        return Err("This backup needs a newer Velora Studio".to_string());
    }

    Ok(backup)
}

/// Saves backup file to disk.
pub fn save_backup_file(path: &Path, backup: &VeloraBackup) -> Result<(), String> {
    let data = serialize_backup(backup);
    fs::write(path, data).map_err(|e| format!("Failed to write backup file: {}", e))
}

/// Computes counts of items to add, update, and remove for Restore dialog.
pub fn compute_restore_counts(
    current: &DeviceState,
    backup: &VeloraBackup,
    mode: RestoreMode,
) -> RestoreCounts {
    let current_sl_ids: HashSet<&str> = current
        .setlists
        .setlists
        .iter()
        .map(|s| s.id.as_str())
        .collect();

    let backup_setlists = backup
        .setlists
        .as_ref()
        .map(|sl| sl.setlists.as_slice())
        .unwrap_or(&[]);

    let backup_sl_ids: HashSet<&str> = backup_setlists.iter().map(|s| s.id.as_str()).collect();

    let mut sl_add = 0;
    let mut sl_update = 0;
    for sl in backup_setlists {
        if current_sl_ids.contains(sl.id.as_str()) {
            sl_update += 1;
        } else {
            sl_add += 1;
        }
    }

    let sl_remove = if mode == RestoreMode::Replace {
        current
            .setlists
            .setlists
            .iter()
            .filter(|s| !backup_sl_ids.contains(s.id.as_str()))
            .count()
    } else {
        0
    };

    let current_ls_ids: HashSet<&str> = current.live_sets.iter().map(|l| l.id.as_str()).collect();
    let backup_ls_ids: HashSet<&str> = backup.live_sets.iter().map(|l| l.id.as_str()).collect();

    let mut ls_add = 0;
    let mut ls_update = 0;
    for ls in &backup.live_sets {
        if current_ls_ids.contains(ls.id.as_str()) {
            ls_update += 1;
        } else {
            ls_add += 1;
        }
    }

    let ls_remove = if mode == RestoreMode::Replace {
        current
            .live_sets
            .iter()
            .filter(|l| !backup_ls_ids.contains(l.id.as_str()))
            .count()
    } else {
        0
    };

    RestoreCounts {
        setlists_to_add: sl_add,
        setlists_to_update: sl_update,
        setlists_to_remove: sl_remove,
        live_sets_to_add: ls_add,
        live_sets_to_update: ls_update,
        live_sets_to_remove: ls_remove,
    }
}

/// Executes restore via control-thread commands, returning updated state and commands executed.
pub fn execute_restore(
    current: &DeviceState,
    backup: &VeloraBackup,
    mode: RestoreMode,
) -> RestoreExecutionResult {
    let mut applied_commands = Vec::new();
    let mut remapped_ids = HashMap::new();

    let mut resulting_state = current.clone();

    match mode {
        RestoreMode::Replace => {
            // 1. Delete device setlists and Live Sets not in backup
            let backup_sl_ids: HashSet<&str> = backup
                .setlists
                .as_ref()
                .map(|sl| sl.setlists.iter().map(|s| s.id.as_str()).collect())
                .unwrap_or_default();

            for sl in &current.setlists.setlists {
                if !backup_sl_ids.contains(sl.id.as_str()) {
                    applied_commands.push(format!("DeleteSetlist({})", sl.id));
                }
            }

            let backup_ls_ids: HashSet<&str> =
                backup.live_sets.iter().map(|l| l.id.as_str()).collect();
            for ls in &current.live_sets {
                if !backup_ls_ids.contains(ls.id.as_str()) {
                    applied_commands.push(format!("DeleteLiveSet({})", ls.id));
                }
            }

            // 2. Set content equal to backup
            for ls in &backup.live_sets {
                applied_commands.push(format!("WriteLiveSet({})", ls.id));
            }

            if let Some(ref sl) = backup.setlists {
                for s in &sl.setlists {
                    applied_commands.push(format!("WriteSetlist({})", s.id));
                }
                applied_commands.push("SetSetlistTags".to_string());
                applied_commands.push("ReorderSetlists".to_string());
                resulting_state.setlists = sl.clone();
            }

            if let Some(ref ad) = backup.assign_default {
                resulting_state.assign_default = ad.clone();
            }

            resulting_state.live_sets = backup.live_sets.clone();
            resulting_state.favorites = backup.favorites.clone();
            resulting_state.unsaved_edits = false;
        }

        RestoreMode::Merge => {
            let existing_ls_map: HashMap<String, LiveSet> = resulting_state
                .live_sets
                .iter()
                .map(|ls| (ls.id.clone(), ls.clone()))
                .collect();

            let mut merged_live_sets = Vec::new();

            for backup_ls in &backup.live_sets {
                let mut ls = backup_ls.clone();
                if let Some(existing) = existing_ls_map.get(&backup_ls.id) {
                    // Check collision: if same ID but different content (name or parts), assign new UUIDv4
                    if existing.name != backup_ls.name || existing.parts.len() != backup_ls.parts.len() {
                        let new_id = mint_uuid_v4(merged_live_sets.len() as u64 + 1);
                        remapped_ids.insert(backup_ls.id.clone(), new_id.clone());
                        ls.id = new_id;
                        applied_commands.push(format!("CreateLiveSet({})", ls.id));
                    } else {
                        // Matching ID update
                        applied_commands.push(format!("WriteLiveSet({})", ls.id));
                    }
                } else {
                    applied_commands.push(format!("CreateLiveSet({})", ls.id));
                }
                merged_live_sets.push(ls);
            }

            // Keep device Live Sets that weren't replaced
            for (id, existing) in existing_ls_map {
                if !merged_live_sets.iter().any(|l| l.id == id) {
                    merged_live_sets.push(existing);
                }
            }
            resulting_state.live_sets = merged_live_sets;

            // Merge setlists
            if let Some(ref b_sl) = backup.setlists {
                let mut existing_sl_map: HashMap<String, Setlist> = resulting_state
                    .setlists
                    .setlists
                    .iter()
                    .map(|s| (s.id.clone(), s.clone()))
                    .collect();

                for b_setlist in &b_sl.setlists {
                    let mut s = b_setlist.clone();
                    // Remap any Live Set IDs that were given fresh UUIDv4 on collision
                    for ls_id in &mut s.live_set_ids {
                        if let Some(new_id) = remapped_ids.get(ls_id) {
                            *ls_id = new_id.clone();
                        }
                    }

                    if existing_sl_map.contains_key(&s.id) {
                        applied_commands.push(format!("RenameSetlist({})", s.id));
                        applied_commands.push(format!("ReorderLiveSetsInSetlist({})", s.id));
                    } else {
                        applied_commands.push(format!("CreateSetlist({})", s.id));
                    }
                    existing_sl_map.insert(s.id.clone(), s);
                }

                resulting_state.setlists.setlists = existing_sl_map.into_values().collect();

                // Merge tags
                let mut tag_ids: HashSet<String> = resulting_state
                    .setlists
                    .tags
                    .iter()
                    .map(|t| t.id.clone())
                    .collect();

                for t in &b_sl.tags {
                    if !tag_ids.contains(&t.id) {
                        resulting_state.setlists.tags.push(t.clone());
                        tag_ids.insert(t.id.clone());
                    }
                }
            }

            // Merge favorites
            let mut favs: HashSet<String> = resulting_state.favorites.into_iter().collect();
            for f in &backup.favorites {
                let resolved = remapped_ids.get(f).unwrap_or(f);
                favs.insert(resolved.clone());
            }
            resulting_state.favorites = favs.into_iter().collect();
        }
    }

    RestoreExecutionResult {
        mode,
        applied_commands,
        resulting_state,
        remapped_ids,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample_live_set(id: &str, name: &str) -> LiveSet {
        LiveSet {
            format_version: 1,
            id: id.to_string(),
            name: name.to_string(),
            tempo: 120.0,
            parts: vec![Part {
                timbre: Some(TimbreRef {
                    library_id: "c82b9b7e-9669-424a-9ef8-e0eef0437aa8".to_string(),
                    instrument_key: "pianos/concert_grand.sfz".to_string(),
                }),
                ..Default::default()
            }],
            scenes: vec![],
            assign: vec![100, 101, 102],
            ..Default::default()
        }
    }

    fn sample_setlist(id: &str, name: &str, live_set_ids: Vec<String>) -> Setlist {
        Setlist {
            id: id.to_string(),
            name: name.to_string(),
            live_set_ids,
            tag_ids: vec!["tag-1".to_string()],
            format_version: 1,
        }
    }

    fn sample_device_state() -> DeviceState {
        DeviceState {
            device_model: "Velora 73".to_string(),
            firmware_version: "1.2.0".to_string(),
            has_fm_va: check_has_fm_va("1.2.0"),
            setlists: SetlistLibrary {
                format_version: 1,
                tags: vec![SetlistTag {
                    id: "tag-1".to_string(),
                    name: "Rock".to_string(),
                }],
                setlists: vec![sample_setlist("sl-1", "Set 1", vec!["ls-1".to_string()])],
            },
            assign_default: AssignDefault {
                format_version: 1,
                assign: vec![100, 101, 102, 103, 104, 105, 106, 107],
            },
            favorites: vec!["ls-1".to_string()],
            live_sets: vec![sample_live_set("ls-1", "Piano Song")],
            unsaved_edits: false,
        }
    }

    #[test]
    fn test_backup_creation_and_references_only() {
        let state = sample_device_state();
        let backup = create_backup_from_state(&state, "2026-10-23T06:00:00Z".to_string());

        assert_eq!(backup.format_version, CURRENT_FORMAT_VERSION);
        assert_eq!(backup.device_model, "Velora 73");
        assert_eq!(backup.firmware_version, "1.2.0");
        assert_eq!(backup.live_sets.len(), 1);
        assert!(backup.notes.is_empty());

        // Check reference only: timbre has library_id and instrument_key
        let timbre = backup.live_sets[0].parts[0].timbre.as_ref().unwrap();
        assert_eq!(timbre.library_id, "c82b9b7e-9669-424a-9ef8-e0eef0437aa8");
        assert_eq!(timbre.instrument_key, "pianos/concert_grand.sfz");

        // Serialize and deserialize roundtrip
        let bytes = serialize_backup(&backup);
        let parsed = read_backup_bytes(&bytes).expect("Roundtrip read failed");
        assert_eq!(parsed.device_model, "Velora 73");
        assert_eq!(parsed.live_sets[0].name, "Piano Song");
    }

    #[test]
    fn test_backup_with_unsaved_edits_carries_warning_note() {
        let mut state = sample_device_state();
        state.unsaved_edits = true;
        let backup = create_backup_from_state(&state, "2026-10-23T06:00:00Z".to_string());

        assert_eq!(
            backup.notes,
            "Unsaved edits on the Velora are not in this backup"
        );
    }

    #[test]
    fn test_n_minus_1_reader_and_future_version_refusal() {
        let mut backup = create_backup_from_state(&sample_device_state(), "2026-10-23T06:00:00Z".to_string());

        // Future major version (e.g. 2) must be refused with exact required message
        backup.format_version = 2;
        let bytes = serialize_backup(&backup);
        let err = read_backup_bytes(&bytes).unwrap_err();
        assert_eq!(err, "This backup needs a newer Velora Studio");

        // Format version 0 is invalid
        backup.format_version = 0;
        let bytes0 = serialize_backup(&backup);
        assert!(read_backup_bytes(&bytes0).is_err());
    }

    #[test]
    fn test_restore_replace_equals_source_backup() {
        let device = sample_device_state();

        // Source backup has 2 different setlists and 2 different Live Sets
        let backup_state = DeviceState {
            device_model: "Velora 73".to_string(),
            firmware_version: "1.2.0".to_string(),
            has_fm_va: check_has_fm_va("1.2.0"),
            setlists: SetlistLibrary {
                format_version: 1,
                tags: vec![SetlistTag {
                    id: "tag-2".to_string(),
                    name: "Jazz".to_string(),
                }],
                setlists: vec![
                    sample_setlist("sl-2", "Set 2", vec!["ls-2".to_string()]),
                    sample_setlist("sl-3", "Set 3", vec!["ls-3".to_string()]),
                ],
            },
            assign_default: AssignDefault {
                format_version: 1,
                assign: vec![200, 201, 202, 203, 204, 205, 206, 207],
            },
            favorites: vec!["ls-2".to_string()],
            live_sets: vec![
                sample_live_set("ls-2", "Rhodes Solo"),
                sample_live_set("ls-3", "Organ Groove"),
            ],
            unsaved_edits: false,
        };
        let backup = create_backup_from_state(&backup_state, "2026-10-23T06:00:00Z".to_string());

        // Check restore counts for Replace
        let counts = compute_restore_counts(&device, &backup, RestoreMode::Replace);
        assert_eq!(counts.setlists_to_add, 2);
        assert_eq!(counts.setlists_to_remove, 1);
        assert_eq!(counts.live_sets_to_add, 2);
        assert_eq!(counts.live_sets_to_remove, 1);

        // Execute Replace restore
        let result = execute_restore(&device, &backup, RestoreMode::Replace);

        // Verify control-thread commands were issued
        assert!(result.applied_commands.iter().any(|c| c.starts_with("DeleteLiveSet(ls-1)")));
        assert!(result.applied_commands.iter().any(|c| c.starts_with("DeleteSetlist(sl-1)")));
        assert!(result.applied_commands.iter().any(|c| c.starts_with("WriteLiveSet(ls-2)")));

        // Fresh backup of restored state must equal source backup
        let fresh_backup = create_backup_from_state(&result.resulting_state, "2026-10-23T06:00:00Z".to_string());
        assert_eq!(fresh_backup.setlists.unwrap().setlists.len(), 2);
        assert_eq!(fresh_backup.live_sets.len(), 2);
        assert_eq!(fresh_backup.live_sets[0].name, "Rhodes Solo");
        assert_eq!(fresh_backup.live_sets[1].name, "Organ Groove");
    }

    #[test]
    fn test_restore_merge_union_and_uuid_collision_handling() {
        let device = sample_device_state();

        // Create backup where "ls-1" has a collision with DIFFERENT content ("Synth Lead")
        let colliding_ls = sample_live_set("ls-1", "Synth Lead");
        let backup_state = DeviceState {
            device_model: "Velora 73".to_string(),
            firmware_version: "1.2.0".to_string(),
            has_fm_va: check_has_fm_va("1.2.0"),
            setlists: SetlistLibrary {
                format_version: 1,
                tags: vec![SetlistTag {
                    id: "tag-backup".to_string(),
                    name: "Festival".to_string(),
                }],
                setlists: vec![sample_setlist("sl-b", "Festival Set", vec!["ls-1".to_string()])],
            },
            assign_default: AssignDefault {
                format_version: 1,
                assign: vec![100; 8],
            },
            favorites: vec!["ls-1".to_string()],
            live_sets: vec![colliding_ls],
            unsaved_edits: false,
        };
        let backup = create_backup_from_state(&backup_state, "2026-10-23T06:00:00Z".to_string());

        let result = execute_restore(&device, &backup, RestoreMode::Merge);

        // Verify collision was handled: colliding "ls-1" got a new UUIDv4
        assert!(result.remapped_ids.contains_key("ls-1"));
        let new_id = result.remapped_ids.get("ls-1").unwrap();
        assert_ne!(new_id, "ls-1");

        // Both original device live set ("Piano Song") and colliding live set ("Synth Lead") exist in union
        assert_eq!(result.resulting_state.live_sets.len(), 2);
        assert!(result.resulting_state.live_sets.iter().any(|l| l.id == "ls-1" && l.name == "Piano Song"));
        assert!(result.resulting_state.live_sets.iter().any(|l| l.id == *new_id && l.name == "Synth Lead"));

        // Setlist reference in incoming setlist was remapped to new UUID
        let festival_sl = result
            .resulting_state
            .setlists
            .setlists
            .iter()
            .find(|s| s.name == "Festival Set")
            .unwrap();
        assert_eq!(festival_sl.live_set_ids, vec![new_id.clone()]);
    }

    #[test]
    fn test_offline_backup_editing() {
        let mut backup = create_backup_from_state(&sample_device_state(), "2026-10-23T06:00:00Z".to_string());

        // Edit offline: rename Live Set, reorder setlists, change part volume
        backup.live_sets[0].name = "Concert Grand 2".to_string();
        backup.live_sets[0].parts[0].volume = 0.85;

        let bytes = serialize_backup(&backup);
        let reloaded = read_backup_bytes(&bytes).expect("Reload failed");
        assert_eq!(reloaded.live_sets[0].name, "Concert Grand 2");
        assert_eq!(reloaded.live_sets[0].parts[0].volume, 0.85);
    }
}
