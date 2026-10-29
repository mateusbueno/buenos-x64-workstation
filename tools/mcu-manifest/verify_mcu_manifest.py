#!/usr/bin/env python3
"""Story 8.4: Build-time verifier and generator for `/usr/lib/velora/mcu/manifest.json`.

Verifies that `/usr/lib/velora/mcu/manifest.json` lists the expected firmware version
and SHA-256 hash of each MCU binary (`keybed` and `panel`) shipped in `/usr/lib/velora/mcu/`,
and fails the build if any binary is missing or its SHA-256 / byte size does not match the manifest.
"""
import argparse
import hashlib
import json
import os
import re
import sys
import tempfile

REQUIRED_MCUS = {
    "keybed": 1,
    "panel": 2,
}
HEX64_RE = re.compile(r"^[0-9a-f]{64}$")
VERSION_RE = re.compile(r"^(\d+)\.(\d+)$")


class ManifestError(Exception):
    pass


def parse_version_u16(ver_str):
    m = VERSION_RE.match(str(ver_str).strip())
    if not m:
        raise ManifestError(f"invalid MCU version string '{ver_str}' (expected 'M.m')")
    major, minor = int(m.group(1)), int(m.group(2))
    if not (1 <= major <= 255 and 0 <= minor <= 255):
        raise ManifestError(f"MCU version out of uint16 range: {ver_str}")
    return (major << 8) | minor


def verify_manifest(manifest_path, mcu_dir=None):
    if not os.path.isfile(manifest_path):
        raise ManifestError(f"manifest file not found: {manifest_path}")
    if mcu_dir is None:
        mcu_dir = os.path.dirname(os.path.abspath(manifest_path))

    try:
        with open(manifest_path, "r", encoding="utf-8") as f:
            doc = json.load(f)
    except Exception as exc:
        raise ManifestError(f"invalid JSON in {manifest_path}: {exc}") from exc

    if doc.get("schema_version") != 1:
        raise ManifestError(f"unsupported schema_version: {doc.get('schema_version')}")

    mcus = doc.get("mcus")
    if not isinstance(mcus, dict):
        raise ManifestError("manifest missing 'mcus' object")

    for name, expected_id in REQUIRED_MCUS.items():
        if name not in mcus:
            raise ManifestError(f"manifest missing required MCU entry '{name}'")
        entry = mcus[name]
        if entry.get("mcu_id") != expected_id:
            raise ManifestError(f"MCU '{name}' has mcu_id={entry.get('mcu_id')}, expected {expected_id}")
        ver_u16 = parse_version_u16(entry.get("version", ""))
        if entry.get("fw_version_u16") != ver_u16:
            raise ManifestError(
                f"MCU '{name}' fw_version_u16={entry.get('fw_version_u16')} does not match version '{entry.get('version')}' ({ver_u16})"
            )
        filename = entry.get("filename", "")
        if not filename or "/" in filename or "\\" in filename or ".." in filename:
            raise ManifestError(f"MCU '{name}' has invalid filename '{filename}'")
        expected_sha = str(entry.get("sha256", "")).lower()
        if not HEX64_RE.match(expected_sha):
            raise ManifestError(f"MCU '{name}' has invalid sha256 '{expected_sha}'")

        bin_path = os.path.join(mcu_dir, filename)
        if not os.path.isfile(bin_path):
            raise ManifestError(f"MCU '{name}' binary missing: {bin_path}")
        with open(bin_path, "rb") as bf:
            data = bf.read()
        actual_sha = hashlib.sha256(data).hexdigest()
        if actual_sha != expected_sha:
            raise ManifestError(
                f"MCU '{name}' ({filename}) SHA-256 mismatch: manifest={expected_sha}, actual={actual_sha}"
            )
        if entry.get("size") != len(data):
            raise ManifestError(
                f"MCU '{name}' ({filename}) size mismatch: manifest={entry.get('size')}, actual={len(data)}"
            )

    return doc


def generate_manifest(mcu_dir, keybed_version="1.2", panel_version="1.2", out_path=None):
    entries = {}
    for name, mcu_id, ver in (("keybed", 1, keybed_version), ("panel", 2, panel_version)):
        fn = f"{name}.bin"
        p = os.path.join(mcu_dir, fn)
        if not os.path.isfile(p):
            raise ManifestError(f"cannot generate manifest; missing binary {p}")
        with open(p, "rb") as f:
            data = f.read()
        entries[name] = {
            "mcu_id": mcu_id,
            "version": ver,
            "fw_version_u16": parse_version_u16(ver),
            "filename": fn,
            "sha256": hashlib.sha256(data).hexdigest(),
            "size": len(data),
        }
    doc = {"schema_version": 1, "mcus": entries}
    if out_path is None:
        out_path = os.path.join(mcu_dir, "manifest.json")
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2, sort_keys=True)
        f.write("\n")
    return doc


def self_test():
    with tempfile.TemporaryDirectory() as tmp:
        kb_path = os.path.join(tmp, "keybed.bin")
        pn_path = os.path.join(tmp, "panel.bin")
        with open(kb_path, "wb") as f:
            f.write(b"VELORA-KEYBED-FW-1.2-IMAGE")
        with open(pn_path, "wb") as f:
            f.write(b"VELORA-PANEL-FW-1.2-IMAGE")

        manifest_path = os.path.join(tmp, "manifest.json")
        generate_manifest(tmp, keybed_version="1.2", panel_version="1.2", out_path=manifest_path)

        # 1. Valid manifest passes
        doc = verify_manifest(manifest_path, tmp)
        assert doc["mcus"]["keybed"]["fw_version_u16"] == 0x0102
        assert doc["mcus"]["panel"]["fw_version_u16"] == 0x0102

        # 2. Tampered binary fails build-time check
        with open(kb_path, "wb") as f:
            f.write(b"VELORA-KEYBED-FW-1.2-TAMPERED")
        try:
            verify_manifest(manifest_path, tmp)
            raise AssertionError("expected SHA-256 mismatch failure")
        except ManifestError as exc:
            assert "SHA-256 mismatch" in str(exc), str(exc)

    print("verify_mcu_manifest: self-test ok")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="Verify or generate /usr/lib/velora/mcu/manifest.json (Story 8.4)")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--manifest", default=None)
    ap.add_argument("--mcu-dir", default=None)
    ap.add_argument("--generate", action="store_true")
    ap.add_argument("--keybed-version", default="1.2")
    ap.add_argument("--panel-version", default="1.2")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    try:
        if args.generate:
            if not args.mcu_dir:
                ap.error("--generate requires --mcu-dir")
            generate_manifest(args.mcu_dir, args.keybed_version, args.panel_version, args.manifest)
            return 0
        if not args.manifest:
            ap.error("--manifest is required")
        verify_manifest(args.manifest, args.mcu_dir)
        print("verify_mcu_manifest: ok")
        return 0
    except ManifestError as exc:
        print(f"verify_mcu_manifest: FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
