#pragma once
// SoundFont 2 -> instrument model (Story 5.3). A clean-room reader written from the public
// SoundFont 2.04 specification (RIFF `sfbk`: INFO, sdta with `smpl` and the optional `sm24`, pdta
// with phdr/pbag/pmod/pgen/inst/ibag/imod/igen/shdr). No third-party SF2 code is used or linked.
//
// What a file becomes
//   * every preset becomes one instrument, `instrument_key` = "<bank>:<preset>" in decimal without
//     leading zeros (`0:0`, `128:0` for percussion), the instrument name is the preset name. The
//     key must be unique in the file: a repeated bank:preset is damage.
//   * every (preset zone x instrument zone) pair that has a `sampleID` becomes one Region, in
//     file order (preset zone order, then instrument zone order). Global zones are folded in
//     (instrument generators: zone value, else instrument global, else the specification default;
//     preset generators: zone value, else preset global, else 0; the preset value is ADDED to the
//     instrument value for the additive generators, instrument-only generators are not accepted at
//     preset level). `keyRange`/`velRange` of both levels are intersected; an empty intersection
//     drops that zone pair silently (it can never sound). Range generators are honoured wherever
//     they appear in a zone (the specification wants them first; readers in the wild do not care).
//     A zone after the first without its terminal generator (`instrument`/`sampleID`) is ignored.
//   * samples: one bundle sample per `shdr` record actually referenced by a preset, mono, at the
//     source rate and bit depth (16-bit `smpl`, 24-bit when a valid `sm24` is present: the value is
//     (smpl word << 8) | sm24 byte), bit-identical to the file, never resampled or requantised. A
//     bundle holds the samples per instrument, so a sample shared by several presets is stored once
//     per preset (documented gap: shared samples are duplicated across presets).
//
// Mapping (units are the SF2 generator units of section 8.1.2)
//   key/velocity range, root key (`overridingRootKey`, else shdr `byOriginalPitch`, 255 or out of
//   range = 60), `coarseTune` (semitones -> Region::transpose), `fineTune` + shdr `pitchCorrection`
//   (cents -> tune), `sampleModes` (0/2 no loop, 1 loop_continuous, 3 loop_sustain; the loop points
//   are the shdr points plus the `startloop`/`endloop` offsets and their coarse variants, relative
//   to the sample start; SF2's end point is the first frame AFTER the loop, so the inclusive model
//   `loop_end` is endloop - 1), `startAddrsOffset` (+ coarse, 32768 frames) -> Region::offset,
//   `initialAttenuation` (cB -> volume dB), `pan` (0.1 % -> -100..100), the volume envelope
//   (`delayVolEnv`, `attackVolEnv`, `holdVolEnv`, `decayVolEnv`, `releaseVolEnv` timecents ->
//   seconds, `sustainVolEnv` cB of attenuation -> percent), `initialFilterFc` (absolute cents ->
//   Hz, `8.176 * 2^(c/1200)`; 13500 and above = no filter) and `initialFilterQ` (cB, see below) into a
//   2-pole low-pass.
//   Engine conventions compensated here: the sample engine's pan law gives a mono sample +3.01 dB at
//   every position against the SF2 law, so every region carries -3.0103 dB (`kSf2PanLawDb`); its
//   envelope times are "nine time constants" (decay/release reach -78.2 dB in `t`), SF2's are the
//   time for a 100% change of the volume envelope, whose full scale is 1000 cB = 100 dB (section 8.1.3: "1000
//   indicates full attenuation"), so decay and release are multiplied by `kSf2NineTau` (0.7817 = 9 time
//   constants / 100 dB).
//   Filter: the model's 2-pole low-pass is the RBJ cookbook biquad whose gain at the cutoff is its Q and
//   whose DC gain is 1. `initialFilterQ` is the height of the peak over a non-resonant filter (Q 0 means
//   Butterworth, 0.707, i.e. -3 dB at the cutoff), so the model's resonance is `q/10 - 3.01` dB, clamped to the
//   model's 0..40: peaks below 3.01 dB are played with Q = 1 (documented gap: a Butterworth Q is not
//   representable). The specification's gain reduction of half the peak height is applied to the whole region:
//   volume -= q/20 dB (also when the cutoff is at its 13500 maximum and no filter is created).
//   Stereo: SF2 stereo samples are two mono samples linked by `sampleLink` (left type 4, right type 2). Each
//   stays one mono bundle sample played with the zone's own `pan` (stereo fonts carry -500 / +500 there);
//   a left or right sample whose zone pan is 0 is played hard panned (left -100, right +100). `sampleLink` is
//   validated (in range) but otherwise unused; type "linked" (8) is played as mono.
//
// Features outside the model are IGNORED, never a reason to skip the preset: every non-default
// modulator (pmod/imod entry that is not one of the ten default modulators of section 8.4),
// LFOs (`modLfoToPitch`, `vibLfoToPitch`, `modLfoToFilterFc`, `modLfoToVolume`, `delayModLFO`,
// `freqModLFO`, `delayVibLFO`, `freqVibLFO`), the modulation envelope (`modEnvToPitch`,
// `modEnvToFilterFc`, `delay/attack/hold/decay/sustain/releaseModEnv`, `keynumToModEnvHold/
// Decay`), `keynumToVolEnvHold/Decay`, `chorusEffectsSend`, `reverbEffectsSend`, `keynum`,
// `velocity`, `scaleTuning` other than 100, `exclusiveClass` other than 0, `endAddrsOffset` /
// `endAddrsCoarseOffset` other than 0 (the model cannot cut a sample short), generators with an
// unknown number and instrument-only generators at preset level. Each
// is listed once per preset (with the number of zones it was seen in) as an `Sf2Warning`, the
// librarian reports them with the code `ignored_sf2_feature`. A generator counts as ignored only
// when its effective value differs from the specification default and the zone produced a region.
// The default modulator set is not reported, with ONE exception: velocity -> attenuation is the
// engine's velocity curve; channel pressure / CC1 -> vibrato, CC7/CC10/CC11, the reverb/chorus sends
// and the pitch wheel have no counterpart in the engine and are not reported; but the default
// velocity -> initialFilterFc modulator (-2400 cents at velocity 0) is dropped, and since it changes
// the sound of every filtered preset (the model's cutoff is fixed: filtered presets sound brighter
// than SF2 players at velocity < 127) it is listed once per preset that has a filter.
//
// Two more kinds of warning exist besides `ignored_sf2_feature`:
//   * `ignored_zone`: a preset-zone x instrument-zone pair whose values the model cannot hold (a start
//     offset or loop outside its sample, a region that fails validate_region) is DROPPED with a
//     warning (preset id, reason, number of zones); the rest of the file imports.
//   * `ignored_empty_preset`: a preset that ends with no region is not published as an instrument (an
//     empty instrument could only fail at engine load); it is listed with that code.
// End offsets (`endAddrsOffset`/`endAddrsCoarseOffset`) are NOT applied: the model has no sample end, so
// the sample is kept whole and the generators are reported as ignored. Start offset and the four loop
// offset generators are applied and validated.
//
// Damage (Sf2IssueKind::kDamaged) is structural: bad RIFF/LIST/chunk sizes (a chunk reaching past its
// container or the file; fewer than 8 bytes of slack inside a container are tolerated), a missing
// required chunk (INFO/ifil, sdta, pdta and the nine tables) or a repeated one, a table whose size is
// not a multiple of its record size or that has no terminal record, a terminal record that does not
// index the terminal of the next table, bag/generator/modulator/instrument/sample indexes that are out
// of range or go backwards, `sm24` of the wrong size, a duplicate bank:preset, and, for REFERENCED
// samples only, `shdr` start > end or points outside `smpl`, an empty sample, an invalid type or an
// out-of-range link (unreferenced sample headers are never examined; an odd `smpl` byte size loses its
// last byte), and more than kMaxZoneCombos zone pairs / kMaxRegions regions / kMaxInstrumentZones
// instrument zones. Unsupported (kUnsupported, reason code unsupported_features): `ifil` major
// version other than 2, a referenced ROM sample (sampleType bit 0x8000) or a referenced sample whose
// rate is not 44.1/48/88.2/96 kHz. The reader never throws and every offset and size is bounds
// checked. Memory is bounded by constants and the file size: each table is read once and is at most
// 64 MiB, instrument zones are cached once (at most kMaxInstrumentZones), regions are at most
// kMaxRegions (about 100 MB of Region records at the cap), sample data is read on request, one sample
// at a time. Generator scanning is linear (`Sf2Stats::generator_scans` counts the records examined).
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "velora/format/bundle.hpp"
#include "velora/format/byte_source.hpp"
#include "velora/format/instrument_model.hpp"

