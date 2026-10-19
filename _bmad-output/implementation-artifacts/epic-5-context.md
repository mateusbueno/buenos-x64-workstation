# Epic 5 Context: Library: import and find timbres

<!-- Generated from planning artifacts. Regenerate with compile-epic-context if planning docs change. -->

## Goal

A keyboardist imports SFZ/SF2 libraries from a pen drive or SSD with suggested categories, finds timbres by search, category, favorites and recent, auditions them while browsing, recovers from missing libraries and removes libraries to free space. It builds on Epic 1's lossless bundle format, demo SFZ importer, atomic publish and `current` resolution, and on Epic 4's device UI shell, Part surface, Live Set state and Setlist Browser components (Search Field, Filter Chip, Result Tile, On-screen Keyboard). Development is Mac-first: formatlib, librarian and importer logic run on the Mac; device UI runs in the LVGL SDL simulator at 1024x600; USB-A mounting, the sandbox and throughput acceptance need the dev board.

## Stories

- Story 5.1: Full SFZ v1 opcode subset in the importer
- Story 5.2: Sample engine plays keyswitches, CC crossfades and basic filters
- Story 5.3: SF2 import into the same instrument model
- Story 5.4: Reference corpus imports and plays with mapping intact
- Story 5.5: Category model, localized labels and auto-suggestion
- Story 5.6: Librarian libraries catalog, import API and atomic publish
- Story 5.7: Import preflight: batch scan, free space and duplicates
- Story 5.8: Import execution survives drive removal and power loss
- Story 5.9: Sandboxed librarian and read-only USB-A drives on the board
- Story 5.10: Design story, Notice Toast mockup
- Story 5.11: Design story, Category Picker mockup
- Story 5.12: Import surface: rows, categories and apply to all
- Story 5.13: Import surface: progress, background and failure states
- Story 5.14: "USB drive found" notice
- Story 5.15: 300 GB SSD library imports and plays end to end
- Story 5.16: Library surface: search, categories, favorites and recent
- Story 5.17: Choose a timbre for a part with audition
- Story 5.18: Design story, Action Sheet library variants
- Story 5.19: Favorites by long press and from the Part surface
- Story 5.20: Missing library state and recovery
- Story 5.21: Storage & Library: free space, installed libraries and removal

## Requirements & Constraints

- Imports must be lossless: never resample or requantize; decode round-trip must be bit-exact. User imports are never encrypted.
- SFZ opcodes outside the v1 subset skip the file with `unsupported_features` plus the offending opcodes; unparseable or missing/truncated samples skip with `damaged`; a non-sounding opcode allow-list is logged, not skipped. Other files in the batch still import.
- Every instrument carries exactly one of 17 categories (stable numeric IDs, grow by addition only; "none" is never storable). Imports require a category: auto-suggested from names/metadata (EN/PT-BR/ES keyword table), user confirms or changes; import is refused if any row lacks one.
- Category labels: full and short in EN, PT-BR, ES; short labels at most 10 characters; tests fail on missing keys. All UI strings are locale-catalog keys.
- A library becomes visible only after complete, verified publish. Drive removal or power loss mid-import publishes what is finished, discards the in-progress timbre, and leaves other libraries untouched.
- Import is refused up front when worst-case required space exceeds free space on `/data` (typed error with both numbers).
- Importing must never disturb sound: zero xruns and zero stream underruns, audio-safe at all times; the disk stays read-only during performance (deletions/GC only at idle, never during an import).
- Sample engine: keyswitches per part, CC crossfades smoothed per block, per-voice filters, no allocation/lock on the render path (RTSan clean); null-tested against sfizz.
- Missing library: part plays silent, never substituted; resolves automatically when the library returns.
- Reference corpus (Salamander, VSCO2, Sonatina, Karoryfer, Pianobook selection) imports and null-tests nightly; corpus audio is never committed.
- Search results update per keystroke within 100 ms (Mac figure); every favorite reachable within 2 interactions, every instrument assignable within 3.

