# formatlib

Role: lossless block-indexed library bundle (Story 1.12) plus the SFZ-to-instrument-model layer (Story 1.13) and the SF2 one (Story 5.3). Writer and reader for the container, identity minting (`bundle_id`, `library_id`, `instrument_key`), in-house SHA-256/SHA-1/UUIDv5, a small WAV loader that feeds the writer (`WavOptions::reject_truncated_data` makes a data chunk shorter than its header says `kTruncated` instead of clamping it; the librarian uses it), the per-instrument region table, and the SFZ importer mapping (`velora_sfz`). The SoundFont 2 reader (Story 5.3, in `velora_format`) maps SF2 presets onto the same model. The protected (encrypted, signed) bundle variant is Story 6.2 (section "Protected bundles" below); the librarian (`librarian/`) and streaming are out of scope.

Dependency rule: `velora_format` (the core) may link only libFLAC (found via CMake config, pkg-config or find_library; >= 1.4, target 1.5.0; library only, no flac/metaflac binaries), the vendored SFZ parser closure under `third_party/` (only for the separate `velora_sfz` target) and the C++ standard library; it contains no cryptography code. The three protected-bundle targets (`velora_format_verify`, `velora_format_decrypt`, `velora_format_protected_writer`, Story 6.2) additionally link OpenSSL libcrypto (>= 3.0, dynamic, Apache-2.0). It must not include anything from `engine/core` (or `engine/host`), and `engine/core` must not depend on it.

Container (target `velora_format`, namespace `velora::format`):

