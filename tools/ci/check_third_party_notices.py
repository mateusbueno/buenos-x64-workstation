#!/usr/bin/env python3
"""Third-party license gate (Story 11.12).

Fails when:
- a component in tools/ci/third_party_manifest.json has a license outside the
  allow-list, or its notice heading/version is missing from THIRD_PARTY_NOTICES.md;
- the manifest's sfizz file list and the license table in
  _bmad-output/business/11-12-sfizz-license-check.md disagree, or a vendored
  directory (manifest "vendor_dirs") holds a file not on that list;
- a shipped build file (outside top-level tools/, build/ and any tests/) links,
  fetches or depends on sfizz other than its parser (sfizz_parser, sfizz::parser).
  sfizz is a test-only oracle; the vendored parser is the only shipped part.

Usage: check_third_party_notices.py [--root DIR] | --self-test
"""
import argparse
import json
import os
import re
import sys
import tempfile

ALLOWED = {"BSD-2-Clause", "BSD-3-Clause", "MIT", "Apache-2.0", "ISC", "Zlib", "CC0-1.0"}
SKIP_TOP = {".git", "_bmad", "_bmad-output", "tools", "build"}
SKIP_ANY = {"tests"}
CMAKE_FILES = re.compile(r"^(CMakeLists\.txt|.*\.cmake|.*\.cmake\.in)$")
OTHER_FILES = re.compile(r"^(CMakePresets\.json|\.gitmodules|Dockerfile.*|.*\.mk|meson\.build|meson_options\.txt)$")
SFIZZ = re.compile(r"sfizz", re.I)
PARSER_OK = re.compile(r"sfizz[_:]+parser|sfizz/parser", re.I)
CMAKE_CMD = re.compile(r"(\w+)\s*\(([^()]*)\)", re.S)
DEP_CMDS = {"target_link_libraries", "add_dependencies", "find_package", "find_library", "pkg_check_modules",
            "fetchcontent_declare", "fetchcontent_makeavailable", "externalproject_add", "add_subdirectory",
            "link_libraries", "target_link_options"}


