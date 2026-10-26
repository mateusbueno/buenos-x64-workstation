SUMMARY = "Velora dev image for the RK3588 dev board (Story 1.4)"
DESCRIPTION = "Minimal systemd image: PREEMPT_RT mainline kernel, SSH, debug UART, dev-marked os-release, no swap, no core dumps. Dev only."
LICENSE = "MIT"

# No extensible SDK: its dependency tree (make, bash, ...) pulls GPL-3.0 build tools into the dependency graph. Must precede the inherit.
IMGCLASSES:remove = "populate_sdk_ext"

inherit core-image

# Licence policy from conf/distro/velora.conf: do_rootfs fails if an installed package has a GPL-3.0/LGPL-3.0/AGPL-3.0 licence.
INCOMPATIBLE_LICENSE = "${VELORA_INCOMPATIBLE_LICENSE}"
INCOMPATIBLE_LICENSE_EXCEPTIONS = "${VELORA_INCOMPATIBLE_LICENSE_EXCEPTIONS}"

# ssh-server-openssh: sshd (key login only; root password stays locked, no debug-tweaks).
# Authorised keys for dev access are provided at build time through VELORA_DEV_AUTHORIZED_KEYS_FILE (no keys in the repo).
IMAGE_FEATURES = "ssh-server-openssh"
IMAGE_FEATURES:remove = "debug-tweaks empty-root-password allow-empty-password allow-root-login"

IMAGE_INSTALL = " \
    packagegroup-core-boot \
    velora-system-config \
    systemd-networkd \
    libgcc \
    libstdc++ \
    kernel-modules \
"

# The audio stack and tools in these packages are NOT allowed in the image (Story 1.4 AC, NFR27):
# the flac/metaflac CLIs, libflac/libflac++ and libsndfile. A hard exclude makes the build fail if anything pulls them in.
PACKAGE_EXCLUDE = "flac metaflac libflac libflac++ libsndfile1 libsndfile1-bin"

# Journal/storage: nothing here enables swap or zram.
IMAGE_LINGUAS = ""

VELORA_DEV_AUTHORIZED_KEYS_FILE ?= ""
# rebuild the rootfs when the key file content changes (a missing file is checked by bbfatal below)
do_rootfs[file-checksums] += "${@'${VELORA_DEV_AUTHORIZED_KEYS_FILE}:True' if d.getVar('VELORA_DEV_AUTHORIZED_KEYS_FILE') else ''}"

ROOTFS_POSTPROCESS_COMMAND += "velora_dev_authorized_keys;"
velora_dev_authorized_keys() {
    keys="${VELORA_DEV_AUTHORIZED_KEYS_FILE}"
    if [ -n "$keys" ]; then
        if [ ! -s "$keys" ]; then
            bbfatal "VELORA_DEV_AUTHORIZED_KEYS_FILE='$keys' is set but the file is missing or empty"
        fi
        install -d -m 0700 ${IMAGE_ROOTFS}/home/root/.ssh
        install -m 0600 "$keys" ${IMAGE_ROOTFS}/home/root/.ssh/authorized_keys
    fi
}
