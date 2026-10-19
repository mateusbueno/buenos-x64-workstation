
## Deferred from: code review of spec-11-12 (2026-10-19)

- Install `THIRD_PARTY_NOTICES.md` in the image and show it in Velora Studio's About (Story 11.12 AC2, first Then). Belongs to the image-build and Studio About stories; they must carry a dependency on this file. Until then the obligation is unmet for any shipped binary.

## Founder decisions pending spec updates (2026-10-19)

- Story 3.8: align wording to 12-bit inputs (MCU-ID-MAP) and state pedal polarity is reported in HELLO.
- Story 4.19: adopt the Action Sheet and Part chrome drawn for Stories 5.18/12.10 (`mockups/action-sheet.html`, `part-surface-fm-va.html`) as the base; create `part-surface-organ.html` to match.
- DESIGN.md: toast copy is sentence case (stories win over the uppercase `status` token for toast text); the 'Apply one category to all' row is a checkbox toggle with On/Off text.
- Story 8.14 plan keeps start date 2027-06-01; re-baseline only after kernel.org confirms a later EOL for 6.18.
