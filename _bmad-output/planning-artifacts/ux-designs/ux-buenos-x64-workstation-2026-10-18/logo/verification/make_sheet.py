#!/usr/bin/env python3
"""Story 11.16 verification sheet: renders the outlined masters in the four required contexts."""
import os, subprocess, tempfile
H = os.path.dirname(os.path.abspath(__file__)); L = os.path.dirname(H)
O = "file://" + L + "/outlined/"
E = "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge"
html = f"""<!doctype html><meta charset=utf-8><style>
body{{margin:0;width:1400px;background:#fff;font:13px monospace;color:#333}}
h2{{margin:14px 20px 6px;font-size:14px}} .row{{display:flex;gap:20px;padding:0 20px 14px;align-items:center}}
.alu{{background:linear-gradient(135deg,#c9ccca,#aeb3b0 50%,#9aa09c);padding:28px 36px;border-radius:6px}}
.alu img{{filter:none;opacity:.88}} .dark{{background:#0b0d0c;padding:28px 36px;border-radius:6px}}
.tiny{{display:flex;gap:18px;align-items:end;background:#fff;border:1px solid #ddd;padding:14px;border-radius:6px}}
.tiny div{{text-align:center}} .z{{image-rendering:pixelated}} .av{{width:140px;height:140px;border-radius:50%;object-fit:cover}}
</style>
<h2>1. Engraved / silkscreened single colour on aluminium (panel ink #2b322e at 88%)</h2>
<div class=row><div class=alu><img src="{O}ladiani-velora73-panel-ink.svg" height=130></div>
<div class=alu><img src="{O}ladiani-symbol-ink.svg" height=130></div><div class=alu><img src="{O}ladiani-wordmark-ink.svg" height=48></div></div>
<h2>2. Dark boot screen (bg #0b0d0c)</h2>
<div class=row><div class=dark><img src="{O}ladiani-lockup-reversed.svg" height=70></div>
<div class=dark><img src="{O}ladiani-velora73-panel-reversed.svg" height=110></div></div>
<h2>3. Tiny icon: delivered app-icon PNGs 16 / 32 / 48 / 64 px actual size, and zoom (16 and 32 px use the simplified symbol)</h2>
<div class=row><div class=tiny>
<div><img src="../app-icon/linux/ladiani-16x16.png"><br>16</div><div><img src="../app-icon/linux/ladiani-32x32.png"><br>32</div><div><img src="../app-icon/linux/ladiani-48x48.png"><br>48</div><div><img src="../app-icon/linux/ladiani-64x64.png"><br>64</div></div>
<div class=tiny><img class=z src="../app-icon/linux/ladiani-16x16.png" width=96><img class=z src="../app-icon/linux/ladiani-32x32.png" width=96><img class=z src="../app-icon/linux/ladiani-48x48.png" width=96></div></div>
<h2>4. Round avatar crop (140 px and 98 px)</h2>
<div class=row><img class=av src="../png/ladiani-avatar-paper-400x400.png"><img class=av src="../png/ladiani-avatar-dark-400x400.png">
<img class=av style="width:98px;height:98px" src="../png/ladiani-avatar-dark-400x400.png"></div>"""
with tempfile.TemporaryDirectory() as t:
    p = os.path.join(H, "_sheet.html"); open(p, "w").write(html)
    subprocess.run([E, "--headless=new", "--disable-gpu", f"--user-data-dir={t}/p", "--virtual-time-budget=8000",
                    "--window-size=1400,1000", "--allow-file-access-from-files",
                    f"--screenshot={H}/logo-contexts-sheet.png", "file://" + p], capture_output=True)
    os.remove(p)
