#!/usr/bin/env python3
"""Stealth-mode holding page for ladiani: static, no external requests, no cookies.

Writes ./site/ (upload its contents to the web root). Uses the outlined logo
masters and a headless Chromium browser for the PNG icons and share image.

Usage: python3 make_site.py [https://your-domain.com]
The URL makes og:image absolute, which Facebook, LinkedIn and WhatsApp need.
"""
import os
import re
import shutil
import sys

import make_youtube as Y

HERE = os.path.dirname(os.path.abspath(__file__))
SITE = os.path.join(HERE, "site")
URL = (sys.argv[1].rstrip("/") if len(sys.argv) > 1 else "")

TITLE = "Ladiani Music"
DESCRIPTION = "Ladiani Music. Coming soon."


def lockup_inline():
    """Lockup with currentColor and hooks for the boot animation."""
    s = open(os.path.join(HERE, "outlined", "ladiani-lockup-reversed.svg")).read().strip()
    s = re.sub(r"<title>.*?</title>", "", s).replace('"#e7ebe8"', '"currentColor"')
    s = s.replace('role="img" aria-label="Ladiani"', 'class="mark" aria-hidden="true" focusable="false"')
    s = s.replace('<g fill="currentColor"><rect', '<g class="bars" fill="currentColor"><rect', 1)
    s = s.replace('<g transform="translate(214 0)"', '<g class="word" transform="translate(214 0)"', 1)
    return s


def page():
    og = f"{URL}/og-image.png" if URL else "og-image.png"
    canonical = f'\n<link rel="canonical" href="{URL}/">' if URL else ""
    og_url = f'\n<meta property="og:url" content="{URL}/">' if URL else ""
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>{TITLE}</title>
<meta name="description" content="{DESCRIPTION}">
<meta name="theme-color" content="#0b0d0c">
<meta name="color-scheme" content="dark">{canonical}
<link rel="icon" href="favicon.svg" type="image/svg+xml">
<link rel="icon" href="favicon-32.png" sizes="32x32" type="image/png">
<link rel="apple-touch-icon" href="apple-touch-icon.png">
<meta property="og:type" content="website">
<meta property="og:site_name" content="{TITLE}">
<meta property="og:title" content="{TITLE}">
<meta property="og:description" content="Coming soon.">{og_url}
<meta property="og:image" content="{og}">
<meta property="og:image:width" content="1200">
<meta property="og:image:height" content="630">
<meta name="twitter:card" content="summary_large_image">
<style>
  :root {{
    --bg: #0b0d0c; --glow: #1b221e; --text: #e7ebe8; --muted: #88938c;
    --f: "Gill Sans", "Gill Sans MT", Seravek, "Segoe UI", "Helvetica Neue", Arial, sans-serif;
    color-scheme: dark;
  }}
  * {{ margin: 0; box-sizing: border-box; }}
  html, body {{ height: 100%; }}
  body {{
    background: radial-gradient(ellipse 60% 55% at 50% 47%, var(--glow), var(--bg) 70%) var(--bg);
    color: var(--text); font-family: var(--f);
    display: grid; grid-template-rows: 1fr auto; min-height: 100svh;
    padding: env(safe-area-inset-top) max(24px, env(safe-area-inset-right))
             env(safe-area-inset-bottom) max(24px, env(safe-area-inset-left));
  }}
  main {{ display: grid; place-content: center; justify-items: center; gap: clamp(36px, 7vh, 64px); }}
  h1 {{ position: absolute; width: 1px; height: 1px; overflow: hidden; clip-path: inset(50%); }}
  .mark {{ width: clamp(220px, 48vw, 560px); height: auto; display: block; }}
  .soon {{
    font-size: clamp(12px, 1.4vw, 15px); font-weight: 300; letter-spacing: .42em;
    text-transform: uppercase; color: var(--muted); padding-left: .42em;
  }}
  footer {{ text-align: center; padding-block: 28px; font-size: 12px; letter-spacing: .08em; color: var(--muted); }}

  /* Boot: the drawbars are pulled out one by one, then the name and the line appear. */
  .bars rect {{ transform-box: fill-box; transform-origin: bottom; animation: pull .8s cubic-bezier(.2,.8,.2,1) both; }}
  .bars rect:nth-child(2) {{ animation-delay: .3s }}  .bars rect:nth-child(3) {{ animation-delay: .42s }}
  .bars rect:nth-child(4) {{ animation-delay: .54s }} .bars rect:nth-child(5) {{ animation-delay: .66s }}
  .word {{ animation: fade 1.4s 1.1s both; }}
  .soon {{ animation: fade 1.4s 1.9s both; }}
  @keyframes pull {{ from {{ transform: scaleY(.06) }} to {{ transform: scaleY(1) }} }}
  @keyframes fade {{ from {{ opacity: 0 }} to {{ opacity: 1 }} }}
  @media (prefers-reduced-motion: reduce) {{ .bars rect, .word, .soon {{ animation: none; }} }}
