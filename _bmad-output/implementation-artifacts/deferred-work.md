
## Deferred from: code review of spec-11-12 (2026-10-19)

- Install `THIRD_PARTY_NOTICES.md` in the image and show it in Velora Studio's About (Story 11.12 AC2, first Then). Belongs to the image-build and Studio About stories; they must carry a dependency on this file. Until then the obligation is unmet for any shipped binary.
- `vendor_dirs` in `tools/ci/third_party_manifest.json` is `formatlib/third_party/sfizz`, a guess; Story 1.13 must set the real path and keep `sfizz_files` equal to the license table.
