#!/usr/bin/env python3
"""Velora dev key tool (Story 6.1): generates the DEV-ROOT content keys into a directory OUTSIDE the repository.

Python 3 standard library only (no `cryptography`, no OpenSSL binding): Ed25519 public-key derivation (RFC 8032) and X25519 (RFC 7748) are
implemented below, verified against the RFC test vectors by tools/keys/tests/test_keys.py. They are NOT constant-time: this is developer
tooling for DEV keys only. Production keys are generated inside an offline HSM (Story 6.10) and never by this tool.

Commands:
  generate [--out DIR] [--force]   write content-manifest (Ed25519), kek (AES-256) and device (X25519) dev keys
  show [--dir DIR]                 print the key ids and public keys of a dev key directory (no private material)
  sign [--dir DIR] FILE            print the hex Ed25519 signature of FILE under the dev content-manifest key
  trust-entry [--dir DIR]          print the `tools/keys/trust/dev.pub-list` line for the dev content-manifest key
  default-dir                      print the default output directory

Formats (consumed by Stories 6.2, 6.3, 6.5, 6.8; see KEY-HIERARCHY.md):
  <name>.ed25519.seed / .pub   raw 32 bytes: the RFC 8032 seed (NOT a libsodium 64-byte secret key) / public key
  <name>.x25519.priv           raw 32-byte X25519 scalar, stored unclamped (clamping happens inside X25519)
  kek.aes256                   raw 32 bytes
  <name>.x25519.priv / .pub    raw 32 bytes (RFC 7748 scalar / u-coordinate)
  key id                       first 8 bytes of SHA-256(public key); for the KEK, of the SHA-256 of the literal "velora-kek-id:" + key
  <name>.json                  sidecar: root=dev, dev_only=true, key ids, public key (never private material)
"""
import argparse
import hashlib
import json
import os
import shutil
import stat
import sys
import tempfile

ROOT = "dev"
KEY_DIR_NAME = "velora-dev-keys"

