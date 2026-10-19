**DRAFT TEMPLATE FOR FOUNDER — does not satisfy Story 11.17 acceptance criteria**

# Story 11.17 — Factory library content plan (template)

Nothing below is a decision. Every `TODO` / `____` is for the founder to fill. No instrument list, size, date or rights fact has been invented. Status of this file: all fields blank.

Sources: `_bmad-output/planning-artifacts/epics.md` Story 11.17 (line 6540), FR34 (line 91), NFR29 (line 282), NFR3 (line 244); Story 6.3 content-pack tool (line 4180); Story 6.9 factory packaging (line 4348); `formatlib/README.md`.

## 1. Content plan table (17 categories)

Category names exactly as in epics.md line 3665. Source values allowed: own recording / own sampling / own programming. Size = on disk (after lossless bundle packing is not assumed; state which basis you use: TODO). Delivery month = project month, with first playable units at month 12.

| # | Category | Instrument / preset | Source | Size estimate (GB) | Target delivery month | Owner | Rights status |
|---|----------|---------------------|--------|--------------------|-----------------------|-------|---------------|
| 1 | Piano | TODO | TODO | ____ | ____ | ____ | TODO |
| 2 | E.Piano | TODO | TODO | ____ | ____ | ____ | TODO |
| 3 | Organ | TODO | TODO | ____ | ____ | ____ | TODO |
| 4 | Keys | TODO | TODO | ____ | ____ | ____ | TODO |
| 5 | Mallet & Bells | TODO | TODO | ____ | ____ | ____ | TODO |
| 6 | Strings | TODO | TODO | ____ | ____ | ____ | TODO |
| 7 | Brass | TODO | TODO | ____ | ____ | ____ | TODO |
| 8 | Woodwind | TODO | TODO | ____ | ____ | ____ | TODO |
| 9 | Choir & Vocal | TODO | TODO | ____ | ____ | ____ | TODO |
| 10 | Guitar | TODO | TODO | ____ | ____ | ____ | TODO |
| 11 | Bass | TODO | TODO | ____ | ____ | ____ | TODO |
| 12 | Synth Lead | TODO | TODO | ____ | ____ | ____ | TODO |
| 13 | Synth Pad | TODO | TODO | ____ | ____ | ____ | TODO |
| 14 | Synth Poly | TODO | TODO | ____ | ____ | ____ | TODO |
| 15 | Drums & Perc | TODO | TODO | ____ | ____ | ____ | TODO |
| 16 | World | TODO | TODO | ____ | ____ | ____ | TODO |
| 17 | FX & Ambient | TODO | TODO | ____ | ____ | ____ | TODO |

Add rows if a category has several instruments (repeat the category name). Use "none planned for v1" explicitly if a category ships empty (TODO: founder decision; AC says each planned item is listed, it does not say every category must be filled).

Note for the organ row: organ presets in the v1 success signal may be engine presets (own programming, referencing engine parameters) rather than samples; see Story 6.9 (line 4348). TODO: founder to state which.

## 2. Minimum Shippable Set (ready by month 12, first playable units)

Required by AC: at least the factory piano and the organ presets.

| Item | Category | Source | Size (GB) | Delivery month (must be <= 12) | Owner | Rights status | Ready? |
|------|----------|--------|-----------|-------------------------------|-------|---------------|--------|
| Factory piano: TODO name | Piano | TODO | ____ | ____ | ____ | TODO | ☐ |
| Organ presets: TODO names / count | Organ | TODO | ____ | ____ | ____ | TODO | ☐ |
| Other items the v1 success signal needs: TODO | TODO | TODO | ____ | ____ | ____ | TODO | ☐ |

MSS total size = ____ GB (sum of the rows above).
Contingency if month 12 slips: TODO.

## 3. Storage budget

Repo figures (cite as is):
- v1 NVMe: up to 1 TB, sealed inside the unit — `_bmad-output/specs/spec-arm64-stage-workstation/hardware-platform.md:68`.
- Indexed library on NVMe: up to 1 TB — `_bmad-output/specs/spec-arm64-stage-workstation/performance-targets.md:7`; also `epics.md:244` (NFR3) and `SPEC.md:32`.
- The repo gives an upper bound on indexed library size, not the installed NVMe capacity or the partition split. TODO: founder/hardware owner to confirm the actual fitted NVMe capacity and the data-partition size (journaled data partition exists, `hardware-platform.md:69`; its size is not stated in the files read).
- Import free-space check example in UI copy ("Needs 18.4 GB, 9.2 GB free"), `epics.md:3878`: shows imports are blocked when free space is short, so headroom matters.
- Single-user-import reference scale: 300 GB SFZ library on external SSD, `SPEC.md:71`; AC in `epics.md:3937` expects >= 600 GB free on the internal NVMe for a >= 300 GB import.

