#!/usr/bin/env python3
"""Story 6.3 / 6.8: the vendor-side libraries are offline or server-side code and must never be linked into anything else.

Three libraries are guarded: `velora_content_pack` (tools/content-pack: cipher, signing and decrypting code), `velora_registry` (tools/registry: the device registry
and key vault server, Story 6.8) and `velora_vault_crypto` (tools/common/vault_crypto: vault_entry_v1 / wrapped_key_v1, shared by the two). Each may be named inside
target_link_libraries / add_dependencies / target_sources / add_library(... $<TARGET_OBJECTS>) / install only by the directories listed in ALLOWED below; the
content-pack tool and the registry may not name each other's library (they share only velora_vault_crypto), and no other directory may name any of them.
The executables `velora-content-pack` / `velora-registry` may be referenced (the ctests that scan them, the subdirectories). --self-test proves the scanner can fail.
"""
import argparse
import os
import re
import sys
import tempfile

CMD = re.compile(r"\b(target_link_libraries|add_dependencies|target_sources|link_libraries|add_library|install)\s*\(([^)]*)\)", re.S | re.I)
GUARDED = ("velora_content_pack", "velora_registry", "velora_vault_crypto")
NAMES = {g: re.compile(r"(?<![\w-])" + g + r"(?![\w-])") for g in GUARDED}
# directory -> the guarded libraries it may name (itself and what it builds on). Anything not listed may name none of them.
ALLOWED = {
    "tools/content-pack": {"velora_content_pack", "velora_vault_crypto"},
    "tools/registry": {"velora_registry", "velora_vault_crypto"},
    "tools/common/vault_crypto": {"velora_vault_crypto"},
}


def allowed_for(rel):
    for d, names in ALLOWED.items():
        if rel == d or rel.startswith(d + "/"):
            return names
    return set()


def scan(root):
    errors = []
    for base, dirs, files in os.walk(root):
        rel = os.path.relpath(base, root).replace(os.sep, "/")
        dirs[:] = [d for d in dirs if d not in (".git", "build", "__pycache__", "third_party") and not d.startswith("build")]
        allow = allowed_for(rel)
        for f in files:
            if f != "CMakeLists.txt" and not f.endswith(".cmake"):
                continue
            path = os.path.join(base, f)
            with open(path, encoding="utf-8", errors="replace") as fh:
                text = fh.read()
            for m in CMD.finditer(text):
                for g in GUARDED:
                    if g not in allow and NAMES[g].search(m.group(2)):
                        errors.append("%s: %s(...) references %s" % (os.path.relpath(path, root), m.group(1), g))
    return errors


def self_test():
    def write(d, rel, text):
        os.makedirs(os.path.dirname(os.path.join(d, rel)), exist_ok=True)
        with open(os.path.join(d, rel), "w") as f:
            f.write(text)

    with tempfile.TemporaryDirectory() as d:
        write(d, "tools/content-pack/CMakeLists.txt", "target_link_libraries(velora-content-pack PRIVATE velora_content_pack)\ntarget_link_libraries(velora_content_pack PUBLIC velora_vault_crypto)\n")
        write(d, "tools/registry/CMakeLists.txt", "target_link_libraries(velora-registry PRIVATE velora_registry)\ntarget_link_libraries(velora_registry PUBLIC velora_vault_crypto)\n")
        write(d, "tools/common/vault_crypto/CMakeLists.txt", "add_library(velora_vault_crypto STATIC crypto.cpp)\n")
        write(d, "CMakeLists.txt", "add_subdirectory(tools/content-pack)\nadd_test(NAME x COMMAND python3 s.py --binary $<TARGET_FILE:velora-content-pack> $<TARGET_FILE:velora-registry>)\n")
        assert scan(d) == [], "the tools' own files, the shared library and a ctest on the executables are fine: %r" % scan(d)
        write(d, "app/CMakeLists.txt", "target_link_libraries(engine PRIVATE\n    velora_format velora_content_pack)\n")
        assert len(scan(d)) == 1, "a shipped target linking the content-pack library must be flagged"
        write(d, "app/CMakeLists.txt", "target_link_libraries(engine PRIVATE velora_registry)\n")
        assert len(scan(d)) == 1, "a shipped target linking the registry library must be flagged"
        write(d, "app/CMakeLists.txt", "target_link_libraries(engine PRIVATE velora_vault_crypto)\n")
        assert len(scan(d)) == 1, "a shipped target linking the shared vault crypto must be flagged"
        write(d, "app/CMakeLists.txt", "add_dependencies(engine velora_registry)\ntarget_sources(engine PRIVATE $<TARGET_OBJECTS:velora_vault_crypto>)\n")
        assert len(scan(d)) == 2, "add_dependencies and target_sources count too"
        os.remove(os.path.join(d, "app/CMakeLists.txt"))
        write(d, "tools/registry/CMakeLists.txt", "target_link_libraries(velora_registry PUBLIC velora_content_pack)\n")
        assert len(scan(d)) == 1, "the registry must not link the content-pack library"
        write(d, "tools/registry/CMakeLists.txt", "target_link_libraries(velora_registry PUBLIC velora_vault_crypto)\n")
        write(d, "tools/content-pack/CMakeLists.txt", "target_link_libraries(velora_content_pack PUBLIC velora_registry)\n")
        assert len(scan(d)) == 1, "the content-pack tool must not link the registry library"
        write(d, "tools/content-pack/CMakeLists.txt", "target_link_libraries(velora_content_pack PUBLIC velora_vault_crypto)\n")
        write(d, "tools/common/vault_crypto/CMakeLists.txt", "target_link_libraries(velora_vault_crypto PUBLIC velora_registry)\n")
        assert len(scan(d)) == 1, "the shared library must not link either tool"
        write(d, "tools/common/vault_crypto/CMakeLists.txt", "add_library(velora_vault_crypto STATIC crypto.cpp)\n")
        assert scan(d) == []
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
