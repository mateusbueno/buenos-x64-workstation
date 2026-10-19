# formatlib

Role: lossless block-indexed library bundle (Story 1.12) plus the SFZ-to-instrument-model layer (Story 1.13) and the SF2 one (Story 5.3). Writer and reader for the container, identity minting (`bundle_id`, `library_id`, `instrument_key`), in-house SHA-256/SHA-1/UUIDv5, a small WAV loader that feeds the writer (`WavOptions::reject_truncated_data` makes a data chunk shorter than its header says `kTruncated` instead of clamping it; the librarian uses it), the per-instrument region table, and the SFZ importer mapping (`velora_sfz`). The SoundFont 2 reader (Story 5.3, in `velora_format`) maps SF2 presets onto the same model. Crypto, the librarian (`librarian/`) and streaming are out of scope.

Dependency rule: may link only libFLAC (found via CMake config, pkg-config or find_library; >= 1.4, target 1.5.0; library only, no flac/metaflac binaries), the vendored SFZ parser closure under `third_party/` (only for the separate `velora_sfz` target) and the C++ standard library. It must not include anything from `engine/core` (or `engine/host`), and `engine/core` must not depend on it.

Container (target `velora_format`, namespace `velora::format`):

- Layout: 128-byte header, manifest, index table, blocks; little-endian, strictly contiguous, `total_size` in the header makes any truncation a typed error.
- Every sample is cut into blocks of `kBlockFrames` (4096) frames; frame k lives in block `k / 4096`, so a seek decodes exactly one block (`BundleReader::read_block`, per-thread `BlockDecoder`). 16/24-bit PCM is coded per block as a self-contained libFLAC stream (verified by decode at write time); 32-bit float is stored raw. Mono/stereo, 44.1/48/88.2/96 kHz. No resampling or requantization.
- Index entry: offset, size, frames and SHA-256 of the stored block bytes (checked before decoding unless `ReaderOptions::verify_block_hash` is off). The manifest carries per-sample PCM SHA-256, so `verify_all()` is the bit-exact decode verify.
- Identity: `bundle_id` = SHA-256 of the manifest (which embeds the index hash, so it covers all content); `library_id` = vendor-assigned or UUIDv5 of a fingerprint of keys, formats and PCM hashes; `instrument_key` = normalized relative path, minted only by `BundleWriter::add_instrument`. Output is deterministic for a given libFLAC version (encoded frames, hence file bytes and `bundle_id`, may differ across libFLAC versions; PCM hashes, decoded PCM and `library_id` do not). Only the STREAMINFO metadata block is written (no VORBIS_COMMENT/padding). Names are not part of `library_id`.
- Versions: `kFormatVersion` = 2 (major) and `kFormatMinor` = 1. The version word in the header and at the start of the manifest is `major | minor << 16`; the reader accepts major N and N-1 (1 and 2), minor N and N-1 (0 and 1; major 1 only minor 0) and rejects anything else with `Error::kUnsupportedVersion`; `BundleReader::format_version()` is the major, `format_minor()` the minor. Version 2 adds, after each instrument's sample list in the manifest, a region table (`u32` count + fixed 96-byte records, layout in `src/layout.hpp`); version 1 bundles read with empty region tables, and the writer refuses regions for `WriterOptions::format_version == 1`. Zero-length samples are allowed.
- Format 2.1 (Story 5.1, keyswitches, CC crossfades, filters): the region record stays 96 bytes. A bundle that has at least one region using the new features is stamped minor 1 and carries, after each instrument's region table, an extension table: `u32 n_ext` followed by `n_ext` fixed 56-byte records in ascending region order (`u32 region_index`, `u8 sw_flags`, 7 note bytes, `u8 xfin_count, xfout_count, xf_curve, filter_type`, 4 xfin and 4 xfout entries of `u8 cc, lo, hi, 0`, `f32 cutoff_hz`, `f32 resonance_db`; exact layout and the canonical-form rules in `src/layout.hpp`). A bundle without such regions is stamped minor 0 and is byte-identical to what the Epic 1 writer produced (no table, same version word), so Epic 1 demo bundles and every committed golden are untouched; the extension is not ignorable: a reader that does not know minor 1 (Epic 1 code) refuses a minor 1 bundle with `kUnsupportedVersion` instead of misreading it, while bundles without extended features stay minor 0 and byte-identical so Epic 1 readers keep opening them. A minor 1 bundle with no extension record at all is non-canonical (`kCorruptManifest`). The reader rejects non-canonical extensions (unsorted or duplicate region indexes, set bits without values, non-zero padding, a record for a region that uses no extension feature) as `kCorruptManifest`. `WriterOptions::format_minor = 0` is the test hook that refuses extension regions. Tests pin the record with a golden hex literal (`format_bundle_test`).
- PCM exchange convention: interleaved `int32_t`; float32 is carried as its IEEE bit pattern.

