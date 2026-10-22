#!/usr/bin/env python3
"""Lint check for hard-coded user-facing strings in UI code.

Story 2.17: 'every user-facing string is a locale-catalog key (a lint fails the build on hard-coded strings)'
Checks that user-facing text assignment in ui/ uses the locale catalog (e.g. tr("..."))
and does not pass raw string literals to label setters, and that catalog keys used in code exist in locale/en.json.

Usage: check_ui_strings.py [--root DIR] | --self-test
"""
import argparse
import json
import os
import re
import sys
import tempfile

LABEL_SETTER_RE = re.compile(r'\blv_label_set_text\s*\(\s*[^,]+,\s*"([^"]*)"\s*\)')
TR_KEY_RE = re.compile(r'\btr\s*\(\s*"([^"]+)"\s*\)')


def load_en_catalog(root):
    path = os.path.join(root, "locale", "en.json")
    if not os.path.exists(path):
        return None
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def check_file(path, rel_path, en_keys):
    violations = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.readlines()

    for line_idx, line in enumerate(lines, 1):
        # Strip comments
        code = line.split("//")[0]

        # 1. Check for raw literal passed to lv_label_set_text(..., "literal")
        # Allowed if literal is purely punctuation/placeholders like "", " ", "-", "%s"
        for m in LABEL_SETTER_RE.finditer(code):
            literal = m.group(1).strip()
            if literal and not all(c in " -_.:/%\n" for c in literal):
                violations.append(
                    f"{rel_path}:{line_idx}: hard-coded user-facing string literal \"{literal}\" passed to lv_label_set_text"
                )

        # 2. Check that tr("key") keys exist in en.json if catalog loaded
        if en_keys is not None:
            for m in TR_KEY_RE.finditer(code):
                key = m.group(1)
                # Formatted keys like "%s" or dynamic keys are handled, but static keys must exist
                if "%" not in key and key not in en_keys:
                    violations.append(
                        f"{rel_path}:{line_idx}: catalog key \"{key}\" not found in locale/en.json"
                    )

    return violations


def scan_ui(root):
    ui_dir = os.path.join(root, "ui")
    if not os.path.isdir(ui_dir):
        return []

    en_catalog = load_en_catalog(root)
    violations = []

    for dirpath, dirnames, filenames in os.walk(ui_dir):
        # Skip tests, build, third_party
        if any(skip in dirpath for skip in ["tests", "build", "third_party", "_deps"]):
            continue
        for fn in filenames:
            if fn.endswith((".cpp", ".hpp", ".c", ".h")):
                p = os.path.join(dirpath, fn)
                rel = os.path.relpath(p, root)
                violations.extend(check_file(p, rel, en_catalog))

    return violations


def self_test():
    with tempfile.TemporaryDirectory() as tmp:
        loc_dir = os.path.join(tmp, "locale")
        os.makedirs(loc_dir)
        en_json = os.path.join(loc_dir, "en.json")
        with open(en_json, "w") as f:
            json.dump({"test.key": "Hello"}, f)

        ui_src = os.path.join(tmp, "ui", "src")
        os.makedirs(ui_src)

        # Good file
        good_file = os.path.join(ui_src, "good.cpp")
        with open(good_file, "w") as f:
            f.write('#include "locale.hpp"\nlv_label_set_text(lbl, tr("test.key").c_str());\n')

        # Bad file 1: hard-coded literal
        bad1_file = os.path.join(ui_src, "bad1.cpp")
        with open(bad1_file, "w") as f:
            f.write('lv_label_set_text(lbl, "Hardcoded text");\n')

        # Bad file 2: unknown catalog key
        bad2_file = os.path.join(ui_src, "bad2.cpp")
        with open(bad2_file, "w") as f:
            f.write('lv_label_set_text(lbl, tr("missing.key").c_str());\n')

        v_good = check_file(good_file, "ui/src/good.cpp", {"test.key": "Hello"})
        assert len(v_good) == 0, f"Expected 0 violations, got {v_good}"

        v_bad1 = check_file(bad1_file, "ui/src/bad1.cpp", {"test.key": "Hello"})
        assert any("hard-coded" in v for v in v_bad1), f"Expected hardcoded violation, got {v_bad1}"

        v_bad2 = check_file(bad2_file, "ui/src/bad2.cpp", {"test.key": "Hello"})
        assert any("missing.key" in v for v in v_bad2), f"Expected missing key violation, got {v_bad2}"

    print("self-test ok")
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=os.getcwd())
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    violations = scan_ui(args.root)
    if violations:
        print("UI strings lint check FAILED:", file=sys.stderr)
        for v in violations:
            print("  " + v, file=sys.stderr)
        return 1

    print("UI strings lint check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
