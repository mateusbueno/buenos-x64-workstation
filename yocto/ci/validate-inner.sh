#!/usr/bin/env bash
# Runs inside the validation container (see validate.sh). Do not call directly on the host.
set -uo pipefail
SRC=/src/yocto
PROJ=/work/proj
export KAS_WORK_DIR="$PROJ" KAS_BUILD_DIR="$PROJ/.yocto-work/build"
mkdir -p "$PROJ/yocto"
rsync -a --delete --exclude .yocto-work "$SRC/" "$PROJ/yocto/"
cd "$PROJ"

FAILED=0; PASSED=0
pass() { echo "  PASS  $1"; PASSED=$((PASSED+1)); }
fail() { echo "  FAIL  $1"; FAILED=$((FAILED+1)); }
LOGDIR="$PROJ/.yocto-work/logs"; mkdir -p "$LOGDIR"
KAS="kas"
bb() { # bb <kas-config> <bitbake args...>  -> output in $OUT, exit code returned
  local cfg="$1"; shift
  OUT="$LOGDIR/bb-$(echo "$*" | tr -c 'A-Za-z0-9\n' '_').log"
  # "-n <image>" is run as "-g <image>" (full task-graph resolution, same provider/licence errors, but no per-task dry-run spawn)
  local args="$*"; args="${args/-n /-g }"
  local d="$PROJ/.yocto-work/scratch"; mkdir -p "$d"
  $KAS shell "yocto/$cfg" -c "cd $d && bitbake $args" >"$OUT" 2>&1
}
tail_log() { echo "  --- log tail ($OUT)"; grep -v 'Duplicate inclusion' "$OUT" | tail -n 25 | sed 's/^/  | /'; }

echo "== 1. static config checks"
if python3 -I "$SRC/ci/static_checks.py" "$SRC"; then pass "static checks"; else fail "static checks"; fi

echo "== 2. checkout pinned layers (kas)"
if $KAS checkout yocto/ci/kas-test-layer.yml >"$LOGDIR/checkout.log" 2>&1; then pass "kas checkout (all layers at pinned commits)"; else fail "kas checkout"; tail -n 25 "$LOGDIR/checkout.log"; echo "== aborting: no layers"; exit 1; fi

echo "== 3. layer and recipe parse (bitbake -p)"
if bb ci/kas-test-layer.yml -p; then pass "bitbake -p (all layers incl. test layer parse)"; else fail "bitbake -p"; tail_log; fi

echo "== 4. dev image resolves (bitbake -g resolution) and dependency graph"
if bb kas/velora-dev.yml -n velora-image-dev; then pass "velora-image-dev resolves (task graph resolves, bitbake -g)"; else fail "velora-image-dev dry run"; tail_log; fi
GRAPH="$PROJ/.yocto-work/graph"; rm -rf "$GRAPH"; mkdir -p "$GRAPH"
if $KAS shell yocto/kas/velora-dev.yml -c "cd $GRAPH && bitbake -g velora-image-dev" >"$LOGDIR/graph.log" 2>&1 && [ -s "$GRAPH/pn-buildlist" ]; then pass "bitbake -g velora-image-dev ($(wc -l < "$GRAPH/pn-buildlist") recipes)"; else fail "bitbake -g"; tail -n 25 "$LOGDIR/graph.log"; fi
for r in linux-yocto systemd openssh busybox libgcc gcc-runtime velora-system-config velora-service-users volatile-binds os-release u-boot rauc rauc-conf velora-rauc-mark-good velora-rollback-record velora-uboot-ab-script libubootenv python3; do
  if grep -qx "$r" "$GRAPH/pn-buildlist" 2>/dev/null; then pass "image package set contains $r"; else fail "image package set lacks $r"; fi
done
for r in flac libsndfile1 libsndfile; do
  if [ ! -s "$GRAPH/pn-buildlist" ]; then fail "forbidden recipe check for $r: no package set available"
  elif grep -qx "$r" "$GRAPH/pn-buildlist"; then fail "forbidden recipe $r IS in the image package set"; else pass "forbidden recipe $r absent from the image package set"; fi
