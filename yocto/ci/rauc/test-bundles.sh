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
bash "$SRC/ci/rauc/gen-dev-root.sh" --out "$T/dev" --variant dev >/dev/null && pass "dev root generated" || fail "dev root generation"
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

echo "== R10. bundle recipe's signing-file check (the shared recipe's own shell function, run with a stub bbfatal)"
python3 -I "$SRC/ci/extract_bb_func.py" "$SRC/meta-velora/recipes-core/bundles/velora-bundle-base.inc" velora_check_signing_files "$T/check.sh" STAGING_BINDIR_NATIVE=/usr/bin PN=velora-bundle-test \
  && pass "check function extracted from velora-bundle-base.inc" || fail "cannot extract the signing check"
sign_check() { # sign_check <variant> <keyring> <key> <cert> [build variant, default = variant] -> output in sc.log
  ( VELORA_BUNDLE_VARIANT="$1" VELORA_VARIANT="${5-$1}" VELORA_RAUC_KEYRING_FILE="$2" RAUC_KEY_FILE="$3" RAUC_CERT_FILE="$4" bash "$T/check.sh" ) >sc.log 2>&1
}
sign_check dev "$T/dev/ca.cert.pem" "$T/dev/signing.key.pem" "$T/dev/signing.cert.pem" && pass "signing check passes for dev key, dev certificate and dev keyring" || { fail "valid signing set rejected"; cat sc.log; }
sign_check dev "" "$T/dev/signing.key.pem" "$T/dev/signing.cert.pem" && fail "accepted an unset keyring" || { grep -q "VELORA_RAUC_KEYRING_FILE is not set" sc.log && pass "unset keyring: bbfatal" || fail "unset keyring: wrong message"; }
sign_check dev "$T/dev/ca.cert.pem" "$T/foreign/signing.key.pem" "$T/foreign/signing.cert.pem" && fail "accepted a foreign certificate against the dev keyring" || { grep -q "does not verify against the keyring" sc.log && pass "certificate from a root outside the keyring: bbfatal" || { fail "foreign cert: wrong message"; cat sc.log; }; }
sign_check dev "$T/dev/ca.cert.pem" "$T/foreign/signing.key.pem" "$T/dev/signing.cert.pem" && fail "accepted a key that does not match the certificate" || { grep -q "does not match the certificate" sc.log && pass "key not matching the certificate: bbfatal" || { fail "key mismatch: wrong message"; cat sc.log; }; }
sign_check dev "$T/dev/ca.cert.pem" "$T/nokey.pem" "$T/dev/signing.cert.pem" && fail "accepted a missing key file" || { grep -q "missing or empty" sc.log && pass "missing key file: bbfatal" || fail "missing key: wrong message"; }

echo "== R11. per-variant roots: dev and factory-test (Story 6.13)"
GEN="$SRC/ci/rauc/gen-dev-root.sh"
bash "$GEN" --variant dev --out "$T/vdev" >/dev/null && pass "generator: --variant dev" || fail "generator --variant dev"
bash "$GEN" --variant factory-test --out "$T/vft" >/dev/null && pass "generator: --variant factory-test" || fail "generator --variant factory-test"
for bad in production prod PRODUCTION other; do
  if bash "$GEN" --variant "$bad" --out "$T/vbad" >"$T/vbad.log" 2>&1; then fail "generator accepted --variant '$bad'"; else
    { grep -q "refusing variant" "$T/vbad.log" && [ ! -e "$T/vbad" ] && pass "generator refuses --variant $bad and writes nothing"; } || fail "generator --variant $bad: wrong failure"
  fi
done
for rn in factory-test dev prod Production FACTORY-TEST; do
  if bash "$GEN" --name "$rn" --out "$T/vres" >"$T/vres.log" 2>&1; then fail "generator accepted the reserved --name $rn"; else { grep -q "reserved" "$T/vres.log" && [ ! -e "$T/vres" ] && pass "generator refuses the reserved --name $rn and writes nothing"; } || fail "generator --name $rn: wrong failure"; fi
