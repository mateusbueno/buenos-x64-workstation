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
IMG = f"{M}/recipes-core/images/velora-image-dev.bb"           # dev variant
IMGB = f"{M}/recipes-core/images/velora-image-base.inc"         # shared base of every variant (Story 6.13)
IMGF = f"{M}/recipes-core/images/velora-image-factory-test.bb"  # factory-test variant

# os-release dev marker
need("os-release dev marker", D, r'^VARIANT_ID = "\$\{VELORA_VARIANT\}"')
need("os-release dev default", D, r'^VELORA_VARIANT \?= "dev"')
need("os-release VARIANT names dev, factory-test and the prod placeholder", D, r"^VARIANT = .*'dev': 'Development'.*'factory-test': 'Factory test'.*'prod': 'Production'")
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
# Story 6.11: read-only root, tmpfs runtime paths, /data layout, service users, data directories
WKS = f"{M}/files/wic/velora-nvme.wks"
need("image uses the committed wks", IMGB, r'^WKS_FILE = "velora-nvme\.wks"')
need("read-only-rootfs in the shared image base", IMGB, r'^IMAGE_FEATURES = ".*\bread-only-rootfs\b')
need("service users in the image", IMGB, r"^\s+velora-service-users \\$")
BF = f"{M}/recipes-core/base-files/base-files_%.bbappend"
need("tmpfs /tmp in fstab", BF, r"tmpfs\s+/tmp\s+tmpfs\s+mode=1777,nodev,nosuid")
need("/data mount point in the rootfs", BF, r"install -d \$\{D\}/data")
need("kernel args mount root read-only", f"{M}/conf/distro/include/velora-bootargs.inc", r'^VELORA_BOOTARGS_BASE \?= ".*\bro\b')
need("kernel args drop rw", f"{M}/conf/distro/include/velora-bootargs.inc", r'^UBOOT_EXTLINUX_KERNEL_ARGS:remove = "rw"$')
need("extlinux root is slot A", f"{M}/conf/distro/include/velora-bootargs.inc", r'^UBOOT_EXTLINUX_ROOT = "root=PARTLABEL=rootfs\.0"$')
need("wks: /data options", WKS, r'^part /data .*--fstype=ext4 .*--fsoptions "[^"]*\bnodev\b[^"]*\bnosuid\b[^"]*\bdata=ordered\b')
need("wks: slot A", WKS, r"^part /\s.*--part-name rootfs\.0 ")
need("wks: slot B", WKS, r"^part\s+--ondisk.*--part-name rootfs\.1 ")
USR = f"{M}/recipes-core/velora-service-users/velora-service-users_1.0.bb"
need("service users via useradd class", USR, r"^inherit .*\buseradd\b")
for u in ("velora-engine", "velora-librarian", "velora-sysd"):
    i = {"velora-engine": 900, "velora-librarian": 901, "velora-sysd": 902}[u]
    need(f"static uid for {u}", USR, rf"--uid {i} --gid {i} {u};")
    need(f"static gid for {u}", USR, rf"--gid {i} {u};")
need("GROUPADD_PARAM present", USR, r"^GROUPADD_PARAM:\$\{PN\} =")
DATA = f"{CFG}/10-velora-data.conf"
for path, mode, user in (("lib", "0755", "velora-librarian"), ("catalog", "0750", "velora-librarian"),
                         ("livesets", "0750", "velora-engine"), ("config", "0755", "velora-sysd")):
    need(f"/data/{path} owned by {user}", DATA, rf"^d\s+/data/{path}\s+{mode}\s+{user}\s+{user}\s")