def read(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def strip_cmake_comments(text):
    return "\n".join(line.split("#", 1)[0] for line in text.splitlines())


def mentions_sfizz_oracle(text):
    return bool(SFIZZ.search(PARSER_OK.sub("", text)))


def scan_build_file(path, name):
    text = read(path)
    out = []
    if CMAKE_FILES.match(name):
        for m in CMAKE_CMD.finditer(strip_cmake_comments(text)):
            if m.group(1).lower() in DEP_CMDS and mentions_sfizz_oracle(m.group(2)):
                out.append(m.group(1))
    else:
        out = [str(i) for i, line in enumerate(text.splitlines(), 1)
               if not line.lstrip().startswith("#") and mentions_sfizz_oracle(line)]
    return out


def check(root):
    errors = []
    mpath = os.path.join(root, "tools/ci/third_party_manifest.json")
    if not os.path.exists(mpath):
        return ["tools/ci/third_party_manifest.json missing"]
    manifest = json.loads(read(mpath))
    comps = manifest.get("components") or []
    if not comps:
        errors.append("manifest has no components")
    npath = os.path.join(root, "THIRD_PARTY_NOTICES.md")
    notices = read(npath) if os.path.exists(npath) else ""
    if not notices:
        errors.append("THIRD_PARTY_NOTICES.md missing or empty")
    for c in comps:
        name = c.get("name", "?")
        if c.get("license") not in ALLOWED:
            errors.append(f"{name}: license {c.get('license')} not in allow-list")
        marker = c.get("notice_marker")
        if not marker:
            errors.append(f"{name}: notice_marker missing")
            continue
        heads = [ln for ln in notices.splitlines() if ln.startswith(marker)]
        if not heads:
            errors.append(f"{name}: notice heading '{marker}' missing from THIRD_PARTY_NOTICES.md")
        elif c.get("version") and not any(c["version"] in h for h in heads):
            errors.append(f"{name}: notice heading does not carry pinned version {c['version']}")
    files = set(manifest.get("sfizz_files", []))
    dpath = os.path.join(root, "_bmad-output/business/11-12-sfizz-license-check.md")
    if files and os.path.exists(dpath):
        table = set(re.findall(r"^\| `src/sfizz/([^`]+)` \|", read(dpath), re.M))
        if table != files:
            errors.append(f"sfizz file list differs from license table: {sorted(table ^ files)}")
    for vd in manifest.get("vendor_dirs", []):
        base = os.path.join(root, vd)
        for d, _, names in os.walk(base):
            for n in names:
                rel = os.path.relpath(os.path.join(d, n), base).replace(os.sep, "/")
                if rel not in files and not n.startswith(("LICENSE", "README", "CMakeLists")):
                    errors.append(f"{vd}/{rel}: vendored file not in the license table")
    for d, dirs, names in os.walk(root):
        top = os.path.abspath(d) == os.path.abspath(root)
        dirs[:] = [x for x in dirs if x not in SKIP_ANY and not (top and x in SKIP_TOP)]
        for n in names:
            if CMAKE_FILES.match(n) or OTHER_FILES.match(n):
                for hit in scan_build_file(os.path.join(d, n), n):
                    errors.append(f"{os.path.relpath(os.path.join(d, n), root)}: sfizz (non-parser) in shipped build file [{hit}]")
    return errors


def self_test():
    def run(license_="MIT", notice="## X 1\n", files=None, version="1"):
        with tempfile.TemporaryDirectory() as r:
            os.makedirs(os.path.join(r, "tools/ci"))
            m = {"components": [{"name": "x", "version": version, "license": license_, "notice_marker": "## X"}]}
            with open(os.path.join(r, "tools/ci/third_party_manifest.json"), "w") as f:
                json.dump(m, f)
            with open(os.path.join(r, "THIRD_PARTY_NOTICES.md"), "w") as f:
                f.write(notice)
            for rel, body in (files or {}).items():
                p = os.path.join(r, rel)
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, "w") as f:
                    f.write(body)
            return check(r)

    def expect(cond, msg):
        if not cond:
            raise SystemExit("self-test failed: " + msg)

    link = "target_link_libraries(a sfizz::sfizz)\n"
    expect(not run(), "clean tree")
    expect(run(license_="GPL-3.0-only"), "copyleft")
    expect(run(notice="nothing"), "missing notice")
    expect(run(version="2"), "version drift")
    expect(run(files={"CMakeLists.txt": link}), "root link")
    expect(run(files={"CMakeLists.txt": "target_link_libraries(a\n  sfizz_static)\n"}), "multi-line link")
    expect(not run(files={"CMakeLists.txt": "target_link_libraries(a sfizz_parser)\n"}), "parser allowed")
    expect(not run(files={"CMakeLists.txt": "target_link_libraries(a sfizz::parser)\n"}), "parser alias allowed")
    expect(not run(files={"CMakeLists.txt": "# sfizz oracle\nproject(a) # sfizz\n"}), "comments")
    expect(not run(files={"formatlib/CMakeLists.txt": "add_library(f sfizz_vendored_note.cpp)\n"}), "non-dep command")
    expect(not run(files={"engine/tests/CMakeLists.txt": link}), "nested tests exempt")
    expect(run(files={"src/x/CMakeLists.txt": link}), "nested src flagged")
    expect(run(files={"src/tools/CMakeLists.txt": link}), "nested tools flagged")
    expect(run(files={"cmake/oracle.cmake": link}), ".cmake flagged")
    expect(run(files={".gitmodules": "[submodule \"sfizz\"]\n\tpath = x/sfizz\n"}), ".gitmodules flagged")
    expect(not run(files={"tools/CMakeLists.txt": link}), "top-level tools exempt")
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
