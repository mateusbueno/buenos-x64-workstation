#!/usr/bin/env python3
"""Story 8.2: Production release signing, per-root update bundles, and release record traceability.

Implements the offline-signing workflow for Velora RAUC A/B update bundles across the three
cryptographic roots (`dev`, `factory-test`, `prod`):
  1. `prepare-request`: runs in the build environment WITHOUT any production private key. Refuses to run
     if any private key file or private-key environment variable is present for `prod`. Produces an
     unsigned bundle payload and a `signing-request.json` containing the SHA-256 digests to be signed
     in the offline HSM ceremony (Story 6.10).
  2. `attach-signature`: verifies the detached signature returned from the offline HSM (or dev/factory-test
     signer) against the target root's public key, refuses cross-root signatures or mismatched payload
     digests, writes the signed `.raucb` bundle (`VELORA-RAUCB-V1`), and appends/updates `release-record.json`.
  3. `verify-bundle`: verifies a `.raucb` bundle against a unit's single trusted root (`prod`,
     `factory-test`, or `dev`). A `prod` unit rejects `dev`-signed and `factory-test`-signed bundles
     with `wrong_root` before touching either A/B slot.
  4. `trace-unit`: traces an installed version or bundle SHA-256 back to the exact `image_sha256`,
     `bundle_sha256`, `signing_key_fingerprint`, `root_ca_fingerprint`, and `ceremony_id` in
     `release-record.json`.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "keys"))
from velora_keys import (  # noqa: E402
    _D,
    _G,
    _L,
    _P,
    _SQRT_M1,
    _compress,
    _edwards_add,
    _edwards_mul,
    ed25519_public,
    ed25519_sign,
    ed25519_validate_public,
)

VALID_ROOTS = ("dev", "factory-test", "prod")
BUNDLE_MAGIC = b"VELORA-RAUCB-V1\n"
UNSIGNED_MAGIC = b"VELORA-RAUCB-UNSIGNED-V1\n"
FORBIDDEN_ENV_VARS = (
    "VELORA_RAUC_KEY",
    "RAUC_KEY_FILE",
    "VELORA_PROD_PRIVATE_KEY",
    "RAUC_PRIVATE_KEY",
)
PRIVATE_KEY_FILE_RE = re.compile(
    r"(\.(seed|priv|kek|aes256|p12|pfx|jks|key|pem|der|pkcs8|gpg)|^id_(rsa|dsa|ecdsa|ed25519))$",
    re.I,
)
HEX64_RE = re.compile(r"^[0-9a-f]{64}$")
HEX128_RE = re.compile(r"^[0-9a-f]{128}$")


class ReleaseSignError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code
        self.message = message


def _decompress_point(pub):
    err = ed25519_validate_public(pub)
    if err is not None:
        raise ValueError(f"invalid Ed25519 public key: {err}")
    n = int.from_bytes(pub, "little")
    y, sign = n & ((1 << 255) - 1), n >> 255
    x2 = (y * y - 1) * pow(_D * y * y + 1, _P - 2, _P) % _P
    x = pow(x2, (_P + 3) // 8, _P)
    if (x * x - x2) % _P:
        x = x * _SQRT_M1 % _P
    if x & 1 != sign:
        x = _P - x
    return (x, y, 1, x * y % _P)


def ed25519_verify(pub_bytes, msg_bytes, sig_bytes):
    """Pure-Python RFC 8032 Ed25519 verification."""
    if len(pub_bytes) != 32 or len(sig_bytes) != 64:
        return False
    try:
        A = _decompress_point(pub_bytes)
        R = _decompress_point(sig_bytes[:32])
    except ValueError:
        return False
    s = int.from_bytes(sig_bytes[32:], "little")
    if s >= _L:
        return False
    k = int.from_bytes(hashlib.sha512(sig_bytes[:32] + pub_bytes + msg_bytes).digest(), "little") % _L
    lhs = _edwards_mul(8, _edwards_mul(s, _G))
    rhs = _edwards_mul(8, _edwards_add(R, _edwards_mul(k, A)))
    return _compress(lhs) == _compress(rhs)


def canonical_signing_message(target_root, payload_sha256):
    return f"velora-rauc-bundle-v1:{target_root}:{payload_sha256}".encode("utf-8")


def _scan_for_private_keys(input_dir, env=None):
    env = os.environ if env is None else env
    for var in FORBIDDEN_ENV_VARS:
        if env.get(var):
            raise ReleaseSignError(
                "private_key_rejected",
                f"forbidden private key environment variable {var} is set in the release build environment",
            )
    for dp, _, fns in os.walk(input_dir):
        for fn in fns:
            if PRIVATE_KEY_FILE_RE.search(fn):
                raise ReleaseSignError(
                    "private_key_rejected",
                    f"private key file {fn} found in build directory; production private keys must never enter the build environment",
                )


def prepare_request(
    input_dir,
    out_dir,
    target_root="prod",
    version="1.0.0",
    compatible="velora-x86_64",
    env=None,
):
    if target_root not in VALID_ROOTS:
        raise ReleaseSignError("invalid_root", f"unknown target root: {target_root}")
    _scan_for_private_keys(input_dir, env=env)

    rootfs_path = os.path.join(input_dir, "rootfs.ext4")
    if not os.path.isfile(rootfs_path):
        raise ReleaseSignError("missing_rootfs", f"missing rootfs.ext4 in {input_dir}")
    with open(rootfs_path, "rb") as f:
        rootfs_bytes = f.read()
    image_sha256 = hashlib.sha256(rootfs_bytes).hexdigest()

    manifest_path = os.path.join(input_dir, "manifest.raucm")
    if os.path.isfile(manifest_path):
        with open(manifest_path, "rb") as f:
            manifest_bytes = f.read()
    else:
        manifest_text = (
            "[update]\n"
            f"compatible={compatible}\n"
            f"version={version}\n"
            "\n"
            "[image.rootfs]\n"
            "filename=rootfs.ext4\n"
            f"sha256={image_sha256}\n"
            f"size={len(rootfs_bytes)}\n"
        )
        manifest_bytes = manifest_text.encode("utf-8")
    manifest_sha256 = hashlib.sha256(manifest_bytes).hexdigest()

    header = {
        "schema_version": 1,
        "target_root": target_root,
        "version": version,
        "compatible": compatible,
        "image_sha256": image_sha256,
        "image_size": len(rootfs_bytes),
        "manifest_sha256": manifest_sha256,
    }
    header_bytes = (json.dumps(header, sort_keys=True) + "\n").encode("utf-8")
    unsigned_payload = UNSIGNED_MAGIC + header_bytes + b"--MANIFEST--\n" + manifest_bytes + b"\n--ROOTFS--\n" + rootfs_bytes
    payload_sha256 = hashlib.sha256(unsigned_payload).hexdigest()

    os.makedirs(out_dir, exist_ok=True)
    payload_out = os.path.join(out_dir, "unsigned.payload")
    with open(payload_out, "wb") as f:
        f.write(unsigned_payload)

    signing_request = {
        "schema_version": 1,
        "target_root": target_root,
        "version": version,
        "compatible": compatible,
        "image_sha256": image_sha256,
        "manifest_sha256": manifest_sha256,
        "payload_sha256": payload_sha256,
        "signing_message_hex": canonical_signing_message(target_root, payload_sha256).hex(),
    }
    req_out = os.path.join(out_dir, "signing-request.json")
    with open(req_out, "w", encoding="utf-8") as f:
        json.dump(signing_request, f, indent=2, sort_keys=True)
        f.write("\n")
    return signing_request, payload_out, req_out


def attach_signature(
    request_path,
    unsigned_payload_path,
    signature_hex,
    signer_pub_hex,
    signer_root,
    root_ca_pub_hex,
    out_bundle_path,
    record_path=None,
    ceremony_id="HSM-CEREMONY-001",
    signed_at_utc="2026-10-29T12:00:00Z",
):
    if signer_root not in VALID_ROOTS:
        raise ReleaseSignError("invalid_root", f"unknown signer root: {signer_root}")
    with open(request_path, "r", encoding="utf-8") as f:
        req = json.load(f)
    target_root = req.get("target_root")
    if target_root not in VALID_ROOTS:
        raise ReleaseSignError("invalid_root", f"invalid target_root in request: {target_root}")
    if signer_root != target_root:
        raise ReleaseSignError(
            "cross_root_rejected",
            f"cannot attach signature from root '{signer_root}' to bundle targeting root '{target_root}'",
        )

    with open(unsigned_payload_path, "rb") as f:
        unsigned_payload = f.read()
    actual_payload_sha256 = hashlib.sha256(unsigned_payload).hexdigest()
    if actual_payload_sha256 != req.get("payload_sha256"):
        raise ReleaseSignError(
            "payload_mismatch",
            "unsigned payload SHA-256 does not match signing-request.json",
        )

    signer_pub_hex = signer_pub_hex.strip().lower()
    root_ca_pub_hex = root_ca_pub_hex.strip().lower()
    signature_hex = signature_hex.strip().lower()
    if not HEX64_RE.match(signer_pub_hex) or not HEX64_RE.match(root_ca_pub_hex):
        raise ReleaseSignError("invalid_public_key", "signer and root CA public keys must be 64-hex Ed25519 keys")
    if not HEX128_RE.match(signature_hex):
        raise ReleaseSignError("invalid_signature", "signature_hex must be 128 hex characters (64 bytes)")

    signer_pub = bytes.fromhex(signer_pub_hex)
    root_ca_pub = bytes.fromhex(root_ca_pub_hex)
    sig_bytes = bytes.fromhex(signature_hex)

    msg = canonical_signing_message(target_root, actual_payload_sha256)
    if not ed25519_verify(signer_pub, msg, sig_bytes):
        raise ReleaseSignError("bad_signature", "detached signature verification failed against signer public key")

    signing_key_fp = hashlib.sha256(signer_pub).hexdigest()
    root_ca_fp = hashlib.sha256(root_ca_pub).hexdigest()

    envelope = {
        "schema_version": 1,
        "root": target_root,
        "version": req["version"],
        "compatible": req["compatible"],
        "image_sha256": req["image_sha256"],
        "manifest_sha256": req["manifest_sha256"],
        "payload_sha256": actual_payload_sha256,
        "signer_pub_hex": signer_pub_hex,
        "root_ca_pub_hex": root_ca_pub_hex,
        "signing_key_fingerprint": signing_key_fp,
        "root_ca_fingerprint": root_ca_fp,
        "signature_hex": signature_hex,
        "ceremony_id": ceremony_id,
        "signed_at_utc": signed_at_utc,
    }
    envelope_bytes = (json.dumps(envelope, sort_keys=True) + "\n").encode("utf-8")
    bundle_bytes = (
        BUNDLE_MAGIC
        + f"root={target_root}\n".encode("utf-8")
        + envelope_bytes
        + b"--PAYLOAD--\n"
        + unsigned_payload
    )
    bundle_sha256 = hashlib.sha256(bundle_bytes).hexdigest()

    os.makedirs(os.path.dirname(os.path.abspath(out_bundle_path)), exist_ok=True)
    with open(out_bundle_path, "wb") as f:
        f.write(bundle_bytes)

    record_entry = {
        "variant": target_root,
        "version": req["version"],
        "compatible": req["compatible"],
        "image_sha256": req["image_sha256"],
        "bundle_sha256": bundle_sha256,
        "signing_key_fingerprint": signing_key_fp,
        "root_ca_fingerprint": root_ca_fp,
        "ceremony_id": ceremony_id,
        "signed_at_utc": signed_at_utc,
    }

    if record_path:
        if os.path.isfile(record_path):
            with open(record_path, "r", encoding="utf-8") as f:
                record_doc = json.load(f)
        else:
            record_doc = {"schema_version": 1, "releases": []}
        releases = [
            r
            for r in record_doc.get("releases", [])
            if not (r.get("variant") == target_root and r.get("version") == req["version"])
        ]
        releases.append(record_entry)
        record_doc["releases"] = sorted(releases, key=lambda r: (r["version"], r["variant"]))
        with open(record_path, "w", encoding="utf-8") as f:
            json.dump(record_doc, f, indent=2, sort_keys=True)
            f.write("\n")

    return record_entry


def verify_bundle(bundle_path, trusted_root, trusted_signer_pub_hex=None, expected_compatible="velora-x86_64"):
    if trusted_root not in VALID_ROOTS:
        raise ReleaseSignError("invalid_root", f"unknown trusted root: {trusted_root}")
    with open(bundle_path, "rb") as f:
        data = f.read()
    if not data.startswith(BUNDLE_MAGIC):
        raise ReleaseSignError("bad_format", "bundle does not start with VELORA-RAUCB-V1 header")

    rest = data[len(BUNDLE_MAGIC):]
    nl1 = rest.find(b"\n")
    nl2 = rest.find(b"\n", nl1 + 1) if nl1 != -1 else -1
    marker = b"--PAYLOAD--\n"
    p_idx = rest.find(marker)
    if nl1 == -1 or nl2 == -1 or p_idx == -1:
        raise ReleaseSignError("bad_format", "malformed bundle header")

    root_line = rest[:nl1].decode("utf-8", errors="replace")
    envelope_raw = rest[nl1 + 1:p_idx]
    unsigned_payload = rest[p_idx + len(marker):]

    try:
        envelope = json.loads(envelope_raw.decode("utf-8"))
    except Exception as exc:
        raise ReleaseSignError("bad_format", f"invalid bundle JSON envelope: {exc}") from exc

    bundle_root = envelope.get("root", "")
    if root_line != f"root={bundle_root}":
        raise ReleaseSignError("bad_format", "inconsistent root marker in bundle header")

    if bundle_root != trusted_root:
        raise ReleaseSignError(
            "wrong_root",
            f"unit trusts root '{trusted_root}' only; rejecting bundle signed under root '{bundle_root}'",
        )

    if expected_compatible and envelope.get("compatible") != expected_compatible:
        raise ReleaseSignError(
            "incompatible",
            f"bundle compatible '{envelope.get('compatible')}' != '{expected_compatible}'",
        )

    actual_payload_sha256 = hashlib.sha256(unsigned_payload).hexdigest()
    if actual_payload_sha256 != envelope.get("payload_sha256"):
        raise ReleaseSignError("bad_signature", "bundle payload SHA-256 mismatch (corrupted or tampered)")

    signer_pub_hex = envelope.get("signer_pub_hex", "")
    if trusted_signer_pub_hex and signer_pub_hex.lower() != trusted_signer_pub_hex.strip().lower():
        raise ReleaseSignError("untrusted_signer", "bundle signer public key is not in the unit's keyring")

    try:
        signer_pub = bytes.fromhex(signer_pub_hex)
        sig_bytes = bytes.fromhex(envelope.get("signature_hex", ""))
    except ValueError as exc:
        raise ReleaseSignError("bad_signature", f"invalid hex in bundle signature: {exc}") from exc

    msg = canonical_signing_message(bundle_root, actual_payload_sha256)
    if not ed25519_verify(signer_pub, msg, sig_bytes):
        raise ReleaseSignError("bad_signature", "Ed25519 signature verification failed")

    return envelope


def verify_release_record(record_path, require_all_variants=True):
    with open(record_path, "r", encoding="utf-8") as f:
        doc = json.load(f)
    releases = doc.get("releases", [])
    if not releases:
        raise ReleaseSignError("empty_record", "release-record.json contains no release entries")
    seen_variants = set()
    for r in releases:
        v = r.get("variant")
        if v not in VALID_ROOTS:
            raise ReleaseSignError("invalid_record", f"invalid variant in release record: {v}")
        seen_variants.add(v)
        for field in ("image_sha256", "bundle_sha256", "signing_key_fingerprint", "root_ca_fingerprint"):
            val = str(r.get(field, ""))
            if not HEX64_RE.match(val):
                raise ReleaseSignError("invalid_record", f"field {field} for {v} is not a 64-hex SHA-256")
        if not r.get("version") or not r.get("ceremony_id"):
            raise ReleaseSignError("invalid_record", f"missing version or ceremony_id for {v}")
    if require_all_variants and seen_variants != set(VALID_ROOTS):
        missing = sorted(set(VALID_ROOTS) - seen_variants)
        raise ReleaseSignError("incomplete_record", f"release record missing variants: {missing}")
    return releases


def trace_unit(record_path, bundle_sha256=None, version=None, variant=None):
    releases = verify_release_record(record_path, require_all_variants=False)
    for r in releases:
        if bundle_sha256 and r["bundle_sha256"] == bundle_sha256.lower():
            return r
        if version and variant and r["version"] == version and r["variant"] == variant:
            return r
    raise ReleaseSignError("not_found", "no matching signing event found in release-record.json")


def self_test():
    with tempfile.TemporaryDirectory() as tmp:
        # Derive deterministic test keypairs in memory for dev, factory-test, and prod (simulating HSM for prod)
        keys = {}
        for idx, root in enumerate(VALID_ROOTS):
            ca_seed = hashlib.sha256(f"test-ca-{root}-{idx}".encode()).digest()
            signer_seed = hashlib.sha256(f"test-signer-{root}-{idx}".encode()).digest()
            keys[root] = {
                "ca_pub": ed25519_public(ca_seed).hex(),
                "signer_seed": signer_seed,
                "signer_pub": ed25519_public(signer_seed).hex(),
            }

        # 1. Build input directory with rootfs.ext4
        in_dir = os.path.join(tmp, "build_in")
        os.makedirs(in_dir)
        with open(os.path.join(in_dir, "rootfs.ext4"), "wb") as f:
            f.write(b"VELORA-ROOTFS-IMAGE-V1.6.0-BLOCKS" * 128)

        # 2. Verify prepare-request fails closed if private key env var or file is present
        try:
            prepare_request(in_dir, os.path.join(tmp, "out_fail"), target_root="prod", env={"VELORA_RAUC_KEY": "/tmp/x"})
            raise AssertionError("expected private_key_rejected on env var")
        except ReleaseSignError as e:
            assert e.code == "private_key_rejected", e.code

        leaked_key = os.path.join(in_dir, "accidental.key")
        with open(leaked_key, "wb") as f:
            f.write(b"secret")
        try:
            prepare_request(in_dir, os.path.join(tmp, "out_fail"), target_root="prod", env={})
            raise AssertionError("expected private_key_rejected on key file")
        except ReleaseSignError as e:
            assert e.code == "private_key_rejected", e.code
        os.remove(leaked_key)

        # 3. Prepare and sign bundles for all 3 roots (dev, factory-test, prod) and record in release-record.json
        record_path = os.path.join(tmp, "release-record.json")
        bundles = {}
        entries = {}
        for root in VALID_ROOTS:
            req_dir = os.path.join(tmp, f"req_{root}")
            req, payload_path, req_path = prepare_request(
                in_dir, req_dir, target_root=root, version="1.6.0", compatible="velora-x86_64", env={}
            )
            msg = bytes.fromhex(req["signing_message_hex"])
            sig_hex = ed25519_sign(keys[root]["signer_seed"], msg).hex()

            # Cross-root attach attempt must fail
            wrong_root = "dev" if root == "prod" else "prod"
            try:
                attach_signature(
                    req_path,
                    payload_path,
                    sig_hex,
                    keys[wrong_root]["signer_pub"],
                    wrong_root,
                    keys[wrong_root]["ca_pub"],
                    os.path.join(tmp, "bad.raucb"),
                )
                raise AssertionError("expected cross_root_rejected")
            except ReleaseSignError as e:
                assert e.code == "cross_root_rejected", e.code

            bundle_out = os.path.join(tmp, f"velora-1.6.0-{root}.raucb")
            entry = attach_signature(
                req_path,
                payload_path,
                sig_hex,
                keys[root]["signer_pub"],
                root,
                keys[root]["ca_pub"],
                bundle_out,
                record_path=record_path,
                ceremony_id=f"CEREMONY-{root.upper()}-20261029",
            )
            bundles[root] = bundle_out
            entries[root] = entry

        # 4. AC2: A production unit (trusted_root="prod") rejects dev and factory-test bundles and accepts prod bundle
        for untrusted in ("dev", "factory-test"):
            try:
                verify_bundle(bundles[untrusted], trusted_root="prod", trusted_signer_pub_hex=keys["prod"]["signer_pub"])
                raise AssertionError(f"prod unit must reject {untrusted} bundle")
            except ReleaseSignError as e:
                assert e.code == "wrong_root", e.code

        verified_env = verify_bundle(
            bundles["prod"], trusted_root="prod", trusted_signer_pub_hex=keys["prod"]["signer_pub"]
        )
        assert verified_env["root"] == "prod"
        assert verified_env["version"] == "1.6.0"

        # Tampered prod bundle is rejected with bad_signature
        tampered_path = os.path.join(tmp, "tampered.raucb")
        with open(bundles["prod"], "rb") as f:
            raw = bytearray(f.read())
        raw[-1] ^= 0xFF
        with open(tampered_path, "wb") as f:
            f.write(raw)
        try:
            verify_bundle(tampered_path, trusted_root="prod", trusted_signer_pub_hex=keys["prod"]["signer_pub"])
            raise AssertionError("tampered bundle must fail verification")
        except ReleaseSignError as e:
            assert e.code == "bad_signature", e.code

        # 5. AC3: Verify release-record.json lists all 3 variants and traces any field unit back to its signing event
        rels = verify_release_record(record_path, require_all_variants=True)
        assert len(rels) == 3
        traced_by_hash = trace_unit(record_path, bundle_sha256=entries["prod"]["bundle_sha256"])
        assert traced_by_hash["signing_key_fingerprint"] == entries["prod"]["signing_key_fingerprint"]
        assert traced_by_hash["root_ca_fingerprint"] == entries["prod"]["root_ca_fingerprint"]
        assert traced_by_hash["ceremony_id"] == "CEREMONY-PROD-20261029"

        traced_by_ver = trace_unit(record_path, version="1.6.0", variant="prod")
        assert traced_by_ver["bundle_sha256"] == entries["prod"]["bundle_sha256"]

    print("velora_release_sign: self-test ok")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="Velora release signing and per-root bundle tool (Story 8.2)")
    ap.add_argument("--self-test", action="store_true", help="run built-in self-tests")
    sub = ap.add_subparsers(dest="cmd")

    p_prep = sub.add_parser("prepare-request", help="create unsigned bundle payload and signing-request.json")
    p_prep.add_argument("--input-dir", required=True)
    p_prep.add_argument("--out-dir", required=True)
    p_prep.add_argument("--root", default="prod", choices=VALID_ROOTS)
    p_prep.add_argument("--version", required=True)
    p_prep.add_argument("--compatible", default="velora-x86_64")

    p_att = sub.add_parser("attach-signature", help="verify offline signature and emit signed .raucb + release-record.json")
    p_att.add_argument("--request", required=True)
    p_att.add_argument("--unsigned-payload", required=True)
    p_att.add_argument("--signature-hex", required=True)
    p_att.add_argument("--signer-pub-hex", required=True)
    p_att.add_argument("--signer-root", required=True, choices=VALID_ROOTS)
    p_att.add_argument("--root-ca-pub-hex", required=True)
    p_att.add_argument("--out-bundle", required=True)
    p_att.add_argument("--record", default=None)
    p_att.add_argument("--ceremony-id", default="HSM-CEREMONY-001")

    p_ver = sub.add_parser("verify-bundle", help="verify a .raucb bundle against a unit's trusted root")
    p_ver.add_argument("--bundle", required=True)
    p_ver.add_argument("--trusted-root", required=True, choices=VALID_ROOTS)
    p_ver.add_argument("--trusted-signer-pub-hex", default=None)

    p_tr = sub.add_parser("trace-unit", help="trace an installed version or bundle hash in release-record.json")
    p_tr.add_argument("--record", required=True)
    p_tr.add_argument("--bundle-sha256", default=None)
    p_tr.add_argument("--version", default=None)
    p_tr.add_argument("--variant", default=None, choices=VALID_ROOTS)

    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    try:
        if args.cmd == "prepare-request":
            req, _, _ = prepare_request(args.input_dir, args.out_dir, args.root, args.version, args.compatible)
            print(json.dumps(req, indent=2, sort_keys=True))
            return 0
        if args.cmd == "attach-signature":
            now_utc = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
            entry = attach_signature(
                args.request,
                args.unsigned_payload,
                args.signature_hex,
                args.signer_pub_hex,
                args.signer_root,
                args.root_ca_pub_hex,
                args.out_bundle,
                record_path=args.record,
                ceremony_id=args.ceremony_id,
                signed_at_utc=now_utc,
            )
            print(json.dumps(entry, indent=2, sort_keys=True))
            return 0
        if args.cmd == "verify-bundle":
            env = verify_bundle(args.bundle, args.trusted_root, args.trusted_signer_pub_hex)
            print(json.dumps(env, indent=2, sort_keys=True))
            return 0
        if args.cmd == "trace-unit":
            entry = trace_unit(args.record, args.bundle_sha256, args.version, args.variant)
            print(json.dumps(entry, indent=2, sort_keys=True))
            return 0
        ap.print_help()
        return 2
    except ReleaseSignError as exc:
        print(f"error [{exc.code}]: {exc.message}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
