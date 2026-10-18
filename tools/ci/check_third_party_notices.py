#!/usr/bin/env python3
"""Third-party license gate (Story 11.12).

Fails when a component in tools/ci/third_party_manifest.json has a copyleft or
unknown license, its notice is missing from THIRD_PARTY_NOTICES.md, or a CMake
file outside tools/ and tests/ references sfizz (sfizz is a test-only oracle).

Usage: check_third_party_notices.py [--root DIR] | --self-test
"""
import argparse
import json
import os
import re
import sys
import tempfile

ALLOWED = {"BSD-2-Clause", "BSD-3-Clause", "MIT", "Apache-2.0", "ISC", "Zlib", "CC0-1.0"}
SKIP_DIRS = {".git", "_bmad", "_bmad-output", "tools", "tests", "build"}  # exempt at any depth
SFIZZ_REF = re.compile(r"sfizz", re.I)


def check(root):
    errors = []
    with open(os.path.join(root, "tools/ci/third_party_manifest.json")) as f:
        manifest = json.load(f)
    notices_path = os.path.join(root, "THIRD_PARTY_NOTICES.md")
    notices = ""
    if os.path.exists(notices_path):
        with open(notices_path) as fh:
            notices = fh.read()
    if not notices:
        errors.append("THIRD_PARTY_NOTICES.md missing or empty")
    for c in manifest["components"]:
        if c["license"] not in ALLOWED:
            errors.append(f"{c['name']}: license {c['license']} not in allow-list")
        if c["notice_marker"] not in notices:
            errors.append(f"{c['name']}: notice '{c['notice_marker']}' missing from THIRD_PARTY_NOTICES.md")
    for d, dirs, names in os.walk(root):
        dirs[:] = [x for x in dirs if x not in SKIP_DIRS]
        for n in names:
            if n == "CMakeLists.txt" or n.endswith(".cmake"):
                p = os.path.join(d, n)
                with open(p, errors="ignore") as fh:
                    lines = fh.readlines()
                for i, line in enumerate(lines, 1):
                    if SFIZZ_REF.search(line.split("#", 1)[0]):
                        errors.append(f"{os.path.relpath(p, root)}:{i}: sfizz referenced in a shipped build file")
    return errors


def self_test():
    def run(license_, notice, cmake):
        with tempfile.TemporaryDirectory() as r:
            os.makedirs(os.path.join(r, "tools/ci"))
            m = {"components": [{"name": "x", "license": license_, "notice_marker": "## X"}]}
            json.dump(m, open(os.path.join(r, "tools/ci/third_party_manifest.json"), "w"))
            open(os.path.join(r, "THIRD_PARTY_NOTICES.md"), "w").write(notice)
            open(os.path.join(r, "CMakeLists.txt"), "w").write(cmake)
            return check(r)
    assert not run("MIT", "## X\n", "project(a)\n")
    assert run("GPL-3.0-only", "## X\n", "project(a)\n")
    assert run("MIT", "nothing", "project(a)\n")
    assert run("MIT", "## X\n", "target_link_libraries(a sfizz::sfizz)\n")
    assert not run("MIT", "## X\n", "# sfizz oracle is test-only\n")
    assert run("MIT", "## X\n", "target_link_libraries(a sfizz_parser)\n")
    assert not run("MIT", "## X\n", "project(a) # sfizz is test-only\n")
    print("self-test ok")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.join(os.path.dirname(__file__), "..", ".."))
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        self_test()
        sys.exit(0)
    errs = check(os.path.abspath(a.root))
    for e in errs:
        print("FAIL:", e)
    sys.exit(1 if errs else 0)