done
if bash "$GEN" --variant dev --name foo --out "$T/vres" >"$T/vres.log" 2>&1; then fail "generator accepted --variant with --name"; else { grep -q "cannot be combined" "$T/vres.log" && [ ! -e "$T/vres" ] && pass "generator rejects --variant together with --name"; } || fail "--variant with --name: wrong failure"; fi
if bash "$GEN" --variant factory-test >"$T/vnoout.log" 2>&1; then fail "factory-test root generated without an explicit --out"; else grep -q "explicit --out" "$T/vnoout.log" && pass "factory-test needs an explicit --out" || fail "factory-test without --out: wrong message"; fi
if bash "$GEN" --variant factory-test --out "$SRC/../should-not-exist" >"$T/vrepo.log" 2>&1; then fail "factory-test root accepted inside the repository"; else grep -q "inside the repository" "$T/vrepo.log" && pass "factory-test root refused inside the repository" || fail "factory-test repo refusal: wrong message"; fi
openssl x509 -in "$T/vdev/ca.cert.pem" -noout -subject | grep -q "Velora dev RAUC root (not for production)" && openssl x509 -in "$T/vft/ca.cert.pem" -noout -subject | grep -q "Velora factory-test RAUC root (not for production)" \
  && pass "root subjects carry their variant marker" || fail "subject markers"
! openssl x509 -in "$T/vft/ca.cert.pem" -noout -subject | grep -q "Velora dev RAUC root" && ! openssl x509 -in "$T/vdev/ca.cert.pem" -noout -subject | grep -q "Velora factory-test RAUC root" && pass "no variant marker matches the other root" || fail "marker overlap"
[ -f "$T/vft/FACTORY-TEST-ONLY.txt" ] && [ ! -e "$T/vft/DEV-ONLY.txt" ] && [ -f "$T/vdev/DEV-ONLY.txt" ] && pass "variant label files" || fail "label files"
[ "$(stat -c %a "$T/vft/signing.key.pem")" = "600" ] && [ "$(stat -c %a "$T/vft/ca.key.pem")" = "600" ] && [ "$(stat -c %a "$T/vft")" = "700" ] && pass "factory-test private keys 0600 in a 0700 directory" || fail "factory-test key permissions"
D_DEV=$(( ($(date -d "$(openssl x509 -in "$T/vdev/ca.cert.pem" -noout -enddate | cut -d= -f2)" +%s) - $(date +%s)) / 86400 ))
D_FT=$(( ($(date -d "$(openssl x509 -in "$T/vft/ca.cert.pem" -noout -enddate | cut -d= -f2)" +%s) - $(date +%s)) / 86400 ))
[ "$D_FT" -ge 360 ] && [ "$D_FT" -le 366 ] && [ "$D_DEV" -gt 3000 ] && pass "validity: factory-test root 365 days, dev root 3650 days" || fail "validity ($D_DEV / $D_FT days)"
openssl verify -CAfile "$T/vft/ca.cert.pem" "$T/vft/signing.cert.pem" >/dev/null 2>&1 && ! openssl verify -CAfile "$T/vft/ca.cert.pem" "$T/vdev/signing.cert.pem" >/dev/null 2>&1 \
  && ! openssl verify -CAfile "$T/vdev/ca.cert.pem" "$T/vft/signing.cert.pem" >/dev/null 2>&1 && pass "each root signs only its own signing certificate" || fail "root/certificate relationship across variants"
! grep -q "PRIVATE KEY" "$T/vft/ca.cert.pem" && pass "the factory-test keyring holds no private key" || fail "factory-test keyring holds a private key"

