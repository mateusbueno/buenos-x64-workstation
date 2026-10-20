#!/usr/bin/env python3
"""Render the outlined SVG masters to transparent PNGs in ./png/.

Uses a headless Chromium browser (Edge or Chrome) as the rasterizer: one page
draws every SVG onto a canvas and the PNG data comes back through --dump-dom.
Run build_logo.py --outline first.
"""
import base64
import os
import re
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "outlined")
OUT = os.path.join(HERE, "png")

BROWSERS = [
    "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "/Applications/Chromium.app/Contents/MacOS/Chromium",
]

# name -> ("w" | "h", sizes): sizes in px along that axis.
WIDE = ("w", [600, 1200, 2400])
JOBS = {}
for v in ("ink", "reversed"):
    JOBS[f"ladiani-lockup-{v}"] = WIDE
    JOBS[f"ladiani-velora73-panel-{v}"] = WIDE
    JOBS[f"ladiani-wordmark-{v}"] = WIDE
    JOBS[f"velora-wordmark-{v}"] = WIDE
    JOBS[f"ladiani-symbol-{v}"] = ("h", [128, 512, 1024])
JOBS["ladiani-app-icon"] = ("w", [16, 32, 64, 128, 256, 512, 1024])
JOBS["ladiani-avatar-paper"] = ("w", [400, 1000])
JOBS["ladiani-avatar-dark"] = ("w", [98, 400, 800, 1000])
JOBS["ladiani-favicon-16"] = ("w", [16, 32])
JOBS["ladiani-favicon-32"] = ("w", [32, 64])
JOBS["ladiani-symbol-16"] = ("w", [16, 32, 64])
JOBS["ladiani-symbol-32"] = ("w", [32, 64, 128])


def main():
    browser = next((b for b in BROWSERS if os.path.exists(b)), None)
    if not browser:
        raise SystemExit("No Chromium-based browser found (Edge, Chrome or Chromium).")
    items = []
    for name, (axis, sizes) in JOBS.items():
        svg = open(os.path.join(SRC, name + ".svg")).read()
        vb = [float(x) for x in re.search(r'viewBox="([^"]+)"', svg).group(1).split()]
        ratio = vb[2] / vb[3]
        for s in sizes:
            w, h = (s, s / ratio) if axis == "w" else (s * ratio, s)
            w, h = round(w), round(h)
            # Give the SVG explicit pixel size so the canvas draw is exact.
            sized = svg.replace("<svg ", f'<svg width="{w}" height="{h}" ', 1)
            items.append({"file": f"{name}-{w}x{h}.png", "w": w, "h": h,
                          "src": "data:image/svg+xml;base64," + base64.b64encode(sized.encode()).decode()})

    js_items = ",".join(
        f'{{f:"{i["file"]}",w:{i["w"]},h:{i["h"]},s:"{i["src"]}"}}' for i in items)
    page = f"""<!doctype html><meta charset="utf-8"><body><pre id="out"></pre><script>
const items=[{js_items}];
(async()=>{{const lines=[];for(const it of items){{const img=new Image();img.src=it.s;await img.decode();
const c=document.createElement("canvas");c.width=it.w;c.height=it.h;
c.getContext("2d").drawImage(img,0,0,it.w,it.h);lines.push(it.f+" "+c.toDataURL("image/png"));}}
document.getElementById("out").textContent=lines.join("\\n");document.title="done";}})();
</script>"""
    with tempfile.TemporaryDirectory() as tmp:
        html = os.path.join(tmp, "render.html")
        open(html, "w").write(page)
        dom = subprocess.run(
            [browser, "--headless=new", "--disable-gpu", f"--user-data-dir={tmp}/profile",
             "--virtual-time-budget=20000", "--dump-dom", "file://" + html],
            capture_output=True, text=True, timeout=180).stdout
    found = re.findall(r"([\w.-]+\.png) data:image/png;base64,([A-Za-z0-9+/=]+)", dom)
    if len(found) != len(items):
        raise SystemExit(f"Rendered {len(found)} of {len(items)} images; browser output was incomplete.")
    os.makedirs(OUT, exist_ok=True)
    for fname, b64 in found:
        open(os.path.join(OUT, fname), "wb").write(base64.b64decode(b64))
    print(f"wrote {len(found)} PNGs to {OUT}")


if __name__ == "__main__":
    main()
