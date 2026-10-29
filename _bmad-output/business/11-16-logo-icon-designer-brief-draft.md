**SUPERSEDED 2026-10-29 — Story 11.16 is produced in-house; no designer is commissioned. Kept for reference only.**

**DRAFT FOR FOUNDER — does not satisfy Story 11.16 acceptance criteria**

# Story 11.16 — Logo, symbol, app icon and avatar designer brief (draft)

Nothing here is a decision. Designer, fee, dates and the final drawings are blank for the founder. `DESIGN.md` = `_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/DESIGN.md`. `epics.md` = `_bmad-output/planning-artifacts/epics.md`. Mockup reference: `.../mockups/logo-drawbar-refine.html`.

Founder fields: Designer: ____ · Same as typeface designer (11.15)? ____ · Fee: ____ · Delivery date: ____ · Brief signed on: ____

## 0. Trademark dependency (Story 11.1)

- Story 11.1 requires a clearance opinion for "Ladiani", "Velora", "Ladiani Velora" and "Velora Studio" (classes 15 and 9) and a recorded go/no-go "before any further branding spend (domains, handles, designer)" (epics.md:6150-6152). Do not commission or send this brief until that go decision is recorded. If a mark is risky or blocked the fallback may change the names being drawn.
- Story 11.16 itself requires the trademark result of 11.1 as an input (epics.md:6530) and recording "the decision to file the figurative mark at INPI (Story 11.1)" (epics.md:6538). Story 11.1 says the figurative-mark decision is recorded "once a final logo exists" (epics.md:6159). Order: 11.1 go -> 11.16 delivery -> figurative-mark filing decision.
- Handles for the avatars come from Story 11.2: `@ladianimusic` and `@ladiani.instruments`, Instagram and YouTube (TikTok, Facebook, WhatsApp Business decision pending) (epics.md:6182-6184). If a handle is lost the replacement may be recorded; the avatar itself carries no handle text unless the founder decides.
- NFR29: no third-party trademarks anywhere in marketing, UI or panel (epics.md:282). The designer must not use any other brand's name or lettering in any artwork.
- Founder fields: 11.1 opinion date ____ · go/no-go ____ · figurative mark filing ____

## 1. Chosen direction (not open for redesign)

From DESIGN.md:510-516 and the mockup (S1/T3/V1 chosen; other options in the mockup are rejected):
- Symbol S1: an "L" built from organ drawbars: one tall bar and four short bars, rounded ends. Reference geometry: mockup line 48 (`viewBox 0 0 80 64`, tall bar 9x56 at x=8, four 9x16 short bars at y=44, rx 4.5).
- Simplified symbol: 1 tall + 2 short bars at 32 px (mockup line 49), 1 tall + 1 short at 16 px (DESIGN.md:510).
- Wordmark LADIANI, direction T3: humanist-technical sans, light weight (300), wide tracking, uppercase ("British and industrial, like a precision instrument"). Stand-in tokens: `wordmark-brand` letterSpacing 0.2em (DESIGN.md:181-185).
- Model mark VELORA, direction V1: uppercase, spaced, same family, always subordinate to LADIANI; stand-in letterSpacing 0.42em (DESIGN.md:186-189). Portfolio rule: every future model name is set this way (DESIGN.md:512).
- Panel lockup: symbol + LADIANI, a hairline, then "VELORA 73" below and smaller (DESIGN.md:510; mockup line 107).
- Boot artwork: centred symbol and VELORA on the dark background, drawbars pull out one by one (each ~0.7 s, ease-out, staggered ~0.1-0.25 s), then VELORA fades in; reduced motion shows the final static frame (DESIGN.md:514, epics.md Story 8.7 at 5057-5081). Boot wordmark token: 25 px, 0.5em tracking (DESIGN.md:190-193). Mockup animation reference: lines 30-34, 115-116.
- The final symbol, wordmark and model mark are custom-drawn from these directions; system fonts are stand-ins (DESIGN.md:513, 685).

## 2. Colours and contexts

- Logo master: monochrome `logo-ink #111111` on `logo-paper #f4f2ee`; on screens `text-dark` on `bg-dark` (DESIGN.md:61-63, 544). Theme-invariant tokens (DESIGN.md:38).
- Panel: ink `panel-ink #2b322e` on aluminum gradient (`alu-high` -> `alu-mid` -> `alu-low`), legends `panel-legend` (DESIGN.md:52, 543). These are render references, not production finish specs; engraving vs silkscreen and aluminum finish are decided by the hardware partners (DESIGN.md:699). Founder: confirm that dependency before the designer finalises the lockup.
- Bar ends use `rounded.full` (DESIGN.md:617).
- Must work: engraved or silkscreened single colour on metal, on the dark 7" boot screen, as a tiny desktop icon, as a round social avatar (DESIGN.md:510, AC epics.md:6533).

