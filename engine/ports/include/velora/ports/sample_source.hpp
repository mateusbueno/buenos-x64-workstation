#pragma once
// Sample-source port (Story 1.15): what the sample engine needs from the outside world to play one
// instrument -- its region table and fully RAM-resident PCM as float, planar (one contiguous array
// per channel). Plain structs and spans only, standard library only. A host or tool adapter
// (engine/host/sample_bundle_loader) fills it from a formatlib bundle; unit tests fill it by hand.
//
// Ownership: the source owns the memory; spans and pointers handed out by instrument() stay valid
// until the source is destroyed. The engine borrows them (it copies nothing), so the source must
// outlive the engine's use of it. Streaming and locked/partial residency are later stories
// (1.16, 1.17) and will extend this port, not change these structs.
#include <cstdint>
#include <span>

namespace velora::ports {

enum class SampleLoopMode : std::uint8_t {
  kNoLoop = 0,
  kOneShot = 1,
  kLoopContinuous = 2,
  kLoopSustain = 3,
};

enum class SampleTrigger : std::uint8_t {
  kAttack = 0,   // fires on note on
  kRelease = 1,  // fires on note off
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
};

// One sample's PCM. channels is 1 or 2; `right` is used only when channels == 2. A sample whose
// data is absent (null pointer, zero frames) makes every region using it unplayable: the engine
// skips such regions and counts them, it never plays garbage.
struct SamplePcm {
  std::uint32_t sample_rate = 48000;
  std::uint8_t channels = 1;
  std::uint64_t frames = 0;
  const float* left = nullptr;
  const float* right = nullptr;
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
