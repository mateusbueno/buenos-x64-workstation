#!/usr/bin/env bash
# Renders an SFZ + MIDI file through the sfizz oracle: sfizz_render at 48 kHz, 64-frame blocks.
#
# Usage: tools/oracle/render-oracle.sh <file.sfz> <file.mid> <out.wav> [quality]
#   quality  sfizz sample quality 0..10 (default 2 = Hermite, the sfizz default; 1 = linear)
# Env:   VELORA_SFIZZ_RENDER  path of an existing sfizz_render (default: built by build-sfizz-render.sh)
#        VELORA_SFIZZ_BLOCKSIZE  sfizz_render --blocksize in frames (default 64; the reference corpus job uses 1, see tools/corpus/README.md)
# The output is sfizz's own 16-bit PCM WAV, untouched (the null-test harness decodes it, see
# tools/oracle/README.md). Exit code 77 when no oracle is available and it cannot be built.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
[ $# -ge 3 ] || { echo "usage: $0 <file.sfz> <file.mid> <out.wav> [quality]" >&2; exit 2; }
sfz="$1"; mid="$2"; out="$3"; quality="${4:-2}"
bin="${VELORA_SFIZZ_RENDER:-}"
if [ -z "$bin" ]; then
  rc=0
  bin="$("$here/build-sfizz-render.sh")" || rc=$?
  [ "$rc" -eq 0 ] || exit "$rc"   # 77 = prerequisites unavailable; any other code is a real build failure
fi
[ -x "$bin" ] || { echo "sfizz_render not executable: $bin" >&2; exit 77; }
# sfizz_render keeps rendering until the output is silent; a voice that never releases (a sfizz
# corner case) would fill the disk. Cap the output file at about 128 MB.
ulimit -f 131072
abs() { case "$1" in /*) echo "$1" ;; *) echo "$PWD/$1" ;; esac; }
# sfizz_render resolves its arguments against the current directory; give it absolute paths.
"$bin" --sfz "$(abs "$sfz")" --midi "$(abs "$mid")" --wav "$(abs "$out")" \
  --samplerate 48000 --blocksize "${VELORA_SFIZZ_BLOCKSIZE:-64}" --quality "$quality" --polyphony 64