Instrument model (`velora/format/instrument_model.hpp`): a `Region` holds sample index, key lo/hi/center, velocity lo/hi, transpose/tune/volume/pan, offset, `seq_length`/`seq_position`, `lorand`/`hirand`, loop mode/start/end and the `ampeg` envelope, plus `trigger` (attack/release) and the Story 5.1 extension: `Keyswitch sw` (`sw_lokey`, `sw_hikey`, `sw_last`, `sw_down`, `sw_up`, `sw_previous`, `sw_default`; -1 = not given), `Crossfade xf` (up to `kMaxCcCrossfades` = 4 xfin and 4 xfout controllers, each `{cc, lo, hi}` with cc 0..127 sorted ascending, `xf_cccurve` gain|power, default power) and `Filter filter` (`fil_type` lpf_1p|lpf_2p|hpf_1p|hpf_2p|bpf_2p, `cutoff` Hz 0..48000, `resonance` dB 0..40; type none = no filter). `has_extension(region)` says whether a region needs an extension record. Playing these is Story 5.2 (done: `engine/core/README.md`, "Keyswitches, CC crossfades and filters"; the loader `engine/host/sample_bundle_loader` carries them to the sample-source port). `BundleWriter::add_region` validates a region against its sample (`validate_region`); the reader validates the same invariants and reports `kCorruptManifest`. Regions are part of the manifest, hence of `bundle_id`. `BundleWriter::set_source_id(s)` mints `library_id` = UUIDv5 of SHA-256("velora.source.v1" || 0 || s) instead of the content fingerprint, so re-importing changed content of the same source keeps its `library_id` (the librarian uses the SFZ's canonical absolute path).

SFZ import (`velora/format/sfz_import.hpp`, target `velora_sfz`): `parse_sfz_file(path)` runs the vendored sfizz 1.2.3 parser (BSD-2-Clause; licenses and the exact file list in `THIRD_PARTY_NOTICES.md` and `_bmad-output/business/11-12-sfizz-license-check.md`, sources in `third_party/`, built as `velora_sfizz_parser`) and maps the supported subset below: headers `<global> <master> <group> <region> <control>`, `#define`, `#include`. It reads no audio. The opcode list is `sfz_opcode_table()`, the single authoritative list; the table here is compared with it by `format_sfz_test` (keep the markers). Opcode classes:

- **supported**: mapped into the instrument model.
- **allow-listed**: purely informational opcodes that cannot change the sound (labels, picture, editor hints). They are accepted anywhere after a header, never stored, and returned in `SfzParseResult::allowlisted` with file:line so the importer can log and count them. `set_cc<N>` is deliberately not on the list: it sets a controller's initial value, which drives crossfades.
- **unsupported**: everything else, including all scripting (`script`), effect buses (`<effect>`, `effect1`...), `<curve>`, LFOs (`lfo*`), envelopes other than `ampeg_*` (`eg*`), `*_oncc*` modulations, `amp_*`, `fil_*` other than `fil_type`, `cutoff2`, `loop_crossfade`, `polyphony`/`group`/`off_by`, `output`, `delay`, `note_offset`/`octave_offset`, `sw_lolast`/`sw_hilast`, headers other than the five above, unsupported values (`trigger=legato`, `loop_mode=loop_bidir`, `fil_type=lpf_4p`, `xf_cccurve=sine`, `sample=*sine` generators, controller numbers above 127, more than 4 xfin or 4 xfout controllers in a region) and parser warnings. Each offender is an issue with file:line, kind and (for `unsupported_*` kinds) the offending `opcode`; the whole file is rejected, there is no partial result. `is_unsupported_feature(kind)` is true for exactly kUnsupportedOpcode/Header/Value; every other kind (syntax, bad number, missing sample, inverted range, path escape) means the file is damaged.

Mapping rules of Story 5.1: `sw_*` opcodes take note numbers or names. `xfin_locc<N>`/`xfin_hicc<N>`/`xfout_locc<N>`/`xfout_hicc<N>` (N is the controller number, 0..127) give per-controller ranges of the CC value 0..127; a missing `*_locc`/`*_hicc` defaults to 0 (so `locc` without `hicc` is an inverted range and an error). The filter exists only when `cutoff` is given, `fil_type` defaults to `lpf_2p`, and `fil_type`/`resonance` without `cutoff` are accepted and mean no filter, but never silently: each is returned in `SfzParseResult::warnings` and the import report lists it as an `ignored_opcode` warning with file:line. Controller numbers must be canonical: `xfin_locc007` is an error (it would spell controller 7 twice). Loop opcodes (`loop_mode` all four values, `loop_start`, `loop_end`) are unchanged from Story 1.13 (defaults are resolved by the importer once the sample length is known).

<!-- sfz-opcode-table:begin -->
| Opcode | Class | Notes |
|---|---|---|
| `sample` | supported | required in every region |
| `default_path` | supported | <control> only |
| `lokey` | supported | note number or name |
| `hikey` | supported |  |
| `key` | supported | lokey = hikey = pitch_keycenter |
| `lovel` | supported |  |
| `hivel` | supported |  |
| `pitch_keycenter` | supported |  |
| `tune` | supported | cents |
| `transpose` | supported | semitones |
| `volume` | supported | dB |
| `pan` | supported | -100..100 |
| `offset` | supported | frames |
| `seq_length` | supported | round robin |
| `seq_position` | supported | round robin |
| `lorand` | supported |  |
| `hirand` | supported |  |
| `loop_mode` | supported | no_loop, one_shot, loop_continuous, loop_sustain |
| `loop_start` | supported | frames |
| `loop_end` | supported | frames, inclusive |
| `ampeg_delay` | supported | s |
| `ampeg_start` | supported | % |
| `ampeg_attack` | supported | s |
| `ampeg_hold` | supported | s |
| `ampeg_decay` | supported | s |
| `ampeg_sustain` | supported | % |
| `ampeg_release` | supported | s |
| `trigger` | supported | attack, release |
| `sw_lokey` | supported | keyswitch range low (Story 5.1) |
| `sw_hikey` | supported | keyswitch range high (Story 5.1) |
| `sw_last` | supported | (Story 5.1) |
| `sw_down` | supported | (Story 5.1) |
| `sw_up` | supported | (Story 5.1) |
| `sw_previous` | supported | (Story 5.1) |
| `sw_default` | supported | (Story 5.1) |
| `xfin_locc<N>` | supported | N = controller 0..127, up to 4 per region (Story 5.1) |
| `xfin_hicc<N>` | supported | (Story 5.1) |
| `xfout_locc<N>` | supported | (Story 5.1) |
| `xfout_hicc<N>` | supported | (Story 5.1) |
| `xf_cccurve` | supported | gain, power (Story 5.1) |
| `fil_type` | supported | lpf_1p, lpf_2p, hpf_1p, hpf_2p, bpf_2p (Story 5.1) |
| `cutoff` | supported | Hz; the filter exists only when cutoff is given (Story 5.1) |
| `resonance` | supported | dB (Story 5.1) |
| `label_cc<N>` | allow-listed | controller name |
| `label_key<N>` | allow-listed | key name |
| `global_label` | allow-listed |  |
| `master_label` | allow-listed |  |
| `group_label` | allow-listed |  |
| `region_label` | allow-listed |  |
| `sw_label` | allow-listed | keyswitch name |
| `image` | allow-listed | instrument picture |
| `hint_*` | allow-listed | editor hints |
<!-- sfz-opcode-table:end -->

SF2 import (`velora/format/sf2_import.hpp`, in `velora_format`, Story 5.3): a clean-room reader written from the public SoundFont 2.04 specification (RIFF `sfbk`: `INFO` with `ifil`, `sdta` with `smpl` and the optional `sm24`, `pdta` with `phdr pbag pmod pgen inst ibag imod igen shdr`); no SF2 library is vendored, linked or consulted (FluidSynth is only a test oracle, `tools/oracle`). `parse_sf2_file(path)` / `parse_sf2_memory(bytes)` / `parse_sf2(ByteSource)` return `Sf2ParseResult`: `presets` (one `Sf2Preset` per preset: `key` = `"bank:preset"` in decimal, name, the preset's own `samples` and `regions`), `warnings`, `stats` and a handle to the sample words. Never throws; every chunk size, record index and sample offset is bounds checked; memory is bounded by constants and the file size: tables are read once (each at most 64 MiB), instrument zones are scanned and cached once (at most 256 Ki zones, about 50 MB), regions are capped at 512 Ki (about 100 MB of `Region` records at the cap), sample data is read one sample at a time on request; generator scanning is linear (`Sf2Stats::generator_scans`, asserted by a counted-operations test, not by a clock). The header comment of `sf2_import.hpp` is the full mapping table; in short:

- **Preset -> instrument**: `instrument_key` = `bank:preset` (`0:0`, `128:0` for the percussion bank, no leading zeros); a repeated bank:preset is damage. The instrument name is the preset name (names are UTF-8: valid sequences are kept, other bytes are read as Latin-1, control characters become blanks; an empty name falls back to the key). A preset that ends with no region is not published (an empty instrument could only fail at engine load): it is listed as a warning with the code `ignored_empty_preset`.
- **Zones -> regions**: every preset-zone x instrument-zone pair with a `sampleID` becomes one region, in file order. Global zones are folded in; the preset generator is **added** to the instrument's for the additive generators and clamped to the generator's range (instrument-only generators at preset level are ignored with a warning); `keyRange`/`velRange` of both levels are intersected (an empty intersection drops the pair silently); a zone after the first without its terminal generator, and generators after the terminal one, are ignored (specification 8.1.2). Mapped: key and velocity range, root key (`overridingRootKey`, else the sample's `byOriginalPitch`, 255 = 60), `coarseTune`, `fineTune` + `pitchCorrection`, `sampleModes` 1 / 3 (continuous / until release; loop points = the `shdr` points plus the four loop/start offset generators and their coarse variants, end point = SF2 end - 1; validated inside the sample), `startAddrsOffset` (the end offsets are not applied, the model has no sample end: reported as ignored), `initialAttenuation`, `pan` (0.1 % to -100..100), the six volume-envelope generators, `initialFilterFc` (absolute cents to Hz; 13500 = no filter) and `initialFilterQ` (see the header: the model's Q is `q/10 - 3.01` dB clamped to 0..40, and half the peak height comes off the region volume). Two engine conventions are compensated in the region values: every region carries -3.0103 dB (the sample engine's pan law gives a mono sample +3.01 dB) and decay/release times are multiplied by 0.7817 (the engine's "nine time constants" against the SF2 time for 100 dB).
- **Samples**: one mono bundle sample per `shdr` record a preset references, at the source rate (44.1/48/88.2/96 kHz) and bit depth (16-bit `smpl`, 24-bit when `sm24` is present: `(word << 8) | byte`), bit identical to the file, never resampled. Stereo samples (left type 4, right type 2) stay two mono samples played with their zones' own pans (hard pan when the zone has none). Shared samples are stored once **per preset** (the bundle keeps samples per instrument): a documented gap.
- **Ignored, never a reason to skip**: non-default modulators, LFOs, the modulation envelope, effects sends, `scaleTuning` other than 100, `exclusiveClass`, `keynum`/`velocity`, `keynumToVolEnv*`, sample end offsets and unknown generators are reported once per preset as an `Sf2Warning` (the librarian's report code `ignored_sf2_feature`, with the preset id and the generator or modulator name; file-level counters). The ten default modulators of section 8.4 are not reported (velocity to attenuation is the engine's velocity curve) except velocity -> filter cutoff, which is listed once per preset with a filter (filtered presets sound brighter at velocity below 127). A zone the model cannot hold (a start offset or loop outside its sample, a region that fails `validate_region`) is **dropped with a warning** (`ignored_zone`: preset, reason) and the file still imports.
- **Damaged** (`Sf2IssueKind::kDamaged`): bad RIFF/LIST/chunk sizes, a missing or repeated chunk or table, table sizes that are not a multiple of the record size, a missing or wrong terminal record, bag/generator/modulator/instrument/sample indexes out of range or going backwards, `sm24` of the wrong size, a duplicate bank:preset, and for **referenced** samples only `shdr` start > end or beyond `smpl`, an empty sample, an invalid `sampleType` or `sampleLink` (unreferenced sample headers are never examined; an odd `smpl` byte size loses its last byte; fewer than 8 bytes of slack inside a container are tolerated), more than `kMaxZoneCombos` (4 Mi) zone pairs, `kMaxRegions` (512 Ki) regions or `kMaxInstrumentZones` (256 Ki) instrument zones. **Unsupported** (`kUnsupported`): `ifil` major version other than 2, a referenced ROM sample, a referenced sample whose rate is outside 44.1/48/88.2/96 kHz (the importer never resamples). Damage wins over unsupported; a damaged or unsupported file yields no preset at all.

Tests: `format_sf2_test` (`tests/sf2_test.cpp`; fixtures from the test-only SF2 **writer** `tests/sf2_builder.hpp`): a General-MIDI-style bank (6 presets over banks 0, 1, 8 and 128, shared and stereo samples, looped and one-shot samples, envelopes, filters, key and velocity splits, preset-level offsets) in 16- and 24-bit, every intended zone compared field by field with the imported region (the expected values are written from the specification's formulas, independently of the importer), sample words compared bit for bit after a bundle round trip (24-bit assembly included), a single-instrument file, the ignored-feature warnings (an LFO, a modulation envelope and a custom modulator report exactly those), the zone/generator semantics (global zones, preset addition, clamping, range intersection, terminal generators), names, unsupported files, 39 structural damage cases (every class of the damage list; model-limit cases are `ignored_zone` warnings, tested separately), odd/padded `sm24`, global-zone ranges, the specification's units (`spec_table_tests`), a counted-operations bound on generator scans (each structural damage case must be skipped with `damaged` and a detail), zone-explosion bounds, 6000 mutated copies of a valid file plus every truncation point (never a crash or a hang, memory bounded). The librarian and render-comparison tests are in `librarian/` and `tools/offline-render/tests/sf2_suite`.

Custom sources: `BundleReader::open_source` takes a `ByteSource` (`velora/format/byte_source.hpp`) so a host can back a reader with its own file-system port (used by the library resolver).

Tests: `format_sf2_test` (above), `format_hash_test`, `format_bundle_test` (round trips, seek, truncation and bit-flip sweeps, corruption, version 1 and 2, region table, identity), `format_wav_test`, `format_sfz_test` (opcode mapping, inheritance, unsupported constructs, allow-list, Story 5.1 opcodes, README table parity).

## Categories and locale catalog (Story 5.5)

**Category model.** `velora/format/category.hpp` is the C++ twin of `proto/category.proto` (package `velora.library.v1`, enum `Category`): `CATEGORY_NONE = 0` (the "unassigned"
sentinel for filters and queries, **never storable**: `is_assignable()` is false, `category_file_text()` and the librarian refuse it) and the 17 categories with stable
numeric ids 1..17 (Piano, E.Piano, Organ, Keys, Mallet & Bells, Strings, Brass, Woodwind, Choir & Vocal, Guitar, Bass, Synth Lead, Synth Pad, Synth Poly, Drums & Perc,
World, FX & Ambient). Ids only grow by addition and are never renumbered or reused; a retired category would be removed and its id put in a proto `reserved` statement.
`format_category_test` freezes the id -> slug list, parses `proto/category.proto` (no value reserved, no duplicate, names `CATEGORY_<SLUG>`, equal to the header) and
`engine/host/tests/category_pb_test.cpp` compares the protoc output with the header. Helpers: `key(c)` (`category.piano`, `category.epiano`, `category.mallet_bells`, ...),
`slug`, `english_name`, `english_short_name`, `is_assignable`, `category_from_id`, `parse_category` / `parse_assignable_category` (key, slug or English full/short name, case-insensitive,
ignoring spaces and punctuation), `kCategoryCount = 17`. The bundle carries it as the file `<bundle_dir>/category` holding the key (`category_file_text` / `parse_category_file`;
the reader also accepts the free text older bundles hold). The manifest is not touched (no `format_version` bump, bundles without a category stay byte-identical); the catalog of
Story 5.6 records the category from this file.

**Locale catalog** (minimal, AD-20; Epic 10 / Story 10.7 extends it). `locale/en.json`, `locale/pt-BR.json`, `locale/es.json`: one UTF-8 JSON object per locale, flat string
keys to string values (no nesting, no duplicates; English is the source and default locale; placeholders will be `{name}`, none today). The category keys are
`category.<slug>.full` and `category.<slug>.short`, copied verbatim from the Categories table of the UX EXPERIENCE.md. `LocaleCatalog` (`velora/format/locale_catalog.hpp`, host code, not
`engine/core`) loads them with the in-house strict JSON reader (`json_lite.hpp`): `find_own` (no fallback), `find` / `get` (fall back to English, then the key itself), `category_label`.
`format_category_test` fails when a category key is missing in a shipped locale (the English fallback does not hide it), when a short label exceeds 10 Unicode code points (counted
as code points, not bytes) or when the three locales do not have exactly the same key set; it also proves those checks fail on broken catalogs.
