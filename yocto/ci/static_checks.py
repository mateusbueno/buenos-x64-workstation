#!/usr/bin/env python3
"""Story 1.4 static checks of the committed yocto/ files (no bitbake needed). Usage: static_checks.py <yocto-dir>"""
import re
import sys
from pathlib import Path

root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent
errors = []


def text(rel):
    p = root / rel
    if not p.is_file():
        errors.append(f"missing file: {rel}")
        return ""
    return p.read_text()


def need(desc, rel, pattern, flags=re.M):
    if not re.search(pattern, text(rel), flags):
        errors.append(f"{desc}: /{pattern}/ not found in {rel}")


def forbid(desc, rel, pattern, flags=re.M):
    if re.search(pattern, text(rel), flags):
        errors.append(f"{desc}: /{pattern}/ found in {rel}")


M = "meta-velora"
D = f"{M}/conf/distro/velora.conf"
CFG = f"{M}/recipes-core/velora-system-config/files"
IMG = f"{M}/recipes-core/images/velora-image-dev.bb"

# os-release dev marker
need("os-release dev marker", D, r'^VARIANT_ID = "\$\{VELORA_VARIANT\}"')
need("os-release dev default", D, r'^VELORA_VARIANT \?= "dev"')
need("os-release fields", D, r'^OS_RELEASE_FIELDS:append = ".*VARIANT_ID')
# no swap
need("swappiness 0", f"{CFG}/50-velora-sysctl.conf", r"^vm\.swappiness\s*=\s*0\s*$")
need("kernel without swap", f"{M}/recipes-kernel/linux/files/velora-dev.cfg", r"^# CONFIG_SWAP is not set$")
need("swap.target masked", f"{M}/recipes-core/velora-system-config/velora-system-config_1.0.bb", r"ln -sf /dev/null .*swap\.target")
for p in root.rglob("*"):
    if not p.is_file() or ".yocto-work" in p.parts:
        continue
    if p.name.endswith((".wks", ".wks.in", ".fstab")) or p.name.startswith("fstab"):
        for line in p.read_text().splitlines():
            if re.search(r"\bswap\b", line) and not line.lstrip().startswith("#"):
                errors.append(f"swap entry in {p.relative_to(root)}: {line.strip()}")
# the install recipe ties each source file to its destination
REC = f"{M}/recipes-core/velora-system-config/velora-system-config_1.0.bb"
for src, dst in (
    ("50-velora-sysctl.conf", r"\$\{D\}\$\{sysconfdir\}/sysctl\.d/50-velora\.conf"),
    ("10-velora-coredump.conf", r"\$\{D\}\$\{sysconfdir\}/systemd/coredump\.conf\.d/10-velora\.conf"),
    ("10-velora-limits.conf", r"\$\{D\}\$\{sysconfdir\}/systemd/system\.conf\.d/10-velora\.conf"),
    ("10-velora-journal.conf", r"\$\{D\}\$\{sysconfdir\}/systemd/journald\.conf\.d/10-velora\.conf"),
    ("20-velora-wired.network", r"\$\{D\}\$\{sysconfdir\}/systemd/network/20-velora-wired\.network"),
):
    need(f"{src} installed to its destination", REC, r"install -m 0644 \$\{UNPACKDIR\}/" + re.escape(src) + " " + dst)
    need(f"{src} listed in SRC_URI", REC, r"file://" + re.escape(src))
need("swap.target symlink to /dev/null", REC, r"ln -sf /dev/null \$\{D\}\$\{sysconfdir\}/systemd/system/swap\.target")
need("FILES covers sysconfdir", REC, r'^FILES:\$\{PN\} = "\$\{sysconfdir\}"')
need("kernel fragment owns CONFIG_EXPERT", f"{M}/recipes-kernel/linux/files/velora-dev.cfg", r"^CONFIG_EXPERT=y$")
# bounded RAM journal
need("journal volatile", f"{CFG}/10-velora-journal.conf", r"^Storage=volatile$")
need("journal RuntimeMaxUse", f"{CFG}/10-velora-journal.conf", r"^RuntimeMaxUse=\d+[KMG]$")
need("journal RuntimeMaxFileSize", f"{CFG}/10-velora-journal.conf", r"^RuntimeMaxFileSize=\d+[KMG]$")
need("networkd DHCP", f"{CFG}/20-velora-wired.network", r"^DHCP=yes$")
# every Velora image applies the policy
imgs = list((root / M).rglob("velora-image-*.bb"))
if not imgs:
    errors.append("no velora-image-*.bb found")
