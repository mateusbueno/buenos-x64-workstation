#!/usr/bin/env bash
# Story 6.12: runs INSIDE the velora-rauc-test container (see run-tests.sh). Proves, without a boot, what RAUC and our config can prove:
# a dev-signed bundle verifies against the dev keyring, a foreign-signed bundle and a bundle with one flipped byte are rejected, and a
# rejected bundle never touches a slot. Key material is generated here, in a throw-away directory of the container, and dies with it.
set -uo pipefail
SRC="${SRC:-/src/yocto}"
RAUCDIR="$SRC/meta-velora/recipes-core/rauc/files"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
cd "$T"
PASSED=0; FAILED=0
pass() { echo "  PASS  $1"; PASSED=$((PASSED+1)); }
fail() { echo "  FAIL  $1"; FAILED=$((FAILED+1)); }
ok()   { if "${@:2}"; then pass "$1"; else fail "$1"; fi; }

echo "== R1. dev and foreign roots (generated outside the repository, in $T)"
bash "$SRC/ci/rauc/gen-dev-root.sh" --out "$T/dev" --name dev >/dev/null && pass "dev root generated" || fail "dev root generation"
bash "$SRC/ci/rauc/gen-dev-root.sh" --out "$T/foreign" --name foreign >/dev/null && pass "foreign root generated" || fail "foreign root generation"
if bash "$SRC/ci/rauc/gen-dev-root.sh" --out "$SRC/../should-not-exist" >"$T/refuse.log" 2>&1; then fail "generator accepted a directory inside the repository"; else grep -q "inside the repository" "$T/refuse.log" && pass "generator refuses a directory inside the repository" || { fail "generator failed for another reason"; cat "$T/refuse.log"; }; fi
if bash "$SRC/ci/rauc/gen-dev-root.sh" --out "$T/dev" >"$T/force.log" 2>&1; then fail "generator overwrote an existing root without --force"; else pass "generator does not overwrite without --force"; fi
[ "$(stat -c %a "$T/dev/signing.key.pem")" = "600" ] && [ "$(stat -c %a "$T/dev")" = "700" ] && pass "private key 0600 in a 0700 directory" || fail "key permissions"
openssl verify -CAfile "$T/dev/ca.cert.pem" "$T/dev/signing.cert.pem" >/dev/null 2>&1 && ! openssl verify -CAfile "$T/dev/ca.cert.pem" "$T/foreign/signing.cert.pem" >/dev/null 2>&1 \
  && pass "dev root signs the dev certificate and does not vouch for the foreign one" || fail "root/certificate relationship"
! grep -q "PRIVATE KEY" "$T/dev/ca.cert.pem" && pass "the keyring (ca.cert.pem) holds no private key" || fail "keyring holds a private key"

echo "== R2. system.conf (the committed file) parses in RAUC and declares the slots"
CONF="$T/system.conf"
# Only host paths are substituted: keyring, data directory and the two device paths (files here). Everything else is the committed text.
sed -e "s#^path=.*#path=$T/dev/ca.cert.pem#" -e "s#/dev/disk/by-partlabel/rootfs.0#$T/slotA.img#" -e "s#/dev/disk/by-partlabel/rootfs.1#$T/slotB.img#" \
    -e "s#^data-directory=.*#data-directory=$T/data#" "$RAUCDIR/system.conf" > "$CONF"
mkdir -p "$T/data"
grep -q "^bootloader=uboot$" "$CONF" && grep -q "^\[slot.rootfs.0\]" "$CONF" && grep -q "^\[slot.rootfs.1\]" "$CONF" && pass "committed system.conf: uboot backend, slots rootfs.0 and rootfs.1" || fail "system.conf content"
COMPAT="$(sed -n 's/^compatible=//p' "$CONF")"