namespace velora::format {

// Engine-convention constants (see the header comment).
inline constexpr float kSf2PanLawDb = -3.0103f;  // 20*log10(1/1.41254): undoes the engine's +3.01 dB mono pan gain
inline constexpr double kSf2NineTau = 0.78173;  // 9 * 20*log10(e) / 100
// Documented bounds against zone-explosion files.
inline constexpr std::uint64_t kMaxZoneCombos = 1u << 22;
inline constexpr std::uint64_t kMaxRegions = 1u << 19;
inline constexpr std::uint64_t kMaxInstrumentZones = 1u << 18;
inline constexpr std::uint64_t kMaxGeneratorScans = 1u << 27;

enum class Sf2IssueKind : std::uint8_t {
  kDamaged,      // reason code `damaged`
  kUnsupported,  // reason code `unsupported_features`
};

struct Sf2Issue {
  Sf2IssueKind kind = Sf2IssueKind::kDamaged;
  std::string message;  // the failing detail
  std::string item;     // kUnsupported: what is not supported ("ifil", "sample")
};

// A feature the model does not have, ignored for one preset. `item` is the generator name
// ("modLfoToPitch") or a modulator description ("modulator src=0x0013 dest=modLfoToPitch ...").
struct Sf2Warning {
  std::string code = "ignored_sf2_feature";  // | "ignored_zone" | "ignored_empty_preset"
  std::string preset;  // "bank:preset"
  std::string item;
  std::string message;
};

// One sample of a preset's bundle sample list (Region::sample indexes Sf2Preset::samples).
struct Sf2SampleRef {
  std::uint32_t shdr = 0;      // index into the file's sample headers
  std::string name;            // UTF-8
  PcmFormat format;            // mono, 16 or 24 bit, the source rate
  std::uint64_t first_frame = 0;  // frame index into `smpl`
  std::uint64_t frames = 0;
};

struct Sf2Preset {
  std::string name;  // UTF-8, never empty (falls back to the key)
  std::uint16_t bank = 0, program = 0;
  std::string key;   // "bank:program"
  std::vector<Sf2SampleRef> samples;
  std::vector<Region> regions;
};

struct Sf2Stats {
  std::size_t presets = 0, instruments = 0, shdr_samples = 0;
  std::size_t zones = 0;  // regions produced over all presets
  std::size_t ignored_generators = 0, ignored_modulators = 0;  // warnings by kind
  std::size_t ignored_zones = 0, ignored_empty_presets = 0;     // ignored_zone / ignored_empty_preset warnings
  std::uint64_t generator_scans = 0;                            // generator records examined (linear bound)
  bool bits24 = false;
  std::size_t ignored_total() const { return ignored_generators + ignored_modulators; }
};

class Sf2Samples;

struct Sf2ParseResult {
  std::vector<Sf2Issue> issues;      // empty = the file is usable
  std::vector<Sf2Preset> presets;    // file order
  std::vector<Sf2Warning> warnings;  // grouped by preset, in first-seen order
  Sf2Stats stats;
  std::shared_ptr<const Sf2Samples> samples;  // access to the sample data (valid when ok())
  bool ok() const { return issues.empty(); }
  bool damaged() const;  // any issue of kind kDamaged
};

// Access to the sample words of a parsed file. Holds the byte source.
class Sf2Samples {
 public:
  virtual ~Sf2Samples() = default;
  // The sample's frames in the PCM exchange convention (mono, sign-extended 16- or 24-bit). Bit
  // exact to the file. kOutOfRange / kTruncated when the sample does not fit.
  virtual Error read(const Sf2SampleRef& ref, std::vector<std::int32_t>& out) const = 0;
};

// Parses the bytes behind `src` (the caller keeps the source alive through the returned handle).
Sf2ParseResult parse_sf2(std::shared_ptr<const ByteSource> src);
// A memory block: the caller keeps `bytes` alive as long as the result's samples are read.
Sf2ParseResult parse_sf2_memory(std::span<const std::uint8_t> bytes);
// A file; unreadable -> a kDamaged issue naming the problem.
Sf2ParseResult parse_sf2_file(const std::string& path);

// Names of the SF2 generators (index = operator number); nullptr beyond 60.
const char* sf2_generator_name(unsigned oper);

}  // namespace velora::format
