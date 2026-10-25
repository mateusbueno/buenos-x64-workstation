#!/usr/bin/env python3
"""Adds a release's persisted files to the compatibility corpus (Story 8.5, FR49, AR29).

  tools/compat/add_release.py <release-tag> <file> [<file> ...] [--root DIR] [--gate PATH] [--gate-config PATH]
  tools/compat/add_release.py --self-test --gate PATH --gate-config PATH

The release job runs it on the files the release's writers produce (a Live Set, a setlist library, the default ASSIGN map, the sysd config), taken from
the release build's fixtures or from a reference device. Files land in tests/compat/releases/<tag>/<kind>/ and are never overwritten with different
bytes. Every later build must keep opening them: `compat_corpus_test` (engine/host) and `compat_config_test` (ui) walk tests/compat recursively and fail
the build, naming the file, when one does not open. --gate / --gate-config run those binaries on the result.

Kinds by extension: .liveset -> live_sets, .library and .pb -> setlists, .json -> config.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

KINDS = {".liveset": "live_sets", ".library": "setlists", ".pb": "setlists", ".json": "config"}
TAG_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")


def default_root():
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tests", "compat")


def candidates(tag, files):
    if not TAG_RE.match(tag):
        raise SystemExit("bad release tag: %r" % tag)
    out = []
    for f in files:
        ext = os.path.splitext(f)[1].lower()
        kind = KINDS.get(ext)
        if kind is None:
            raise SystemExit("no reader for %s (extensions: %s)" % (f, ", ".join(sorted(KINDS))))
        if not os.path.isfile(f) or os.path.islink(f):
            raise SystemExit("not a regular file: %s" % f)
        out.append((f, os.path.join("releases", tag, kind, os.path.basename(f))))
    return out


def same_bytes(a, b):
    with open(a, "rb") as x, open(b, "rb") as y:
        return x.read() == y.read()


def run_gate(gate, root):
    """Runs one gate binary on `root`; a missing / non-executable binary is a clear failure, not a traceback."""
    try:
        return subprocess.run([gate, root], capture_output=True, text=True)
    except OSError as e:
        raise SystemExit("cannot run the gate %s: %s" % (gate, e))


def run_gates(root, gates):
    ok = True
    for g in gates:
        if not g:
            continue
        p = run_gate(g, root)
        sys.stdout.write(p.stdout)
        sys.stderr.write(p.stderr)
        ok = ok and p.returncode == 0
    return ok


def add(root, tag, files, gates):
    """Stages the candidates in a temp COPY of the corpus, runs the gates there, and only then installs them (each by temp + rename). Returns the added
    paths, or None when a gate failed (nothing in `root` was touched)."""
    cands = candidates(tag, files)
    with tempfile.TemporaryDirectory() as tmp:
        stage = os.path.join(tmp, "compat")
        shutil.copytree(root, stage)
        new = []
        for src, rel in cands:
            dest = os.path.join(stage, rel)
            real = os.path.join(root, rel)
            if os.path.exists(real):
                if not same_bytes(real, src):
                    raise SystemExit("refusing to overwrite %s with different bytes (a released corpus file is immutable)" % real)
                continue
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            shutil.copyfile(src, dest)
            new.append((src, rel))
        if not run_gates(stage, gates):
            print("a gate failed: nothing was added to %s" % root)
            return None
    added = []
    try:
        for src, rel in new:
            real = os.path.join(root, rel)
            os.makedirs(os.path.dirname(real), exist_ok=True)
            part = real + ".part"
            shutil.copyfile(src, part)
            os.replace(part, real)
            added.append(real)
    except OSError as e:
        for r in added:  # roll back what was installed
            try:
                os.remove(r)
            except OSError:
                pass
        raise SystemExit("install failed (%s); rolled back" % e)
    return added


def expect_fail_naming(gate, root, name, what):
    p = run_gate(gate, root)
    if p.returncode == 0 or name not in p.stdout:
        print("self-test: the gate did not fail on, or did not name, %s (%s):\n%s" % (name, what, p.stdout))
        return False
    return True


def self_test(gate, gate_config):
    """The gates are not vacuous: a corpus with one unreadable or unclaimed file must fail them, naming the file."""
    root = default_root()
    with tempfile.TemporaryDirectory() as tmp:
        c = os.path.join(tmp, "compat")
        shutil.copytree(root, c)
        for g in (gate, gate_config):
            if run_gate(g, c).returncode != 0:
                print("self-test: the pristine corpus must pass %s" % g)
                return 1
        rel = os.path.join(c, "releases", "selftest")
        plants = [
            (gate, os.path.join(rel, "live_sets", "broken.liveset"), b"\xff\xff not a live set"),
            (gate, os.path.join(rel, "setlists", "broken.library"), b"\xff\xff not a library"),
            (gate, os.path.join(rel, "setlists", "broken.pb"), b"\xff\xff not an assign map"),
            (gate, os.path.join(rel, "mystery.bin"), b"x"),
            (gate_config, os.path.join(rel, "config", "broken.json"), b"{ not json"),
        ]
        for g, path, data in plants:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
            ok = expect_fail_naming(g, c, os.path.basename(path), path)
            os.remove(path)
            if not ok:
                return 1
    print("compat gate self-test: ok")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tag", nargs="?")
    ap.add_argument("files", nargs="*")
    ap.add_argument("--root", default=default_root())
    ap.add_argument("--gate", help="path of the compat_corpus_test binary")
    ap.add_argument("--gate-config", help="path of the compat_config_test binary")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        if not a.gate or not a.gate_config:
            raise SystemExit("--self-test needs --gate and --gate-config")
        return self_test(a.gate, a.gate_config)
    if not a.tag or not a.files:
        ap.error("a release tag and at least one file are required")
    added = add(a.root, a.tag, a.files, [a.gate, a.gate_config])
    if added is None:
        return 1
    for d in added:
        print("added", d)
    return 0


if __name__ == "__main__":
    sys.exit(main())
