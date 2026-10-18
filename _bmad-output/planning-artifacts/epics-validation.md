---
artifact: epics-validation
target: _bmad-output/planning-artifacts/epics.md
workflow: bmad-create-epics-and-stories, Step 4 (final validation), re-run after fixes
date: 2026-10-18
result: 5 of 5 checks pass (epics.md stays status: draft until the founder selects [C])
---

# Epics validation report: Ladiani Velora 73 (re-run)

Scope: 12 epics, 226 stories (was 221), FR1–FR122, NFR1–NFR52, AR1–AR47, UX-DR1–UX-DR114, EXT1–EXT15.
Method: a script parsed every story's **Covers:**, **Runs on:** and **Blocked by:** lines and every `Story X.Y` reference. It checked the template, FR coverage, sequential numbering, broken references, forward references inside an epic and references to higher epics. Then the ACs of every story touched by the fixes were read against the FRs they claim.

| # | Check | Result |
|---|-------|--------|
| 1 | FR coverage | **PASS** |
| 2 | Architecture implementation | **PASS** |
| 3 | Story quality | **PASS** (3 borderline-size stories noted) |
| 4 | Epic structure | **PASS** |
| 5 | Dependency validation | **PASS** (Epic 11 gates are explicit **Blocked by:** lines) |

Story counts: Epic 1: 23 · Epic 2: 21 · Epic 3: 20 · Epic 4: 38 · Epic 5: 21 · Epic 6: 17 · Epic 7: 9 · Epic 8: 14 · Epic 9: 22 · Epic 10: 8 · Epic 11: 19 · Epic 12: 14 · **Total 226**.

---

## What changed since the first run

