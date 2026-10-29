# Story 11.17 — Factory Library Content Plan, Rights Records & Source Delivery Handoff

- **Status:** Signed off (2026-10-29)
- **Owner:** Mateus Bueno (Founder, Ladiani)
- **Covers:** EXT14, FR34 (source content), NFR29 (trademark-clean naming), NFR3 (storage budget)

---

## 1. Content Plan Table (All 17 Categories)

All instrument and preset names strictly comply with NFR29 (generic musical terminology only: e.g., `"Tonewheel Organ"`, `"Rotary Speaker"`, `"Tine Electric Piano"`, `"6-Op FM Bell"` — zero third-party trademarks). Size estimates are stated on a raw lossless WAV (48 kHz / 24-bit) basis prior to lossless FLAC bundle compression (`formatlib` reduces on-disk bundle size to ~0.55× raw WAV).

| # | Category | Instrument / Preset Pack | Source | Size Estimate (GB, raw WAV) | Target Delivery Month | Owner | Rights Status |
|---|----------|--------------------------|--------|-----------------------------|-----------------------|-------|---------------|
| 1 | Piano | **Velora Factory Grand** (88-key concert grand, 6 velocity layers + release + pedal resonance) | own recording | 12.00 | Month 10 | Mateus Bueno | 100% Ladiani own recording |
| 1b | Piano | **Velora Studio Upright** (intimate felt & studio upright) | own recording | 4.50 | Month 11 | Mateus Bueno | 100% Ladiani own recording |
| 2 | E.Piano | **Velora Tine & Reed Electric Pianos** (sampled tine/reed + FM/VA E.Pianos) | own sampling + own programming | 3.00 | Month 11 | Mateus Bueno | 100% Ladiani own work |
| 3 | Organ | **Velora Tonewheel & Rotary Organ Presets** (24 parametric drawbar/percussion/rotary registrations + **Velora Factory Pipes** SFZ) | own programming + own sampling | 1.50 | Month 10 | Mateus Bueno | 100% Ladiani own work |
| 4 | Keys | **Velora Factory Keys** (clavinet-style funky keys, harpsichord, combo organ) | own sampling | 1.50 | Month 11 | Mateus Bueno | 100% Ladiani own sampling |
| 5 | Mallet & Bells | **Velora Vibraphone, Marimba & FM Bells** (sampled acoustic mallets + 6-op FM bells) | own recording + own programming | 1.80 | Month 11 | Mateus Bueno | 100% Ladiani own work |
| 6 | Strings | **Velora Factory Strings** (chamber & symphonic ensemble sustain/spiccato + VA string machine) | own recording + own programming | 4.00 | Month 11 | Mateus Bueno | 100% Ladiani own work |
| 7 | Brass | **Velora Stage Brass & Synth Brass** (section swells/stabs + VA/FM poly brass) | own recording + own programming | 2.20 | Month 11 | Mateus Bueno | 100% Ladiani own work |
| 8 | Woodwind | **Velora Solo Flute & Reeds** (concert flute, alto sax, oboe) | own recording | 1.40 | Month 12 | Mateus Bueno | 100% Ladiani own recording |
| 9 | Choir & Vocal | **Velora Chamber Choir & Vowel Pads** (aah/ooh ensemble + hybrid vocal pad) | own recording + own programming | 1.80 | Month 12 | Mateus Bueno | 100% Ladiani own work |
| 10 | Guitar | **Velora Nylon, Steel & Clean Stage Guitars** | own recording | 1.50 | Month 12 | Mateus Bueno | 100% Ladiani own recording |
| 11 | Bass | **Velora Acoustic Upright, Fingered Electric & FM/VA Synth Bass** | own recording + own programming | 1.30 | Month 11 | Mateus Bueno | 100% Ladiani own work |
| 12 | Synth Lead | **Velora VA & FM Stage Leads** (mono/legato analog-style & FM leads, Story 12.9) | own programming | 0.01 | Month 10 (Delivered) | Mateus Bueno | 100% Ladiani own programming |
| 13 | Synth Pad | **Velora Warm Analog & Evolving Pads** (VA/FM + hybrid layered pads) | own programming + own sampling | 0.50 | Month 10 | Mateus Bueno | 100% Ladiani own work |
| 14 | Synth Poly | **Velora Init FM, Init VA & Poly Synths** (52-timbre Factory FM/VA pack, Story 12.9) | own programming | 0.01 | Month 10 (Delivered) | Mateus Bueno | 100% Ladiani own programming |
| 15 | Drums & Perc | **Velora Stage Kit & Brazilian Percussion** (studio kit, cajón, pandeiro, congas, shakers) | own recording | 1.00 | Month 12 | Mateus Bueno | 100% Ladiani own recording |
| 16 | World | **Velora Accordion & Regional Plucked Strings** (sanfona/accordion, viola caipira, cavaquinho) | own recording | 0.40 | Month 12 | Mateus Bueno | 100% Ladiani own recording |
| 17 | FX & Ambient | **Velora Stage Risers, Textures & Ambient Beds** | own programming + own sampling | 0.09 | Month 12 | Mateus Bueno | 100% Ladiani own work |

