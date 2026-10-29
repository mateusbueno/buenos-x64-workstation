#!/usr/bin/env python3
"""Delivery assets for Story 11.16 built from the outlined masters:
PDF masters (pdf/), desktop icon sets (app-icon/: .icns, .ico, Linux PNGs)
and boot artwork 1024x600 with drawbar pull-out frames (boot/).
Run build_logo.py --outline and render_png.py first. Needs Edge/Chrome, iconutil, Pillow.
"""
import base64, glob, math, os, re, shutil, subprocess, tempfile
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "outlined")
PNG = os.path.join(HERE, "png")
BROWSER = "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge"
BG, FG = "#0b0d0c", "#e7ebe8"
W, H, FPS = 1024, 600, 30


def browser(args, tmp):
    return subprocess.run([BROWSER, "--headless=new", "--disable-gpu",
                           f"--user-data-dir={tmp}/profile", *args],
                          capture_output=True, text=True, timeout=240)


def pdfs():
    out = os.path.join(HERE, "pdf"); os.makedirs(out, exist_ok=True)
    for f in sorted(glob.glob(os.path.join(SRC, "*.svg"))):
        name = os.path.basename(f)[:-4]
        svg = open(f).read()
        vb = [float(x) for x in re.search(r'viewBox="([^"]+)"', svg).group(1).split()]
        w_mm = 200.0; h_mm = w_mm * vb[3] / vb[2]
        sized = svg.replace("<svg ", '<svg width="100%" height="100%" ', 1)
        page = (f'<!doctype html><meta charset="utf-8"><style>@page{{size:{w_mm}mm {h_mm:.3f}mm;margin:0}}'
                f'html,body{{margin:0;width:{w_mm}mm;height:{h_mm:.3f}mm;overflow:hidden}}'
                f'svg{{display:block}}</style>{sized}')
        with tempfile.TemporaryDirectory() as tmp:
            p = os.path.join(tmp, "p.html"); open(p, "w").write(page)
            browser(["--no-pdf-header-footer", f"--print-to-pdf={out}/{name}.pdf", "file://" + p], tmp)
    print("pdf:", len(os.listdir(out)))


def icons():
    out = os.path.join(HERE, "app-icon"); os.makedirs(out, exist_ok=True)
    full = lambda s: os.path.join(PNG, f"ladiani-app-icon-{s}x{s}.png")
    # DESIGN.md: the symbol simplifies at small sizes (1 tall + 2 short at 32 px, 1 tall + 1 short at 16 px),
    # so icon sizes up to 32 px use the favicon masters instead of the 5-bar app icon.
    small = {16: os.path.join(PNG, "ladiani-favicon-16-16x16.png"), 32: os.path.join(PNG, "ladiani-favicon-32-32x32.png")}
    def p(s):
        return small.get(s, full(s))
    with tempfile.TemporaryDirectory() as tmp:
        iset = os.path.join(tmp, "Ladiani.iconset"); os.makedirs(iset)
        for base, s, s2 in [(16, 16, 32), (32, 32, 64), (128, 128, 256), (256, 256, 512), (512, 512, 1024)]:
            shutil.copy(p(s), f"{iset}/icon_{base}x{base}.png")
            shutil.copy(p(s2), f"{iset}/icon_{base}x{base}@2x.png")
        subprocess.run(["iconutil", "-c", "icns", iset, "-o", f"{out}/ladiani.icns"], check=True)
    # Pillow drops sizes it must resample from a single image, so build each ICO frame explicitly.
    f24 = Image.open(os.path.join(PNG, "ladiani-favicon-32-32x32.png")).convert("RGBA")
    f24 = Image.open(os.path.join(PNG, "ladiani-favicon-32-64x64.png")).convert("RGBA").resize((24, 24), Image.LANCZOS)
    frames = [Image.open(p(s)).convert("RGBA") for s in (16, 32, 64, 128, 256)]
    frames.insert(1, f24)
    frames.insert(3, Image.open(full(1024)).convert("RGBA").resize((48, 48), Image.LANCZOS))
    frames.sort(key=lambda i: i.width)
    frames[-1].save(f"{out}/ladiani.ico", sizes=[(i.width, i.height) for i in frames], append_images=frames[:-1])
    lin = os.path.join(out, "linux"); os.makedirs(lin, exist_ok=True)
    for s in (16, 32, 48, 64, 128, 256, 512):
        src = Image.open(p(1024)).convert("RGBA") if s == 48 else Image.open(p(s))
        (src.resize((s, s), Image.LANCZOS) if s == 48 else src).save(f"{lin}/ladiani-{s}x{s}.png")
    print("icons ok")


