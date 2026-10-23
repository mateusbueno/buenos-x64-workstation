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

    // Compile proto/ws_v1.proto into Rust types with prost
    let ws_v1_proto = proto_dir.join("ws_v1.proto");
    let mut config = prost_build::Config::new();
    config
        .compile_protos(
            &[ws_v1_proto],
            &[gen_proto_dir.as_path(), proto_dir.as_path()],
        )
        .expect("Failed to compile ws_v1.proto with prost");

    tauri_build::build();
}