## Technical Decisions

- Processes: `engine`, `ui`, `librarian`, `sysd`. `librarian` is the only writer under `/data/lib` and of the catalog (SQLite WAL, FTS5 search index, recent and favorites tables, versioned `schema_version`); ui and desktop reach it only via the Library API (`ws.v1` protobuf over UDS).
- Publish order: stage bundle, verify bit-exact decode round-trip, fsync, rename, flip `current`, then write catalog rows; a startup reconcile removes orphans. Published bundles are immutable; adding timbres publishes a new bundle reusing verified sample data (hard links). Manifest records instrument names and categories so the catalog can be rebuilt from bundles.
- Identity: `library_id` stable (UUIDv5 of source fingerprint for user imports), `bundle_id` is the manifest hash, `instrument_key` stable across re-conversion. Engine resolves only via `/data/lib/<library_id>/current`; paths never appear in Live Sets.
- Bundle `format_version` is bumped on model extension; reader accepts N and N-1; earlier demo bundles must render bit-identically.
- Engine holds a `BundleLease` on bundles referenced by live generations; librarian deletes only unleased, non-`current` bundles. Preview and assignment use engine generations swapped in once heads are resident; held notes finish on their original generation.
- Board sandbox: librarian runs under systemd sandboxing (own user, `ProtectSystem=strict`, write only to `/data/lib` and catalog/staging, no network, `SystemCallFilter`, `NoNewPrivileges`), low priority, off the audio cores. USB-A drives mount read-only `nosuid,nodev,noexec`; only librarian reads them; NTFS and other filesystems are unsupported.
- Code layout: `formatlib/` (bundle, codec, vendored SFZ parser), `librarian/` (import, convert, index, catalog, category suggestion), `proto/` (category type, API messages). C++20.

## UX & Interaction Patterns

- Import surface: full-width Import Rows (name, size, format, Category Chip with `✓ suggested` or warn-bordered `Choose category`), header with drive name/count/size, "Apply one category to all", Skip default for already-imported rows; progress bar with background-on-EXIT while keys keep playing.
- Library surface: Search Field with On-screen Keyboard (keys still sound), Filter Chips (Favorites, Recent, then 17 categories), 3-column Result Tile grid with `CATEGORY · FACTORY/IMPORTED` meta; defined empty states. Category shown as short label on Part Strips, full label elsewhere, never the engine type.
- Choose-timbre mode ("CHOOSE TIMBRE FOR PART n"): playing keys auditions on the part; WRITE assigns as a Live Set edit (recorded in Recent); EXIT reverts without a cut. Preview refused with a Hint Bar message when memory is short.
- Notice Toast: never takes focus, 8 s auto-dismiss (paused on touch), EXIT dismisses, waits for ASSIGN overlay, never covers Live Set Hero or Scene strip; at most one per insertion.
- Action Sheets: centred, VALUE highlight, WRITE confirms, EXIT cancels, never raised unprompted during performance. Favorites via long press or the Part surface star.
- Design stories need Stage and Studio themes, EN/PT-BR/ES, pseudo-localization at +40% (`ÁÇÊÕÑ Ação`), and contrast of 4.5:1 text and 3:1 boundaries.

## Cross-Story Dependencies

- 5.1 -> 5.2, 5.3, 5.4; 5.5 -> 5.6, 5.12; 5.6 -> 5.7 -> 5.8 -> 5.9 and the Import surface (5.12, 5.13); 5.15 needs the board and the full pipeline.
- Design stories gate their UI: 5.10 -> 5.13/5.14 (Notice Toast is built in 5.13); 5.11 -> 5.12; 5.18 -> 5.19-5.21.
- 5.4 is blocked by Story 11.12; 5.14's update toast target is wired by Epic 8.
- Depends on Epic 1 (formatlib, importer, publish), Epic 4 (UI shell, Part surface, Live Set edits, Story 4.11 librarian service, Story 4.19 Action Sheet base) and the Settings entry point for Storage & Library (5.21).
