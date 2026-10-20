#!/usr/bin/env python3
"""Story 6.1 (AR34 / NFR23): fails if private key material is committed or present in the git index / working tree.

Scanned files: git index (`git ls-files`) plus untracked non-ignored working-tree files when ROOT has a .git; otherwise (the Docker copy, the
self-test fixtures) a plain directory walk that skips .git and build directories. Findings:
  1. pem       a PEM private-key block in any file, any size (chunked scan);
  2. name      a file named like a private key: *.seed, *.priv, *.kek, *.aes256, *.p12, *.pfx, *.jks, *.key, *.pem, *.der, *.pkcs8, *.gpg, id_rsa, id_ed25519...;
  3. dev-dir   any file under a directory named velora-dev-keys or .velora-dev-keys (any case);
  4. json      a *.json whose key name says private/seed/secret/kek/priv (or "private": true, or a JWK "d"/"k") holds, as a string or in a list,
               exactly 64 hex digits or base64 of exactly 32 bytes; a JSON over 16 MiB is flagged (json-oversize) instead of skipped;
  5. marker    the dev key directory marker text or a "dev_only": true sidecar (the dev key tool's output must never be committed);
  6. name32    a file of exactly 32 bytes whose name contains seed, priv or kek.
Symlinks are judged by name only. Submodules are reported but not scanned. Reviewed exceptions: tools/ci/private_key_allowlist.txt (path globs).
Public keys (.pub, *.pub-list, public_key_hex) are fine. Usage: check_no_private_keys.py [--root DIR] | --self-test
"""
import argparse
import base64
import binascii
import fnmatch
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

PEM_RE = re.compile(rb"-----BEGIN (?:[A-Z0-9]+ )*PRIVATE KEY(?: BLOCK)?-----")
MARKER_RE = re.compile(rb"DEV-ONLY KEYS \(root=dev\)")
NAME_RE = re.compile(r"(\.(seed|priv|kek|aes256|p12|pfx|jks|key|pem|der|pkcs8|gpg)|^id_(rsa|dsa|ecdsa|ed25519))$", re.I)
NAME32_RE = re.compile(r"(seed|priv|kek)", re.I)
DEV_DIRS = {"velora-dev-keys", ".velora-dev-keys"}
SECRET_KEY_RE = re.compile(r"(private|seed|secret|kek|priv)", re.I)
SKIP_DIRS = {".git", "__pycache__"}  # compiled caches are git-ignored and regenerated; everything else, build directories included, is scanned
CHUNK = 1024 * 1024
JSON_CAP = 16 * 1024 * 1024
ALLOWLIST = "tools/ci/private_key_allowlist.txt"


def _key_like(v):
    """True if the string is exactly 64 hex digits or base64/base64url that decodes to exactly 32 bytes."""
    s = v.strip()
    if re.fullmatch(r"[0-9a-fA-F]{64}", s):
        return True
    if re.fullmatch(r"[A-Za-z0-9+/_-]{43,44}={0,1}", s):
        try:
            t = s.rstrip("=").replace("-", "+").replace("_", "/")
            return len(base64.b64decode(t + "=" * (-len(t) % 4), validate=True)) == 32
        except (binascii.Error, ValueError):
            return False
    return False


def _json_findings(obj, private_flag, path="", name=""):
    out = []
    if isinstance(obj, str):
        if (SECRET_KEY_RE.search(name) or private_flag) and _key_like(obj) and not re.search(r"public|key_id", name, re.I):
            out.append(path)
    elif isinstance(obj, dict):
        flag = private_flag or obj.get("private") is True or str(obj.get("kind", "")).lower() == "private"
        if "kty" in obj:  # JWK: "d" is the private exponent / scalar, "k" the oct key
            for k in ("d", "p", "q", "k"):
                if isinstance(obj.get(k), str) and obj[k]:
                    out.append("%s.%s (JWK)" % (path, k))
        for k, v in obj.items():
            out += _json_findings(v, flag, "%s.%s" % (path, k), k)
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            out += _json_findings(v, private_flag, "%s[%d]" % (path, i), name)  # list items inherit the parent key name
    return out


