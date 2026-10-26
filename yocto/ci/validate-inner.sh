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
for r in linux-yocto systemd openssh busybox libgcc gcc-runtime velora-system-config os-release u-boot; do
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
envdump velora-image-dev; envdump os-release; envdump linux-yocto
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

chk "ROOTFS_POSTPROCESS_COMMAND runs velora_dev_authorized_keys" velora-image-dev ROOTFS_POSTPROCESS_COMMAND 'velora_dev_authorized_keys'
echo "  -- generated SSH overlay (dummy key) parses with kas dump"
echo "ssh-ed25519 AAAAdummykeyfortestsonly validate" > "$PROJ/.yocto-work/dummy_authorized_keys"
bash "$PROJ/yocto/ci/make-ssh-overlay.sh" "$PROJ" "$PROJ/.yocto-work/dummy_authorized_keys" "$PROJ/.yocto-work/kas-ssh.yml"
if $KAS dump "$PROJ/.yocto-work/kas-ssh.yml" > "$LOGDIR/ssh-dump.yml" 2>"$LOGDIR/ssh-dump.err" && grep -q "VELORA_DEV_AUTHORIZED_KEYS_FILE" "$LOGDIR/ssh-dump.yml"; then pass "kas dump of generated SSH overlay"; else fail "kas dump of generated SSH overlay"; tail -n 15 "$LOGDIR/ssh-dump.err"; fi

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
