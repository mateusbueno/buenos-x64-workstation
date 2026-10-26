#!/usr/bin/env python3
"""Story 6.11: parse files/wic/velora-nvme.wks with wic's own kickstart parser and check the layout.
Usage: check_wks.py <wic-src-dir> <wks> <bitbake-env-dump of velora-image-dev>
Every ${VAR} in the wks must be listed in the resolved WICVARS (so wic can expand it at build time); the values from the env dump are
substituted before parsing, so the parse sees what a real build sees (no bitbake needed)."""
import re
import sys
import tempfile
from pathlib import Path

wic_src, wks, envdump = sys.argv[1:4]
sys.path.insert(0, wic_src)
errors = []

env = {}
for line in Path(envdump).read_text(errors="replace").splitlines():
    m = re.match(r'^([A-Za-z_][A-Za-z0-9_]*)="(.*)"$', line)
    if m:
        env.setdefault(m.group(1), m.group(2))
wicvars = set(env.get("WICVARS", "").split())
src = Path(wks).read_text()
for var in sorted(set(re.findall(r"\$\{([^{}@\n\t :]+)\}", src))):
    if var not in wicvars:
        errors.append(f"wks uses ${{{var}}} but it is not in WICVARS ({sorted(wicvars)})")
    elif var not in env:
        errors.append(f"wks variable {var} is in WICVARS but has no value in the env dump")
    else:
        # RK_UBOOT_ENV may legitimately be a single space
        src = src.replace("${%s}" % var, env[var])
with tempfile.NamedTemporaryFile("w", suffix=".wks", delete=False) as f:
    f.write(src)
    expanded = f.name

try:
    import wic.ksparser as ksmod
    from wic.ksparser import KickStart
    ksmod.get_bitbake_var = lambda var, *a, **k: env.get(var)  # no bitbake here: answer from the env dump
    ks = KickStart(expanded)
except Exception as e:  # parse failure is a check failure
    print(f"wks parse FAILED: {type(e).__name__}: {e}")
    sys.exit(1)

parts = ks.partitions
names = [p.part_name for p in parts]
by_name = {p.part_name: p for p in parts}


def need(cond, msg):
    if not cond:
        errors.append(msg)


need(ks.bootloader.ptable == "gpt", "bootloader ptable must be gpt")
need(len(set(names)) == len(names), f"duplicate partition names: {names}")
# Boot area: offset (sectors of 512 B) and size (KiB), copied from meta-rockchip's files/wic/rockchip.wks at the commit pinned in
# kas/velora-dev.yml (itself following https://opensource.rock-chips.com/wiki_Partitions). Changing them needs a boot-chain decision.
BOOT_AREA = {"loader1": (64, 3552), "v_storage": (7168, 256), "reserved": (7680, 192), "reserved1": (8064, 32),
             "uboot_env": (8128, 32), "reserved2": (8192, 4096), "loader2": (16384, 4096), "atf": (24576, 4096)}
for n, (off, kib) in BOOT_AREA.items():
    p = by_name.get(n)
    if p is None:
        errors.append(f"boot area partition {n} missing")
        continue
    need(p.offset == off * 512, f"boot area partition {n}: offset {p.offset} bytes, expected {off * 512}")
    need(p.fixed_size == kib, f"boot area partition {n}: size {p.fixed_size} KiB, expected {kib}")
    need(p.fstype == "none", f"boot area partition {n} must be fstype none")
# monotonic order and no overlap (partitions without --offset follow the previous one)
cursor = 0
for p in parts:
    start = p.offset if p.offset is not None else cursor
    need(start >= cursor, f"partition {p.part_name} starts at {start} before the end of the previous partition ({cursor}): overlap or wrong order")
    cursor = start + (p.fixed_size or 0) * 1024
need(not any(p.fstype == "swap" or (p.mountpoint or "") == "swap" for p in parts), "wks contains a swap partition")
a, b = by_name.get("rootfs.0"), by_name.get("rootfs.1")
need(a is not None and b is not None, "root slots rootfs.0 / rootfs.1 missing")
if a and b:
    need(a.fixed_size and a.fixed_size == b.fixed_size, f"root slots must have identical fixed size ({a.fixed_size} vs {b.fixed_size})")
    need(a.mountpoint == "/" and a.fstype == "ext4" and a.source == "rootfs", "slot A must be '/' ext4 populated from the rootfs")
    need(a.label == "rootfs.0", "slot A ext4 label must be rootfs.0 (slot B is fstype none: only its GPT name rootfs.1 is meaningful)")
    need(b.source is None and not b.mountpoint and not b.rootfs_dir and not b.include_path, "slot B must be unpopulated (no source, no mountpoint)")
    need(a.part_type and a.part_type == b.part_type, "slot A and B must share the root partition type")
    need(names.index("rootfs.0") < names.index("rootfs.1") < names.index("data") if "data" in by_name else False, "order must be rootfs.0, rootfs.1, data")
    need(sum(1 for p in parts if p.mountpoint == "/") == 1, "exactly one partition may be mounted at /")
d = [p for p in parts if p.mountpoint == "/data"]
need(len(d) == 1, f"exactly one /data partition required, found {len(d)}")
if d:
    d = d[0]
    opts = (d.fsopts or "").split(",")
    need(d.fstype == "ext4", f"/data must be ext4, is {d.fstype}")
    need("data=ordered" in opts, f"/data fsoptions must include data=ordered: {d.fsopts}")
    for o in ("nodev", "nosuid"):
        need(o in opts, f"/data fsoptions must include {o}: {d.fsopts}")
    need(d.fixed_size, "/data needs an explicit size")
    need("has_journal" in (d.mkfs_extraopts or ""), "/data must create an explicit ext4 journal (-O has_journal)")
extroot = env.get("UBOOT_EXTLINUX_ROOT")
need(a is not None and extroot == f"root=PARTLABEL={a.part_name}", f"UBOOT_EXTLINUX_ROOT ({extroot!r}) must be root=PARTLABEL=<slot A GPT name>")
need(len(parts) == len([p for p in parts if p.disk == "nvme0n1"]), "every partition must be on nvme0n1")

if errors:
    print("wks checks FAILED:")
    for e in errors:
        print("  -", e)
    sys.exit(1)
print(f"wks checks: ok ({len(parts)} partitions: {', '.join(names)}; slot size {a.fixed_size} KiB, data {d.fixed_size} KiB)")
