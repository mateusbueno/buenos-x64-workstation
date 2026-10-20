#!/usr/bin/env python3
"""Ladiani logo system: one geometric source for every SVG master.

Directions locked in DESIGN.md: symbol S1 (drawbar "L"), wordmark T3
(humanist-technical, light, wide tracking), model mark V1 (uppercase, spaced,
subordinate). Letters are drawn on a 100-unit cap height as monoline strokes;
diagonals are trimmed flat at cap line and baseline, like cut type.

Usage: python3 build_logo.py            -> stroked masters next to this script
       python3 build_logo.py --outline  -> filled outlines in ./outlined/
                                           (needs shapely: pip install shapely)
"""
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUTLINE = "--outline" in sys.argv
OUT = os.path.join(HERE, "outlined") if OUTLINE else HERE

INK = "#111111"      # logo-ink
PAPER = "#f4f2ee"    # logo-paper
SCREEN_BG = "#0b0d0c"  # bg-dark
SCREEN_FG = "#e7ebe8"  # text-dark
SURFACE = "#151917"  # surface-dark

CAP = 100.0


def f(v):
    return f"{v:.2f}".rstrip("0").rstrip(".")


# ---------------------------------------------------------------- glyphs
# Each glyph returns (paths, width, clipped, left_side, right_side).
# paths: list of SVG path "d" strings stroked at width w.
# Side classes drive spacing: S straight, R round, D diagonal, O open.

def g_L(w):
    h = w / 2
    return [f"M{f(h)} 0V{f(CAP-h)}H46"], 46, False, "S", "O"


def g_I(w):
    h = w / 2
    return [f"M{f(h)} 0V{f(CAP)}"], w, False, "S", "S"


def _legs(w, s, flat):
    h = w / 2
    k = h * math.sqrt(1 + s * s)          # horizontal half-thickness of a leg
    ya = (k - flat / 2) / s               # where leg centerlines meet
    cx = s * (CAP - ya) + k               # half-width at the open end
    return k, ya, cx


def g_A(w, s=0.30, flat=2.0, bar=64.0):
    k, ya, cx = _legs(w, s, flat)
    y2 = CAP + 12
    d = [
        f"M{f(cx - s*(y2-ya))} {f(y2)}L{f(cx)} {f(ya)}L{f(cx + s*(y2-ya))} {f(y2)}",
        f"M{f(cx - s*(bar-ya))} {f(bar)}H{f(cx + s*(bar-ya))}",
    ]
    return d, 2 * cx, True, "D", "D"


def g_V(w, s=0.30, flat=2.0):
    k, ya, cx = _legs(w, s, flat)
    yb = CAP - ya
    y1 = -12
    d = [f"M{f(cx - s*(yb-y1))} {f(y1)}L{f(cx)} {f(yb)}L{f(cx + s*(yb-y1))} {f(y1)}"]
    return d, 2 * cx, True, "D", "D"


def g_N(w, W=68.0):
    h = w / 2
    k = h
    for _ in range(20):                   # diagonal edges meet the outer corners
        dx = W - 2 * k
        k = h * math.sqrt(1 + (dx / CAP) ** 2)
    x0, x1 = k, W - k
    t0, t1 = -0.12, 1.12
    d = [
        f"M{f(h)} {f(CAP)}V0",
        f"M{f(W-h)} 0V{f(CAP)}",
        f"M{f(x0 + (x1-x0)*t0)} {f(CAP*t0)}L{f(x0 + (x1-x0)*t1)} {f(CAP*t1)}",
    ]
    return d, W, True, "S", "S"


def g_D(w, W=70.0, xs=26.0):
    h = w / 2
    xr = W - h
    r = (CAP - 2 * h) / 2
    c = 0.60
    d = [
        f"M{f(h)} {f(h)}H{f(xs)}"
        f"C{f(xs + (xr-xs)*c)} {f(h)} {f(xr)} {f(50 - r*c)} {f(xr)} 50"
        f"C{f(xr)} {f(50 + r*c)} {f(xs + (xr-xs)*c)} {f(CAP-h)} {f(xs)} {f(CAP-h)}"
        f"H{f(h)}Z"
    ]
    return d, W, False, "S", "R"


def g_E(w):
    h = w / 2
    d = [f"M50 {f(h)}H{f(h)}V{f(CAP-h)}H52", f"M{f(h)} 48H45"]
    return d, 52, False, "S", "O"


