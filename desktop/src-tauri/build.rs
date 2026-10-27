use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    let manifest_dir = env::var("CARGO_MANIFEST_DIR").unwrap();
    let repo_root = Path::new(&manifest_dir)
        .parent()
        .and_then(Path::parent)
        .expect("Failed to locate repo root from CARGO_MANIFEST_DIR");

    let proto_dir = repo_root.join("proto");
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());
    let gen_proto_dir = out_dir.join("proto");
    std::fs::create_dir_all(&gen_proto_dir).expect("Failed to create gen proto dir");

    let ws_v1_params = gen_proto_dir.join("ws_v1_params.proto");
    let gen_params_py = proto_dir.join("gen_params.py");
    let params_json = proto_dir.join("params.json");

    println!("cargo:rerun-if-changed={}", proto_dir.join("ws_v1.proto").display());
    println!("cargo:rerun-if-changed={}", proto_dir.join("setlist.proto").display());
    println!("cargo:rerun-if-changed={}", proto_dir.join("live_set.proto").display());
    println!("cargo:rerun-if-changed={}", proto_dir.join("backup.proto").display());
    println!("cargo:rerun-if-changed={}", proto_dir.join("library_api.proto").display());
    println!("cargo:rerun-if-changed={}", params_json.display());
    println!("cargo:rerun-if-changed={}", gen_params_py.display());

    // Run python3 proto/gen_params.py --defs proto/params.json --proto <ws_v1_params.proto>
    let status = Command::new("python3")
        .args([
            gen_params_py.to_str().unwrap(),
            "--defs",
            params_json.to_str().unwrap(),
            "--proto",
            ws_v1_params.to_str().unwrap(),
        ])
        .status()
        .expect("Failed to run proto/gen_params.py");
    assert!(status.success(), "proto/gen_params.py failed");

    // Compile protos into Rust types with prost
    let ws_v1_proto = proto_dir.join("ws_v1.proto");
    let setlist_proto = proto_dir.join("setlist.proto");
    let live_set_proto = proto_dir.join("live_set.proto");
    let backup_proto = proto_dir.join("backup.proto");
    // Story 9.11: the Library API (package ws.v1, the librarian's protocol): Studio lists the installed timbres through the bridge's library allow-list.
    let library_api_proto = proto_dir.join("library_api.proto");

    let mut config = prost_build::Config::new();
    config.type_attribute(".", "#[derive(serde::Serialize, serde::Deserialize)]");
    // Absent fields read as their defaults when a message arrives as JSON from the UI (the generic `device_request` command).
    config.message_attribute(".", "#[serde(default)]");
    config
        .compile_protos(
            &[ws_v1_proto, setlist_proto, live_set_proto, backup_proto, library_api_proto],
            &[gen_proto_dir.as_path(), proto_dir.as_path()],
        )
        .expect("Failed to compile protos with prost");

    tauri_build::build();
}
