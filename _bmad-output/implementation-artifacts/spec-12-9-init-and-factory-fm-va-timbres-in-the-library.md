---
title: 'Story 12.9: Init and factory FM/VA timbres in the Library'
type: 'feature'
created: '2026-10-22'
status: 'done'
baseline_revision: 'b265139'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/formatlib/include/velora/format/parametric_bundle.hpp'
  - '{project-root}/formatlib/src/parametric_bundle.cpp'
  - '{project-root}/engine/host/include/velora/host/library_resolver.hpp'
  - '{project-root}/engine/host/src/library_resolver.cpp'
  - '{project-root}/librarian/include/velora/librarian/catalog.hpp'
  - '{project-root}/librarian/include/velora/librarian/library_records.hpp'
  - '{project-root}/librarian/src/library_index.cpp'
  - '{project-root}/librarian/src/catalog.cpp'
  - '{project-root}/ui/include/velora/ui/screens/library_view.hpp'
  - '{project-root}/ui/src/screens/library_view.cpp'
warnings: []
deferred: []
---

<intent-contract>

## Intent

**Problem:** Keyboardists need FM and VA timbres to appear in the Library under standard categories with a starting factory pack, while "Init FM" and "Init VA" timbres must always be available directly from the engine even without an installed content pack. In addition, UI presentation must show categories (never engine types, per FR58), and indexed factory parametric timbres must maintain stable `(library_id, instrument_key)` identity across re-installation (FR23 semantics).

**Acceptance Criteria:**
1. **Built-in Init FM and Init VA Timbres (FR14, FR56, FR58):**
   - An "Init FM" and an "Init VA" timbre are always available directly in the engine/host under category `Synth Poly` (14).
   - Usable without any library content pack installed.
   - Displayed with their category (short label on Part Strips e.g. `SYNTH POLY`, full label elsewhere), never showing the engine type (`FR58`).
   - Stable built-in library ID `00000000-0000-0000-0000-000000000001` and keys `"init-fm"` and `"init-va"`.
2. **Founder-Authored Factory FM/VA Pack (≥50 timbres, FR34, FR46, FR59):**
   - 52 curated factory timbres across Electric Pianos, Mallet & Bells, Basses, Brass, Strings, Leads, Pads, and Polys across the 17 categories.
   - Zero third-party trademarks in timbre names.
   - Shipped as a vendor library bundle (`bundle.vlb` / `catalog.meta` / `bundle.vlrp`) with revision tracking.
   - Installs separately from firmware bundle (FR46).
   - Timbres appear in Library search, category chips, Favorites, and Recent.
   - Audition via key-play and WRITE assignment to parts work identical to sample timbres (FR59).
3. **Parametric Bundle VA Support & Stable Indexing (Story 12.3, FR23):**
   - `ParametricBundle` supports `VaVoiceParameters` binary serialization and deserialization (`kEngineTypeVa = 4`).
   - Catalog indexes parametric timbres with stable `(library_id, instrument_key)`.
   - Live Sets using factory FM/VA timbres resolve reliably across pack re-installation.

## Boundaries & Constraints
- Never display engine types ("FM" or "VA") in place of category labels in UI timbre lists or part strips (FR58).
- Built-in init timbres must not depend on filesystem storage or content packs.
- Zero third-party trademarks in timbre naming.

</intent-contract>