| Finding | Fix applied |
|---------|-------------|
| 1-A, 1-B, 1-C | New **Story 10.2 "Settings › Keyboard & Pedals and MIDI & Clock rows"**: Velocity curve, Aftertouch (shown only when `aftertouch_available`), Footswitch (moved from 4.36), Sustain and Expression calibration flows, and the MIDI & Clock rows (Clock source from 7.7, MIDI out channel, Keybed to MIDI OUT, MIDI THRU). No Release velocity row (release velocity is always on, FR71). Covers FR70, FR72, FR115 and FR42 (partial). Story 3.17 now creates the MIDI `sysd` keys. Stories 3.6, 3.7 and 3.11 point to Story 10.2 instead of "Epic 10". |
| 1-D | Story 1.8 reserves engine type IDs for Sample, Organ, FM and VA, and returns `ENGINE_NOT_AVAILABLE` (never a substitute) for an engine that is not compiled in. Story 4.2 stores an engine type ID plus `param_id`-keyed parameters, and round-trips an FM/VA test part. Both list FR14 (partial). |
| 1-E | FR112 (Arpeggiator section) added to Story 7.5. |
| 2-A | Story 4.11 creates the minimal `librarian` service, its UDS Library API and the catalog. Story 5.6 extends it with the libraries and instruments tables, the import API and atomic publish. |
| 2-B | Story 4.2 defines the arpeggiator fields now and leaves them unset. Story 7.4 loads older files with the initial values, with no format bump and no migration. |
| 2-C | Story 2.18 owns the single token generator (LVGL output, colour-literal lint, CI contrast gate). Story 9.5 adds the CSS output and extends the lint and the gate to `desktop`. Story 10.3 only adds the Theme row, the guidance text, the drawbar edge rule and the screenshot set, with no second contrast check. |
| 2 (gap) | Story 1.3's directory list includes `tools/spikes/`. The Additional Requirements now say Story 1.3, not "Epic 1 Story 1", sets up the monorepo. |
| 3-A | Prototype bring-up moved from 3.12 to **3.2**, and old 3.2–3.11 became 3.3–3.12. Its MCU-link check is now a frame loopback with the Story 3.1 library, so it no longer needs MCU firmware. |
| 3-B | Epic 5 reordered: 5.10 Notice Toast design, 5.11 Category Picker design, 5.12 Import rows, 5.13 Import progress (builds the Notice Toast component), 5.14 "USB drive found". The toast now opens an Import surface that already exists. The Notice Toast design stays at 5.10, because 5.13 needs it for the import-complete notice. |
| 3-C | Story 5.16 creates the favorites table, and Story 5.19 writes to it. |
| 3-D | The read-only followed-tempo AC moved from 7.5 into 7.8 (with FR76). Story 7.5 uses "the internal clock (the default)". |
| 3-E | Story 9.3's unpaired allow-list is `Hello` only, kept as a table. It no longer references 9.6. |
| 3-F | Old 2.17 split into **2.17** UI shell (SDL + DRM/KMS, `ws.v1` client, crash resync, string lint), **2.18** tokens, themes and contrast gate, and **2.19** Hint Bar, List Row, Segmented Control and the Mac virtual panel. Old 2.18 and 2.19 became 2.20 and 2.21. |
| 3-G | Old 9.9 split into **9.9** Part Editor layout with Studio Knobs and reflow, **9.10** live-edit state (`STUDIO`, ◆, `STALE_REVISION`, disconnect mid-edit), and **9.11** "+ Add timbre". Old 9.10–9.20 became 9.12–9.22. |
| 4-A | Old 10.5 merged into 4.37: row values and defaults, first-boot defaults, automated simulator tests and FR115 rows. 10.5 was deleted, and old 10.2–10.4 became 10.3–10.5. |
| 4-B | Story 8.12 (old 8.14) uses the Story 5.6 reconcile rule and the Story 5.21 GC unchanged. It keeps only fault injection, the 1 TB ≤5 s bound and starting after the target is playable. Story 5.6 now says it recreates rows "for bundles under `current`". |
| 4-C | The conditionals in 5.21 and 8.9 (old 8.11) were removed. They now name the Story 4.35 surface and the Story 4.37 section. |
| 4-D, 5-A | Old 8.1 and 8.2 moved into Epic 6 as **6.11** (read-only rootfs) and **6.12** (RAUC A/B). The dev and factory-test half of old 8.4 became **6.13**. **6.14** (old 6.11) is the only owner of the production image build: it adds the production keyring, an empty `console=` and the external probe AC. What is left of old 8.4 became **8.2 "Production release signing and per-root update bundles"**: CI signing request, HSM signing and per-root rejection through `sysd`. Old 6.12–6.14 became 6.15–6.17, and old 8.3–8.16 (except 8.4) became 8.1, 8.3–8.14. The Epic 6 and Epic 8 intros, the Epic List and the FR66 map line were updated. |
| 4-E | The Epic List entry for Epic 10 now says it "finds every earlier row (incl. Boot into from Epic 8)". The Epic 10 intro lists the rows consolidated from Epics 4, 5, 7, 8 and 9. |
| 5-B | Every story that needs an Epic 11 deliverable now has a **Blocked by:** line, and the Epic 11 intro and Epic List say it runs in parallel from month 0. |
| 8.9 note | Story 8.7 (old 8.9) creates the `reduced_motion` `sysd` key (default Off). Story 10.6 exposes it. |
| Forward notes | 1.10 ("Stories 1.11 and later"), 3.3 ("Story 3.7") and 4.21 ("Story 4.37") were reworded, so no story names a later story of its own epic. |

---

## 1. FR coverage: PASS

**Mechanical:** FR1–FR122 each appear in at least one story's Covers line, with none missing. NFR49 and AR47 are still tracking-only, which is acceptable.

**AC fulfilment of the previously failing FRs:**
- **FR70:** Story 3.6 provides the curves, applied once. Story 10.2 adds the Velocity curve Segmented Control in Settings › Keyboard & Pedals. Met.
- **FR72:** Story 3.7 provides routing and `aftertouch_available`. Story 10.2 adds the Aftertouch row, which shows only when that flag is true, with a simulator test for both keybed profiles. Met.
- **FR115:** Display & Language comes from 4.37, 8.9, 10.3, 10.4 and 10.6 (Language from 10.7). Keyboard & Pedals and MIDI & Clock come from 10.2 (plus 4.36 and 7.7). Knobs · Global Assign comes from 4.35, Computer from 9.6, Storage & Library from 5.21, System & Update from 8.3. 10.1 sets the structure. Met.
- **FR14:** 1.8 (engine interface and type IDs) and 4.2 (format round-trip, part plays silent and is never substituted) cover v1. 12.8 covers the post-launch engines. Met.
- **FR112:** 4.33, 4.34 and 7.5 (Arpeggiator section). Met.

The FR Coverage Map was updated for FR14, FR66 (Epic 6 + Epic 8), FR70, FR72 (Epic 3 + Epic 10), FR112 (Epic 4 + Epic 7) and FR115.

