SUMMARY = "Velora factory test runner and its systemd unit (Story 6.13)"
DESCRIPTION = "Python runner with one test per subsystem (MCU links, keybed, panel and LEDs, codec loopback, display and touch, NVMe speed), JSON report and text summary. Factory-test image only."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COREBASE}/meta/COPYING.MIT;md5=3da9cfbcb788c80a0384361b4de20420"

SRC_URI = " \
    file://velora-factory-test \
    file://velora-factory-test.service \
"

S = "${UNPACKDIR}"

inherit systemd allarch

# The runner uses only the Python standard library modules below (argparse, datetime, glob, json, tempfile, threading).
RDEPENDS:${PN} = "python3-core python3-json python3-threading"

do_install() {
    install -d ${D}${libexecdir}/velora
    install -m 0755 ${UNPACKDIR}/velora-factory-test ${D}${libexecdir}/velora/velora-factory-test
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/velora-factory-test.service ${D}${systemd_system_unitdir}/velora-factory-test.service
    # report directory on the persistent data partition (tmpfiles.d creates it at boot: /data is mounted after the read-only rootfs is built)
    install -d ${D}${sysconfdir}/tmpfiles.d
    echo 'd /data/factory-test 0755 root root -' > ${D}${sysconfdir}/tmpfiles.d/20-velora-factory-test.conf
}

SYSTEMD_SERVICE:${PN} = "velora-factory-test.service"
SYSTEMD_AUTO_ENABLE = "enable"
FILES:${PN} += "${libexecdir}/velora ${systemd_system_unitdir}/velora-factory-test.service ${sysconfdir}/tmpfiles.d/20-velora-factory-test.conf"
