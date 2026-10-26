# Story 6.12/6.13: Velora RAUC system configuration, the keyring of the image variant's own root, and U-Boot environment access.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# Path of the PUBLIC certificate of the RAUC root of the variant being built (the keyring): dev root for VELORA_VARIANT = "dev", factory-test
# root for "factory-test". Supplied at build time, never committed (tools/keys/KEY-HIERARCHY.md, "RAUC bundle signing"). Generate it with
# yocto/ci/rauc/gen-dev-root.sh --variant <variant> and set, e.g. in a kas overlay or local.conf:
#   VELORA_RAUC_KEYRING_FILE = "/home/me/.velora-dev-keys/rauc/ca.cert.pem"
# One build is one variant (VELORA_VARIANT, kas/velora-<variant>.yml), so one keyring variable is enough; do_install checks the certificate
# names that variant and no other.
# Empty by default so that the layer parses without keys; building the image (do_install of this recipe) then fails with a clear message.
VELORA_RAUC_KEYRING_FILE ?= ""

# meta-rauc's keyring URI is the given path. Its only job here is change tracking (the file's checksum is part of do_install's signature) and
# a parse-time error for a path that does not exist; do_install (replaced below) reads the absolute path itself and installs it as
# ca.cert.pem, the name system.conf refers to.
RAUC_KEYRING_URI = "${@('file://' + d.getVar('VELORA_RAUC_KEYRING_FILE')) if d.getVar('VELORA_RAUC_KEYRING_FILE') else ''}"
RAUC_KEYRING_FILE = "ca.cert.pem"
SRC_URI += "file://fw_env.config"

DEPENDS += "openssl-native"

# meta-rauc's do_install is replaced: it only warns about a missing keyring and cannot take an absolute path.
do_install () {
    case "${VELORA_VARIANT}" in
        dev|factory-test) ;;
        prod) bbfatal "VELORA_VARIANT=prod: the production keyring rule is supplied by Story 6.14 and is not defined in this layer yet" ;;
        *) bbfatal "VELORA_VARIANT='${VELORA_VARIANT}' is not one of dev, factory-test, prod" ;;
    esac
    keyring="${VELORA_RAUC_KEYRING_FILE}"
    if [ -z "$keyring" ]; then
        bbfatal "VELORA_RAUC_KEYRING_FILE is not set: the ${VELORA_VARIANT} image needs the public certificate of the ${VELORA_VARIANT} RAUC root (see yocto/README.md, 'RAUC')"
    fi
    if [ ! -s "$keyring" ]; then
        bbfatal "VELORA_RAUC_KEYRING_FILE='$keyring' is missing or empty"
    fi
    if grep -q "PRIVATE KEY" "$keyring"; then
        bbfatal "VELORA_RAUC_KEYRING_FILE='$keyring' contains a private key: the image may carry only the public certificate"
    fi
    # exactly one PEM block, and it is a plain CERTIFICATE (no second block of any type: X509 CERTIFICATE, TRUSTED CERTIFICATE, keys, ...)
    nblocks=$(grep -c '^-----BEGIN ' "$keyring" || true)
    firstblock=$(sed -n 's/^-----BEGIN \(.*\)-----$/\1/p' "$keyring" | head -1)
    if [ "$nblocks" != "1" ] || [ "$firstblock" != "CERTIFICATE" ]; then
        bbfatal "VELORA_RAUC_KEYRING_FILE='$keyring' must hold exactly one PEM certificate (the ${VELORA_VARIANT} root) and no other PEM block, found $nblocks block(s), first block type '$firstblock'"
    fi
    # an image trusts only its own variant's root. gen-dev-root.sh writes the subject "O=Velora, CN=Velora <variant> RAUC root (not for production)"; the
    # whole subject (RFC 2253) must equal that, so a marker hidden in another field, a prefix or suffix, a production or foreign CA, or the root of
    # another variant (dev keyring in a factory-test image and the reverse) cannot slip in. The certificate must also be a self-signed root.
    # The "not for production" rule applies to dev and factory-test only; the production keyring rule is supplied by Story 6.14 (not defined here).
    subject=$(${STAGING_BINDIR_NATIVE}/openssl x509 -in "$keyring" -noout -subject -nameopt RFC2253 2>/dev/null | sed 's/^subject= *//')
    issuer=$(${STAGING_BINDIR_NATIVE}/openssl x509 -in "$keyring" -noout -issuer -nameopt RFC2253 2>/dev/null | sed 's/^issuer= *//')
    want="CN=Velora ${VELORA_VARIANT} RAUC root (not for production),O=Velora"
    if [ "$subject" != "$want" ]; then
        bbfatal "VELORA_RAUC_KEYRING_FILE='$keyring' is not the ${VELORA_VARIANT} RAUC root: its subject is '$subject', expected exactly '$want' (the 'not for production' marker written by yocto/ci/rauc/gen-dev-root.sh; a keyring of another variant must not be baked into this image)"
    fi
    if [ "$issuer" != "$subject" ] || ! ${STAGING_BINDIR_NATIVE}/openssl verify -CAfile "$keyring" "$keyring" >/dev/null 2>&1; then
        bbfatal "VELORA_RAUC_KEYRING_FILE='$keyring' is not a self-signed root certificate (issuer '$issuer')"
    fi
    install -d ${D}${nonarch_libdir}/rauc
    install -m 0644 ${UNPACKDIR}/system.conf ${D}${nonarch_libdir}/rauc/system.conf
    install -m 0644 "$keyring" ${D}${nonarch_libdir}/rauc/ca.cert.pem
    install -d ${D}${sysconfdir}
    install -m 0644 ${UNPACKDIR}/fw_env.config ${D}${sysconfdir}/fw_env.config
}
FILES:${PN} += "${sysconfdir}/fw_env.config"
