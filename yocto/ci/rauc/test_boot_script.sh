#!/usr/bin/env bash
# Story 6.12: runs the REAL velora-ab-boot.cmd against stub U-Boot commands (no U-Boot, no board). The script is written in the subset of hush
# that bash also runs; the only translation is the RAUC-documented idiom `for X in "${BOOT_ORDER}"` (hush splits the quoted variable, bash does
# not), which is rewritten to the unquoted form for this simulation. Usage: test_boot_script.sh <velora-ab-boot.cmd>
set -uo pipefail
SCRIPT="$1"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
PASSED=0; FAILED=0
pass() { echo "  PASS  $1"; PASSED=$((PASSED+1)); }
fail() { echo "  FAIL  $1"; FAILED=$((FAILED+1)); }
sed -e 's/for BOOT_SLOT in "\${BOOT_ORDER}"; do/for BOOT_SLOT in ${BOOT_ORDER}; do/' -e 's#@VELORA_KERNEL_ARGS@#ro rootwait isolcpus=TEST#' -e 's#@VELORA_FDT@#test.dtb#' "$SCRIPT" > "$T/script.sh"
grep -q 'for BOOT_SLOT in ${BOOT_ORDER}; do' "$T/script.sh" || { echo "FAIL: expected for-loop idiom not found in the script"; exit 1; }
ENVFILE="$T/env"   # the "U-Boot environment": persists across simulated boots, like the uboot_env partition

