#pragma once
// Instrument model carried in the bundle manifest (format v2, Story 1.13): the SFZ key/velocity/
// round-robin/loop/envelope mapping of one instrument as a flat region table. A region points at
// one of the instrument's samples (index into the instrument's own sample list) and holds exactly
// the opcode subset the importer supports. Playback of regions is Story 1.15, not formatlib.
#include <cstdint>

#include "velora/format/error.hpp"

namespace velora::format {

enum class LoopMode : std::uint8_t {
  kNoLoop = 0,          // loop_mode=no_loop (also the default)
  kOneShot = 1,         // loop_mode=one_shot
  kLoopContinuous = 2,  // loop_mode=loop_continuous
  kLoopSustain = 3,     // loop_mode=loop_sustain
};

enum class TriggerMode : std::uint8_t {
  kAttack = 0,   // trigger=attack (default)
  kRelease = 1,  // trigger=release
};

// SFZ amplitude envelope (ampeg_*): times in seconds, start and sustain in percent.
struct AmpEnvelope {
  float delay = 0, start = 0, attack = 0, hold = 0, decay = 0, sustain = 100, release = 0;
  friend bool operator==(const AmpEnvelope&, const AmpEnvelope&) = default;
};

struct Region {
  std::uint32_t sample = 0;  // index into the owning instrument's samples
  std::uint8_t key_lo = 0, key_hi = 127, key_center = 60;
  std::uint8_t vel_lo = 0, vel_hi = 127;
  std::int32_t transpose = 0;  // semitones
  float tune_cents = 0;
  float volume_db = 0;
  float pan = 0;               // -100 .. 100
  std::uint64_t offset = 0;    // start offset in frames
  std::uint32_t seq_length = 1, seq_position = 1;  // round robin
  float lorand = 0, hirand = 1;
  LoopMode loop_mode = LoopMode::kNoLoop;
  bool has_loop_points = false;  // loop_start/loop_end were given
  std::uint64_t loop_start = 0, loop_end = 0;  // frames, loop_end inclusive as in SFZ
  AmpEnvelope ampeg;
  TriggerMode trigger = TriggerMode::kAttack;

  friend bool operator==(const Region&, const Region&) = default;
};

// Checks every range invariant of a region against its sample length (`sample_frames`). Returns
// kNone or kInvalidArgument. Used by the writer, the reader (as kCorruptManifest) and the importer.
Error validate_region(const Region& r, std::uint64_t sample_frames);

}  // namespace velora::format
