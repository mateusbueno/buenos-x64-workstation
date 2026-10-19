#!/usr/bin/env python3
"""Third-party license gate (Story 11.12).

Fails when a component in tools/ci/third_party_manifest.json has a copyleft or
unknown license, its notice is missing from THIRD_PARTY_NOTICES.md, or a CMake
file outside top-level tools/ and any tests/ references sfizz (sfizz is a test-only oracle) other than the
vendored BSD-2 parser (target velora_sfizz_parser, directory formatlib/third_party/sfizz).

The full sfizz engine is a test oracle only (Story 1.15): tools/oracle/ may hold scripts and docs, never
sfizz sources (it fetches them into the git-ignored tools/oracle/.work), the ignore rule must be in
.gitignore, no .gitmodules entry may name sfizz, and no shipped CMake file may reference tools/oracle.

Usage: check_third_party_notices.py [--root DIR] | --self-test
"""
import argparse
import fnmatch
import json
import os
import re
import subprocess
import sys
import tempfile

ALLOWED = {"BSD-2-Clause", "BSD-3-Clause", "MIT", "Apache-2.0", "ISC", "Zlib", "CC0-1.0"}
SKIP_TOP = {".git", "_bmad", "_bmad-output", "tools", "build"}  # exempt at the repository root only
SKIP_ANY = {"tests"}  # test directories are exempt at any depth
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
    with open(doc, encoding="utf-8") as fh:
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


ORACLE_DIR = os.path.join("tools", "oracle")
ORACLE_WORK = ".work"  # the ignored build directory the oracle scripts clone and build into
ORACLE_WORK_REL = "tools/oracle/.work"
ORACLE_ALLOWED_EXT = {".sh"}           # scripts ...
ORACLE_ALLOWED_NAMES = {"README.md"}   # ... and the README, nothing else
ORACLE_REF = re.compile(r"tools/oracle|sfizz_render", re.I)


def _git(root, *args):
    """(returncode, stdout) of `git -C root ...`, or None when git is unavailable."""
    try:
        r = subprocess.run(["git", "-C", root, *args], capture_output=True, text=True, encoding="utf-8", check=False)
    except (OSError, ValueError):
        return None
    return r.returncode, r.stdout


def _gitignore_ignores(root, rel):
    """True when .gitignore (last matching pattern wins, '!' negates) ignores `rel`. Used when git is absent
    or `root` is not a repository."""
    path = os.path.join(root, ".gitignore")
    if not os.path.exists(path):
        return False
    with open(path, encoding="utf-8") as fh:
        lines = fh.read().splitlines()
    ignored = False
    parts = rel.split("/")
    for line in lines:
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        neg = line.startswith("!")
        pat = line[1:] if neg else line
        anchored = "/" in pat.rstrip("/")
        pat = pat.strip("/")
        if anchored:
            hit = any(fnmatch.fnmatch("/".join(parts[:i]), pat) for i in range(1, len(parts) + 1))
        else:
            hit = any(fnmatch.fnmatch(x, pat) for x in parts)
        if hit:
            ignored = not neg
    return ignored


def check_oracle(root):
    """The sfizz oracle must stay outside the repository's sources."""
    errors = []
    base = os.path.join(root, ORACLE_DIR)
    if os.path.isdir(base):
        probe = ORACLE_WORK_REL + "/probe"
        res = _git(root, "check-ignore", "-q", probe)
        if res is not None and res[0] in (0, 1):
            ignored = res[0] == 0
        else:  # no git, or not a repository
            ignored = _gitignore_ignores(root, probe)
        if not ignored:
            errors.append(f"{ORACLE_WORK_REL} is not git-ignored: the oracle build directory would be committable")
        tracked = _git(root, "ls-files", "--", ORACLE_WORK_REL)
        if tracked is not None and tracked[0] == 0 and tracked[1].strip():
            errors.append(f"{ORACLE_WORK_REL}: files are tracked by git ({tracked[1].split()[0]} ...); the oracle must never be committed")
        for d, dirs, names in os.walk(base):
            dirs[:] = [x for x in dirs if x != ORACLE_WORK]
            for n in names:
                rel = os.path.relpath(os.path.join(d, n), root)
                if os.path.splitext(n)[1].lower() not in ORACLE_ALLOWED_EXT and n not in ORACLE_ALLOWED_NAMES:
                    errors.append(f"{rel}: not allowed in tools/oracle (only *.sh scripts and README.md; "
                                  "sources, archives and patches must not be vendored)")
    gm = os.path.join(root, ".gitmodules")
    if os.path.exists(gm):
        with open(gm, encoding="utf-8", errors="replace") as fh:
            if re.search(r"sfizz", fh.read(), re.I):
                errors.append(".gitmodules: sfizz must not be a submodule (the oracle is fetched into tools/oracle/.work)")
    return errors