def g_O(w, rx=46.0, over=1.5):
    h = w / 2
    ry = 50 - h + over
    cx = rx + h
    k = 0.5523
    d = [
        f"M{f(cx-rx)} 50"
        f"C{f(cx-rx)} {f(50-ry*k)} {f(cx-rx*k)} {f(50-ry)} {f(cx)} {f(50-ry)}"
        f"C{f(cx+rx*k)} {f(50-ry)} {f(cx+rx)} {f(50-ry*k)} {f(cx+rx)} 50"
        f"C{f(cx+rx)} {f(50+ry*k)} {f(cx+rx*k)} {f(50+ry)} {f(cx)} {f(50+ry)}"
        f"C{f(cx-rx*k)} {f(50+ry)} {f(cx-rx)} {f(50+ry*k)} {f(cx-rx)} 50Z"
    ]
    return d, 2 * cx, False, "R", "R"


def g_R(w, W=60.0):
    h = w / 2
    xs, xb, yb = 30.0, 55.0, 54.0
    cy, ry, rx = (h + yb) / 2, (yb - h) / 2, xb - xs
    k = 0.5523
    s = (W - 3 - xs) / (CAP - yb)
    d = [
        f"M{f(h)} {f(CAP)}V{f(h)}H{f(xs)}"
        f"C{f(xs + rx*k)} {f(h)} {f(xb)} {f(cy - ry*k)} {f(xb)} {f(cy)}"
        f"C{f(xb)} {f(cy + ry*k)} {f(xs + rx*k)} {f(yb)} {f(xs)} {f(yb)}H{f(h)}",
        f"M{f(xs - 4)} {f(yb - 4/s)}L{f(xs + s*(CAP + 12 - yb))} {f(CAP + 12)}",
    ]
    return d, W, True, "S", "D"


def g_7(w):
    h = w / 2
    d = [f"M0 {f(h)}H52L{f(52 - 0.36*(CAP + 12))} {f(CAP + 12)}"]
    return d, 54, True, "O", "D"


def g_3(w):
    d = [
        "M5 15C11 7 20 3.6 30 3.6C44 3.6 52 12 52 25C52 38 43 46.5 28 46.5H22",
        "M22 46.5H31C48 46.5 57.5 56 57.5 71C57.5 87 46 97.6 30 97.6"
        "C18 97.6 8.5 93 3 83",
    ]
    return d, 61, False, "R", "R"


GLYPHS = {"L": g_L, "I": g_I, "A": g_A, "V": g_V, "N": g_N, "D": g_D,
          "E": g_E, "O": g_O, "R": g_R, "7": g_7, "3": g_3}

# Gap between outer edges for a (right side, left side) pair at base tracking.
GAPS = {
    ("S", "S"): 46, ("S", "R"): 40, ("R", "S"): 40, ("R", "R"): 34,
    ("S", "D"): 34, ("D", "S"): 34, ("D", "D"): 20, ("R", "D"): 30,
    ("D", "R"): 30, ("O", "D"): 18, ("O", "S"): 40, ("O", "R"): 34,
    ("S", "O"): 46, ("D", "O"): 26, ("R", "O"): 34, ("O", "O"): 30,
}


def _flatten(d, steps=48):
    """Parse our absolute M/L/H/V/C/Z paths into polylines: [(points, closed)]."""
    toks = re.findall(r"[MLHVCZ]|-?\d*\.?\d+", d)
    subs, pts, i, cmd = [], [], 0, None
    x = y = 0.0
    while i < len(toks):
        t = toks[i]
        if t.isalpha():
            cmd = t
            i += 1
            if cmd == "Z":
                subs.append((pts, True))
                pts = []
            continue
        n = lambda k: float(toks[i + k])
        if cmd == "M":
            if pts:
                subs.append((pts, False))
            x, y = n(0), n(1); pts = [(x, y)]; i += 2; cmd = "L"
        elif cmd == "L":
            x, y = n(0), n(1); pts.append((x, y)); i += 2
        elif cmd == "H":
            x = n(0); pts.append((x, y)); i += 1
        elif cmd == "V":
            y = n(0); pts.append((x, y)); i += 1
        elif cmd == "C":
            x1, y1, x2, y2, x3, y3 = (n(k) for k in range(6))
            for j in range(1, steps + 1):
                u = j / steps; v = 1 - u
                pts.append((v**3*x + 3*v*v*u*x1 + 3*v*u*u*x2 + u**3*x3,
                            v**3*y + 3*v*v*u*y1 + 3*v*u*u*y2 + u**3*y3))
            x, y = x3, y3; i += 6
    if pts:
        subs.append((pts, False))
    return subs


def _outline(paths, w, clipped):
    """Stroke centerlines into one filled, unioned shape (butt caps, miter joins)."""
    from shapely.geometry import LineString, LinearRing, box
    from shapely.ops import unary_union
    shapes = []
    for d in paths:
        for pts, closed in _flatten(d):
            line = LinearRing(pts) if closed else LineString(pts)
            shapes.append(line.buffer(w / 2, cap_style="flat", join_style="mitre", mitre_limit=8))
    shape = unary_union(shapes)
    if clipped:
        shape = shape.intersection(box(-200, 0, 2000, CAP))
    return shape.simplify(0.01)