# one simulated boot: prints a line per interesting event; the environment persists only through saveenv
boot() {
  (
    BOOT_ORDER=""; BOOT_A_LEFT=""; BOOT_B_LEFT=""
    [ -f "$ENVFILE" ] && . "$ENVFILE"
    setenv() { if [ $# -eq 1 ]; then unset "$1"; else printf -v "$1" '%s' "$2"; fi; }
    setexpr() { printf -v "$1" '%s' "$(( $2 $3 $4 ))"; }
    saveenv() { { declare -p BOOT_ORDER BOOT_A_LEFT BOOT_B_LEFT | sed 's/^declare -- //'; } > "$ENVFILE"; echo "SAVEENV"; }
    reset() { echo "RESET"; exit 0; }
    part() { printf -v "$5" '%s' "$(( 9 + ${4#rootfs.} ))"; }   # `part number nvme 0 rootfs.N var`: wks puts rootfs.0 at partition 9
    load() { echo "LOAD $*"; }
    # real booti does not return on success: nothing placed after it in the script runs
    booti() { echo "BOOTARGS $bootargs"; echo "BOOTI $*"; exit 0; }
    kernel_addr_r=0x2000000; fdt_addr_r=0x1000000
    . "${SCRIPT_UNDER_TEST:-$T/script.sh}"
  ) 2>&1
}
save_env() { { echo "BOOT_ORDER='$1'"; echo "BOOT_A_LEFT='$2'"; echo "BOOT_B_LEFT='$3'"; } > "$ENVFILE"; }
get() { sed -n "s/^$1='\\(.*\\)'$/\\1/p;s/^$1=\"\\(.*\\)\"$/\\1/p" "$ENVFILE"; }
expect() { # expect <description> <output> <egrep pattern>
  if echo "$2" | grep -Eq -- "$3"; then pass "$1"; else fail "$1 (pattern: $3)"; echo "$2" | sed 's/^/    | /'; fi
}

rm -f "$ENVFILE"
out=$(boot)
expect "empty environment: boots slot A with 2 attempts left" "$out" 'Found valid slot A, 2 attempts remaining'
expect "slot A: root=PARTLABEL=rootfs.0 and rauc.slot=A on the command line" "$out" 'BOOTARGS root=PARTLABEL=rootfs\.0 rauc\.slot=A ro rootwait isolcpus=TEST'
expect "slot A: kernel loaded from partition 9 (looked up by name rootfs.0)" "$out" 'LOAD nvme 0:9 0x2000000 /boot/Image'
expect "slot A: boots (booti)" "$out" 'BOOTI 0x2000000 - 0x1000000'
[ "$(get BOOT_A_LEFT)" = "2" ] && [ "$(get BOOT_B_LEFT)" = "3" ] && pass "counters saved: A_LEFT=2, B_LEFT=3 (3 attempts is the default)" || fail "saved counters: A=$(get BOOT_A_LEFT) B=$(get BOOT_B_LEFT)"

# after `rauc install` to B: BOOT_ORDER="B A", B_LEFT=3
save_env "B A" 3 3
out=$(boot)
expect "after an install: boots slot B first" "$out" 'Found valid slot B, 2 attempts remaining'
expect "slot B: root=PARTLABEL=rootfs.1 (not the rootfs.0 of the extlinux image) and rauc.slot=B" "$out" 'BOOTARGS root=PARTLABEL=rootfs\.1 rauc\.slot=B '
expect "slot B: kernel loaded from partition 10" "$out" 'LOAD nvme 0:10 '

# a bad slot that is never marked good: 3 attempts, then the previous slot
save_env "B A" 3 3
seq=""
for i in 1 2 3 4; do out=$(boot); seq="$seq $(echo "$out" | sed -n 's/^Found valid slot \(.\),.*/\1/p')"; done
[ "$seq" = " B B B A" ] && pass "never marked good: boots B three times, then falls back to A (sequence:$seq)" || fail "attempt sequence:$seq"
[ "$(get BOOT_B_LEFT)" = "0" ] && pass "failed slot B ends with 0 attempts left (RAUC reports it bad)" || fail "B_LEFT=$(get BOOT_B_LEFT)"
[ "$(get BOOT_A_LEFT)" = "2" ] && pass "fallback slot A keeps its attempts (2 left after one boot)" || fail "A_LEFT=$(get BOOT_A_LEFT)"

# marked good (rauc status mark-good resets the counter): stays on B
save_env "B A" 3 3; boot >/dev/null; save_env "B A" 3 2   # first boot used one attempt; mark-good restores B_LEFT=3 as RAUC does
save_env "B A" 3 3; out=$(boot)
expect "slot marked good (counter back to 3): next boot is B again" "$out" 'Found valid slot B, 2 attempts remaining'

# both slots exhausted, order "B A" (B is the failed new slot, A the previous one): ONLY the fallback (last of BOOT_ORDER) is given new attempts
save_env "B A" 0 0
out=$(boot)
expect "both slots out of attempts: the fallback slot A gets 3 more attempts" "$out" 'No valid slot found, giving the fallback slot A 3 more attempts'
if echo "$out" | grep -q BOOTI; then fail "booted although no valid slot"; else pass "both slots out of attempts: nothing is booted before the reset"; fi
echo "$out" | grep -q '^RESET' && pass "both slots out of attempts: reset issued" || fail "no reset"
[ "$(get BOOT_A_LEFT)" = "3" ] && [ "$(get BOOT_B_LEFT)" = "0" ] && pass "fallback A restored to 3, the failed slot B stays at 0 (never resurrected)" || fail "counters after reset: A=$(get BOOT_A_LEFT) B=$(get BOOT_B_LEFT)"
out=$(boot)
expect "after that reset the fallback A boots, not B" "$out" 'Found valid slot A, 2 attempts remaining'
# the fallback fails too: it runs out again and is given attempts again, B is still never booted
save_env "B A" 0 0; seq=""
for i in 1 2 3 4 5 6 7 8; do out=$(boot); seq="$seq $(echo "$out" | sed -n 's/^Found valid slot \(.\),.*/\1/p;s/^No valid slot found.*slot \(.\) 3 more.*/r\1/p' | tr '\n' ' ')"; done
echo "$seq" | grep -q "B" && fail "failed slot B was booted while out of attempts: $seq" || pass "failed slot B stays dead while the fallback is rebooted (sequence:$seq)"
# order "A B": last = B is the fallback
save_env "A B" 0 0
out=$(boot)
expect "order A B, both exhausted: the last slot B is the fallback" "$out" 'giving the fallback slot B 3 more attempts'
[ "$(get BOOT_A_LEFT)" = "0" ] && [ "$(get BOOT_B_LEFT)" = "3" ] && pass "A stays at 0, B restored to 3" || fail "counters: A=$(get BOOT_A_LEFT) B=$(get BOOT_B_LEFT)"
# unusable BOOT_ORDER
save_env "X Y" 0 0
out=$(boot)
expect "unusable BOOT_ORDER: default order restored, slot A re-armed" "$out" 'BOOT_ORDER is unusable'
[ "$(get BOOT_ORDER)" = "A B" ] && [ "$(get BOOT_A_LEFT)" = "3" ] && pass "default order A B and A_LEFT=3 saved" || fail "env after unusable order: $(cat "$ENVFILE")"

# ordering: saveenv must run BEFORE booti (booti never returns). The real script: SAVEENV precedes BOOTI in the output.
save_env "A B" 3 3
out=$(boot)
if [ "$(echo "$out" | grep -n 'SAVEENV' | head -1 | cut -d: -f1)" -lt "$(echo "$out" | grep -n 'BOOTI' | head -1 | cut -d: -f1)" ]; then pass "saveenv runs before booti"; else fail "saveenv after booti"; fi
# a script that saves only after booti must be caught: with the exiting booti stub the counter is never persisted
python3 - "$T/script.sh" "$T/late-save.sh" <<'PYEOF'
import sys
s = open(sys.argv[1]).read()
first = s.index("  saveenv\nelse")
s = s[:first] + s[first + len("  saveenv\n"):] + "\nsaveenv\n"
open(sys.argv[2], "w").write(s)
PYEOF
save_env "A B" 3 3
out=$(SCRIPT_UNDER_TEST="$T/late-save.sh" boot)
if echo "$out" | grep -q SAVEENV || [ "$(get BOOT_A_LEFT)" != "3" ]; then fail "saveenv after booti was not caught by the harness"; else pass "a saveenv placed after booti never runs (counter stays 3): the harness catches that mutation"; fi

# a slot with 0 attempts is skipped even when first in BOOT_ORDER
save_env "A B" 0 3
out=$(boot)
expect "first slot has 0 attempts: slot B boots" "$out" 'Found valid slot B, 2 attempts remaining'

echo "boot script logic: $PASSED passed, $FAILED failed"
[ "$FAILED" -eq 0 ]