echo "== R3. bundle build and verification"
mkdir -p rootfs-src bundle-dir
echo 'VARIANT_ID=dev' > rootfs-src/os-release
head -c 3000000 /dev/urandom > rootfs-src/payload.bin
truncate -s 8M bundle-dir/rootfs.ext4
mkfs.ext4 -q -d rootfs-src bundle-dir/rootfs.ext4
# plain format: no [bundle] section (the bundle recipe sets RAUC_BUNDLE_FORMAT = "plain", which writes none either)
printf '[update]\ncompatible=%s\nversion=0.0.1-test\ndescription=Velora test bundle\n\n[image.rootfs]\nfilename=rootfs.ext4\n' "$COMPAT" > bundle-dir/manifest.raucm
# the same flags as the bundle recipe (bundle.bbclass + BUNDLE_ARGS --keyring)
if rauc bundle --cert "$T/dev/signing.cert.pem" --key "$T/dev/signing.key.pem" --keyring "$T/dev/ca.cert.pem" bundle-dir dev.raucb >build.log 2>&1; then pass "rauc bundle builds a dev-signed verity bundle (verified against the dev keyring at creation)"; else fail "dev bundle build"; cat build.log; fi
if rauc --conf "$CONF" info dev.raucb >info.log 2>&1; then pass "rauc info verifies the dev-signed bundle with the dev keyring"; else fail "rauc info on the dev bundle"; cat info.log; fi
grep -Eq "Verified (inline|detached) signature by 'O = Velora, CN = Velora dev RAUC bundle signing'" info.log && pass "rauc info names the dev signing certificate" || fail "signer not reported"
grep -q "Compatible:     '$COMPAT'" info.log && grep -q "Bundle Format:  plain" info.log && pass "bundle compatible string and plain format as configured" || fail "compatible/format"
if grep -Eqi "unknown|invalid key|deprecated" info.log; then fail "rauc reported a problem with system.conf"; grep -Ei "unknown|invalid|deprecated" info.log; else pass "rauc accepted every system.conf key without warning"; fi

echo "== R4. foreign-signed bundle"
if rauc bundle --cert "$T/foreign/signing.cert.pem" --key "$T/foreign/signing.key.pem" --keyring "$T/dev/ca.cert.pem" bundle-dir foreign-checked.raucb >fbuild.log 2>&1; then fail "bundle build with the foreign key passed the dev keyring check"; else pass "rauc bundle --keyring (the recipe's BUNDLE_ARGS) refuses to build a bundle the dev keyring would reject"; fi
rauc bundle --cert "$T/foreign/signing.cert.pem" --key "$T/foreign/signing.key.pem" bundle-dir foreign.raucb >/dev/null 2>&1 || fail "could not build the foreign-signed bundle"
if rauc --conf "$CONF" info foreign.raucb >finfo.log 2>&1; then fail "foreign-signed bundle was ACCEPTED by rauc info"; else grep -q "signature verification failed" finfo.log && pass "foreign-signed bundle rejected by rauc info (signature verification failed)" || { fail "foreign bundle rejected for another reason"; cat finfo.log; }; fi

echo "== R5. one flipped byte: rauc info must reject EVERY flip (plain format: signature over the whole payload hash)"
SIZE=$(stat -c %s dev.raucb)
SIGLEN=$(tail -c 8 dev.raucb | od -An -tu8 --endian=big | tr -d ' ')
DATASIZE=$((SIZE-8-SIGLEN))
# plain layout of a RAUC bundle: payload (squashfs) | CMS signature | 8-byte signature length
flip() { cp dev.raucb "$2"; local b; b=$(dd if="$2" bs=1 skip="$1" count=1 2>/dev/null | od -An -tu1 | tr -d ' '); printf "$(printf '\\%03o' $(( (b ^ 85) & 255 )))" | dd of="$2" bs=1 seek="$1" conv=notrunc 2>/dev/null; }
echo "  (payload $DATASIZE B, signature $SIGLEN B)"
FLIPS="signature:$((SIZE-8-SIGLEN/2)) signature-length-trailer:$((SIZE-3)) payload-start:100 payload-middle:$((DATASIZE/2)) payload-end:$((DATASIZE-1))"
for spec in $FLIPS; do
  name="${spec%%:*}"; off="${spec#*:}"; f="flip-$name.raucb"
  flip "$off" "$f"
  cmp -s dev.raucb "$f" && { fail "flip $name: file unchanged"; continue; }
  if rauc --conf "$CONF" info "$f" >"flip-$name.log" 2>&1; then fail "flip $name (offset $off): ACCEPTED by rauc info"; else pass "flip $name (offset $off): rejected by rauc info ($(grep -m1 -iE 'failed|exceeds|invalid|mismatch' flip-$name.log | cut -c1-100))"; fi
done