def _shape_d(shape, dx=0.0):
    polys = getattr(shape, "geoms", [shape])
    out = []
    for poly in polys:
        for ring in [poly.exterior, *poly.interiors]:
            c = list(ring.coords)[:-1]
            out.append("M" + "L".join(f"{f(px + dx)} {f(py)}" for px, py in c) + "Z")
    return "".join(out)


def word(text, w, tracking=1.0, space=96):
    """Lay out a word; returns (svg group body, width).

    Stroked mode: one stroked <g> per glyph. Outline mode: a single filled path.
    """
    x = 0.0
    prev = None
    parts = []
    for ch in text:
        if ch == " ":
            x += space * tracking
            prev = None
            continue
        paths, gw, clipped, ls, rs = GLYPHS[ch](w)
        if prev is not None:
            x += GAPS[(prev, ls)] * tracking
        if OUTLINE:
            parts.append(_shape_d(_outline(paths, w, clipped), x))
        else:
            clip = ' clip-path="url(#cap)"' if clipped else ""
            body = "".join(f'<path d="{d}"/>' for d in paths)
            parts.append(f'<g transform="translate({f(x)} 0)"{clip}>{body}</g>')
        x += gw
        prev = rs
    if OUTLINE:
        return f'<path fill-rule="evenodd" d="{"".join(parts)}"/>', x
    return "".join(parts), x


def ink(color, w):
    """Paint attributes for a text group: fill in outline mode, stroke otherwise."""
    if OUTLINE:
        return f'fill="{color}"'
    return f'stroke="{color}" {strokes(w)}'


def strokes(w):
    return (f'fill="none" stroke-width="{f(w)}" stroke-linejoin="miter" '
            f'stroke-miterlimit="8" stroke-linecap="butt"')


CAP_CLIP = '<clipPath id="cap"><rect x="-200" y="0" width="2000" height="100"/></clipPath>'

# ---------------------------------------------------------------- symbol
# S1: one tall + four short drawbars. Tall = 1.7 cap, short = half cap
# (its top sits on the cap mid line), bar width 0.22 cap, gap 0.13 cap.
BAR, GAP, TALL, SHORT = 22.0, 13.0, 170.0, 50.0
SYMBOL_W = 5 * BAR + 4 * GAP


def symbol_rects(x0=0.0, base=CAP):
    out = [f'<rect x="{f(x0)}" y="{f(base-TALL)}" width="{f(BAR)}" height="{f(TALL)}" rx="{f(BAR/2)}"/>']
    for i in range(1, 5):
        x = x0 + i * (BAR + GAP)
        out.append(f'<rect x="{f(x)}" y="{f(base-SHORT)}" width="{f(BAR)}" height="{f(SHORT)}" rx="{f(BAR/2)}"/>')
    return "".join(out)


def svg(vb, body, title, bg=None):
    x, y, w, h = vb
    back = f'<rect x="{f(x)}" y="{f(y)}" width="{f(w)}" height="{f(h)}" fill="{bg}"/>' if bg else ""
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{f(x)} {f(y)} {f(w)} {f(h)}" '
            f'role="img" aria-label="{title}"><title>{title}</title>{back}{body}</svg>\n')


BRAND_W = 7.0      # wordmark stroke: light
MODEL_W = 9.0      # model mark stroke: optically heavier, it is set small
MODEL_SCALE = 0.40
MODEL_TRACK = 1.9  # VELORA is spaced wider than LADIANI (V1)
SYM_TO_WORD = 52.0