# --------------------------------------------------------------------------------------------- Ed25519 (RFC 8032), public-key derivation
_P = 2 ** 255 - 19
_D = -121665 * pow(121666, _P - 2, _P) % _P
_SQRT_M1 = pow(2, (_P - 1) // 4, _P)


def _recover_x(y, sign):
    x2 = (y * y - 1) * pow(_D * y * y + 1, _P - 2, _P) % _P
    x = pow(x2, (_P + 3) // 8, _P)
    if (x * x - x2) % _P:
        x = x * _SQRT_M1 % _P
    if x & 1 != sign:
        x = _P - x
    return x


def _edwards_add(a, b):
    A = (a[1] - a[0]) * (b[1] - b[0]) % _P
    B = (a[1] + a[0]) * (b[1] + b[0]) % _P
    C = 2 * a[3] * b[3] * _D % _P
    D = 2 * a[2] * b[2] % _P
    E, F, G, H = B - A, D - C, D + C, B + A
    return (E * F % _P, G * H % _P, F * G % _P, E * H % _P)


def _edwards_mul(s, pt):
    q = (0, 1, 1, 0)
    while s:
        if s & 1:
            q = _edwards_add(q, pt)
        pt = _edwards_add(pt, pt)
        s >>= 1
    return q


_GY = 4 * pow(5, _P - 2, _P) % _P
_GX = _recover_x(_GY, 0)
_G = (_GX, _GY, 1, _GX * _GY % _P)


def ed25519_public(seed):
    if len(seed) != 32:
        raise ValueError("Ed25519 seed must be 32 bytes")
    h = bytearray(hashlib.sha512(seed).digest()[:32])
    h[0] &= 248
    h[31] &= 127
    h[31] |= 64
    x, y, z, _ = _edwards_mul(int.from_bytes(h, "little"), _G)
    zi = pow(z, _P - 2, _P)
    x, y = x * zi % _P, y * zi % _P
    return (y | ((x & 1) << 255)).to_bytes(32, "little")


_L = 2 ** 252 + 27742317777372353535851937790883648493


def ed25519_validate_public(pub):
    """None if `pub` is a canonical, on-curve, non-small-order Ed25519 public key, else the reason."""
    if len(pub) != 32:
        return "not 32 bytes"
    n = int.from_bytes(pub, "little")
    y, sign = n & ((1 << 255) - 1), n >> 255
    if y >= _P:
        return "non-canonical encoding (y >= p)"
    x2 = (y * y - 1) * pow(_D * y * y + 1, _P - 2, _P) % _P
    x = pow(x2, (_P + 3) // 8, _P)
    if (x * x - x2) % _P:
        x = x * _SQRT_M1 % _P
    if (x * x - x2) % _P:
        return "not on the curve"
    if x == 0 and sign:
        return "non-canonical encoding (x = 0 with sign bit)"
    if x & 1 != sign:
        x = _P - x
    X, Y, Z, _ = _edwards_mul(8, (x, y, 1, x * y % _P))
    if X % _P == 0 and (Y - Z) % _P == 0:
        return "small-order point"
    return None


def _compress(pt):
    x, y, z, _ = pt
    zi = pow(z, _P - 2, _P)
    x, y = x * zi % _P, y * zi % _P
    return (y | ((x & 1) << 255)).to_bytes(32, "little")


def ed25519_sign(seed, msg):
    """Pure Ed25519 (RFC 8032) signature, 64 bytes."""
    h = hashlib.sha512(seed).digest()
    a = bytearray(h[:32])
    a[0] &= 248
    a[31] &= 127
    a[31] |= 64
    a = int.from_bytes(a, "little")
    pub = ed25519_public(seed)
    r = int.from_bytes(hashlib.sha512(h[32:] + msg).digest(), "little") % _L
    rs = _compress(_edwards_mul(r, _G))
    k = int.from_bytes(hashlib.sha512(rs + pub + msg).digest(), "little") % _L
    return rs + ((r + k * a) % _L).to_bytes(32, "little")


# --------------------------------------------------------------------------------------------- X25519 (RFC 7748)
def x25519(k, u):
    if len(k) != 32 or len(u) != 32:
        raise ValueError("X25519 inputs must be 32 bytes")
    kb = bytearray(k)
    kb[0] &= 248
    kb[31] &= 127
    kb[31] |= 64
    kk = int.from_bytes(kb, "little")
    x1 = (int.from_bytes(u, "little") & ((1 << 255) - 1)) % _P
    x2, z2, x3, z3, swap = 1, 0, x1, 1, 0
    for t in range(254, -1, -1):
        bit = (kk >> t) & 1
        swap ^= bit
        if swap:
            x2, x3, z2, z3 = x3, x2, z3, z2
        swap = bit
        A, B = (x2 + z2) % _P, (x2 - z2) % _P
        AA, BB = A * A % _P, B * B % _P
        E = (AA - BB) % _P
        C, D = (x3 + z3) % _P, (x3 - z3) % _P
        DA, CB = D * A % _P, C * B % _P
        x3 = pow(DA + CB, 2, _P)
        z3 = x1 * pow(DA - CB, 2, _P) % _P
        x2 = AA * BB % _P
        z2 = E * (AA + 121665 * E) % _P
    if swap:
        x2, x3, z2, z3 = x3, x2, z3, z2
    return (x2 * pow(z2, _P - 2, _P) % _P).to_bytes(32, "little")


def x25519_public(priv):
    return x25519(priv, (9).to_bytes(32, "little"))


# --------------------------------------------------------------------------------------------- key ids, paths
def key_id(public):
    """First 8 bytes of SHA-256(public key)."""
    return hashlib.sha256(public).digest()[:8]


def kek_id(kek):
    """The KEK has no public key: its id is derived from a domain-separated hash so the key itself is never hashed bare."""
    return hashlib.sha256(b"velora-kek-id:" + kek).digest()[:8]


def default_dir(env=None):
    env = os.environ if env is None else env
    xdg = env.get("XDG_DATA_HOME")
    if xdg and os.path.isabs(xdg):
        return os.path.join(xdg, KEY_DIR_NAME)
    home = env.get("HOME") or os.path.expanduser("~")
    return os.path.join(home, "." + KEY_DIR_NAME)


REPO_ROOT = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def refusal_reason(path):
    """Why `path` (which may not exist yet) must not receive keys, or None: inside a git work tree (some ancestor holds a `.git` directory
    or file) or inside the repository that holds tools/keys (works for a copy without .git, e.g. the Docker tree)."""
    real = os.path.realpath(os.path.abspath(path))
    if real == REPO_ROOT or real.startswith(REPO_ROOT + os.sep):
        return "%s is inside the repository %s" % (real, REPO_ROOT)
    cur = real
    while True:
        if os.path.exists(os.path.join(cur, ".git")):
            home = os.path.realpath(os.path.expanduser("~"))
            hint = " ($HOME itself is a git repository: pass --out outside it)" if cur == home else ""
            return "%s is inside the git work tree %s%s" % (real, cur, hint)
        parent = os.path.dirname(cur)
        if parent == cur:
            return None
        cur = parent


def inside_git_work_tree(path):
    return refusal_reason(path) is not None


class KeyToolError(Exception):
    pass


def _write_private(path, data):
    """Create `path` exclusively, never following a symlink, mode 0600."""
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0), 0o600)
    try:
        os.fchmod(fd, 0o600)
        view = memoryview(data)
        while view:
            view = view[os.write(fd, view):]
    finally:
        os.close(fd)


def _sidecar(role, algorithm, ident, public, files):
    d = {"root": ROOT, "dev_only": True, "role": role, "algorithm": algorithm, "key_id": ident.hex(), "files": files,
         "warning": "DEV-ONLY key. Never trusted by factory-test or production images. Private material lives only in the raw key files."}
    if public is not None:
        d["public_key_hex"] = public.hex()
    return json.dumps(d, indent=2, sort_keys=True) + "\n"


DEV_ONLY_TXT = ("DEV-ONLY " "KEYS (root=dev)\n"
                "Generated by tools/keys/velora_keys.py. These keys are trusted only by dev image variants.\n"
                "Never copy them into a repository, a factory-test image or a production image.\n")


def generate(out_dir, force=False, rng=os.urandom):
    """Writes the dev keys. All files are first written into a staging directory (0700, created with mkdtemp) and then moved into place, so a
    failure never leaves a half-written key set. The output directory is chmod-ed 0700 only when this call created it."""
    out_dir = os.path.abspath(out_dir)
    why = refusal_reason(out_dir)
    if why:
        raise KeyToolError("refusing to write keys: " + why)
    names = ["content-manifest.ed25519.seed", "content-manifest.ed25519.pub", "content-manifest.json", "kek.aes256", "kek.json",
             "device.x25519.priv", "device.x25519.pub", "device.json", "DEV-ONLY.txt"]
    try:
        st = os.lstat(out_dir)
    except FileNotFoundError:
        st = None
    if st is not None:
        if not stat.S_ISDIR(st.st_mode):
            raise KeyToolError("%s exists and is not a directory (symlinks are not followed)" % out_dir)
        if st.st_uid != os.geteuid():
            raise KeyToolError("%s is not owned by the current user" % out_dir)
        entries = os.listdir(out_dir)
        if entries and stat.S_IMODE(st.st_mode) & 0o077:
            raise KeyToolError("%s is not empty and not private (mode %o): chmod 700 it yourself or pick another directory" % (out_dir, stat.S_IMODE(st.st_mode)))
        for n in names:
            try:
                est = os.lstat(os.path.join(out_dir, n))
            except FileNotFoundError:
                continue
            if not stat.S_ISREG(est.st_mode):
                raise KeyToolError("%s is not a regular file: refusing even with --force" % os.path.join(out_dir, n))
            if not force:
                raise KeyToolError("refusing to overwrite existing key files (use --force): %s" % ", ".join(
                    n for n in names if os.path.lexists(os.path.join(out_dir, n))))
        stage = tempfile.mkdtemp(prefix=".stage-", dir=out_dir)
    else:
        parent = os.path.dirname(out_dir)
        os.makedirs(parent, exist_ok=True)
        stage = tempfile.mkdtemp(prefix=".stage-", dir=parent)
    try:
        seed, kek, dpriv = rng(32), rng(32), rng(32)
        cpub, dpub = ed25519_public(seed), x25519_public(dpriv)
        files = {
            "content-manifest.ed25519.seed": seed, "content-manifest.ed25519.pub": cpub,
            "content-manifest.json": _sidecar("content-manifest-signing", "ed25519", key_id(cpub), cpub,
                                              ["content-manifest.ed25519.seed", "content-manifest.ed25519.pub"]).encode(),
            "kek.aes256": kek,
            "kek.json": _sidecar("content-key-vault-kek", "aes-256", kek_id(kek), None, ["kek.aes256"]).encode(),
            "device.x25519.priv": dpriv, "device.x25519.pub": dpub,
            "device.json": _sidecar("device-key-pair", "x25519", key_id(dpub), dpub, ["device.x25519.priv", "device.x25519.pub"]).encode(),
            "DEV-ONLY.txt": DEV_ONLY_TXT.encode(),
        }
        for n, data in files.items():
            _write_private(os.path.join(stage, n), data)
        if st is None:
            os.rename(stage, out_dir)  # atomic; the staging directory was created 0700 by mkdtemp
        else:
            for n in files:
                os.replace(os.path.join(stage, n), os.path.join(out_dir, n))
            os.rmdir(stage)
    except BaseException:
        shutil.rmtree(stage, ignore_errors=True)
        raise
    return out_dir


def read_public(dir_, name):
    with open(os.path.join(dir_, name), "rb") as f:
        b = f.read()
    if len(b) != 32:
        raise KeyToolError("%s is not 32 bytes" % name)
    return b


def trust_entry(dir_):
    pub = read_public(dir_, "content-manifest.ed25519.pub")
    return "%s content-manifest %s %s" % (ROOT, key_id(pub).hex(), pub.hex())


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("generate")
    g.add_argument("--out", default=None)
    g.add_argument("--force", action="store_true")
    s = sub.add_parser("show")
    s.add_argument("--dir", default=None)
    te = sub.add_parser("trust-entry")
    te.add_argument("--dir", default=None)
    sg = sub.add_parser("sign")
    sg.add_argument("--dir", default=None)
    sg.add_argument("file")
    sub.add_parser("default-dir")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "default-dir":
            print(default_dir())
        elif a.cmd == "generate":
            if a.force:
                print("warning: --force replaces the dev keys; dev vault entries wrapped under the old KEK become unreadable", file=sys.stderr)
            out = generate(a.out or default_dir(), a.force)
            print("dev keys written to %s (root=dev, DEV-ONLY)" % out)
            print(trust_entry(out))
        elif a.cmd == "show":
            d = a.dir or default_dir()
            cpub, dpub = read_public(d, "content-manifest.ed25519.pub"), read_public(d, "device.x25519.pub")
            print("root=dev (DEV-ONLY)")
            print("content-manifest ed25519 key_id=%s public=%s" % (key_id(cpub).hex(), cpub.hex()))
            print("device x25519 key_id=%s public=%s" % (key_id(dpub).hex(), dpub.hex()))
        elif a.cmd == "sign":
            with open(os.path.join(a.dir or default_dir(), "content-manifest.ed25519.seed"), "rb") as f:
                seed = f.read()
            with open(a.file, "rb") as f:
                print(ed25519_sign(seed, f.read()).hex())
        elif a.cmd == "trust-entry":
            print(trust_entry(a.dir or default_dir()))
    except (KeyToolError, OSError) as e:
        print("velora_keys: error: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
