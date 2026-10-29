use crate::backup_v1::*;
use crate::liveset_v1::*;
use prost::Message;
use serde::{Deserialize, Serialize};
use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;

pub const EXPORT_FORMAT_VERSION: u32 = 1;
pub const TWO_GIGABYTES: u64 = 2 * 1024 * 1024 * 1024;

// Global cancellation tracking
static CANCEL_REQUESTED: AtomicBool = AtomicBool::new(false);
static ACTIVE_EXPORT_PATH: Mutex<Option<PathBuf>> = Mutex::new(None);

pub fn request_cancel() {
    CANCEL_REQUESTED.store(true, Ordering::SeqCst);
    if let Ok(mut guard) = ACTIVE_EXPORT_PATH.lock() {
        if let Some(path) = guard.take() {
            if path.exists() {
                let _ = fs::remove_file(path);
            }
        }
    }
}

pub fn is_cancel_requested() -> bool {
    CANCEL_REQUESTED.load(Ordering::SeqCst)
}

pub fn reset_cancel() {
    CANCEL_REQUESTED.store(false, Ordering::SeqCst);
    if let Ok(mut guard) = ACTIVE_EXPORT_PATH.lock() {
        *guard = None;
    }
}

pub fn set_active_export_path(path: PathBuf) {
    if let Ok(mut guard) = ACTIVE_EXPORT_PATH.lock() {
        *guard = Some(path);
    }
}

pub fn clear_active_export_path() {
    if let Ok(mut guard) = ACTIVE_EXPORT_PATH.lock() {
        *guard = None;
    }
}