done
for f in task-depends.dot; do
  if [ ! -s "$GRAPH/$f" ]; then fail "forbidden-package check: $f missing"
  elif grep -Eq '"(flac|metaflac|libflac|libflac\+\+|libsndfile1?)(\.|")' "$GRAPH/$f"; then fail "forbidden package found in $f"; else pass "no flac/libsndfile node in $f"; fi
done
# self-test of the forbidden-package check: an image that really adds libsndfile1 must be detected
GRAPH2="$PROJ/.yocto-work/graph-sndfile"; rm -rf "$GRAPH2"; mkdir -p "$GRAPH2"
$KAS shell yocto/ci/kas-test-layer.yml -c "cd $GRAPH2 && bitbake -g velora-test-sndfile-image" >"$LOGDIR/graph2.log" 2>&1
if grep -qx libsndfile1 "$GRAPH2/pn-buildlist" 2>/dev/null; then pass "forbidden-package check detects libsndfile1 in a deliberately bad image"; else fail "forbidden-package check did not detect libsndfile1 in the bad image"; tail -n 15 "$LOGDIR/graph2.log"; fi

echo "== 5. resolved configuration (bitbake -e)"
envdump() { $KAS shell yocto/kas/velora-dev.yml -c "bitbake -e $1" > "$LOGDIR/env-$1.txt" 2>&1; }
envdump velora-image-dev; envdump os-release; envdump linux-yocto; envdump volatile-binds
ev() { grep -E "^$2=" "$LOGDIR/env-$1.txt" | head -1 | sed -E 's/^[^=]*="(.*)"$/\1/'; }
chk() { # chk <description> <recipe> <var> <egrep-pattern>
  if ev "$2" "$3" | grep -Eq "$4"; then pass "$1"; else fail "$1 (got: $(ev "$2" "$3" | head -c 200))"; fi
}
chk "INIT_MANAGER = systemd" velora-image-dev INIT_MANAGER '^systemd$'
chk "DISTRO = velora" velora-image-dev DISTRO '^velora$'
chk "DISTRO_FEATURES has systemd" velora-image-dev DISTRO_FEATURES '(^| )systemd( |$)'
if ev velora-image-dev DISTRO_FEATURES | grep -qw sysvinit; then fail "DISTRO_FEATURES contains sysvinit"; else pass "DISTRO_FEATURES has no sysvinit"; fi
chk "IMAGE_FEATURES = ssh-server-openssh" velora-image-dev IMAGE_FEATURES 'ssh-server-openssh'
if ev velora-image-dev IMAGE_FEATURES | grep -Eq 'debug-tweaks|empty-root-password|allow-empty-password'; then fail "IMAGE_FEATURES has an insecure login feature"; else pass "IMAGE_FEATURES has no debug-tweaks/empty password"; fi
chk "INCOMPATIBLE_LICENSE covers GPL-3.0* LGPL-3.0* AGPL-3.0*" velora-image-dev INCOMPATIBLE_LICENSE '^GPL-3.0\* LGPL-3.0\* AGPL-3.0\*$'
chk "SERIAL_CONSOLES = 1500000;ttyS2 (debug UART getty)" velora-image-dev SERIAL_CONSOLES '1500000;ttyS2'
chk "PACKAGE_EXCLUDE has flac metaflac libsndfile1" velora-image-dev PACKAGE_EXCLUDE 'flac.*metaflac.*libsndfile1'
chk "kernel args carry the isolation placeholder (isolcpus)" velora-image-dev UBOOT_EXTLINUX_KERNEL_ARGS 'isolcpus='
chk "os-release VARIANT_ID = dev" os-release VARIANT_ID '^dev$'
chk "os-release fields include VARIANT_ID" os-release OS_RELEASE_FIELDS 'VARIANT_ID'
chk "kernel is 6.18.x" linux-yocto LINUX_VERSION '^6\.18\.'
chk "kernel SRC_URI has preempt-rt.cfg and velora-dev.cfg" linux-yocto SRC_URI 'preempt-rt\.cfg.*velora-dev\.cfg|velora-dev\.cfg.*preempt-rt\.cfg'

