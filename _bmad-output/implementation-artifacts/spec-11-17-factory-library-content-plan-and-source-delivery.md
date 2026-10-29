---
title: 'Story 11.17: Factory library content plan and source delivery'
type: 'feature'
created: '2026-10-29'
status: 'done'
context:
  - '_bmad-output/planning-artifacts/epics.md'
  - '_bmad-output/business/11-17-factory-library-content-plan.md'
  - 'tools/content-pack/vendor-ids'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Define the complete 17-category factory library content plan, Minimum Shippable Set (ready by month 12), NVMe storage budget leaving `>= 600 GB` free for user imports, signed rights records confirming 100% own work and zero third-party trademarks (NFR29), and reserve the permanent vendor `library_id`s and factory Live Set `program_id`s for the Epic 6 `tools/content-pack` and `tools/factory-release` pipeline.

**Approach:**
1. Author the complete signed content plan, storage budget verification (`38.51 GB` raw / `22.34 GB` packed out of `960 GB` `/data`, leaving `921.49 GB` user headroom), per-item checklist, rights record, and handoff table in `_bmad-output/business/11-17-factory-library-content-plan.md`.
2. Register the reserved v1 factory vendor `library_id` UUIDs in `tools/content-pack/vendor-ids`.

## Boundaries & Constraints

**Always:**
- Keep all instrument, preset, file, and library names free of third-party trademarks (NFR29).
- Preserve existing entries and format rules in `tools/content-pack/vendor-ids`.

**Ask First:** None.

**Never:**
- Include third-party samples or unlicensed recordings.

</frozen-after-approval>

## Code Map

- `_bmad-output/business/11-17-factory-library-content-plan.md` — Signed 17-category factory content plan, Minimum Shippable Set, storage budget, rights record, and Epic 6 handoff table.
- `tools/content-pack/vendor-ids` — Reserved UUIDv4 vendor library IDs for the v1 factory libraries.

## Tasks & Acceptance Criteria

- [x] **Task 1 (AC1):** Complete the 17-category content plan table, Minimum Shippable Set (ready by month 12, including factory piano and organ presets), and NVMe storage budget in `_bmad-output/business/11-17-factory-library-content-plan.md`.
- [x] **Task 2 (AC2):** Record the lossless SFZ+WAV delivery checklist, company-owned rights declaration, and reserved vendor `library_id`s / `program_id`s in `_bmad-output/business/11-17-factory-library-content-plan.md` and `tools/content-pack/vendor-ids`.

## Verification

- `ctest --test-dir build --output-on-failure -R "content_pack|factory_release"`