for img in imgs:
    rel = str(img.relative_to(root))
    need("image applies licence policy", rel, r'^INCOMPATIBLE_LICENSE = "\$\{VELORA_INCOMPATIBLE_LICENSE\}"')
    need("image applies licence exceptions", rel, r'^INCOMPATIBLE_LICENSE_EXCEPTIONS = "\$\{VELORA_INCOMPATIBLE_LICENSE_EXCEPTIONS\}"')
need("authorized keys bbfatal when missing", IMG, r"bbfatal .*VELORA_DEV_AUTHORIZED_KEYS_FILE")
need("authorized keys file-checksums", IMG, r"^do_rootfs\[file-checksums\]")
# core dumps disabled
need("core_pattern", f"{CFG}/50-velora-sysctl.conf", r"^kernel\.core_pattern\s*=\s*\|/bin/false\s*$")
need("suid_dumpable", f"{CFG}/50-velora-sysctl.conf", r"^fs\.suid_dumpable\s*=\s*0\s*$")
need("coredump Storage=none", f"{CFG}/10-velora-coredump.conf", r"^Storage=none$")
need("DefaultLimitCORE=0", f"{CFG}/10-velora-limits.conf", r"^DefaultLimitCORE=0$")
# ssh + debug uart, dev image only
IMG = f"{M}/recipes-core/images/velora-image-dev.bb"
need("ssh server", IMG, r'^IMAGE_FEATURES = "ssh-server-openssh"')
forbid("insecure root login in image features", IMG, r'^IMAGE_FEATURES(\s*\+?=|:append)[^\n]*(debug-tweaks|empty-root-password|allow-empty-password)')
need("debug uart getty", D, r'^SERIAL_CONSOLES = "\d+;ttyS\d+"')
# distro
need("systemd init manager", D, r'^INIT_MANAGER = "systemd"')
need("licence policy", D, r'^VELORA_INCOMPATIBLE_LICENSE = "GPL-3\.0\* LGPL-3\.0\* AGPL-3\.0\*"')
forbid("no licence exceptions expected", D, r'^VELORA_INCOMPATIBLE_LICENSE_EXCEPTIONS = "[^"]+"')
need("kernel 6.18", D, r'^PREFERRED_VERSION_linux-yocto = "6\.18%"')
need("layer compat", f"{M}/conf/layer.conf", r'LAYERSERIES_COMPAT_velora = "wrynose"')
need("forbidden packages excluded", IMG, r'^PACKAGE_EXCLUDE = ".*\bflac\b.*\bmetaflac\b.*\blibflac\b.*libflac\+\+.*\blibsndfile1\b')
# placeholders are marked
need("1.1 placeholder: rt fragment", f"{M}/recipes-kernel/linux/files/preempt-rt.cfg", r"TODO\(Story 1\.1\)")
need("PREEMPT_RT in fragment", f"{M}/recipes-kernel/linux/files/preempt-rt.cfg", r"^CONFIG_PREEMPT_RT=y$")
need("1.1 placeholder: bootargs", f"{M}/conf/distro/include/velora-bootargs.inc", r"TODO\(Story 1\.1\)")
need("1.2 placeholder", f"{M}/recipes-kernel/linux/linux-yocto_%.bbappend", r"TODO\(Story 1\.2\)")
need("README lists unresolved deps", "README.md", r"TODO\(Story 1\.1\)")
need("README lists 1.2", "README.md", r"TODO\(Story 1\.2\)")
need("README: image build not done", "README.md", r"(?i)NOT done")
# kas: every layer pinned to a full commit; test layer not in the product file
kas = text("kas/velora-dev.yml")
urls = re.findall(r"^\s+url: (\S+)", kas, re.M)
commits = re.findall(r"^\s+commit: ([0-9a-f]{40})\s*$", kas, re.M)
if len(urls) != len(commits) or not urls:
    errors.append(f"kas: {len(urls)} repos with url but {len(commits)} full commit pins")
if "test-layer" in kas or "meta-velora-test" in kas:
    errors.append("kas/velora-dev.yml references the test layer")
# CI workflow: manual only
wf = (root.parent / ".github/workflows/yocto.yml")
if wf.is_file():
    w = wf.read_text()
    on = re.search(r"^on:\n((?:[ \t]+.*\n|\n)+)", w, re.M)
    if not on or re.findall(r"^  (\w+):", on.group(1), re.M) != ["workflow_dispatch"]:
        errors.append("yocto.yml: trigger must be workflow_dispatch only")
    for k in ("license.manifest", "upload-artifact"):
        if k not in w:
            errors.append(f"yocto.yml: missing {k}")
else:
    errors.append("missing .github/workflows/yocto.yml")

if errors:
    print("static checks FAILED:")
    for e in errors:
        print("  -", e)
    sys.exit(1)
print("static checks: ok")
