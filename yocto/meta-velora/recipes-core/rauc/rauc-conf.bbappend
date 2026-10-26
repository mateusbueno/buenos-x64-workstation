# Story 6.12: Velora RAUC system configuration, dev keyring and U-Boot environment access.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# Path of the PUBLIC certificate of the dev RAUC root (the keyring). Supplied at build time, never committed (tools/keys/KEY-HIERARCHY.md,
# "RAUC bundle signing | dev"). Generate it with yocto/ci/rauc/gen-dev-root.sh and set, e.g. in a kas overlay or local.conf:
#   VELORA_RAUC_KEYRING_FILE = "/home/me/.velora-dev-keys/rauc/ca.cert.pem"
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
    keyring="${VELORA_RAUC_KEYRING_FILE}"
    if [ -z "$keyring" ]; then
        bbfatal "VELORA_RAUC_KEYRING_FILE is not set: the dev image needs the public certificate of the dev RAUC root (see yocto/README.md, 'RAUC')"
    fi
    if [ ! -s "$keyring" ]; then
        bbfatal "VELORA_RAUC_KEYRING_FILE='$keyring' is missing or empty"
    fi
    if grep -q "PRIVATE KEY" "$keyring"; then
        bbfatal "VELORA_RAUC_KEYRING_FILE='$keyring' contains a private key: the image may carry only the public certificate"
    fi
    ncerts=$(grep -c "BEGIN CERTIFICATE" "$keyring" || true)
    if [ "$ncerts" != "1" ]; then
        bbfatal "VELORA_RAUC_KEYRING_FILE='$keyring' must hold exactly one PEM certificate (the dev root), found $ncerts"
    fi
    # the dev image trusts only the dev root, which gen-dev-root.sh marks in its subject: a production or foreign CA must not slip in
    if ! ${STAGING_BINDIR_NATIVE}/openssl x509 -in "$keyring" -noout -subject 2>/dev/null | grep -q "not for production"; then
        bbfatal "VELORA_RAUC_KEYRING_FILE='$keyring' is not a dev RAUC root: its subject lacks the 'not for production' marker written by yocto/ci/rauc/gen-dev-root.sh"
    fi
    install -d ${D}${nonarch_libdir}/rauc
    install -m 0644 ${UNPACKDIR}/system.conf ${D}${nonarch_libdir}/rauc/system.conf
    install -m 0644 "$keyring" ${D}${nonarch_libdir}/rauc/ca.cert.pem
    install -d ${D}${sysconfdir}
    install -m 0644 ${UNPACKDIR}/fw_env.config ${D}${sysconfdir}/fw_env.config
}
FILES:${PN} += "${sysconfdir}/fw_env.config"
