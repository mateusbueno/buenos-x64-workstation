#!/usr/bin/env python3
"""Third-party license gate (Story 11.12).

Fails when a component in tools/ci/third_party_manifest.json has a copyleft or
unknown license, its notice is missing from THIRD_PARTY_NOTICES.md, or a CMake
file outside tools/ and tests/ references sfizz (sfizz is a test-only oracle) other than the
vendored BSD-2 parser (target velora_sfizz_parser, directory formatlib/third_party/sfizz).

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
# The vendored parser (Story 1.13, closure in 11-12) is the only sfizz code that may ship: its CMake
# target name anywhere, and path-like references (sfizz/...) inside its own build file only.
SFIZZ_ALLOWED = re.compile(r"velora_sfizz_parser|velora::sfizz_parser")
VENDOR_BUILD_FILE = os.path.join("formatlib", "third_party", "CMakeLists.txt")
SFIZZ_PATH_TOKEN = re.compile(r"\S*/\S*sfizz\S*|\S*sfizz\S*/\S*")


CLOSURE_DOC = os.path.join("_bmad-output", "business", "11-12-sfizz-license-check.md")
CLOSURE_ROW = re.compile(r"^\|\s*`src/sfizz/([^`]+)`\s*\|", re.M)
VENDORED_SFIZZ = os.path.join("formatlib", "third_party", "sfizz")
VENDORED_EXTRAS = {"LICENSE", "AUTHORS.md"}  # upstream license files kept next to the sources


def check_vendored_sfizz(root):
    """Every file under formatlib/third_party/sfizz must be in the 11-12 closure table."""
    base = os.path.join(root, VENDORED_SFIZZ)
    if not os.path.isdir(base):
        return []
    doc = os.path.join(root, CLOSURE_DOC)
    if not os.path.exists(doc):
        return [f"{CLOSURE_DOC} missing: cannot check the vendored sfizz closure"]
    with open(doc) as fh:
        listed = set(CLOSURE_ROW.findall(fh.read()))
    errors = []
    for d, _, names in os.walk(base):
        for n in names:
            rel = os.path.relpath(os.path.join(d, n), base).replace(os.sep, "/")
            if rel in VENDORED_EXTRAS:
                continue
            if not rel.startswith("sfizz/") or rel[len("sfizz/"):] not in listed:
                errors.append(f"{VENDORED_SFIZZ}/{rel}: not in the 11-12 closure table")
    return errors


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
    errors += check_vendored_sfizz(root)
    for c in manifest["components"]:
        if "vendored_dir" in c and not os.path.isdir(os.path.join(root, c["vendored_dir"])):
            errors.append(f"{c['name']}: vendored_dir {c['vendored_dir']} does not exist")
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
                    code = SFIZZ_ALLOWED.sub("", line.split("#", 1)[0])
                    if os.path.relpath(p, root) == VENDOR_BUILD_FILE:
                        code = SFIZZ_PATH_TOKEN.sub("", code)
                    if SFIZZ_REF.search(code):
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
    assert run("MIT", "## X\n", "target_link_libraries(a sfizz_parser)\n")  # upstream target name
    assert not run("MIT", "## X\n", "target_link_libraries(a velora_sfizz_parser)\n")
    assert run("MIT", "## X\n", "add_library(a sfizz/sfizz/Opcode.cpp)\n")  # paths only in the vendor file
    assert run("MIT", "## X\n", "target_link_libraries(a velora_sfizz_parser sfizz::renderer)\n")
    assert not run("MIT", "## X\n", "project(a) # sfizz is test-only\n")
    with tempfile.TemporaryDirectory() as r:  # the vendor build file may name its own sources
        os.makedirs(os.path.join(r, "tools/ci"))
        os.makedirs(os.path.join(r, "formatlib/third_party"))
        json.dump({"components": []}, open(os.path.join(r, "tools/ci/third_party_manifest.json"), "w"))
        open(os.path.join(r, "THIRD_PARTY_NOTICES.md"), "w").write("x")
        vend = os.path.join(r, VENDOR_BUILD_FILE)
        open(vend, "w").write("add_library(velora_sfizz_parser sfizz/sfizz/Opcode.cpp)\n"
                              "target_include_directories(velora_sfizz_parser PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/sfizz/sfizz)\n")
        assert not check(r)
        open(vend, "a").write("target_link_libraries(velora_sfizz_parser PUBLIC sfizz::sfizz)\n")
        assert check(r)
    with tempfile.TemporaryDirectory() as r:  # the closure table gates the vendored sfizz files
        os.makedirs(os.path.join(r, "tools/ci"))
        os.makedirs(os.path.join(r, "_bmad-output/business"))
        os.makedirs(os.path.join(r, VENDORED_SFIZZ, "sfizz"))
        json.dump({"components": []}, open(os.path.join(r, "tools/ci/third_party_manifest.json"), "w"))
        open(os.path.join(r, "THIRD_PARTY_NOTICES.md"), "w").write("x")
        open(os.path.join(r, CLOSURE_DOC), "w").write("| `src/sfizz/Opcode.h` | 1.2.3 |\n")
        open(os.path.join(r, VENDORED_SFIZZ, "LICENSE"), "w").write("l")
        open(os.path.join(r, VENDORED_SFIZZ, "sfizz", "Opcode.h"), "w").write("x")
        assert not check(r)
        open(os.path.join(r, VENDORED_SFIZZ, "sfizz", "Synth.cpp"), "w").write("x")
        assert check(r)
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