need("tmpfiles.d installed", REC, r"install -m 0644 \$\{UNPACKDIR\}/10-velora-data\.conf \$\{D\}\$\{sysconfdir\}/tmpfiles\.d/10-velora-data\.conf")
need("tmpfiles.d in SRC_URI", REC, r"file://10-velora-data\.conf")
need("system-config depends on the users", REC, r'^RDEPENDS:\$\{PN\} \+= "velora-service-users"')
need("README: layout table", "README.md", r"(?m)^## Disk layout")
need("README: data=ordered reason", "README.md", r"data=ordered")
need("README: AD-9 table", "README.md", r"AD-9")
need("README: boot/EROFS/fsck not done", "README.md", r"(?is)NOT done.*power")
# every Velora image applies the policy
imgs = list((root / M).rglob("velora-image-*.bb"))
if not imgs:
    errors.append("no velora-image-*.bb found")
need("licence policy in the shared image base", IMGB, r'^INCOMPATIBLE_LICENSE = "\$\{VELORA_INCOMPATIBLE_LICENSE\}"')
need("licence exceptions in the shared image base", IMGB, r'^INCOMPATIBLE_LICENSE_EXCEPTIONS = "\$\{VELORA_INCOMPATIBLE_LICENSE_EXCEPTIONS\}"')
for img in imgs:
    rel = str(img.relative_to(root))
    need("every image recipe requires the shared base", rel, r"^require velora-image-base\.inc$")
    need("every image recipe states its variant", rel, r'^VELORA_IMAGE_VARIANT = "(dev|factory-test)"$')
    need("image applies licence policy", IMGB, r'^INCOMPATIBLE_LICENSE = "\$\{VELORA_INCOMPATIBLE_LICENSE\}"')
    need("image applies licence exceptions", IMGB, r'^INCOMPATIBLE_LICENSE_EXCEPTIONS = "\$\{VELORA_INCOMPATIBLE_LICENSE_EXCEPTIONS\}"')
