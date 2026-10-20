#!/usr/bin/env python3
"""YouTube channel art: video thumbnail (1280x720) and channel banner (2560x1440).

Both are HTML compositions rasterized by a headless Chromium browser, using the
outlined logo masters (run build_logo.py --outline first). Output: ./youtube/.

Banner: brand only (the product is not public yet). Safe areas (YouTube): everything that must be read sits inside the
1546x423 centre; desktop shows the full-width 2560x423 strip; TV shows it all.
"""
import os
import re
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "outlined")
OUT = os.path.join(HERE, "youtube")
BROWSERS = [
    "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "/Applications/Chromium.app/Contents/MacOS/Chromium",
]

BG, SURFACE, LINE, LINE_STRONG = "#0b0d0c", "#151917", "#2b322e", "#5a6860"
TEXT, MUTED, ACCENT = "#e7ebe8", "#88938c", "#9fbaa8"

# Drawbar order on a tonewheel organ: footage, head colour.
FOOTAGES = [("16", "brown"), ("5⅓", "brown"), ("8", "white"), ("4", "white"),
            ("2⅔", "black"), ("2", "white"), ("1⅗", "black"), ("1⅓", "black"), ("1", "white")]
HEADS = {"brown": ("#7f5e47", "#f1e9df", None),
         "white": ("#e7e4dc", "#1d201e", None),
         "black": ("#1d201e", "#e7e4dc", "#667069")}


def logo(name, cls=""):
    s = open(os.path.join(SRC, name + ".svg")).read().strip()
    s = re.sub(r"<title>.*?</title>", "", s)
    return s.replace("<svg ", f'<svg class="{cls}" ', 1)


def drawbars(registration, x0, rail_y, slot=46, gap=18, step=21, head=64, labels=True, scale=1.0):
    """A set of nine drawbars hanging from a rail, pulled out per registration ('888000000')."""
    out = []
    slot, gap, step, head = slot * scale, gap * scale, step * scale, head * scale
    width = 9 * slot + 8 * gap
    out.append(f'<rect x="{x0 - 14*scale}" y="{rail_y - 6*scale}" width="{width + 28*scale}" height="{12*scale}" '
               f'rx="{6*scale}" fill="{SURFACE}" stroke="{LINE}" stroke-width="{2*scale}"/>')
    for i, ((foot, kind), level) in enumerate(zip(FOOTAGES, registration)):
        x = x0 + i * (slot + gap)
        pull = int(level)
        top = rail_y + 8 * scale + pull * step
        fill, ink, edge = HEADS[kind]
        shaft_w = 12 * scale
        out.append(f'<rect x="{x + slot/2 - shaft_w/2}" y="{rail_y}" width="{shaft_w}" height="{top - rail_y + 4*scale}" '
                   f'fill="{LINE_STRONG}"/>')
        stroke = f' stroke="{edge}" stroke-width="{2*scale}"' if edge else ""
        out.append(f'<rect x="{x}" y="{top}" width="{slot}" height="{head}" rx="{10*scale}" fill="{fill}"{stroke}/>')
        if labels:
            out.append(f'<text x="{x + slot/2}" y="{top + head*0.62}" text-anchor="middle" '
                       f'font-size="{19*scale}" fill="{ink}" class="foot">{foot}′</text>')
    return "".join(out), width


CSS = f"""
* {{ margin:0; padding:0; box-sizing:border-box; }}
html, body {{ background:{BG}; overflow:hidden; }}
body {{ font-family:"Gill Sans","Gill Sans MT",Calibri,sans-serif; color:{TEXT}; }}
svg {{ display:block; }}
.foot {{ font-family:"Gill Sans","Gill Sans MT",Calibri,sans-serif; font-weight:600; }}
"""