echo "== R6. rauc install: rejected bundles are rejected at verification, before mount or any slot write"
# The decisive evidence is the install LOG: each rejected install must end with a verification failure and must never reach the stages that
# follow verification (bundle mount, compatible check, slot writes). "Slots byte-identical" alone is NOT evidence: this container cannot mount a
# bundle, so even a bundle that passed verification would leave the slots untouched. It is only meaningful together with the log check.
head -c 4194304 /dev/urandom > slotA.img; head -c 4194304 /dev/urandom > slotB.img
HA="$(sha256sum slotA.img | cut -d' ' -f1)"; HB="$(sha256sum slotB.img | cut -d' ' -f1)"
slots_unchanged() { [ "$(sha256sum slotA.img | cut -d' ' -f1)" = "$HA" ] && [ "$(sha256sum slotB.img | cut -d' ' -f1)" = "$HB" ]; }
inst() { rauc --conf "$CONF" install --override-boot-slot=A "$1" >"inst-$(basename "$1").log" 2>&1; }
LATER='Verifying signature done|Checking bundle done|[Mm]ount(ing)? (path|point)|Failed mounting|ompatible|Updating slot|Copying|Writing|Marking'
rejected() { # rejected <bundle> <message regex> <description>
  local log="inst-$(basename "$1").log"
  if inst "$1"; then fail "rauc install ACCEPTED $3"; return; fi
  if ! grep -Eq "$2" "$log"; then fail "$3: install failed, but without the expected verification failure /$2/"; sed 's/^/    | /' "$log"; return; fi
  if grep -Eq "$LATER" "$log"; then fail "$3: install got past verification (/$LATER/ in the log)"; sed 's/^/    | /' "$log"; return; fi
  if ! slots_unchanged; then fail "$3: slot images changed"; return; fi
  pass "rauc install rejects $3 at verification (log: $(grep -m1 -E "$2" "$log" | cut -c1-80)); no mount/compatible/write stage reached; slots byte-identical"
}
rejected foreign.raucb 'failed: signature verification failed: Verify error: unable to get local issuer certificate' "the foreign-signed bundle"
rejected flip-signature.raucb 'failed: signature verification failed: ' "a flipped signature byte"
rejected flip-signature-length-trailer.raucb 'failed: Invalid bundle format: Signature size \([0-9]+\) exceeds bundle size' "a flipped signature-length trailer byte"
for n in payload-start payload-middle payload-end; do rejected "flip-$n.raucb" 'CMS_verify:content verify error' "a flipped $n byte (payload digest mismatch)"; done
# positive control: the same command on the valid bundle passes verification and reaches the stages after it (it stops at the mount, which
# needs root, loop and a board); this proves the log patterns above are the ones that appear when verification succeeds.
inst dev.raucb || true
if grep -Eq "Verified (inline|detached) signature by 'O = Velora, CN = Velora dev RAUC bundle signing'" inst-dev.raucb.log && grep -Eq "$LATER" inst-dev.raucb.log && ! grep -q "signature verification failed" inst-dev.raucb.log; then pass "positive control: the dev-signed bundle passes verification and reaches the mount stage (install itself is board-only)"; else fail "positive control: dev bundle install log"; cat inst-dev.raucb.log; fi

echo "== R7. status JSON for the rollback recorder test"
mkdir -p status
mk_status() { # mk_status <name> <booted bootname> : central status says rootfs.1 got 1.1.0 (newer) and rootfs.0 holds 1.0.0
  local d="$T/status/$1-data"; mkdir -p "$d"
  cat > "$d/central.raucs" <<EOT
[slot.rootfs.0]
bundle.compatible=$COMPAT
bundle.version=1.0.0
status=ok
installed.timestamp=2026-10-20T10:00:00Z
installed.count=1

[slot.rootfs.1]
bundle.compatible=$COMPAT
bundle.version=1.1.0
status=ok
installed.timestamp=2026-10-25T10:00:00Z
installed.count=1
EOT
  sed -e "s#^bootloader=.*#bootloader=noop#" -e "s#^data-directory=.*#data-directory=$d#" "$CONF" > "$T/status/$1.conf"
  rauc --conf "$T/status/$1.conf" --override-boot-slot="$2" status --detailed --output-format=json-pretty > "$T/status/$1.json" 2>/dev/null
}
mk_status_flashed() { # slot A was flashed by wic (no RAUC record), only rootfs.1 has a record; <name> <booted bootname> <with central status: 1|0>
  local d="$T/status/$1-data"; mkdir -p "$d"
  if [ "$3" = 1 ]; then printf '[slot.rootfs.1]\nbundle.compatible=%s\nbundle.version=1.1.0\nstatus=ok\ninstalled.timestamp=2026-10-25T10:00:00Z\ninstalled.count=1\n' "$COMPAT" > "$d/central.raucs"; fi
  sed -e "s#^bootloader=.*#bootloader=noop#" -e "s#^data-directory=.*#data-directory=$d#" "$CONF" > "$T/status/$1.conf"
  rauc --conf "$T/status/$1.conf" --override-boot-slot="$2" status --detailed --output-format=json-pretty > "$T/status/$1.json" 2>/dev/null
}
mk_status_flashed flashed-rolledback A 1   # wic-flashed A (no version) booted, B (1.1.0) was attempted
mk_status_flashed flashed-fresh A 0        # never updated: no central status file at all
mk_status rolledback A   # slot B (1.1.0) was attempted, but RAUC booted slot A (1.0.0)
mk_status updated B      # slot B (1.1.0) booted
[ -s status/rolledback.json ] && [ -s status/updated.json ] && [ -s status/flashed-rolledback.json ] && [ -s status/flashed-fresh.json ] && pass "real rauc status JSON produced (noop backend, committed slot names)" || fail "status JSON not produced"

