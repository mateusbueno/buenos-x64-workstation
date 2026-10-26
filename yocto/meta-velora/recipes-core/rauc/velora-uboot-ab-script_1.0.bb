SUMMARY = "Velora U-Boot A/B boot script (Story 6.12): RAUC's BOOT_ORDER / BOOT_x_LEFT contract with 3 attempts"
DESCRIPTION = "Compiles files/uboot/velora-ab-boot.cmd to /boot/boot.scr. PLACEHOLDER for the boot chain of Story 1.6 (TODO(Story 1.6))."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COREBASE}/meta/COPYING.MIT;md5=3da9cfbcb788c80a0384361b4de20420"

FILESEXTRAPATHS:prepend := "${THISDIR}/../../files:"
SRC_URI = "file://uboot/velora-ab-boot.cmd"
S = "${UNPACKDIR}"

# a script image: no cross toolchain or libc needed, only mkimage
INHIBIT_DEFAULT_DEPS = "1"
DEPENDS = "u-boot-tools-native"
# The device tree file name is the board's (meta-rockchip KERNEL_DEVICETREE); the first entry is used.
# TODO(Story 1.6): confirm the device tree path under /boot and the kernel image name with the real build.
VELORA_AB_FDT ?= "${@(d.getVar('KERNEL_DEVICETREE') or 'rockchip/rk3588-rock-5b.dtb').split()[0]}"
PACKAGE_ARCH = "${MACHINE_ARCH}"

do_compile() {
    sed -e 's#@VELORA_KERNEL_ARGS@#${UBOOT_EXTLINUX_KERNEL_ARGS}#' -e 's#@VELORA_FDT@#${VELORA_AB_FDT}#' \
        ${UNPACKDIR}/uboot/velora-ab-boot.cmd > ${B}/boot.cmd
    mkimage -A arm64 -T script -C none -n "Velora A/B boot" -d ${B}/boot.cmd ${B}/boot.scr
}

do_install() {
    install -d ${D}/boot
    install -m 0644 ${B}/boot.scr ${D}/boot/boot.scr
}
FILES:${PN} = "/boot/boot.scr"
