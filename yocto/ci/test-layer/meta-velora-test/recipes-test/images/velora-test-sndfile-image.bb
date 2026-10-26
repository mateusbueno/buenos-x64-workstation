require recipes-core/images/velora-image-dev.bb
SUMMARY = "Policy test image: dev image plus libsndfile1"
IMAGE_INSTALL:append = " libsndfile1"
