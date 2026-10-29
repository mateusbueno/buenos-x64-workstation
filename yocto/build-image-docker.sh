#!/usr/bin/env bash
# Builds the Velora RK3588 dev image and RAUC bundle inside Docker using kas and bitbake.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
IMAGE_NAME="velora-yocto-builder"
VOLUME_NAME="${VELORA_BUILD_VOLUME:-velora-yocto-build}"
OUT_DIR="$REPO_ROOT/build-yocto-output"
KEYS_DIR="$HOME/.velora-dev-keys/rauc"
SSH_KEY="$HOME/.ssh/id_ed25519.pub"

echo "=== Velora Yocto Docker Build ==="
echo "Repo: $REPO_ROOT"
echo "Volume: $VOLUME_NAME"
echo "Output: $OUT_DIR"

# 1. Check or generate dev keys
if [ ! -f "$KEYS_DIR/ca.cert.pem" ] || [ ! -f "$KEYS_DIR/signing.key.pem" ]; then
    echo "Generating dev RAUC keys in $KEYS_DIR..."
    bash "$REPO_ROOT/yocto/ci/rauc/gen-dev-root.sh" --variant dev
fi

# 2. Check SSH key
if [ ! -f "$SSH_KEY" ]; then
    echo "Warning: $SSH_KEY not found; root SSH key will be omitted"
    TOUCHED_KEY=1
    TMP_KEY="$(mktemp)"
    touch "$TMP_KEY"
    SSH_KEY="$TMP_KEY"
else
    TOUCHED_KEY=0
fi

# 3. Create host output dir
mkdir -p "$OUT_DIR"

# 4. Build builder image if needed
echo "Building / checking Docker builder image..."
docker build -q -t "$IMAGE_NAME" "$REPO_ROOT/yocto/docker" >/dev/null

# 5. Create volume and ensure permissions
docker volume create "$VOLUME_NAME" >/dev/null
docker run --rm -u root -v "$VOLUME_NAME":/work "$IMAGE_NAME" chown -R builder:builder /work

# 6. Run kas build inside container
echo "Starting Yocto build via kas in Docker (this may take 1.5 to 3 hours)..."
docker run --rm \
    --name velora-yocto-build-run \
    -v "$VOLUME_NAME":/work \
    -v "$REPO_ROOT/yocto":/src/yocto:ro \
    -v "$KEYS_DIR":/certs/rauc:ro \
    -v "$SSH_KEY":/certs/id_ed25519.pub:ro \
    -v "$OUT_DIR":/out \
    "$IMAGE_NAME" bash -c '
set -euo pipefail
PROJ=/work/proj
mkdir -p "$PROJ/yocto" "$PROJ/.yocto-work/build"
rsync -a --delete --exclude .yocto-work /src/yocto/ "$PROJ/yocto/"
cd "$PROJ"

export KAS_WORK_DIR="$PROJ"
export KAS_BUILD_DIR="$PROJ/.yocto-work/build"

cat <<EOF > "$PROJ/yocto/kas/build-dev.yml"
header:
  version: 14
  includes:
    - velora-dev.yml
local_conf_header:
  build-dev-custom: |
    VELORA_DEV_AUTHORIZED_KEYS_FILE = "/certs/id_ed25519.pub"
    VELORA_RAUC_KEYRING_FILE = "/certs/rauc/ca.cert.pem"
    VELORA_RAUC_SIGN_DIR = "/certs/rauc"
    BB_NUMBER_THREADS = "6"
    PARALLEL_MAKE = "-j 6"
EOF

echo "==> Running kas checkout..."
kas checkout yocto/kas/build-dev.yml

echo "==> Running kas build for target velora-image-dev..."
kas build yocto/kas/build-dev.yml

DEPLOY_DIR="$KAS_BUILD_DIR/tmp/deploy/images/rock-5b"
if [ -d "$DEPLOY_DIR" ]; then
    echo "==> Copying build artifacts to /out..."
    mkdir -p /out/images
    cp -L "$DEPLOY_DIR"/velora-image-dev* /out/images/ 2>/dev/null || true
    cp -L "$DEPLOY_DIR"/Image* /out/images/ 2>/dev/null || true
    cp -L "$DEPLOY_DIR"/*.dtb /out/images/ 2>/dev/null || true
    cp -L "$DEPLOY_DIR"/*.raucb /out/images/ 2>/dev/null || true
    echo "==> Artifacts copied successfully!"
    ls -lh /out/images/
fi
'

if [ "$TOUCHED_KEY" -eq 1 ]; then
    rm -f "$TMP_KEY"
fi

echo "=== Build finished ==="
ls -lh "$OUT_DIR/images" 2>/dev/null || true