- Layout: 128-byte header, manifest, index table, blocks; little-endian, strictly contiguous, `total_size` in the header makes any truncation a typed error.
- Every sample is cut into blocks of `kBlockFrames` (4096) frames; frame k lives in block `k / 4096`, so a seek decodes exactly one block (`BundleReader::read_block`, per-thread `BlockDecoder`). 16/24-bit PCM is coded per block as a self-contained libFLAC stream (verified by decode at write time); 32-bit float is stored raw. Mono/stereo, 44.1/48/88.2/96 kHz. No resampling or requantization.
- `BundleWriter::discard_last_instrument()` (Story 5.8) drops the instrument added last with its samples, regions and encoded blocks, leaving the writer exactly as if it had never been added (only while its blocks are the tail of the block store). The librarian uses it to discard a timbre whose source vanished halfway; `finalize()` may be called repeatedly while instruments are added in between, which is how a checkpoint bundle of the first k timbres is written. No format change.
- Index entry: offset, size, frames and SHA-256 of the stored block bytes (checked before decoding unless `ReaderOptions::verify_block_hash` is off). The manifest carries per-sample PCM SHA-256, so `verify_all()` is the bit-exact decode verify.
- Identity: `bundle_id` = SHA-256 of the manifest (which embeds the index hash, so it covers all content); `library_id` = vendor-assigned or UUIDv5 of a fingerprint of keys, formats and PCM hashes; `instrument_key` = normalized relative path, minted only by `BundleWriter::add_instrument`. Output is deterministic for a given libFLAC version (encoded frames, hence file bytes and `bundle_id`, may differ across libFLAC versions; PCM hashes, decoded PCM and `library_id` do not). Only the STREAMINFO metadata block is written (no VORBIS_COMMENT/padding). Names are not part of `library_id`.
- Versions: `kFormatVersion` = 2 (major) and `kFormatMinor` = 6. The version word in the header and at the start of the manifest is `major | minor << 16`; the reader accepts major N and N-1 (1 and 2), minor N and N-1 and the Epic 1 minor (0, 1, 2, 3, 4 and 5; major 1 only minor 0) and rejects anything else with `Error::kUnsupportedVersion`; `BundleReader::format_version()` is the major, `format_minor()` the minor. Version 2 adds, after each instrument's sample list in the manifest, a region table (`u32` count + fixed 96-byte records, layout in `src/layout.hpp`); version 1 bundles read with empty region tables, and the writer refuses regions for `WriterOptions::format_version == 1`. Zero-length samples are allowed.
- Format 2.1 (Story 5.1, keyswitches, CC crossfades, filters): the region record stays 96 bytes. A bundle that has at least one region using the new features is stamped minor 1 and carries, after each instrument's region table, an extension table: `u32 n_ext` followed by `n_ext` fixed 56-byte records in ascending region order (`u32 region_index`, `u8 sw_flags`, 7 note bytes, `u8 xfin_count, xfout_count, xf_curve, filter_type`, 4 xfin and 4 xfout entries of `u8 cc, lo, hi, 0`, `f32 cutoff_hz`, `f32 resonance_db`; exact layout and the canonical-form rules in `src/layout.hpp`). A bundle without such regions is stamped minor 0 and is byte-identical to what the Epic 1 writer produced (no table, same version word), so Epic 1 demo bundles and every committed golden are untouched; the extension is not ignorable: a reader that does not know minor 1 (Epic 1 code) refuses a minor 1 bundle with `kUnsupportedVersion` instead of misreading it, while bundles without extended features stay minor 0 and byte-identical so Epic 1 readers keep opening them. A minor 1 bundle with no extension record at all is non-canonical (`kCorruptManifest`). The reader rejects non-canonical extensions (unsorted or duplicate region indexes, set bits without values, non-zero padding, a record for a region that uses no extension feature) as `kCorruptManifest`. `WriterOptions::format_minor = 0` is the test hook that refuses extension regions. Tests pin the record with a golden hex literal (`format_bundle_test`).
- Format 2.2 (Story 13.3, `trigger=first|legato|release_key` and `end`): the 96-byte region record is unchanged, except that its trigger byte is 1 for `trigger=release` and 0 for every other mode. A bundle with at least one region that has `trigger` first/legato/release_key or an `end` is stamped minor 2 and carries, after each instrument's minor 1 table (written in a minor 2 bundle even when it has no records, `u32 0`), a trigger/end table: `u32 n_trig` followed by `n_trig` fixed 16-byte records in ascending region order (`u32 region_index`, `u8 trigger` 0..4 = attack, release, first, legato, release_key, `u8 flags` bit 0 = has_end, `u16 0`, `u64 end`; layout in `src/layout.hpp`). `end` is in frames and exclusive: the last played frame is `min(end, sample length) - 1` (a value beyond the file is clamped by the engine), and `has_end` with `end == 0` stores a disabled region (it never plays). Every other bundle keeps minor 0 or 1 and is byte-identical to what the previous writer produced, so every existing demo bundle and golden is untouched. A minor 2 bundle with no trigger/end record, a record for a region that needs none, a trigger byte that disagrees with the region record, `end` without `has_end`, non-zero padding or unknown flag bits is `kCorruptManifest`. `WriterOptions::format_minor = 1` is the test hook that refuses such regions (and so writes the N-1 form of any other content).
- Format 2.3 (Story 13.4, `amp_veltrack`, `amp_velcurve_<N>`, `amplitude`, `amp_keytrack`/`amp_keycenter`, `ampeg_vel2attack|decay|sustain|release`): the region record is unchanged. A bundle with at least one region whose `AmpMap` differs from its default is stamped minor 3 and carries, after each instrument's minor 2 table (written in a minor 3 bundle even when empty, `u32 0`), an amp table: `u32 n_amp` followed by `n_amp` variable-length records in ascending region order (`u32 region_index`, `f32 amp_veltrack` percent, `f32 amplitude` percent, `f32 amp_keytrack` dB per key, `u8 amp_keycenter`, `u8 curve_count` 0..32, `u16 0`, `f32 vel2attack, vel2decay, vel2sustain, vel2release`, then `curve_count` points of `u8 vel` (strictly ascending), `u8 0`, `u16 0`, `f32 gain`; layout in `src/layout.hpp`). `group_volume`, `master_volume` and `global_volume` are added to the region's `volume_db` at import and `group_`/`master_`/`global_amplitude` multiply `amplitude`, as sfizz sums them, so they need no field. Every other bundle keeps minor 0, 1 or 2 and its exact bytes, so every existing demo bundle and golden is untouched. A minor 3 bundle with no amp record, a record for a region whose map is the default, a non-ascending or out-of-range curve, an out-of-range scalar or non-zero padding is `kCorruptManifest`; minor 7 is `kUnsupportedVersion`. `WriterOptions::format_minor = 2` is the test hook that refuses such regions.
- Format 2.4 (Story 13.5, CC at voice start: `locc`/`hicc`/`lohdcc`/`hihdcc`, `set_cc`/`set_hdcc`, `*_oncc`/`*_cc`/`*_curvecc` on amplitude, volume, pan, tune, offset and the `ampeg_*` envelope): the region record is unchanged. A bundle with a region that has a CC gate or a CC modulation entry, or an instrument with a CC default or a `<curve>` table the regions use, is stamped minor 4 and carries, after each instrument's minor 3 table (written in a minor 4 bundle even when empty, `u32 0`), the cc table: `u32 n_cc` and `n_cc` variable-length records in ascending region order (`u32 region_index`, `u8 gate_count` <= 8, `u8 mod_count` <= 16, `u16 0`, then the gates `{u8 cc, 3 x 0, f32 lo, f32 hi}` ascending by cc, with `lo`/`hi` in CC units 0..127 inclusive, then the entries `{u8 target 0..11, u8 cc, u8 curve, u8 0, f32 depth}` ascending by (target, cc)), then `u32 n_defaults` records `{u8 cc, 3 x 0, f32 value}` ascending by cc (`set_cc`, CC units), then `u32 n_curves` curves `{u8 index, u8 0, u16 n_points <= 128, points {u8 x ascending, 3 x 0, f32 y}}` ascending by index. Targets: 0 amplitude (percent), 1 volume (dB), 2 pan (percent), 3 tune (cents), 4 offset (frames), 5..11 ampeg delay, start (percent), attack, hold, decay, sustain (percent), release; controllers: 0..127 and the sfizz pseudo-CCs 131, 135, 136, 137, 140, 141. The importer prunes the instrument setup to what a region uses (a `set_cc` of a controller no gate, entry or crossfade reads and every `<curve>` no entry names are not stored), so a library with `<curve>` blocks or `set_cc` that nothing reads keeps its bytes. Every other bundle keeps minor 0..3 and its exact bytes. A minor 4 bundle with no cc record, default or curve, a record for a region without a gate or entry, out-of-range or non-ascending values, a depth outside its target's range, a controller that is not a source, NaN, -0.0 or non-zero padding is `kCorruptManifest`; minor 7 is `kUnsupportedVersion`. `BundleWriter::set_instrument_cc_setup` stores the instrument part (`InstrumentInfo::cc_setup` reads it back); `WriterOptions::format_minor = 3` is the test hook that refuses such content.
- Format 2.5 (Story 13.6, `group`, `off_by`, `off_mode`, `off_time`, `polyphony`, `note_polyphony`, `note_selfmask`): the region record is unchanged. A bundle with at least one region whose `VoiceGroup` differs from its default is stamped minor 5 and carries, after each instrument's minor 4 tables (written in a minor 5 bundle even when empty, `u32 0`), the voice-group table: `u32 n_vg` and `n_vg` fixed 28-byte records in ascending region order (`u32 region_index`, `i32 group`, `i32 off_by` (0 without off_by), `u8 flags` bit 0 = has_off_by, bit 1 = note_selfmask off, `u8 off_mode` 0 fast, 1 normal, 2 time, `u16 0`, `f32 off_time` seconds (0 unless off_mode is time), `u16 polyphony`, `u16 group_polyphony`, `u16 note_polyphony` (each 0..255, 0 = no limit), `u16 0`; layout in `src/layout.hpp`). `group_polyphony` is the limit of the region's group: the importer gives every region of a group the group's final limit (sfizz keeps one limit per group number, the last assignment winning), so the engine needs no group table. Every other bundle keeps minor 0..4 and its exact bytes. A minor 5 bundle with no voice-group record, a record for a region whose `VoiceGroup` is the default, a limit above 255, an unknown flag bit or off_mode, `off_time` outside 0..100 or non-zero without `off_mode` time, -0.0, NaN or non-zero padding is `kCorruptManifest`; minor 7 is `kUnsupportedVersion`. `WriterOptions::format_minor = 4` is the test hook that refuses such regions.
- Format 2.6 (Story 13.7, `bend_up`/`bend_down`/`bend_step`/`bend_smooth` and `on_locc`/`on_hicc` controller triggers): the region record is unchanged. A bundle with at least one region that sets a bend opcode or has a controller trigger is stamped minor 6 and carries, after each instrument's minor 5 table (written in a minor 6 bundle even when empty, `u32 0`), the bend/trigger table: `u32 n_bt` and `n_bt` records in ascending region order (`u32 region_index`, `u8 flags` bit 0 = bend enabled, bit 1 = the region never starts from a note, `u8 trig_count` 0..4, `u16 smooth_ms` (0 unless bend enabled), `f32 bend_up`, `f32 bend_down`, `f32 bend_step` (200, -200, 1 unless bend enabled), then `trig_count` triggers `{u8 cc, 3 x 0, f32 lo, f32 hi}` ascending by cc, `lo`/`hi` in CC units 0..127 inclusive; layout in `src/layout.hpp`). Every other bundle keeps minor 0..5 and its exact bytes (tested for each writer ceiling). A minor 6 bundle with no record, a record for a region whose bend and triggers are the default, bend values outside +-12000 cents, a step outside 1..12000, a "never from a note" flag without a trigger, nonzero padding, a trigger above controller 127 or out of order, or a minor above 6 is rejected (`kCorruptManifest` / `kUnsupportedVersion`). The reader accepts minor 0..6.
- Format 2.7 (Story 13.8, per-voice filter): the region record is unchanged. A bundle with at least one region that has a `FilterMap` (a filter tuning on the first filter or a second filter) is stamped minor 7 and carries, after each instrument's minor 6 table (written in a minor 7 bundle even when empty, `u32 0`), the filter table: `u32 n_fl` and `n_fl` records in ascending region order (`u32 region_index`, `u8 flags` bit 0 = second filter present, 3 x 0, the first filter's tune, then with bit 0 `u8 type` 1..5, 3 x 0, `f32 cutoff_hz`, `f32 resonance_db` and the second filter's tune). A tune is `f32 keytrack` (cents per key, 0..1200), `f32 veltrack` (cents at velocity 127, +-12000), `u8 keycenter`, `u8 cutoff_count`, `u8 res_count` (each 0..4), `u8 0`, then the cutoff entries and the resonance entries `{u8 cc, u8 curve, 2 x 0, f32 depth}` ascending by cc (cutoff depth in cents +-12000, resonance depth in dB 0..96; cc a controller 0..127 or a Story 13.5 pseudo-CC; curve a `<curve>` index); layout in `src/layout.hpp`. Every other bundle keeps minor 0..6 and its exact bytes (tested for each writer ceiling, plain and protected). A minor 7 bundle with no record, a record whose map is the default, a tuning or a second filter on a region without a first filter, out-of-range values, nonzero padding, entries out of order or past the bound, or a minor above 7 is rejected (`kCorruptManifest` / `kUnsupportedVersion`). The reader accepts minor 0..7.
- PCM exchange convention: interleaved `int32_t`; float32 is carried as its IEEE bit pattern.

