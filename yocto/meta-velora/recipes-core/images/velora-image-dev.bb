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
IMAGE_FEATURES = "ssh-server-openssh read-only-rootfs"
IMAGE_FEATURES:remove = "debug-tweaks empty-root-password allow-empty-password allow-root-login"

IMAGE_INSTALL = " \
    packagegroup-core-boot \
    velora-system-config \
    velora-service-users \
    systemd-networkd \
    libgcc \
    libstdc++ \
    kernel-modules \
    rauc \
    rauc-conf \
    velora-rauc-mark-good \
    velora-rollback-record \
    velora-uboot-ab-script \
"

# The audio stack and tools in these packages are NOT allowed in the image (Story 1.4 AC, NFR27):
# the flac/metaflac CLIs, libflac/libflac++ and libsndfile. A hard exclude makes the build fail if anything pulls them in.
PACKAGE_EXCLUDE = "flac metaflac libflac libflac++ libsndfile1 libsndfile1-bin"

# Story 6.12: the stock meta-rauc mark-good unit (wanted by multi-user.target, waits only for boot-complete.target) is not wanted; ours is
# velora-rauc-mark-good, gated on the engine, ui and sysd readiness targets.
BAD_RECOMMENDATIONS += "rauc-mark-good"

# Story 6.12: the plain ext4 rootfs image is what the RAUC bundle ships for slot rootfs (RAUC_IMAGE_FSTYPE in the bundle recipe).
IMAGE_FSTYPES += "ext4"

# Story 6.12: the dev image carries ONLY the dev keyring (public certificate of the dev RAUC root), supplied as VELORA_RAUC_KEYRING_FILE;
# the rauc-conf recipe fails the build with a clear message when it is missing, so this layer still parses without keys.

# Disk layout (Story 6.11): the single committed wks (boot area, rootfs.0/rootfs.1 A/B slots, /data). Hard assignment: overrides
# the meta-rockchip default (WKS_FILE ?= "rockchip.wks"). Found through the layer's files/wic/ directory (WKS_SEARCH_PATH).
WKS_FILE = "velora-nvme.wks"

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