- **Total Planned Factory Content (`F_total`, raw WAV basis):** **38.51 GB**

---

## 2. Minimum Shippable Set (MSS — Ready by Month 12, First Playable Units)

Required by AC: at least the factory piano and the organ presets ready by month 12 (`first playable units`).

| Item | Category | Source | Size (GB) | Delivery Month (`<= 12`) | Owner | Rights Status | Ready / Pipeline Verified |
|------|----------|--------|-----------|--------------------------|-------|---------------|---------------------------|
| **Velora Factory Grand** (`piano/Factory_Grand.sfz`) | Piano | own recording | 12.00 | Month 10 | Mateus Bueno | 100% Ladiani own recording | ☑ Pipeline verified (`Story 6.9`) |
| **Velora Tonewheel Organ Presets (24 registrations) + Factory Pipes** (`organ-samples/Factory_Pipe.sfz`) | Organ | own programming + own sampling | 1.50 | Month 10 | Mateus Bueno | 100% Ladiani own work | ☑ Pipeline verified (`Story 6.9`) |
| **Velora Factory Keys & E.Pianos** (`keys/Factory_Keys.sf2`) | E.Piano / Keys | own sampling | 4.50 | Month 11 | Mateus Bueno | 100% Ladiani own sampling | ☑ Pipeline verified (`Story 6.9`) |
| **Velora Factory Strings & Pads** (`strings/Factory_Strings.sfz`) | Strings / Synth Pad | own recording + own sampling | 4.00 | Month 11 | Mateus Bueno | 100% Ladiani own work | ☑ Pipeline verified (`Story 6.9`) |
| **Velora Factory FM & VA Collection (52 presets + Init FM / Init VA)** | E.Piano, Mallet & Bells, Brass, Bass, Synth Lead, Synth Pad, Synth Poly | own programming | 0.01 | Month 10 | Mateus Bueno | 100% Ladiani own programming | ☑ Delivered in repo (`Story 12.9`) |

- **MSS Total Size (`F_mss`):** **22.01 GB** (raw WAV basis; ~12.1 GB packed lossless `.vlb`).
- **Contingency if any non-MSS category slips past Month 12:** Ship first playable units with the 22.01 GB Minimum Shippable Set plus the 52 parametric FM/VA presets covering the remaining categories, and deliver additional acoustic packs as signed `.vlb` content updates (FR46).

---

## 3. Storage Budget Verification

```text
C_nvme            = 1000.00 GB   (v1 sealed internal NVMe capacity)
C_data            =  960.00 GB   (journaled /data partition after boot + two 16 GB RAUC A/B rootfs slots)
F_total           =   38.51 GB   (sum of all 17 categories in Section 1, raw WAV worst-case basis)
F_mss             =   22.01 GB   (sum of Minimum Shippable Set in Section 2)
H_user            =  921.49 GB   (C_data - F_total = 960.00 - 38.51 GB user-import headroom)
H_user_pct        =   95.99 %    (H_user / C_data * 100)
Check 1:          H_user (921.49 GB) >= H_required (600.00 GB for 300 GB SFZ import per Story 5.6 AC) -> PASS
Check 2:          F_total (38.51 GB) <= C_data - H_required (360.00 GB)                               -> PASS (☑ yes)
Overhead factor   =    0.58      (formatlib lossless FLAC block coding + catalog/index headers)
F_total_on_device =   22.34 GB   (38.51 * 0.58 GB actual on-device footprint)
```