Instrument model (`velora/format/instrument_model.hpp`): a `Region` holds sample index, key lo/hi/center, velocity lo/hi, transpose/tune/volume/pan, offset, `seq_length`/`seq_position`, `lorand`/`hirand`, loop mode/start/end and the `ampeg` envelope, plus `trigger` (attack, release, and since Story 13.3 first, legato, release_key), `has_end`/`end` (Story 13.3, exclusive sample end in frames; `end=0` disables the region) and the Story 5.1 extension: `Keyswitch sw` (`sw_lokey`, `sw_hikey`, `sw_last`, `sw_down`, `sw_up`, `sw_previous`, `sw_default`; -1 = not given), `Crossfade xf` (up to `kMaxCcCrossfades` = 4 xfin and 4 xfout controllers, each `{cc, lo, hi}` with cc 0..127 sorted ascending, `xf_cccurve` gain|power, default power) and `Filter filter` (`fil_type` lpf_1p|lpf_2p|hpf_1p|hpf_2p|bpf_2p, `cutoff` Hz 0..48000, `resonance` dB 0..40; type none = no filter). `has_extension(region)` says whether a region needs an extension record. Playing these is Story 5.2 (done: `engine/core/README.md`, "Keyswitches, CC crossfades and filters"; the loader `engine/host/sample_bundle_loader` carries them to the sample-source port). `BundleWriter::add_region` validates a region against its sample (`validate_region`); the reader validates the same invariants and reports `kCorruptManifest`. Regions are part of the manifest, hence of `bundle_id`. `BundleWriter::set_source_id(s)` mints `library_id` = UUIDv5 of SHA-256("velora.source.v1" || 0 || s) instead of the content fingerprint, so re-importing changed content of the same source keeps its `library_id` (the librarian uses the SFZ's canonical absolute path).

SFZ import (`velora/format/sfz_import.hpp`, target `velora_sfz`): `parse_sfz_file(path)` runs the vendored sfizz 1.2.3 parser (BSD-2-Clause; licenses and the exact file list in `THIRD_PARTY_NOTICES.md` and `_bmad-output/business/11-12-sfizz-license-check.md`, sources in `third_party/`, built as `velora_sfizz_parser`) and maps the supported subset below: headers `<global> <master> <group> <region> <control> <curve>` (inert), `#define`, `#include`. It reads no audio. The opcode list is `sfz_opcode_table()`, the single authoritative list; the table here is compared with it by `format_sfz_test` (keep the markers). Opcode classes:

- **supported**: mapped into the instrument model.
- **`<curve>` blocks** (Story 13.2, consumed since Story 13.5): `curve_index` and `vNNN` points are parsed into `SfzParseResult::curves`. Indexing follows sfizz's `CurveSet`: seven predefined slots (0 linear, 1 -1..1, 2 1..0, 3 1..-1, 4 x squared, 5 square root, 6 inverse square root), a block without `curve_index` is appended (and is dropped once any block had an explicit index), an explicit `curve_index` replaces or extends, an unusable `curve_index` spells 0, an unknown index in `*_curvecc<N>` is the linear default. Only the curves some `*_curvecc<N>` names are stored in the bundle (`SfzParseResult::cc_setup`); a library whose curves nothing uses is unchanged. A named curve used through a still-unsupported opcode (`amp_veltrack_curvecc<N>`, ...) skips the file. **Ignored values** (`SfzParseResult::ignored_values`): see the `lokey`/`hikey`/`key` rows below.
- **allow-listed**: purely informational opcodes that cannot change the sound (labels, picture, editor hints). They are accepted anywhere after a header, never stored, and returned in `SfzParseResult::allowlisted` with file:line so the importer can log and count them. `set_cc<N>` is deliberately not on the list: it sets a controller's initial value, which drives crossfades and, since Story 13.5, gates and modulation.
- **unsupported**: everything else, including all scripting (`script`), effect buses (`<effect>`, `effect1`...), LFOs (`lfo*`), envelopes other than `ampeg_*` (`eg*`), `*_oncc*` modulations other than the Story 13.5 targets in the table (EQ, `width`, `position`, `delay`, `end`, `loop_*`, LFO and EG matrix targets, `amp_veltrack_oncc<N>`; the filter targets `cutoff`/`resonance` landed in Story 13.8), `*_stepcc<N>` and `*_smoothcc<N>`, controller numbers outside 0..127, 131, 135, 136, 137, 140, 141 in a modulation entry and above 127 in a gate or `set_cc`, `amp_*` other than the Story 13.4 ones in the table (`amp_random`, `amp_veltrack_oncc<N>`...), `ampeg_vel2*` other than attack/decay/sustain/release, `fil_*` other than `fil_type`, `fil_keytrack`, `fil_keycenter`, `fil_veltrack` and their `fil1_`/`fil2_` spellings (`fil_random`, `fil_gain`, `fil_veltrack_oncc<N>`, `fil3_*`), `cutoff3`, `cutoff_random`, `cutoff*_chanaft`/`_polyaft`, `loop_crossfade`, `output`, a `polyphony` in a header that limits the whole region set of that header without a non-zero `group` set at that header or a broader one (`unsupported_value` on `polyphony`), `delay`, `note_offset`/`octave_offset`, `sw_lolast`/`sw_hilast`, headers other than the six above (`<curve>` is parsed and, since Story 13.5, consumed by `*_curvecc<N>`), unsupported values (`trigger=` anything but attack, release, first, legato, release_key; `loop_mode=loop_bidir`, `fil_type=lpf_4p`, `xf_cccurve=sine`, `sample=*sine` generators, controller numbers above 127, more than 4 xfin or 4 xfout controllers in a region) and parser warnings. Each offender is an issue with file:line, kind and (for `unsupported_*` kinds) the offending `opcode`; the whole file is rejected, there is no partial result. `is_unsupported_feature(kind)` is true for exactly kUnsupportedOpcode/Header/Value; every other kind (syntax, bad number, missing sample, inverted range, path escape) means the file is damaged.

Mapping rules of Story 5.1: `sw_*` opcodes take note numbers or names. `xfin_locc<N>`/`xfin_hicc<N>`/`xfout_locc<N>`/`xfout_hicc<N>` (N is the controller number, 0..127) give per-controller ranges of the CC value 0..127; a missing `*_locc`/`*_hicc` defaults to 0 (so `locc` without `hicc` is an inverted range and an error). The filter exists only when `cutoff` is given, `fil_type` defaults to `lpf_2p`, and `fil_type`/`resonance` without `cutoff` are accepted and mean no filter, but never silently: each is returned in `SfzParseResult::warnings` and the import report lists it as an `ignored_opcode` warning with file:line. Controller numbers must be canonical: `xfin_locc007` is an error (it would spell controller 7 twice). Loop opcodes (`loop_mode` all four values, `loop_start`, `loop_end`) are unchanged from Story 1.13 (defaults are resolved by the importer once the sample length is known).

Mapping rules of Story 13.5 (CC at voice start). Gates: `locc<N>`/`hicc<N>` are CC units 0..127; a gate exists for a controller as soon as either end is given (the other end is sfizz's 0 / 127) and is dropped when it covers 0..127. `hicc` below 127 means "up to, not including, hicc + 1" (sfizz's fill-gap: CC 100 passes `hicc=100`, 101 does not); `lohdcc`/`hihdcc` (and `lorealcc`/`hirealcc`) take 0..1 and have no fill-gap. A region plays only when every gate holds at the part's controller values when the note on (or, for `trigger=release`/`release_key`, the note off) arrives. An inverted gate (`locc` above `hicc`) is `invalid_value`. Values outside the range are clamped and logged (`ignored_values`); a controller above 127 is `unsupported_value` (sfizz never refreshes a gate on a pseudo-controller after load). Modulation: `<target>_oncc<N>` (aliases `<target>_cc<N>`, `ampeg_<eg>cc<N>`) sets the depth, `<target>_curvecc<N>` the curve; the pair is one entry. The value is `curve(controller)` for amplitude (percent/100 x curve, multiplied; a depth of 0 silences, as sfizz's multiplicative connection; a `_curvecc` alone is no entry), volume (dB added; also `gain_*`), pan (percent added), tune (cents added; also `pitch_*`) and the envelope (seconds added, percent for start and sustain, the key deltas 140/141 go through the curve here); for offset it is `int(depth x controller)` frames, no curve (`offset_curvecc<N>` is accepted and ignored, as sfizz, with an `ignored_opcode` warning). For amplitude, volume, pan and tune the key deltas 140/141 are raw semitones, not curved. Depths are clamped to the target's range and logged; an entry of depth 0 is dropped except for amplitude; at most 8 gates and 16 entries per region (`unsupported_value`). Controllers 0..127 and the pseudo-controllers 131, 135, 136, 137, 140, 141 are accepted; 128-130, 132-134, 138, 139 and above 141 are `unsupported_value`. `set_cc<N>`/`set_hdcc<N>`/`set_realcc<N>` are `<control>` only (anywhere else: unsupported opcode). `ampeg_dynamic` is no longer allow-listed: with `ampeg_*_oncc<N>` in the same region it is rejected (`unsupported_value` naming `ampeg_dynamic`), because the importer samples the envelope controllers once when the note starts; `true`, `on` and a non-zero integer set it, any other unparseable value is logged and means off; alone it changes nothing and still imports (every VSCO2 file).

