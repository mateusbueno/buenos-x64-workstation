#!/usr/bin/env bash
# Fetches sfizz 1.2.3 (commit 4e70dc0bef53b41f2853ed46e26f5911114c92d0) with submodules into an
# ignored build directory and builds its `sfizz_render` CLI. The oracle is test tooling only:
# it is never vendored, never linked into a shipped target (see tools/oracle/README.md).
#
# Usage: tools/oracle/build-sfizz-render.sh      (prints the path of sfizz_render on success)
# Env:   VELORA_ORACLE_DIR  work directory (default: tools/oracle/.work, git-ignored)
#        JOBS               parallel build jobs
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
work="${VELORA_ORACLE_DIR:-$here/.work}"
src="$work/sfizz"
bld="$work/build"
bin="$bld/library/bin/sfizz_render"
commit=4e70dc0bef53b41f2853ed46e26f5911114c92d0
jobs="${JOBS:-$( (nproc || sysctl -n hw.ncpu) 2>/dev/null || echo 4)}"

# Exit codes: 0 built (path printed), 77 prerequisites genuinely unavailable (no git/cmake, or the
# clone/fetch cannot reach the network), anything else a real failure (checkout, configure, build).
for tool in git cmake; do
  command -v "$tool" >/dev/null 2>&1 || { echo "build-sfizz-render: $tool not found" >&2; exit 77; }
done
stamp="$(dirname "$bin")/.built-commit"
if [ -x "$bin" ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$commit" ]; then echo "$bin"; exit 0; fi
rm -f "$stamp"   # a binary without a matching stamp is rebuilt

mkdir -p "$work"
if [ ! -d "$src/.git" ]; then
  git clone --quiet https://github.com/sfztools/sfizz.git "$src" || { echo "build-sfizz-render: cannot clone sfizz (network?)" >&2; exit 77; }
fi
if [ "$(git -C "$src" rev-parse HEAD 2>/dev/null)" != "$commit" ]; then
  git -C "$src" fetch --quiet --tags origin || { echo "build-sfizz-render: cannot fetch sfizz (network?)" >&2; exit 77; }
  git -C "$src" checkout --quiet "$commit"
fi
git -C "$src" submodule update --quiet --init --recursive
[ "$(git -C "$src" rev-parse HEAD)" = "$commit" ] || { echo "sfizz commit mismatch" >&2; exit 1; }

# sfizz 1.2.3 passes the 32-bit ARM flags -mfpu/-mfloat-abi for any "arm*" processor, which Apple
# clang rejects on arm64; naming the processor aarch64 skips them (no effect elsewhere).
extra=()
case "$(uname -m)" in arm64|aarch64) extra+=(-DPROJECT_SYSTEM_PROCESSOR=aarch64) ;; esac

cmake -S "$src" -B "$bld" -DCMAKE_BUILD_TYPE=Release \
  -DSFIZZ_JACK=OFF -DSFIZZ_RENDER=ON -DSFIZZ_LV2=OFF -DSFIZZ_VST=OFF -DSFIZZ_PURE_DATA=OFF \
  -DSFIZZ_TESTS=OFF -DSFIZZ_DEMOS=OFF -DSFIZZ_DEVTOOLS=OFF -DSFIZZ_BENCHMARKS=OFF \
  -DSFIZZ_SHARED=OFF -DSFIZZ_USE_SYSTEM_ABSEIL=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  "-DCMAKE_CXX_FLAGS=-Wno-missing-template-arg-list-after-template-kw" ${extra[@]+"${extra[@]}"} >&2
cmake --build "$bld" --target sfizz_render -j "$jobs" >&2
[ -x "$bin" ] || bin="$(find "$bld" -name sfizz_render -type f -perm -u+x | head -1)"
[ -n "$bin" ] || { echo "sfizz_render not found after build" >&2; exit 1; }
echo "$commit" > "$(dirname "$bin")/.built-commit"
echo "$bin"
