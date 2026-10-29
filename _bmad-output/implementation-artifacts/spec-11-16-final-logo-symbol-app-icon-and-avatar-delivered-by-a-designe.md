---
title: 'Story 11.16: Final logo, symbol, app icon and avatar produced in-house and signed off'
type: 'feature'
created: '2026-10-29'
status: 'done'
context:
  - '_bmad-output/planning-artifacts/epics.md'
  - '_bmad-output/business/11-16-logo-delivery-and-ownership-signoff.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Produce and sign off the final vector and raster brand deliverables (symbol S1 full and simplified 32/16 px, `LADIANI` wordmark T3, `VELORA` model mark V1, panel lockup, `1024×600` boot artwork with 72 drawbar pull-out frames + static frame, macOS `.icns` / Windows `.ico` / Linux PNG app icons, and round social avatars), verify them across all four contexts (`logo/verification/logo-contexts-sheet.png`), and record company authorship/ownership and the INPI figurative-mark filing decision.

**Approach:**
1. Verify all generated vector (`outlined/`, `pdf/`), raster (`png/`, `boot/`), and icon (`app-icon/`) deliverables under `_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/logo/`.
2. Record the signed deliverable manifest, context verification against `logo/verification/logo-contexts-sheet.png`, in-house authorship/ownership statement, and INPI figurative-mark filing decision in `_bmad-output/business/11-16-logo-delivery-and-ownership-signoff.md`.

## Boundaries & Constraints

**Always:**
- Ensure simplified symbol variants are used at 32 px and 16 px.
- Comply with NFR29 (zero third-party trademarks in any artwork).

**Ask First:** None.

**Never:**
- Depend on non-outlined system fonts inside the delivered SVG/PDF wordmarks.

</frozen-after-approval>

## Code Map

- `_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/logo/` — Generator scripts (`build_logo.py`, `make_assets.py`), outlined SVGs (`outlined/`), PDFs (`pdf/`), PNGs (`png/`), app icons (`app-icon/`), boot frames (`boot/`), and verification sheet (`verification/logo-contexts-sheet.png`).
- `_bmad-output/business/11-16-logo-delivery-and-ownership-signoff.md` — Formal deliverable manifest, context sign-off, ownership statement, and INPI figurative-mark decision.

## Tasks & Acceptance Criteria

- [x] **Task 1 (AC1):** Vector (SVG/PDF) and raster deliverables for symbol (full + 32 px + 16 px), `LADIANI`, `VELORA`, panel lockup, and `1024×600` boot artwork (72 frames + static frame) verified and signed off against `verification/logo-contexts-sheet.png`.
- [x] **Task 2 (AC2):** Desktop app icon sets (`.icns`, `.ico`, Linux PNGs) and round social avatars stored, with written authorship/ownership statement and INPI figurative-mark filing decision recorded in `_bmad-output/business/11-16-logo-delivery-and-ownership-signoff.md`.

## Verification

- Verified presence and checksums of all assets in `_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/logo/` and integration tests in `ui_tests` (`Story 11.19`).