def banner():
    """Brand only, no product: the LADIANI lockup under a low spotlight."""
    W, H = 2560, 1440
    strip_top = (H - 423) / 2
    lock_w = 860
    lock_h = lock_w * 276 / 1200
    art = (f'<svg width="{W}" height="{H}" viewBox="0 0 {W} {H}" xmlns="http://www.w3.org/2000/svg">'
           f'<defs><radialGradient id="spot" cx="50%" cy="50%" r="50%">'
           f'<stop offset="0" stop-color="#1b221e"/><stop offset=".55" stop-color="#101412"/>'
           f'<stop offset="1" stop-color="{BG}"/></radialGradient></defs>'
           f'<rect width="{W}" height="{H}" fill="{BG}"/>'
           f'<ellipse cx="{W/2}" cy="{H/2}" rx="{W*0.30}" ry="{H*0.30}" fill="url(#spot)"/>'
           f'</svg>')
    lock = logo("ladiani-lockup-reversed", "lock")
    return f"""<!doctype html><meta charset="utf-8"><style>{CSS}
.stage {{ position:relative; width:{W}px; height:{H}px; }}
.lock {{ position:absolute; width:{lock_w}px; height:auto; left:{(W-lock_w)/2}px; top:{strip_top + (423 - lock_h)/2}px; }}
</style><div class="stage">{art}{lock}</div>"""


def thumb():
    W, H = 1280, 720
    bars, bw = drawbars("888000000", 0, 0, scale=1.0, labels=True)
    bx, by = 84, 170
    art = (f'<svg width="{W}" height="{H}" viewBox="0 0 {W} {H}" xmlns="http://www.w3.org/2000/svg">'
           f'<defs><radialGradient id="spot" cx="28%" cy="45%" r="55%">'
           f'<stop offset="0" stop-color="#1f2621"/><stop offset="1" stop-color="{BG}"/></radialGradient></defs>'
           f'<rect width="{W}" height="{H}" fill="url(#spot)"/>'
           f'<g transform="translate({bx} {by})">{bars}</g>'
           f'<text x="{bx + bw/2}" y="{by + 8 + 8*21 + 64 + 64}" text-anchor="middle" font-size="26" '
           f'letter-spacing="8" fill="{MUTED}">88 8000 000</text>'
           f'</svg>')
    return f"""<!doctype html><meta charset="utf-8"><style>{CSS}
.stage {{ position:relative; width:{W}px; height:{H}px; }}
.right {{ position:absolute; left:720px; top:0; bottom:0; width:480px; display:flex; flex-direction:column;
  justify-content:center; gap:34px; }}
.brand {{ width:250px; height:auto; }}
.model {{ width:480px; height:auto; }}
.kicker {{ font-size:40px; font-weight:400; letter-spacing:.02em; color:{TEXT}; line-height:1.15; }}
.kicker span {{ color:{ACCENT}; }}
.rule {{ height:2px; width:72px; background:{ACCENT}; }}
</style><div class="stage">{art}
<div class="right">{logo("ladiani-lockup-reversed", "brand")}
<div class="rule"></div>
{logo("velora-wordmark-reversed", "model")}
<div class="kicker">O novo teclado de palco<br><span>da Ladiani.</span></div>
</div></div>"""


def thumb_brand():
    """Teaser thumbnail: brand only, same low spotlight as the banner."""
    W, H = 1280, 720
    lock_w = 720
    lock_h = lock_w * 276 / 1200
    art = (f'<svg width="{W}" height="{H}" viewBox="0 0 {W} {H}" xmlns="http://www.w3.org/2000/svg">'
           f'<defs><radialGradient id="spot" cx="50%" cy="50%" r="50%">'
           f'<stop offset="0" stop-color="#1d2520"/><stop offset=".55" stop-color="#111513"/>'
           f'<stop offset="1" stop-color="{BG}"/></radialGradient></defs>'
           f'<rect width="{W}" height="{H}" fill="{BG}"/>'
           f'<ellipse cx="{W/2}" cy="{H*0.47}" rx="{W*0.46}" ry="{H*0.5}" fill="url(#spot)"/>'
           f'</svg>')
    lock = logo("ladiani-lockup-reversed", "lock")
    return f"""<!doctype html><meta charset="utf-8"><style>{CSS}
.stage {{ position:relative; width:{W}px; height:{H}px; }}
.lock {{ position:absolute; width:{lock_w}px; height:auto; left:{(W-lock_w)/2}px; top:{H*0.47 - lock_h/2}px; }}
</style><div class="stage">{art}{lock}</div>"""