Mapping rules of Story 13.7 (pitch bend and controller-triggered voices). `bend_up` (alias `bendup`) and `bend_down` (`benddown`) are cents at full wheel travel (sfizz defaults 200 and -200, `bend_down` negative bends down; a positive `bend_down` is accepted and bends up at the bottom of the wheel, as sfizz); `bend_step` is a quantum in cents (1..12000, default 1 = none, lower values clamp to 1 and are logged); `bend_smooth` is a ramp time in ms (0..65535, default 0). A region that sets any of the four follows the pitch wheel (the unset ones keep the defaults); a region that sets none is stored without bend and ignores the wheel exactly as before this story (sfizz would bend it by +-200 cents; this is a deliberate difference so every existing instrument renders bit for bit as before, see engine/core/README.md). Values of `bend_up`/`bend_down` outside +-12000 are `unsupported_value` (sfizz keeps them, the importer does not). `lobend`/`hibend` stay unsupported. `on_locc<N>`/`on_hicc<N>` (aliases `start_locc`/`start_hicc`, N 0..127, CC units, `on_hicc` below 127 covers every value from `on_hicc` up to, but not including, `on_hicc` + 1 (so the stored upper end is nextafter(hicc + 1, 0) and the range is inclusive of it)) make a region start a voice from the controller: a region with a trigger must have `trigger=attack` (any other mode is `unsupported_value`: sfizz fires a controller voice as an attack whatever the mode); a trigger exists as soon as either end is given (the other end is 0 / 127), at most 4 per region (`unsupported_value`), an inverted pair is `invalid_value`, a controller above 127 is `unsupported_value`; `on_lohdcc`/`on_hihdcc` are unsupported. A region with a trigger also starts from notes unless `hikey=-1` (the importer then stores `no_note`); `hikey=-1` WITHOUT a trigger can never play and is still dropped before the bundle (`never_note_triggered`, one `ignored_values` line per opcode). The gates (`locc`/`hicc`) of a triggered region are evaluated with the controller's new value, as sfizz. Salamander's pedal-action regions (`lokey=-1 hikey=-1 group=1 on_locc64=127 on_hicc64=127 locc64=127 hicc64=127`) import this way.

Mapping rules of Story 13.8 (per-voice filter). The cutoff of a voice is `clamp(clamp(cutoff * 2^((fil_keytrack * (note - fil_keycenter) + fil_veltrack * velocity / 127) / 1200)) * 2^(sum of cutoff_oncc depth * curve(controller) / 1200))` Hz with each clamp to 0..20000 (sfizz `FilterHolder::setup` and the constant modulation of that instant), the resonance is `resonance + sum of resonance_oncc depth * curve(controller)` dB. `fil_keytrack` is cents per key (0..1200, default 0), `fil_keycenter` a note number or name (default 60) that only matters with a non-zero `fil_keytrack` (without one it is dropped at import, and a stored centre other than 60 beside a zero key track is invalid), `fil_veltrack` cents at velocity 127 (+-12000, default 0; the raw velocity, not the amplitude law). `cutoff_oncc<N>` (aliases `cutoff_cc<N>`, `cutoff1_oncc<N>`, `cutoff2_oncc<N>`; cents, +-12000) and `resonance_oncc<N>` (dB, 0..96) take a controller 0..127 or the pseudo-CCs 131, 135, 136, 137, 140, 141 of Story 13.5 and the CC is read once when the voice starts; `*_curvecc<N>` names a `<curve>` (the curves and `set_cc` values they need are kept by `build_cc_setup` as for Story 13.5); at most 4 entries per target and filter (`unsupported_value`); a curve alone and a zero depth store nothing; out-of-range depths clamp and are logged. The second filter (`cutoff2`, `resonance2`, `fil2_type`, `fil2_keytrack`, `fil2_keycenter`, `fil2_veltrack`, `cutoff2_oncc<N>`, `resonance2_oncc<N>`) runs in series after the first and exists only when `cutoff2` is given; `cutoff2` on a region without `cutoff` is `unsupported_value` (sfizz would run the missing first filter at cutoff 0). As for Story 5.1, every filter opcode of a region without `cutoff` (or `fil2_*` without `cutoff2`) is accepted, means no such filter and is returned as an `ignored_opcode` warning. `fil_type` values stay lpf_1p, lpf_2p, hpf_1p, hpf_2p, bpf_2p. Rejected as before: `fil_random`/`cutoff_random`, `fil_gain`, `fil_veltrack_oncc<N>`, `cutoff*_stepcc`/`_smoothcc`, `cutoff*_chanaft`/`_polyaft`, a third filter, other filter types, `fileg_*`, `fillfo_*`, `egNN_cutoff`, `lfoNN_cutoff` (Story 13.9).

