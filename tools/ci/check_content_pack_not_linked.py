#!/usr/bin/env python3
"""Story 6.3: tools/content-pack is an offline tool and must never be linked into anything else.

Fails when a CMake file outside tools/content-pack names the target `velora_content_pack` (the tool's library, which carries the cipher, signing and
decrypting code) inside target_link_libraries / add_dependencies / target_sources / add_library(... $<TARGET_OBJECTS>), or adds a library named like it.
The executable `velora-content-pack` may be referenced (the ctest that scans it, the subdirectory). --self-test proves the scanner can fail.
"""
import argparse
import os
import re
import sys
import tempfile

CMD = re.compile(r"\b(target_link_libraries|add_dependencies|target_sources|link_libraries|add_library|install)\s*\(([^)]*)\)", re.S | re.I)
NAME = re.compile(r"(?<![\w-])velora_content_pack(?![\w-])")


def scan(root):
    errors = []
    for base, dirs, files in os.walk(root):
        rel = os.path.relpath(base, root).replace(os.sep, "/")
        dirs[:] = [d for d in dirs if d not in (".git", "build", "__pycache__", "third_party") and not d.startswith("build")]
        if rel == "tools/content-pack" or rel.startswith("tools/content-pack/"):
            continue
        for f in files:
            if f != "CMakeLists.txt" and not f.endswith(".cmake"):
                continue
            path = os.path.join(base, f)
            with open(path, encoding="utf-8", errors="replace") as fh:
                text = fh.read()
            for m in CMD.finditer(text):
                if NAME.search(m.group(2)):
                    errors.append("%s: %s(...) references velora_content_pack" % (os.path.relpath(path, root), m.group(1)))
    return errors


def self_test():
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, "app"))
        os.makedirs(os.path.join(d, "tools/content-pack"))
        with open(os.path.join(d, "tools/content-pack/CMakeLists.txt"), "w") as f:
            f.write("target_link_libraries(velora-content-pack PRIVATE velora_content_pack)\n")
        with open(os.path.join(d, "CMakeLists.txt"), "w") as f:
            f.write("add_subdirectory(tools/content-pack)\nadd_test(NAME x COMMAND python3 s.py --binary $<TARGET_FILE:velora-content-pack>)\n")
        assert scan(d) == [], "the tool's own files and a ctest on the executable are fine"
        with open(os.path.join(d, "app/CMakeLists.txt"), "w") as f:
            f.write("target_link_libraries(engine PRIVATE\n    velora_format velora_content_pack)\n")
        assert len(scan(d)) == 1, "a shipped target linking the library must be flagged"
    print("check_content_pack_not_linked self-test: ok")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        self_test()
        return 0
    errs = scan(os.path.abspath(a.root))
    for e in errs:
        print("check_content_pack_not_linked: " + e)
    if not errs:
        print("check_content_pack_not_linked: ok")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
