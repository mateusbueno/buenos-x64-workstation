# TODO(Story 1.1): PLACEHOLDER boot arguments for the isolated RT cores. Story 1.1 (PREEMPT_RT latency spike)
# produces the validated isolcpus/nohz_full/rcu_nocbs set; replace these values with its result.
# Cores 5-7 (three of the four Cortex-A76) are assumed from the epic text and are NOT validated.
# TODO(Story 1.2): audio-related kernel arguments, if any, also come from Story 1.2.
VELORA_BOOTARGS_RT ?= "isolcpus=managed_irq,domain,5-7 nohz_full=5-7 rcu_nocbs=5-7 threadirqs"
# no swap: swapaccount irrelevant; systemd must not look for a resume/swap device
VELORA_BOOTARGS_BASE ?= "noresume"
UBOOT_EXTLINUX_KERNEL_ARGS:append = " ${VELORA_BOOTARGS_BASE} ${VELORA_BOOTARGS_RT}"
