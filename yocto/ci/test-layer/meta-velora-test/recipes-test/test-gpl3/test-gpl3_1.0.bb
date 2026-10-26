SUMMARY = "Policy test recipe (GPL-3.0-only), empty package"
LICENSE = "GPL-3.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-3.0-only;md5=c79ff39f19dfec6d293b95dea7b07891"
inherit allarch
S = "${UNPACKDIR}"
ALLOW_EMPTY:${PN} = "1"
do_install() {
    :
}
