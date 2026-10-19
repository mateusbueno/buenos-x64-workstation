**DRAFT FOR FOUNDER — does not satisfy Story 11.15 acceptance criteria**

# Story 11.15 — Typeface designer brief (draft)

Nothing here is a decision. Designer, fee, dates, and the choice between "custom" and "licensed existing face" are blank for the founder. Paths are relative to the repo root. `DESIGN.md` = `_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/DESIGN.md`. `epics.md` = `_bmad-output/planning-artifacts/epics.md`.

Gate: Story 11.1 requires a recorded go/no-go on the names before "any further branding spend (domains, handles, designer)" (epics.md:6150). Do not send this brief before that decision is recorded.

Founder fields: Designer/foundry: ____ · Route (custom / licensed): ____ · Fee: ____ · Delivery date: ____ · Brief signed on: ____

## 1. Product context for the designer

- Ladiani is the brand; Velora 73 is the first instrument (a stage keyboard with a 7" 1024x600 touchscreen and printed panel). Velora Studio is the desktop companion app (DESIGN.md:510-516, epics.md:6592).
- Direction: "humanist-technical sans", "British and industrial, like a precision instrument" (DESIGN.md:510). Gill Sans is a stand-in only (DESIGN.md:65-66, 569).
- One family is used across panel, device screen and desktop (DESIGN.md:569).

## 2. Where the face is used

| Surface | Use | Source |
|---|---|---|
| Device UI (LVGL 9.6.0, DRM/KMS, `ui` process) | All UI text, 1024x600 landscape, rasterised/subsetted glyphs built into firmware | architecture ARCHITECTURE-SPINE.md:437 (LVGL 9.6.0); epics.md:1867 Story 2.18 (fonts built with ascent measured from Á/Ç/Ñ/Ê, tabular numerals) |
| Velora Studio (Tauri 2.12 desktop, macOS/Windows/Linux) | Denser rem-based ramp, bundled font files | epics.md:5383-5395 Story 9.5; DESIGN.md:164-177 |
| Printed/engraved panel legends | Uppercase English legends, cap height >= 2.5 mm, tracking 0.17em | DESIGN.md:194-198 (`panel-legend`); DESIGN.md:3 of Hardware asks (legends) |
| Wordmarks | LADIANI (light, wide), VELORA (spaced, subordinate), boot wordmark | DESIGN.md:181-193 |
| Marketing | Covered by license, not by technical constraints | epics.md:6515 |

## 3. Technical constraints from the repo (device)

All device tokens are px at native 1024x600 and all use lineHeight >= 1.2 so Á Ç Ê Õ Ñ never clip (DESIGN.md:67-68).

| Token | Size px | DESIGN.md line (fontSize) | Notes |
|---|---|---|---|
| live-set-name | 53 | 71 | Largest; read at 1.5 m, >= 43 px (NFR33, epics.md:292), uppercase, 0.25em tracking |
| live-set-name-large | 66 | 76 | Large text on Performance (+25%) |
| next-title / -large | 22 / 28 | 81 / 85 | |
| modal-title | 27 | 89 | |
| organ-registration | 25 | 93 | Tabular numerals; e.g. `88 8600 000` (DESIGN.md:573) |
| organ-name | 23 | 98 | |
| search-input | 20 | 101 | |
| list-item | 19 | 104 | |
| drawbar-value | 19 | 107 | Tabular numerals; also fractions 1⅗' 1⅓' 2⅔' 5⅓' |
| part-name / -large | 17 / 21 | 111 / 115 | |
| tile-label / -large | 16 / 20 | 119 / 122 | |
| bar-label | 15 | 125 | |
| mode-mirror | 20, bold (700) | 130-131 | Needs a real bold >= 600/700 |
| status / -large | 14 / 18 | 137 / 142 | weight 600, uppercase, 0.18em; "minimum label size" |
| status-takeover / -large | 24 / 30 | 147 / 152 | >= 12 arcmin at 0.7 m (NFR33) |
| chip | 14 | 157 | |
| meta-caps | 12 | 160 | Smallest device size; must stay legible at 12 px |
| boot-wordmark | 25 | 192 | Tracking 0.5em |

Weights in use: 400 (most), 600 (status, DESIGN.md:178 desktop section label), 700 (mode-mirror), 300 only for `wordmark-brand` (DESIGN.md:183). Device UI text must be >= 400 (AC, epics.md:6510).

Desktop (Studio) ramp in rem: 1.75rem live-set-name (DESIGN.md:166), 0.875rem part-name (170), 0.8125rem body (173, min body 13 px), 0.6875rem section label weight 600 (177-178, min label 11 px) (DESIGN.md:578).

Glyph coverage required: full Latin-1 + Latin Extended-A (pt-BR and es; English default), with accented capitals (Á Ç Ê Õ Ñ) fitting a 1.2 line box (epics.md:6510). Add: ⅓ ⅔ ⅗ (and ⅓-style drawbar footages; confirm 5⅓' needs only ⅓), tabular numerals, ◆ ● ▶ ▲ ⚠ ⌕ ⏻ are UI glyphs currently used (DESIGN.md:322, 368, 451, 267, 631, 636). Question for founder: whether these symbols come from the typeface or from a separate icon set (not decided in repo). Test string: `ÁÇÊÕÑ Ação` (epics.md:6610).

Tracking is applied by the UI at the sizes above (0.06em to 0.5em); the face must hold up when uppercase and widely tracked.

Format and pipeline facts:
- The firmware build converts the licensed files into LVGL glyph subsets at every token size (Story 11.19 AC, epics.md:6606). There is no font build code in the repo yet; `tools/tokens/` is specified in Story 2.18 (epics.md:1873) but does not exist at this commit. Deliver outlines, not bitmaps.
- Memory: the font atlas must fit the "UI memory budget" and not slow redraws (epics.md:6612). No numeric budget is written in the repo; do not give the designer a number. Hinting/size-specific quality at 12-30 px matters because glyphs are rasterised.
- Studio bundles the font files with the license notice (epics.md:6606).

## 4. Design requirements (copied from the acceptance criteria)

Source: DESIGN.md:698 and epics.md:6510.
1. Distinct I / l / 1 and 0 / O.
2. x-height >= 0.5 em.
3. Open apertures.
4. Tabular numerals; glyphs ⅓ ⅔ ⅗.
5. Weights >= 400 for device UI text; 300 allowed only for the wordmark.
6. Full Latin-1 + Latin Extended-A with accented capitals (Á Ç Ê Õ Ñ) fitting a 1.2 line box.
Also from the direction: uppercase tracked labels must work; the same family serves panel legend, screen and desktop. Founder to decide whether the bold weight and a 600 weight are required (the tokens use 600 and 700; DESIGN.md:131, 138, 178).

## 5. Deliverable formats

- OTF and/or TTF for each weight, plus a variable font if provided (epics.md:6511). Stored with version numbers.
- Webfont (WOFF2) for Studio is not required by the AC; founder to decide whether to request it (Studio is a Tauri web UI, ARCHITECTURE-SPINE / Story 9.5). Desktop CSS is generated by the token generator (epics.md:1874-1875).
- Glyph coverage chart, kerning/hinting notes, and the font metrics (ascender, descender, line gap, x-height, cap height) so type tokens can be regenerated (Story 11.19, epics.md:6607).
- Wordmark: any custom LADIANI/VELORA lettering is delivered under Story 11.16, not here; agree with the logo designer if one person does both. Founder: same designer for 11.15 and 11.16? ____

## 6. Licensing terms checklist (from epics.md:6515-6516)

The written license must permit, for the term and territories stated by the founder:
- [ ] Embedding as rasterised/subsetted glyphs in LVGL firmware on every shipped unit, unlimited units, no per-unit royalty.
- [ ] Modification/subsetting/format conversion (OTF to LVGL bitmap or vector font) for embedding.
- [ ] Bundling in Velora Studio (macOS, Windows, Linux) and redistribution to end users, unlimited downloads.
- [ ] Use on the printed/engraved/silkscreened panel, including by the hardware partner's manufacturing (confirm the license lets the panel supplier receive the file).
- [ ] Use in marketing, website, packaging, manuals and the wordmark.
- [ ] Perpetual, irrevocable, transferable to successors/assignees of the brand; sublicensable to contractors and manufacturers.
- [ ] Updates: bug-fix and glyph additions policy; who may extend the character set.
- [ ] No cap on app/user count, no web-font page-view limits, no revenue-share.
- [ ] Open-source vs proprietary status stated; if OFL, the Reserved Font Name and notice requirements are recorded.
- [ ] Copyright/ownership: for a custom face, assignment in writing to the company (same standard as Story 11.16, epics.md:6538); for a licensed face, licensor and license text stored.
- [ ] Warranty that the design does not infringe third-party rights.
- [ ] If required, add the font to `THIRD_PARTY_NOTICES.md` (repo root; currently section 1 is sfizz) and to the notices shipped in the image and Studio's About (epics.md:6449, 6516).

## 7. Acceptance checklist mapped to the ACs (founder signs)

| AC (epics.md) | Check | Evidence | Signed |
|---|---|---|---|
| 6508-6510 | I/l/1 and 0/O distinct | specimen page | ____ |
| 6510 | x-height >= 0.5 em | metrics | ____ |
| 6510 | open apertures | specimen at 12, 14, 19 px | ____ |
| 6510 | tabular numerals and ⅓ ⅔ ⅗ | glyph chart | ____ |
| 6510 | weights >= 400 for device; 300 wordmark only | weight list | ____ |
| 6510 | Latin-1 + Latin Ext-A; Á Ç Ê Õ Ñ in 1.2 line box | render of `ÁÇÊÕÑ Ação` | ____ |
| 6511 | OTF/TTF (+ variable) stored with version numbers | file list | ____ |
| 6513-6515 | License reviewed: LVGL embedding, Studio bundling, panel, marketing, no royalty | signed license | ____ |
| 6516 | License stored; notices updated if required | path/pointer | ____ |

Storage: confidential contracts outside the repo with a pointer and status here (epics.md:6623 convention).
