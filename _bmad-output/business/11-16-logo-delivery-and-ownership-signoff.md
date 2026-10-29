# Story 11.16 — In-House Logo, Symbol, App Icon, Boot Artwork & Avatar Delivery and Sign-Off Record

- **Status:** Signed off (2026-10-29)
- **Production Route:** Authored in-house as deterministic geometry by the founder (`logo/build_logo.py` and `logo/make_assets.py`); no external designer commissioned.
- **Asset Root:** `_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/logo/`

---

## 1. Delivered Asset Manifest

| Deliverable | Vector Masters (SVG / PDF) | Raster / Platform Bundles | Spec & Verification |
|-------------|----------------------------|---------------------------|---------------------|
| **Symbol S1 (Full)** — 1 tall + 4 short drawbars (`viewBox 0 0 80 64`, `rx 4.5`) | `outlined/ladiani-symbol-ink.svg`, `ladiani-symbol-reversed.svg`, `pdf/ladiani-symbol-ink.pdf`, `ladiani-symbol-reversed.pdf` | `png/ladiani-symbol-*.png` | Verified on dark, light, and aluminum contexts |
| **Symbol S1 (Simplified 32 px & 16 px)** — 1 tall + 2 short at 32 px; 1 tall + 1 short at 16 px | `outlined/ladiani-symbol-32.svg`, `ladiani-symbol-16.svg`, `ladiani-favicon-32.svg`, `ladiani-favicon-16.svg`, `pdf/ladiani-symbol-32.pdf`, `ladiani-symbol-16.pdf` | `app-icon/linux/16x16.png`, `24x24.png`, `32x32.png` | Legible at 32 px and 16 px without bar merging |
| **LADIANI Wordmark (Direction T3)** — outlined geometry, light weight, wide tracking | `outlined/ladiani-wordmark-ink.svg`, `ladiani-wordmark-reversed.svg`, `pdf/ladiani-wordmark-ink.pdf`, `ladiani-wordmark-reversed.pdf` | `png/ladiani-wordmark-*.png` | Outlined pure paths; zero font dependency |
| **VELORA Model Mark (Direction V1)** — outlined uppercase spaced geometry | `outlined/velora-wordmark-ink.svg`, `velora-wordmark-reversed.svg`, `pdf/velora-wordmark-ink.pdf`, `velora-wordmark-reversed.pdf` | `png/velora-wordmark-*.png` | Subordinate to LADIANI; portfolio rule established |
| **Panel Lockup (`LADIANI` + hairline + `VELORA 73`)** | `outlined/ladiani-velora73-panel-ink.svg`, `ladiani-velora73-panel-reversed.svg`, `pdf/ladiani-velora73-panel-ink.pdf`, `ladiani-velora73-panel-reversed.pdf` | `png/ladiani-velora73-panel-*.png` | Single-colour `#2b322e` (`panel-ink`) and legend tokens for engraving/silkscreen on brushed aluminum |
| **Boot-Screen Artwork & Animation (`1024×600`)** | `boot/boot-static-1024x600.svg` | `boot/boot-static-1024x600.png` + 72 staggered pull-out frames in `boot/frames/frame_000.png`..`frame_071.png` | Integrated into LVGL Boot screen (Stories 8.7 & 11.19); static frame used when reduced motion is active |
| **Desktop App Icons (Velora Studio)** | `outlined/ladiani-app-icon.svg`, `pdf/ladiani-app-icon.pdf` | `app-icon/ladiani.icns` (macOS), `app-icon/ladiani.ico` (Windows), `app-icon/linux/` (16, 24, 32, 48, 64, 128, 256, 512 px) | Integrated into `desktop/src-tauri/icons/` (Story 11.19) |
| **Social Avatars (Round Crop, Story 11.2 handles)** | `outlined/ladiani-avatar-dark.svg`, `ladiani-avatar-paper.svg`, `pdf/ladiani-avatar-dark.pdf`, `ladiani-avatar-paper.pdf` | `png/ladiani-avatar-*.png`, `youtube/` channel assets | Verified in circular mask crop for `@ladianimusic` / `@ladiani.instruments` |

---

## 2. Context Verification Sign-Off (`logo/verification/logo-contexts-sheet.png`)

Reviewed against `_bmad-output/planning-artifacts/ux-designs/ux-buenos-x64-workstation-2026-10-18/logo/verification/logo-contexts-sheet.png`:
- [x] **Engraved / silkscreened single-colour on aluminum:** Panel lockup and symbol maintain clean bar separation and hairline legibility on brushed aluminum.
- [x] **Dark 7" boot screen (`1024×600`):** Centred symbol + `VELORA` wordmark and 72-frame drawbar pull-out sequence render crisply on `#0d0f0e`.
- [x] **Tiny desktop icon (32 px & 16 px):** Simplified 3-bar (32 px) and 2-bar (16 px) symbols read clearly without blurring.
- [x] **Round social avatar:** Symbol centred in circular crop maintains balanced optical margins on dark and paper backgrounds.

---

## 3. Authorship, Ownership & INPI Figurative-Mark Filing Decision

1. **Authorship & Company Ownership Statement:**
   - All symbol, wordmark (`LADIANI`), model mark (`VELORA`), panel lockup, app icon, boot artwork, and avatar drawings were authored directly in-house by the founder, Mateus Bueno, using `logo/build_logo.py` and `logo/make_assets.py`.
   - No outside designer, contractor, stock asset, or third-party trademark was used (compliant with NFR29).
   - Full copyright and intellectual property rights in all delivered artwork vest directly in the company (Ladiani) from creation; no third-party assignment instrument is required (to be noted for the trademark attorney of Story 11.1).
2. **INPI Figurative / Mixed Mark Filing Decision (Story 11.1 link):**
   - **Decision recorded:** File the S1 drawbar "L" symbol + `LADIANI` lockup as a figurative/mixed mark at INPI in classes 15 (musical instruments) and 9 (audio/music software) alongside the nominative filings of Story 11.1, using `pdf/ladiani-symbol-ink.pdf` and `pdf/ladiani-lockup-ink.pdf` as the specimen attachments.

---

## 4. Founder Sign-off

- **Approved by:** Mateus Bueno (Founder, Ladiani)
- **Date:** 2026-10-29
