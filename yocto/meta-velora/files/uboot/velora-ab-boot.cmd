# Velora A/B slot selection for RAUC's U-Boot backend (Story 6.12). Compiled to boot.scr by recipes-core/rauc/velora-uboot-ab-script.
#
# The RAUC U-Boot contract (https://rauc.readthedocs.io, "Bootloader Integration"): the environment variables
#   BOOT_ORDER   space separated bootnames, first = preferred ("A B"); RAUC writes it on install and mark-good/bad
#   BOOT_A_LEFT, BOOT_B_LEFT   remaining boot attempts of the slot; 0 = slot is bad
# This script decrements the counter of the slot it boots; only `rauc status mark-good` (velora-rauc-mark-good.service, after engine, ui and
# sysd are ready) resets it to 3. A slot that never gets marked good therefore runs out of attempts after 3 boots, and the next slot of
# BOOT_ORDER boots: the rollback. `rauc install` also sets the target slot's counter to 3 and puts it first in BOOT_ORDER.
#
# Slot B holds the same image content as slot A, whose extlinux.conf says root=PARTLABEL=rootfs.0. That is why this script does not use
# extlinux: it builds root=PARTLABEL=rootfs.<n> itself from the slot it picked.
#
# Written in the subset of U-Boot hush that bash also runs, so test_boot_script.sh can run the real file against stub U-Boot commands.
# TODO(Story 1.6): PLACEHOLDER until the boot chain is chosen: the interface/device (nvme 0), the kernel and device tree file names, the load
# addresses (kernel_addr_r, fdt_addr_r come from the board environment), the decimal partition numbers `part number` returns (used as
# `nvme 0:<n>`, which U-Boot also reads as decimal; hex vs decimal consistency must be confirmed on the board) and how this script is found (distro boot scans boot.scr) are
# assumptions. The attempt counter (3) and the variable names are fixed by RAUC and the spec.
# TODO(Story 1.1): the isolcpus/nohz_full/rcu_nocbs arguments come in through @VELORA_KERNEL_ARGS@ (UBOOT_EXTLINUX_KERNEL_ARGS of the image).

test -n "${BOOT_ORDER}" || setenv BOOT_ORDER "A B"
test -n "${BOOT_A_LEFT}" || setenv BOOT_A_LEFT 3
test -n "${BOOT_B_LEFT}" || setenv BOOT_B_LEFT 3

setenv velora_slot ""
setenv velora_slotnum ""
setenv velora_last ""
for BOOT_SLOT in "${BOOT_ORDER}"; do
  setenv velora_last "${BOOT_SLOT}"
  if test "x${velora_slot}" != "x"; then
    echo "slot ${BOOT_SLOT} skipped, ${velora_slot} already chosen"
  elif test "x${BOOT_SLOT}" = "xA"; then
    if test ${BOOT_A_LEFT} -gt 0; then
      setexpr BOOT_A_LEFT ${BOOT_A_LEFT} - 1
      echo "Found valid slot A, ${BOOT_A_LEFT} attempts remaining"
      setenv velora_slot A
      setenv velora_slotnum 0
    fi
  elif test "x${BOOT_SLOT}" = "xB"; then
    if test ${BOOT_B_LEFT} -gt 0; then
      setexpr BOOT_B_LEFT ${BOOT_B_LEFT} - 1
      echo "Found valid slot B, ${BOOT_B_LEFT} attempts remaining"
      setenv velora_slot B
      setenv velora_slotnum 1
    fi
  fi
done

if test -n "${velora_slot}"; then
  saveenv
else
  # Every slot of BOOT_ORDER is out of attempts (each one failed to be marked good). Only the LAST slot of BOOT_ORDER, the fallback (the
  # previously good slot after an install put the new one first), gets 3 fresh attempts; a slot at 0 that RAUC marked bad stays bad.
  # If the fallback fails too, it runs out again and gets the same treatment: the known-bad slot is never resurrected.
  # saveenv comes BEFORE reset and, like booti below, nothing after a successful reset/booti runs.
  if test "x${velora_last}" = "xA"; then
    echo "No valid slot found, giving the fallback slot A 3 more attempts"
    setenv BOOT_A_LEFT 3
  elif test "x${velora_last}" = "xB"; then
    echo "No valid slot found, giving the fallback slot B 3 more attempts"
    setenv BOOT_B_LEFT 3
  else
    echo "No valid slot found and BOOT_ORDER is unusable, restoring the default order and slot A"
    setenv BOOT_ORDER "A B"
    setenv BOOT_A_LEFT 3
  fi
  saveenv
  reset
fi

# The partition number is looked up by GPT name, so the layout (Story 6.11 wks) stays the only place that knows it.
part number nvme 0 rootfs.${velora_slotnum} velora_rootpart
setenv bootargs "root=PARTLABEL=rootfs.${velora_slotnum} rauc.slot=${velora_slot} @VELORA_KERNEL_ARGS@"
load nvme 0:${velora_rootpart} ${kernel_addr_r} /boot/Image
load nvme 0:${velora_rootpart} ${fdt_addr_r} /boot/@VELORA_FDT@
booti ${kernel_addr_r} - ${fdt_addr_r}