Formulas (fill blanks):

```
C_nvme        = ____ GB          (TODO: confirmed NVMe capacity; repo upper bound 1000 GB)
C_data        = ____ GB          (TODO: data partition after OS/slots; not stated in repo)
F_total       = sum(size_i over all rows in section 1) = ____ GB
F_mss         = sum(size_i over section 2)             = ____ GB
H_user        = C_data - F_total                        = ____ GB   (user-import headroom)
H_user_pct    = H_user / C_data * 100                   = ____ %
Check 1:      H_user >= H_required ?   H_required = ____ GB (TODO: founder target; e.g. repo AC at epics.md:3937 expects >= 600 GB free to import 300 GB)
Check 2:      F_total <= C_data - H_required ?  ☐ yes ☐ no
Overhead:     bundle/index overhead factor = ____ (TODO: measure with formatlib; lossless coding changes size vs raw WAV)
F_total_on_device = F_total * overhead factor = ____ GB
```

Size basis used for section 1 (raw WAV / FLAC-coded bundle): TODO.

## 4. Per-item delivery checklist

Copy once per item delivered. Item: ____________  Category: ____________  Date: ______

- [ ] SFZ file delivered; uses only the supported subset (headers `<global> <master> <group> <region> <control>`; opcodes listed in `formatlib/README.md` line 18). Unsupported opcodes reject the whole file.
- [ ] Audio is WAV, lossless, original rate/depth kept (16/24-bit PCM or 32-bit float; mono/stereo; 44.1/48/88.2/96 kHz). No resampling, no requantization, no lossy codec (`formatlib/README.md` line 10).
- [ ] Every sample referenced by the SFZ is present; paths resolve relative to the SFZ.
- [ ] Instrument name set and is generic.
- [ ] Exactly one of the 17 categories assigned (name as in section 1).
- [ ] No third-party trademark in any instrument, file, folder, preset or library name (NFR29, epics.md line 282: e.g. "rotary speaker", never "Leslie"; no "Hammond", no "DX7").
- [ ] Rights record (section 5) completed and filed with the item.
- [ ] Size on disk recorded in section 1.
- [ ] Imported cleanly with formatlib / round-trip verified (TODO: command and result).

## 5. Rights record template (one per item or per recording session)

| Field | Entry |
|-------|-------|
| Item / library name | TODO |
| Category | TODO |
| Source type (own recording / own sampling / own programming) | TODO |
| Recording or sampling date(s) | TODO |
| Location / studio | TODO |
| Instrument(s) recorded, make/model | TODO |
| Instrument owner | TODO |
| Owner's written permission to record and commercially use the samples (file / date) | TODO ☐ attached |
| Performer(s) and consent (if not the founder) | TODO |
| Statement: contains no third-party samples, loops, or presets | TODO (founder confirms) ☐ |
| Statement: names contain no third-party trademarks | TODO (founder confirms) ☐ |
| Programming items: confirm built from scratch, not converted from third-party patches | TODO |
| Copyright holder | TODO (company / founder; TODO assignment document if founder) |
| Signed by / date | TODO |

Legal review of these statements: TODO (not assessed here).

## 6. Handoff note to Epic 6 content-pack tool owner (Story 6.3 / Story 6.9)

To: owner of `tools/content-pack` (Story 6.3, epics.md line 4180) and the factory release pipeline (Story 6.9, line 4348).
From: TODO founder.   Date: ____

Per Story 11.17 AC, delivered source content is handed over with vendor-assigned library and program IDs reserved. Request:

1. Reserve vendor `library_id` values for each factory library listed in section 1, and vendor program IDs for each factory Live Set/preset (including the organ presets), before packing. IDs are assigned by the company, not derived from content (Story 6.3 takes a vendor `library_id` and per-instrument categories in a pack description file; Story 6.9 consumes a release description listing vendor `library_id`s, categories and names; `formatlib/README.md` line 12 says `library_id` = vendor-assigned or UUIDv5).
2. Once reserved, IDs must stay stable across rebuilds so existing Live Sets keep pointing at them (`epics.md` FR23, line 77).
3. ID table (founder/owner fills):

| Library / preset | Category | Vendor library_id | Vendor program ID(s) | Source folder | Rights record ref |
|------------------|----------|-------------------|----------------------|---------------|-------------------|
| TODO | TODO | TODO (to be reserved) | TODO | TODO | TODO |

4. Open questions for the tool owner: ID format and numbering scheme (TODO), where the reservation registry lives (TODO), who approves new IDs (TODO).

Handoff accepted by: ____  Date: ____