echo "== R8. rollback recorder (unit tests, real rauc JSON)"
if python3 -I "$SRC/ci/rauc/test_rollback_record.py" "$SRC/meta-velora/recipes-core/rauc/files/rollback/velora-rollback-record" "$T/status" >rb.log 2>&1; then pass "rollback recorder: $(tail -1 rb.log)"; else fail "rollback recorder tests"; cat rb.log; fi

echo "== R9. U-Boot A/B boot script logic (stub U-Boot commands)"
if bash "$SRC/ci/rauc/test_boot_script.sh" "$SRC/meta-velora/files/uboot/velora-ab-boot.cmd" >ub.log 2>&1; then pass "boot script: $(tail -1 ub.log)"; else fail "boot script logic"; cat ub.log; fi

echo "== R10. bundle recipe's signing-file check (the recipe's own shell function, run with a stub bbfatal)"
python3 - "$SRC/meta-velora/recipes-core/bundles/velora-bundle-dev.bb" "$T/check.sh" <<'PYEOF'
import re, sys
s = open(sys.argv[1]).read()
m = re.search(r"^velora_check_signing_files\(\) \{\n(.*?)^\}\n", s, re.S | re.M)
body = m.group(1).replace("${STAGING_BINDIR_NATIVE}", "/usr/bin")
open(sys.argv[2], "w").write("bbfatal() { echo \"FATAL: $*\"; exit 1; }\n" + body)
PYEOF
sign_check() { # sign_check <keyring> <key> <cert> -> output in sc.log
  ( VELORA_RAUC_KEYRING_FILE="$1" RAUC_KEY_FILE="$2" RAUC_CERT_FILE="$3" bash "$T/check.sh" ) >sc.log 2>&1
}
sign_check "$T/dev/ca.cert.pem" "$T/dev/signing.key.pem" "$T/dev/signing.cert.pem" && pass "signing check passes for dev key, dev certificate and dev keyring" || { fail "valid signing set rejected"; cat sc.log; }
sign_check "" "$T/dev/signing.key.pem" "$T/dev/signing.cert.pem" && fail "accepted an unset keyring" || { grep -q "VELORA_RAUC_KEYRING_FILE is not set" sc.log && pass "unset keyring: bbfatal" || fail "unset keyring: wrong message"; }
sign_check "$T/dev/ca.cert.pem" "$T/foreign/signing.key.pem" "$T/foreign/signing.cert.pem" && fail "accepted a foreign certificate against the dev keyring" || { grep -q "does not verify against the keyring" sc.log && pass "certificate from a root outside the keyring: bbfatal" || { fail "foreign cert: wrong message"; cat sc.log; }; }
sign_check "$T/dev/ca.cert.pem" "$T/foreign/signing.key.pem" "$T/dev/signing.cert.pem" && fail "accepted a key that does not match the certificate" || { grep -q "does not match the certificate" sc.log && pass "key not matching the certificate: bbfatal" || { fail "key mismatch: wrong message"; cat sc.log; }; }
sign_check "$T/dev/ca.cert.pem" "$T/nokey.pem" "$T/dev/signing.cert.pem" && fail "accepted a missing key file" || { grep -q "missing or empty" sc.log && pass "missing key file: bbfatal" || fail "missing key: wrong message"; }

echo
echo "RAUC SUMMARY: $PASSED passed, $FAILED failed"
[ "$FAILED" -eq 0 ]
