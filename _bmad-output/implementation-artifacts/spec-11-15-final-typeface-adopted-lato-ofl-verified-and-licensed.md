---
title: 'Story 11.15: Final typeface adopted: Lato (SIL OFL), verified and licensed for embedding'
type: 'feature'
created: '2026-10-29'
status: 'done'
context:
  - '_bmad-output/planning-artifacts/epics.md'
  - '_bmad-output/business/11-15-lato-verification-and-license-signoff.md'
  - 'THIRD_PARTY_NOTICES.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Verify and sign off Lato 400/700 (SIL Open Font License 1.1) against all UX typography criteria in `DESIGN.md › Deferred` and record the legal licensing conclusion for LVGL firmware embedding, Velora Studio desktop bundling, panel legends, and marketing, including the OFL Reserved Font Name clause.

**Approach:**
1. Verify `ui/assets/fonts/lato/Lato-Regular.ttf`, `Lato-Bold.ttf`, and `OFL.txt` checksums, metrics (`x-height = 0.506 em`, accented capital height `1.10 em <= 1.20` line box, tabular numerals `0.580 em`, fractions `⅓ ⅔ ⅗`, distinct `I/l/1` and `0/O`), and record the signed checklist in `_bmad-output/business/11-15-lato-verification-and-license-signoff.md`.
2. Record the SIL OFL 1.1 licensing and Reserved Font Name conclusion in `_bmad-output/business/11-15-lato-verification-and-license-signoff.md` and `THIRD_PARTY_NOTICES.md` §9.

## Boundaries & Constraints

**Always:**
- Retain `ui/assets/fonts/lato/OFL.txt` and `THIRD_PARTY_NOTICES.md` §9 in shipped distributions.

**Ask First:** None.

**Never:**
- Distribute a modified `.ttf`/`.otf` font file under the Reserved Font Name "Lato".

</frozen-after-approval>

## Code Map

- `ui/assets/fonts/lato/Lato-Regular.ttf`, `ui/assets/fonts/lato/Lato-Bold.ttf`, `ui/assets/fonts/lato/OFL.txt` — Stored Lato 2.015 font files and license text.
- `THIRD_PARTY_NOTICES.md` (§9) — Third-party notice for Lato 2.015 under SIL OFL 1.1.
- `_bmad-output/business/11-15-lato-verification-and-license-signoff.md` — Signed metrics checklist and SIL OFL 1.1 / Reserved Font Name conclusion.

## Tasks & Acceptance Criteria

- [x] **Task 1 (AC1):** Verify Lato 400/700 files, checksums, and measured metrics (`x-height = 0.506 em`, accented capitals `1.10 em`, tabular figures, `⅓ ⅔ ⅗`, distinct `I/l/1` and `0/O`, open apertures) in `_bmad-output/business/11-15-lato-verification-and-license-signoff.md`.
- [x] **Task 2 (AC2):** Record the written SIL OFL 1.1 licensing and Reserved Font Name conclusion in `_bmad-output/business/11-15-lato-verification-and-license-signoff.md` and `THIRD_PARTY_NOTICES.md` §9.

## Verification

- `ctest --test-dir build --output-on-failure -R "tokens_verify|third_party_notices"`