def _has_pem_or_marker(f):
    """Chunked scan of a whole file (no size cap), overlapping chunks so a match split at a boundary is still found."""
    tail = b""
    kinds = set()
    while True:
        chunk = f.read(CHUNK)
        if not chunk:
            break
        buf = tail + chunk
        if PEM_RE.search(buf):
            kinds.add("pem")
        if MARKER_RE.search(buf):
            kinds.add("marker")
        tail = buf[-128:]
    return kinds


def scan_file(rel, full):
    found = []
    parts = rel.replace("\\", "/").split("/")
    if any(p.lower() in DEV_DIRS for p in parts[:-1]):
        found.append(("dev-dir", rel, "file under a dev key directory"))
    if NAME_RE.search(parts[-1]):
        found.append(("name", rel, "file name looks like private key material"))
    if os.path.islink(full):
        return found  # a symlink is judged by its name only: never followed, same in git and walk mode
    try:
        size = os.path.getsize(full)
        with open(full, "rb") as f:
            for k in sorted(_has_pem_or_marker(f)):
                found.append((k, rel, "PEM private key block" if k == "pem" else "dev-only key directory marker text"))
        if size == 32 and NAME32_RE.search(parts[-1]) and not parts[-1].lower().endswith((".pub", ".md", ".txt", ".json")):
            found.append(("name32", rel, "exactly 32 bytes and named like a seed / private key / KEK"))
        if parts[-1].lower().endswith(".json"):
            if size > JSON_CAP:
                found.append(("json-oversize", rel, "json larger than %d bytes cannot be parsed: reviewed allowlist entry required" % JSON_CAP))
            else:
                with open(full, "rb") as f:
                    data = f.read()
                try:
                    obj = json.loads(data.decode("utf-8"))
                except (ValueError, UnicodeDecodeError):
                    obj = None
                if obj is not None:
                    if isinstance(obj, dict) and obj.get("dev_only") is True:
                        found.append(("marker", rel, "dev-only key sidecar (\"dev_only\": true)"))
                    for h in _json_findings(obj, False):
                        found.append(("json", rel, "private-looking key material at %s" % h))
    except OSError:
        pass
    return found


def gitlinks(root):
    r = subprocess.run(["git", "-C", root, "ls-files", "-s", "-z"], capture_output=True)
    if r.returncode != 0:
        return []
    return [e.split("\t", 1)[1] for e in r.stdout.decode("utf-8", "surrogateescape").split("\0") if e.startswith("160000") and "\t" in e]


def list_files(root):
    if os.path.exists(os.path.join(root, ".git")) and shutil.which("git"):
        r = subprocess.run(["git", "-C", root, "ls-files", "-z", "--cached", "--others", "--exclude-standard"], capture_output=True)
        if r.returncode == 0:
            for g in gitlinks(root):
                print("note: submodule %s is not scanned; run this check inside it" % g, file=sys.stderr)
            return sorted({p for p in r.stdout.decode("utf-8", "surrogateescape").split("\0")
                           if p and (os.path.islink(os.path.join(root, p)) or os.path.isfile(os.path.join(root, p)))})
        print("warning: git ls-files failed, walking the directory instead", file=sys.stderr)
    out = []
    for dp, dns, fns in os.walk(root):  # build directories are scanned too
        dns[:] = [d for d in dns if d not in SKIP_DIRS]
        for fn in fns:
            full = os.path.join(dp, fn)
            if os.path.islink(full) or os.path.isfile(full):
                out.append(os.path.relpath(full, root))
    return sorted(out)


def load_allowlist(root):
    p = os.path.join(root, ALLOWLIST)
    if not os.path.exists(p):
        return []
    with open(p, encoding="utf-8") as f:
        return [l.split("#", 1)[0].strip() for l in f if l.split("#", 1)[0].strip()]


