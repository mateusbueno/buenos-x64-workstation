---
title: 'Decision: SFZ scope for the reference corpus'
type: 'chore'
created: '2026-10-27'
status: 'done'
baseline_revision: '62b97ef0f6043f9a365b8cca873fee0c3dc660c4'
review_loop_iteration: 0
followup_review_recommended: false
context: ['{project-root}/_bmad-output/implementation-artifacts/spec-5-4-reference-corpus-imports-and-plays-with-mapping-intact.md', '{project-root}/tools/corpus/README.md', '{project-root}/_bmad-output/planning-artifacts/epics.md']
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** FR18 (deliberate v1 SFZ subset) and FR20 (every corpus library imports) contradict each other; Epic 13 needs a written scope decision before 13.2-13.9 start.

**Approach:** Write a decision record that marks each opcode tier in scope, deferred or out of scope with a reason, based on the Story 5.4 findings (per-library rejected opcodes and region counts, engine capability, effort, risk). The unattended run records the epic's own ordering as the **proposed** decision (tiers of 13.2-13.8 and 13.10 in scope; the 13.9 tier conditional on measured need after 13.8) and flags it for founder ratification. Amend FR18 and the epic list to match.

## Boundaries & Constraints

**Always:** every claim cites a Story 5.4 finding, the corpus manifest or README; each of the nine tiers (importer rules, note-on scalars, trigger and release modes, CC at voice start, choke groups, pitch bend, per-voice filter, region EQ, LFO/EG modulation) gets exactly one verdict plus reason; out-of-scope files keep `unsupported_features` skip reporting (FR22) and are listed in the manifest with the same reason; the Epic 5 corpus requirement is restated as "imports and plays with mapping intact for every file in a supported tier".

**Never:** change importer or engine code; claim founder approval; hide the unattended-proposal status; edit corpus audio or tolerances.

</intent-contract>

## Code Map

- `_bmad-output/implementation-artifacts/spec-5-4-reference-corpus-imports-and-plays-with-mapping-intact.md` -- findings and rejected opcodes per library.
- `tools/corpus/README.md`, `tools/corpus/manifest.json` -- vocabulary and exclusions.
- `_bmad-output/planning-artifacts/epics.md` -- FR18 (line ~72), FR20, Epic 13 (line ~7035).
- `formatlib/src/sfz_import.cpp` -- reject list for opcodes (read-only evidence).

## Tasks & Acceptance

**Execution:**
- `_bmad-output/planning-artifacts/sfz-scope-decision.md` -- create decision record with tier table, per-library opcode evidence, effort/risk, status "proposed, awaiting founder ratification" -- the deliverable.
- `_bmad-output/planning-artifacts/epics.md` -- amend FR18 text and Epic 13 list/ordering to name the final subset; restate the FR20 corpus requirement -- keeps requirements consistent.
- `tools/corpus/README.md` -- document the rule that manifest exclusions list only out-of-scope tiers -- corpus vocabulary.

**Acceptance Criteria:**
- Given the decision record, when read, then all nine tiers carry a verdict and a reason.
- Given an out-of-scope tier, when a library needs it, then the record states it is skipped with `unsupported_features` naming the opcodes and listed in the manifest with the same reason.
- Given epics.md, when FR18/FR20 are read, then they agree with the record.

## Verification

**Commands:**
- `python3 tools/corpus/corpus.py check` -- expected: manifest check passes (no manifest regression)

## Review Triage Log

### 2026-10-27 — Review pass
- verdicts: 0 findings — documentation-only diff read directly by the orchestrator; multi-reviewer layers skipped (no code changed).
- note: decision is a proposal for founder ratification, stated in the record, FR18 and README.

## Auto Run Result

Status: done. Added `_bmad-output/planning-artifacts/sfz-scope-decision.md`; amended FR18, FR20 and the Epic 13 intro in `epics.md`; added the `skip_files` scope rule to `tools/corpus/README.md`. `corpus.py check` passes. Residual risk: per-library "importable after" mapping follows Epic 13's claims, not a measurement; ratification pending.
