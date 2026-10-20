#!/usr/bin/env python3
"""Assemble the Ladiani brand sheet (ladiani-logo.html) from the SVG masters.

Run build_logo.py first. Monochrome masters are inlined with currentColor so
they follow the sheet's light/dark theme; app icon and avatars keep their own
colors.
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))


def load(name, current=True, cls=""):
    s = open(os.path.join(HERE, name)).read().strip()
    s = re.sub(r"<title>.*?</title>", "", s)
    if current:
        s = s.replace('"#111111"', '"currentColor"')
    if cls:
        s = s.replace("<svg ", f'<svg class="{cls}" ', 1)
    return s


import build_logo as B  # noqa: E402

CAP = B.CAP
lock_w = B.SYMBOL_W + B.SYM_TO_WORD + 528.0

# Construction drawing: lockup plus guides, in lockup units.
guides = []
top = CAP - B.TALL
for y, label in ((top, "1.7 C"), (0, "cap"), (50, "½ C"), (CAP, "base")):
    guides.append(f'<path d="M-40 {y}H{lock_w + 40}" class="g"/>'
                  f'<text x="{lock_w + 48}" y="{y + 7}" class="gl">{label}</text>')
guides.append(f'<path d="M{B.SYMBOL_W} {top - 30}V{CAP + 30}M{B.SYMBOL_W + B.SYM_TO_WORD} {top - 30}V{CAP + 30}" class="g"/>')
guides.append(f'<text x="{B.SYMBOL_W + 4}" y="{CAP + 52}" class="gl">0.52 C</text>')
guides.append(f'<path d="M0 {CAP + 30}H{B.BAR}" class="m"/><text x="0" y="{CAP + 52}" class="gl">0.22 C</text>')
lockup_ink = load("ladiani-lockup-ink.svg")
inner = re.search(r"<svg[^>]*>(.*)</svg>", lockup_ink, re.S).group(1)
construction = (f'<svg class="construct" viewBox="-60 {top - 50} {lock_w + 190} {B.TALL + 120}" '
                f'role="img" aria-label="Construction grid of the Ladiani lockup">'
                f'{"".join(guides)}{inner}</svg>')

# Boot screen: symbol + VELORA, drawbars pull out one by one.
sym_body = re.search(r"<svg[^>]*>(.*)</svg>", load("ladiani-symbol-ink.svg"), re.S).group(1)
velora = load("velora-wordmark-reversed.svg").replace('"#e7ebe8"', '"currentColor"')

files = sorted(f for f in os.listdir(HERE) if f.endswith(".svg"))

T = open(os.path.join(HERE, "sheet_template.html")).read()
out = (T.replace("{{LOCKUP}}", load("ladiani-lockup-ink.svg", cls="hero-mark"))
        .replace("{{CONSTRUCTION}}", construction)
        .replace("{{LOCKUP_SM}}", load("ladiani-lockup-ink.svg"))
        .replace("{{PANEL}}", load("ladiani-velora73-panel-ink.svg"))
        .replace("{{SYMBOL}}", load("ladiani-symbol-ink.svg"))
        .replace("{{WORDMARK}}", load("ladiani-wordmark-ink.svg"))
        .replace("{{VELORA}}", load("velora-wordmark-ink.svg"))
        .replace("{{S32}}", load("ladiani-symbol-32.svg"))
        .replace("{{S16}}", load("ladiani-symbol-16.svg"))
        .replace("{{ICON}}", load("ladiani-app-icon.svg", current=False))
        .replace("{{AV_PAPER}}", load("ladiani-avatar-paper.svg", current=False))
        .replace("{{AV_DARK}}", load("ladiani-avatar-dark.svg", current=False))
        .replace("{{FAV32}}", load("ladiani-favicon-32.svg", current=False))
        .replace("{{FAV16}}", load("ladiani-favicon-16.svg", current=False))
        .replace("{{BOOT_SYMBOL}}", f'<svg class="boot-sym" viewBox="0 {CAP - B.TALL} {B.SYMBOL_W} {B.TALL}" aria-hidden="true">{sym_body}</svg>')
        .replace("{{BOOT_WORD}}", velora.replace("<svg ", '<svg class="boot-word" ', 1))
        .replace("{{FILES}}", "".join(f"<li><code>{f}</code></li>" for f in files)))
open(os.path.join(HERE, "ladiani-logo.html"), "w").write(out)
print("wrote ladiani-logo.html", len(out), "bytes")
