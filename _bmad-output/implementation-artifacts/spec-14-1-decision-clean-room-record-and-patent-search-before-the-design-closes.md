---
title: 'Story 14.1: Decision: clean-room record and patent search before the design closes'
type: 'chore'
created: '2026-10-30'
status: 'done'
baseline_revision: '81cbee6'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/specs/spec-arm64-stage-workstation/ip-and-references.md'
  - '{project-root}/_bmad-output/specs/spec-arm64-stage-workstation/engines.md'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Before engineering closes the design and begins writing physical modeling DSP for the modeled electric piano (tine and reed), the project requires a verified clean-room record and a thorough patent search on file. Without this on record, the engine risks incorporating encumbered IP or competing implementations that cannot be shipped.

**Approach:**
1. Author `engine/core/engines/ep/CLEAN-ROOM.md` documenting clean-room provenance: no disassembly, no reverse engineering, no competitor context, and enumerate the theoretical literature (cantilever beam vibration, Euler-Bernoulli beam theory, electromagnetic reluctance pickup models, electrostatic capacitive variable-distance pickup models, non-linear hammer impact Hertzian contact).
2. Verify Freesound development reference recordings for both tine and reed, confirming individual sound licenses (CC BY 4.0 / CC0), recording exact attributions, and dropping any unverified or non-commercial items.
3. Update `_bmad-output/specs/spec-arm64-stage-workstation/ip-and-references.md` with targeted classification (G10H 1/00, G10H 7/00, G10H 3/18) and holder searches for physical modeling of electric pianos, explicitly listing findings and potential risks for attorney review prior to Epic 14 release gating.

## Boundaries & Constraints

**Always:**
- Keep the clean-room record in `engine/core/engines/ep/` versioned with the engine code.
- Explicitly certify that no competing products or copyleft/GPL code were inspected or given as context.
- Verify license status for every referenced sound file.
- Document limitations of preliminary patent searches clearly for legal counsel.

**Never:**
- Use CC BY-NC or unlicensed audio even for development without express written permission.
- Claim a non-legal preliminary search constitutes a formal legal freedom-to-operate opinion.
- Vendor or ship reference audio files in firmware or factory content bundles.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Clean-Room Documentation | Engineering inspection of `engine/core/engines/ep/` | `CLEAN-ROOM.md` exists, defines physical beam & pickup math, and disclaims disassembly/GPL | Fail review if missing or incomplete |
| Freesound Attribution | Selection of reference sounds from tim.kahn and OldBassMan | Individual sounds checked for CC BY / CC0 and attributed with URL and author | Exclude any sound lacking explicit CC BY / CC0 license |
| Patent Search Scope | Search across G10H classification and key holders (Yamaha, Roland, Korg, Rhodes, Wurlitzer) | Added to `ip-and-references.md` with active vs expired status and notes for attorney | Note unpublished 18-month window and jurisdiction limits |

</intent-contract>

## Code Map

- `engine/core/engines/ep/CLEAN-ROOM.md` -- Clean-room declaration, physical modeling math foundations (Euler-Bernoulli, electromagnetic/electrostatic pickups), and provenance.
- `_bmad-output/specs/spec-arm64-stage-workstation/ip-and-references.md` -- Verified reference audio table, Freesound license audit, and expanded patent classification/holder search.

## Tasks & Acceptance

**Execution:**
- `engine/core/engines/ep/CLEAN-ROOM.md` -- create clean-room statement and theoretical foundations for EP physical modeling -- satisfies NFR56 and Story 14.1 AC 1.
- `_bmad-output/specs/spec-arm64-stage-workstation/ip-and-references.md` -- expand reference audio verification and add electric piano modeling patent search -- satisfies Story 14.1 AC 2 and AC 3.

**Acceptance Criteria:**
- Given `ip-and-references.md`, when Story 14.1 closes, then `engine/core/engines/ep/CLEAN-ROOM.md` lists the sources used (published literature, own measurements), states no competing product was disassembled, reverse engineered or given to implementing agents, and is kept with the code.
- Given Freesound references, each selected sound has its license checked and recorded with attribution, and any sound without CC BY or public domain is dropped.
- Given patent search, search by holder and classification for EP modeling is added to `ip-and-references.md`, and any patent that may read on the model is listed for attorney review.

## Spec Change Log

<!-- Empty until first bad_spec loopback -->

## Review Triage Log