def check(root):
    findings = []
    allow = load_allowlist(root)
    for rel in list_files(root):
        if any(fnmatch.fnmatch(rel.replace("\\", "/"), g) for g in allow):
            continue
        findings += scan_file(rel, os.path.join(root, rel))
    return findings


def run(root):
    findings = check(root)
    for kind, rel, why in findings:
        print("FAIL [%s] %s: %s" % (kind, rel, why), file=sys.stderr)
    if findings:
        print("check_no_private_keys: %d finding(s): private key material must never be committed" % len(findings), file=sys.stderr)
        return 1
    print("check_no_private_keys: ok (no private key material)")
    return 0


def self_test():
    ok = True
    seed = os.urandom(32)

    def expect(cond, what):
        nonlocal ok
        if not cond:
            print("self-test FAILED: " + what, file=sys.stderr)
            ok = False

    def put(root, rel, data):
        p = os.path.join(root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)

    with tempfile.TemporaryDirectory() as d:
        clean = os.path.join(d, "clean")
        put(clean, "src/a.cpp", b"int main(){}\n")
        put(clean, "keys/content-manifest.ed25519.pub", seed)  # public key file: allowed
        put(clean, "keys/trust/dev.pub-list", b"dev content-manifest 21fe31dfa154a261 " + b"d7" * 32 + b"\n")
        put(clean, "keys/side.json", json.dumps({"root": "dev", "key_id": "21fe31dfa154a261", "public_key_hex": seed.hex()}).encode())
        put(clean, "doc.md", b"Private keys live in an HSM; PEM blocks are never committed.\n")
        expect(check(clean) == [], "clean tree must pass: %s" % check(clean))
        cases = {
            "pem": ("deploy/key.txt", ("-----BEGIN " + "PRIVATE KEY-----\nAAAA\n-----END " + "PRIVATE KEY-----\n").encode(), "pem"),
            "pem-rsa": ("a/b.conf", ("x\n-----BEGIN " + "RSA PRIVATE KEY-----\n").encode(), "pem"),
            "pem-openssh": ("a/id", ("-----BEGIN " + "OPENSSH PRIVATE KEY-----\n").encode(), "pem"),
            "pem-past-4mib": ("a/big.bin", b"\0" * (4 * 1024 * 1024 + 7) + ("-----BEGIN " + "PRIVATE KEY-----\n").encode(), "pem"),
            "pem-at-chunk-edge": ("a/edge.bin", b"\0" * (CHUNK - 10) + ("-----BEGIN " + "PRIVATE KEY-----\n").encode(), "pem"),
            "name-seed": ("keys/content-manifest.ed25519.seed", seed, "name"),
            "name-priv": ("keys/device.x25519.priv", seed, "name"),
            "name-kek": ("keys/kek.aes256", seed, "name"),
            "name-key": ("certs/server.key", b"x", "name"),
            "name-pem": ("certs/server.pem", b"x", "name"),
            "name-id": ("home/id_ed25519", b"x", "name"),
            "name-gpg": ("home/secret.gpg", b"x", "name"),
            "name32": ("blob/my_seed_file", seed, "name32"),
            "dev-dir": (".velora-dev-keys/anything.bin", b"x", "dev-dir"),
            "dev-dir-case": ("Velora-Dev-Keys/anything.bin", b"x", "dev-dir"),
            "marker-text": ("notes/x.txt", b"DEV-ONLY " + b"KEYS (root=dev)\n", "marker"),
            "marker-sidecar": ("k/x.json", json.dumps({"root": "dev", "dev_only": True}).encode(), "marker"),
            "json-b64": ("keys/x.json", json.dumps({"root": "dev", "private_seed": base64.b64encode(seed).decode()}).encode(), "json"),
            "json-hex": ("keys/y.json", json.dumps({"nested": {"kek": seed.hex()}}).encode(), "json"),
            "json-flagged": ("keys/z.json", json.dumps({"private": True, "material": base64.b64encode(seed).decode()}).encode(), "json"),
            "json-list": ("keys/l.json", json.dumps({"seeds": [base64.b64encode(seed).decode()]}).encode(), "json"),
            "json-jwk": ("keys/j.json", json.dumps({"kty": "OKP", "crv": "Ed25519", "x": "AAAA", "d": "x"}).encode(), "json"),
            "json-oversize": ("keys/huge.json", b'{"a":"' + b"x" * (JSON_CAP + 1) + b'"}', "json-oversize"),
        }
        for name, (rel, data, kind) in cases.items():
            t = os.path.join(d, name)
            put(t, "ok.txt", b"ok\n")
            put(t, rel, data)
            kinds = {k for k, _, _ in check(t)}
            expect(kind in kinds, "violating fixture '%s' must be flagged as %s, got %s" % (name, kind, kinds))
        # not flagged: 31/33-byte values, a long base64 that is not 32 bytes, a symlink's target content, an allowlisted path
        ok = os.path.join(d, "ok2")
        put(ok, "k/a.json", json.dumps({"seed": base64.b64encode(os.urandom(33)).decode(), "private_note": "x" * 50, "public_key": seed.hex()}).encode())
        put(ok, "k/b.pub", seed)
        put(ok, "my_seed.md", seed)
        expect(check(ok) == [], "near misses pass: %s" % check(ok))
        sl = os.path.join(d, "sl")
        put(sl, "real.txt", ("-----BEGIN " + "PRIVATE KEY-----\n").encode())
        os.symlink(os.path.join(sl, "real.txt"), os.path.join(sl, "link.txt"))
        expect({r for _, r, _ in check(sl)} == {"real.txt"}, "symlinks are not followed")
        al = os.path.join(d, "al")
        put(al, "certs/server.pem", b"x")
        put(al, ALLOWLIST, b"# reviewed\ncerts/*.pem\n")
        expect(check(al) == [], "allowlisted path passes")
        wb = os.path.join(d, "walkbuild")  # the walk fallback scans build directories too
        put(wb, "build/leak.seed", seed)
        expect(any(r == "build/leak.seed" for _, r, _ in check(wb)), "build directories are scanned in walk mode")
        # the generated dev key directory contents are flagged wherever they land
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "keys"))
        import velora_keys
        g = os.path.join(d, "gen")
        velora_keys.generate(os.path.join(g, "out"))
        res = {(r, k) for k, r, _ in check(g)}
        names = {r for r, _ in res}
        expect({"out/content-manifest.ed25519.seed", "out/device.x25519.priv", "out/kek.aes256"} <= names, "generated raw key files flagged: %s" % names)
        expect({"out/content-manifest.json", "out/kek.json", "out/device.json", "out/DEV-ONLY.txt"} <= names and ("out/DEV-ONLY.txt", "marker") in res,
               "sidecars and the DEV-ONLY marker are flagged: %s" % res)
        expect(not any(n.endswith(".pub") for n in names), "public keys are not flagged: %s" % names)
        # a git work tree: an untracked-but-not-ignored key file and a staged key file are both found
        if shutil.which("git"):
            gt = os.path.join(d, "gitrepo")
            put(gt, "a.txt", b"a\n")
            subprocess.run(["git", "-C", gt, "init", "-q"], check=True)
            expect(check(gt) == [], "clean git tree passes")
            put(gt, "staged.seed", seed)
            subprocess.run(["git", "-C", gt, "add", "staged.seed"], check=True)
            put(gt, "loose.priv", seed)
            put(gt, ".gitignore", b"ignored/\n")
            put(gt, "ignored/x.seed", seed)
            names = {r for _, r, _ in check(gt)}
            expect(names == {"staged.seed", "loose.priv"}, "git index and untracked files found, ignored skipped: %s" % names)
    print("check_no_private_keys self-test: " + ("passed" if ok else "FAILED"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    return self_test() if a.self_test else run(os.path.abspath(a.root))


if __name__ == "__main__":
    sys.exit(main())
