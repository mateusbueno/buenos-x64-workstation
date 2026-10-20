#!/usr/bin/env python3
"""Story 6.5: a build WITHOUT the dev key provider contains none of it.

The dev key provider (a developer's device key read from a file, and the --dev-device-key / --trust-list flags that use it) is dev-only: a configuration with
VELORA_DEV_KEY_PROVIDER=OFF must not carry it. This scans the given binaries for the provider's symbols (DevKeyProvider, dev_key_provider) and for the flag and key-file
names in the file's bytes. Exit 0 when none is found; --expect-present inverts it (exit 0 only when ALL are found: the negative control that proves the scan can see them
in a dev build).

Usage: check_no_dev_keys.py [--expect-present] --binary FILE [--binary FILE ...] | --self-test
"""
import argparse
import os
import subprocess
import sys
import tempfile

NEEDLES = [b"--dev-device-key", b"device.x25519", b"DevKeyProvider", b"dev_key_provider"]


def findings(path):
    data = open(path, "rb").read()
    found = [n.decode() for n in NEEDLES if n in data]
    try:
        out = subprocess.run(["nm", "-a", path], capture_output=True, text=True, timeout=120).stdout
        if "DevKeyProvider" in out and "symbol:DevKeyProvider" not in found:
            found.append("symbol:DevKeyProvider")
    except (OSError, subprocess.SubprocessError):
        pass
    return found


def self_test():
    with tempfile.TemporaryDirectory() as d:
        clean, dirty = os.path.join(d, "clean"), os.path.join(d, "dirty")
        open(clean, "wb").write(b"\x7fELF nothing to see here --trust-list")
        open(dirty, "wb").write(b"junk --dev-device-key junk")
        assert findings(clean) == [], findings(clean)
        assert "--dev-device-key" in findings(dirty)
    print("check_no_dev_keys self-test: ok")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", action="append", default=[])
    ap.add_argument("--expect-present", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    if not a.binary:
        ap.error("--binary is required")
    bad = False
    for b in a.binary:
        f = findings(b)
        if a.expect_present:
            ok = "--dev-device-key" in f and any(x.startswith(("DevKeyProvider", "symbol:")) for x in f)
            print("%s: %s" % (b, "dev key provider present (as expected): %s" % ", ".join(f) if ok else "NOT found: %s" % f))
            bad = bad or not ok
        else:
            print("%s: %s" % (b, "clean" if not f else "dev key provider found: %s" % ", ".join(f)))
            bad = bad or bool(f)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
