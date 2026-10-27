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

// One region of the instrument (the SFZ opcode subset of Story 1.13). Field meaning follows the
// SFZ specification; times are in seconds, ampeg_start and ampeg_sustain in percent, pan in
// -100..100, offset and loop points in frames of the region's sample (loop_end inclusive).
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
};

class SampleSource {
 public:
  virtual ~SampleSource() = default;
  virtual InstrumentView instrument() const noexcept = 0;
};

}  // namespace velora::ports