need("authorized keys bbfatal when missing", IMG, r"bbfatal .*VELORA_DEV_AUTHORIZED_KEYS_FILE")
need("authorized keys file-checksums", IMG, r"^do_rootfs\[file-checksums\]")
# core dumps disabled
need("core_pattern", f"{CFG}/50-velora-sysctl.conf", r"^kernel\.core_pattern\s*=\s*\|/bin/false\s*$")
need("suid_dumpable", f"{CFG}/50-velora-sysctl.conf", r"^fs\.suid_dumpable\s*=\s*0\s*$")
need("coredump Storage=none", f"{CFG}/10-velora-coredump.conf", r"^Storage=none$")
need("DefaultLimitCORE=0", f"{CFG}/10-velora-limits.conf", r"^DefaultLimitCORE=0$")
# ssh + debug uart, dev image only
IMG = f"{M}/recipes-core/images/velora-image-dev.bb"
need("ssh server (dev variant only)", IMG, r'^IMAGE_FEATURES \+= "ssh-server-openssh"$')
forbid("no ssh feature in the shared base", IMGB, r"^[^#\n]*ssh-server")
forbid("no authorized keys in the shared base", IMGB, r"^[^#\n]*authorized_keys")
forbid("insecure root login in image features", IMGB, r'^IMAGE_FEATURES(\s*\+?=|:append)[^\n]*(debug-tweaks|empty-root-password|allow-empty-password)')
need("debug uart getty", D, r'^SERIAL_CONSOLES = "\d+;ttyS\d+"')
# distro
need("systemd init manager", D, r'^INIT_MANAGER = "systemd"')
need("licence policy", D, r'^VELORA_INCOMPATIBLE_LICENSE = "GPL-3\.0\* LGPL-3\.0\* AGPL-3\.0\*"')
forbid("no licence exceptions expected", D, r'^VELORA_INCOMPATIBLE_LICENSE_EXCEPTIONS = "[^"]+"')
need("kernel 6.18", D, r'^PREFERRED_VERSION_linux-yocto = "6\.18%"')
need("layer compat", f"{M}/conf/layer.conf", r'LAYERSERIES_COMPAT_velora = "wrynose"')
need("forbidden packages excluded", IMGB, r'^PACKAGE_EXCLUDE = ".*\bflac\b.*\bmetaflac\b.*\blibflac\b.*libflac\+\+.*\blibsndfile1\b')
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
# Story 6.12: RAUC A/B slots, dev keyring only, mark good, rollback record
RA = f"{M}/recipes-core/rauc"
SYSCONF = f"{RA}/files/system.conf"
need("rauc: compatible string", SYSCONF, r"^compatible=Velora RK3588$")
need("rauc: u-boot backend", SYSCONF, r"^bootloader=uboot$")
need("rauc: TODO(Story 1.6) on the placeholder backend", SYSCONF, r"TODO\(Story 1\.6\)")
need("rauc: plain bundles only", SYSCONF, r"^bundle-formats=-verity -crypt$")
need("rauc: status on /data", SYSCONF, r"^data-directory=/data/rauc$")
need("rauc: keyring is the dev ca.cert.pem", SYSCONF, r"^\[keyring\]\n(#.*\n)*path=/usr/lib/rauc/ca\.cert\.pem$", re.M)
need("rauc: slot rootfs.0 by partition name", SYSCONF, r"^\[slot\.rootfs\.0\]\ndevice=/dev/disk/by-partlabel/rootfs\.0\ntype=ext4\nbootname=A$")
need("rauc: slot rootfs.1 by partition name", SYSCONF, r"^\[slot\.rootfs\.1\]\ndevice=/dev/disk/by-partlabel/rootfs\.1\ntype=ext4\nbootname=B$")
forbid("rauc: no device path by number", SYSCONF, r"^device=/dev/(nvme|mmc|sd)")
need("fw_env.config points at the uboot_env partition", f"{RA}/files/fw_env.config", r"^/dev/disk/by-partlabel/uboot_env 0x0 0x[0-9a-f]+$")
need("fw_env.config: TODO(Story 1.6)", f"{RA}/files/fw_env.config", r"TODO\(Story 1\.6\)")
RC = f"{RA}/rauc-conf.bbappend"
need("keyring path variable defaults to empty", RC, r'^VELORA_RAUC_KEYRING_FILE \?= ""$')
need("keyring: bbfatal when unset", RC, r'bbfatal "VELORA_RAUC_KEYRING_FILE is not set')
need("keyring: bbfatal when missing", RC, r"bbfatal \"VELORA_RAUC_KEYRING_FILE='\$keyring' is missing or empty")
need("keyring: bbfatal on a private key", RC, r"grep -q \"PRIVATE KEY\" \"\$keyring\"")
need("keyring: exactly one certificate", RC, r'must hold exactly one PEM certificate')
need("keyring: dev marker required", RC, r"not for production")
need("keyring: openssl-native for the subject check", RC, r'^DEPENDS \+= "openssl-native"$')
need("keyring: installs only ca.cert.pem and system.conf", RC, r'install -m 0644 "\$keyring" \$\{D\}\$\{nonarch_libdir\}/rauc/ca\.cert\.pem')
for p in root.rglob("*"):
    if p.is_file() and ".yocto-work" not in p.parts and p.name != "static_checks.py" and re.search(rb"-----BEGIN [A-Z ]*(PRIVATE KEY|CERTIFICATE)-----", p.read_bytes()):
        errors.append(f"PEM block committed under yocto/: {p.relative_to(root)}")
    if p.is_file() and ".yocto-work" not in p.parts and p.suffix in (".pem", ".key", ".crt", ".cert", ".p12", ".der"):
        errors.append(f"key or certificate file committed under yocto/: {p.relative_to(root)}")
need("rauc target packageconfig", f"{RA}/rauc_%.bbappend", r'^PACKAGECONFIG = "service json nocreate gpt"$')
need("rauc: libubootenv provides fw_setenv", f"{RA}/rauc_%.bbappend", r'^RAUC_BOOTLOADER_RDEPENDS = "libubootenv-bin"$')
need("stock mark-good recommendation removed", f"{RA}/rauc_%.bbappend", r'^RRECOMMENDS:\$\{PN\}:remove = "\$\{PN\}-mark-good"$')
need("image: stock mark-good not installed", IMGB, r'^BAD_RECOMMENDATIONS \+= "rauc-mark-good"$')
for pkg in ("rauc", "rauc-conf", "velora-rauc-mark-good", "velora-rollback-record", "velora-uboot-ab-script"):
    need(f"image installs {pkg}", IMGB, rf"^\s+{re.escape(pkg)} \\$")
