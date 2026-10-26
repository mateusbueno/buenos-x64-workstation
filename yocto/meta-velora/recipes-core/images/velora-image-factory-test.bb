SUMMARY = "Velora factory-test image for the production line (Story 6.13)"
DESCRIPTION = "Same sources as the dev image, but: SSH off (no sshd, no authorized keys), no login on the debug UART, the factory test runner installed, and ONLY the factory-test RAUC keyring. Signed with the factory-test root, never the dev root."

# Variant: factory-test. Everything shared with the other variants lives in velora-image-base.inc.
VELORA_IMAGE_VARIANT = "factory-test"
require velora-image-base.inc

# No SSH: no ssh-server feature, and the packages are excluded outright so nothing can pull a server in by dependency.
IMAGE_FEATURES:remove += "ssh-server-openssh ssh-server-dropbear"
PACKAGE_EXCLUDE += "openssh openssh-sshd openssh-sftp-server dropbear"

# The factory test runner (one test per subsystem, JSON report plus text summary) and its systemd unit.
IMAGE_INSTALL += "velora-factory-test"

# Debug UART policy (Story 6.13, README "Image variants"): the kernel console stays (boot messages are readable for line diagnostics), but NO
# login getty runs on the serial console and root has no password: the runner needs no console input, and a getty nobody can use is only
# attack surface. The runner reports through the on-screen summary and the JSON report file the station link collects.
ROOTFS_POSTPROCESS_COMMAND += "velora_factory_no_login; velora_factory_assert_no_ssh;"
velora_factory_no_login() {
    install -d ${IMAGE_ROOTFS}${sysconfdir}/systemd/system
    consoles='${SERIAL_CONSOLES}'
    for entry in $consoles; do
        tty="${entry#*;}"
        rm -f ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/getty.target.wants/serial-getty@$tty.service
        ln -sf /dev/null ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/serial-getty@$tty.service
    done
}

# Defence in depth: whatever ends up in the rootfs, a factory-test image with an SSH daemon, an SSH unit or any authorized key fails the build.
VELORA_DEV_AUTHORIZED_KEYS_FILE ?= ""
velora_factory_assert_no_ssh() {
    if [ -n "${VELORA_DEV_AUTHORIZED_KEYS_FILE}" ]; then
        bbfatal "VELORA_DEV_AUTHORIZED_KEYS_FILE is set: the factory-test image never carries authorized keys (use the dev variant for SSH access)"
    fi
    for f in ${IMAGE_ROOTFS}${sbindir}/sshd ${IMAGE_ROOTFS}${sysconfdir}/ssh/sshd_config ${IMAGE_ROOTFS}/home/root/.ssh \
             ${IMAGE_ROOTFS}${systemd_system_unitdir}/sshd.service ${IMAGE_ROOTFS}${systemd_system_unitdir}/sshd.socket ${IMAGE_ROOTFS}${systemd_system_unitdir}/sshd@.service; do
        if [ -e "$f" ]; then
            bbfatal "factory-test image contains SSH material: $f"
        fi
    done
    # whatever else: any authorized_keys*, sshd*, dropbear*, ssh*.socket or ssh*.service anywhere in the rootfs
    found=$(find ${IMAGE_ROOTFS} \( -name 'authorized_keys*' -o -name 'sshd*' -o -name 'dropbear*' -o -name 'ssh*.socket' -o -name 'ssh*.service' \) -print 2>/dev/null | head -1)
    if [ -n "$found" ]; then
        bbfatal "factory-test image contains SSH material: $found"
    fi
}
