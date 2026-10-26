#!/usr/bin/env bash
# Usage: make-ssh-overlay.sh <repo-root> <public-key-file> <out.yml>
# Writes a kas overlay on top of yocto/kas/velora-dev.yml that installs the public key for root SSH in the dev image.
set -euo pipefail
root="$1"; keys="$2"; out="$3"
printf 'header:\n  version: 14\n  includes:\n    - %s/yocto/kas/velora-dev.yml\nlocal_conf_header:\n  dev-ssh: |\n    VELORA_DEV_AUTHORIZED_KEYS_FILE = "%s"\n' "$root" "$keys" > "$out"
