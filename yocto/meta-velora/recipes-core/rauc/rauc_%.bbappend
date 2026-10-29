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

# Story 8.1: sysd is the sole installer allowed to call RAUC over D-Bus, and rauc CLI access is removed from non-root users.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += "file://dbus/de.pengutronix.rauc.velora.conf"

do_install:append() {
    install -d ${D}${datadir}/dbus-1/system.d
    install -m 0644 ${UNPACKDIR}/dbus/de.pengutronix.rauc.velora.conf ${D}${datadir}/dbus-1/system.d/de.pengutronix.rauc.velora.conf
    if [ -e ${D}${bindir}/rauc ]; then
        chmod 0750 ${D}${bindir}/rauc
    fi
}

FILES:${PN} += "${datadir}/dbus-1/system.d/de.pengutronix.rauc.velora.conf"

