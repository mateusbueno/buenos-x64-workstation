# Story 4.38 show run-through checklist (first playable unit)

Sources: story `_bmad-output/planning-artifacts/epics.md:3509-3534`; Flow 4 and Flow 1 in
`_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/EXPERIENCE.md:509-520` (Flow 4) and `:471-483` (Flow 1).
Record per step: PASS / FAIL, issue id if filed, notes. Any FAIL needs an issue filed (AC3). Thresholds: only those in the story; everything else TODO.

## Preconditions (AC1/AC2 "Given")
- [ ] Prototype with keybed, panel, codec outputs; NO computer connected
- [ ] Epic 1 demo libraries loaded (protected factory library is Epic 6, not required)
- [ ] Setlist of >= 6 Live Sets incl. one organ song (epics.md:3524) - TODO: setlist name/contents (story gives none; Flow 1 uses "Wedding Ana & Pedro")
- [ ] Engine counters (xruns, underruns) reset/read at start: ____ / ____ (FR120, epics.md:228). TODO: how to read on the unit (dev interface) not specified in the story
- [ ] Disk-write monitor armed (needed for "no disk write except the Write"). TODO: method not specified

## Part A - Flow 4: build "Goodness of God" (EXPERIENCE.md:511-518). "Every step works and sounds as it goes."
| # | Step | Sounds as you go? | P/F | Issue |
|---|------|---|---|---|
| A1 | MENU > Setlist; in "Sunday Service" **+ New Live Set**; type "Goodness of God" on On-screen Keyboard; WRITE. Performance shows new Live Set, 8 empty parts, one scene | n/a | | |
| A2 | Long-press part 1 > Part > Change timbre > Library: Piano chip, "Concert Grand", WRITE; EXIT to Part 1; Zone & keyboard: Low key C3 | yes | | |
| A3 | Part 2 > Change timbre > Synth Pad "Warm Pad", full range (layer over piano) | yes | | |
| A4 | Part 3 > Change timbre > Bass "Upright Bass", High key B2 (split under piano) | yes | | |
| A5 | MENU > Effects > Sends: reverb Hall, part 1 send 25%, part 2 send 40%; Part inserts: compressor on part 3 | yes (reverb audible, compressor active) | | |
| A6 | Part 2 surface: Arpeggiator Up, 1/8, 2 octaves, Latch off; Top Bar shows `♩ 72`; set Live Set tempo in same section | yes | | |
| A7 | Long-press Scene Tile 1 > Scenes: rename "Verse"; new scene slot 2 "Chorus"; in Chorus turn part 2 on, push fader 2 up | yes | | |
| A8 | Hold WRITE 0.5 s > **Whole Live Set** > WRITE - Yes | n/a | | |
| A9 | "Goodness of God" sits in setlist with `● READY` when its turn comes (EXPERIENCE.md:518) | n/a | | |
Failure-path spot checks (optional, from Flow 4): EXIT never discards edits (stay with ◆); storage full -> "Not enough space to write"; missing library -> Missing library on part.

## Part B - Flow 1: play the setlist (EXPERIENCE.md:473-481), no note cut on any switch
| # | Step | P/F | Issue |
|---|------|---|---|
| B1 | Power on; Boot into target playable (Flow text: under 10 s; NOT an AC of 4.38 - informational, TODO threshold owner) | | |
| B2 | MENU > Setlist, pick the setlist, WRITE to load; current sound keeps playing | | |
| B3 | Song 3 on Performance: Live Set name, scene 2 "Verse" filled, NEXT shows `● READY` | | |
| B4 | Footswitch tap: scene 3 recalls, strings come in; no note cut. Fader pickup: ghost + `▲ FADER 3 → 65%` shown, push up, take over | | |
| B5 | DRAWBARS: Organ Card and drawbars appear; pull 2 2/3' past stored 6 to take over; ROTARY S/F to fast (organ song) | | |
| B6 | End of song: LIVE SET ▶; next Live Set was preloaded; last chord rings out; no note cut | | |
| B7 | Off-script: long-press setlist context in Top Bar (Safe touch) > Setlist Browser, type "thou", pick "Thou Art Worthy", jump | | |
| B8 | Switch does not cut sound (sustained chord carries over); LIVE SET ◀ ▶ returns to song 4 | | |
| B9 | Edit part 2 volume (◆ appears), hold WRITE 0.5 s, choose Scene 2 "Verse", WRITE - Yes (the only permitted disk write) | | |
Note: Flow 1 names "Wedding Ana & Pedro" / "Thou Art Worthy"; the Epic 1 demo setlist must contain equivalents or the step names are substituted (record substitutions). TODO: confirm.

## Part C - Pass criteria (epics.md:3527) - evaluate over the whole Flow 1 run
- [ ] No note cut on any switch (scene, Live Set, jump, return)
- [ ] Next Live Set shows `● READY` before each in-setlist change
- [ ] Engine counters: xruns = 0 and underruns = 0 (end value, minus start value)
- [ ] No disk write except the Write in B9

## Part D - Report (AC3, epics.md:3531)
- [ ] Each flow step above listed pass/fail; every FAIL has an issue filed (id in table)
- [ ] Milestone result stated: NFR50 month-12 first playable unit (samples, organ, effects on prototype hardware): PASS / FAIL
- Unit/board rev: ____  Image: ____  Date: ____  Keyboardist: ____  Observer: ____
