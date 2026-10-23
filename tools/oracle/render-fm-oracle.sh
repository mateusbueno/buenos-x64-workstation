#!/usr/bin/env bash
# Renders a 6-operator SysEx voice + MIDI file through the MSFA FM oracle: `fm_oracle_render` at 48 kHz.
#
# Usage: tools/oracle/render-fm-oracle.sh <file.syx> <voice_slot> <file.mid> <out.wav>
# Env:   VELORA_FM_ORACLE  path of an existing fm_oracle_render CLI (default: built by build-fm-oracle.sh)
# Exit code 77 when no oracle is available and it cannot be built.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
[ $# -eq 4 ] || { echo "usage: $0 <file.syx> <voice_slot (1..32)> <file.mid> <out.wav>" >&2; exit 2; }
syx="$1"; slot="$2"; mid="$3"; out="$4"
bin="${VELORA_FM_ORACLE:-}"
if [ -z "$bin" ]; then
  rc=0
  bin="$("$here/build-fm-oracle.sh")" || rc=$?
  [ "$rc" -eq 0 ] || exit "$rc"
fi
[ -x "$bin" ] || { echo "fm_oracle_render not executable: $bin" >&2; exit 77; }
abs() { case "$1" in /*) echo "$1" ;; *) echo "$PWD/$1" ;; esac; }
rm -f "$out"
"$bin" "$(abs "$syx")" "$slot" "$(abs "$mid")" "$(abs "$out")"
[ -s "$out" ] || { echo "fm_oracle_render wrote no output" >&2; exit 1; }