---

## 4. Per-Item Delivery Checklist & Rights Record

Every delivered source library satisfies:
- [x] **Format:** SFZ (`<global>`, `<master>`, `<group>`, `<region>`, `<control>` subset supported by `formatlib`) + lossless WAV (48 kHz / 24-bit PCM, zero resampling, zero lossy compression), or native parametric bundle (`Story 12.9`).
- [x] **Paths & Integrity:** All sample paths resolve relative to the `.sfz` file and verify cleanly through `velora-content-pack` and `velora-factory-release`.
- [x] **Categories:** Every instrument is assigned to one of the 17 canonical categories (`proto/library_v1.proto`).
- [x] **Zero Third-Party Trademarks (NFR29):** Verified across all library, instrument, file, folder, and Live Set names.

### Master Rights Record (Ladiani Own Work Declaration)

| Field | Entry |
|-------|-------|
| **Libraries Covered** | All 17 category packs in Section 1 & Minimum Shippable Set in Section 2 |
| **Source Type** | 100% Ladiani own recording, own sampling, and own synthesis programming |
| **Studio / Location** | Ladiani Studio, São Paulo, SP, Brazil |
| **Instrument Owner & Permission** | Acoustic pianos, electro-mechanical keys, winds, strings, guitars, basses, and Brazilian percussion recorded under written commercial sampling permission held by Ladiani; FM/VA/Organ presets programmed from scratch on the Velora engine |
| **Performer Consent** | Performed by Mateus Bueno and contracted session musicians under written work-for-hire / commercial buyout agreements |
| **Third-Party Samples / Loops** | **None.** Zero third-party sample libraries, loops, or converted commercial patches used ☑ |
| **Third-Party Trademarks** | **None.** All names use generic musical terminology compliant with NFR29 ☑ |
| **Copyright Holder** | Ladiani Instrumentos Musicais Ltda. (assigned in full by founder Mateus Bueno) |
| **Signed By / Date** | Mateus Bueno — 2026-10-29 |

---

## 5. Handoff to Epic 6 Content-Pack & Factory-Release Pipeline (`Story 6.3` / `Story 6.9`)

The following vendor `library_id` UUIDs (registered in `tools/content-pack/vendor-ids`) and factory Live Set `program_id`s are permanently reserved for the v1 factory release (`tools/factory-release`):

| Library / Preset Pack | Category | Reserved Vendor `library_id` | Reserved Factory `program_id`(s) | Source Entry | Rights Record |
|-----------------------|----------|------------------------------|----------------------------------|--------------|---------------|
| `Velora Factory Piano` | `piano` | `3f1d6a52-9b7e-4c1a-8d2e-5a6b7c8d9e0f` | `1` (`Factory Grand`), `3` (`Piano and Strings`) | `piano/Factory_Grand.sfz` | Section 4 Master Record |
| `Velora Factory Pipes` | `organ` | `6c8a2d14-5e3b-4f90-9a1c-7b8d2e4f1a02` | `2` (`Gospel Organ`), `4` (`Split Keys`), `5..16` (Organ registrations) | `organ-samples/Factory_Pipe.sfz` + Organ Engine | Section 4 Master Record |
| `Velora Factory Strings` | `strings` | `a19f3e72-4c8b-4d19-8e22-3d5b7f9a1c44` | `3` (`Piano and Strings`), `17..24` | `strings/Factory_Strings.sfz` | Section 4 Master Record |
| `Velora Factory Keys` | `keys` / `epiano` | `8b4e1c90-2d7a-4e55-b103-9c4f6a8d2e11` | `4` (`Split Keys`), `25..36` | `keys/Factory_Keys.sf2` | Section 4 Master Record |
| `Velora Factory FM & VA` | `synth_poly` (and 7 categories) | `d4e8b210-6f3c-4a91-9e05-2b7c8d1f3a60` | `37..88` (52 curated FM/VA timbres, Story 12.9) | Built-in / `bundle.vlrp` | Section 4 Master Record |

- **Handoff Accepted By:** Mateus Bueno (Owner of `tools/content-pack` & `tools/factory-release`) — **Date:** 2026-10-29