need("image produces the ext4 rootfs the bundle ships", IMGB, r'^IMAGE_FSTYPES \+= "ext4"$')
need("rauc distro feature", D, r'^DISTRO_FEATURES:append = ".* rauc"')
need("layer depends on meta-rauc", f"{M}/conf/layer.conf", r'^LAYERDEPENDS_velora = "core rockchip rauc"$')
need("kas pins meta-rauc on wrynose", "kas/velora-dev.yml", r"meta-rauc:\n\s+url: https://github\.com/rauc/meta-rauc\n\s+path: \S+\n\s+branch: wrynose\n\s+commit: [0-9a-f]{40}\n", re.M)
BUN = f"{M}/recipes-core/bundles/velora-bundle-base.inc"   # shared by the dev and factory-test bundle recipes (Story 6.13)
need("bundle recipe uses the bundle class", BUN, r"^inherit bundle$")
need("bundle recipe: plain", BUN, r'^RAUC_BUNDLE_FORMAT = "plain"$')
need("bundle recipe: compatible equals system.conf", BUN, r'^RAUC_BUNDLE_COMPATIBLE = "Velora RK3588"$')
need("bundle recipe: rootfs slot from the dev image", f"{M}/recipes-core/bundles/velora-bundle-dev.bb", r'^RAUC_SLOT_rootfs = "velora-image-dev"$')
need("bundle recipe: signing dir variable empty by default", BUN, r'^VELORA_RAUC_SIGN_DIR \?= ""$')
need("bundle recipe: bbfatal without signing files", BUN, r"bbfatal \"VELORA_RAUC_SIGN_DIR is not set")
need("bundle recipe: keyring verifies at creation", BUN, r"--keyring=")
need("bundle recipe: bbfatal without keyring", BUN, r'bbfatal "VELORA_RAUC_KEYRING_FILE is not set')
need("bundle recipe: certificate verified against the keyring", BUN, r"openssl verify -CAfile \"\$keyring\" \"\$\{RAUC_CERT_FILE\}\"")
need("bundle recipe: key matches certificate", BUN, r"bbfatal \"signing key .* does not match the certificate")
MG = f"{RA}/files/systemd/velora-rauc-mark-good.service"
for t in ("engine", "ui", "sysd"):
    need(f"mark-good Requires velora-{t}-ready.target", MG, rf"^Requires=.*\bvelora-{t}-ready\.target\b")
    need(f"mark-good After velora-{t}-ready.target", MG, rf"^After=.*\bvelora-{t}-ready\.target\b")
    need(f"velora-{t}-ready.target has a placeholder dependency set", f"{RA}/files/systemd/velora-{t}-ready.target", rf"^Requires=velora-{t}\.service$")
    need(f"velora-{t}-ready.target is marked TODO", f"{RA}/files/systemd/velora-{t}-ready.target", r"TODO\(")
