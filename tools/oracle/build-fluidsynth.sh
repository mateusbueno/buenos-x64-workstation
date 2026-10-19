#!/usr/bin/env bash
# Fetches FluidSynth 2.6.1 (tag v2.6.1 = commit 71e85b2ca6bf48641ba7e2261d8f2640ae473773) into an
# ignored build directory and builds its `fluidsynth` CLI. FluidSynth is LGPL-2.1: it is an oracle
# for the SF2 import tests (Story 5.3) ONLY. It is never vendored, never linked into a shipped target,
# only the CLI process is run on committed SF2/MIDI fixtures (see tools/oracle/README.md).
#
# Usage: tools/oracle/build-fluidsynth.sh      (prints the path of the fluidsynth binary on success)
# Env:   VELORA_ORACLE_DIR  work directory (default: tools/oracle/.work, git-ignored)
#        JOBS               parallel build jobs
# Exit codes: 0 built (path printed), 77 prerequisites genuinely unavailable (no git/cmake, network
# unreachable for the clone), anything else a real failure (checkout, configure, build).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
work="${VELORA_ORACLE_DIR:-$here/.work}"
src="$work/fluidsynth"
bld="$work/fluidsynth-build"
bin="$bld/src/fluidsynth"
tag=v2.6.1
commit=71e85b2ca6bf48641ba7e2261d8f2640ae473773
jobs="${JOBS:-$( (nproc || sysctl -n hw.ncpu) 2>/dev/null || echo 4)}"

for tool in git cmake; do
  command -v "$tool" >/dev/null 2>&1 || { echo "build-fluidsynth: $tool not found" >&2; exit 77; }
done
stamp="$bld/.built-commit"
if [ -x "$bin" ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$commit" ]; then echo "$bin"; exit 0; fi
rm -f "$stamp"

mkdir -p "$work"
if [ -e "$src" ] && [ ! -d "$src/.git" ]; then
  echo "build-fluidsynth: $src exists but is not a git checkout; remove it" >&2
  exit 1
fi
if [ ! -d "$src/.git" ]; then
  git clone --quiet https://github.com/FluidSynth/fluidsynth.git "$src" || { echo "build-fluidsynth: cannot clone FluidSynth (network?)" >&2; exit 77; }
fi
if [ "$(git -C "$src" rev-parse HEAD 2>/dev/null)" != "$commit" ]; then
  git -C "$src" fetch --quiet --tags origin || { echo "build-fluidsynth: cannot fetch FluidSynth (network?)" >&2; exit 77; }
  git -C "$src" checkout --quiet "$tag"
fi
[ "$(git -C "$src" rev-parse HEAD)" = "$commit" ] || { echo "FluidSynth commit mismatch" >&2; exit 1; }

# Only what the file renderer needs: libsndfile for `-F out.wav`, no audio/MIDI drivers, no
# network server, no readline; static library so the binary does not depend on a build-tree dylib.
log="$work/fluidsynth-configure.log"
if ! cmake -S "$src" -B "$bld" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -Denable-libsndfile=ON -Denable-network=OFF -Denable-readline=OFF -Denable-dbus=OFF \
  -Denable-jack=OFF -Denable-alsa=OFF -Denable-pulseaudio=OFF -Denable-pipewire=OFF -Denable-oss=OFF \
  -Denable-sdl3=OFF -Denable-sdl2=OFF -Denable-portaudio=OFF -Denable-coreaudio=OFF -Denable-coremidi=OFF \
  -Denable-aufile=ON -Denable-midishare=OFF -Denable-opensles=OFF -Denable-oboe=OFF -Denable-waveout=OFF \
  -Denable-dsound=OFF -Denable-wasapi=OFF -Denable-openmp=OFF -Denable-threads=ON \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >"$log" 2>&1; then
  cat "$log" >&2
  # A missing libsndfile (brew install libsndfile / apt-get install libsndfile1-dev) is a missing prerequisite.
  if grep -Eqi "could not find.*sndfile|sndfile.*(not found|-NOTFOUND)" "$log"; then
    echo "build-fluidsynth: libsndfile development files not found (brew install libsndfile / apt-get install libsndfile1-dev)" >&2
    exit 77
  fi
  echo "build-fluidsynth: configure failed" >&2
  exit 1
fi
cmake --build "$bld" --target fluidsynth -j "$jobs" >&2
[ -x "$bin" ] || bin="$(find "$bld" -name fluidsynth -type f -perm -u+x | head -1)"
[ -n "$bin" ] || { echo "fluidsynth not found after build" >&2; exit 1; }
echo "$commit" > "$bld/.built-commit"
echo "$bin"
