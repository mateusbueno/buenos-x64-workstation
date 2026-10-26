# Story 6.12: target RAUC configuration for the Velora image.
# No network or streaming install (sysd installs from a USB-A stick or Velora Studio over its own link, Story 8.x); the D-Bus service is on
# (sysd is the only caller, Story 8.x); json is needed by velora-rollback-record (rauc status --output-format=json); no bundle creation on target.
PACKAGECONFIG = "service json nocreate gpt"

# Bootloader environment access for the uboot backend. meta-rauc derives this from PREFERRED_PROVIDER_virtual/bootloader, which is unset here.
# Wrynose has no u-boot-fw-utils: fw_printenv/fw_setenv come from libubootenv (libubootenv-bin).
# TODO(Story 1.6): follows the bootloader chain decision (barebox: dt-utils-barebox-state).
RAUC_BOOTLOADER_RDEPENDS = "libubootenv-bin"

# meta-rauc's stock rauc-mark-good.service is NOT used: it is wanted by multi-user.target and only waits for boot-complete.target. Ours is
# velora-rauc-mark-good (recipes-core/rauc/velora-rauc-mark-good_1.0.bb), gated on the engine, ui and sysd readiness targets.
RRECOMMENDS:${PN}:remove = "${PN}-mark-good"