need("mark-good runs rauc status mark-good", MG, r"^ExecStart=/usr/bin/rauc status mark-good$")
forbid("mark-good is not wanted by default.target", MG, r"^WantedBy=.*default\.target")
forbid("mark-good has no unconditional After=default.target shortcut", MG, r"^(Before|After)=.*default\.target")
need("mark-good installed to multi-user.target only", MG, r"^\[Install\]\nWantedBy=multi-user\.target$", re.M)
need("mark-good recipe enables the unit", f"{RA}/velora-rauc-mark-good_1.0.bb", r'^SYSTEMD_SERVICE:\$\{PN\} = "velora-rauc-mark-good\.service"$')
need("stock rauc-mark-good.service masked", f"{RA}/velora-rauc-mark-good_1.0.bb", r"ln -sf /dev/null \$\{D\}\$\{sysconfdir\}/systemd/system/rauc-mark-good\.service")
need("rollback record unit writes /data/config only", f"{RA}/files/systemd/velora-rollback-record.service", r"^ReadWritePaths=/data/config$")
need("rollback recorder: temp + fsync + rename", f"{RA}/files/rollback/velora-rollback-record", r"(?s)mkstemp.*os\.fsync\(f\.fileno\(\)\).*os\.replace\(tmp, path\)")
need("rollback recorder default path", f"{RA}/files/rollback/velora-rollback-record", r'"--out", default="/data/config/rollback\.json"')
need("/data/rauc exists (tmpfiles)", DATA, r"^d\s+/data/rauc\s+0755\s+root\s+root\s")
need("/data/update exists and is owned by velora-sysd (Story 8.1)", DATA, r"^d\s+/data/update\s+0750\s+velora-sysd\s+velora-sysd\s")
need("rauc dbus policy installed (Story 8.1)", f"{RA}/rauc_%.bbappend", r"de\.pengutronix\.rauc\.velora\.conf")
need("rauc dbus policy allows velora-sysd only (Story 8.1)", f"{RA}/files/dbus/de.pengutronix.rauc.velora.conf", r'<policy user="velora-sysd">')
need("u-boot script: 3 attempts and the RAUC variables", f"{M}/files/uboot/velora-ab-boot.cmd", r"(?s)BOOT_ORDER.*setenv BOOT_A_LEFT 3.*setenv BOOT_B_LEFT 3")
need("u-boot script: TODO(Story 1.6)", f"{M}/files/uboot/velora-ab-boot.cmd", r"TODO\(Story 1\.6\)")
need("u-boot script: root by partition name per slot", f"{M}/files/uboot/velora-ab-boot.cmd", r"root=PARTLABEL=rootfs\.\$\{velora_slotnum\} rauc\.slot=\$\{velora_slot\}")
need("kernel fragment for verity installs", f"{M}/recipes-kernel/linux/files/velora-rauc.cfg", r"^CONFIG_DM_VERITY=y$")
need("README: RAUC section", "README.md", r"(?m)^## RAUC")
need("README: install/boot counter/rollback not done", "README.md", r"(?is)RAUC.*NOT done.*(install|rollback)")
# Story 6.13: image variants (shared base + dev + factory-test), per-variant keyring and bundle, factory test runner
FT = f"{M}/recipes-core/velora-factory-test"
need("dev variant states VELORA_IMAGE_VARIANT", IMG, r'^VELORA_IMAGE_VARIANT = "dev"$')
need("factory-test variant states VELORA_IMAGE_VARIANT", IMGF, r'^VELORA_IMAGE_VARIANT = "factory-test"$')
need("image base checks the variant before the rootfs", IMGB, r'^do_rootfs\[prefuncs\] \+= "velora_check_image_variant"$')
need("factory-test: no ssh feature", IMGF, r'^IMAGE_FEATURES:remove \+= "ssh-server-openssh ssh-server-dropbear"$')
need("factory-test: ssh packages excluded", IMGF, r'^PACKAGE_EXCLUDE \+= ".*\bopenssh-sshd\b.*\bdropbear\b')
forbid("factory-test: no ssh feature added", IMGF, r'^IMAGE_FEATURES \+= ')
forbid("factory-test: no authorized keys installed", IMGF, r"install .*authorized_keys")
need("factory-test: runner installed", IMGF, r'^IMAGE_INSTALL \+= "velora-factory-test"$')
forbid("dev: no factory-test runner", IMG, r"velora-factory-test")
forbid("shared base: no factory-test runner", IMGB, r"velora-factory-test")
need("factory-test: serial getty masked", IMGF, r"ln -sf /dev/null \$\{IMAGE_ROOTFS\}\$\{sysconfdir\}/systemd/system/serial-getty@\$tty\.service")
need("factory-test: rootfs asserted free of SSH", IMGF, r"ROOTFS_POSTPROCESS_COMMAND \+= \"velora_factory_no_login; velora_factory_assert_no_ssh;\"")
need("factory-test: authorized keys fatal", IMGF, r'bbfatal "VELORA_DEV_AUTHORIZED_KEYS_FILE is set')
need("factory-test: debug UART policy documented", "README.md", r"(?is)debug UART.*factory-test.*getty")
KASF = "kas/velora-factory-test.yml"
need("kas factory-test: includes the dev file (same pins)", KASF, r"^\s+- velora-dev\.yml$")
need("kas factory-test: target", KASF, r"^target: velora-image-factory-test$")
need("kas factory-test: VELORA_VARIANT", KASF, r'^\s+VELORA_VARIANT = "factory-test"$')
forbid("kas factory-test: no repos of its own (nothing to pin)", KASF, r"^\s+url:")
BF_ = f"{M}/recipes-core/bundles"
need("bundle dev variant", f"{BF_}/velora-bundle-dev.bb", r'^VELORA_BUNDLE_VARIANT = "dev"$')
need("bundle factory-test variant", f"{BF_}/velora-bundle-factory-test.bb", r'^VELORA_BUNDLE_VARIANT = "factory-test"$')
need("bundle factory-test: slot from the factory-test image", f"{BF_}/velora-bundle-factory-test.bb", r'^RAUC_SLOT_rootfs = "velora-image-factory-test"$')
for b in ("velora-bundle-dev.bb", "velora-bundle-factory-test.bb"):
    need(f"{b} requires the shared bundle base", f"{BF_}/{b}", r"^require velora-bundle-base\.inc$")
