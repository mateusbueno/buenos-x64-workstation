// Story 9.14: Desktop Library Management & Heavy SFZ Conversion
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;
use std::time::SystemTime;

pub const CATEGORIES: &[(&str, u32, &str, &str)] = &[
    ("category.piano", 1, "piano", "Piano"),
    ("category.epiano", 2, "epiano", "E.Piano"),
    ("category.organ", 3, "organ", "Organ"),
    ("category.keys", 4, "keys", "Keys"),
    ("category.mallet_bells", 5, "mallet_bells", "Mallet & Bells"),
    ("category.strings", 6, "strings", "Strings"),
    ("category.brass", 7, "brass", "Brass"),
    ("category.woodwind", 8, "woodwind", "Woodwind"),
    ("category.choir_vocal", 9, "choir_vocal", "Choir & Vocal"),
    ("category.guitar", 10, "guitar", "Guitar"),
    ("category.bass", 11, "bass", "Bass"),
    ("category.synth_lead", 12, "synth_lead", "Synth Lead"),
    ("category.synth_pad", 13, "synth_pad", "Synth Pad"),
    ("category.synth_poly", 14, "synth_poly", "Synth Poly"),
    ("category.drums_perc", 15, "drums_perc", "Drums & Perc"),
    ("category.world", 16, "world", "World"),
    ("category.fx_ambient", 17, "fx_ambient", "FX & Ambient"),
];

pub fn category_id_from_key(key: &str) -> u32 {
    let clean = key.trim();
    for (k, id, slug, _) in CATEGORIES {
        if *k == clean || *slug == clean {
            return *id;
        }
    }
    0
}

pub fn category_key_from_id(id: u32) -> &'static str {
    for (k, cat_id, _, _) in CATEGORIES {
        if *cat_id == id {
            return *k;
        }
    }
    "none"
}

// Global cancellation tracking
static CANCEL_REQUESTED: AtomicBool = AtomicBool::new(false);
static ACTIVE_CHILD: Mutex<Option<u32>> = Mutex::new(None);
static ACTIVE_STAGING: Mutex<Option<PathBuf>> = Mutex::new(None);

pub fn request_cancel() {
    CANCEL_REQUESTED.store(true, Ordering::SeqCst);
    if let Ok(guard) = ACTIVE_CHILD.lock() {
        if let Some(pid) = *guard {
            #[cfg(unix)]
            unsafe {
                libc::kill(pid as i32, libc::SIGTERM);
            }
            #[cfg(windows)]
            {
                let _ = Command::new("taskkill").args(["/F", "/T", "/PID", &pid.to_string()]).output();
            }
        }
    }
    if let Ok(mut guard) = ACTIVE_STAGING.lock() {
        if let Some(lib_root) = guard.take() {
            if lib_root.exists() {
                if let Ok(entries) = fs::read_dir(&lib_root) {
                    for entry in entries.flatten() {
                        let path = entry.path();
                        let name = entry.file_name().to_string_lossy().to_string();
                        if name.starts_with(".staging") || name.starts_with(".tmp") {
                            let _ = fs::remove_dir_all(&path);
                        }
                    }
                }
            }
        }
    }
}

pub fn reset_cancel() {
    CANCEL_REQUESTED.store(false, Ordering::SeqCst);
    if let Ok(mut guard) = ACTIVE_CHILD.lock() {
        *guard = None;
    }
    if let Ok(mut guard) = ACTIVE_STAGING.lock() {
        *guard = None;
    }
}

