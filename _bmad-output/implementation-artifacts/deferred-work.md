
## Deferred from: code review of spec-11-12 (2026-10-19)

- Install `THIRD_PARTY_NOTICES.md` in the image and show it in Velora Studio's About (Story 11.12 AC2, first Then). Dependency recorded in Story 11.12 text; still needs an image-build task and a Studio About screen. Until then the obligation is unmet for any shipped binary.

## Founder decisions (2026-10-19) — applied

- Story 3.8 wording (12-bit, polarity in HELLO), toast sentence case and Apply-all toggle are written into epics.md, DESIGN.md and EXPERIENCE.md. Story 4.19 mockups (Action Sheet incl. Favorites variants, ASSIGN Overlay) are built on the 5.18/12.10 base. Story 8.14 keeps 2027-06-01.
- Fixed: action-sheet.html keycap borders (`.key`, `.foot b`) now use `--kc` (Stage `#6b7c73`, 3.49:1 on raised; Studio unchanged at 3.74:1). DESIGN.md `keycap-border` token is still `line-strong-dark` (2.63:1 on raised): update it when the token is next revised.


## Deferred from: Story 4.1 (tier profile), 2026-10-26

### RESOLVED (2026-10-29): Arpeggiator stuck-voice bug (`engine/core/src/arpeggiator.cpp`)

- **Fixed in `Arpeggiator::set_config` and `Arpeggiator::remove_unheld_notes`:**
  1. When `set_config` disables the arpeggiator non-seamlessly (`!seamless_transition`) while step notes are still sounding from an earlier seamless scene switch (`!config_.enabled && has_active_sounding()`), it now arms `transition_start_passthrough_ = true` so `apply_mode_transition` emits immediate Note Offs for the lingering step notes.
  2. `remove_unheld_notes()` preserves entries that still have `passthrough_sounding == true` until `apply_mode_transition` emits their Note Off, preventing a passthrough voice from being orphaned if Latch/Sustain held the entry across an Arp On/Off toggle before `apply_mode_transition` ran.
- **Verified:** `arpeggiator_traffic_test` now runs both `8` and `params::kPartCount` (`16`) parts across seeds `54321`, `11111`, and `424242` with zero stuck voices, plus dedicated unit regression tests in `engine/core/tests/arpeggiator_test.cpp`.

### Other items left by Story 4.1

- Per-snapshot `active_parts` bound: the RT loops (mixer, part router, encoder ALL mode, per-part arps and event buffers) iterate the 16 structural parts whatever the tier cap, so v1 pays for 16. Bound them by the generation's part count.
- `--tier pro|v1` must not exist in production builds: gate it as dev-only (like the dev key options) in the board's `main`, which reads the strap through its `TierSource`.
- Recalibrate on the board, per tier: the organ (voice capacity, generator cost for 16 parts), FM and VA voice capacities, the arbiter budget, the FX budget (Pro is about 1.75x v1: 2240 vs 1280 units), the head budgets and the floors. All Pro values are 2x placeholders.
- Pro bank A/B, the 16-part UI part strip, the board strap / OTP reader, the sequencer module (see the spec).
