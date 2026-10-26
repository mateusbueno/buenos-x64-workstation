SUMMARY = "Policy test recipe (AGPL-3.0-only), empty package"
LICENSE = "AGPL-3.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/AGPL-3.0-only;md5=73f1eb20517c55bf9493b7dd6e480788"
inherit allarch
S = "${UNPACKDIR}"
ALLOW_EMPTY:${PN} = "1"
do_install() {
    :
}