chk "DISTRO_FEATURES has rauc" velora-image-dev DISTRO_FEATURES '(^| )rauc( |$)'
chk "stock rauc-mark-good is a bad recommendation" velora-image-dev BAD_RECOMMENDATIONS 'rauc-mark-good'
chk "IMAGE_FSTYPES has ext4 (the bundle's rootfs file)" velora-image-dev IMAGE_FSTYPES '(^| )ext4( |$)'
for pkg in rauc rauc-conf velora-rauc-mark-good velora-rollback-record velora-uboot-ab-script; do chk "image installs $pkg" velora-image-dev IMAGE_INSTALL "(^| )$pkg( |$)"; done
chk "ROOTFS_POSTPROCESS_COMMAND runs velora_dev_authorized_keys" velora-image-dev ROOTFS_POSTPROCESS_COMMAND 'velora_dev_authorized_keys'
chk "IMAGE_FEATURES has read-only-rootfs" velora-image-dev IMAGE_FEATURES '(^| )read-only-rootfs( |$)'
chk "WKS_FILE = velora-nvme.wks" velora-image-dev WKS_FILE '^velora-nvme\.wks$'
chk "WKS_FULL_PATH resolves to the layer's committed wks" velora-image-dev WKS_FULL_PATH 'meta-velora/files/wic/velora-nvme\.wks$'
chk "extlinux root is PARTLABEL=rootfs.0" velora-image-dev UBOOT_EXTLINUX_ROOT '^root=PARTLABEL=rootfs\.0$'
if ev velora-image-dev UBOOT_EXTLINUX_KERNEL_ARGS | grep -Eq '(^| )rw( |$)'; then fail "kernel args still contain rw"; else chk "kernel args mount root ro" velora-image-dev UBOOT_EXTLINUX_KERNEL_ARGS '(^| )ro( |$)'; fi
chk "kernel args contain rootwait" velora-image-dev UBOOT_EXTLINUX_KERNEL_ARGS '(^| )rootwait( |$)'
chk "kernel args contain rootfstype=ext4" velora-image-dev UBOOT_EXTLINUX_KERNEL_ARGS '(^| )rootfstype=ext4( |$)'
chk "IMAGE_FSTYPES has wic" velora-image-dev IMAGE_FSTYPES '(^| )wic( |$)'
chk "WKS_FILE_DEPENDS covers virtual/bootloader" velora-image-dev WKS_FILE_DEPENDS 'virtual/bootloader'
chk "image installs velora-service-users" velora-image-dev IMAGE_INSTALL 'velora-service-users'
chk "VOLATILE_BINDS maps /var/lib to tmpfs" volatile-binds VOLATILE_BINDS 'volatile/lib'
echo "  -- generated SSH overlay (dummy key) parses with kas dump"
echo "ssh-ed25519 AAAAdummykeyfortestsonly validate" > "$PROJ/.yocto-work/dummy_authorized_keys"
bash "$PROJ/yocto/ci/make-ssh-overlay.sh" "$PROJ" "$PROJ/.yocto-work/dummy_authorized_keys" "$PROJ/.yocto-work/kas-ssh.yml"
if $KAS dump "$PROJ/.yocto-work/kas-ssh.yml" > "$LOGDIR/ssh-dump.yml" 2>"$LOGDIR/ssh-dump.err" && grep -q "VELORA_DEV_AUTHORIZED_KEYS_FILE" "$LOGDIR/ssh-dump.yml"; then pass "kas dump of generated SSH overlay"; else fail "kas dump of generated SSH overlay"; tail -n 15 "$LOGDIR/ssh-dump.err"; fi

