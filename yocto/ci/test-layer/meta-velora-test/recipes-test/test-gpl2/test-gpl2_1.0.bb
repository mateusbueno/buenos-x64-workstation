SUMMARY = "Policy test recipe (GPL-2.0-only), empty package"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"
inherit allarch
S = "${UNPACKDIR}"
ALLOW_EMPTY:${PN} = "1"
do_install() {
    :
}
