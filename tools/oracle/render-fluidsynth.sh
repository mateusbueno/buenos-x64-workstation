#!/usr/bin/env bash
# Renders an SF2 + MIDI file through the FluidSynth oracle: `fluidsynth -F` (fast file rendering) at 48 kHz.
#
# Usage: tools/oracle/render-fluidsynth.sh <file.sf2> <file.mid> <out.wav>
# Env:   VELORA_FLUIDSYNTH  path of an existing fluidsynth CLI (default: built by build-fluidsynth.sh)
# Exit code 77 when no oracle is available and it cannot be built.
#
# Flags (all documented in tools/oracle/README.md):
#   -ni                 no shell, no MIDI input
#   -F out.wav -r 48000 render the MIDI file to a 16-bit stereo WAV at 48 kHz (audio.file.format s16, the default)
#   -R 0 -C 0           reverb and chorus off (their sends would add a wet signal the engine does not have)
#   -g 0.8409           master gain = 2^(-1/4). FluidSynth 2.6.1 always runs a 2-pole low-pass at 0.45 x the sample rate
#                       whose gain is normalised by 1/sqrt(Q) with Q = 0.707 for a filter without resonance: its output is
#                       1.51 dB (x1.189) above the SoundFont reference level at -g 1, independent of pan, velocity and key
#                       (measured and found in src/rvoice/fluid_iir_filter_impl.cpp, see README.md); 0.8409 undoes it
#   -o synth.midi-bank-select=mma   bank = CC0 * 128 + CC32, so bank 128 (percussion) is selectable on any channel
# Interpolation is FluidSynth's default (4th order). The output is FluidSynth's own file, untouched.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
[ $# -eq 3 ] || { echo "usage: $0 <file.sf2> <file.mid> <out.wav>" >&2; exit 2; }
sf2="$1"; mid="$2"; out="$3"
bin="${VELORA_FLUIDSYNTH:-}"
if [ -z "$bin" ]; then
  rc=0
  bin="$("$here/build-fluidsynth.sh")" || rc=$?
  [ "$rc" -eq 0 ] || exit "$rc"   # 77 = prerequisites unavailable; any other code is a real build failure
fi
[ -x "$bin" ] || { echo "fluidsynth not executable: $bin" >&2; exit 77; }
# The player waits for the voices and then 2 s of grace: cap the output file (about 64 MB).
ulimit -f 131072
abs() { case "$1" in /*) echo "$1" ;; *) echo "$PWD/$1" ;; esac; }
rm -f "$out"
"$bin" -ni -F "$(abs "$out")" -r 48000 -g 0.8409 -R 0 -C 0 -o synth.midi-bank-select=mma "$(abs "$sf2")" "$(abs "$mid")" >/dev/null
[ -s "$out" ] || { echo "fluidsynth wrote no output" >&2; exit 1; }
