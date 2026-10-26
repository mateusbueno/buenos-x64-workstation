---
title: 'Story 6.13: Dev and factory-test image variants with per-root keyrings (config, runner and Docker tests)'
type: 'feature'
created: '2026-10-25'
status: 'done'
baseline_revision: 'b2d7fddf57624cb731859dbc6eb846aae5aaf0d7'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/yocto/README.md', '{project-root}/tools/keys/KEY-HIERARCHY.md']
warnings: []
deferred:
  - summary: >-
      Not verified (no full build / board / prototype): do_rootfs post-processing, built /etc/os-release, sshd truly absent from a built rootfs, boot of either variant, on-screen rendering (console and file only), real hardware probes (keybed, panel/LED, codec loopback are stubs), runner limits are placeholders, no station-link transport.
    evidence: |-
      Verified in Docker: bitbake resolution of both variants, keyring/subject identity checks, cross-root rauc rejection both directions, runner logic with fakes (28 tests). See yocto/README.md.
    severity: high
  - summary: >-
      Design points left open: shared compatible string and slot names across variants (isolation rests on signature roots), /data on the NVMe under test (needs factory-test nofail fstab), unit identity (volatile machine-id, serial-number, untrusted clock), python3 RDEPENDS sub-packages to confirm on the first real build, use-bundle-signing-time=true trade-off, production variant rule (Story 6.14).
    evidence: |-
      Recorded in yocto/README.md open points.
    severity: medium
---

<intent-contract>

## Intent

**Problem:** Only a dev image exists. The factory line needs a factory-test image (own signing root, SSH off, factory test suite installed) built from the same sources, each variant trusting only its own RAUC root, and the layer must be structured so the production image can later be a third variant (AR3 partial, NFR23 partial, AR2 partial, AR34 partial).

**Approach:** Refactor the Yocto layer into a shared image base (`velora-image-base.inc` or class) plus two variants: `velora-image-dev` (existing behaviour) and `velora-image-factory-test` (SSH off, no dev authorized keys, factory test runner installed, factory-test keyring); a per-variant RAUC bundle recipe signed by that variant's root and a per-variant keyring (keyring path variables supplied from outside the repo, same fail-closed checks as the dev keyring: exactly one certificate with a variant marker in the subject); `/etc/os-release` states the variant (`VARIANT_ID=dev|factory-test`, a production placeholder documented but NOT built); a factory test runner (Python, one test per subsystem available now: MCU links, keybed scan, panel controls and LEDs, codec loopback, display and touch, NVMe read/write speed) with a pluggable hardware-probe layer, each test reporting `pass`/`fail`/`not present` (missing hardware never crashes the runner), a human-readable on-screen summary text and a machine-readable JSON report for the factory station link; Docker tests proving: a dev-signed bundle is rejected against the factory-test keyring and vice versa, the runner's results with simulated hardware (pass, fail, missing, crashing probe) and report schema.

## Boundaries & Constraints

**Always:** Reuse Story 6.12 mechanisms (rauc-conf keyring check, bundle recipe, gen-dev-root.sh, Docker RAUC tests) by generalising them over a variant, without weakening the dev checks. Private keys (dev and factory-test) are generated outside the repo by a script and never committed; check_no_private_keys.py stays green. The factory-test image has SSH OFF (no ssh-server feature, no sshd, no dev keys), the debug UART policy for factory-test is stated in the README (keep getty off unless the runner needs a console; choose and justify). Each image ships ONLY its own root's keyring. The runner is a self-contained Python module with a hardware abstraction (probes read from a mockable layer such as /sys, /dev and sockets; real probes are best-effort stubs with clear TODO where the hardware interface does not exist yet, e.g. the MCU link library of Stories 3.x runs on the engine side), a per-test timeout, exception isolation, and JSON report fields: variant, version, timestamp, unit identifiers if available, per-test {name, status, detail, duration_ms}, summary counts. The on-screen summary is plain text (UI rendering is out of scope).