need("bundle base: variant must equal VELORA_VARIANT", BUN, r'bbfatal "bundle \$\{PN\} is for variant')
need("bundle base: keyring marker per variant", BUN, r"Velora \$variant RAUC root")
need("keyring: exact RFC 2253 subject per variant", RC, r'want="CN=Velora \$\{VELORA_VARIANT\} RAUC root \(not for production\),O=Velora"')
need("keyring: subject compared with !=", RC, r'if \[ "\$subject" != "\$want" \]')
need("keyring: exactly one PEM block, a CERTIFICATE", RC, r"grep -c '\^-----BEGIN ' \"\$keyring\"")
need("keyring: block type must be CERTIFICATE", RC, r'\[ "\$firstblock" != "CERTIFICATE" \]')
need("keyring: self-signed root required", RC, r'openssl verify -CAfile "\$keyring" "\$keyring"')
need("keyring: VELORA_VARIANT validated", RC, r"dev\|factory-test\) ;;")
need("keyring: prod rule left to Story 6.14", RC, r"prod\) bbfatal .*Story 6\.14")
need("bundle base: exact RFC 2253 subject", BUN, r'want="CN=Velora \$variant RAUC root \(not for production\),O=Velora"')
need("bundle base: one PEM block, a CERTIFICATE", BUN, r'\[ "\$firstblock" != "CERTIFICATE" \]')
need("bundle base: self-signed root required", BUN, r'openssl verify -CAfile "\$keyring" "\$keyring"')
need("bundle base: VELORA_VARIANT validated", BUN, r"dev\|factory-test\) ;;")
need("image base: root account must be locked", IMGB, r'^ROOTFS_POSTPROCESS_COMMAND \+= "velora_assert_root_locked;"$')
need("image base: root lock accepts only * or !", IMGB, r"'\*'\|'!'\*\) ;;")
need("factory-test: find pass for ssh material", IMGF, r"find \$\{IMAGE_ROOTFS\} .*-name 'authorized_keys\*' -o -name 'sshd\*' -o -name 'dropbear\*' -o -name 'ssh\*\.socket' -o -name 'ssh\*\.service'")
need("gen script: --variant with --name refused", "ci/rauc/gen-dev-root.sh", r"--variant and --name cannot be combined")
need("gen script: reserved --name refused", "ci/rauc/gen-dev-root.sh", r"dev\|factory-test\|prod\|production\) echo \"refusing --name")
need("keyring: still exactly one variant keyring file", RC, r'^VELORA_RAUC_KEYRING_FILE \?= ""$')
need("system.conf: one keyring path for every variant", SYSCONF, r"^path=/usr/lib/rauc/ca\.cert\.pem$")
need("gen script supports --variant dev and factory-test", "ci/rauc/gen-dev-root.sh", r"^\s+dev\|factory-test\) NAME=")
need("gen script refuses other variants (production)", "ci/rauc/gen-dev-root.sh", r"refusing variant")
need("runner: schema string", f"{FT}/files/velora-factory-test", r'^SCHEMA = "velora-factory-test-report/2"$')
for name in ("MCU links", "Keybed scan", "Panel controls and LEDs", "Codec loopback", "Display and touch", "NVMe read/write speed"):
    need(f"runner covers {name}", f"{FT}/files/velora-factory-test", rf'^\s+\("{re.escape(name)}", ')