def watermark(size=150):
    """Branding watermark: one-colour symbol on transparent, as YouTube asks."""
    pad = size * 0.12
    return f"""<!doctype html><meta charset="utf-8"><style>
* {{ margin:0; padding:0; }} html, body {{ background:transparent; overflow:hidden; }}
.wm {{ width:{size}px; height:{size}px; display:grid; place-items:center; padding:{pad}px; box-sizing:border-box; }}
.wm svg {{ height:100%; width:auto; display:block; }}
</style><div class="wm">{logo("ladiani-symbol-reversed").replace("#e7ebe8", "#ffffff")}</div>"""


def guides():
    """Banner with YouTube's safe areas drawn on top, for checking crops."""
    W, H = 2560, 1440
    html = banner()
    over = (f'<svg style="position:absolute;inset:0" width="{W}" height="{H}">'
            f'<rect x="{(W-1546)/2}" y="{(H-423)/2}" width="1546" height="423" fill="none" stroke="#e05a4a" stroke-width="4" stroke-dasharray="16 10"/>'
            f'<rect x="2" y="{(H-423)/2}" width="{W-4}" height="423" fill="none" stroke="#e0b44a" stroke-width="4" stroke-dasharray="16 10"/>'
            f'<text x="{(W-1546)/2 + 16}" y="{(H-423)/2 + 40}" fill="#e05a4a" font-size="30" font-family="Menlo">celular · 1546×423 (área segura)</text>'
            f'<text x="20" y="{(H-423)/2 - 16}" fill="#e0b44a" font-size="30" font-family="Menlo">computador · 2560×423</text>'
            f'<text x="20" y="44" fill="#88938c" font-size="30" font-family="Menlo">TV · 2560×1440</text></svg>')
    return html[:-len("</div>")] + over + "</div>"


def shoot(browser, html, w, h, path, transparent=False):
    with tempfile.TemporaryDirectory() as tmp:
        f = os.path.join(tmp, "p.html")
        open(f, "w").write(html)
        subprocess.run([browser, "--headless=new", "--disable-gpu", "--hide-scrollbars",
                        f"--user-data-dir={tmp}/profile", f"--window-size={w},{h}",
                        "--force-device-scale-factor=1", "--virtual-time-budget=3000",
                        *(["--default-background-color=00000000"] if transparent else []),
                        f"--screenshot={path}", "file://" + f],
                       capture_output=True, timeout=120, check=True)


def main():
    browser = next((b for b in BROWSERS if os.path.exists(b)), None)
    if not browser:
        raise SystemExit("No Chromium-based browser found (Edge, Chrome or Chromium).")
    os.makedirs(OUT, exist_ok=True)
    shoot(browser, banner(), 2560, 1440, os.path.join(OUT, "ladiani-youtube-banner-2560x1440.png"))
    shoot(browser, guides(), 2560, 1440, os.path.join(OUT, "guias-banner-areas-seguras.png"))
    shoot(browser, thumb_brand(), 1280, 720, os.path.join(OUT, "ladiani-youtube-thumb-1280x720.png"))
    # Product thumbnail, kept for launch day.
    shoot(browser, thumb(), 1280, 720, os.path.join(OUT, "lancamento-velora-thumb-1280x720.png"))
    for size in (150, 600):
        shoot(browser, watermark(size), size, size,
              os.path.join(OUT, f"ladiani-youtube-marca-dagua-{size}x{size}.png"), transparent=True)
    print("wrote", sorted(os.listdir(OUT)))


if __name__ == "__main__":
    main()