Mapping rules of Story 13.6 (choke groups and polyphony). The model is sfizz 1.2.3's (`Synth::checkOffGroups`, `VoiceManager::checkPolyphony`, `Voice::off`). `group` (alias `polyphony_group`) is an integer of the region (default 0; inherited like any opcode, so a `<group>` header with `group=2` gives all its regions group 2). `off_by` (`offby`) names the group whose starting voices cut this region's voices. `off_mode` (`offmode`) is `fast` (the default: the cut uses a 6 ms exponential release), `normal` (the voice's own `ampeg_release`) or `time`; `off_time` implies `time` (sfizz), a later `off_mode` replaces it, `off_mode=time` without `off_time` is 6 ms, and times below 6 ms behave as 6 ms. `polyphony` (1..255; 256 is sfizz's default, no limit; 0 is `unsupported_value`) is the maximum number of voices of its region and, with a non-zero group, of the whole group (sfizz `setGroupPolyphony`: the last assignment for a group number wins for all its regions). `note_polyphony` (1..255) limits the voices of the group that were started by the same note; `note_selfmask` (`on` default, `off`): with `on` a retrigger only releases same-note voices that are not louder than itself (so a quieter retrigger does not cut a louder note), with `off` the oldest goes. A `polyphony` in a `<global>`, `<master>` or `<group>` header is also a limit on all the regions under that header (sfizz `RegionSet`); that set limit is not represented. It is imported when the region has a non-zero `group` set at the same header or a broader one (every region under the header is then in that group, or the file is rejected through the region that leaves it): sfizz limits the group with the same value from the flattened region, the set is part of that group, and the group's limit stands for both (they differ only when the group also has regions under another header with a different final limit). That is the Salamander pedal-noise pattern (`<global> group=1 polyphony=1`). Without such a group the file is rejected as `unsupported_value` on `polyphony` (file:line of the opcode). Values: `group`/`off_by` beyond the int32 range are `unsupported_value`; an unknown `off_mode` or `note_selfmask` is `unsupported_value`; `polyphony`/`note_polyphony` outside 1..256 are ignored (`ignored_value`, no limit, as sfizz enforces the bounds). `output` stays unsupported (audio routing to other buses; the importer is stereo-only).

