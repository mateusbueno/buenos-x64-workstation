SUMMARY = "Policy test recipe (LGPL-3.0-only), empty package"
LICENSE = "LGPL-3.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/LGPL-3.0-only;md5=bfccfe952269fff2b407dd11f2f3083b"
inherit allarch
S = "${UNPACKDIR}"
ALLOW_EMPTY:${PN} = "1"
do_install() {
    :
}