need("runner: three statuses", f"{FT}/files/velora-factory-test", r'^PASS, FAIL, NOT_PRESENT = "pass", "fail", "not present"$')
need("runner: probe timeout isolation", f"{FT}/files/velora-factory-test", r"t\.join\(timeout\)")
need("runner: TODO where the hardware interface is missing", f"{FT}/files/velora-factory-test", r"TODO\(Stories? 3\.x\)")
need("runner recipe installs runner and unit", f"{FT}/velora-factory-test_1.0.bb", r"install -m 0755 \$\{UNPACKDIR\}/velora-factory-test \$\{D\}\$\{libexecdir\}/velora/velora-factory-test")
need("runner recipe enables the unit", f"{FT}/velora-factory-test_1.0.bb", r'^SYSTEMD_SERVICE:\$\{PN\} = "velora-factory-test\.service"$')
UNIT = f"{FT}/files/velora-factory-test.service"
need("runner unit writes /data/factory-test only (tolerates its absence)", UNIT, r"^ReadWritePaths=-/data/factory-test$")
need("runner unit: fallback RuntimeDirectory", UNIT, r"^RuntimeDirectory=velora-factory-test$")
need("runner unit: ordered after udev trigger, sound, tmpfiles, local-fs", UNIT, r"^After=.*\blocal-fs\.target\b.*\bsystemd-tmpfiles-setup\.service\b.*\bsystemd-udev-trigger\.service\b.*\bsound\.target\b")
need("runner unit: waits for the /data mount without requiring it", UNIT, r"(?s)^After=[^\n]*\bdata\.mount\b.*^Wants=data\.mount$")
forbid("runner unit: no always-true ConditionPathExists=/data", UNIT, r"^ConditionPathExists=")
need("runner unit: TimeoutStartSec", UNIT, r"^TimeoutStartSec=\d+$")
need("runner: expected links defined", f"{FT}/files/velora-factory-test", r'^EXPECTED_LINKS = \("velora-mcu-keybed", "velora-mcu-panel"\)')
need("runner: panel constants", f"{FT}/files/velora-factory-test", r"^PANEL_CONTROLS_EXPECTED = \d+\nPANEL_LEDS_EXPECTED = \d+$")
need("runner: report carries boot_id", f"{FT}/files/velora-factory-test", r'"boot_id": boot_id or "unknown"')
need("runner: stale report removed first", f"{FT}/files/velora-factory-test", r"a stale report from an earlier run")
need("runner: report files 0644", f"{FT}/files/velora-factory-test", r"os\.fchmod\(f\.fileno\(\), 0o644\)")
need("runner unit: exit 1 and 2 are test results", UNIT, r"^SuccessExitStatus=1 2$")
need("README: image variants section", "README.md", r"(?m)^## Image variants")
need("README: variants verified vs not", "README.md", r"(?is)factory-test.*NOT done.*(boot|hardware)")
need("README: production variant structure only", "README.md", r"(?is)production.*NOT (built|defined)")

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
