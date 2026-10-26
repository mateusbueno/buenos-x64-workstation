#!/usr/bin/env python3
"""Tier-branching gate (Story 4.1, AD-27): one image runs v1 and Pro, so no code may branch on the tier ID.

The tier ID (`TierId::kV1` / `TierId::kPro`, `Tier::...`) is mapped to a profile in ONE place, engine/host/src/tier_profile.cpp (and the port header and the
profile header that declare it). Everything else reads the profile object (part_count, head_budget_bytes, ...). This script fails when a source file under
engine/, tools/, sysd/, ui/ or librarian/ outside those files compares a tier ID (`== TierId::k...`, `!= ...`, `case TierId::k...`, `.tier ==`, `tier ==`),
or uses the preprocessor to compile per tier (`#if VELORA_TIER...`). Comments and string literals are ignored (except tier-name comparisons). Tests are
scanned too; only tier_profile_test.cpp and engine_host_boot_smoke.cpp, which assert on the table's values and State.tier, are exempt. A line carrying the comment
`tier-lint: allow` is skipped (for the wire-value static_assert in control_plane.cpp).

This is a LINT, not a proof: it catches the common spellings of a tier branch; an indirect one (an alias, a helper that returns the ID) can slip past it.
Review stays the real gate.

Usage: check_tier_branching.py [--root DIR] | --self-test
"""
import argparse
import os
import re
import sys

SRC_EXT = {".h", ".hpp", ".cc", ".cpp", ".cxx", ".inl", ".ipp", ".tpp", ".mm"}
ROOTS = ("engine", "tools", "sysd", "ui", "librarian")
# The one place that maps a tier ID to its profile, and the files that declare the ID.
ALLOWED = {
    "engine/host/src/tier_profile.cpp",
    "engine/host/include/velora/host/tier_profile.hpp",
    "engine/ports/include/velora/ports/tier_source.hpp",
}
# Tests are scanned too; only the test of the profile table itself asserts on tier IDs and profiles (it checks data, it does not branch).
TEST_EXEMPT = {"engine/host/tests/tier_profile_test.cpp", "engine/host/macos/engine_host_boot_smoke.cpp"}

ID = r"(?:TierId::kPro|TierId::kV1|(?<![:\w])kPro|(?<![:\w])kV1|TIER_V1|TIER_PRO)"
# Patterns over the code with comments and string literals removed.
CODE_PATTERNS = [
    (re.compile(r"(?:==|!=)\s*[\w:]*" + ID + r"\b"), "comparison with a tier ID"),
    (re.compile(ID + r"\b\s*(?:==|!=)"), "comparison with a tier ID"),
    (re.compile(r"\bcase\s+[\w:]*" + ID + r"\b"), "switch on a tier ID"),
    (re.compile(r"(?:==|!=)\s*[\w:.]*\b(?:pro|v1)_profile\s*\(\s*\)(?!\s*(?:\.|->))"), "comparison with a profile instance"),
    (re.compile(r"\b(?:pro|v1)_profile\s*\(\s*\)(?!\s*(?:\.|->))\s*(?:==|!=)"), "comparison with a profile instance"),
    (re.compile(r"(?:==|!=)\s*&\s*[\w:.]*\b(?:pro|v1)_profile\b"), "comparison with a profile instance"),
    (re.compile(r"\w*[tT]ier\w*\s*(?:==|!=)"), "comparison on a tier value"),
    (re.compile(r"static_cast<\s*(?:int|unsigned|std::u?int\d+_t)\s*>\s*\([^)]*(?:[tT]ier|\bid\b)[^)]*\)\s*(?:==|!=)\s*\d"), "comparison of a cast ID with a number"),
    (re.compile(r"\bpart_count\s*(?:==|!=)\s*(?:8|16)\b|\b(?:8|16)\s*(?:==|!=)\s*[\w.>-]*part_count\b"), "part count compared with a tier's number"),
    (re.compile(r"#\s*(?:if|ifdef|ifndef|elif)\b.*\bVELORA_TIER\w*"), "compile-time tier switch"),
]
# Patterns that need the string literal (comments removed).
STRING_PATTERNS = [
    (re.compile(r"(?:==|!=)\s*\"(?:pro|v1)\""), "comparison with a tier name"),
    (re.compile(r"\"(?:pro|v1)\"\s*(?:==|!=)"), "comparison with a tier name"),
    (re.compile(r"\bstr(?:n)?cmp\s*\([^)]*\"(?:pro|v1)\""), "strcmp with a tier name"),
]


