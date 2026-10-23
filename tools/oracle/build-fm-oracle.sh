#!/usr/bin/env bash
# Fetches Music Synthesizer for Android (MSFA / Dexed core, commit f67d41d313b7dc85f6fb99e79e515cc9d208cfff)
# into an ignored build directory and builds the `fm_oracle_render` CLI.
# MSFA is Apache-2.0: it is an unshipped test oracle for 6-operator FM null tests (Story 12.2) ONLY.
# It is never vendored into any shipped directory, never linked into a shipped target, only run as
# a separate process on committed FM fixtures (see tools/oracle/README.md).
#
# Usage: tools/oracle/build-fm-oracle.sh   (prints the path of the fm_oracle_render binary on success)
# Env:   VELORA_ORACLE_DIR  work directory (default: tools/oracle/.work, git-ignored)
# Exit codes: 0 built (path printed), 77 prerequisites unavailable (no git/compiler, network
# unreachable for clone), anything else a real failure.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
work="${VELORA_ORACLE_DIR:-$here/.work}"
src="$work/msfa"
bld="$work/bin"
bin="$bld/fm_oracle_render"
commit=f67d41d313b7dc85f6fb99e79e515cc9d208cfff

for tool in git; do
  command -v "$tool" >/dev/null 2>&1 || { echo "build-fm-oracle: $tool not found" >&2; exit 77; }
done

CXX="${CXX:-}"
if [ -z "$CXX" ]; then
  for candidate in clang++ g++ c++; do
    if command -v "$candidate" >/dev/null 2>&1; then
      CXX="$candidate"
      break
    fi
  done
fi
[ -n "$CXX" ] || { echo "build-fm-oracle: C++ compiler not found" >&2; exit 77; }

stamp="$work/.msfa-built-commit"
if [ -x "$bin" ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$commit" ]; then
  echo "$bin"
  exit 0
fi
rm -f "$stamp"

mkdir -p "$work" "$bld"
if [ -e "$src" ] && [ ! -d "$src/.git" ]; then
  echo "build-fm-oracle: $src exists but is not a git checkout; remove it" >&2
  exit 1
fi
if [ ! -d "$src/.git" ]; then
  git clone --quiet https://github.com/google/music-synthesizer-for-android.git "$src" || { echo "build-fm-oracle: cannot clone MSFA (network?)" >&2; exit 77; }
fi
if [ "$(git -C "$src" rev-parse HEAD 2>/dev/null)" != "$commit" ]; then
  git -C "$src" fetch --quiet origin || { echo "build-fm-oracle: cannot fetch MSFA (network?)" >&2; exit 77; }
  git -C "$src" checkout --quiet "$commit"
fi
[ "$(git -C "$src" rev-parse HEAD)" = "$commit" ] || { echo "MSFA commit mismatch" >&2; exit 1; }

msfa_jni="$src/app/src/main/jni"
adapter="$here/../oracle-adapter/fm_oracle_render.cpp"
offline_src="$here/../offline-render/src"

"$CXX" -std=c++17 -O2 \
  -I "$msfa_jni" \
  -I "$offline_src" \
  "$msfa_jni/dx7note.cc" \
  "$msfa_jni/env.cc" \
  "$msfa_jni/exp2.cc" \
  "$msfa_jni/fm_core.cc" \
  "$msfa_jni/fm_op_kernel.cc" \
  "$msfa_jni/freqlut.cc" \
  "$msfa_jni/lfo.cc" \
  "$msfa_jni/pitchenv.cc" \
  "$msfa_jni/sin.cc" \
  "$msfa_jni/patch.cc" \
  "$offline_src/smf.cpp" \
  "$offline_src/wav.cpp" \
  "$adapter" \
  -o "$bin" >&2

[ -x "$bin" ] || { echo "fm_oracle_render binary not executable" >&2; exit 1; }
echo "$commit" > "$stamp"
echo "$bin"