/// Known factory / partner library IDs (e.g. founder's factory piano and partner packs)
pub fn is_factory_or_partner_library(library_id: &str) -> bool {
    let lower = library_id.to_lowercase();
    // Default factory concert grand / grand piano library UUID
    if lower == "c82b9b7e-9669-424a-9ef8-e0eef0437aa8" {
        return true;
    }
    if lower.contains("factory") || lower.contains("partner") {
        return true;
    }
    false
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ExportEstimateParams {
    pub scope: String, // "sets" | "timbres" | "bundle"
    pub live_set_ids: Vec<String>,
    pub timbre_keys: Vec<String>, // "library_id:instrument_key"
    pub target_path: Option<String>,
    #[serde(default)]
    pub custom_user_timbre_sizes: HashMap<String, u64>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct ExportEstimateResult {
    pub scope: String,
    pub total_size_bytes: u64,
    pub live_set_count: usize,
    pub timbre_count: usize,
    pub reference_timbre_count: usize,
    pub user_timbre_count: usize,
    pub exceeds_2gb: bool,
    pub free_space_bytes: Option<u64>,
    pub insufficient_free_space: bool,
    pub formatted_size: String,
    pub summary_text: String,
    pub manifest: Vec<ExportItemManifest>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ExportSelectionParams {
    pub target_path: String,
    pub scope: String, // "sets" | "timbres" | "bundle"
    pub live_set_ids: Vec<String>,
    pub timbre_keys: Vec<String>,
    #[serde(default)]
    pub user_bundles: HashMap<String, Vec<u8>>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ExportResult {
    pub success: bool,
    pub file_path: String,
    pub file_name: String,
    pub bytes_written: u64,
    pub live_set_count: usize,
    pub timbre_count: usize,
    pub reference_timbre_count: usize,
    pub announced_message: String,
}

pub fn format_bytes(bytes: u64) -> String {
    if bytes >= 1024 * 1024 * 1024 {
        format!("{:.1} GB", bytes as f64 / (1024.0 * 1024.0 * 1024.0))
    } else if bytes >= 1024 * 1024 {
        format!("{:.1} MB", bytes as f64 / (1024.0 * 1024.0))
    } else if bytes >= 1024 {
        format!("{:.1} KB", bytes as f64 / 1024.0)
    } else {
        format!("{} B", bytes)
    }
}

#[cfg(unix)]
pub fn get_available_disk_space<P: AsRef<Path>>(path: P) -> Option<u64> {
    use std::ffi::CString;
    use std::os::unix::ffi::OsStrExt;
    let target = if path.as_ref().is_dir() {
        path.as_ref().to_path_buf()
    } else {
        path.as_ref()
            .parent()
            .filter(|p| !p.as_os_str().is_empty())
            .map(|p| p.to_path_buf())
            .unwrap_or_else(|| PathBuf::from("."))
    };
    let c_path = CString::new(target.as_os_str().as_bytes()).ok()?;
    unsafe {
        let mut stat: libc::statvfs = std::mem::zeroed();
        if libc::statvfs(c_path.as_ptr(), &mut stat) == 0 {
            Some((stat.f_bavail as u64).saturating_mul(stat.f_frsize as u64))
        } else {
            None
        }
    }
}

#[cfg(not(unix))]
pub fn get_available_disk_space<P: AsRef<Path>>(_path: P) -> Option<u64> {
    None
}

/// Compute manifest and size estimation for export selection
pub fn compute_export_estimate(
    available_live_sets: &[LiveSet],
    params: &ExportEstimateParams,
) -> ExportEstimateResult {
    let scope_str = params.scope.to_lowercase();
    let mut manifest = Vec::new();
    let mut total_size_bytes = 0u64;

    let selected_ls_ids: HashSet<String> = params.live_set_ids.iter().cloned().collect();
    let mut included_live_sets = Vec::new();
    for ls in available_live_sets {
        if selected_ls_ids.contains(&ls.id) {
            included_live_sets.push(ls.clone());
        }
    }

    let mut referenced_timbres: HashMap<String, (String, String, String, bool)> = HashMap::new(); // key -> (lib_id, inst_key, name, is_factory)

    // Process Live Sets
    if scope_str == "sets" || scope_str == "bundle" {
        for ls in &included_live_sets {
            let ls_size = (ls.encoded_len() as u64).max(1024);
            total_size_bytes += ls_size;
            manifest.push(ExportItemManifest {
                id: ls.id.clone(),
                name: ls.name.clone(),
                category: "Live Set".to_string(),
                size_bytes: ls_size,
                is_reference_only: false,
                item_type: "live_set".to_string(),
                library_id: String::new(),
                instrument_key: String::new(),
            });

            // Find referenced timbres in Live Set parts
            if scope_str == "bundle" {
                for part in &ls.parts {
                    if let Some(ref t) = part.timbre {
                        if !t.library_id.is_empty() && !t.instrument_key.is_empty() {
                            let key = format!("{}:{}", t.library_id.to_lowercase(), t.instrument_key);
                            let is_factory = is_factory_or_partner_library(&t.library_id);
                            referenced_timbres.entry(key).or_insert_with(|| {
                                (t.library_id.clone(), t.instrument_key.clone(), t.instrument_key.clone(), is_factory)
                            });
                        }
                    }
                }
            }
        }
    }

    // Process Timbres
    let mut timbre_count = 0usize;
    let mut ref_timbre_count = 0usize;
    let mut user_timbre_count = 0usize;

    if scope_str == "bundle" {
        for (key, (lib_id, inst_key, name, is_factory)) in referenced_timbres {
            timbre_count += 1;
            if is_factory {
                ref_timbre_count += 1;
                manifest.push(ExportItemManifest {
                    id: key,
                    name,
                    category: "Timbre (Reference)".to_string(),
                    size_bytes: 0,
                    is_reference_only: true,
                    item_type: "timbre".to_string(),
                    library_id: lib_id,
                    instrument_key: inst_key,
                });
            } else {
                user_timbre_count += 1;
                let user_size = params.custom_user_timbre_sizes.get(&key).copied().unwrap_or(25 * 1024 * 1024); // default ~25 MB if unknown
                total_size_bytes += user_size;
                manifest.push(ExportItemManifest {
                    id: key,
                    name,
                    category: "User Timbre".to_string(),
                    size_bytes: user_size,
                    is_reference_only: false,
                    item_type: "timbre".to_string(),
                    library_id: lib_id,
                    instrument_key: inst_key,
                });
            }
        }
    } else if scope_str == "timbres" {
        for key in &params.timbre_keys {
            let parts: Vec<&str> = key.splitn(2, ':').collect();
            let lib_id = parts[0].to_string();
            let inst_key = if parts.len() > 1 { parts[1].to_string() } else { String::new() };
            let is_factory = is_factory_or_partner_library(&lib_id);

            timbre_count += 1;
            if is_factory {
                ref_timbre_count += 1;
                manifest.push(ExportItemManifest {
                    id: key.clone(),
                    name: inst_key.clone(),
                    category: "Timbre (Reference)".to_string(),
                    size_bytes: 0,
                    is_reference_only: true,
                    item_type: "timbre".to_string(),
                    library_id: lib_id,
                    instrument_key: inst_key,
                });
            } else {
                user_timbre_count += 1;
                let user_size = params.custom_user_timbre_sizes.get(key).copied().unwrap_or(25 * 1024 * 1024);
                total_size_bytes += user_size;
                manifest.push(ExportItemManifest {
                    id: key.clone(),
                    name: inst_key.clone(),
                    category: "User Timbre".to_string(),
                    size_bytes: user_size,
                    is_reference_only: false,
                    item_type: "timbre".to_string(),
                    library_id: lib_id,
                    instrument_key: inst_key,
                });
            }
        }
    }

    let exceeds_2gb = total_size_bytes > TWO_GIGABYTES;
    let free_space_bytes = params.target_path.as_ref().and_then(get_available_disk_space);
    let insufficient_free_space = if let Some(free) = free_space_bytes {
        total_size_bytes > free
    } else {
        false
    };

    let formatted_size = format_bytes(total_size_bytes);
    let summary_text = format!(
        "About {} · {} Live Sets, {} timbres ({} by reference)",
        formatted_size,
        included_live_sets.len(),
        timbre_count,
        ref_timbre_count
    );

    ExportEstimateResult {
        scope: scope_str,
        total_size_bytes,
        live_set_count: included_live_sets.len(),
        timbre_count,
        reference_timbre_count: ref_timbre_count,
        user_timbre_count,
        exceeds_2gb,
        free_space_bytes,
        insufficient_free_space,
        formatted_size,
        summary_text,
        manifest,
    }
}

/// Serializes VeloraSetsExport to protobuf bytes
pub fn serialize_sets_export(export: &VeloraSetsExport) -> Vec<u8> {
    let mut buf = Vec::new();
    export.encode(&mut buf).expect("Failed to encode VeloraSetsExport");
    buf
}

/// Reads VeloraSetsExport from bytes, validating format_version
pub fn read_sets_export_bytes(data: &[u8]) -> Result<VeloraSetsExport, String> {
    let export = if let Ok(pb) = VeloraSetsExport::decode(data) {
        pb
    } else if let Ok(json_str) = std::str::from_utf8(data) {
        serde_json::from_str::<VeloraSetsExport>(json_str)
            .map_err(|e| format!("Failed to parse VeloraSetsExport JSON: {}", e))?
    } else {
        return Err("Malformed .velorasets file".to_string());
    };

    if export.format_version == 0 {
        return Err("Invalid export: missing or zero format_version".to_string());
    }
    if export.format_version > EXPORT_FORMAT_VERSION {
        return Err("This export needs a newer Velora Studio".to_string());
    }

    Ok(export)
}

/// Serializes VeloraTimbreExport to protobuf bytes
pub fn serialize_timbre_export(export: &VeloraTimbreExport) -> Vec<u8> {
    let mut buf = Vec::new();
    export.encode(&mut buf).expect("Failed to encode VeloraTimbreExport");
    buf
}

/// Reads VeloraTimbreExport from bytes, validating format_version
pub fn read_timbre_export_bytes(data: &[u8]) -> Result<VeloraTimbreExport, String> {
    let export = if let Ok(pb) = VeloraTimbreExport::decode(data) {
        pb
    } else if let Ok(json_str) = std::str::from_utf8(data) {
        serde_json::from_str::<VeloraTimbreExport>(json_str)
            .map_err(|e| format!("Failed to parse VeloraTimbreExport JSON: {}", e))?
    } else {
        return Err("Malformed .veloratimbre file".to_string());
    };

    if export.format_version == 0 {
        return Err("Invalid export: missing or zero format_version".to_string());
    }
    if export.format_version > EXPORT_FORMAT_VERSION {
        return Err("This export needs a newer Velora Studio".to_string());
    }

    Ok(export)
}

/// Security verification scanner: scans bytes to ensure no factory or partner audio content or encryption keys leaked.
pub fn scan_for_vendor_content(bytes: &[u8], factory_library_ids: &[&str]) -> Result<(), String> {
    // Known vendor audio signatures and patterns
    let forbidden_signatures: [&[u8]; 3] = [
        b"VELORA_VENDOR_ENCRYPTED_BLOCK_OR_PCM_AUDIO",
        b"VENDOR_AES_CONTENT_KEY",
        b"FACTORY_AUDIO_MAGIC_SAMPLE_DATA",
    ];

    for sig in &forbidden_signatures {
        if bytes.windows(sig.len()).any(|w| w == *sig) {
            return Err("Security Violation: Vendor audio or key signature found in export payload".to_string());
        }
    }

    // Inspect decoded message if possible
    if let Ok(sets_exp) = VeloraSetsExport::decode(bytes) {
        for t in &sets_exp.timbres {
            let is_vendor = is_factory_or_partner_library(&t.library_id)
                || factory_library_ids.iter().any(|fid| t.library_id.eq_ignore_ascii_case(fid));
            if is_vendor {
                if !t.bundle_data.is_empty() {
                    return Err(format!(
                        "Security Violation: Factory timbre {} contains non-empty bundle audio payload",
                        t.library_id
                    ));
                }
                if !t.is_reference_only {
                    return Err(format!(
                        "Security Violation: Factory timbre {} is not marked reference_only",
                        t.library_id
                    ));
                }
            }
        }
    }

    if let Ok(timbre_exp) = VeloraTimbreExport::decode(bytes) {
        for t in &timbre_exp.timbres {
            let is_vendor = is_factory_or_partner_library(&t.library_id)
                || factory_library_ids.iter().any(|fid| t.library_id.eq_ignore_ascii_case(fid));
            if is_vendor {
                if !t.bundle_data.is_empty() {
                    return Err(format!(
                        "Security Violation: Factory timbre {} contains non-empty bundle audio payload",
                        t.library_id
                    ));
                }
                if !t.is_reference_only {
                    return Err(format!(
                        "Security Violation: Factory timbre {} is not marked reference_only",
                        t.library_id
                    ));
                }
            }
        }
    }

    Ok(())
}

/// Executes the export operation to target_path
pub fn perform_export(
    available_live_sets: &[LiveSet],
    device_model: &str,
    firmware_version: &str,
    export_time: String,
    params: ExportSelectionParams,
) -> Result<ExportResult, String> {
    reset_cancel();
    perform_export_with_cancel(
        available_live_sets,
        device_model,
        firmware_version,
        export_time,
        params,
        &CANCEL_REQUESTED,
    )
}

pub fn perform_export_with_cancel(
    available_live_sets: &[LiveSet],
    device_model: &str,
    firmware_version: &str,
    export_time: String,
    params: ExportSelectionParams,
    cancel_flag: &AtomicBool,
) -> Result<ExportResult, String> {
    let target_path = PathBuf::from(&params.target_path);
    set_active_export_path(target_path.clone());

    let scope_str = params.scope.to_lowercase();

    // 1. Check if cancel was already requested
    if cancel_flag.load(Ordering::SeqCst) {
        clear_active_export_path();
        return Err("Export cancelled".to_string());
    }

    // 2. Validate scope and selection
    if params.live_set_ids.is_empty() && params.timbre_keys.is_empty() {
        clear_active_export_path();
        return Err("Nothing selected for export".to_string());
    }

    if (scope_str == "sets" || scope_str == "bundle") && params.live_set_ids.is_empty() {
        clear_active_export_path();
        return Err("No Live Sets selected for export".to_string());
    }

    if scope_str == "timbres" && params.timbre_keys.is_empty() {
        clear_active_export_path();
        return Err("No timbres selected for export".to_string());
    }

    // 3. Check timbre-only factory exclusion
    if scope_str == "timbres" {
        let has_user_timbre = params.timbre_keys.iter().any(|k| {
            let lib_id = k.split(':').next().unwrap_or(k);
            !is_factory_or_partner_library(lib_id)
        });
        if !has_user_timbre {
            clear_active_export_path();
            return Err("A timbre-only export containing only factory or partner timbres cannot be created.".to_string());
        }
    }

    // 4. Compute estimate and verify free space
    let estimate_params = ExportEstimateParams {
        scope: scope_str.clone(),
        live_set_ids: params.live_set_ids.clone(),
        timbre_keys: params.timbre_keys.clone(),
        target_path: Some(params.target_path.clone()),
        custom_user_timbre_sizes: params.user_bundles.iter().map(|(k, v)| (k.clone(), v.len() as u64)).collect(),
    };
    let estimate = compute_export_estimate(available_live_sets, &estimate_params);

    if estimate.insufficient_free_space {
        clear_active_export_path();
        return Err("Insufficient free disk space at destination".to_string());
    }

    // 5. Build export payload
    let file_name = target_path.file_name().and_then(|n| n.to_str()).unwrap_or("export").to_string();

    let file_bytes = if scope_str == "timbres" {
        let mut export_timbres = Vec::new();
        for key in &params.timbre_keys {
            let parts: Vec<&str> = key.splitn(2, ':').collect();
            let lib_id = parts[0].to_string();
            let inst_key = if parts.len() > 1 { parts[1].to_string() } else { String::new() };
            let is_factory = is_factory_or_partner_library(&lib_id);

            if is_factory {
                // Factory timbres are reference-only at 0 B
                export_timbres.push(ExportTimbrePayload {
                    library_id: lib_id,
                    instrument_key: inst_key,
                    name: key.clone(),
                    category: "Factory".to_string(),
                    is_reference_only: true,
                    bundle_data: Vec::new(),
                });
            } else {
                let bundle = params.user_bundles.get(key).cloned().unwrap_or_default();
                export_timbres.push(ExportTimbrePayload {
                    library_id: lib_id,
                    instrument_key: inst_key,
                    name: key.clone(),
                    category: "User".to_string(),
                    is_reference_only: false,
                    bundle_data: bundle,
                });
            }
        }

        let timbre_export = VeloraTimbreExport {
            format_version: EXPORT_FORMAT_VERSION,
            device_model: device_model.to_string(),
            firmware_version: firmware_version.to_string(),
            export_time,
            manifest: estimate.manifest.clone(),
            timbres: export_timbres,
        };
        serialize_timbre_export(&timbre_export)
    } else {
        // Sets or Bundle scope
        let export_scope = if scope_str == "bundle" {
            ExportScope::Bundle as i32
        } else {
            ExportScope::Sets as i32
        };

        let selected_ls_ids: HashSet<String> = params.live_set_ids.iter().cloned().collect();
        let mut export_live_sets = Vec::new();
        for ls in available_live_sets {
            if selected_ls_ids.contains(&ls.id) {
                export_live_sets.push(ls.clone());
            }
        }

        let mut export_timbres = Vec::new();
        if scope_str == "bundle" {
            for m in &estimate.manifest {
                if m.item_type == "timbre" {
                    if m.is_reference_only || is_factory_or_partner_library(&m.library_id) {
                        export_timbres.push(ExportTimbrePayload {
                            library_id: m.library_id.clone(),
                            instrument_key: m.instrument_key.clone(),
                            name: m.name.clone(),
                            category: m.category.clone(),
                            is_reference_only: true,
                            bundle_data: Vec::new(),
                        });
                    } else {
                        let key = format!("{}:{}", m.library_id.to_lowercase(), m.instrument_key);
                        let bundle = params.user_bundles.get(&key).cloned().unwrap_or_default();
                        export_timbres.push(ExportTimbrePayload {
                            library_id: m.library_id.clone(),
                            instrument_key: m.instrument_key.clone(),
                            name: m.name.clone(),
                            category: m.category.clone(),
                            is_reference_only: false,
                            bundle_data: bundle,
                        });
                    }
                }
            }
        }

        let sets_export = VeloraSetsExport {
            format_version: EXPORT_FORMAT_VERSION,
            device_model: device_model.to_string(),
            firmware_version: firmware_version.to_string(),
            export_time,
            scope: export_scope,
            manifest: estimate.manifest.clone(),
            live_sets: export_live_sets,
            timbres: export_timbres,
        };
        serialize_sets_export(&sets_export)
    };

    // 6. Security scan before writing
    scan_for_vendor_content(&file_bytes, &["c82b9b7e-9669-424a-9ef8-e0eef0437aa8"])?;

    // 7. Check cancel before disk write
    if cancel_flag.load(Ordering::SeqCst) {
        if target_path.exists() {
            let _ = fs::remove_file(&target_path);
        }
        clear_active_export_path();
        return Err("Export cancelled".to_string());
    }

    // 8. Write to disk
    if let Some(parent) = target_path.parent() {
        let _ = fs::create_dir_all(parent);
    }
    if let Err(e) = fs::write(&target_path, &file_bytes) {
        clear_active_export_path();
        return Err(format!("Failed to write export file: {}", e));
    }

    // 9. Check cancel after writing
    if cancel_flag.load(Ordering::SeqCst) {
        if target_path.exists() {
            let _ = fs::remove_file(&target_path);
        }
        clear_active_export_path();
        return Err("Export cancelled".to_string());
    }

    clear_active_export_path();

    let bytes_written = file_bytes.len() as u64;
    let announced_message = format!(
        "Exported {} Live Sets and {} timbres to {}",
        estimate.live_set_count,
        estimate.timbre_count,
        file_name
    );

    Ok(ExportResult {
        success: true,
        file_path: target_path.to_string_lossy().to_string(),
        file_name,
        bytes_written,
        live_set_count: estimate.live_set_count,
        timbre_count: estimate.timbre_count,
        reference_timbre_count: estimate.reference_timbre_count,
        announced_message,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_test_live_sets() -> Vec<LiveSet> {
        vec![
            LiveSet {
                format_version: 1,
                id: "ls-1".to_string(),
                name: "Gig Opener".to_string(),
                tempo: 120.0,
                parts: vec![
                    // Factory Grand Piano
                    Part {
                        volume: 0.8,
                        timbre: Some(TimbreRef {
                            library_id: "c82b9b7e-9669-424a-9ef8-e0eef0437aa8".to_string(),
                            instrument_key: "pianos/concert_grand.sfz".to_string(),
                        }),
                        ..Default::default()
                    },
                    // User Timbre
                    Part {
                        volume: 0.7,
                        timbre: Some(TimbreRef {
                            library_id: "user-lib-123".to_string(),
                            instrument_key: "lead/synth_lead.sfz".to_string(),
                        }),
                        ..Default::default()
                    },
                ],
                ..Default::default()
            },
            LiveSet {
                format_version: 1,
                id: "ls-2".to_string(),
                name: "Ballad".to_string(),
                tempo: 68.0,
                parts: vec![
                    // Factory Grand Piano again
                    Part {
                        volume: 0.9,
                        timbre: Some(TimbreRef {
                            library_id: "c82b9b7e-9669-424a-9ef8-e0eef0437aa8".to_string(),
                            instrument_key: "pianos/concert_grand.sfz".to_string(),
                        }),
                        ..Default::default()
                    },
                ],
                ..Default::default()
            },
        ]
    }

    #[test]
    fn test_export_live_sets_only() {
        let sets = make_test_live_sets();
        let tmp = std::env::temp_dir().join("test_sets.velorasets");

        let params = ExportSelectionParams {
            target_path: tmp.to_string_lossy().to_string(),
            scope: "sets".to_string(),
            live_set_ids: vec!["ls-1".to_string(), "ls-2".to_string()],
            timbre_keys: vec![],
            user_bundles: HashMap::new(),
        };

        let result = perform_export(&sets, "Velora 73", "1.4.0", "2026-10-28T22:00:00Z".to_string(), params).unwrap();
        assert_eq!(result.live_set_count, 2);
        assert_eq!(result.timbre_count, 0);

        let read_bytes = fs::read(&tmp).unwrap();
        let decoded = read_sets_export_bytes(&read_bytes).unwrap();
        assert_eq!(decoded.format_version, 1);
        assert_eq!(decoded.device_model, "Velora 73");
        assert_eq!(decoded.live_sets.len(), 2);
        assert_eq!(decoded.timbres.len(), 0);

        let _ = fs::remove_file(tmp);
    }

    #[test]
    fn test_export_bundle_with_factory_and_user_timbres() {
        let sets = make_test_live_sets();
        let tmp = std::env::temp_dir().join("test_bundle.velorasets");

        let user_key = "user-lib-123:lead/synth_lead.sfz".to_string();
        let mut user_bundles = HashMap::new();
        user_bundles.insert(user_key.clone(), vec![1, 2, 3, 4, 5, 6]);

        let params = ExportSelectionParams {
            target_path: tmp.to_string_lossy().to_string(),
            scope: "bundle".to_string(),
            live_set_ids: vec!["ls-1".to_string()],
            timbre_keys: vec![],
            user_bundles,
        };

        let result = perform_export(&sets, "Velora 73", "1.4.0", "2026-10-28T22:00:00Z".to_string(), params).unwrap();
        assert_eq!(result.live_set_count, 1);
        assert_eq!(result.timbre_count, 2);
        assert_eq!(result.reference_timbre_count, 1);

        let read_bytes = fs::read(&tmp).unwrap();
        let decoded = read_sets_export_bytes(&read_bytes).unwrap();
        assert_eq!(decoded.timbres.len(), 2);

        // Verify factory timbre is reference only (0 B)
        let factory_timbre = decoded.timbres.iter().find(|t| t.is_reference_only).unwrap();
        assert_eq!(factory_timbre.library_id, "c82b9b7e-9669-424a-9ef8-e0eef0437aa8");
        assert!(factory_timbre.bundle_data.is_empty());

        // Verify user timbre has bundle data
        let user_timbre = decoded.timbres.iter().find(|t| !t.is_reference_only).unwrap();
        assert_eq!(user_timbre.library_id, "user-lib-123");
        assert_eq!(user_timbre.bundle_data, vec![1, 2, 3, 4, 5, 6]);

        let _ = fs::remove_file(tmp);
    }

    #[test]
    fn test_export_factory_timbres_only_is_refused() {
        let sets = make_test_live_sets();
        let tmp = std::env::temp_dir().join("factory_only.veloratimbre");

        let params = ExportSelectionParams {
            target_path: tmp.to_string_lossy().to_string(),
            scope: "timbres".to_string(),
            live_set_ids: vec![],
            timbre_keys: vec!["c82b9b7e-9669-424a-9ef8-e0eef0437aa8:pianos/concert_grand.sfz".to_string()],
            user_bundles: HashMap::new(),
        };

        let err = perform_export(&sets, "Velora 73", "1.4.0", "2026-10-28T22:00:00Z".to_string(), params).unwrap_err();
        assert!(err.contains("A timbre-only export containing only factory or partner timbres cannot be created"));
        assert!(!tmp.exists());
    }

    #[test]
    fn test_vendor_ip_protection_and_audio_exclusion_scanner() {
        // Factory timbre must have zero audio data
        let mut forbidden_payload = VeloraSetsExport {
            format_version: 1,
            device_model: "Velora 73".to_string(),
            firmware_version: "1.4.0".to_string(),
            export_time: "2026-10-28T22:00:00Z".to_string(),
            scope: ExportScope::Bundle as i32,
            manifest: vec![],
            live_sets: vec![],
            timbres: vec![ExportTimbrePayload {
                library_id: "c82b9b7e-9669-424a-9ef8-e0eef0437aa8".to_string(),
                instrument_key: "pianos/concert_grand.sfz".to_string(),
                name: "Concert Grand".to_string(),
                category: "Piano".to_string(),
                is_reference_only: false, // Security violation!
                bundle_data: b"FACTORY_AUDIO_MAGIC_SAMPLE_DATA_PCM_BLOCK".to_vec(),
            }],
        };

        let bytes = serialize_sets_export(&forbidden_payload);
        let scan_result = scan_for_vendor_content(&bytes, &["c82b9b7e-9669-424a-9ef8-e0eef0437aa8"]);
        assert!(scan_result.is_err(), "Scanner must reject vendor audio content!");

        // Fix it to compliant reference-only
        forbidden_payload.timbres[0].is_reference_only = true;
        forbidden_payload.timbres[0].bundle_data = Vec::new();
        let clean_bytes = serialize_sets_export(&forbidden_payload);
        let clean_scan = scan_for_vendor_content(&clean_bytes, &["c82b9b7e-9669-424a-9ef8-e0eef0437aa8"]);
        assert!(clean_scan.is_ok(), "Clean reference-only factory timbre must pass scanner");
    }

    #[test]
    fn test_size_estimation_and_2gb_warning() {
        let sets = make_test_live_sets();
        let mut custom_sizes = HashMap::new();
        custom_sizes.insert("user-lib-123:lead/synth_lead.sfz".to_string(), 3 * 1024 * 1024 * 1024); // 3 GB

        let params = ExportEstimateParams {
            scope: "bundle".to_string(),
            live_set_ids: vec!["ls-1".to_string()],
            timbre_keys: vec![],
            target_path: None,
            custom_user_timbre_sizes: custom_sizes,
        };

        let est = compute_export_estimate(&sets, &params);
        assert!(est.exceeds_2gb);
        assert_eq!(est.live_set_count, 1);
        assert_eq!(est.timbre_count, 2);
        assert_eq!(est.reference_timbre_count, 1);
        assert!(est.summary_text.contains("1 Live Sets, 2 timbres (1 by reference)"));
    }

    #[test]
    fn test_cancel_export_deletes_partial_file() {
        let sets = make_test_live_sets();
        let tmp = std::env::temp_dir().join(format!("cancelled_export_{}.velorasets", std::process::id()));
        if tmp.exists() {
            let _ = fs::remove_file(&tmp);
        }

        let cancel_flag = AtomicBool::new(true);

        let params = ExportSelectionParams {
            target_path: tmp.to_string_lossy().to_string(),
            scope: "sets".to_string(),
            live_set_ids: vec!["ls-1".to_string()],
            timbre_keys: vec![],
            user_bundles: HashMap::new(),
        };

        let err = perform_export_with_cancel(&sets, "Velora 73", "1.4.0", "2026-10-28T22:00:00Z".to_string(), params, &cancel_flag).unwrap_err();
        assert_eq!(err, "Export cancelled");
        assert!(!tmp.exists(), "Cancelled export must not leave partial file on disk");
    }

    #[test]
    fn test_format_version_rejection() {
        let sets = make_test_live_sets();
        let mut exp = VeloraSetsExport {
            format_version: 99,
            device_model: "Velora 73".to_string(),
            firmware_version: "1.4.0".to_string(),
            export_time: "2026-10-28T22:00:00Z".to_string(),
            scope: ExportScope::Sets as i32,
            manifest: vec![],
            live_sets: sets,
            timbres: vec![],
        };
        let bytes_v99 = serialize_sets_export(&exp);
        let err_sets = read_sets_export_bytes(&bytes_v99).unwrap_err();
        assert!(err_sets.contains("This export needs a newer Velora Studio"));

        exp.format_version = 0;
        let bytes_v0 = serialize_sets_export(&exp);
        let err_sets_v0 = read_sets_export_bytes(&bytes_v0).unwrap_err();
        assert!(err_sets_v0.contains("format_version"));

        let mut timbre_exp = VeloraTimbreExport {
            format_version: 99,
            device_model: "Velora 73".to_string(),
            firmware_version: "1.4.0".to_string(),
            export_time: "2026-10-28T22:00:00Z".to_string(),
            manifest: vec![],
            timbres: vec![],
        };
        let bytes_t99 = serialize_timbre_export(&timbre_exp);
        let err_t = read_timbre_export_bytes(&bytes_t99).unwrap_err();
        assert!(err_t.contains("This export needs a newer Velora Studio"));

        timbre_exp.format_version = 0;
        let bytes_t0 = serialize_timbre_export(&timbre_exp);
        let err_t0 = read_timbre_export_bytes(&bytes_t0).unwrap_err();
        assert!(err_t0.contains("format_version"));
    }

    #[test]
    fn test_in_flight_cancel_unlinks_written_file() {
        let tmp = std::env::temp_dir().join(format!("inflight_cancel_test_{}.velorasets", std::process::id()));
        fs::write(&tmp, b"partial-data").unwrap();
        assert!(tmp.exists());

        set_active_export_path(tmp.clone());
        request_cancel();

        assert!(!tmp.exists(), "request_cancel() must unlink active in-flight file");
        reset_cancel();
    }
}