mkconf() { # mkconf <out> <keyring>
  sed -e "s#^path=.*#path=$2#" -e "s#/dev/disk/by-partlabel/rootfs.0#$T/slotA.img#" -e "s#/dev/disk/by-partlabel/rootfs.1#$T/slotB.img#" -e "s#^data-directory=.*#data-directory=$T/data#" "$RAUCDIR/system.conf" > "$1"
}
mkconf "$T/devimg.conf" "$T/vdev/ca.cert.pem"   # what the dev image carries
mkconf "$T/ftimg.conf" "$T/vft/ca.cert.pem"     # what the factory-test image carries
cmp -s <(grep -v '^path=' "$T/devimg.conf") <(grep -v '^path=' "$T/ftimg.conf") && pass "both images use the same system.conf apart from the keyring file" || fail "system.conf differs"
build_variant() { # build_variant <dir> <outfile>
  rauc bundle --cert "$1/signing.cert.pem" --key "$1/signing.key.pem" --keyring "$1/ca.cert.pem" bundle-dir "$2" >"$2.log" 2>&1
}
build_variant "$T/vdev" vdev.raucb && pass "dev bundle builds, verified against the dev keyring" || { fail "dev bundle build"; cat vdev.raucb.log; }
build_variant "$T/vft" vft.raucb && pass "factory-test bundle builds, verified against the factory-test keyring" || { fail "factory-test bundle build"; cat vft.raucb.log; }
if rauc bundle --cert "$T/vdev/signing.cert.pem" --key "$T/vdev/signing.key.pem" --keyring "$T/vft/ca.cert.pem" bundle-dir x1.raucb >x1b.log 2>&1; then fail "dev-signed bundle build passed the factory-test keyring check"; else grep -q "signature verification failed" x1b.log && pass "rauc bundle --keyring refuses a dev-signed bundle against the factory-test keyring (signature verification failed)" || { fail "dev bundle vs factory keyring at build: wrong failure"; cat x1b.log; }; fi
if rauc bundle --cert "$T/vft/signing.cert.pem" --key "$T/vft/signing.key.pem" --keyring "$T/vdev/ca.cert.pem" bundle-dir x2.raucb >x2b.log 2>&1; then fail "factory-test-signed bundle build passed the dev keyring check"; else grep -q "signature verification failed" x2b.log && pass "rauc bundle --keyring refuses a factory-test-signed bundle against the dev keyring (signature verification failed)" || { fail "factory bundle vs dev keyring at build: wrong failure"; cat x2b.log; }; fi
# own root: verifies
rauc --conf "$T/devimg.conf" info vdev.raucb >o1.log 2>&1 && grep -q "Verified .*signature by 'O = Velora, CN = Velora dev RAUC bundle signing'" o1.log && pass "dev bundle verifies against the dev keyring" || { fail "dev bundle vs dev keyring"; cat o1.log; }
rauc --conf "$T/ftimg.conf" info vft.raucb >o2.log 2>&1 && grep -q "Verified .*signature by 'O = Velora, CN = Velora factory-test RAUC bundle signing'" o2.log && pass "factory-test bundle verifies against the factory-test keyring" || { fail "factory-test bundle vs its keyring"; cat o2.log; }
# cross-root, both directions: rauc info and rauc install; install rejected at verification, before mount or any slot write (R6's check, other conf)
rauc --conf "$T/ftimg.conf" info vdev.raucb >x3.log 2>&1 && fail "factory-test keyring ACCEPTED a dev-signed bundle" || { grep -q "signature verification failed: Verify error: unable to get local issuer certificate" x3.log && pass "dev-signed bundle rejected by rauc info against the factory-test keyring" || { fail "dev bundle vs factory-test keyring: wrong failure"; cat x3.log; }; }
rauc --conf "$T/devimg.conf" info vft.raucb >x4.log 2>&1 && fail "dev keyring ACCEPTED a factory-test-signed bundle" || { grep -q "signature verification failed: Verify error: unable to get local issuer certificate" x4.log && pass "factory-test-signed bundle rejected by rauc info against the dev keyring" || { fail "factory-test bundle vs dev keyring: wrong failure"; cat x4.log; }; }
CONF_R6="$CONF"
CONF="$T/ftimg.conf"; cp vdev.raucb dev-on-ft.raucb
rejected dev-on-ft.raucb 'failed: signature verification failed: Verify error: unable to get local issuer certificate' "a dev-signed bundle on the factory-test image"
CONF="$T/devimg.conf"; cp vft.raucb ft-on-dev.raucb
rejected ft-on-dev.raucb 'failed: signature verification failed: Verify error: unable to get local issuer certificate' "a factory-test-signed bundle on the dev image"
CONF="$T/ftimg.conf"; cp vft.raucb ft-on-ft.raucb
inst ft-on-ft.raucb || true
if grep -Eq "Verified (inline|detached) signature by 'O = Velora, CN = Velora factory-test RAUC bundle signing'" inst-ft-on-ft.raucb.log && grep -Eq "$LATER" inst-ft-on-ft.raucb.log && ! grep -q "signature verification failed" inst-ft-on-ft.raucb.log; then pass "positive control: the factory-test bundle passes verification on the factory-test image and reaches the mount stage"; else fail "positive control: factory-test install log"; cat inst-ft-on-ft.raucb.log; fi
CONF="$T/devimg.conf"; cp vdev.raucb dev-on-dev.raucb
inst dev-on-dev.raucb || true
if grep -Eq "Verified (inline|detached) signature by 'O = Velora, CN = Velora dev RAUC bundle signing'" inst-dev-on-dev.raucb.log && grep -Eq "$LATER" inst-dev-on-dev.raucb.log && ! grep -q "signature verification failed" inst-dev-on-dev.raucb.log; then pass "positive control: the dev bundle passes verification on the dev image and reaches the mount stage"; else fail "positive control: dev install log"; cat inst-dev-on-dev.raucb.log; fi
CONF="$CONF_R6"
slots_unchanged && pass "slots still byte-identical after every cross-root and own-root install attempt" || fail "slots changed during the cross-root tests"
# the shared bundle recipe check, per variant
python3 -I "$SRC/ci/extract_bb_func.py" "$SRC/meta-velora/recipes-core/bundles/velora-bundle-base.inc" velora_check_signing_files "$T/check.sh" STAGING_BINDIR_NATIVE=/usr/bin PN=velora-bundle-test
sign_check factory-test "$T/vft/ca.cert.pem" "$T/vft/signing.key.pem" "$T/vft/signing.cert.pem" && pass "signing check passes for the factory-test key, certificate and keyring" || { fail "factory-test signing set rejected"; cat sc.log; }
sign_check dev "$T/vft/ca.cert.pem" "$T/vft/signing.key.pem" "$T/vft/signing.cert.pem" && fail "dev bundle accepted the factory-test keyring and key" || { grep -q "is not the dev RAUC root" sc.log && pass "dev bundle with the factory-test keyring and key: bbfatal (marker)" || { fail "dev bundle/factory keyring: wrong message"; cat sc.log; }; }
sign_check factory-test "$T/vdev/ca.cert.pem" "$T/vdev/signing.key.pem" "$T/vdev/signing.cert.pem" && fail "factory-test bundle accepted the dev keyring and key" || { grep -q "is not the factory-test RAUC root" sc.log && pass "factory-test bundle with the dev keyring and key: bbfatal (marker)" || { fail "factory-test bundle/dev keyring: wrong message"; cat sc.log; }; }
sign_check factory-test "$T/vft/ca.cert.pem" "$T/vdev/signing.key.pem" "$T/vdev/signing.cert.pem" && fail "factory-test keyring accepted a dev-signed signing set" || { grep -q "does not verify against the keyring" sc.log && pass "factory-test keyring with the dev signing certificate: bbfatal (chain)" || { fail "dev cert/factory keyring: wrong message"; cat sc.log; }; }
sign_check dev "$T/vdev/ca.cert.pem" "$T/vdev/signing.key.pem" "$T/vdev/signing.cert.pem" factory-test && fail "dev bundle built in a factory-test configuration" || { grep -q "is for variant 'dev' but this build is VELORA_VARIANT='factory-test'" sc.log && pass "bundle variant must equal VELORA_VARIANT: bbfatal" || { fail "variant mismatch: wrong message"; cat sc.log; }; }