## 2. Architecture implementation: PASS

- **Starter template:** none in the architecture. Story 1.3 builds the skeleton after the 1.1 and 1.2 go/no-go spikes and keeps `tools/spikes/`.
- **Incremental entities:** the librarian and catalog start in 4.11. Libraries and instruments are added in 5.6, search, recent and favorites in 5.16. The engine type IDs start in 1.8. The arpeggiator fields are defined in 4.2 and stay unset until 7.4. The MIDI keys come from 3.17, `reduced_motion` from 8.7, and the aftertouch and expression-calibration keys from 10.2. The rootfs layout and A/B slots are created in 6.11 and 6.12, just before the stories that use them.
- **Consistency:** one token generator (2.18, extended in 9.5). One librarian origin (4.11). One arpeggiator-field approach (reserved, no bump). One production-image owner (6.14).

## 3. Story quality: PASS

- All 226 stories have a user story, Covers, Runs on and Given/When/Then ACs, and they are numbered 1..n in each epic with no gaps.
- **Forward references inside an epic:** none. The script found no `Story X.Y` that points to a later story of the same epic.
- **Broken references:** none, including the epic intros, the Epic List and the coverage map.
- **Size:** 2.17 and 9.9 are split. These remain borderline but acceptable, because each is one cohesive concern: 4.37 (7 AC blocks after absorbing 10.5, all about one set of guard settings), 10.2 (7 AC blocks, two Settings sections that bind existing `sysd` keys), 6.16, 8.3, 9.18 and 12.1. If a dev agent finds 10.2 too large, split it into Keyboard & Pedals and MIDI & Clock.

## 4. Epic structure: PASS

- Every epic still states a keyboardist outcome. Epic 6 now also delivers the platform pieces that its anti-rollback and factory stories need (rootfs, A/B slots, dev and factory-test images). Epic 8 delivers the update and boot experience on top of them.
- **Duplication and contradictions resolved:** 10.5 is gone (merged into 4.37). 5.6 and 8.12 no longer contradict each other (8.12 adds no rule). 6.14 is the only story that builds the production image (8.2 handles only release signing). The stale conditionals in 5.21 and 8.9 are gone (7.7's conditional is still valid).
- **Known overlaps still justified:** power-fail 3.18 and 8.11, MCU reflash 3.4 and 8.4, and the Settings rows added per epic and then consolidated in 10.1 and 10.2.

## 5. Dependency validation: PASS

| Epic | Needs a higher epic? | Detail |
|------|----------------------|--------|
| 1 | Only the Epic 11 gate | 1.13 is **Blocked by** 11.12 (month 5). |
| 2 | Only Epic 11 gates | 2.13 is blocked by 11.13 (month 8), 2.14 by 11.14 (month 11). |
| 3 | Only Epic 11 gates | 3.2 by 11.7; 3.3 by 11.6 and 11.7; 3.4 by 11.7; 3.7 by 11.6; 3.8 and 3.9 by 11.7 and 11.8; 3.13, 3.18 and 3.20 by 11.9. Mentions of 8.4, 8.11 and 10.2 are "builds on it later" or "row added in" notes, not dependencies. |
| 4 | No | 4.19's mention of 5.18 is a note about later refinement. |
| 5 | Only the Epic 11 gate | 5.4 is blocked by 11.12. The Epic 8 mentions are "until then" notes. |
| 6 | **No longer needs Epic 8** | 6.15 uses the 6.12 A/B slots, 6.16 the 6.13 factory-test image and the 6.14 production image. Gates: 6.9 by 11.17 (month 12), 6.16 by 11.7. |
| 7 | No | — |
| 8 | No | It uses Epic 6 (6.11–6.14) and lower epics. 8.7 creates its own `reduced_motion` key. |
| 9–12 | No | — |

Epic 11 runs in parallel from month 0. Its intro lists each gate with its target month: 11.12 by month 5; 11.6 and 11.7 by month 7; 11.13 by month 8; 11.8 by month 9; 11.9 by month 10; 11.14 by month 11; 11.17 by month 12. Within each epic, every story builds only on earlier stories.

---

## Status

All five checks pass. epics.md keeps `status: draft` and `stepsCompleted: [1, 2, 3]` until the founder selects **[C] Complete Workflow**.
