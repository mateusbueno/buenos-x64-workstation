SUMMARY = "Velora dev firmware bundle (Story 6.12): the dev image rootfs, signed with the dev RAUC root"
DESCRIPTION = "A plain-format RAUC bundle with one slot class (rootfs) written to whichever of rootfs.0/rootfs.1 is inactive. Dev root only: factory-test and production bundles are Story 6.13 / Epic 8."
LICENSE = "MIT"

inherit bundle

# Signing material is never in the repository (tools/keys/KEY-HIERARCHY.md, "RAUC bundle signing | dev"). Point this at the directory written by
# yocto/ci/rauc/gen-dev-root.sh (signing.key.pem, signing.cert.pem, ca.cert.pem); the build fails with a clear message when it is unset.
VELORA_RAUC_SIGN_DIR ?= ""
RAUC_KEY_FILE ?= "${@(d.getVar('VELORA_RAUC_SIGN_DIR') + '/signing.key.pem') if d.getVar('VELORA_RAUC_SIGN_DIR') else ''}"
RAUC_CERT_FILE ?= "${@(d.getVar('VELORA_RAUC_SIGN_DIR') + '/signing.cert.pem') if d.getVar('VELORA_RAUC_SIGN_DIR') else ''}"

# Plain bundles only (the system.conf rejects verity and crypt; plain is verified over the whole payload before any slot write). The compatible string must equal [system] compatible in
# recipes-core/rauc/files/system.conf (checked by yocto/ci/static_checks.py).
RAUC_BUNDLE_FORMAT = "plain"
RAUC_BUNDLE_COMPATIBLE = "Velora RK3588"
RAUC_BUNDLE_VERSION ?= "${PV}"
RAUC_BUNDLE_DESCRIPTION = "Velora dev image (dev-signed)"

# One slot class "rootfs": RAUC writes it to the inactive one of slot.rootfs.0 / slot.rootfs.1. The image provides the plain ext4 file
# (IMAGE_FSTYPES += "ext4" in velora-image-dev).
# TODO(Story 6.12 follow-up, board): the ext4 file is smaller than the 2 GiB slot partition; the filesystem is not grown after install.
RAUC_BUNDLE_SLOTS = "rootfs"
RAUC_SLOT_rootfs = "velora-image-dev"
RAUC_SLOT_rootfs[fstype] = "ext4"
RAUC_IMAGE_FSTYPE = "ext4"

# When the dev keyring is known, `rauc bundle` also verifies the finished bundle against it (a bundle the dev image would reject fails here).
BUNDLE_ARGS = "${@('--keyring=' + d.getVar('VELORA_RAUC_KEYRING_FILE')) if d.getVar('VELORA_RAUC_KEYRING_FILE') else ''}"
VELORA_RAUC_KEYRING_FILE ?= ""

DEPENDS += "openssl-native"
do_bundle[prefuncs] += "velora_check_signing_files"
velora_check_signing_files() {
    keyring="${VELORA_RAUC_KEYRING_FILE}"
    if [ -z "$keyring" ]; then
        bbfatal "VELORA_RAUC_KEYRING_FILE is not set: the dev bundle is checked against the dev keyring the image carries (see yocto/README.md, 'RAUC')"
    fi
    if [ ! -s "$keyring" ]; then
        bbfatal "VELORA_RAUC_KEYRING_FILE='$keyring' is missing or empty"
    fi
    for f in "${RAUC_KEY_FILE}" "${RAUC_CERT_FILE}"; do
        if [ -z "$f" ]; then
            bbfatal "VELORA_RAUC_SIGN_DIR is not set: the dev bundle needs the dev RAUC signing key and certificate (see yocto/README.md, 'RAUC')"
        fi
        if [ ! -s "$f" ]; then
            bbfatal "RAUC signing file '$f' is missing or empty"
        fi
    done
    # the signing certificate must chain to the dev keyring the image carries, and the key must belong to the certificate: otherwise the
    # bundle would be rejected by the very image it is built for
    if ! ${STAGING_BINDIR_NATIVE}/openssl verify -CAfile "$keyring" "${RAUC_CERT_FILE}" >/dev/null 2>&1; then
        bbfatal "signing certificate '${RAUC_CERT_FILE}' does not verify against the keyring '$keyring'"
    fi
    certpub=$(${STAGING_BINDIR_NATIVE}/openssl x509 -in "${RAUC_CERT_FILE}" -noout -pubkey)
    keypub=$(${STAGING_BINDIR_NATIVE}/openssl pkey -in "${RAUC_KEY_FILE}" -pubout)
    if [ -z "$certpub" ] || [ "$certpub" != "$keypub" ]; then
        bbfatal "signing key '${RAUC_KEY_FILE}' does not match the certificate '${RAUC_CERT_FILE}'"
    fi
}