echo "== 5b. disk layout (wic kickstart parser) and read-only root"
# wic is a standalone tool in Wrynose (recipe wic_*.bb in the pinned openembedded-core, pinned there by SRCREV): fetch and unpack it
# through bitbake (honours DL_DIR; needs network the first time, then the downloads cache).
envdump wic-native
WICS="$(ev wic-native S)"
if $KAS shell yocto/kas/velora-dev.yml -c "bitbake wic-native -c unpack" >"$LOGDIR/wic-unpack.log" 2>&1 && [ -f "$WICS/src/wic/ksparser.py" ]; then pass "wic fetched and unpacked through bitbake (wic-native do_unpack, SRCREV $(ev wic-native SRCREV | head -c 12))"; else fail "cannot fetch/unpack wic through bitbake (wic-native do_unpack needs network access or a populated DL_DIR); see $LOGDIR/wic-unpack.log"; tail -n 10 "$LOGDIR/wic-unpack.log"; WICS=""; fi
WKSFILE="$PROJ/yocto/meta-velora/files/wic/velora-nvme.wks"
if [ -z "$WICS" ]; then fail "wks layout: wic not available"
elif WICOUT=$(python3 -I "$PROJ/yocto/ci/check_wks.py" "$WICS/src" "$WKSFILE" "$LOGDIR/env-velora-image-dev.txt" 2>&1); then pass "wks parses with wic and has the required layout: ${WICOUT#wks checks: ok }"; else fail "wks layout"; echo "$WICOUT" | sed 's/^/  | /'; fi
# self-test: each deliberately broken layout must be rejected with its specific message
wks_variant() { # wks_variant <name> <expected message (fixed string)> <sed expression>
  local bad="$PROJ/.yocto-work/bad.wks"
  sed -e "$3" "$WKSFILE" > "$bad"
  if cmp -s "$bad" "$WKSFILE"; then fail "wks variant '$1': the sed expression changed nothing"; return; fi
  if [ -z "$WICS" ]; then fail "wks variant '$1': wic not available"; return; fi
  if python3 -I "$PROJ/yocto/ci/check_wks.py" "$WICS/src" "$bad" "$LOGDIR/env-velora-image-dev.txt" >"$LOGDIR/badwks.log" 2>&1; then fail "wks variant '$1' was accepted"
  elif grep -qF -- "$2" "$LOGDIR/badwks.log"; then pass "wks check rejects: $1"
  else fail "wks variant '$1' failed, but not with '$2'"; sed 's/^/  | /' "$LOGDIR/badwks.log"; fi
}
wks_variant "slot B populated" "slot B must be unpopulated" 's/--fstype=none --part-name rootfs\.1/--fstype=ext4 --source rootfs --part-name rootfs.1/'
wks_variant "unequal slots" "root slots must have identical fixed size" 's/^\(part  *--ondisk nvme0n1  *--fixed-size\) 2G/\1 1G/'
wks_variant "/data without data=ordered" "/data fsoptions must include data=ordered" 's/,data=ordered//'
wks_variant "/data without nosuid" "/data fsoptions must include nosuid" 's/nosuid,//'
wks_variant "/data without has_journal" "/data must create an explicit ext4 journal" 's/ --mkfs-extraopts "-O has_journal"//'
wks_variant "swap partition added" "wks contains a swap partition" 's/^bootloader/part swap --ondisk nvme0n1 --fstype=swap --size 1G\'$'\n''bootloader/'
wks_variant "wrong partition table" "bootloader ptable must be gpt" 's/--ptable gpt/--ptable msdos/'
wks_variant "boot area offset moved" "boot area partition loader2: offset" 's/--offset 16384s/--offset 16400s/'
wks_variant "overlapping partitions" "overlap or wrong order" 's/--offset 32768s/--offset 30000s/'
# the pinned stock fstab provides tmpfs for /run and /var/volatile
BFSTAB="$PROJ/.yocto-work/layers/openembedded-core/meta/recipes-core/base-files/base-files/fstab"
if grep -Eq '^tmpfs\s+/run\s+tmpfs' "$BFSTAB" && grep -Eq '^tmpfs\s+/var/volatile\s+tmpfs' "$BFSTAB"; then pass "base-files fstab: tmpfs on /run and /var/volatile"; else fail "base-files fstab lacks tmpfs /run or /var/volatile"; fi

