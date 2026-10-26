SUMMARY = "Velora RAUC mark-good unit and readiness targets (Story 6.12)"
DESCRIPTION = "velora-rauc-mark-good.service marks the running slot good only after velora-{engine,ui,sysd}-ready.target are active."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COREBASE}/meta/COPYING.MIT;md5=3da9cfbcb788c80a0384361b4de20420"

SRC_URI = " \
    file://systemd/velora-engine-ready.target \
    file://systemd/velora-ui-ready.target \
    file://systemd/velora-sysd-ready.target \
    file://systemd/velora-rauc-mark-good.service \
"
S = "${UNPACKDIR}"

inherit allarch systemd

SYSTEMD_SERVICE:${PN} = "velora-rauc-mark-good.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install() {
    install -d ${D}${systemd_system_unitdir}
    for u in velora-engine-ready.target velora-ui-ready.target velora-sysd-ready.target velora-rauc-mark-good.service; do
        install -m 0644 ${UNPACKDIR}/systemd/$u ${D}${systemd_system_unitdir}/$u
    done
    # defence in depth: if meta-rauc's stock rauc-mark-good package ever gets installed (it is wanted by multi-user.target and only waits for
    # boot-complete.target), its unit stays masked
    install -d ${D}${sysconfdir}/systemd/system
    ln -sf /dev/null ${D}${sysconfdir}/systemd/system/rauc-mark-good.service
}
FILES:${PN} = "${systemd_system_unitdir} ${sysconfdir}/systemd/system/rauc-mark-good.service"
RDEPENDS:${PN} += "rauc"
