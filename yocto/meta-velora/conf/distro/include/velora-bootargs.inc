# TODO(Story 1.1): PLACEHOLDER boot arguments for the isolated RT cores. Story 1.1 (PREEMPT_RT latency spike)
# produces the validated isolcpus/nohz_full/rcu_nocbs set; replace these values with its result.
# Cores 5-7 (three of the four Cortex-A76) are assumed from the epic text and are NOT validated.
# TODO(Story 1.2): audio-related kernel arguments, if any, also come from Story 1.2.
VELORA_BOOTARGS_RT ?= "isolcpus=managed_irq,domain,5-7 nohz_full=5-7 rcu_nocbs=5-7 threadirqs"
# no swap: swapaccount irrelevant; systemd must not look for a resume/swap device
# Story 6.11: the root slot is mounted read-only ("ro"; meta-rockchip's default "rw" is removed below).
VELORA_BOOTARGS_BASE ?= "noresume ro"
UBOOT_EXTLINUX_KERNEL_ARGS:append = " ${VELORA_BOOTARGS_BASE} ${VELORA_BOOTARGS_RT}"
# Story 6.11: boot from root slot A by GPT partition name (wic --part-name rootfs.0 in files/wic/velora-nvme.wks); RAUC switches slots in Story 6.12.
# Story 6.12: this extlinux root is only the FALLBACK for slot A. Slot B holds the same image content (same extlinux root=rootfs.0), so the
# slot choice is made by files/uboot/velora-ab-boot.cmd, which builds root=PARTLABEL=rootfs.<n> itself and boots without extlinux.
UBOOT_EXTLINUX_ROOT = "root=PARTLABEL=rootfs.0"
UBOOT_EXTLINUX_KERNEL_ARGS:remove = "rw"
