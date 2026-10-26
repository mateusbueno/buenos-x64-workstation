# Shared base of the per-variant RAUC bundle recipes (Story 6.12, generalised in 6.13). A variant bundle recipe sets SUMMARY, DESCRIPTION,
# VELORA_BUNDLE_VARIANT, RAUC_BUNDLE_DESCRIPTION and RAUC_SLOT_rootfs (its own image), then requires this file:
#   velora-bundle-dev            signed with the dev root, built from velora-image-dev
#   velora-bundle-factory-test   signed with the factory-test root, built from velora-image-factory-test
# Production bundles are NOT defined here (Story 6.14 / Epic 8): the production key exists only in an HSM.
# A plain-format RAUC bundle with one slot class (rootfs) written to whichever of rootfs.0/rootfs.1 is inactive.
LICENSE = "MIT"

inherit bundle

# Signing material is never in the repository (tools/keys/KEY-HIERARCHY.md, "RAUC bundle signing"). Point this at the directory written by
# yocto/ci/rauc/gen-dev-root.sh --variant <variant> (signing.key.pem, signing.cert.pem, ca.cert.pem) of THIS bundle's variant; the build fails
# with a clear message when it is unset, and when the certificate, key or keyring belong to another variant.
VELORA_BUNDLE_VARIANT ?= ""
VELORA_RAUC_SIGN_DIR ?= ""
RAUC_KEY_FILE ?= "${@(d.getVar('VELORA_RAUC_SIGN_DIR') + '/signing.key.pem') if d.getVar('VELORA_RAUC_SIGN_DIR') else ''}"
RAUC_CERT_FILE ?= "${@(d.getVar('VELORA_RAUC_SIGN_DIR') + '/signing.cert.pem') if d.getVar('VELORA_RAUC_SIGN_DIR') else ''}"

# Plain bundles only (the system.conf rejects verity and crypt; plain is verified over the whole payload before any slot write). The compatible string must equal [system] compatible in
# recipes-core/rauc/files/system.conf (checked by yocto/ci/static_checks.py).
RAUC_BUNDLE_FORMAT = "plain"
RAUC_BUNDLE_COMPATIBLE = "Velora RK3588"
RAUC_BUNDLE_VERSION ?= "${PV}"

# One slot class "rootfs": RAUC writes it to the inactive one of slot.rootfs.0 / slot.rootfs.1. The image (RAUC_SLOT_rootfs, set by the variant
# recipe) provides the plain ext4 file (IMAGE_FSTYPES += "ext4" in velora-image-base.inc).
# TODO(Story 6.12 follow-up, board): the ext4 file is smaller than the 2 GiB slot partition; the filesystem is not grown after install.
RAUC_BUNDLE_SLOTS = "rootfs"
RAUC_SLOT_rootfs[fstype] = "ext4"
RAUC_IMAGE_FSTYPE = "ext4"

# When the variant keyring is known, `rauc bundle` also verifies the finished bundle against it (a bundle the variant image would reject fails here).
BUNDLE_ARGS = "${@('--keyring=' + d.getVar('VELORA_RAUC_KEYRING_FILE')) if d.getVar('VELORA_RAUC_KEYRING_FILE') else ''}"
VELORA_RAUC_KEYRING_FILE ?= ""

DEPENDS += "openssl-native"
do_bundle[prefuncs] += "velora_check_signing_files"
velora_check_signing_files() {
    variant="${VELORA_BUNDLE_VARIANT}"
    case "${VELORA_VARIANT}" in
        dev|factory-test) ;;
        prod) bbfatal "VELORA_VARIANT=prod: the production keyring rule is supplied by Story 6.14 and is not defined in this layer yet" ;;
        *) bbfatal "VELORA_VARIANT='${VELORA_VARIANT}' is not one of dev, factory-test, prod" ;;
    esac
    if [ -z "$variant" ] || [ "$variant" != "${VELORA_VARIANT}" ]; then
        bbfatal "bundle ${PN} is for variant '$variant' but this build is VELORA_VARIANT='${VELORA_VARIANT}': build it with yocto/kas/velora-$variant.yml"
    fi
    keyring="${VELORA_RAUC_KEYRING_FILE}"
    if [ -z "$keyring" ]; then
        bbfatal "VELORA_RAUC_KEYRING_FILE is not set: the $variant bundle is checked against the $variant keyring the image carries (see yocto/README.md, 'RAUC')"
    fi
    if [ ! -s "$keyring" ]; then
        bbfatal "VELORA_RAUC_KEYRING_FILE='$keyring' is missing or empty"
    fi
    for f in "${RAUC_KEY_FILE}" "${RAUC_CERT_FILE}"; do
        if [ -z "$f" ]; then
            bbfatal "VELORA_RAUC_SIGN_DIR is not set: the $variant bundle needs the $variant RAUC signing key and certificate (see yocto/README.md, 'RAUC')"
        fi
        if [ ! -s "$f" ]; then
            bbfatal "RAUC signing file '$f' is missing or empty"
        fi
    done
    # the keyring must be this variant's root (same rules as rauc-conf: one plain CERTIFICATE block, exact RFC 2253 subject, self-signed): a bundle
    # must never be signed for, or checked against, another variant's root
    nblocks=$(grep -c '^-----BEGIN ' "$keyring" || true)
    firstblock=$(sed -n 's/^-----BEGIN \(.*\)-----$/\1/p' "$keyring" | head -1)
    if [ "$nblocks" != "1" ] || [ "$firstblock" != "CERTIFICATE" ]; then
        bbfatal "keyring '$keyring' must hold exactly one PEM certificate and no other PEM block, found $nblocks block(s), first block type '$firstblock'"
    fi
    subject=$(${STAGING_BINDIR_NATIVE}/openssl x509 -in "$keyring" -noout -subject -nameopt RFC2253 2>/dev/null | sed 's/^subject= *//')
    issuer=$(${STAGING_BINDIR_NATIVE}/openssl x509 -in "$keyring" -noout -issuer -nameopt RFC2253 2>/dev/null | sed 's/^issuer= *//')
    want="CN=Velora $variant RAUC root (not for production),O=Velora"
    if [ "$subject" != "$want" ]; then
        bbfatal "keyring '$keyring' is not the $variant RAUC root: its subject is '$subject', expected exactly '$want'"
    fi
    if [ "$issuer" != "$subject" ] || ! ${STAGING_BINDIR_NATIVE}/openssl verify -CAfile "$keyring" "$keyring" >/dev/null 2>&1; then
        bbfatal "keyring '$keyring' is not a self-signed root certificate (issuer '$issuer')"
    fi
    # the signing certificate must chain to the keyring the image carries, and the key must belong to the certificate: otherwise the
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
