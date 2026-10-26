
## Deferred from: code review of spec-11-12 (2026-10-19)

- Install `THIRD_PARTY_NOTICES.md` in the image and show it in Velora Studio's About (Story 11.12 AC2, first Then). Dependency recorded in Story 11.12 text; still needs an image-build task and a Studio About screen. Until then the obligation is unmet for any shipped binary.

## Founder decisions (2026-10-19) — applied

- Story 3.8 wording (12-bit, polarity in HELLO), toast sentence case and Apply-all toggle are written into epics.md, DESIGN.md and EXPERIENCE.md. Story 4.19 mockups (Action Sheet incl. Favorites variants, ASSIGN Overlay) are built on the 5.18/12.10 base. Story 8.14 keeps 2027-06-01.
- Fixed: action-sheet.html keycap borders (`.key`, `.foot b`) now use `--kc` (Stage `#6b7c73`, 3.49:1 on raised; Studio unchanged at 3.74:1). DESIGN.md `keycap-border` token is still `line-strong-dark` (2.63:1 on raised): update it when the token is next revised.


## Deferred from: Story 4.1 (tier profile), 2026-10-26

### BUG: Arpeggiator leaves a voice stuck after the final release (production code, found by arpeggiator_traffic_test)

- **Symptom.** After the traffic stops, every held key is released, every arpeggiator is disabled with latch off and the sustain pedal is lifted on every part, yet `br->test_tone().active_voices()` stays at 1 ("FAIL: 1 test-tone voices still active after release").
- **Repro.** `engine/core/tests/rtsan_arpeggiator_test.cpp` (ctest `arpeggiator_traffic_test`) with other random streams. At the pre-4.1 baseline (8 parts) seeds 11111 and 424242 reproduce it, and so does merely changing how many random numbers `Control::publish_variant` consumes. Harnesses: `.../scratchpad/arp_s11111.cpp` and `arp_s424242.cpp` (session scratchpad of the 4.1 work). The committed test keeps its tuned stream and `kTrafficParts = 8`, which hide the bug.
- **Suspect area.** `Arpeggiator::set_config`, `is_held_non_passthrough` and `track_passthrough_note`, and the note-off suppression in `engine/core/src/block_renderer.cpp` around lines 453-460 and `engine/core/src/arpeggiator.cpp` around line 119: a note-off swallowed because the arpeggiator still believed the key was arp-held, after a config change (enabled to disabled, or a latch change) moved the note's ownership.
- **Not fixed in 4.1** (out of scope; the change is independent of the tier). Needs its own story with a regression test per seed.

### Other items left by Story 4.1

- Per-snapshot `active_parts` bound: the RT loops (mixer, part router, encoder ALL mode, per-part arps and event buffers) iterate the 16 structural parts whatever the tier cap, so v1 pays for 16. Bound them by the generation's part count.
- `--tier pro|v1` must not exist in production builds: gate it as dev-only (like the dev key options) in the board's `main`, which reads the strap through its `TierSource`.
- Recalibrate on the board, per tier: the organ (voice capacity, generator cost for 16 parts), FM and VA voice capacities, the arbiter budget, the FX budget (Pro is about 1.75x v1: 2240 vs 1280 units), the head budgets and the floors. All Pro values are 2x placeholders.
- Pro bank A/B, the 16-part UI part strip, the board strap / OTP reader, the sequencer module (see the spec).
