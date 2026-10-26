#!/usr/bin/env bash
# Story 6.12: builds the RAUC test image (RAUC 1.15.2 from the pinned release tarball, ~0.5 GB) and runs test-bundles.sh in it. Needs only Docker.
#   bash yocto/ci/rauc/run-tests.sh            run the tests
#   bash yocto/ci/rauc/run-tests.sh --clean    remove the image
# The repository is mounted read-only; keys are generated inside the container and vanish with it.
set -euo pipefail
cd "$(dirname "$0")/../../.."
IMAGE=velora-rauc-test
if [ "${1:-}" = "--clean" ]; then docker rmi "$IMAGE" >/dev/null 2>&1 && echo "removed image $IMAGE" || echo "image $IMAGE not present"; exit 0; fi
docker build -q -t "$IMAGE" yocto/ci/rauc >/dev/null
exec docker run --rm -v "$PWD/yocto":/src/yocto:ro "$IMAGE" bash /src/yocto/ci/rauc/test-bundles.sh
