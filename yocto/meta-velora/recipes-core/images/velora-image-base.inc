# Shared base of every Velora image variant (Story 6.13). A variant recipe sets SUMMARY/DESCRIPTION, LICENSE-independent variant
# specifics (IMAGE_FEATURES additions, extra IMAGE_INSTALL, post-processing) and VELORA_IMAGE_VARIANT, then `require`s this file:
#   velora-image-dev            dev variant: SSH (key login), dev keyring, debug UART
#   velora-image-factory-test   factory-test variant: SSH off, factory test runner, factory-test keyring
#   (production)                NOT built, NOT defined: a third variant would be velora-image-production.bb requiring this file (README, "Image variants")
# A variant image carries ONLY its own root's RAUC keyring: rauc-conf takes the keyring from VELORA_RAUC_KEYRING_FILE and checks that its
# subject names VELORA_VARIANT (rauc-conf.bbappend). VELORA_VARIANT is set per build by the variant's kas file (kas/velora-<variant>.yml).
LICENSE = "MIT"

# No extensible SDK: its dependency tree (make, bash, ...) pulls GPL-3.0 build tools into the dependency graph. Must precede the inherit.
IMGCLASSES:remove = "populate_sdk_ext"

inherit core-image

# Licence policy from conf/distro/velora.conf: do_rootfs fails if an installed package has a GPL-3.0/LGPL-3.0/AGPL-3.0 licence.
INCOMPATIBLE_LICENSE = "${VELORA_INCOMPATIBLE_LICENSE}"
INCOMPATIBLE_LICENSE_EXCEPTIONS = "${VELORA_INCOMPATIBLE_LICENSE_EXCEPTIONS}"

# Common features: read-only root. SSH is a variant feature (dev adds ssh-server-openssh; factory-test and production have none).
# No debug-tweaks: root password stays locked in every variant.
IMAGE_FEATURES = "read-only-rootfs"
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

# Story 6.12/6.13: an image carries ONLY the keyring of its own variant's RAUC root (public certificate), supplied as VELORA_RAUC_KEYRING_FILE;
# the rauc-conf recipe fails the build with a clear message when it is missing or belongs to another root, so this layer still parses without keys.

# Disk layout (Story 6.11): the single committed wks (boot area, rootfs.0/rootfs.1 A/B slots, /data). Hard assignment: overrides
# the meta-rockchip default (WKS_FILE ?= "rockchip.wks"). Found through the layer's files/wic/ directory (WKS_SEARCH_PATH).
WKS_FILE = "velora-nvme.wks"

# Journal/storage: nothing here enables swap or zram.
IMAGE_LINGUAS = ""

# The variant this image belongs to; must equal VELORA_VARIANT (distro variable, becomes VARIANT_ID in /etc/os-release and selects the keyring
# marker in rauc-conf). Each variant's kas file sets VELORA_VARIANT; building an image with another variant's configuration fails here,
# before the rootfs is assembled, instead of producing an image whose os-release lies about its variant.
VELORA_IMAGE_VARIANT ?= ""
do_rootfs[prefuncs] += "velora_check_image_variant"
python velora_check_image_variant() {
    want = d.getVar('VELORA_IMAGE_VARIANT')
    have = d.getVar('VELORA_VARIANT')
    if not want or want != have:
        bb.fatal("image %s is variant '%s' but VELORA_VARIANT is '%s': build it with yocto/kas/velora-%s.yml (or set VELORA_VARIANT = \"%s\")" % (d.getVar('PN'), want, have, want, want))
}

# root has no usable password in every variant: oe-core's zap_empty_root_password (runs because empty-root-password is never a feature here)
# writes "*" into /etc/shadow; this fails the build if root ends up with an empty or real password hash instead.
ROOTFS_POSTPROCESS_COMMAND += "velora_assert_root_locked;"
velora_assert_root_locked() {
    shadow="${IMAGE_ROOTFS}${sysconfdir}/shadow"
    if [ ! -f "$shadow" ]; then
        bbfatal "cannot check the root account: $shadow does not exist"
    fi
    pw=$(sed -n 's/^root:\([^:]*\):.*/\1/p' "$shadow")
    case "$pw" in
        '*'|'!'*) ;;
        *) bbfatal "root account is not locked in $shadow (password field '$pw'): no variant may ship an empty or set root password" ;;
    esac
}