pub fn is_cancel_requested() -> bool {
    CANCEL_REQUESTED.load(Ordering::SeqCst)
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ConversionTask {
    pub path: String,
    pub category: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct SuggestedCategory {
    pub name: String,
    pub category: String,
    pub category_id: u32,
    pub slug: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ConversionProgress {
    pub task_index: usize,
    pub total_tasks: usize,
    pub current_file: String,
    pub status: String, // "starting" | "converting" | "imported" | "skipped" | "failed" | "cancelled"
    pub reason: Option<String>,
    pub messages: Vec<String>,
    pub library_id: Option<String>,
    pub bundle_id: Option<String>,
    pub sample_count: usize,
    pub region_count: usize,
    pub suggested_category: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct ConvertedLibraryInfo {
    pub library_id: String,
    pub bundle_id: String,
    pub name: String,
    pub category: String,
    pub category_id: u32,
    pub size_bytes: u64,
    pub instrument_count: u32,
    pub file_path: String,
    pub modified_timestamp: u64,
}

// Embedded Category Keyword Table based on librarian/data/category_keywords.json
struct KeywordRule {
    category: &'static str,
    weight: u32,
    phrases: &'static [&'static str],
}

static KEYWORD_RULES: &[KeywordRule] = &[
    KeywordRule { category: "piano", weight: 10, phrases: &["piano", "pianos", "pianoforte", "steinway", "bosendorfer", "bechstein", "honky tonk", "salamander"] },
    KeywordRule { category: "piano", weight: 15, phrases: &["grand piano", "upright piano", "acoustic piano", "piano acustico", "piano cauda", "piano de cauda", "piano vertical", "piano de armario"] },
    KeywordRule { category: "epiano", weight: 10, phrases: &["rhodes", "wurlitzer", "wurli", "wurly", "pianet", "epiano"] },
    KeywordRule { category: "epiano", weight: 15, phrases: &["electric piano", "e piano", "piano eletrico", "piano electrico", "rhodes piano", "wurlitzer piano", "wurli piano", "wurly piano", "electric grand"] },
    KeywordRule { category: "organ", weight: 10, phrases: &["organ", "organs", "orgao", "organo", "hammond", "tonewheel", "farfisa"] },
    KeywordRule { category: "organ", weight: 15, phrases: &["pipe organ", "church organ", "drawbar organ", "tone wheel", "vox humana", "vox continental", "orgao de tubos"] },
    KeywordRule { category: "keys", weight: 10, phrases: &["keys", "clavinet", "clavinete", "harpsichord", "cravo", "clavecin", "celesta", "celeste", "accordion", "acordeao", "acordeon", "sanfona"] },
    KeywordRule { category: "mallet_bells", weight: 10, phrases: &["vibraphone", "vibrafone", "vibes", "marimba", "xylophone", "xilofone", "xilofon", "xilofono", "glockenspiel", "mallet", "mallets", "metallophone", "sinos", "campanas", "carillon", "crotales"] },
    KeywordRule { category: "mallet_bells", weight: 15, phrases: &["tubular bells", "sinos tubulares", "campanas tubulares"] },
    KeywordRule { category: "strings", weight: 10, phrases: &["strings", "cordas", "cuerdas", "violin", "violins", "violino", "violinos", "violon", "viola", "violas", "cello", "cellos", "violoncello", "violoncelo", "orchestra", "orquestra", "orquesta", "pizzicato", "pizz", "spiccato"] },
    KeywordRule { category: "strings", weight: 15, phrases: &["string ensemble", "string section", "string quartet", "string orchestra", "synth strings", "viola da gamba"] },
    KeywordRule { category: "brass", weight: 10, phrases: &["brass", "metais", "metales", "trumpet", "trumpets", "trompete", "trompetes", "trompeta", "trompetas", "trombone", "trombones", "tuba", "tubas", "trompa", "flugelhorn", "cornet", "euphonium", "bugle", "sousaphone"] },
    KeywordRule { category: "brass", weight: 15, phrases: &["french horn", "brass section", "bass tuba", "bass trombone", "trombone baixo"] },
    KeywordRule { category: "woodwind", weight: 10, phrases: &["woodwind", "woodwinds", "madeiras", "maderas", "flute", "flutes", "flauta", "flautas", "piccolo", "clarinet", "clarinets", "clarinete", "clarinetes", "oboe", "oboes", "bassoon", "fagote", "fagot", "saxophone", "saxofone", "saxofon", "sax", "recorder", "harmonica", "armonica", "ocarina", "panflute"] },
    KeywordRule { category: "woodwind", weight: 15, phrases: &["bass clarinet", "bass flute", "english horn", "cor anglais", "pan flute", "clarinete baixo", "clarinete bajo"] },
    KeywordRule { category: "choir_vocal", weight: 10, phrases: &["choir", "choirs", "coro", "coros", "coral", "choral", "vocal", "vocals", "voices", "vozes", "voces", "aahs", "aah", "oohs", "ooh", "vocalise"] },
    KeywordRule { category: "guitar", weight: 10, phrases: &["guitar", "guitars", "guitarra", "guitarras", "violao", "violoes", "telecaster"] },
    KeywordRule { category: "guitar", weight: 15, phrases: &["nylon guitar", "steel guitar", "acoustic guitar", "electric guitar", "flamenco guitar", "guitarra electrica", "guitarra eletrica", "violao nylon", "violao aco"] },
    KeywordRule { category: "bass", weight: 10, phrases: &["bass", "basses", "baixo", "bajo", "bajos", "upright", "fretless", "contrabaixo", "contrabajo", "subbass"] },
    KeywordRule { category: "bass", weight: 15, phrases: &["upright bass", "electric bass", "acoustic bass", "synth bass", "slap bass", "bass guitar", "baixo eletrico", "bajo electrico", "double bass", "sub bass", "baixo acustico", "bajo acustico"] },
    KeywordRule { category: "synth_lead", weight: 15, phrases: &["synth lead", "lead synth", "saw lead", "sawtooth lead", "square lead", "sync lead", "mono lead"] },
    KeywordRule { category: "synth_pad", weight: 10, phrases: &["pad", "pads"] },
    KeywordRule { category: "synth_pad", weight: 15, phrases: &["synth pad", "warm pad"] },
    KeywordRule { category: "synth_poly", weight: 10, phrases: &["polysynth", "supersaw", "pluck", "plucks", "arpeggio", "arpejo"] },
    KeywordRule { category: "synth_poly", weight: 15, phrases: &["synth poly", "poly synth", "synth brass", "brass synth", "synth pluck", "synth arp"] },
    KeywordRule { category: "drums_perc", weight: 10, phrases: &["drums", "drum", "bateria", "kit", "kits", "percussion", "percussao", "percusion", "percusiones", "perc", "snare", "kick", "hihat", "cymbal", "cymbals", "timpani", "timbales", "conga", "congas", "bongo", "bongos", "cajon", "tambourine", "shaker", "claps", "cowbell", "pandeiro", "surdo", "tamborim", "cuica", "agogo", "djembe"] },
    KeywordRule { category: "drums_perc", weight: 15, phrases: &["drum kit", "bass drum", "drum pad", "hi hat", "percussao latina"] },
    KeywordRule { category: "world", weight: 10, phrases: &["sitar", "koto", "shamisen", "oud", "erhu", "guzheng", "tabla", "berimbau", "cavaquinho", "bouzouki", "balalaika", "didgeridoo", "kalimba", "mbira", "shakuhachi", "bagpipe", "bagpipes", "banjo", "ukulele", "bandolim", "charango", "quena", "gamelan", "steelpan"] },
    KeywordRule { category: "world", weight: 15, phrases: &["guitarra portuguesa", "steel drum", "steel drums"] },
    KeywordRule { category: "fx_ambient", weight: 10, phrases: &["fx", "sfx", "effects", "efeitos", "efectos", "ambient", "ambience", "ambiences", "ambiente", "ambientes", "atmosphere", "atmospheres", "atmosfera", "atmosferas", "soundscape", "soundscapes", "drone", "drones", "noise", "ruido", "ruidos", "riser", "risers", "impact", "impacts", "whoosh", "sweep", "sweeps", "texture", "textures", "textura", "texturas"] },
    KeywordRule { category: "fx_ambient", weight: 15, phrases: &["sound effects", "efeitos sonoros"] },
    KeywordRule { category: "none", weight: 100, phrases: &["viola caipira", "viola de cocho", "viola de arame", "viola sertaneja", "viola de dez cordas", "viola brasileira", "viola portuguesa", "viola braguesa", "viola amarantina", "viola da terra", "brazilian viola"] },
];

pub fn normalize_for_suggest(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for c in s.chars() {
        if c.is_alphanumeric() {
            out.extend(c.to_lowercase());
        } else {
            out.push(' ');
        }
    }
    let parts: Vec<&str> = out.split_whitespace().collect();
    parts.join(" ")
}

pub fn suggest_category_for_name(name: &str) -> SuggestedCategory {
    let p = Path::new(name);
    let stem = p.file_stem().and_then(|s| s.to_str()).unwrap_or(name);
    let parent = p.parent().and_then(|pr| pr.file_name()).and_then(|s| s.to_str()).unwrap_or("");
    let combined = if !parent.is_empty() {
        format!("{} {}", parent, stem)
    } else {
        stem.to_string()
    };

    let norm_stem = format!(" {} ", normalize_for_suggest(stem));
    let norm_combined = format!(" {} ", normalize_for_suggest(&combined));

    let mut scores: HashMap<&'static str, u32> = HashMap::new();

    for rule in KEYWORD_RULES {
        for phrase in rule.phrases {
            let norm_phrase = format!(" {} ", normalize_for_suggest(phrase));
            if norm_stem.contains(&norm_phrase) {
                let entry = scores.entry(rule.category).or_insert(0);
                *entry = (*entry).max(rule.weight * 2);
            } else if norm_combined.contains(&norm_phrase) {
                let entry = scores.entry(rule.category).or_insert(0);
                *entry = (*entry).max(rule.weight);
            }
        }
    }

    if let Some(&none_weight) = scores.get("none") {
        if none_weight >= 100 {
            return SuggestedCategory {
                name: name.to_string(),
                category: "none".to_string(),
                category_id: 0,
                slug: "none".to_string(),
            };
        }
    }

    let mut best_cat = "none";
    let mut best_score = 0;

    for (cat, score) in scores {
        if cat != "none" && score > best_score {
            best_score = score;
            best_cat = cat;
        }
    }

    if best_score > 0 {
        let cat_key = format!("category.{}", best_cat);
        let id = category_id_from_key(&cat_key);
        SuggestedCategory {
            name: name.to_string(),
            category: cat_key,
            category_id: id,
            slug: best_cat.to_string(),
        }
    } else {
        SuggestedCategory {
            name: name.to_string(),
            category: "none".to_string(),
            category_id: 0,
            slug: "none".to_string(),
        }
    }
}

pub fn suggest_categories_batch(names: &[String]) -> Vec<SuggestedCategory> {
    names.iter().map(|n| suggest_category_for_name(n)).collect()
}

pub fn locate_librarian_binary() -> Option<PathBuf> {
    if let Ok(path_str) = std::env::var("VELORA_LIBRARIAN_BIN") {
        let p = PathBuf::from(path_str);
        if p.exists() {
            return Some(p);
        }
    }

    let candidates = [
        "build/librarian/librarian",
        "../build/librarian/librarian",
        "../../build/librarian/librarian",
        "librarian/librarian",
    ];

    for c in &candidates {
        let p = PathBuf::from(c);
        if p.exists() {
            return Some(p.canonicalize().unwrap_or(p));
        }
    }

    if let Ok(exe) = std::env::current_exe() {
        if let Some(parent) = exe.parent() {
            let p = parent.join("librarian");
            if p.exists() {
                return Some(p);
            }
        }
    }

    if let Ok(output) = Command::new("which").arg("librarian").output() {
        if output.status.success() {
            let p_str = String::from_utf8_lossy(&output.stdout).trim().to_string();
            let p = PathBuf::from(p_str);
            if p.exists() {
                return Some(p);
            }
        }
    }

    None
}

pub fn get_converted_libraries_root(cache_dir: Option<&Path>) -> PathBuf {
    if let Some(dir) = cache_dir {
        dir.join("converted_libraries")
    } else if let Ok(custom) = std::env::var("VELORA_CONVERTED_LIBRARIES_DIR") {
        PathBuf::from(custom)
    } else {
        let base = std::env::var("VELORA_STUDIO_DATA_DIR")
            .ok()
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("./converted_cache"));
        base.join("converted_libraries")
    }
}

pub fn list_converted_libraries(cache_root: &Path) -> Result<Vec<ConvertedLibraryInfo>, String> {
    let root = if cache_root.ends_with("converted_libraries") {
        cache_root.to_path_buf()
    } else {
        cache_root.join("converted_libraries")
    };

    let lib_dir = root.join("lib");
    if !lib_dir.exists() {
        return Ok(vec![]);
    }

    let mut results = Vec::new();
    let entries = fs::read_dir(&lib_dir).map_err(|e| e.to_string())?;

    for entry in entries.flatten() {
        let path = entry.path();
        if !path.is_dir() {
            continue;
        }

        let library_id = entry.file_name().to_string_lossy().to_string();
        if library_id.starts_with('.') {
            continue;
        }

        let current_link = path.join("current");
        let bundle_target = if current_link.exists() {
            current_link.canonicalize().unwrap_or(current_link)
        } else {
            let mut sub = None;
            if let Ok(subs) = fs::read_dir(&path) {
                for s in subs.flatten() {
                    let sp = s.path();
                    if sp.is_dir() && !s.file_name().to_string_lossy().starts_with('.') {
                        sub = Some(sp);
                        break;
                    }
                }
            }
            match sub {
                Some(s) => s,
                None => continue,
            }
        };

        let bundle_file = bundle_target.join("bundle.vlb");
        if !bundle_file.exists() {
            continue;
        }

        let bundle_id = bundle_target.file_name().map(|n| n.to_string_lossy().to_string()).unwrap_or_default();

        let cat_file = bundle_target.join("category");
        let category = if cat_file.exists() {
            fs::read_to_string(&cat_file).unwrap_or_default().trim().to_string()
        } else {
            "none".to_string()
        };

        let category_id = category_id_from_key(&category);

        let metadata = fs::metadata(&bundle_file).map_err(|e| e.to_string())?;
        let size_bytes = metadata.len();
        let modified_timestamp = metadata.modified()
            .ok()
            .and_then(|t| t.duration_since(SystemTime::UNIX_EPOCH).ok())
            .map(|d| d.as_secs())
            .unwrap_or(0);

        let name = if bundle_target.join("name").exists() {
            fs::read_to_string(bundle_target.join("name")).unwrap_or_default().trim().to_string()
        } else {
            bundle_target.parent().and_then(|p| p.file_name()).map(|n| n.to_string_lossy().to_string()).unwrap_or_else(|| library_id.clone())
        };

        results.push(ConvertedLibraryInfo {
            library_id: library_id.clone(),
            bundle_id,
            name,
            category,
            category_id,
            size_bytes,
            instrument_count: 1,
            file_path: bundle_file.to_string_lossy().to_string(),
            modified_timestamp,
        });
    }

    results.sort_by(|a, b| b.modified_timestamp.cmp(&a.modified_timestamp));
    Ok(results)
}

pub fn delete_converted_library(library_id: &str, cache_root: &Path) -> Result<(), String> {
    if library_id.is_empty() || library_id.contains('/') || library_id.contains('\\') || library_id.contains("..") {
        return Err("invalid library_id".to_string());
    }

    let root = if cache_root.ends_with("converted_libraries") {
        cache_root.to_path_buf()
    } else {
        cache_root.join("converted_libraries")
    };

    let target = root.join("lib").join(library_id);
    if target.exists() {
        fs::remove_dir_all(&target).map_err(|e| format!("Failed to delete library {}: {}", library_id, e))?;
    }
    Ok(())
}

#[derive(Debug, Deserialize)]
struct LibrarianJsonlFileLine {
    #[serde(rename = "type")]
    line_type: String,
    file: Option<String>,
    status: Option<String>,
    reason: Option<String>,
    messages: Option<Vec<String>>,
    library_id: Option<String>,
    bundle_id: Option<String>,
    samples: Option<usize>,
    regions: Option<usize>,
    suggested_category: Option<String>,
}

pub fn convert_single_task(
    librarian_bin: &Path,
    task: &ConversionTask,
    target_root: &Path,
    task_index: usize,
    total_tasks: usize,
    progress_sink: &dyn Fn(ConversionProgress),
) -> Result<ConversionProgress, String> {
    if is_cancel_requested() {
        let prog = ConversionProgress {
            task_index,
            total_tasks,
            current_file: task.path.clone(),
            status: "cancelled".to_string(),
            reason: Some("Operation cancelled by user".to_string()),
            messages: vec!["Conversion cancelled".to_string()],
            library_id: None,
            bundle_id: None,
            sample_count: 0,
            region_count: 0,
            suggested_category: None,
        };
        progress_sink(prog.clone());
        return Ok(prog);
    }

    let category_arg = task.category.clone().unwrap_or_else(|| {
        let sugg = suggest_category_for_name(&task.path);
        if sugg.category_id != 0 {
            sugg.slug
        } else {
            "piano".to_string()
        }
    });

    progress_sink(ConversionProgress {
        task_index,
        total_tasks,
        current_file: task.path.clone(),
        status: "converting".to_string(),
        reason: None,
        messages: vec![],
        library_id: None,
        bundle_id: None,
        sample_count: 0,
        region_count: 0,
        suggested_category: None,
    });

    if let Ok(mut guard) = ACTIVE_STAGING.lock() {
        *guard = Some(target_root.join("lib"));
    }

    let mut cmd = Command::new(librarian_bin);
    cmd.arg("import")
        .arg(&task.path)
        .arg("--root")
        .arg(target_root)
        .arg("--category")
        .arg(&category_arg)
        .arg("--report")
        .arg("-")
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());

    let child = cmd.spawn().map_err(|e| format!("Failed to spawn librarian: {}", e))?;
    let child_pid = child.id();

    if let Ok(mut guard) = ACTIVE_CHILD.lock() {
        *guard = Some(child_pid);
    }

    let output = child.wait_with_output().map_err(|e| format!("Librarian process error: {}", e))?;

    if let Ok(mut guard) = ACTIVE_CHILD.lock() {
        *guard = None;
    }

    if is_cancel_requested() {
        let prog = ConversionProgress {
            task_index,
            total_tasks,
            current_file: task.path.clone(),
            status: "cancelled".to_string(),
            reason: Some("Operation cancelled by user".to_string()),
            messages: vec!["Conversion cancelled".to_string()],
            library_id: None,
            bundle_id: None,
            sample_count: 0,
            region_count: 0,
            suggested_category: None,
        };
        progress_sink(prog.clone());
        return Ok(prog);
    }

    let stdout_str = String::from_utf8_lossy(&output.stdout);
    let mut last_progress = None;

    for line in stdout_str.lines() {
        if line.starts_with('{') {
            if let Ok(parsed) = serde_json::from_str::<LibrarianJsonlFileLine>(line) {
                if parsed.line_type == "file" {
                    let status = parsed.status.unwrap_or_else(|| "imported".to_string());
                    let prog = ConversionProgress {
                        task_index,
                        total_tasks,
                        current_file: parsed.file.unwrap_or_else(|| task.path.clone()),
                        status,
                        reason: parsed.reason,
                        messages: parsed.messages.unwrap_or_default(),
                        library_id: parsed.library_id.clone(),
                        bundle_id: parsed.bundle_id.clone(),
                        sample_count: parsed.samples.unwrap_or(0),
                        region_count: parsed.regions.unwrap_or(0),
                        suggested_category: parsed.suggested_category,
                    };
                    if let (Some(lib_id), Some(b_id)) = (&parsed.library_id, &parsed.bundle_id) {
                        let friendly = Path::new(&task.path)
                            .file_stem()
                            .and_then(|s| s.to_str())
                            .unwrap_or("Sound Library");
                        let bundle_dir = target_root.join("lib").join(lib_id).join(b_id);
                        let _ = fs::write(bundle_dir.join("name"), friendly);
                    }
                    last_progress = Some(prog);
                    break;
                }
            }
        }
    }

    let final_prog = match last_progress {
        Some(p) => p,
        None => {
            let stderr_str = String::from_utf8_lossy(&output.stderr).to_string();
            ConversionProgress {
                task_index,
                total_tasks,
                current_file: task.path.clone(),
                status: if output.status.success() { "imported".to_string() } else { "failed".to_string() },
                reason: if output.status.success() { None } else { Some("Process exited with error".to_string()) },
                messages: if stderr_str.is_empty() { vec![] } else { vec![stderr_str] },
                library_id: None,
                bundle_id: None,
                sample_count: 0,
                region_count: 0,
                suggested_category: None,
            }
        }
    };

    progress_sink(final_prog.clone());
    Ok(final_prog)
}

pub fn convert_libraries_batch(
    tasks: &[ConversionTask],
    cache_root: &Path,
    progress_sink: &dyn Fn(ConversionProgress),
) -> Result<Vec<ConversionProgress>, String> {
    reset_cancel();

    let root = if cache_root.ends_with("converted_libraries") {
        cache_root.to_path_buf()
    } else {
        cache_root.join("converted_libraries")
    };

    fs::create_dir_all(&root).map_err(|e| e.to_string())?;

    let librarian_bin = locate_librarian_binary()
        .ok_or_else(|| "librarian CLI binary not found. Build librarian or set VELORA_LIBRARIAN_BIN.".to_string())?;

    let mut results = Vec::new();
    let total = tasks.len();

    for (idx, task) in tasks.iter().enumerate() {
        if is_cancel_requested() {
            let prog = ConversionProgress {
                task_index: idx,
                total_tasks: total,
                current_file: task.path.clone(),
                status: "cancelled".to_string(),
                reason: Some("Operation cancelled by user".to_string()),
                messages: vec!["Conversion cancelled".to_string()],
                library_id: None,
                bundle_id: None,
                sample_count: 0,
                region_count: 0,
                suggested_category: None,
            };
            progress_sink(prog.clone());
            results.push(prog);
            break;
        }

        let res = convert_single_task(&librarian_bin, task, &root, idx + 1, total, progress_sink)?;
        let was_cancelled = res.status == "cancelled";
        results.push(res);
        if was_cancelled || is_cancel_requested() {
            break;
        }
    }

    Ok(results)
}

#[cfg(test)]
mod tests {
    use super::*;

    static TEST_MUTEX: Mutex<()> = Mutex::new(());

    #[test]
    fn test_category_normalization_and_id_mappings() {
        assert_eq!(category_id_from_key("category.piano"), 1);
        assert_eq!(category_id_from_key("piano"), 1);
        assert_eq!(category_id_from_key("category.epiano"), 2);
        assert_eq!(category_id_from_key("category.fx_ambient"), 17);
        assert_eq!(category_id_from_key("unknown"), 0);

        assert_eq!(category_key_from_id(1), "category.piano");
        assert_eq!(category_key_from_id(17), "category.fx_ambient");
        assert_eq!(category_key_from_id(99), "none");
    }

    #[test]
    fn test_suggest_category_keyword_matching() {
        let s1 = suggest_category_for_name("Steinway Model D Grand Piano.sfz");
        assert_eq!(s1.category, "category.piano");
        assert_eq!(s1.category_id, 1);

        let s2 = suggest_category_for_name("Fender Rhodes Mark I.sf2");
        assert_eq!(s2.category, "category.epiano");
        assert_eq!(s2.category_id, 2);

        let s3 = suggest_category_for_name("Alto Sax Solo.sfz");
        assert_eq!(s3.category, "category.woodwind");
        assert_eq!(s3.category_id, 8);

        let s4 = suggest_category_for_name("/samples/orchestra/Violin Section Legato.sfz");
        assert_eq!(s4.category, "category.strings");
        assert_eq!(s4.category_id, 6);

        let s5 = suggest_category_for_name("Studio Drum Kit 80s.sf2");
        assert_eq!(s5.category, "category.drums_perc");
        assert_eq!(s5.category_id, 15);

        let s6 = suggest_category_for_name("Viola Caipira 10 Cordas.sfz");
        assert_eq!(s6.category, "none");
        assert_eq!(s6.category_id, 0);

        let s7 = suggest_category_for_name("X9000 Custom Tool.sfz");
        assert_eq!(s7.category, "none");
        assert_eq!(s7.category_id, 0);
    }

    #[test]
    fn test_locate_librarian_binary() {
        let bin = locate_librarian_binary();
        assert!(bin.is_some(), "librarian binary should be located in build tree");
        let p = bin.unwrap();
        assert!(p.exists());
    }

    #[test]
    fn test_cancellation_tracking() {
        let _lock = TEST_MUTEX.lock().unwrap();
        reset_cancel();
        assert!(!is_cancel_requested());
        request_cancel();
        assert!(is_cancel_requested());
        reset_cancel();
        assert!(!is_cancel_requested());

        // Verify staging cleanup on cancel
        let temp_dir = std::env::temp_dir().join(format!("velora_cancel_staging_test_{}", std::process::id()));
        let staging_dir = temp_dir.join(".staging-1234");
        fs::create_dir_all(&staging_dir).unwrap();
        fs::write(staging_dir.join("partial.vlb"), b"PARTIAL").unwrap();
        assert!(staging_dir.exists());

        if let Ok(mut guard) = ACTIVE_STAGING.lock() {
            *guard = Some(temp_dir.clone());
        }
        request_cancel();
        assert!(!staging_dir.exists(), "request_cancel must clean active staging directories");
        let _ = fs::remove_dir_all(&temp_dir);
        reset_cancel();
    }

    #[test]
    fn test_list_and_delete_converted_libraries() {
        let temp_dir = std::env::temp_dir().join(format!("velora_converter_test_{}", std::process::id()));
        let root = temp_dir.join("converted_libraries");
        let lib_id = "test-uuid-1234";
        let bundle_id = "bundle-5678";
        let bundle_dir = root.join("lib").join(lib_id).join(bundle_id);
        fs::create_dir_all(&bundle_dir).unwrap();

        fs::write(bundle_dir.join("bundle.vlb"), b"VELORA_BUNDLE_MOCK_DATA").unwrap();
        fs::write(bundle_dir.join("category"), b"category.piano\n").unwrap();

        #[cfg(unix)]
        {
            let current = root.join("lib").join(lib_id).join("current");
            let _ = std::os::unix::fs::symlink(bundle_id, current);
        }

        let libs = list_converted_libraries(&root).unwrap();
        assert_eq!(libs.len(), 1);
        assert_eq!(libs[0].library_id, lib_id);
        assert_eq!(libs[0].category, "category.piano");
        assert_eq!(libs[0].category_id, 1);
        assert_eq!(libs[0].size_bytes, 23);

        delete_converted_library(lib_id, &root).unwrap();
        let libs_after = list_converted_libraries(&root).unwrap();
        assert_eq!(libs_after.len(), 0);

        let _ = fs::remove_dir_all(&temp_dir);
    }

    #[test]
    fn test_convert_sfz_roundtrip_with_librarian_cli() {
        let _lock = TEST_MUTEX.lock().unwrap();
        let temp_dir = std::env::temp_dir().join(format!("velora_sfz_conv_test_{}", std::process::id()));
        let src_dir = temp_dir.join("src");
        let cache_dir = temp_dir.join("cache");
        fs::create_dir_all(&src_dir).unwrap();
        fs::create_dir_all(&cache_dir).unwrap();

        let wav_path = src_dir.join("sample.wav");
        let mut wav_bytes = Vec::new();
        let pcm_size: u32 = 480 * 2;
        let riff_size: u32 = 36 + pcm_size;
        wav_bytes.extend_from_slice(b"RIFF");
        wav_bytes.extend_from_slice(&riff_size.to_le_bytes());
        wav_bytes.extend_from_slice(b"WAVE");
        wav_bytes.extend_from_slice(b"fmt ");
        wav_bytes.extend_from_slice(&16u32.to_le_bytes());
        wav_bytes.extend_from_slice(&1u16.to_le_bytes());
        wav_bytes.extend_from_slice(&1u16.to_le_bytes());
        wav_bytes.extend_from_slice(&48000u32.to_le_bytes());
        wav_bytes.extend_from_slice(&96000u32.to_le_bytes());
        wav_bytes.extend_from_slice(&2u16.to_le_bytes());
        wav_bytes.extend_from_slice(&16u16.to_le_bytes());
        wav_bytes.extend_from_slice(b"data");
        wav_bytes.extend_from_slice(&pcm_size.to_le_bytes());
        for i in 0..480 {
            let val = ((i % 100) * 100) as i16;
            wav_bytes.extend_from_slice(&val.to_le_bytes());
        }
        fs::write(&wav_path, &wav_bytes).unwrap();

        let sfz_path = src_dir.join("Test Grand Piano.sfz");
        let sfz_content = "<region> sample=sample.wav key=60 lokey=21 hikey=108\n";
        fs::write(&sfz_path, sfz_content).unwrap();

        let tasks = vec![ConversionTask {
            path: sfz_path.to_string_lossy().to_string(),
            category: Some("piano".to_string()),
        }];

        let events = std::sync::Mutex::new(Vec::new());
        let sink = |p: ConversionProgress| {
            events.lock().unwrap().push(p);
        };

        let res = convert_libraries_batch(&tasks, &cache_dir, &sink).unwrap();
        assert_eq!(res.len(), 1);
        assert_eq!(res[0].status, "imported");
        assert!(res[0].library_id.is_some());
        assert!(res[0].bundle_id.is_some());

        let list = list_converted_libraries(&cache_dir).unwrap();
        assert_eq!(list.len(), 1);
        assert_eq!(list[0].category, "category.piano");
        assert_eq!(list[0].category_id, 1);
        assert!(list[0].size_bytes > 0);

        let _ = fs::remove_dir_all(&temp_dir);
    }
}
