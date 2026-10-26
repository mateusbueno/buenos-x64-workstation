require recipes-core/images/velora-image-dev.bb
SUMMARY = "Policy test image: dev image plus test-gpl3"
IMAGE_INSTALL:append = " test-gpl3"
