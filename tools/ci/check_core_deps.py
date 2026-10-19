#!/usr/bin/env python3
"""Dependency-purity check for engine/core.

engine/core may include only core, ports and C++ standard library headers, and
its CMake files may link only core/ports targets. Exits non-zero naming the
file and offending include/target on violation.

Usage: check_core_deps.py [--root DIR] | --self-test
"""
import argparse
import os
import re
import subprocess
import sys

SRC_EXT = {".h", ".hpp", ".hh", ".hxx", ".c", ".cc", ".cpp", ".cxx", ".inl", ".ipp"}
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')

# C++ standard library (plus the C compatibility headers).
STD_HEADERS = set("""
algorithm any array atomic barrier bit bitset charconv chrono codecvt compare complex concepts
condition_variable coroutine deque exception execution expected filesystem flat_map flat_set format
forward_list fstream functional future generator initializer_list iomanip ios iosfwd iostream istream
iterator latch limits list locale map mdspan memory memory_resource mutex new numbers numeric optional
ostream print queue random ranges ratio regex scoped_allocator semaphore set shared_mutex source_location
span spanstream sstream stack stacktrace stdexcept stop_token streambuf string string_view strstream
syncstream system_error thread tuple type_traits typeindex typeinfo unordered_map unordered_set utility
valarray variant vector version
cassert cctype cerrno cfenv cfloat cinttypes climits clocale cmath csetjmp csignal cstdarg cstddef
cstdint cstdio cstdlib cstring ctime cuchar cwchar cwctype
assert.h ctype.h errno.h fenv.h float.h inttypes.h limits.h locale.h math.h setjmp.h signal.h stdarg.h
stdbool.h stddef.h stdint.h stdio.h stdlib.h string.h time.h uchar.h wchar.h wctype.h
""".split())

ALLOWED_LINK = re.compile(r"^(velora_core|velora_core_o2|velora_ports|velora::core|velora::ports)$")
LINK_CMD_RE = re.compile(
    r"(target_link_libraries|target_link_options|link_libraries|add_dependencies)\s*\(([^)]*)\)",
    re.I | re.S)
EXTERNAL_CMD_RE = re.compile(
    r"\b(find_package|find_library|find_path|FetchContent_\w+|pkg_check_modules|ExternalProject_Add)\s*\(",
    re.I)
LINK_KEYWORDS = {"PUBLIC", "PRIVATE", "INTERFACE", "LINK_PUBLIC", "LINK_PRIVATE",
                 "LINK_INTERFACE_LIBRARIES", "debug", "optimized", "general"}


def check_include(core_dir, path, kind, name):
    """Return a reason string if the include is forbidden, else None."""
    if kind == "<":
        if name in STD_HEADERS:
            return None
        if name.startswith("velora/core/") or name.startswith("velora/ports/"):
            return None
        return "non-standard system/third-party header"
    # Quoted include: allow the project's core/ports headers, or files that
    # resolve inside engine/core without escaping it.
    if name.startswith("velora/core/") or name.startswith("velora/ports/"):
        return None
    if name in STD_HEADERS:
        return None
    if os.path.isabs(name):
        return "absolute include path"
    if "host" in name.split("/"):
        return "include reaches into engine/host"
    resolved = os.path.normpath(os.path.join(os.path.dirname(path), name))
    core_abs = os.path.abspath(core_dir)
    if os.path.realpath(resolved).startswith(os.path.realpath(core_abs) + os.sep) \
            and os.path.isfile(resolved):
        return None
    return "include outside engine/core, engine/ports and the standard library"


def check_cmake(path, text):
    problems = []
    stripped = re.sub(r"#[^\n]*", "", text)
    for m in EXTERNAL_CMD_RE.finditer(stripped):
        problems.append((path, m.group(1).lower(), "external dependency discovery"))
    for m in LINK_CMD_RE.finditer(stripped):
        cmd, args = m.group(1).lower(), m.group(2).split()
        if cmd == "target_link_options":
            for it in args[1:]:
                if it not in LINK_KEYWORDS and not it.startswith("-fsanitize="):
                    problems.append((path, cmd, it))
            continue
        # First arg of target_link_libraries/add_dependencies is the target itself.
        items = args[1:] if cmd in ("target_link_libraries", "add_dependencies") else args
        for it in items:
            if it in LINK_KEYWORDS:
                continue
            if not ALLOWED_LINK.match(it):
                problems.append((path, cmd, it))
    return problems


def run_check(root):
    core_dir = os.path.join(root, "engine", "core")
    if not os.path.isdir(core_dir):
        print(f"error: {core_dir} not found", file=sys.stderr)
        return 2
    violations = []
    for dirpath, _, files in os.walk(core_dir):
        for f in files:
            p = os.path.join(dirpath, f)
            rel = os.path.relpath(p, root)
            ext = os.path.splitext(f)[1]
            if ext in SRC_EXT:
                with open(p, encoding="utf-8", errors="replace") as fh:
                    for n, line in enumerate(fh, 1):
                        m = INCLUDE_RE.match(line)
                        if m:
                            why = check_include(core_dir, p, m.group(1), m.group(2))
                            if why:
                                violations.append(f"{rel}:{n}: forbidden include "
                                                  f"{m.group(1)}{m.group(2)}"
                                                  f"{'>' if m.group(1) == '<' else chr(34)} ({why})")
            elif f == "CMakeLists.txt" or ext == ".cmake":
                with open(p, encoding="utf-8", errors="replace") as fh:
                    for _, cmd, it in check_cmake(p, fh.read()):
                        violations.append(f"{rel}: forbidden link dependency '{it}' in {cmd}()")
    if violations:
        print("engine/core dependency purity check FAILED:", file=sys.stderr)
        for v in violations:
            print("  " + v, file=sys.stderr)
        return 1
    print("engine/core dependency purity check passed")
    return 0


def self_test():
    here = os.path.dirname(os.path.abspath(__file__))
    script = os.path.abspath(__file__)
    clean = subprocess.run([sys.executable, script, "--root", os.path.join(here, "..", "..")],
                           capture_output=True, text=True)
    if clean.returncode != 0:
        print("self-test FAILED: clean tree should pass\n" + clean.stderr, file=sys.stderr)
        return 1
    fixture = os.path.join(here, "fixtures", "violation")
    bad = subprocess.run([sys.executable, script, "--root", fixture],
                         capture_output=True, text=True)
    if bad.returncode != 1:
        print(f"self-test FAILED: violating fixture exit {bad.returncode}, expected 1\n{bad.stderr}",
              file=sys.stderr)
        return 1
    for needle in ("alsa/asoundlib.h", "../host/x.h", "pthread", "alsa/quoted.h", "$<"):
        if needle not in bad.stderr:
            print(f"self-test FAILED: violation '{needle}' not reported\n{bad.stderr}",
                  file=sys.stderr)
            return 1
    print("self-test passed: clean tree OK, violating fixture rejected")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    return self_test() if a.self_test else run_check(os.path.abspath(a.root))


if __name__ == "__main__":
    sys.exit(main())
