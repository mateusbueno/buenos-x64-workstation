require recipes-core/images/velora-image-dev.bb
SUMMARY = "Policy test image: dev image plus test-agpl3"
IMAGE_INSTALL:append = " test-agpl3"
