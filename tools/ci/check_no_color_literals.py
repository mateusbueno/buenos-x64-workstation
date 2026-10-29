#!/usr/bin/env python3
"""Lint check that no color literals exist in UI code outside theme files.

Story 2.18 AC:
'no colour literal exists in ui source outside the generated files (checked by a lint rule in CI)'

Usage: check_no_color_literals.py [--root DIR] | --self-test
"""
import argparse
import os
import re
import sys
import tempfile

# Match hex color literals like 0x123456 or "#123456" or lv_color_hex(0x...) or lv_color_make(...)
HEX_COLOR_RE = re.compile(r'\b0x[0-9a-fA-F]{6}\b')
HEX_STR_RE = re.compile(r'["\']#[0-9a-fA-F]{3,8}["\']')
LV_COLOR_FN_RE = re.compile(r'\blv_color_(hex|make|hex3)\s*\(')
CSS_HEX_RE = re.compile(r'#[0-9a-fA-F]{3,8}\b')
CSS_COLOR_FN_RE = re.compile(r'\b(rgb|rgba|hsl|hsla)\s*\(')

EXEMPT_FILES = {"theme.cpp", "theme.hpp", "lv_conf.h", "tokens.css"}


def check_file(path, rel_path):
    violations = []
    base_name = os.path.basename(path)
    if base_name in EXEMPT_FILES:
        return violations

    is_web = path.endswith((".css", ".html", ".js", ".ts"))

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for idx, line in enumerate(f, 1):
            code = line.split("//")[0]
            if not code.strip():
                continue

            if LV_COLOR_FN_RE.search(code):
                violations.append(
                    f"{rel_path}:{idx}: forbidden direct color creation function call in UI source"
                )
            elif HEX_STR_RE.search(code):
                violations.append(
                    f"{rel_path}:{idx}: forbidden color hex string literal in UI source"
                )
            elif HEX_COLOR_RE.search(code):
                violations.append(
                    f"{rel_path}:{idx}: forbidden 24-bit hex color literal in UI source"
                )
            elif is_web and CSS_HEX_RE.search(code):
                violations.append(
                    f"{rel_path}:{idx}: forbidden raw hex color in desktop web source"
                )
            elif is_web and CSS_COLOR_FN_RE.search(code):
                violations.append(
                    f"{rel_path}:{idx}: forbidden raw rgb/hsl color function in desktop web source"
                )

    return violations


def scan_ui(root):
    ui_dir = os.path.join(root, "ui")
    if not os.path.isdir(ui_dir):
        return []

    violations = []
    for dirpath, _, filenames in os.walk(ui_dir):
        if any(skip in dirpath for skip in ["tests", "build", "third_party", "_deps"]):
            continue
        for fn in filenames:
            if fn.endswith((".cpp", ".hpp", ".c", ".h")):
                p = os.path.join(dirpath, fn)
                rel = os.path.relpath(p, root)
                violations.extend(check_file(p, rel))

    return violations


def scan_desktop(root):
    desktop_dir = os.path.join(root, "desktop")
    if not os.path.isdir(desktop_dir):
        return []

    violations = []
    for dirpath, _, filenames in os.walk(desktop_dir):
        if any(skip in dirpath for skip in ["target", "node_modules", "dist", ".git", "gen", "icons", "tests"]):
            continue
        for fn in filenames:
            if fn.endswith((".css", ".html", ".js", ".ts", ".rs")):
                p = os.path.join(dirpath, fn)
                rel = os.path.relpath(p, root)
                violations.extend(check_file(p, rel))

    return violations


def self_test():
    with tempfile.TemporaryDirectory() as tmp:
        src_dir = os.path.join(tmp, "ui", "src")
        os.makedirs(src_dir)

        # Good file
        good_path = os.path.join(src_dir, "good.cpp")
        with open(good_path, "w") as f:
            f.write('#include "velora/ui/theme.hpp"\nauto c = Theme::current().text_color;\n')

        # Bad file 1: lv_color_hex
        bad1_path = os.path.join(src_dir, "bad1.cpp")
        with open(bad1_path, "w") as f:
            f.write('lv_obj_set_style_bg_color(obj, lv_color_hex(0xff00ff), 0);\n')

        # Bad file 2: hex literal
        bad2_path = os.path.join(src_dir, "bad2.cpp")
        with open(bad2_path, "w") as f:
            f.write('uint32_t col = 0x151917;\n')

        v_good = check_file(good_path, "ui/src/good.cpp")
        assert len(v_good) == 0, f"Expected 0, got {v_good}"

        v_bad1 = check_file(bad1_path, "ui/src/bad1.cpp")
        assert len(v_bad1) > 0, "Expected violation for lv_color_hex"

        v_bad2 = check_file(bad2_path, "ui/src/bad2.cpp")
        assert len(v_bad2) > 0, "Expected violation for hex literal"

        # Desktop test
        desktop_src = os.path.join(tmp, "desktop", "src")
        os.makedirs(desktop_src)
        desktop_good = os.path.join(desktop_src, "style.css")
        with open(desktop_good, "w") as f:
            f.write('body { background: var(--bg); color: var(--text); }\n')
        assert len(check_file(desktop_good, "desktop/src/style.css")) == 0

        desktop_bad = os.path.join(desktop_src, "bad.css")
        with open(desktop_bad, "w") as f:
            f.write('body { background: #123456; }\n')
        assert len(check_file(desktop_bad, "desktop/src/bad.css")) > 0

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
    violations.extend(scan_desktop(args.root))
    if violations:
        print("Color literals lint check FAILED:", file=sys.stderr)
        for v in violations:
            print("  " + v, file=sys.stderr)
        return 1

    print("Color literals lint check passed (no color literals outside theme files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
