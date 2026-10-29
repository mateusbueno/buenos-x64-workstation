SUMMARY = "Velora system policy: data directories (AD-9), no swap, no core dumps, bounded volatile journal, DHCP on ethernet (dev image)"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COREBASE}/meta/COPYING.MIT;md5=3da9cfbcb788c80a0384361b4de20420"

FILESEXTRAPATHS:prepend := "${THISDIR}/../../../..:"

SRC_URI = " \
    file://50-velora-sysctl.conf \
    file://10-velora-coredump.conf \
    file://10-velora-limits.conf \
    file://10-velora-journal.conf \
    file://20-velora-wired.network \
    file://10-velora-data.conf \
    file://THIRD_PARTY_NOTICES.md \
"
S = "${UNPACKDIR}"

inherit allarch

do_install() {
    install -d ${D}${sysconfdir}/sysctl.d
    install -m 0644 ${UNPACKDIR}/50-velora-sysctl.conf ${D}${sysconfdir}/sysctl.d/50-velora.conf
    install -d ${D}${sysconfdir}/systemd/coredump.conf.d
    install -m 0644 ${UNPACKDIR}/10-velora-coredump.conf ${D}${sysconfdir}/systemd/coredump.conf.d/10-velora.conf
    install -d ${D}${sysconfdir}/systemd/system.conf.d
    install -m 0644 ${UNPACKDIR}/10-velora-limits.conf ${D}${sysconfdir}/systemd/system.conf.d/10-velora.conf
    install -d ${D}${sysconfdir}/systemd/journald.conf.d
    install -m 0644 ${UNPACKDIR}/10-velora-journal.conf ${D}${sysconfdir}/systemd/journald.conf.d/10-velora.conf
    install -d ${D}${sysconfdir}/systemd/network
    install -m 0644 ${UNPACKDIR}/20-velora-wired.network ${D}${sysconfdir}/systemd/network/20-velora-wired.network
    install -d ${D}${sysconfdir}/tmpfiles.d
    install -m 0644 ${UNPACKDIR}/10-velora-data.conf ${D}${sysconfdir}/tmpfiles.d/10-velora-data.conf
    # Mask swap.target. Individual .swap units/generators (fstab, GPT) are covered by the kernel having no swap (CONFIG_SWAP off) and by no swap entry in fstab/wks.
    install -d ${D}${sysconfdir}/systemd/system
    ln -sf /dev/null ${D}${sysconfdir}/systemd/system/swap.target
    install -d ${D}${datadir}/licenses/velora
    install -m 0644 ${UNPACKDIR}/THIRD_PARTY_NOTICES.md ${D}${datadir}/licenses/velora/THIRD_PARTY_NOTICES.md
}

FILES:${PN} = "${sysconfdir}"
FILES:${PN} += "${datadir}/licenses/velora"
# the tmpfiles.d entries name these users
RDEPENDS:${PN} += "velora-service-users"
