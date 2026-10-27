#pragma once
// Instrument model carried in the bundle manifest (format v2, Story 1.13): the SFZ key/velocity/
// round-robin/loop/envelope mapping of one instrument as a flat region table. A region points at
// one of the instrument's samples (index into the instrument's own sample list) and holds exactly
// the opcode subset the importer supports. Playback of regions is Story 1.15, not formatlib.
//
// Story 5.1 extends the region with keyswitches, CC crossfades and a basic filter. Those live in
// the bundle's additive extension table (format 2.1, see formatlib/README.md); a region that uses
// none of them keeps its Epic 1 serialization byte for byte. Playing them is Story 5.2.
#include <cstdint>

#include "velora/format/error.hpp"

namespace velora::format {

enum class LoopMode : std::uint8_t {
  kNoLoop = 0,          // loop_mode=no_loop (also the default)
  kOneShot = 1,         // loop_mode=one_shot
  kLoopContinuous = 2,  // loop_mode=loop_continuous
  kLoopSustain = 3,     // loop_mode=loop_sustain
};

// kFirst, kLegato and kReleaseKey (Story 13.3) live in the trigger/end table of bundle format 2.2.
enum class TriggerMode : std::uint8_t {
  kAttack = 0,      // trigger=attack (default)
  kRelease = 1,     // trigger=release (waits for the sustain pedal like the voice it follows)
  kFirst = 2,       // trigger=first: note on while no other key is held
  kLegato = 3,      // trigger=legato: note on while another key is held
  kReleaseKey = 4,  // trigger=release_key: fires at the physical key up, sustain pedal or not
};
inline constexpr std::uint8_t kMaxTriggerMode = 4;

// SFZ amplitude envelope (ampeg_*): times in seconds, start and sustain in percent.
struct AmpEnvelope {
  float delay = 0, start = 0, attack = 0, hold = 0, decay = 0, sustain = 100, release = 0;
  friend bool operator==(const AmpEnvelope&, const AmpEnvelope&) = default;
};

// SFZ `fil_type` values of the subset. kNone = the region has no filter.
enum class FilterType : std::uint8_t {
  kNone = 0,
  kLpf1p = 1,
  kLpf2p = 2,
  kHpf1p = 3,
  kHpf2p = 4,
  kBpf2p = 5,
};

// SFZ `xf_cccurve`. SFZ's default is power.
enum class XfCurve : std::uint8_t {
  kGain = 0,
  kPower = 1,
};

// Keyswitch opcodes of one region. -1 = the opcode was not given, otherwise a MIDI note 0..127.
// `lokey`/`hikey` are sw_lokey/sw_hikey (the keyswitch range), `last` sw_last, `down` sw_down,
// `up` sw_up, `previous` sw_previous and `default_key` sw_default.
struct Keyswitch {
  std::int16_t lokey = -1, hikey = -1, last = -1, down = -1, up = -1, previous = -1, default_key = -1;
  bool any() const { return *this != Keyswitch{}; }
  friend bool operator==(const Keyswitch&, const Keyswitch&) = default;
};

// One CC crossfade range: the controller number and the range of its value (0..127).
struct CcRange {
  std::uint8_t cc = 0, lo = 0, hi = 0;
  friend bool operator==(const CcRange&, const CcRange&) = default;
};

// At most this many xfin and this many xfout controllers per region (documented bound).
inline constexpr unsigned kMaxCcCrossfades = 4;

// CC crossfades of one region (xfin_locc<N>/xfin_hicc<N>, xfout_locc<N>/xfout_hicc<N>,
// xf_cccurve). Entries are sorted by cc, unused entries are zero.
struct Crossfade {
  std::uint8_t in_count = 0, out_count = 0;
  XfCurve curve = XfCurve::kPower;
  CcRange in[kMaxCcCrossfades], out[kMaxCcCrossfades];
  friend bool operator==(const Crossfade&, const Crossfade&) = default;
};

// Basic filter (fil_type, cutoff, resonance). type == kNone means no filter (cutoff and
// resonance are then 0). SFZ: the filter exists when `cutoff` is given; fil_type defaults to lpf_2p.
struct Filter {
  FilterType type = FilterType::kNone;
  float cutoff_hz = 0, resonance_db = 0;
  friend bool operator==(const Filter&, const Filter&) = default;
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
  // Story 13.3 (bundle format 2.2): `end` in frames, EXCLUSIVE: the last played frame is min(end, sample length) - 1. has_end = false: the
  // region plays to the end of its sample (end == 0 then). has_end with end == 0: the region is disabled (it never plays).
  bool has_end = false;
  std::uint64_t end = 0;
  // Story 5.1 extension (bundle format 2.1).
  Keyswitch sw;
  Crossfade xf;
  Filter filter;

  friend bool operator==(const Region&, const Region&) = default;
};

// True when the region uses any Story 5.1 feature (keyswitch, CC crossfade, xf_cccurve other than
// the default, filter), i.e. when it needs an extension record in the bundle.
bool has_extension(const Region& r);

// True when the region needs a record in the format 2.2 trigger/end table: a trigger other than attack/release, or `end`.
bool has_trigger_extension(const Region& r);

// Checks every range invariant of a region against its sample length (`sample_frames`). Returns
// kNone or kInvalidArgument. Used by the writer, the reader (as kCorruptManifest) and the importer.
Error validate_region(const Region& r, std::uint64_t sample_frames);

}  // namespace velora::format
