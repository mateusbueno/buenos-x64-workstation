# buenos-x64-workstation
## Local validation (replaces automatic CI)

GitHub Actions minutes are limited, so the CI workflow is manual-only. Run these locally before every commit:

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure   # macOS host
tools/ci/docker-check.sh            # Linux (arm64) in Docker: purity, gcc, clang-20 RealtimeSanitizer
```