def build():
    files = {}
    brand_body, brand_w = word("LADIANI", BRAND_W)
    model_body, model_w = word("VELORA", MODEL_W, MODEL_TRACK)
    model73_body, model73_w = word("VELORA 73", MODEL_W, MODEL_TRACK)

    def brand(color, x=0.0):
        return (f'<g transform="translate({f(x)} 0)" {ink(color, BRAND_W)}>'
                f'{brand_body}</g>')

    def model(body, color, x, y, scale=MODEL_SCALE):
        return (f'<g transform="translate({f(x)} {f(y)}) scale({f(scale)})" '
                f'{ink(color, MODEL_W)}>{body}</g>')

    defs = "" if OUTLINE else f"<defs>{CAP_CLIP}</defs>"
    pad = 0

    for name, fg, bg in (("ink", INK, None), ("reversed", SCREEN_FG, None)):
        # Symbol master
        files[f"ladiani-symbol-{name}.svg"] = svg(
            (0, CAP - TALL, SYMBOL_W, TALL), f'<g fill="{fg}">{symbol_rects()}</g>',
            "Ladiani")
        # Wordmark
        files[f"ladiani-wordmark-{name}.svg"] = svg(
            (-1, -1, brand_w + 2, CAP + 2), defs + brand(fg), "Ladiani")
        # Horizontal lockup
        lw = SYMBOL_W + SYM_TO_WORD + brand_w
        files[f"ladiani-lockup-{name}.svg"] = svg(
            (-pad, CAP - TALL, lw + 1, TALL + 1),
            defs + f'<g fill="{fg}">{symbol_rects()}</g>' + brand(fg, SYMBOL_W + SYM_TO_WORD),
            "Ladiani")
        # Panel lockup: brand row, hairline, VELORA 73 below and smaller
        rule_y = CAP + 44
        my = rule_y + 40
        panel_h = my + CAP * MODEL_SCALE
        files[f"ladiani-velora73-panel-{name}.svg"] = svg(
            (0, CAP - TALL, lw + 1, panel_h - (CAP - TALL) + 2),
            defs + f'<g fill="{fg}">{symbol_rects()}</g>' + brand(fg, SYMBOL_W + SYM_TO_WORD)
            + f'<rect x="0" y="{f(rule_y - 0.8)}" width="{f(lw)}" height="1.6" fill="{fg}" opacity=".55"/>'
            + model(model73_body, fg, 0, my),
            "Ladiani Velora 73")
        # Model mark alone
        files[f"velora-wordmark-{name}.svg"] = svg(
            (-1, -1, model_w + 2, CAP + 2),
            defs + f'<g {ink(fg, MODEL_W)}>{model_body}</g>', "Velora")

    # Small-size symbols, drawn on their own pixel grid.
    def small(size, bars, fg, bg, radius):
        return svg((0, 0, size, size),
                   (f'<rect width="{size}" height="{size}" rx="{f(radius)}" fill="{bg}"/>' if bg else "")
                   + f'<g fill="{fg}">{bars}</g>', "Ladiani")

    s32 = ('<rect x="5" y="4" width="6" height="24" rx="3"/>'
           '<rect x="13" y="19" width="6" height="9" rx="3"/>'
           '<rect x="21" y="19" width="6" height="9" rx="3"/>')
    s16 = ('<rect x="3.5" y="2" width="4" height="12" rx="2"/>'
           '<rect x="9" y="9" width="4" height="5" rx="2"/>')
    files["ladiani-symbol-32.svg"] = small(32, s32, INK, None, 0)
    files["ladiani-symbol-16.svg"] = small(16, s16, INK, None, 0)
    files["ladiani-favicon-32.svg"] = small(32, s32, SCREEN_FG, SURFACE, 7)
    files["ladiani-favicon-16.svg"] = small(16, s16, SCREEN_FG, SURFACE, 3.5)

    # App icon 1024: symbol on the Sálvia surface, optically centred.
    sc = 3.0
    sw, sh = SYMBOL_W * sc, TALL * sc
    ox, oy = (1024 - sw) / 2, (1024 - sh) / 2 + 10
    files["ladiani-app-icon.svg"] = svg(
        (0, 0, 1024, 1024),
        f'<rect width="1024" height="1024" rx="230" fill="{SURFACE}"/>'
        f'<g transform="translate({f(ox)} {f(oy - (CAP-TALL)*sc)}) scale({f(sc)})" fill="{SCREEN_FG}">'
        f'{symbol_rects()}</g>', "Ladiani")

    # Social avatar 400: circle-safe, symbol at 56% of the diameter.
    for name, fg, bg in (("paper", INK, PAPER), ("dark", SCREEN_FG, SCREEN_BG)):
        sc = 400 * 0.50 / TALL
        sw, sh = SYMBOL_W * sc, TALL * sc
        ox, oy = (400 - sw) / 2, (400 - sh) / 2
        files[f"ladiani-avatar-{name}.svg"] = svg(
            (0, 0, 400, 400),
            f'<rect width="400" height="400" fill="{bg}"/>'
            f'<g transform="translate({f(ox)} {f(oy - (CAP-TALL)*sc)}) scale({f(sc)})" fill="{fg}">'
            f'{symbol_rects()}</g>', "Ladiani")

    os.makedirs(OUT, exist_ok=True)
    for fname, content in files.items():
        with open(os.path.join(OUT, fname), "w") as fh:
            fh.write(content)
    print(f"brand wordmark width {brand_w:.1f}, model {model_w:.1f}; wrote {len(files)} files")


if __name__ == "__main__":
    build()