echo "== R11b. keyring format and identity checks of the shared bundle check (same rules as rauc-conf)"
KR="$T/kr"; mkdir -p "$KR"
mkroot() { # mkroot <file> <subject> : self-signed root with that subject
  openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -subj "$2" -days 2 -keyout "$KR/k.tmp" -out "$1" >/dev/null 2>&1
}
DEVSUBJ="/O=Velora/CN=Velora dev RAUC root (not for production)"
openssl x509 -in "$T/vdev/ca.cert.pem" -noout -subject -nameopt RFC2253 | grep -qx "subject=CN=Velora dev RAUC root (not for production),O=Velora" && pass "real generated dev root subject (RFC 2253) is exactly CN=...,O=Velora" || { fail "generated subject format"; openssl x509 -in "$T/vdev/ca.cert.pem" -noout -subject -nameopt RFC2253; }
cat "$T/vdev/ca.cert.pem" "$T/vdev/ca.cert.pem" > "$KR/two.pem"
cat "$T/vdev/ca.cert.pem" > "$KR/x509.pem"; sed 's/CERTIFICATE/X509 CERTIFICATE/' "$T/vdev/ca.cert.pem" >> "$KR/x509.pem"
cat "$T/vdev/ca.cert.pem" > "$KR/trusted.pem"; sed 's/CERTIFICATE/TRUSTED CERTIFICATE/' "$T/vdev/ca.cert.pem" >> "$KR/trusted.pem"
sed 's/CERTIFICATE/X509 CERTIFICATE/' "$T/vdev/ca.cert.pem" > "$KR/onlyx509.pem"
mkroot "$KR/o.pem" "/O=Velora dev RAUC root (not for production)/CN=x"
mkroot "$KR/prefix.pem" "/O=Velora/CN=evil Velora dev RAUC root (not for production)"
mkroot "$KR/suffix.pem" "/O=Velora/CN=Velora dev RAUC root (not for production) extra"
mkroot "$KR/ou.pem" "/O=Velora/OU=x/CN=Velora dev RAUC root (not for production)"
mkroot "$KR/noprod.pem" "/O=Velora/CN=Velora dev RAUC root"
# right subject but issued by another CA (not self-signed)
openssl req -new -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -subj "$DEVSUBJ" -keyout "$KR/ns.key" -out "$KR/ns.csr" >/dev/null 2>&1
openssl x509 -req -in "$KR/ns.csr" -CA "$T/foreign/ca.cert.pem" -CAkey "$T/foreign/ca.key.pem" -CAcreateserial -days 2 -out "$KR/notself.pem" >/dev/null 2>&1
for v in two:"exactly one PEM certificate" x509:"exactly one PEM certificate" trusted:"exactly one PEM certificate" onlyx509:"exactly one PEM certificate" \
         o:"expected exactly" prefix:"expected exactly" suffix:"expected exactly" ou:"expected exactly" noprod:"expected exactly" notself:"not a self-signed root"; do
  n="${v%%:*}"; msg="${v#*:}"
  [ -s "$KR/$n.pem" ] || { fail "keyring variant '$n' could not be generated"; continue; }
  if sign_check dev "$KR/$n.pem" "$T/vdev/signing.key.pem" "$T/vdev/signing.cert.pem"; then fail "bundle check accepted keyring variant '$n'"; elif grep -q "$msg" sc.log; then pass "bundle check rejects keyring variant '$n' ($msg)"; else fail "keyring variant '$n': wrong message"; cat sc.log; fi
