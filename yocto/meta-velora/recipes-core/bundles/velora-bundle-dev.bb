SUMMARY = "Velora dev firmware bundle (Story 6.12): the dev image rootfs, signed with the dev RAUC root"
DESCRIPTION = "A plain-format RAUC bundle with one slot class (rootfs) written to whichever of rootfs.0/rootfs.1 is inactive. Dev root only: the factory-test bundle is velora-bundle-factory-test (Story 6.13), production bundles are Story 6.14 / Epic 8."

VELORA_BUNDLE_VARIANT = "dev"
require velora-bundle-base.inc

RAUC_BUNDLE_DESCRIPTION = "Velora dev image (dev-signed)"
RAUC_SLOT_rootfs = "velora-image-dev"