### 2026-10-30 — Review pass
- verdicts: 9 findings — high 0, medium 2, low 6, false 0, maybe-false 1
- findings:
  - `[medium]` `[patch]` Non-linear pickup displacement singularity avoidance — Patched: Added explicit numerical safety constraints ($d_{\text{eff}} = \max(d_0 - y, d_{\min})$ and hyperbolic tangent soft-clipping) to `CLEAN-ROOM.md`.
  - `[medium]` `[patch]` Singularity in transducer equations on large excursions — Patched: Addressed in `CLEAN-ROOM.md` with explicit bounding conditions.
  - `[low]` `[patch]` Freesound pack URL references rather than consecutive ID assumptions — Patched: Added explicit pack URLs (5012 and 5370) to `ip-and-references.md`.
  - `[low]` `[patch]` Patent table missing explicit grant dates and publication years — Patched: Added grant and expiration years to table in `ip-and-references.md`.
  - `[low]` `[patch]` Missing target keybed frequency range in beam equations — Patched: Added 73-key range E1 (41.2 Hz) to E7 (2637 Hz) in `CLEAN-ROOM.md`.
  - `[low]` `[patch]` Downstream attribution requirement for Freesound references — Patched: Specified attribution requirement for offline reports in `ip-and-references.md`.
  - `[low]` `[patch]` 18-month patent secrecy window note for worldwide applications — Patched: Added worldwide secrecy advisory in `ip-and-references.md`.
  - `[low]` `[patch]` Clean-room text missing mention of "own measurements" — Patched: Added internal acoustic/physical measurements to Section 1.2 of `CLEAN-ROOM.md`.
  - `[maybe-false]` `[reject]` Automated CI test enforcement for legal documentation existence — Rejected: Story 14.1 is explicitly "Runs on: Founder and attorney (no code)". Automated code tests begin in Story 14.2.

## Auto Run Result

Status: done
Summary: Completed Story 14.1 clean-room record, Freesound license verification audit, and electric piano physical modeling patent search.
Files changed:
- `engine/core/engines/ep/CLEAN-ROOM.md`: Clean-room declaration for modeled electric piano (NFR56, NFR57), grounding in published acoustics literature and numerical safety constraints.
- `_bmad-output/specs/spec-arm64-stage-workstation/ip-and-references.md`: Audited Freesound reference audio (CC BY 4.0), dropped jRhodes3d (CC BY-NC), and cataloged patent search across G10H classifications and holders for legal counsel.
- `_bmad-output/implementation-artifacts/spec-14-1-decision-clean-room-record-and-patent-search-before-the-design-closes.md`: Chore specification, review triage log, and auto run result.
Review findings breakdown:
- 8 patches applied (numerical safety bounding, pack URLs, grant dates, keybed range, own measurements, attribution notes).
- 1 rejected low/maybe-false finding (CI script for governance documents).
Follow-up review recommendation: false (no high findings; work fully converged on first review pass).
Verification performed:
- `test -f engine/core/engines/ep/CLEAN-ROOM.md`: passed.
- `grep -q "Zero GPL" engine/core/engines/ep/CLEAN-ROOM.md`: passed.
- `grep -q "Electric piano physical modeling" _bmad-output/specs/spec-arm64-stage-workstation/ip-and-references.md`: passed.
Residual risks:
- Formal patent clearance remains contingent upon legal counsel's freedom-to-operate opinion prior to release gating (Story 14.13).

## Design Notes

Physical modeling for electric pianos breaks down into four coupled stages:
1. Hammer strike: non-linear force pulse $F_h(t) \propto k_h [y_h(t) - y_b(t)]^p$ (Hertzian contact, $p \approx 1.5 - 2.5$).
2. Cantilever beam / tine: Euler-Bernoulli beam with fixed-free boundary conditions, yielding inharmonic partial ratios $\approx 1.0, 6.27, 17.55, 34.39$ for an ideal uniform cantilever, modified by tone bar / tuning spring mass distribution.
3. Pickup transducer:
   - Tine: variable reluctance electromagnetic pickup. Non-linear flux change $\Delta \Phi \propto \frac{1}{(d_0 - y(t))^2}$ producing strong even and odd harmonics depending on pickup tip alignment.
   - Reed: electrostatic variable capacitance pickup with DC polarizing voltage $V_0$. Current $i(t) \propto \frac{d}{dt}[C(t) V_0]$ where $C(t) \approx \frac{\epsilon A}{d_0 - y(t)}$.
4. Damper: felt damper contact introducing high damping ratio $\zeta$ upon note release, with release velocity modulating the decay rate and damper release transient noise.

## Verification

**Commands:**
- `test -f engine/core/engines/ep/CLEAN-ROOM.md` -- expected: file exists
- `grep -q "Zero GPL Code" engine/core/engines/ep/CLEAN-ROOM.md` -- expected: returncode 0
- `grep -q "Electric piano physical modeling" _bmad-output/specs/spec-arm64-stage-workstation/ip-and-references.md` -- expected: returncode 0

**Manual checks (if no CLI):**
- Verify Freesound license terms and patent search entries in `ip-and-references.md`.
