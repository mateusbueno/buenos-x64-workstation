SUMMARY = "Velora service users (Story 6.11): one system user per persistent-data writer of AD-9"
DESCRIPTION = "velora-engine (engine control thread), velora-librarian (librarian), velora-sysd (sysd). No login, no home."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COREBASE}/meta/COPYING.MIT;md5=3da9cfbcb788c80a0384361b4de20420"

S = "${UNPACKDIR}"

inherit allarch useradd

# Static UIDs and GIDs (900 to 902) keep ownership stable across builds and across RAUC slot updates. The root filesystem is read-only, so these
# entries are baked into /etc/passwd at image build time.
USERADD_PACKAGES = "${PN}"
GROUPADD_PARAM:${PN} = " \
    --system --gid 900 velora-engine; \
    --system --gid 901 velora-librarian; \
    --system --gid 902 velora-sysd; \
"
USERADD_PARAM:${PN} = " \
    --system --no-create-home --home-dir /nonexistent --shell /bin/false --uid 900 --gid 900 velora-engine; \
    --system --no-create-home --home-dir /nonexistent --shell /bin/false --uid 901 --gid 901 velora-librarian; \
    --system --no-create-home --home-dir /nonexistent --shell /bin/false --uid 902 --gid 902 velora-sysd; \
"

do_install() {
    :
}
ALLOW_EMPTY:${PN} = "1"