</style>
</head>
<body>
<main>
  <h1>Ladiani Music</h1>
  {lockup_inline()}
  <p class="soon">Coming soon</p>
</main>
<footer>© 2026 Ladiani Music</footer>
</body>
</html>
"""


def og_image():
    W, H = 1200, 630
    lock_w = 640
    lock_h = lock_w * 171 / 743
    return f"""<!doctype html><meta charset="utf-8"><style>
* {{ margin:0; }} html, body {{ width:{W}px; height:{H}px; overflow:hidden;
  background: radial-gradient(ellipse 60% 60% at 50% 46%, #1d2520, #0b0d0c 72%) #0b0d0c; }}
.l {{ position:absolute; width:{lock_w}px; left:{(W-lock_w)/2}px; top:{H*0.44 - lock_h/2}px; color:#e7ebe8; }}
.s {{ position:absolute; width:100%; top:{H*0.44 + lock_h/2 + 56}px; text-align:center;
  font: 300 20px "Gill Sans","Gill Sans MT",sans-serif; letter-spacing:.42em; color:#88938c; text-transform:uppercase; padding-left:.42em; }}
</style><div class="l">{lockup_inline().replace('class="mark"', 'width="100%"')}</div><p class="s">Coming soon</p>"""


def icon(size):
    s = open(os.path.join(HERE, "outlined", "ladiani-app-icon.svg")).read()
    s = s.replace("<svg ", f'<svg width="{size}" height="{size}" ', 1)
    # iOS rounds the corners itself: square, full-bleed background.
    s = s.replace('rx="230"', 'rx="0"')
    return f"<!doctype html><style>*{{margin:0}}svg{{display:block}}</style>{s}"


def main():
    browser = next((b for b in Y.BROWSERS if os.path.exists(b)), None)
    if not browser:
        raise SystemExit("No Chromium-based browser found (Edge, Chrome or Chromium).")
    os.makedirs(SITE, exist_ok=True)
    open(os.path.join(SITE, "index.html"), "w").write(page())
    shutil.copy(os.path.join(HERE, "outlined", "ladiani-favicon-32.svg"), os.path.join(SITE, "favicon.svg"))
    shutil.copy(os.path.join(HERE, "png", "ladiani-favicon-32-32x32.png"), os.path.join(SITE, "favicon-32.png"))
    Y.shoot(browser, icon(180), 180, 180, os.path.join(SITE, "apple-touch-icon.png"))
    Y.shoot(browser, og_image(), 1200, 630, os.path.join(SITE, "og-image.png"))
    open(os.path.join(SITE, "robots.txt"), "w").write("User-agent: *\nAllow: /\n")
    print("wrote", sorted(os.listdir(SITE)), "| og:image", "absolute" if URL else "relative (pass your URL)")


if __name__ == "__main__":
    main()
