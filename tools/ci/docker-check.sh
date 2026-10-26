#!/usr/bin/env bash
# Runs the Linux CI jobs locally in Docker: purity, gcc build+tests, clang-20 RTSan build+tests, clang-20 ThreadSanitizer run of the threaded tests.
# The purity job runs the self-tests only; the artifact scans (nm on the built verify target and binaries: format_no_decrypt*, librarian_no_crypto) run through ctest in the gcc and rtsan jobs.
# The tsan job (Story 1.17) builds only the targets that run threads against each other (the stream manager and its I/O threads beside the audio thread, the parallel voice
# render, the worker pool) with clang-20 -fsanitize=thread and runs them: no data race may be reported (TSan exits non-zero on one).
# Notes: every job copies the tree into the container and configures there, which FETCHES LVGL from GitHub (ui/CMakeLists.txt): the container needs network at configure time and git
# (added to the image for Story 1.17). Changing tools/ci/docker/Dockerfile rebuilds the image on the next run (apt and the LLVM repository are downloaded again, a few minutes).
# The tsan job covers stream_test (stream manager, bundle block source, stream voices), stream_rt_traffic_test and the parallel render; it does NOT run head_mode_test (the Live Set
# loader in stream mode needs protobuf and the control plane) nor engine-host.
# Usage: tools/ci/docker-check.sh [purity|gcc|rtsan|tsan|all]   (default: all)
set -euo pipefail
cd "$(dirname "$0")/../.."
IMAGE=velora-ci-linux
docker build -q -t "$IMAGE" tools/ci/docker >/dev/null
job="${1:-all}"
run() { # builds in a container-local copy so the host tree stays untouched
  docker run --rm -v "$PWD":/src:ro "$IMAGE" bash -euc "
    mkdir /work/s && cd /src && tar --exclude=./build --exclude=./.git --exclude='__pycache__' --exclude=./tools/oracle/.work -cf - . | tar -xf - -C /work/s && cd /work/s
    $1"
}
case "$job" in purity|all)
  echo "== purity"; run "python3 tools/ci/check_core_deps.py && python3 tools/ci/check_core_deps.py --self-test && python3 tools/ci/check_third_party_notices.py && python3 tools/ci/check_third_party_notices.py --self-test && python3 tools/ci/check_no_private_keys.py && python3 tools/ci/check_no_private_keys.py --self-test && python3 tools/ci/check_format_no_decrypt.py --self-test && python3 tools/ci/check_librarian_no_crypto.py --self-test --cc cc && python3 tools/ci/check_content_pack_not_linked.py && python3 tools/ci/check_content_pack_not_linked.py --self-test && python3 tools/ci/check_no_dev_keys.py --self-test";; esac
case "$job" in gcc|all)
  echo "== linux-gcc"; run "CC=gcc CXX=g++ cmake -S . -B build -G Ninja -DVELORA_DEV_KEY_PROVIDER=ON && cmake --build build && ctest --test-dir build --output-on-failure --no-tests=error";; esac
case "$job" in rtsan|all)
  echo "== linux-clang-rtsan"; run "cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_C_COMPILER=clang-20 -DVELORA_DEV_KEY_PROVIDER=ON && grep -q VELORA_HAVE_RTSAN:INTERNAL=1 build/CMakeCache.txt && cmake --build build && for t in rtsan_integrity_test rtsan_protected_test rtsan_decrypt_unsafe_test rtsan_damage_report_unsafe_test protected_keyless_test protected_engine_test protected_render_test dev_key_provider_absent_when_off factory_live_set_tests factory_release_test factory_release_flagged_by_no_decrypt_scan rtsan_stream_test; do ctest --test-dir build -N | grep -q \" \$t\\\$\" || { echo \"docker-check: test \$t is not registered\"; exit 1; }; done && ctest --test-dir build --output-on-failure --no-tests=error";; esac
case "$job" in tsan|all)
  echo "== linux-clang-tsan"; run "cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_FLAGS='-fsanitize=thread -O1 -g' -DCMAKE_C_FLAGS='-fsanitize=thread -O1 -g' -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=thread' -DVELORA_RTSAN=OFF && cmake --build build --target stream_test stream_rt_traffic_test parallel_render_tests parallel_traffic_test worker_pool_tests && TSAN_OPTIONS='halt_on_error=1 second_deadlock_stack=1' ctest --test-dir build -R '^(stream_test|stream_rt_traffic_test|parallel_render_tests|parallel_traffic_test|worker_pool_tests)\$' --output-on-failure --no-tests=error";; esac
echo "docker-check: ok ($job)"
