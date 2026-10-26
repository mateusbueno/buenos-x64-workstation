SUMMARY = "Velora factory-test firmware bundle (Story 6.13): the factory-test image rootfs, signed with the factory-test RAUC root"
DESCRIPTION = "A plain-format RAUC bundle with one slot class (rootfs). Signed with the factory-test root, which only the factory-test signing host holds; the dev image rejects it and the factory-test image rejects dev-signed bundles."

VELORA_BUNDLE_VARIANT = "factory-test"
require velora-bundle-base.inc

RAUC_BUNDLE_DESCRIPTION = "Velora factory-test image (factory-test-signed)"
RAUC_SLOT_rootfs = "velora-image-factory-test"
