---
title: 'Story 12.8: FM and VA timbre parameters in the versioned formats'
type: 'feature'
created: '2026-10-22'
status: 'done'
baseline_revision: 'cb2d864'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/proto/params.json'
  - '{project-root}/proto/gen_params.py'
  - '{project-root}/proto/live_set.proto'
  - '{project-root}/engine/host/tests/live_set_test.cpp'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists need FM and VA timbres and their edits saved in Live Sets, scenes, ASSIGN encoder maps, and backups like any other part, so that sounds survive Write operations, power cycles, firmware updates, rollbacks, and restores.

**Approach:**
1. **Generated Parameter Registry (`proto/params.json` & `gen_params.py`)**:
   - Add parameter definitions for 6-operator FM (1000..1103) and Virtual Analog (1200..1266).
   - Every parameter has a stable `id`, `name`, `range` (`min`, `max`), `unit`, `default`, `class` (`performance`), and `locale_key` for its localized display name.
   - Purely additive: existing parameter IDs, field numbers, and semantics remain completely untouched.
   - Support `locale_key` in `gen_params.py` emitting `locale_key_of(id)` in `velora::host::param_names`.
   - All performance parameters default to `assignable: true`, allowing ASSIGN encoders (K1..K8) to map any performance-class FM/VA parameter.
2. **Live Set Storage & Scene Overrides (`live_set.proto` & `live_set_store.cpp`)**:
   - Part engine data stores FM (`engine_type_id = 3`) and VA (`engine_type_id = 4`) timbre parameters in `EngineData.params`.
   - Scenes store FM/VA performance parameter overrides in `ScenePart.engine_params` (never the timbre assignment).
   - Round-trip fidelity: saving and reading back preserves all FM and VA parameters bit-for-bit.
3. **Compatibility & Rollback Resilience (AR29, FR14)**:
   - Compatibility corpus: all prior releases' saved Live Sets open unchanged with no migration.
   - N-1 reader rollback: a Live Set saved with FM/VA parts opened by an N-1 reader (or build without FM/VA) loads without error, FM/VA parts resolve to `kEngineNotAvailable` (playing silent, never substituted by another timbre), and unknown fields are preserved intact on re-save.

## Boundaries & Constraints

**Always:**
- Purely additive: never mutate or renumber existing parameter IDs or protobuf tags.
- Round-trip bit-identical parameter values.
- Unknown fields preserved across read-write cycles.
- Compatible with ASSIGN encoder mapping and Scene parameter recall.

**Never:**
- Break compatibility corpus of prior releases.
- Substitute unavailable engine parts with another timbre.
- Store timbre assignments in scenes.

</intent-contract>