done
sign_check dev "$T/vdev/ca.cert.pem" "$T/vdev/signing.key.pem" "$T/vdev/signing.cert.pem" && pass "bundle check still accepts the real dev root" || { fail "real dev root rejected"; cat sc.log; }
for bv in prod other ""; do
  if sign_check dev "$T/vdev/ca.cert.pem" "$T/vdev/signing.key.pem" "$T/vdev/signing.cert.pem" "$bv"; then fail "bundle check accepted VELORA_VARIANT='$bv'"; elif grep -Eq "not one of dev, factory-test, prod|Story 6.14" sc.log; then pass "bundle check fails closed for VELORA_VARIANT='$bv'"; else fail "VELORA_VARIANT '$bv': wrong message"; cat sc.log; fi
done

echo "== R12. image post-processing of the factory-test variant (the recipe's own shell functions, fake rootfs)"
FIMG="$SRC/meta-velora/recipes-core/images/velora-image-factory-test.bb"
mkfr() { rm -rf "$T/fr"; mkdir -p "$T/fr/etc/systemd/system/getty.target.wants" "$T/fr/usr/sbin" "$T/fr/usr/lib/systemd/system"; }
extract_img() { python3 -I "$SRC/ci/extract_bb_func.py" "$FIMG" "$1" "$T/img-$1.sh" IMAGE_ROOTFS="$T/fr" sysconfdir=/etc sbindir=/usr/sbin systemd_system_unitdir=/usr/lib/systemd/system SERIAL_CONSOLES='1500000;ttyS2' "VELORA_DEV_AUTHORIZED_KEYS_FILE=$2"; }
extract_img velora_factory_assert_no_ssh "" && extract_img velora_factory_no_login ""
mkfr; bash "$T/img-velora_factory_assert_no_ssh.sh" >fa.log 2>&1 && pass "no-SSH assertion passes on a rootfs without SSH" || { fail "no-SSH assertion on a clean rootfs"; cat fa.log; }
for f in usr/sbin/sshd etc/ssh/sshd_config home/root/.ssh usr/lib/systemd/system/sshd.service usr/lib/systemd/system/sshd.socket usr/lib/systemd/system/sshd@.service; do
  mkfr; mkdir -p "$T/fr/$(dirname "$f")"; case "$f" in */.ssh) mkdir -p "$T/fr/$f";; *) : > "$T/fr/$f";; esac
  if bash "$T/img-velora_factory_assert_no_ssh.sh" >fa.log 2>&1; then fail "no-SSH assertion missed $f"; else grep -q "contains SSH material: $T/fr/$f" fa.log && pass "no-SSH assertion rejects a rootfs with $f" || { fail "no-SSH assertion: wrong message for $f"; cat fa.log; }; fi