def check(root):
    errors = []
    errors += check_oracle(root)
    with open(os.path.join(root, "tools/ci/third_party_manifest.json"), encoding="utf-8") as f:
        manifest = json.load(f)
    notices_path = os.path.join(root, "THIRD_PARTY_NOTICES.md")
    notices = ""
    if os.path.exists(notices_path):
        with open(notices_path, encoding="utf-8") as fh:
            notices = fh.read()
    if not notices:
        errors.append("THIRD_PARTY_NOTICES.md missing or empty")
    errors += check_vendored_sfizz(root)
    for c in manifest["components"]:
        if "vendored_dir" in c and not os.path.isdir(os.path.join(root, c["vendored_dir"])):
            errors.append(f"{c['name']}: vendored_dir {c['vendored_dir']} does not exist")
        if c["license"] not in ALLOWED:
            errors.append(f"{c['name']}: license {c['license']} not in allow-list")
        heads = [ln for ln in notices.splitlines() if ln.startswith(c["notice_marker"])]
        if not heads:
            errors.append(f"{c['name']}: notice '{c['notice_marker']}' missing from THIRD_PARTY_NOTICES.md")
        elif c.get("version") and not any(c["version"] in h for h in heads):
            errors.append(f"{c['name']}: notice heading does not carry pinned version {c['version']}")
    for d, dirs, names in os.walk(root):
        top = os.path.abspath(d) == os.path.abspath(root)
        dirs[:] = [x for x in dirs if x not in SKIP_ANY and not (top and x in SKIP_TOP)]
        for n in names:
            if n == "CMakeLists.txt" or n.endswith(".cmake"):
                p = os.path.join(d, n)
                with open(p, encoding="utf-8", errors="ignore") as fh:
                    lines = fh.readlines()
                for i, line in enumerate(lines, 1):
                    code = SFIZZ_ALLOWED.sub("", line.split("#", 1)[0])
                    if os.path.relpath(p, root) == VENDOR_BUILD_FILE:
                        code = SFIZZ_PATH_TOKEN.sub("", code)
                    if SFIZZ_REF.search(code):
                        errors.append(f"{os.path.relpath(p, root)}:{i}: sfizz referenced in a shipped build file")
                    elif ORACLE_REF.search(code):
                        errors.append(f"{os.path.relpath(p, root)}:{i}: tools/oracle referenced in a shipped build file")
    return errors


def _w(path, text, mode="w"):
    with open(path, mode, encoding="utf-8") as fh:
        fh.write(text)