<!-- sfz-opcode-table:begin -->
| Opcode | Class | Notes |
|---|---|---|
| `sample` | supported | required in every region |
| `default_path` | supported | <control> only |
| `lokey` | supported | note number or name |
| `hikey` | supported |  |
| `key` | supported | lokey = hikey = pitch_keycenter; invalid: defaults, logged (Story 13.2) |
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
| `trigger` | supported | attack, release, first, legato, release_key (Story 13.3) |
| `end` | supported | frames, exclusive: last played frame = min(end, length) - 1; 0 disables the region (Story 13.3) |
| `amp_veltrack` | supported | percent -100..100 (Story 13.4) |
| `amp_velcurve_<N>` | supported | N = velocity 0..127, gain 0..1, linear between points, at most 32 points per region (Story 13.4) |
| `amplitude` | supported | percent 0..10000 (Story 13.4) |
| `global_amplitude` | supported | percent, multiplies amplitude (Story 13.4) |
| `master_amplitude` | supported | percent, multiplies amplitude (Story 13.4) |
| `group_amplitude` | supported | percent, multiplies amplitude (Story 13.4) |
| `global_volume` | supported | dB, added to volume (Story 13.4) |
| `master_volume` | supported | dB, added to volume (Story 13.4) |
| `group_volume` | supported | dB, added to volume (Story 13.4) |
| `amp_keytrack` | supported | dB per key -96..12 (Story 13.4) |
| `amp_keycenter` | supported | note number or name (Story 13.4) |
| `ampeg_vel2attack` | supported | s at velocity 127 (Story 13.4) |
| `ampeg_vel2decay` | supported | s at velocity 127 (Story 13.4) |
| `ampeg_vel2sustain` | supported | % at velocity 127 (Story 13.4) |
| `ampeg_vel2release` | supported | s at velocity 127 (Story 13.4) |
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
| `filtype` | supported | alias of fil_type (Story 13.8) |
| `fil1_type` | supported | alias of fil_type (Story 13.8) |
| `fil2_type` | supported | type of the second filter, which exists only when cutoff2 is given (Story 13.8) |
| `cutoff1` | supported | alias of cutoff (Story 13.8) |
| `cutoff2` | supported | Hz, the second filter, in series after the first; needs a first filter (Story 13.8) |
| `resonance1` | supported | alias of resonance (Story 13.8) |
| `resonance2` | supported | dB, the second filter (Story 13.8) |
| `fil_keytrack` | supported | cents per key 0..1200 from fil_keycenter (Story 13.8) |
| `fil1_keytrack` | supported | alias of fil_keytrack (Story 13.8) |
| `fil2_keytrack` | supported | fil_keytrack of the second filter (Story 13.8) |
| `fil_keycenter` | supported | note number or name, default 60 (Story 13.8) |
| `fil1_keycenter` | supported | alias of fil_keycenter (Story 13.8) |
| `fil2_keycenter` | supported | fil_keycenter of the second filter (Story 13.8) |
| `fil_veltrack` | supported | cents at velocity 127, -12000..12000 (Story 13.8) |
| `fil1_veltrack` | supported | alias of fil_veltrack (Story 13.8) |
| `fil2_veltrack` | supported | fil_veltrack of the second filter (Story 13.8) |
| `cutoff_oncc<N>` | supported | cents added at voice start, -12000..12000; N = controller 0..127 or pseudo-CC 131, 135, 136, 137, 140, 141 (Story 13.8) |
| `cutoff_cc<N>` | supported | alias of cutoff_oncc<N> (Story 13.8) |
| `cutoff_curvecc<N>` | supported | curve index (Story 13.8) |
| `cutoff1_oncc<N>` | supported | alias of cutoff_oncc<N> (Story 13.8) |
| `cutoff1_cc<N>` | supported | alias of cutoff_oncc<N> (Story 13.8) |
| `cutoff1_curvecc<N>` | supported | curve index (Story 13.8) |
| `cutoff2_oncc<N>` | supported | cutoff_oncc<N> of the second filter (Story 13.8) |
| `cutoff2_cc<N>` | supported | alias of cutoff2_oncc<N> (Story 13.8) |
| `cutoff2_curvecc<N>` | supported | curve index (Story 13.8) |
| `resonance_oncc<N>` | supported | dB added at voice start, 0..96; N as above (Story 13.8) |
| `resonance_cc<N>` | supported | alias of resonance_oncc<N> (Story 13.8) |
| `resonance_curvecc<N>` | supported | curve index (Story 13.8) |
| `resonance1_oncc<N>` | supported | alias of resonance_oncc<N> (Story 13.8) |
| `resonance1_cc<N>` | supported | alias of resonance_oncc<N> (Story 13.8) |
| `resonance1_curvecc<N>` | supported | curve index (Story 13.8) |
| `resonance2_oncc<N>` | supported | resonance_oncc<N> of the second filter (Story 13.8) |
| `resonance2_cc<N>` | supported | alias of resonance2_oncc<N> (Story 13.8) |
| `resonance2_curvecc<N>` | supported | curve index (Story 13.8) |
| `label_cc<N>` | allow-listed | controller name |
| `label_key<N>` | allow-listed | key name |
| `global_label` | allow-listed |  |
| `master_label` | allow-listed |  |
| `group_label` | allow-listed |  |
| `region_label` | allow-listed |  |
| `sw_label` | allow-listed | keyswitch name |
| `image` | allow-listed | instrument picture |
| `hint_*` | allow-listed | editor hints |
| `locc<N>` | supported | N = controller 0..127, CC units; the region plays while the part's CC value is in [locc, hicc] (Story 13.5) |
| `hicc<N>` | supported | hicc below 127 covers values up to hicc + 1, exclusive (sfizz fill-gap) (Story 13.5) |
| `lohdcc<N>` | supported | normalized 0..1 (Story 13.5) |
| `hihdcc<N>` | supported | normalized 0..1 (Story 13.5) |
| `lorealcc<N>` | supported | alias of lohdcc<N> (Story 13.5) |
| `hirealcc<N>` | supported | alias of hihdcc<N> (Story 13.5) |
| `set_cc<N>` | supported | <control> only: the part's initial value of controller N (0..127) (Story 13.5) |
| `set_hdcc<N>` | supported | <control> only, normalized 0..1 (Story 13.5) |
| `set_realcc<N>` | supported | alias of set_hdcc<N> (Story 13.5) |
| `amplitude_oncc<N>` | supported | percent 0..10000, gain x (depth/100 x curve); N = controller 0..127 or pseudo-CC 131, 135, 136, 137, 140, 141 (Story 13.5) |
| `amplitude_cc<N>` | supported | alias of amplitude_oncc<N> (Story 13.5) |
| `amplitude_curvecc<N>` | supported | curve index: a <curve> block or a predefined curve 0..6 (Story 13.5) |
| `volume_oncc<N>` | supported | dB; N = controller 0..127 or pseudo-CC 131, 135, 136, 137, 140, 141 (Story 13.5) |
| `volume_cc<N>` | supported | alias of volume_oncc<N> (Story 13.5) |
| `volume_curvecc<N>` | supported | curve index: a <curve> block or a predefined curve 0..6 (Story 13.5) |
| `gain_oncc<N>` | supported | dB, alias of volume; N = controller 0..127 or pseudo-CC 131, 135, 136, 137, 140, 141 (Story 13.5) |
| `gain_cc<N>` | supported | alias of gain_oncc<N> (Story 13.5) |
| `gain_curvecc<N>` | supported | curve index: a <curve> block or a predefined curve 0..6 (Story 13.5) |
| `pan_oncc<N>` | supported | percent; N = controller 0..127 or pseudo-CC 131, 135, 136, 137, 140, 141 (Story 13.5) |
| `pan_cc<N>` | supported | alias of pan_oncc<N> (Story 13.5) |
| `pan_curvecc<N>` | supported | curve index: a <curve> block or a predefined curve 0..6 (Story 13.5) |
| `tune_oncc<N>` | supported | cents; N = controller 0..127 or pseudo-CC 131, 135, 136, 137, 140, 141 (Story 13.5) |
| `tune_cc<N>` | supported | alias of tune_oncc<N> (Story 13.5) |
| `tune_curvecc<N>` | supported | curve index: a <curve> block or a predefined curve 0..6 (Story 13.5) |
| `pitch_oncc<N>` | supported | cents, alias of tune; N = controller 0..127 or pseudo-CC 131, 135, 136, 137, 140, 141 (Story 13.5) |
| `pitch_cc<N>` | supported | alias of pitch_oncc<N> (Story 13.5) |
| `pitch_curvecc<N>` | supported | curve index: a <curve> block or a predefined curve 0..6 (Story 13.5) |
| `offset_oncc<N>` | supported | frames: depth x CC value, no curve; N = controller 0..127 or pseudo-CC 131, 135, 136, 137, 140, 141 (Story 13.5) |
| `offset_cc<N>` | supported | alias of offset_oncc<N> (Story 13.5) |
| `offset_curvecc<N>` | supported | accepted and ignored, as sfizz: offset is not curve-modulated (ignored_opcode warning) (Story 13.5) |
| `ampeg_delay_oncc<N>` | supported | s, added at voice start; N as above (Story 13.5) |
| `ampeg_delay_cc<N>` | supported | alias of ampeg_delay_oncc<N> (Story 13.5) |
| `ampeg_delaycc<N>` | supported | SFZ 1 alias of ampeg_delay_oncc<N> (Story 13.5) |
| `ampeg_delay_curvecc<N>` | supported | curve index (Story 13.5) |
| `ampeg_start_oncc<N>` | supported | %, added at voice start; N as above (Story 13.5) |
| `ampeg_start_cc<N>` | supported | alias of ampeg_start_oncc<N> (Story 13.5) |
| `ampeg_startcc<N>` | supported | SFZ 1 alias of ampeg_start_oncc<N> (Story 13.5) |
| `ampeg_start_curvecc<N>` | supported | curve index (Story 13.5) |
| `ampeg_attack_oncc<N>` | supported | s, added at voice start; N as above (Story 13.5) |
| `ampeg_attack_cc<N>` | supported | alias of ampeg_attack_oncc<N> (Story 13.5) |
| `ampeg_attackcc<N>` | supported | SFZ 1 alias of ampeg_attack_oncc<N> (Story 13.5) |
| `ampeg_attack_curvecc<N>` | supported | curve index (Story 13.5) |
| `ampeg_hold_oncc<N>` | supported | s, added at voice start; N as above (Story 13.5) |
| `ampeg_hold_cc<N>` | supported | alias of ampeg_hold_oncc<N> (Story 13.5) |
| `ampeg_holdcc<N>` | supported | SFZ 1 alias of ampeg_hold_oncc<N> (Story 13.5) |
| `ampeg_hold_curvecc<N>` | supported | curve index (Story 13.5) |
| `ampeg_decay_oncc<N>` | supported | s, added at voice start; N as above (Story 13.5) |
| `ampeg_decay_cc<N>` | supported | alias of ampeg_decay_oncc<N> (Story 13.5) |
| `ampeg_decaycc<N>` | supported | SFZ 1 alias of ampeg_decay_oncc<N> (Story 13.5) |
| `ampeg_decay_curvecc<N>` | supported | curve index (Story 13.5) |
| `ampeg_sustain_oncc<N>` | supported | %, added at voice start; N as above (Story 13.5) |
| `ampeg_sustain_cc<N>` | supported | alias of ampeg_sustain_oncc<N> (Story 13.5) |
| `ampeg_sustaincc<N>` | supported | SFZ 1 alias of ampeg_sustain_oncc<N> (Story 13.5) |
| `ampeg_sustain_curvecc<N>` | supported | curve index (Story 13.5) |
| `ampeg_release_oncc<N>` | supported | s, added at voice start; N as above (Story 13.5) |
| `ampeg_release_cc<N>` | supported | alias of ampeg_release_oncc<N> (Story 13.5) |
| `ampeg_releasecc<N>` | supported | SFZ 1 alias of ampeg_release_oncc<N> (Story 13.5) |
| `ampeg_release_curvecc<N>` | supported | curve index (Story 13.5) |
| `group` | supported | integer, the region's choke / polyphony group (default 0); a `<group>` header is unaffected (Story 13.6) |
| `polyphony_group` | supported | alias of group (Story 13.6) |
| `off_by` | supported | integer: the voices of every group with this number are cut when this region starts a voice (Story 13.6) |
| `offby` | supported | alias of off_by (Story 13.6) |
| `off_mode` | supported | fast (6 ms, the default), normal (the voice's own ampeg_release), time (off_time) (Story 13.6) |
| `offmode` | supported | alias of off_mode (Story 13.6) |
| `off_time` | supported | s 0..100, implies off_mode=time (sfizz); times below 6 ms behave as 6 ms (Story 13.6) |
| `polyphony` | supported | 1..255 voices; in <region>, or in a header together with a non-zero group set at that header or a broader one (the group's limit); otherwise a region-set limit, rejected (Story 13.6) |
| `note_polyphony` | supported | 1..255 voices of the group for one note (Story 13.6) |
| `note_selfmask` | supported | on or mask (default: a quieter retrigger does not cut a louder voice) or off (Story 13.6) |
| `ampeg_dynamic` | supported | flag; rejected together with ampeg_*_oncc<N> (continuous envelope modulation is not implemented), otherwise no effect (Story 13.5) |
| `bend_up` | supported | cents -12000..12000 at full wheel travel up (default 200); setting any bend opcode makes the region follow the pitch wheel (Story 13.7) |
| `bendup` | supported | alias of bend_up (Story 13.7) |
| `bend_down` | supported | cents -12000..12000 at full wheel travel down (default -200; a positive value bends up at the bottom of the wheel) (Story 13.7) |
| `benddown` | supported | alias of bend_down (Story 13.7) |
| `bend_step` | supported | cents 1..12000: the bend is quantised to multiples of this (default 1 = none) (Story 13.7) |
| `bend_smooth` | supported | ms 0..65535: length of the linear ramp to a new wheel position (default 0) (Story 13.7) |
| `on_locc<N>` | supported | N = controller 0..127, CC units: the region starts a voice when the controller moves into [on_locc, on_hicc] (Story 13.7) |
| `on_hicc<N>` | supported | on_hicc below 127 covers values up to on_hicc + 1, exclusive (sfizz fill-gap) (Story 13.7) |
| `start_locc<N>` | supported | alias of on_locc<N> (Story 13.7) |
| `start_hicc<N>` | supported | alias of on_hicc<N> (Story 13.7) |
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

## Protected bundles (Story 6.2: AR27, NFR13, FR30/FR31 format)

Story 6.3: `tools/content-pack` is the producer of these bundles (it feeds the librarian importer's verified `BundleWriter` into `finalize_protected`); `BundleWriter::nonce_from_plain_digest` is the nonce derivation of `derive_nonce()` from an already computed digest of the unprotected bundle; `uuid_from_string` (hash.hpp) parses the vendor `library_id` it passes to `BundleWriter::set_library_id`.

The protected variant of the block-indexed bundle is **format_version 3**. It is a separate major version, not a flag on version 2: the unprotected reader
(`BundleReader`) answers a version 3 file with `kUnsupportedVersion` exactly as it does for any version it does not know, every existing unprotected bundle
(versions 1 and 2, minors 0 and 1) is byte-identical to before and keeps its `bundle_id`, and the protected readers answer an unprotected bundle with
`kNotProtected`. The version word is `3 | minor << 16`; minor 1 means the same extension table as format 2.1 (keyswitches, crossfades, filters) and is stamped only
when a region needs it; minors above 3 (minor 2 is the same trigger/end table as format 2.2, minor 3 the amp table of format 2.3) and majors other than 3 are `kUnsupportedVersion`. The instrument/sample/region manifest body, the block size
(`kBlockFrames`), the FLAC/raw-float block coding and the index entry are those of format 2 unchanged; only the items below are new.

### Layout (little-endian; all fields byte-exact)

```
header   128 B   as format 2: magic "VLRB" | u32 version word | u64 manifest_off (=128), manifest_size, index_off, index_size, data_off, data_size, total_size |
                 manifest_sha256[32] @64 | index_sha256[32] @96. Strict layout, no gaps: data_off = index_off + index_size + 128, total_size = file size.
manifest         format 2 manifest with a protection section after the index hash (offsets from the manifest start):
   0   u32  version word (equals the header's)          68  u8   cipher = 1 (AES-256-CTR)
   4   [16] library_id (vendor-assigned UUID)           69  u8   hash_list_alg = 1 (Merkle SHA-256)
   20  u32  instrument count                            70  u8[2] reserved, zero
   24  u32  sample count                                72  [8]  nonce (per bundle, unique per content key)
   28  u64  block count (<= 2^32)                       80  [16] key_check
   36  [32] index_sha256                                96  [32] hash_list_root
   128 ... instruments, samples, region tables (format 2 / 2.1 body, unchanged)
index    48 B per block: u64 offset | u32 size | u32 frames | [32] SHA-256 of the CIPHERTEXT bytes of the block. size <= kMaxProtectedBlockBytes (1 MiB).
seal     128 B: "VSL1" | u8 alg = 1 (Ed25519) | u8[3] zero | [8] signing key id | [64] Ed25519 signature | u8[48] zero
blocks   ciphertext, in global block order (instrument-major, sample, block), contiguous
```

* `bundle_id` = SHA-256 of the manifest bytes (as before); it covers the nonce, key check, hash-list root and the index hash. `library_id` is the vendor id the
  content-pack tool assigns (`BundleWriter::set_library_id`).
* **Encryption**: each block is encrypted independently with AES-256-CTR under the per-library content key. The 16-byte counter block of byte 0 of global block `b`
  is `nonce[8] || BE32(b) || BE32(0)` (`counter_block()`); CTR then increments the whole 128-bit counter. A block is capped at `kMaxProtectedBlockBytes` (1 MiB =
  2^16 AES blocks), the writer and the reader assert it, so the low 32 bits never carry into the block-index field and no counter block is reused inside a
  key, as long as the nonce is unique per content key and the bundle has fewer than 2^32 blocks (both enforced or documented below). The nonce is chosen by
  the caller; `BundleWriter::derive_nonce()` derives it from the content (first 8 bytes of SHA-256("velora/protected-nonce/v1" 0x00 || SHA-256(plain bundle))), so revisions of a
  library under one key get different nonces. Random access: block `b` is decrypted by itself, no other block is touched.
* **Hashes**: the index SHA-256 is over the ciphertext, so a block is verified before any decryption. `hash_list_root` is a Merkle root over those block hashes
  in global order: leaf = SHA-256(0x00 || block_hash), node = SHA-256(0x01 || left || right), an odd node at the end of a level is promoted unchanged, the root of an
  empty list is SHA-256 of the empty string. The root is checked against the index at open.
* **Key check**: `key_check` = first 16 bytes of HMAC-SHA256(content_key, "velora/key-check/v1" || 0x00 || nonce). A wrong key (or a wrong nonce)
  is `kWrongContentKey` at open, never garbage PCM. Block plaintext additionally has to pass the FLAC frame CRCs (`kCorruptBlock`); float blocks are raw words and are checked by size only, so a signed bundle encrypted under a different key than its key check claims would decode to noise for float samples (such a bundle can only be produced by a holder of the signing key).
* **Signature**: pure Ed25519 over the statement `"velora/protected-manifest/v1" 0x00 || u32 version_word || library_id[16] || bundle_id[32] || index_sha256[32] ||
  hash_list_root[32] || u64 block_count || nonce[8] || signing_key_id[8]`. The signing key id is the first 8 bytes of SHA-256(public key), the id format of the trusted-key lists of
  `tools/keys/KEY-HIERARCHY.md`. Verification follows the strictness of its section 6 (S < L, canonical R and A, small-order A rejected, cofactorless) on top of
  OpenSSL's check, and additionally rejects a small-order R. Tests carry vectors that plain OpenSSL accepts (non-canonical and small-order A with R = identity and S = 0, a canonical A with
  small-order R) and assert that `ed25519_verify_strict` rejects them, plus the S + L case and the RFC 8032 vectors.
* Open order and typed errors: header layout (`kBadMagic`, `kTruncated`, `kUnsupportedVersion`, `kNotProtected`, `kCorruptHeader`), manifest and index hashes
  (`kCorruptManifest`, `kCorruptIndex`), fixed manifest fields and seal, **trusted-key lookup** (`kUntrustedKey`; Story 6.4 does it right after the header and the seal, before the manifest and the index are read), **signature** (`kSignatureInvalid`), the rest of the manifest
  and the index structure, **hash-list root** (`kHashListMismatch`), then, in the decrypting reader only, the key check (`kWrongContentKey`). A trusted-key list that names one key id twice never trusts it. Reading a block
  hashes the ciphertext first (`kBlockHashMismatch`, no bytes returned and the caller's output untouched), then decrypts and decodes. Tests flip every byte of the header, manifest,
  index and seal, three bytes of every block and every byte of the smallest block, assert the exact error per byte class, and cut the file at every structural boundary; none opens or yields PCM.

### Targets (who may link what)

| Target | Contents | Linked by |
|---|---|---|
| `velora_format` | the container, hashing, HMAC, Merkle root, key check, statement bytes: **no cipher, no signing** | everything |
| `velora_format_verify` | `ProtectedBundleView`, `ed25519_verify_strict`, `TrustedKeys`: signature, hash list and ciphertext block-hash verification, **no decrypt** | desktop build, librarian (Story 6.4: `install_protected`, `describe_bundle`, reconcile) |
| `velora_format_decrypt` | `ProtectedBundleReader` (AES-256-CTR decrypt, FLAC decode of only the blocks read) on top of verify | engine, offline-render, tests |
| `velora_format_protected_writer` | `finalize_protected`, `ed25519_sign`, `aes256_ctr_xor` | tools/content-pack (Story 6.3), tests; never shipped |

"No decrypt path" means the code never CALLS the cipher API: no cipher/decrypt symbol is defined or imported by the verify target. The shared libcrypto it links is a system library and does contain AES. The core holds only data-layout and hashing helpers (`counter_block`, `hmac_sha256`, `key_check_value`, `hash_list_root`, `statement_bytes`), none of which calls a cipher.

`tools/ci/check_format_no_decrypt.py` (ctest `format_no_decrypt`, `tools/ci/docker-check.sh`, the CI purity job with `--self-test`) scans the symbols (`nm`) of
`velora_format_verify`, `velora_format`, the verify-only probe binary (`format_verify_probe`) and the librarian binaries and libraries, and fails on any OpenSSL
cipher entry point (`EVP_aes_*`, `EVP_Decrypt*`, `EVP_Encrypt*`, `EVP_CIPHER_*`, `AES_*`, CommonCrypto, mbedTLS, libsodium, ...) or project decrypt symbol, and, as an allowlist, on any
undefined crypto-library symbol (`EVP_`, `OPENSSL_`, `CRYPTO_`, `ERR_`, `BIO_`, ...) other than the six Ed25519 verification calls (version suffixes such as `@OPENSSL_3.0.0` are stripped first);
`format_no_decrypt_detects_decrypt` (`--expect-flagged`) succeeds only if the scanner flags `EVP_DecryptInit_ex`/`EVP_EncryptInit_ex`/`EVP_aes_256_ctr` in the decrypt and writer libraries. `tools/ci/check_librarian_no_crypto.py` now allows only the Ed25519 verification entry points of OpenSSL (`EVP_PKEY_new_raw_public_key`, `EVP_PKEY_free`, `EVP_MD_CTX_new/free`, `EVP_DigestVerifyInit`, `EVP_DigestVerify`; and a shared libcrypto) and
still forbids every cipher/decrypt symbol and static libcrypto.

**catalog.meta binding and pack revision (Story 6.4).** `catalog.meta` (library name, instrument names and categories, the librarian's display data) is not in the signed manifest, so a pack carries `catalog.meta.sig`, a detached 84-byte seal: `"VCM2"` | u8 alg = 1 | u8[3] zero | u32 `pack_revision` (LE) | signer key id[8] | Ed25519 signature[64] over `"velora/catalog-meta/v1" 0x00 || bundle_id[32] || SHA-256(catalog.meta)[32] || signer key id[8] || pack_revision (u32 LE)`, made with the bundle's own signing key (`sign_catalog_meta` in the writer target, `verify_catalog_meta_seal` in the verify target; `catalog_meta_statement` / `encode_catalog_meta_seal` / `decode_catalog_meta_seal` in the core). `bundle_id` in the statement ties the seal to one bundle; a malformed seal (or revision 0) is `kCatalogMetaInvalid`, an unknown signer `kUntrustedKey`, a signature that does not verify `kSignatureInvalid`. `pack_revision` (>= 1) is a monotonic per-library counter the vendor raises with every release: an installer accepts a revision only if it is not lower than the installed one, and an equal one only for the same bundle - replaying an older signed bundle is a downgrade; a deliberate rollback is a new, higher revision with the old content. The librarian refuses to install a pack whose `catalog.meta` does not verify against it. **Bounds before allocation:** `parse_bundle` refuses a manifest above 64 MiB or (protected) an index above 512 MiB (`kCorruptHeader`) before allocating, and reads the seal first: the signer's key id is checked against the trust list before the manifest and the index are read and hashed (so an untrusted signer is `kUntrustedKey` even when the rest of the file is damaged).

**RTSan / threading.** Decrypt runs on non-real-time threads only: `ProtectedBundleView`/`ProtectedBundleReader` open, `read_block`, `read_frames` and `verify_all` allocate,
do file I/O, hash and run AES, and are documented NOT real-time safe (like `BundleReader::read_block`); the engine's streaming threads call them, never the audio callback.
Const methods are thread-safe with one `BlockDecoder` per thread. The reader keeps a copy of the content key and wipes it on destruction.

**Nonce.** `ProtectParams::nonce`/`finalize_protected` require a nonce that is unique per content key and never all zero (zero is `kInvalidArgument`); a repeat under one key reuses the keystream. With 64-bit nonces keep far below 2^32 bundles per key (birthday bound ~n^2/2^65). `finalize_protected(writer, key, seed)` uses the content-derived nonce. `finalize_protected_to_file` writes through an `O_EXCL|O_NOFOLLOW` temp file (mode 0644), fsyncs it and the directory.

**Writer API.** `BundleWriter::finalize_protected(ProtectParams)` (core, callbacks for encrypt and sign) builds the bytes; tools call
`finalize_protected(writer, ContentKey, Nonce, SigningSeed)` from `velora/format/protected_writer.hpp`. Same writer content, key, nonce and signing seed give identical
bytes (Ed25519 is deterministic). Key generation, wrapping and the vault are Stories 6.1 / 6.3 / 6.8; trusted keys come from `velora_trusted_keys_<variant>.hpp` via `TrustedKeys::from_generated`.

Tests: `format_protected_test` (HMAC, AES-CTR, Ed25519 and Merkle known answers; counter layout; round trip of 16/24-bit and float samples; 10,000 deterministic random seeks
bit-identical to the source; reproducibility; the tamper matrix above; untrusted keys, wrong content key, nonce change with and without re-signing, unknown versions, the block-size cap),
`format_verify_probe` (a binary that links only the verify target), `format_no_decrypt*`.

Story 6.5 adds `ProtectedBundleReader::open_view(shared_ptr<const ProtectedBundleView>, span<const uint8_t, 32> key, shared_ptr<const void> key_owner)` and `shared_view()`: the engine opens the reader over a view the library resolver already verified, and the reader **borrows** the content key from the caller's memory (the engine's locked, non-dumpable key page, which `key_owner` keeps alive) instead of copying it; a key that fails the manifest's key check is `kWrongContentKey`. The three `open_*` entry points keep their private, wiped copy. `ProtectedBundleReader` keeps its view by `shared_ptr` so several holders (the resolver's `ResolveResult` and the reader) share one open descriptor.