def split_code(text):
    """Returns (raw, nolit): the text with comments removed, and the same with string / char literals emptied. Newlines are kept."""
    raw, nolit = [], []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == "//":
            while i < n and text[i] != "\n":
                i += 1
        elif two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            keep = "\n" * text.count("\n", i, j)
            raw.append(keep)
            nolit.append(keep)
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            raw.append(text[i:j])
            nolit.append(c + c)
            i = j
        else:
            raw.append(c)
            nolit.append(c)
            i += 1
    return "".join(raw), "".join(nolit)


def scan_text(path, text):
    out = []
    raw, nolit = split_code(text)
    rl, nl, ol = raw.split("\n"), nolit.split("\n"), text.split("\n")
    for n, (r, c) in enumerate(zip(rl, nl), 1):
        if n <= len(ol) and ("tier-lint: allow" in ol[n - 1] or (n >= 2 and "tier-lint: allow" in ol[n - 2])):
            continue
        for rx, why in CODE_PATTERNS:
            if rx.search(c):
                out.append((path, n, why, r.strip()))
                break
        else:
            for rx, why in STRING_PATTERNS:
                if rx.search(r):
                    out.append((path, n, why, r.strip()))
                    break
    return out


def scan(root):
    bad = []
    for top in ROOTS:
        base = os.path.join(root, top)
        for dp, dn, fn in os.walk(base):
            dn[:] = [d for d in dn if d not in ("build", "node_modules", ".git")]
            for f in fn:
                if os.path.splitext(f)[1] not in SRC_EXT:
                    continue
                full = os.path.join(dp, f)
                rel = os.path.relpath(full, root).replace(os.sep, "/")
                if rel in ALLOWED or rel in TEST_EXEMPT:
                    continue
                with open(full, encoding="utf-8", errors="replace") as fh:
                    bad += scan_text(rel, fh.read())
    return bad


def self_test():
    ok = True
    must_flag = [
        "if (t.id == ports::TierId::kPro) {",
        "if (tier != Tier::kV1) return;",
        "case TierId::kPro: break;",
        "if (profile.tier == x) {",
        "#if VELORA_TIER_PRO",
        "if (host::profile_for(id) == host::pro_profile()) {",
        "if (&p == &host::pro_profile()) {",
        "if (sel.profile == &v1_profile()) {",
        "if (id == kPro) {",
        "if (t == ws::v1::TIER_PRO) {",
        "if (name == \"pro\") {",
        "if (!strcmp(arg, \"v1\")) {",
        "if (current_tier == 2) {",
        "if (static_cast<int>(id) == 2) {",
        "if (profile.part_count == 16) {",
        "if (8 == opt.part_count) {",
        "int x = 1; /* c */ if (id == TierId::kV1) {",
    ]
    must_pass = [
        "const TierProfile& p = profile_for(TierId::kPro);",
        "constexpr TierProfile kV1{ports::TierId::kV1, \"v1\", 8};",
        "if (profile.part_count > 8) {",
        "// if (id == TierId::kPro) is what this gate forbids",
        "/* if (id == TierId::kPro) */ int y = 0;",
        "log(\"tier == pro\");",
        "o.part_count = 16;",
        "const char* s = \"== TierId::kPro\";",
    ]
    for l in must_flag:
        if not scan_text("x.cpp", l):
            print("self-test: not flagged:", l)
            ok = False
    for l in must_pass:
        if scan_text("x.cpp", l):
            print("self-test: wrongly flagged:", l)
            ok = False
    print("check_tier_branching self-test:", "ok" if ok else "FAILED")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    bad = scan(os.path.abspath(a.root))
    for path, n, why, line in bad:
        print(f"{path}:{n}: {why}: {line}")
    if bad:
        print("tier branching outside engine/host/src/tier_profile.cpp: read the TierProfile instead (AD-27)")
        return 1
    print("check_tier_branching: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