done
for f in usr/bin/authorized_keys2 var/lib/x/authorized_keys.bak usr/sbin/sshd-keygen etc/pam.d/sshd usr/sbin/dropbearmulti usr/bin/dropbear etc/dropbear usr/lib/systemd/system/ssh-foo.socket usr/lib/systemd/system/ssh-foo.service usr/lib/systemd/system/sshd-keygen.service; do
  mkfr; mkdir -p "$T/fr/$(dirname "$f")"; : > "$T/fr/$f"
  if bash "$T/img-velora_factory_assert_no_ssh.sh" >fa.log 2>&1; then fail "no-SSH find pass missed $f"; else grep -q "contains SSH material: $T/fr/$f" fa.log && pass "no-SSH find pass rejects a rootfs with $f" || { fail "no-SSH find pass: wrong message for $f"; cat fa.log; }; fi
done
mkfr; mkdir -p "$T/fr/usr/bin"; : > "$T/fr/usr/bin/ssh-keygen"; : > "$T/fr/usr/bin/scp"
bash "$T/img-velora_factory_assert_no_ssh.sh" >fa.log 2>&1 && pass "no-SSH assertion does not reject unrelated names (ssh-keygen, scp)" || { fail "no-SSH assertion over-matches"; cat fa.log; }
python3 -I "$SRC/ci/extract_bb_func.py" "$SRC/meta-velora/recipes-core/images/velora-image-base.inc" velora_assert_root_locked "$T/root-locked.sh" IMAGE_ROOTFS="$T/fr" sysconfdir=/etc
rootpw() { mkfr; mkdir -p "$T/fr/etc"; printf '%s\n' "$1" > "$T/fr/etc/shadow"; bash "$T/root-locked.sh" >rl.log 2>&1; }
for line in 'root:*:19000:0:99999:7:::' 'root:!:19000:0:99999:7:::' 'root:!!:19000:0:99999:7:::'; do rootpw "$line" && pass "root lock assertion accepts '${line%%:19000*}'" || { fail "root lock rejected $line"; cat rl.log; }; done
for line in 'root::19000:0:99999:7:::' 'root:$6$abc$def:19000:0:99999:7:::' 'daemon:*:19000:0:99999:7:::'; do if rootpw "$line"; then fail "root lock assertion accepted '$line'"; else grep -q "not locked" rl.log && pass "root lock assertion rejects '${line%%:19000*}'" || { fail "root lock: wrong message for $line"; cat rl.log; }; fi; done
mkfr; if bash "$T/root-locked.sh" >rl.log 2>&1; then fail "root lock assertion accepted a rootfs without shadow"; else grep -q "does not exist" rl.log && pass "root lock assertion fails without /etc/shadow" || fail "missing shadow: wrong message"; fi
extract_img velora_factory_assert_no_ssh "/k/authorized_keys"; mkfr
if bash "$T/img-velora_factory_assert_no_ssh.sh" >fa.log 2>&1; then fail "no-SSH assertion accepted VELORA_DEV_AUTHORIZED_KEYS_FILE"; else grep -q "never carries authorized keys" fa.log && pass "a dev authorized-keys file in a factory-test build: bbfatal" || fail "authorized keys: wrong message"; fi
mkfr; ln -s ../serial-getty@.service "$T/fr/etc/systemd/system/getty.target.wants/serial-getty@ttyS2.service"
bash "$T/img-velora_factory_no_login.sh" >fl.log 2>&1 && [ ! -e "$T/fr/etc/systemd/system/getty.target.wants/serial-getty@ttyS2.service" ] && [ "$(readlink "$T/fr/etc/systemd/system/serial-getty@ttyS2.service")" = /dev/null ] \
  && pass "factory-test: serial getty unlinked and masked on the debug UART (no login path)" || { fail "serial getty not disabled"; cat fl.log; }

