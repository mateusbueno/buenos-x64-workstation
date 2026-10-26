SUMMARY = "Velora rollback recorder (Story 6.12): writes /data/config/rollback.json when RAUC booted the previous slot"
DESCRIPTION = "Stand-in for sysd (which does not exist yet): reads `rauc status --detailed --output-format=json` and records from/to versions atomically."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COREBASE}/meta/COPYING.MIT;md5=3da9cfbcb788c80a0384361b4de20420"

SRC_URI = " \
    file://rollback/velora-rollback-record \
    file://systemd/velora-rollback-record.service \
"
S = "${UNPACKDIR}"

inherit allarch systemd

SYSTEMD_SERVICE:${PN} = "velora-rollback-record.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install() {
    install -d ${D}${libexecdir}/velora
    install -m 0755 ${UNPACKDIR}/rollback/velora-rollback-record ${D}${libexecdir}/velora/velora-rollback-record
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/systemd/velora-rollback-record.service ${D}${systemd_system_unitdir}/velora-rollback-record.service
}
FILES:${PN} = "${libexecdir}/velora ${systemd_system_unitdir}"
# python3-core: interpreter + os/json/tempfile/argparse/datetime are in python3-core, python3-json, python3-datetime
RDEPENDS:${PN} += "python3-core python3-json python3-datetime rauc"