**Never:** No production variant build (document the structure only), no production keys/keyrings, no full image build, no board tests, no changes outside `yocto/`, docs and workflow. Do not weaken or delete the Story 6.12 checks. Do not commit private key material.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Cross-root install | dev bundle vs factory-test keyring (and reverse) | rejected by rauc before any slot write | test asserts the specific failure |
| Own-root install | dev bundle vs dev keyring; factory-test vs its own | verifies | none |
| Keyring identity | factory-test keyring file | exactly one cert with the factory-test marker | bbfatal otherwise |
| Variants | resolved image configs | dev has ssh + keys + dev keyring; factory-test has no ssh, runner installed, factory keyring, os-release variant correct | static + bitbake -e checks |
| Runner: hardware present | probe returns ok | test pass | none |
| Runner: failing | probe returns bad value | test fail with detail | none |
| Runner: missing hw | probe reports absent | status not present, runner continues | none |
| Runner: probe crash/hang | exception or timeout | test fail (crash/timeout) detail, runner continues | none |
| Report | any run | valid JSON with the schema above; summary text lists each subsystem | none |

## Code Map

- `yocto/meta-velora/recipes-core/images/velora-image-dev.bb`, `recipes-core/rauc/*` (rauc-conf.bbappend, system.conf, bundles/velora-bundle-dev.bb), `velora-system-config`, `conf/distro/velora.conf`, `yocto/ci/rauc/*` (gen-dev-root.sh, test-bundles.sh), `yocto/ci/static_checks.py`, `validate-inner.sh`, `validate.sh`, `yocto/README.md` -- from Stories 1.4/6.11/6.12 to generalise.
- `tools/keys/KEY-HIERARCHY.md` -- roots and who holds which key (factory-test root: signing host operator; RAUC key + keyring per root).
- `_bmad-output/planning-artifacts/epics.md` Story 6.13 (and 6.14 for the production variant structure).
- New: `yocto/meta-velora/recipes-core/images/velora-image-base.inc`, `velora-image-factory-test.bb`, `recipes-core/bundles/velora-bundle-factory-test.bb`, `recipes-core/velora-factory-test/` (runner package + systemd unit), `yocto/ci/rauc/` generalised root generator and cross-root tests, runner tests under `yocto/ci/factory-test/`.

## Tasks & Acceptance

**Execution:**
- shared image base + dev and factory-test image recipes; os-release variant; SSH/keys only on dev; runner only on factory-test; production-variant structure documented
- generalise rauc-conf keyring handling and bundle recipe per variant (own keyring, marker check); generalise gen-dev-root.sh to generate a root for a named variant outside the repo (dev | factory-test; refuse production)
- factory test runner + unit + tests (Docker), report schema
- Docker tests: cross-root rejection both directions, own-root verification; static and bitbake -e checks of both variants; keep all Story 1.4/6.11/6.12 checks green
- README: variants, keyrings per root, runner, what is verified vs not (boot, on-screen display, real hardware probes)

**Acceptance Criteria:**
- Given the layer, then dev and factory-test images and bundles are defined from one source with separate signing roots and the structure leaves room for a production variant.
- Given each image, then it ships only its own root's keyring and a bundle of the other root is rejected (tested with rauc in Docker, both directions).
- Given the factory-test runner, then each subsystem test reports pass/fail/not present, a missing device never crashes it, and a JSON report plus text summary are produced.
- Given /etc/os-release of either variant, then it states the variant.
- Boot on a prototype and real hardware probes are recorded as not done.

## Spec Change Log

## Review Triage Log

### 2026-10-25 — Review pass (single focused reviewer; config work)
- findings:
  - `[high]` `patch` keyring identity bypasses (multi-PEM-type, unanchored subject marker, reserved --name), weak no-SSH assertion, runner decided pass from probe-supplied expectations, MCU presence-only pass, no data compare in NVMe test, 0600 report mode, stale report, vacuous validation checks — fixed (validation 182/81 -> 194/120)
  - `[medium]` `patch` runner unit ordering and fallback report location, display/touch wording, README overstatements (365-day validity, on-screen summary, production variant), KEY-HIERARCHY.md updates
  - `[medium]` `defer` A6 shared compatible string, B6 /data on NVMe under test, B8 unit identity, B9 python3 RDEPENDS

## Verification

**Commands:**
- `bash yocto/ci/validate.sh` -- expected: all checks pass (previous 119 + 35 plus the new ones)
- `python3 tools/ci/check_no_private_keys.py` -- expected: exit 0

## Auto Run Result

Summary: shared image base + dev and factory-test image recipes, per-variant kas, os-release variant, per-variant RAUC keyring and bundle recipes with strict identity checks, gen-dev-root.sh --variant, factory test runner (6 subsystem tests, JSON report /2, text summary), Docker tests.
Verification: bash yocto/ci/validate.sh 194 Yocto-side + 120 RAUC/runner checks pass (re-run by the orchestrator); check_no_private_keys ok.
