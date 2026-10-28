#pragma once
// Sample-source port (Story 1.15): what the sample engine needs from the outside world to play one
// instrument -- its region table and fully RAM-resident PCM as float, planar (one contiguous array
// per channel). Plain structs and spans only, standard library only. A host or tool adapter
// (engine/host/sample_bundle_loader) fills it from a formatlib bundle; unit tests fill it by hand.
//
// Ownership: the source owns the memory; spans and pointers handed out by instrument() stay valid
// until the source is destroyed. The engine borrows them (it copies nothing), so the source must
// outlive the engine's use of it. Story 1.16 extended SamplePcm additively with heads (total_frames, has_tail); streaming of the tail is Story 1.17.
#include <atomic>
#include <cstdint>
#include <span>

#include "velora/ports/stream.hpp"

namespace velora::ports {

enum class SampleLoopMode : std::uint8_t {
  kNoLoop = 0,
  kOneShot = 1,
  kLoopContinuous = 2,
  kLoopSustain = 3,
};

enum class SampleTrigger : std::uint8_t {
  kAttack = 0,   // fires on note on
  kRelease = 1,  // fires on note off (after the sustain pedal, like the voice it follows)
  // Story 13.3.
  kFirst = 2,       // fires on note on while no other key of the part is held
  kLegato = 3,      // fires on note on while another key of the part is held
  kReleaseKey = 4,  // fires at the physical key up, whatever the sustain pedal does
};

// Story 5.2 extension (keyswitches, CC crossfades, basic filter). Every field defaults to "the region does not use it", so a
// region built before this story keeps its meaning and its render bit for bit.

// Keyswitch opcodes of a region; -1 = not given, otherwise a MIDI note 0..127 (sw_lokey/sw_hikey are the keyswitch range,
// sw_last, sw_down, sw_up, sw_previous, sw_default).
struct SampleKeyswitch {
  std::int16_t lokey = -1, hikey = -1, last = -1, down = -1, up = -1, previous = -1, default_key = -1;
};

// One CC crossfade range: controller number and the CC value range 0..127 (inclusive, as written in the SFZ).
struct SampleCcRange {
  std::uint8_t cc = 0, lo = 0, hi = 0;
};
inline constexpr unsigned kSampleMaxCcCrossfades = 4;  // per direction and region

enum class SampleXfCurve : std::uint8_t { kGain = 0, kPower = 1 };

enum class SampleFilterType : std::uint8_t {
  kNone = 0,
  kLpf1p = 1,
  kLpf2p = 2,
  kHpf1p = 3,
  kHpf2p = 4,
  kBpf2p = 5,
};

// Story 13.8: the per-voice shaping of a filter. cutoff / resonance entries are CC modulation read once at voice start (cc: a controller 0..127 or a pseudo-CC; curve as
// in SampleCcMod); depth is cents for the cutoff, dB for the resonance. keytrack: cents per key from keycenter; veltrack: cents at velocity 127. The defaults add nothing.
inline constexpr unsigned kSampleMaxFilterMods = 4;
struct SampleFilterMod {
  std::uint8_t cc = 0, curve = 0;
  float depth = 0.0f;
};
struct SampleFilterTune {
  float keytrack = 0.0f, veltrack = 0.0f;
  std::uint8_t keycenter = 60;
  std::uint8_t cutoff_count = 0, res_count = 0;
  SampleFilterMod cutoff[kSampleMaxFilterMods]{}, res[kSampleMaxFilterMods]{};
};

// One region of the instrument (the SFZ opcode subset of Story 1.13). Field meaning follows the
// SFZ specification; times are in seconds, ampeg_start and ampeg_sustain in percent, pan in
// -100..100, offset and loop points in frames of the region's sample (loop_end inclusive).
// Story 13.4: one `amp_velcurve_N` point (velocity 0..127 -> gain 0..1) and the bound on the points of a region.
struct SampleVelPoint {
  std::uint8_t vel = 0;
  float gain = 0.0f;
};
inline constexpr unsigned kSampleMaxVelPoints = 32;

// Story 13.5: CC gates and CC modulation read once when a voice starts. Controller numbers 0..127 are the part's real controllers; 131 (note-on velocity), 135 / 136
// (unipolar / bipolar random), 137 (alternate), 140 / 141 (key delta, absolute key delta) are sfizz pseudo-CCs the engine resolves from the note, never from a controller.
inline constexpr unsigned kSampleMaxCcGates = 8;
inline constexpr unsigned kSampleMaxCcMods = 16;
// The region plays only while the part's controller value (CC units, 0..127, fractional) is inside [lo, hi].
struct SampleCcGate {
  std::uint8_t cc = 0;
  float lo = 0.0f, hi = 127.0f;
};
enum class SampleCcTarget : std::uint8_t {
  kAmplitude = 0,  // gain x (depth / 100 x curve), percent
  kVolume = 1,     // dB added
  kPan = 2,        // percent added
  kTune = 3,       // cents added
  kOffset = 4,     // frames added: depth x controller (normalized), no curve
  kEgDelay = 5,    // seconds added
  kEgStart = 6,    // percent added
  kEgAttack = 7,   // seconds added
  kEgHold = 8,     // seconds added
  kEgDecay = 9,    // seconds added
  kEgSustain = 10, // percent added
  kEgRelease = 11, // seconds added
};
struct SampleCcMod {
  SampleCcTarget target = SampleCcTarget::kAmplitude;
  std::uint8_t cc = 0, curve = 0;  // curve: an SFZ curve index (SampleCurve::index, a predefined curve 0..6, otherwise the linear default)
  float depth = 0.0f;
};
// One `<curve>` table of the instrument: sparse points (x = 0..127 ascending); ends default to (0, 0) and (127, 1), gaps are filled linearly (sfizz).
struct SampleCurvePoint {
  std::uint8_t x = 0;
  float y = 0.0f;
};
struct SampleCurve {
  std::uint8_t index = 0;
  std::span<const SampleCurvePoint> points;
};
struct SampleCcDefault {
  std::uint8_t cc = 0;
  float value = 0.0f;  // CC units
};

// Story 13.6: choke groups and polyphony limits (see formatlib VoiceGroup). A voice that starts in group G releases every voice of its part whose region has
// off_by == G (kFast: a 6 ms release, kNormal: the victim's own release, kTime: off_time seconds); a limit that is reached releases the oldest voice (and the
// voices of its note on). 0 in a limit = none.
enum class SampleOffMode : std::uint8_t { kFast = 0, kNormal = 1, kTime = 2 };
inline constexpr unsigned kSampleMaxVoiceLimit = 255;

// Story 13.7: pitch bend and controller triggers (see formatlib PitchBend / CcTriggers). A region with bend_enabled false ignores the pitch wheel (the behaviour of
// every region before this story); with it the voice follows the part's wheel: cents = wheel x bend_up (wheel > 0) or -wheel x bend_down (wheel < 0), quantised to
// bend_step when that is above 1, reached through a linear ramp of bend_smooth_ms. A region with cc_trigger_count starts a voice when one of its controllers moves into
// [lo, hi] (CC units, inclusive); no_note: it never starts from a note.
inline constexpr unsigned kSampleMaxCcTriggers = 4;
struct SampleCcTrigger {
  std::uint8_t cc = 0;
  float lo = 0.0f, hi = 127.0f;
};

struct SampleRegionDesc {
  std::uint32_t sample_index = 0;  // into InstrumentView::samples
  std::uint8_t key_lo = 0, key_hi = 127, key_center = 60;
  std::uint8_t vel_lo = 0, vel_hi = 127;  // 7-bit velocity, inclusive
  std::int32_t transpose = 0;             // semitones
  float tune_cents = 0.0f;
  float volume_db = 0.0f;
  float pan = 0.0f;
  std::uint64_t offset = 0;
  std::uint32_t seq_length = 1, seq_position = 1;
  float lorand = 0.0f, hirand = 1.0f;  // selects when lorand <= random < hirand
  SampleLoopMode loop_mode = SampleLoopMode::kNoLoop;
  bool has_loop_points = false;
  std::uint64_t loop_start = 0, loop_end = 0;
  float ampeg_delay = 0.0f, ampeg_start = 0.0f, ampeg_attack = 0.0f, ampeg_hold = 0.0f,
        ampeg_decay = 0.0f, ampeg_sustain = 100.0f, ampeg_release = 0.0f;
  SampleTrigger trigger = SampleTrigger::kAttack;
  // Story 13.3: `end`, exclusive, in frames of the region's sample. The last played frame is min(end, length) - 1. has_end with end == 0: the
  // region is disabled (never played, not counted as unplayable). has_end = false: plays to the end of the sample.
  bool has_end = false;
  std::uint64_t end = 0;
  // Story 5.2: keyswitches, CC crossfades (xfin_*/xfout_* on up to four controllers each, entries sorted by cc), basic filter
  // (filter_type kNone = no filter; cutoff in Hz, resonance in dB).
  SampleKeyswitch sw;
  std::uint8_t xfin_count = 0, xfout_count = 0;
  SampleXfCurve xf_curve = SampleXfCurve::kPower;
  SampleCcRange xfin[kSampleMaxCcCrossfades], xfout[kSampleMaxCcCrossfades];
  SampleFilterType filter_type = SampleFilterType::kNone;
  float filter_cutoff_hz = 0.0f, filter_resonance_db = 0.0f;
  // Story 13.4: the note-on amplitude map (SFZ amp_veltrack / amp_velcurve_N / amplitude / amp_keytrack / amp_keycenter / ampeg_vel2*). The defaults give
  // the fixed law gain = velocity squared and the region's own envelope, bit for bit. veltrack and amplitude are percent, keytrack dB per key, the
  // vel2 times seconds and vel2sustain percent, all added at velocity 127 (scaled by velocity / 127). vel_points are ascending by vel, unused entries zero.
  float amp_veltrack = 100.0f, amplitude = 100.0f, amp_keytrack = 0.0f;
  std::uint8_t amp_keycenter = 60;
  std::uint8_t vel_point_count = 0;
  SampleVelPoint vel_points[kSampleMaxVelPoints]{};
  float vel2attack = 0.0f, vel2decay = 0.0f, vel2sustain = 0.0f, vel2release = 0.0f;
  // Story 13.5: CC gates and CC modulation entries (ascending by cc / by target and cc); the defaults mean the region has none.
  std::uint8_t cc_gate_count = 0, cc_mod_count = 0;
  SampleCcGate cc_gates[kSampleMaxCcGates]{};
  SampleCcMod cc_mods[kSampleMaxCcMods]{};
  // Story 13.6: choke group, off_by, off mode and time (seconds, only with kTime), region / group / per-note polyphony limits, note_selfmask. The defaults mean the
  // region is in no choke relation and has no limit: the engine then does nothing extra for it.
  std::int32_t group = 0;
  bool has_off_by = false;
  std::int32_t off_by = 0;
  SampleOffMode off_mode = SampleOffMode::kFast;
  float off_time = 0.0f;
  std::uint16_t polyphony = 0, group_polyphony = 0, note_polyphony = 0;
  bool note_selfmask = true;
  // Story 13.7: pitch bend range and controller triggers; the defaults mean the region ignores the wheel and starts from notes only.
  bool bend_enabled = false;
  float bend_up = 200.0f, bend_down = -200.0f, bend_step = 1.0f;
  std::uint16_t bend_smooth_ms = 0;
  std::uint8_t cc_trigger_count = 0;
  bool no_note = false;
  SampleCcTrigger cc_triggers[kSampleMaxCcTriggers]{};
  // Story 13.8: the tuning of the filter above and an optional second filter in series after it (filter2_type kNone = none). A tuning or a second filter without a first
  // filter is unplayable.
  SampleFilterTune filter_tune;
  SampleFilterType filter2_type = SampleFilterType::kNone;
  float filter2_cutoff_hz = 0.0f, filter2_resonance_db = 0.0f;
  SampleFilterTune filter2_tune;
};

// Story 1.17: a resident piece of a sample past its head: frames [start, start + frames) of the sample.
struct SamplePcmSegment {
  std::uint64_t start = 0, frames = 0;
  const float* left = nullptr;
  const float* right = nullptr;
};

// One sample's PCM. channels is 1 or 2; `right` is used only when channels == 2. A sample whose
// data is absent (null pointer, zero frames) makes every region using it unplayable: the engine
// skips such regions and counts them, it never plays garbage.
//
// Story 6.5 (content integrity). A protected library's blocks are hash-checked and decrypted by non-real-time threads only (the loader, and the I/O edge
// threads that re-read blocks). When such a thread finds a block of a sample damaged it sets that block's flag in `damaged_blocks`; the audio thread only
// READS the flags (one relaxed byte load per voice per frame) and answers a damaged block with a short fade to silence and a content_integrity count --
// it never hashes, decrypts or waits. nullptr (every unprotected sample) = no damage map, the render is exactly the Story 1.15 one, bit for bit.
// Block b covers frames [b * damaged_block_frames, (b + 1) * damaged_block_frames) of the sample; a frame beyond the map counts as undamaged. The map is
// owned by the source and must outlive its use, like the PCM.
struct SamplePcm {
  std::uint32_t sample_rate = 48000;
  std::uint8_t channels = 1;
  std::uint64_t frames = 0;
  const float* left = nullptr;
  const float* right = nullptr;
  const std::atomic<std::uint8_t>* damaged_blocks = nullptr;  // one byte per block, nonzero = damaged; nullptr = none
  std::uint32_t damaged_block_frames = 0;                     // frames per block of the map (formatlib kBlockFrames = 4096)
  std::uint32_t damaged_block_count = 0;                      // entries in the map
  // Story 1.16 (instrument heads). `frames` / `left` / `right` are what is RESIDENT: the whole sample, or only its head. `total_frames` is the length of the
  // sample as stored; 0 (the default, every fully resident source) means "the same as `frames`". `has_tail` is true when total_frames > frames: the rest of the
  // sample is not in memory (Story 1.17 streams it). Until then a voice whose playhead reaches the head end of a sample with a tail ends with a short fade and
  // is counted as a stream underrun; a source that is fully resident never produces one, and its render is bit for bit the Story 1.15 one.
  std::uint64_t total_frames = 0;
  bool has_tail = false;
  // Story 1.17 (streaming). A sample with a tail may carry the reader of its tail: `tail_source` (borrowed, owned by the source that owns this PCM) and its
  // instrument-local index `tail_sample`. nullptr (the default; every head-only source of Story 1.16 and every resident one) = the tail cannot be streamed and the voice
  // keeps the Story 1.16 placeholder (fade and count at the head end). A SampleEngine without a StreamService ignores both.
  // `segments` are extra RESIDENT pieces of the sample past the head: the loop-start regions of loops that reach beyond the head, so a loop wrap is seamless while the
  // stream re-seeks (see engine/core/README.md "Streaming"). Frame i of segment k is `left[i - start]`; where a segment overlaps the head the head wins.
  const BlockSource* tail_source = nullptr;
  std::uint32_t tail_sample = 0;
  const SamplePcmSegment* segments = nullptr;
  std::uint32_t segment_count = 0;
};

struct InstrumentView {
  std::span<const SampleRegionDesc> regions;
  std::span<const SamplePcm> samples;  // indexed by SampleRegionDesc::sample_index
  // Story 13.5: the controller values a part starts with (set_cc) and the curves the regions' modulation entries name. Empty = none.
  std::span<const SampleCcDefault> cc_defaults{};
  std::span<const SampleCurve> curves{};
};

class SampleSource {
 public:
  virtual ~SampleSource() = default;
  virtual InstrumentView instrument() const noexcept = 0;
};

}  // namespace velora::ports