def self_test():
    def run(license_, notice, cmake):
        with tempfile.TemporaryDirectory() as r:
            os.makedirs(os.path.join(r, "tools/ci"))
            m = {"components": [{"name": "x", "license": license_, "notice_marker": "## X"}]}
            _w(os.path.join(r, "tools/ci/third_party_manifest.json"), json.dumps(m))
            _w(os.path.join(r, "THIRD_PARTY_NOTICES.md"), notice, "w")
            _w(os.path.join(r, "CMakeLists.txt"), cmake, "w")
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
        _w(os.path.join(r, "tools/ci/third_party_manifest.json"), json.dumps({"components": []}))
        _w(os.path.join(r, "THIRD_PARTY_NOTICES.md"), "x", "w")
        vend = os.path.join(r, VENDOR_BUILD_FILE)
        open(vend, "w").write("add_library(velora_sfizz_parser sfizz/sfizz/Opcode.cpp)\n"
                              "target_include_directories(velora_sfizz_parser PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/sfizz/sfizz)\n")
        assert not check(r)
        _w(vend, "target_link_libraries(velora_sfizz_parser PUBLIC sfizz::sfizz)\n", "a")
        assert check(r)
    with tempfile.TemporaryDirectory() as r:  # the closure table gates the vendored sfizz files
        os.makedirs(os.path.join(r, "tools/ci"))
        os.makedirs(os.path.join(r, "_bmad-output/business"))
        os.makedirs(os.path.join(r, VENDORED_SFIZZ, "sfizz"))
        _w(os.path.join(r, "tools/ci/third_party_manifest.json"), json.dumps({"components": []}))
        _w(os.path.join(r, "THIRD_PARTY_NOTICES.md"), "x", "w")
        _w(os.path.join(r, CLOSURE_DOC), "| `src/sfizz/Opcode.h` | 1.2.3 |\n", "w")
        _w(os.path.join(r, VENDORED_SFIZZ, "LICENSE"), "l", "w")
        _w(os.path.join(r, VENDORED_SFIZZ, "sfizz", "Opcode.h"), "x", "w")
        assert not check(r)
        _w(os.path.join(r, VENDORED_SFIZZ, "sfizz", "Synth.cpp"), "x", "w")
        assert check(r)
    with tempfile.TemporaryDirectory() as r:  # the oracle directory: scripts yes, sources/archives no
        os.makedirs(os.path.join(r, "tools/ci"))
        os.makedirs(os.path.join(r, ORACLE_DIR, ORACLE_WORK, "sfizz", "src"))
        _w(os.path.join(r, "tools/ci/third_party_manifest.json"), json.dumps({"components": []}))
        _w(os.path.join(r, "THIRD_PARTY_NOTICES.md"), "x")
        _w(os.path.join(r, ORACLE_DIR, "build-sfizz-render.sh"), "#!/bin/sh\n")
        _w(os.path.join(r, ORACLE_DIR, "README.md"), "x")
        _w(os.path.join(r, ORACLE_DIR, ORACLE_WORK, "sfizz", "src", "Synth.cpp"), "x")  # ignored build dir
        assert any("not git-ignored" in e for e in check(r))  # no ignore rule yet
        for rule in ("/tools/oracle/.work/", "tools/oracle/.work", ".work/", "/tools/oracle/"):
            _w(os.path.join(r, ".gitignore"), "/build*/\n" + rule + "\n")
            assert not check(r), rule
        _w(os.path.join(r, ".gitignore"), "/tools/oracle/.work/\n!/tools/oracle/.work/\n")  # a negation re-includes it
        assert any("not git-ignored" in e for e in check(r))
        _w(os.path.join(r, ".gitignore"), "/tools/oracle/.work/\n")
        assert not check(r)
        for bad in ("Synth.cpp", "sfizz.tar.gz", "fix.patch", "CMakeLists.txt", "notes.txt"):
            _w(os.path.join(r, ORACLE_DIR, bad), "x")
            assert any(bad in e for e in check(r)), bad
            os.remove(os.path.join(r, ORACLE_DIR, bad))
        _w(os.path.join(r, ".gitmodules"), '[submodule "x"]\n\turl = https://github.com/sfztools/sfizz\n')
        assert any("submodule" in e for e in check(r))
        os.remove(os.path.join(r, ".gitmodules"))
        _w(os.path.join(r, "CMakeLists.txt"), "add_subdirectory(tools/oracle)\n")
        assert any("tools/oracle referenced" in e for e in check(r))
    def run_tree(files, notice="## X 1\n", version="1"):  # nested exemptions and version drift
        with tempfile.TemporaryDirectory() as r:
            os.makedirs(os.path.join(r, "tools/ci"))
            m = {"components": [{"name": "x", "version": version, "license": "MIT", "notice_marker": "## X"}]}
            _w(os.path.join(r, "tools/ci/third_party_manifest.json"), json.dumps(m))
            _w(os.path.join(r, "THIRD_PARTY_NOTICES.md"), notice)
            for rel, body in files.items():
                os.makedirs(os.path.dirname(os.path.join(r, rel)), exist_ok=True)
                _w(os.path.join(r, rel), body)
            return check(r)
    link = "target_link_libraries(a sfizz::sfizz)\n"
    if run_tree({"engine/tests/CMakeLists.txt": link}) or run_tree({"tools/CMakeLists.txt": link}):
        raise SystemExit("self-test failed: tests/ anywhere and top-level tools/ must be exempt")
    for rel in ("src/x/CMakeLists.txt", "src/tools/CMakeLists.txt", "cmake/oracle.cmake"):
        if not run_tree({rel: link}):
            raise SystemExit(f"self-test failed: {rel} must be flagged")
    if not run_tree({}, version="2"):
        raise SystemExit("self-test failed: pinned version missing from the notice heading must fail")
    if _git(os.getcwd(), "--version") is not None:  # tracked files under the build directory are rejected
        with tempfile.TemporaryDirectory() as r:
            os.makedirs(os.path.join(r, "tools/ci"))
            os.makedirs(os.path.join(r, ORACLE_DIR, ORACLE_WORK))
            _w(os.path.join(r, "tools/ci/third_party_manifest.json"), json.dumps({"components": []}))
            _w(os.path.join(r, "THIRD_PARTY_NOTICES.md"), "x")
            _w(os.path.join(r, ".gitignore"), "/tools/oracle/.work/\n")
            _w(os.path.join(r, ORACLE_DIR, ORACLE_WORK, "built"), "x")
            if _git(r, "init", "-q") and _git(r, "init", "-q")[0] == 0:
                assert not check(r)
                _git(r, "add", "-f", "tools/oracle/.work/built")
                assert any("tracked" in e for e in check(r))
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