def ease(t):
    t = min(max(t, 0), 1); return 1 - (1 - t) ** 3


def boot():
    out = os.path.join(HERE, "boot"); os.makedirs(out, exist_ok=True)
    wm = open(os.path.join(SRC, "velora-wordmark-reversed.svg")).read()
    inner = re.search(r"<svg[^>]*>(.*)</svg>", wm, re.S).group(1)
    inner = re.sub(r"<title>.*?</title>", "", inner)
    vb = [float(x) for x in re.search(r'viewBox="([^"]+)"', wm).group(1).split()]
    ww = 300.0; wh = ww * vb[3] / vb[2]
    # Symbol: tall bar + 4 short bars, 162x170 units, scaled x1.6 and centred a little above middle.
    sc = 1.6; sw, sh = 162 * sc, 170 * sc
    sx, sy = (W - sw) / 2, 120
    bars = [(0, -70, 170), (35, 50, 50), (70, 50, 50), (105, 50, 50), (140, 50, 50)]
    dur, stag, fade0, fade1 = 0.7, 0.15, 1.3, 1.8
    total = 2.4
    frames = []
    for i in range(round(total * FPS)):
        t = i / FPS; g = ""
        for k, (x, y, h) in enumerate(bars):
            pull = ease((t - k * stag) / dur)
            # Drawbars slide out toward the player: the bar grows upward from its base at y=100.
            hh = max(h * pull, 0.0)
            if hh > 0.5:
                g += f'<rect x="{x}" y="{100 - hh}" width="22" height="{hh}" rx="11"/>'
        a = ease((t - fade0) / (fade1 - fade0))
        svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">'
               f'<rect width="{W}" height="{H}" fill="{BG}"/>'
               f'<g transform="translate({sx} {sy}) scale({sc}) translate(0 70)" fill="{FG}">{g}</g>'
               f'<svg x="{(W - ww) / 2}" y="{sy + sh + 50}" width="{ww}" height="{wh}" viewBox="{vb[0]} {vb[1]} {vb[2]} {vb[3]}" opacity="{a:.3f}">{inner}</svg></svg>')
        frames.append(svg)
    items = ",".join('"data:image/svg+xml;base64,' + base64.b64encode(s.encode()).decode() + '"' for s in frames)
    page = f"""<!doctype html><meta charset="utf-8"><pre id="o"></pre><script>
const it=[{items}];(async()=>{{const L=[];for(let i=0;i<it.length;i++){{const im=new Image();im.src=it[i];await im.decode();
const c=document.createElement("canvas");c.width={W};c.height={H};c.getContext("2d").drawImage(im,0,0);L.push(i+" "+c.toDataURL("image/png"));}}
document.getElementById("o").textContent=L.join("\\n");}})();</script>"""
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, "b.html"); open(p, "w").write(page)
        dom = browser(["--virtual-time-budget=60000", "--dump-dom", "file://" + p], tmp).stdout
    found = re.findall(r"(\d+) data:image/png;base64,([A-Za-z0-9+/=]+)", dom)
    assert len(found) == len(frames), f"{len(found)}/{len(frames)}"
    fr = os.path.join(out, "frames"); os.makedirs(fr, exist_ok=True)
    for i, b in found:
        open(f"{fr}/boot-{int(i):03d}.png", "wb").write(base64.b64decode(b))
    open(f"{out}/boot-static-1024x600.svg", "w").write(frames[-1])
    shutil.copy(f"{fr}/boot-{len(frames) - 1:03d}.png", f"{out}/boot-static-1024x600.png")
    open(f"{out}/README.md", "w").write(
        f"# Boot artwork\n\n{len(frames)} frames at {FPS} fps, {W}x{H}, bg {BG}. Drawbars pull out one by one "
        f"(~{dur}s ease-out, {stag}s stagger), then VELORA fades in ({fade0}-{fade1}s).\n"
        "`boot-static-1024x600.*` is the final frame, used under reduced motion (UX-DR111).\n")
    print("boot frames:", len(found))


if __name__ == "__main__":
    pdfs(); icons(); boot()
