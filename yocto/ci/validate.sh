#!/usr/bin/env bash
# Story 1.4: validates the yocto/ tree in Docker WITHOUT building an image.
#   bash yocto/ci/validate.sh            run all checks (first run clones the pinned layers into a Docker volume, ~1.5 GB)
#   bash yocto/ci/validate.sh --clean    remove the Docker volume (layer clones) and exit
# Needs only Docker. Nothing is written to the repository. Runs the Yocto parse checks (validate-inner.sh) and the RAUC tests (rauc/run-tests.sh).
set -euo pipefail
cd "$(dirname "$0")/../.."
IMAGE=velora-yocto-validate
VOLUME="${VELORA_YOCTO_VOLUME:-velora-yocto-work}"
if [ "${1:-}" = "--clean" ]; then bash yocto/ci/rauc/run-tests.sh --clean; if docker volume rm "$VOLUME" >/dev/null 2>&1; then echo "removed volume $VOLUME"; elif docker volume inspect "$VOLUME" >/dev/null 2>&1; then echo "cannot remove volume $VOLUME: it is in use (stop the container using it first)" >&2; exit 1; else echo "volume $VOLUME does not exist, nothing to clean"; fi; exit 0; fi
docker build -q -t "$IMAGE" yocto/docker >/dev/null
docker volume create "$VOLUME" >/dev/null
docker run --rm -u root -v "$VOLUME":/work "$IMAGE" chown builder:builder /work
RC=0
docker run --rm -v "$PWD/yocto":/src/yocto:ro -v "$PWD/.github":/src/.github:ro -v "$VOLUME":/work "$IMAGE" bash /src/yocto/ci/validate-inner.sh || RC=1
# Story 6.12: RAUC bundle, signature, rollback-record and boot-script tests (own small image: RAUC 1.15.2 from the pinned tarball)
echo "== RAUC tests (yocto/ci/rauc, Docker)"
bash yocto/ci/rauc/run-tests.sh || RC=1
exit $RC
