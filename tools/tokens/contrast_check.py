#!/usr/bin/env python3
"""Contrast gate for Velora design tokens.

Story 2.18 AC:
'When the CI contrast check runs over both themes
Then it fails the build below 4.5:1 for any text pair or 3:1 for any meaningful boundary,
and on any style that applies opacity to text'

Usage: contrast_check.py [--design-file PATH] | --self-test
"""
import argparse
import sys
import os
import re

TEXT_THRESHOLD = 4.5
BOUNDARY_THRESHOLD = 3.0


def srgb_to_linear(c_byte):
    c = c_byte / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def relative_luminance(hex_str):
    hex_str = hex_str.strip().lstrip('#')
    if len(hex_str) == 3:
        hex_str = "".join(c * 2 for c in hex_str)
    r = int(hex_str[0:2], 16)
    g = int(hex_str[2:4], 16)
    b = int(hex_str[4:6], 16)
    return 0.2126 * srgb_to_linear(r) + 0.7152 * srgb_to_linear(g) + 0.0722 * srgb_to_linear(b)


def contrast_ratio(hex1, hex2):
    l1 = relative_luminance(hex1)
    l2 = relative_luminance(hex2)
    lighter = max(l1, l2)
    darker = min(l1, l2)
    return (lighter + 0.05) / (darker + 0.05)


def strip_yaml_comment(line):
    in_single = False
    in_double = False
    for i, ch in enumerate(line):
        if ch == "'" and not in_double:
            in_single = not in_single
        elif ch == '"' and not in_single:
            in_double = not in_double
        elif ch == '#' and not in_single and not in_double:
            return line[:i].rstrip()
    return line.rstrip()


def safe_load_yaml(text):
    try:
        import yaml
        return yaml.safe_load(text)
    except ImportError:
        pass

    root = {}
    stack = [(root, -1)]
    for raw_line in text.splitlines():
        line = strip_yaml_comment(raw_line)
        if not line.strip():
            continue
        indent = len(raw_line) - len(raw_line.lstrip())
        content = line.strip()
        while len(stack) > 1 and indent <= stack[-1][1]:
            stack.pop()
        curr_container, _ = stack[-1]
        if content.startswith('- '):
            item = content[2:].strip().strip('\'"')
            if isinstance(curr_container, list):
                curr_container.append(item)
            continue
        if ':' in content:
            key, val = content.split(':', 1)
            key = key.strip().strip('\'"')
            val = val.strip()
            if not val:
                new_dict = {}
                if isinstance(curr_container, dict):
                    curr_container[key] = new_dict
                stack.append((new_dict, indent))
            else:
                val = val.strip('\'"')
                if isinstance(curr_container, dict):
                    curr_container[key] = val
    return root


def parse_frontmatter(design_md_path):
    with open(design_md_path, "r", encoding="utf-8") as f:
        content = f.read()

    m = re.match(r"^---\s*\n(.*?)\n---", content, re.DOTALL)
    if not m:
        raise ValueError(f"No YAML frontmatter found in {design_md_path}")

    yaml_text = m.group(1)
    data = safe_load_yaml(yaml_text)
    colors = data.get("colors", {})
    return colors, yaml_text


def check_contrast(colors, yaml_text):
    errors = []

    # 1. Check for text opacity in yaml_text
    if re.search(r"opacity\s*:\s*(?!1(\.0+)?\b)[0-9.]+", yaml_text):
        errors.append("Found style token applying opacity to text (violates contrast standard)")

    # Text pairs to verify
    text_checks = [
        # Stage theme
        ("Stage text on bg", "text-dark", "bg-dark"),
        ("Stage text on surface", "text-dark", "surface-dark"),
        ("Stage text on raised", "text-dark", "raised-dark"),
        ("Stage on-accent on accent", "on-accent-dark", "accent-dark"),
        ("Stage ok on bg", "ok-dark", "bg-dark"),
        ("Stage warn on bg", "warn-dark", "bg-dark"),
        # Studio theme
        ("Studio text on bg", "text-light", "bg-light"),
        ("Studio text on surface", "text-light", "surface-light"),
        ("Studio text on raised", "text-light", "raised-light"),
        ("Studio on-accent on accent", "on-accent-light", "accent-light"),
        ("Studio ok on bg", "ok-light", "bg-light"),
        ("Studio warn on bg", "warn-light", "bg-light"),
    ]

    for label, fg_name, bg_name in text_checks:
        if fg_name not in colors or bg_name not in colors:
            errors.append(f"Missing color token: {fg_name} or {bg_name}")
            continue
        fg = colors[fg_name]
        bg = colors[bg_name]
        cr = contrast_ratio(fg, bg)
        if cr < TEXT_THRESHOLD:
            errors.append(
                f"{label} ({fg} on {bg}) has contrast {cr:.2f}:1, below required {TEXT_THRESHOLD}:1"
            )

    # Meaningful boundary checks
    boundary_checks = [
        ("Stage line-strong on bg", "line-strong-dark", "bg-dark"),
        ("Stage line-strong on surface", "line-strong-dark", "surface-dark"),
        ("Studio line-strong on bg", "line-strong-light", "bg-light"),
        ("Studio line-strong on surface", "line-strong-light", "surface-light"),
    ]

    for label, fg_name, bg_name in boundary_checks:
        if fg_name not in colors or bg_name not in colors:
            errors.append(f"Missing color token: {fg_name} or {bg_name}")
            continue
        fg = colors[fg_name]
        bg = colors[bg_name]
        cr = contrast_ratio(fg, bg)
        if cr < BOUNDARY_THRESHOLD:
            errors.append(
                f"{label} ({fg} on {bg}) has contrast {cr:.2f}:1, below required {BOUNDARY_THRESHOLD}:1"
            )

    return errors


def self_test():
    # 1. Pure black on pure white = 21:1
    assert abs(contrast_ratio("#000000", "#ffffff") - 21.0) < 0.1
    # 2. Pure white on pure black = 21:1
    assert abs(contrast_ratio("#ffffff", "#000000") - 21.0) < 0.1
    # 3. Failing pair: #777777 on #888888
    assert contrast_ratio("#777777", "#888888") < 3.0
    print("self-test ok")
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default="")
    parser.add_argument("--design-file", default="")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    root = args.root or os.getcwd()
    design_file = args.design_file
    if not design_file:
        candidates = [
            os.path.join(root, "_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/DESIGN.md"),
            "_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/DESIGN.md",
            "../_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/DESIGN.md",
        ]
        for c in candidates:
            if os.path.exists(c):
                design_file = c
                break

    if not design_file or not os.path.exists(design_file):
        print(f"Error: DESIGN.md not found at {design_file}", file=sys.stderr)
        return 1

    colors, yaml_text = parse_frontmatter(design_file)
    errors = check_contrast(colors, yaml_text)
    if errors:
        print("Contrast check FAILED:", file=sys.stderr)
        for e in errors:
            print("  " + e, file=sys.stderr)
        return 1

    print("Contrast gate check passed (all text pairs >= 4.5:1, boundaries >= 3.0:1)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