echo "== 5c. produced artefacts (do_install of base-files and our recipes; no image build)"
# Only these three recipes are installed (plus pseudo-native for fakeroot): small, no cross toolchain. Our two recipes are run with -b because
# they have no bbappends; base-files goes through the normal path so that our base-files_%.bbappend is applied.
# hermetic: start from an empty TMPDIR (leftovers of an interrupted run break do_prepare_recipe_sysroot) and drop it afterwards to save disk
rm -rf "$PROJ/.yocto-work/build/tmp"
SYSREC="$PROJ/yocto/meta-velora/recipes-core/velora-system-config/velora-system-config_1.0.bb"
USRREC="$PROJ/yocto/meta-velora/recipes-core/velora-service-users/velora-service-users_1.0.bb"
if $KAS shell yocto/kas/velora-dev.yml -c "cd $PROJ/.yocto-work/scratch && bitbake base-files -c install && bitbake -b $SYSREC -c install && bitbake -b $USRREC -c install" >"$LOGDIR/install.log" 2>&1; then pass "do_install of base-files, velora-system-config, velora-service-users"; else fail "do_install of base-files/velora-system-config/velora-service-users"; grep -v 'Duplicate inclusion' "$LOGDIR/install.log" | tail -n 25 | sed 's/^/  | /'; fi
WORK="$PROJ/.yocto-work/build/tmp/work"
BFD=$(ls -d "$WORK"/*/base-files/*/image 2>/dev/null | head -1); SCD=$(ls -d "$WORK"/all-*/velora-system-config/*/image 2>/dev/null | head -1)
fgrep_q() { grep -Eq "$2" "$1" 2>/dev/null; }
if fgrep_q "$BFD/etc/fstab" '^tmpfs\s+/tmp\s+tmpfs\s+mode=1777,nodev,nosuid'; then pass "installed fstab: tmpfs /tmp (nodev,nosuid)"; else fail "installed fstab lacks the tmpfs /tmp line (dir: '$BFD')"; fi
if fgrep_q "$BFD/etc/fstab" '^tmpfs\s+/run\s+tmpfs' && fgrep_q "$BFD/etc/fstab" '^tmpfs\s+/var/volatile\s+tmpfs'; then pass "installed fstab: stock tmpfs /run and /var/volatile"; else fail "installed fstab lacks tmpfs /run or /var/volatile"; fi
if [ -d "$BFD/data" ]; then pass "installed rootfs content has the /data mount point"; else fail "/data missing in the base-files install dir"; fi
T="$SCD/etc/tmpfiles.d/10-velora-data.conf"
for spec in "lib 0755 velora-librarian" "catalog 0750 velora-librarian" "livesets 0750 velora-engine" "config 0755 velora-sysd"; do
  set -- $spec
  if fgrep_q "$T" "^d\s+/data/$1\s+$2\s+$3\s+$3\s"; then pass "installed tmpfiles.d: /data/$1 $2 $3:$3 (AD-9)"; else fail "installed tmpfiles.d lacks /data/$1 $2 $3:$3"; fi
done
if fgrep_q "$SCD/etc/systemd/journald.conf.d/10-velora.conf" '^Storage=volatile$' && fgrep_q "$SCD/etc/systemd/journald.conf.d/10-velora.conf" '^RuntimeMaxUse=[0-9]+[KMG]$'; then pass "installed journald drop-in: volatile and bounded"; else fail "installed journald drop-in is not volatile+bounded"; fi
# static IDs: the useradd class turns USERADD_PARAM/GROUPADD_PARAM into the package preinst; check the resolved values
envdump velora-service-users
for spec in "engine 900" "librarian 901" "sysd 902"; do
  set -- $spec
  if grep -Fq -- "--gid $2 velora-$1;" <(grep -E '^GROUPADD_PARAM:velora-service-users=' "$LOGDIR/env-velora-service-users.txt") && grep -Fq -- "--uid $2 --gid $2 velora-$1;" <(grep -E '^USERADD_PARAM:velora-service-users=' "$LOGDIR/env-velora-service-users.txt"); then pass "resolved useradd/groupadd data: velora-$1 uid=gid=$2"; else fail "resolved useradd/groupadd data lacks velora-$1 uid=gid=$2"; fi
done

rm -rf "$PROJ/.yocto-work/build/tmp"

echo "== 5d. RAUC (Story 6.12): keyring handling, bundle recipe variables, installed artefacts (no image build)"
# the layer parses without any key (step 3 already did); the keyring variable is empty by default
envdump rauc-conf; envdump velora-bundle-dev; envdump rauc
chk "VELORA_RAUC_KEYRING_FILE is empty by default (layer parses without keys)" rauc-conf VELORA_RAUC_KEYRING_FILE '^$'
chk "rauc-conf SRC_URI has system.conf and fw_env.config but no keyring when unset" rauc-conf SRC_URI '^ *file://system\.conf +file://fw_env\.config *$'
chk "bundle: plain format" velora-bundle-dev RAUC_BUNDLE_FORMAT '^plain$'
chk "bundle: one rootfs slot from velora-image-dev" velora-bundle-dev RAUC_SLOT_rootfs '^velora-image-dev$'
chk "bundle: signing key unset without VELORA_RAUC_SIGN_DIR" velora-bundle-dev RAUC_KEY_FILE '^$'
chk "bundle: compatible equals system.conf" velora-bundle-dev RAUC_BUNDLE_COMPATIBLE '^Velora RK3588$'
if [ -n "$(ev rauc RRECOMMENDS:rauc)" ] && ! ev rauc RRECOMMENDS:rauc | grep -q 'rauc-mark-good'; then pass "RRECOMMENDS:rauc has no rauc-mark-good (stock mark-good not pulled in): $(ev rauc RRECOMMENDS:rauc | head -c 120)"; else fail "RRECOMMENDS:rauc contains rauc-mark-good or is empty (got: $(ev rauc RRECOMMENDS:rauc | head -c 200))"; fi
RAUCDIR="$PROJ/.yocto-work/rauc-test"; rm -rf "$RAUCDIR"; mkdir -p "$RAUCDIR"
# a throw-away PUBLIC certificate (self-signed, generated here, not an image key) and one carrying a private key
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -subj "/CN=validate-only dev root (not for production)" -days 2 -keyout "$RAUCDIR/k.pem" -out "$RAUCDIR/ca.pem" >/dev/null 2>&1
cat "$RAUCDIR/ca.pem" "$RAUCDIR/k.pem" > "$RAUCDIR/ca-with-key.pem"
cat "$RAUCDIR/ca.pem" "$RAUCDIR/ca.pem" > "$RAUCDIR/two-certs.pem"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -subj "/CN=Some Production Root" -days 2 -keyout "$RAUCDIR/k2.pem" -out "$RAUCDIR/prod.pem" >/dev/null 2>&1
: > "$RAUCDIR/empty.pem"
printf 'VELORA_RAUC_KEYRING_FILE = "%s"\n' "$RAUCDIR/ca.pem" > "$RAUCDIR/good.conf"
printf 'VELORA_RAUC_KEYRING_FILE = "%s"\n' "$RAUCDIR/ca-with-key.pem" > "$RAUCDIR/withkey.conf"
printf 'VELORA_RAUC_KEYRING_FILE = "%s"\n' "$RAUCDIR/empty.pem" > "$RAUCDIR/emptyfile.conf"
printf 'VELORA_RAUC_KEYRING_FILE = "%s"\n' "$RAUCDIR/two-certs.pem" > "$RAUCDIR/twocerts.conf"
printf 'VELORA_RAUC_KEYRING_FILE = "%s"\n' "$RAUCDIR/prod.pem" > "$RAUCDIR/prod.conf"
printf 'VELORA_RAUC_KEYRING_FILE = "%s"\n' "$RAUCDIR/does-not-exist.pem" > "$RAUCDIR/missing.conf"
printf 'VELORA_RAUC_SIGN_DIR = "/signing"\nVELORA_RAUC_KEYRING_FILE = "%s"\n' "$RAUCDIR/ca.pem" > "$RAUCDIR/sign.conf"
rm -rf "$PROJ/.yocto-work/build/tmp"
rc_install() { # rc_install <postfile or ""> : do_install of rauc-conf, log in $RAUCDIR/inst.log
  local post=""; [ -n "$1" ] && post="-R $1"
  $KAS shell yocto/kas/velora-dev.yml -c "cd $PROJ/.yocto-work/scratch && bitbake $post -c cleansstate rauc-conf >/dev/null 2>&1; bitbake $post rauc-conf -c install" >"$RAUCDIR/inst.log" 2>&1
}
if rc_install ""; then fail "rauc-conf do_install passed WITHOUT a keyring"; else
  if grep -q "VELORA_RAUC_KEYRING_FILE is not set" "$RAUCDIR/inst.log"; then pass "no keyring: rauc-conf fails with a clear bbfatal"; else fail "no keyring: failed, but not with the expected message"; tail -n 15 "$RAUCDIR/inst.log"; fi; fi
for v in withkey:"contains a private key" emptyfile:"is missing or empty" twocerts:"must hold exactly one PEM certificate" prod:"not for production" missing:"does-not-exist.pem: file could not be found"; do
  n="${v%%:*}"; msg="${v#*:}"
  if rc_install "$RAUCDIR/$n.conf"; then fail "rauc-conf accepted keyring variant '$n'"; elif grep -q "$msg" "$RAUCDIR/inst.log"; then pass "keyring variant '$n' rejected: $msg"; else fail "keyring variant '$n' failed with another message"; tail -n 15 "$RAUCDIR/inst.log"; fi
done
if rc_install "$RAUCDIR/good.conf"; then pass "rauc-conf do_install with a public dev-style certificate"; else fail "rauc-conf do_install with a certificate"; tail -n 25 "$RAUCDIR/inst.log"; fi
RCD=$(ls -d "$PROJ"/.yocto-work/build/tmp/work/*/rauc-conf/*/image 2>/dev/null | head -1)
if [ -n "$RCD" ] && cmp -s "$RCD/usr/lib/rauc/ca.cert.pem" "$RAUCDIR/ca.pem" && ! grep -q "PRIVATE KEY" "$RCD/usr/lib/rauc/ca.cert.pem"; then pass "installed keyring is exactly the supplied certificate and holds no private key"; else fail "installed keyring (dir: '$RCD')"; fi
RCD=$(ls -d "$PROJ"/.yocto-work/build/tmp/work/*/rauc-conf/*/image 2>/dev/null | head -1)
if [ -n "$RCD" ] && cmp -s "$RCD/usr/lib/rauc/system.conf" "$PROJ/yocto/meta-velora/recipes-core/rauc/files/system.conf" && grep -q '^/dev/disk/by-partlabel/uboot_env' "$RCD/etc/fw_env.config"; then pass "installed system.conf is the committed one; fw_env.config targets uboot_env"; else fail "installed system.conf / fw_env.config"; fi
# the signing directory variable reaches the bundle recipe's key and certificate paths
$KAS shell yocto/kas/velora-dev.yml -c "bitbake -R $RAUCDIR/sign.conf -e velora-bundle-dev" > "$LOGDIR/env-bundle-sign.txt" 2>&1
if grep -q '^RAUC_KEY_FILE="/signing/signing.key.pem"' "$LOGDIR/env-bundle-sign.txt" && grep -q '^RAUC_CERT_FILE="/signing/signing.cert.pem"' "$LOGDIR/env-bundle-sign.txt" && grep -q "^BUNDLE_ARGS=\"--keyring=$RAUCDIR/ca.pem\"" "$LOGDIR/env-bundle-sign.txt"; then pass "VELORA_RAUC_SIGN_DIR / keyring reach RAUC_KEY_FILE, RAUC_CERT_FILE and BUNDLE_ARGS"; else fail "bundle recipe signing variables"; grep -E '^(RAUC_KEY_FILE|RAUC_CERT_FILE|BUNDLE_ARGS)=' "$LOGDIR/env-bundle-sign.txt"; fi
# our units, recorder and boot script through do_install
MGREC="$PROJ/yocto/meta-velora/recipes-core/rauc/velora-rauc-mark-good_1.0.bb"; RRREC="$PROJ/yocto/meta-velora/recipes-core/rauc/velora-rollback-record_1.0.bb"; UBREC="$PROJ/yocto/meta-velora/recipes-core/rauc/velora-uboot-ab-script_1.0.bb"
if $KAS shell yocto/kas/velora-dev.yml -c "cd $PROJ/.yocto-work/scratch && bitbake -b $MGREC -c install && bitbake -b $RRREC -c install && bitbake velora-uboot-ab-script -c install" >"$LOGDIR/install-rauc.log" 2>&1; then pass "do_install of velora-rauc-mark-good, velora-rollback-record, velora-uboot-ab-script"; else fail "do_install of the RAUC recipes"; grep -v 'Duplicate inclusion' "$LOGDIR/install-rauc.log" | tail -n 25 | sed 's/^/  | /'; fi
MGD=$(ls -d "$PROJ"/.yocto-work/build/tmp/work/all-*/velora-rauc-mark-good/*/image 2>/dev/null | head -1)
RRD=$(ls -d "$PROJ"/.yocto-work/build/tmp/work/all-*/velora-rollback-record/*/image 2>/dev/null | head -1)
UBD=$(ls -d "$PROJ"/.yocto-work/build/tmp/work/*/velora-uboot-ab-script/*/image 2>/dev/null | head -1)
U="$MGD/usr/lib/systemd/system"; [ -d "$U" ] || U="$MGD/lib/systemd/system"
MGU="$U/velora-rauc-mark-good.service"
okm=1; for t in engine ui sysd; do grep -Eq "^Requires=.*velora-$t-ready\.target" "$MGU" && grep -Eq "^After=.*velora-$t-ready\.target" "$MGU" && [ -f "$U/velora-$t-ready.target" ] || okm=0; done
[ -L "$MGD/etc/systemd/system/rauc-mark-good.service" ] && [ "$(readlink "$MGD/etc/systemd/system/rauc-mark-good.service")" = /dev/null ] && pass "stock rauc-mark-good.service is masked in the image" || fail "rauc-mark-good.service mask missing"
[ "$okm" = 1 ] && pass "installed mark-good unit: Requires= and After= engine, ui and sysd ready targets; the targets are installed" || fail "installed mark-good unit ordering (dir '$MGD')"
if grep -Eq '^WantedBy=.*default\.target' "$MGU" || ! grep -Eq '^ExecStart=/usr/bin/rauc status mark-good$' "$MGU"; then fail "installed mark-good unit: wanted by default.target or no rauc status mark-good"; else pass "installed mark-good unit: runs rauc status mark-good, not wanted by default.target"; fi
if [ -n "$RRD" ] && [ -x "$RRD/usr/libexec/velora/velora-rollback-record" ] && python3 -I -c "import ast,sys; ast.parse(open(sys.argv[1]).read())" "$RRD/usr/libexec/velora/velora-rollback-record"; then pass "installed rollback recorder is executable and parses"; else fail "installed rollback recorder (dir '$RRD')"; fi
if [ -n "$UBD" ] && [ -s "$UBD/boot/boot.scr" ] && file "$UBD/boot/boot.scr" | grep -qi "u-boot legacy uImage"; then pass "boot.scr compiled with mkimage (u-boot-tools-native)"; else fail "boot.scr missing or not a U-Boot script image (dir '$UBD')"; fi
rm -rf "$PROJ/.yocto-work/build/tmp" "$RAUCDIR"

echo "== 6. license policy"
for pair in gpl3:GPL-3.0-only lgpl3:LGPL-3.0-only agpl3:AGPL-3.0-only; do
  t="${pair%%:*}"; lic="${pair#*:}"
  if bb ci/kas-test-layer.yml -n "velora-test-$t-image"; then fail "policy: $t recipe in the image did NOT fail"; tail_log
  elif grep -q "test-$t was skipped: it has incompatible license(s): $lic" "$OUT"; then pass "policy: test-$t ($lic) is skipped as incompatible and the image does not resolve"
  else fail "policy: $t failed, but not with 'test-$t was skipped: it has incompatible license(s): $lic'"; tail_log; fi
done
if bb ci/kas-test-layer.yml -n velora-test-gpl2-image; then pass "policy: GPL-2.0-only recipe (kernel/BusyBox class) is allowed"; else fail "policy: GPL-2.0-only recipe blocked"; tail_log; fi
# GPL-2.0-only (BusyBox, linux-yocto, U-Boot) and GPL-3.0-with-GCC-exception (libgcc, gcc-runtime/libstdc++) carry the policy too in this
# test layer (pn- scoped) and the image still resolves, so they are not blocked:
GRAPH3="$PROJ/.yocto-work/graph-gpl2"; rm -rf "$GRAPH3"; mkdir -p "$GRAPH3"
$KAS shell yocto/ci/kas-test-layer.yml -c "cd $GRAPH3 && bitbake -g velora-test-gpl2-image" >"$LOGDIR/graph3.log" 2>&1
for r in busybox linux-yocto u-boot libgcc gcc-runtime; do
  if grep -qx "$r" "$GRAPH3/pn-buildlist" 2>/dev/null; then pass "policy: $r (policy applied) is in the resolved image and was not blocked"; else fail "policy: $r missing from the resolved image"; fi
done

echo
echo "SUMMARY: $PASSED passed, $FAILED failed"
[ "$FAILED" -eq 0 ]
