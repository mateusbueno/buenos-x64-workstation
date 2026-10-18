#!/usr/bin/env bash
# Runs the Linux CI jobs locally in Docker: purity, gcc build+tests, clang-20 RTSan build+tests.
# Usage: tools/ci/docker-check.sh [purity|gcc|rtsan|all]   (default: all)
set -euo pipefail
cd "$(dirname "$0")/../.."
IMAGE=velora-ci-linux
docker build -q -t "$IMAGE" tools/ci/docker >/dev/null
job="${1:-all}"
run() { # builds in a container-local copy so the host tree stays untouched
  docker run --rm -v "$PWD":/src:ro "$IMAGE" bash -euc "
    mkdir /work/s && cd /src && tar --exclude=./build --exclude=./.git -cf - . | tar -xf - -C /work/s && cd /work/s
    $1"
}
case "$job" in purity|all)
  echo "== purity"; run "python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test";; esac
case "$job" in gcc|all)
  echo "== linux-gcc"; run "CC=gcc CXX=g++ cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure --no-tests=error";; esac
case "$job" in rtsan|all)
  echo "== linux-clang-rtsan"; run "cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_C_COMPILER=clang-20 && grep -q VELORA_HAVE_RTSAN:INTERNAL=1 build/CMakeCache.txt && cmake --build build && ctest --test-dir build --output-on-failure --no-tests=error";; esac
echo "docker-check: ok ($job)"