# the image base's variant check (python prefunc of do_rootfs), run with a fake datastore: image and build variant must agree
python3 -I - "$SRC/meta-velora/recipes-core/images/velora-image-base.inc" <<'PYEOF' && pass "image base: variant prefunc passes on a match, fatal on a mismatch or empty variant" || fail "image variant prefunc"
import re, sys, textwrap
s = open(sys.argv[1]).read()
m = re.search(r"^python velora_check_image_variant\(\) \{\n(.*?)^\}\n", s, re.S | re.M)
class Fatal(Exception): pass
class BB:
    @staticmethod
    def fatal(msg): raise Fatal(msg)
class D:
    def __init__(self, **v): self.v = v
    def getVar(self, k): return self.v.get(k)
ns = {"bb": BB}
exec("def f(d):\n" + textwrap.indent(m.group(1), "    "), ns)
ns["f"](D(VELORA_IMAGE_VARIANT="dev", VELORA_VARIANT="dev", PN="velora-image-dev"))
ns["f"](D(VELORA_IMAGE_VARIANT="factory-test", VELORA_VARIANT="factory-test", PN="x"))
for img, have in (("dev", "factory-test"), ("factory-test", "dev"), ("", "dev"), ("factory-test", "prod")):
    try:
        ns["f"](D(VELORA_IMAGE_VARIANT=img, VELORA_VARIANT=have, PN="velora-image-x"))
    except Fatal as e:
        assert "kas/velora-" in str(e), str(e)
    else:
        sys.exit("variant mismatch %r/%r accepted" % (img, have))
PYEOF

echo "== R13. factory test runner with simulated hardware (pass, fail, not present, crash, hang) and report schema"
if python3 -I "$SRC/ci/factory-test/test_runner.py" "$SRC/meta-velora/recipes-core/velora-factory-test/files/velora-factory-test" >ft.log 2>&1; then pass "factory test runner: $(grep -E '^Ran ' ft.log) $(tail -1 ft.log)"; else fail "factory test runner tests"; cat ft.log; fi

echo
echo "RAUC SUMMARY: $PASSED passed, $FAILED failed"
[ "$FAILED" -eq 0 ]
