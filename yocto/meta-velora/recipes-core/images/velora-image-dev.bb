SUMMARY = "Velora dev image for the RK3588 dev board (Story 1.4)"
DESCRIPTION = "Minimal systemd image: PREEMPT_RT mainline kernel, SSH, debug UART, dev-marked os-release, no swap, no core dumps. Dev only."

# Variant: dev. Everything shared with the other variants lives in velora-image-base.inc (Story 6.13).
VELORA_IMAGE_VARIANT = "dev"
require velora-image-base.inc

# ssh-server-openssh: sshd (key login only; root password stays locked, no debug-tweaks). DEV VARIANT ONLY.
# Authorised keys for dev access are provided at build time through VELORA_DEV_AUTHORIZED_KEYS_FILE (no keys in the repo).
IMAGE_FEATURES += "ssh-server-openssh"

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