## 3. Deliverables and sizes

| Deliverable | Format | Size/spec from repo | Source |
|---|---|---|---|
| Symbol, full | SVG + PDF (vector) | one tall + four short bars | epics.md:6532 |
| Symbol, simplified | SVG + PDF | 32 px (1 tall + 2 short) and 16 px (1 tall + 1 short) | epics.md:6532, DESIGN.md:510 |
| LADIANI wordmark | SVG + PDF; outlined | light 300, wide tracking | epics.md:6532 |
| VELORA model mark | SVG + PDF; outlined | uppercase spaced | epics.md:6532 |
| Panel lockup | SVG + PDF | in panel ink and legend colours; single-colour version for engraving | epics.md:6532 |
| Boot-screen artwork | SVG/PDF master + animation frames (PNG sequence unless founder prefers otherwise) | 1024x600; drawbar pull-out frames; plus final static frame | epics.md:6532 |
| macOS app icon | `.icns` with all standard sizes | "macOS .icns sizes" (epics.md:6537) | list not in repo |
| Windows icon | `.ico` multi-size | "Windows .ico sizes" | list not in repo |
| Linux icons | PNG set | "Linux PNG sizes" | list not in repo |
| Social avatars, round crop | PNG (+SVG master) per handle | for Story 11.2 handles | epics.md:6537 |
| Assignment of copyright | signed document | all delivered artwork, in writing | epics.md:6538 |

Sizes not in the repo (founder or designer to specify; not invented here): exact icns/ico/Linux PNG size lists, avatar pixel dimensions per platform, animation frame count and rate. The Studio app is Tauri 2.12 (epics.md:5389); Tauri's tooling consumes a source PNG/set to produce icons, so ask the designer for a large square master (suggest >= 1024x1024; this is a suggestion, confirm in Story 9.5). The `desktop/` directory does not exist yet at this commit.

Boot timing constraints: the animation must not delay the switch to Performance (epics.md:5071-5072); Story 8.10's boot budget is <= 10 s power-on to first note (epics.md:5141). Animation frames are shipped on the device, so ask for a frame count and file weight the founder can approve (no budget exists in the repo).

## 4. Licensing and rights

- [ ] Copyright in all artwork assigned to the company in writing (epics.md:6538); moral-rights waiver as far as Brazilian law allows (attorney to confirm).
- [ ] Original work, no third-party stock or fonts embedded without a license; any typeface used in the drawings is outlined or comes from Story 11.15.
- [ ] Designer's warranty of non-infringement and prior-art/similarity check; the logo must not resemble existing marks (attorney to review before the figurative filing).
- [ ] Source (AI/SVG) files delivered, not only exports.
- [ ] Right for the company to modify the artwork.
- [ ] Portfolio rights for future model marks (VELORA is the first, DESIGN.md:512): who draws the next ones is for the founder.

## 5. Acceptance checklist mapped to the ACs

| AC (epics.md) | Check | Evidence | Signed |
|---|---|---|---|
| 6530-6532 | Vector SVG/PDF for symbol, LADIANI, VELORA, panel lockup | file list | ____ |
| 6532 | Simplified symbol reads at 32 px and 16 px | test render | ____ |
| 6532 | Panel lockup in panel ink and legend colours | render on alu gradient | ____ |
| 6532 | Boot artwork + drawbar pull-out frames at 1024x600 | frame set | ____ |
| 6533 | Works engraved/silkscreened on aluminum | proof from hardware partner or mock | ____ |
| 6533 | Works on the dark boot screen | render | ____ |
| 6533 | Works as tiny icon | 32/16 px render | ____ |
| 6533 | Works as round avatar | circle-crop render | ____ |
| 6537 | .icns, .ico, Linux PNG icon sets stored | file list | ____ |
| 6537 | Round avatars for Story 11.2 handles stored | file list | ____ |
| 6538 | Copyright assignment signed | pointer | ____ |
| 6538 | INPI figurative-mark filing decision recorded | decision note | ____ |

Storage: confidential contracts outside the repo, pointer and status here.
