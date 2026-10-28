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
#include <vector>

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

// One point of an `amp_velcurve_N` curve: velocity N (0..127) maps to gain 0..1.
struct VelPoint {
  std::uint8_t vel = 0;
  float gain = 0;
  friend bool operator==(const VelPoint&, const VelPoint&) = default;
};
// At most this many `amp_velcurve_N` points per region (documented bound; sfizz takes up to 128).
inline constexpr unsigned kMaxVelPoints = 32;

// Story 13.4 (bundle format 2.3): the note-on amplitude map of one region, evaluated once when a note starts (sfizz noteGain / EGDescription).
// The defaults leave the sound exactly as it was (gain = velocity squared). group_volume, master_volume and global_volume are not here:
// the importer adds them to the region's volume_db, and group/master/global_amplitude multiply `amplitude`.
struct AmpMap {
  float veltrack = 100;        // amp_veltrack, percent -100..100: 100 = the full velocity law, 0 = none, negative = inverted
  float amplitude = 100;       // amplitude, percent 0..10000
  float keytrack = 0;          // amp_keytrack, dB per key -96..12
  std::uint8_t keycenter = 60; // amp_keycenter
  std::uint8_t curve_count = 0;  // amp_velcurve_N points, ascending by vel, unused entries zero; 0 = the default law (velocity squared)
  VelPoint curve[kMaxVelPoints]{};
  float vel2attack = 0, vel2decay = 0, vel2release = 0;  // ampeg_vel2attack/decay/release, seconds added at velocity 127 (-100..100)
  float vel2sustain = 0;                                  // ampeg_vel2sustain, percent added at velocity 127 (-100..100)
  friend bool operator==(const AmpMap&, const AmpMap&) = default;
};

// Story 13.5 (bundle format 2.4): CC gates and CC modulation read once when a voice starts (sfizz: the controller value at note-on).
// Controller numbers: 0..127 are the part's real controllers; 131 (note-on velocity), 135 (unipolar random), 136 (bipolar random), 137 (alternate), 140
// (key delta) and 141 (absolute key delta) are sfizz pseudo-CCs resolved from the note, never from a real controller.
inline constexpr unsigned kMaxCcGates = 8;
inline constexpr unsigned kMaxCcMods = 16;
inline bool is_cc_mod_source(unsigned cc) { return cc <= 127 || cc == 131 || cc == 135 || cc == 136 || cc == 137 || cc == 140 || cc == 141; }

// locc/hicc (and lohdcc/hihdcc): the region plays only while the part's controller value is inside [lo, hi], both in CC units (0..127, fractional for
// MIDI 2.0 / hdcc). A hicc below 127 is stored as nextafter(hicc + 1, 0): every value from hicc up to, not including, hicc + 1 passes (sfizz).
struct CcGate {
  std::uint8_t cc = 0;
  float lo = 0, hi = 127;
  friend bool operator==(const CcGate&, const CcGate&) = default;
};

enum class CcTarget : std::uint8_t {
  kAmplitude = 0,  // amplitude_oncc: gain x (depth / 100 x curve), percent
  kVolume = 1,     // volume_oncc / gain_oncc: dB added
  kPan = 2,        // pan_oncc: percent added
  kTune = 3,       // tune_oncc / pitch_oncc: cents added
  kOffset = 4,     // offset_oncc: frames added (depth x CC, no curve, as sfizz)
  kEgDelay = 5,    // ampeg_delay_oncc: seconds
  kEgStart = 6,    // ampeg_start_oncc: percent
  kEgAttack = 7,   // ampeg_attack_oncc: seconds
  kEgHold = 8,     // ampeg_hold_oncc: seconds
  kEgDecay = 9,    // ampeg_decay_oncc: seconds
  kEgSustain = 10, // ampeg_sustain_oncc: percent
  kEgRelease = 11, // ampeg_release_oncc: seconds
};
inline constexpr std::uint8_t kMaxCcTarget = 11;

// One `<target>_oncc<N>` (with its `<target>_curvecc<N>`): depth in the target's unit, curve = a `<curve>` index (0 = sfizz's linear default).
struct CcMod {
  CcTarget target = CcTarget::kAmplitude;
  std::uint8_t cc = 0, curve = 0;
  float depth = 0;
  friend bool operator==(const CcMod&, const CcMod&) = default;
};

// Per region. Gates are ascending by cc (unique), modulation entries ascending by (target, cc) (unique); unused entries are zero.
struct CcMap {
  std::uint8_t gate_count = 0, mod_count = 0;
  CcGate gates[kMaxCcGates]{};
  CcMod mods[kMaxCcMods]{};
  friend bool operator==(const CcMap&, const CcMap&) = default;
};

// Instrument level (one per instrument): the CC values a part starts with (`set_ccN`, `set_hdccN`, CC units) and the `<curve>` tables the regions refer to.
// A curve is its sparse points (x = 0..127 ascending, y any finite number); ends default to (0, 0) and (127, 1) and the gaps are filled linearly (sfizz).
struct CcDefault {
  std::uint8_t cc = 0;
  float value = 0;
  friend bool operator==(const CcDefault&, const CcDefault&) = default;
};
struct CurvePoint {
  std::uint8_t x = 0;
  float y = 0;
  friend bool operator==(const CurvePoint&, const CurvePoint&) = default;
};
inline constexpr unsigned kMaxCurvePoints = 128;
struct CurveDef {
  std::uint8_t index = 0;
  std::vector<CurvePoint> points;
  friend bool operator==(const CurveDef&, const CurveDef&) = default;
};
struct InstrumentCcSetup {
  std::vector<CcDefault> defaults;  // ascending by cc, unique
  std::vector<CurveDef> curves;     // ascending by index, unique
  bool any() const { return !defaults.empty() || !curves.empty(); }
  friend bool operator==(const InstrumentCcSetup&, const InstrumentCcSetup&) = default;
};
// Checks the invariants of a setup (ranges, ordering, finite values). kNone or kInvalidArgument.
Error validate_cc_setup(const InstrumentCcSetup& s);

// Story 13.6 (bundle format 2.5): choke groups and polyphony limits, evaluated when a voice starts (sfizz Synth::checkOffGroups and VoiceManager::checkPolyphony).
// `group` / `off_by`: a voice that starts in group G releases every voice of its part whose region has off_by == G (a voice of the same group and the same note is
// spared while its own gates still hold, as sfizz); the cut uses the victim's off_mode: fast = 6 ms, normal = the victim's own release, time = off_time seconds.
// polyphony = at most that many voices of this region, group_polyphony = at most that many voices of the region's group (the importer gives every region of a
// group the group's final limit), note_polyphony = at most that many voices of the group for the same note (a quieter retrigger does not cut a louder voice
// unless note_selfmask is false). When a limit is reached the oldest voice (and the voices of its note on) is released with the victim's off_mode.
// 0 in a limit = none. The importer stores at most 255: sfizz's default (256) is no limit.
enum class OffMode : std::uint8_t {
  kFast = 0,    // off_mode=fast (the default): 6 ms
  kNormal = 1,  // off_mode=normal: the voice's own ampeg_release
  kTime = 2,    // off_mode=time, or any off_time: off_time seconds
};
inline constexpr std::uint8_t kMaxOffMode = 2;
inline constexpr std::uint32_t kMaxVoiceLimit = 255;
struct VoiceGroup {
  std::int32_t group = 0;
  bool has_off_by = false;
  std::int32_t off_by = 0;      // 0 when !has_off_by
  OffMode off_mode = OffMode::kFast;
  float off_time = 0;           // seconds 0..100, only with kTime (0 otherwise)
  std::uint16_t polyphony = 0, group_polyphony = 0, note_polyphony = 0;
  bool note_selfmask = true;    // note_selfmask=on (the default)
  friend bool operator==(const VoiceGroup&, const VoiceGroup&) = default;
};

// Story 13.7 (bundle format 2.6): the pitch-bend range of a region and its controller triggers (sfizz bend_up / bend_down / bend_step / bend_smooth and
// on_locc / on_hicc). `enabled` is false for a region that sets none of the four bend opcodes: the engine then ignores the pitch wheel for it, exactly as it did
// before this story. With `enabled` the unset opcodes keep sfizz's defaults (200, -200, 1, 0). The wheel at full travel up bends `up` cents, at full travel down
// `down` cents (negative = down), linearly in between; `step` quantises the cents (trunc(cents / step) * step, `step` > 1 only); `smooth_ms` is the length of the
// linear ramp the voice makes to a new wheel position (0 or one frame = at once).
inline constexpr float kMaxBendCents = 12000.f;
inline constexpr float kMaxBendStep = 12000.f;
struct PitchBend {
  bool enabled = false;
  float up = 200.f, down = -200.f, step = 1.f;
  std::uint16_t smooth_ms = 0;
  friend bool operator==(const PitchBend&, const PitchBend&) = default;
};

// on_locc<N> / on_hicc<N>: the region starts a voice when controller N moves to a value inside [lo, hi] (CC units, a hicc below 127 stored as nextafter(hicc + 1,
// 0), both ends inclusive) that differs from the value before the message, like sfizz. `no_note`: the region never starts from a note (hikey=-1), only from its
// triggers; it requires at least one trigger (a region that nothing can start is not stored). Triggers are ascending by cc (unique); unused entries are zero.
inline constexpr unsigned kMaxCcTriggers = 4;
struct CcTrigger {
  std::uint8_t cc = 0;
  float lo = 0, hi = 127;
  friend bool operator==(const CcTrigger&, const CcTrigger&) = default;
};
struct CcTriggers {
  std::uint8_t count = 0;
  bool no_note = false;
  CcTrigger t[kMaxCcTriggers]{};
  friend bool operator==(const CcTriggers&, const CcTriggers&) = default;
};

// Story 13.8 (bundle format 2.7): the per-voice shaping of the region's filters. Everything here is evaluated once when a voice starts (sfizz FilterHolder::setup plus
// the constant CC modulation of that instant): cutoff = clamp(clamp(cutoff * 2^((keytrack * (note - keycenter) + veltrack * velocity) / 1200)) * 2^(sum of
// cutoff-entry depth * curve(controller) / 1200)) in 0..20000 Hz, resonance = resonance + sum of resonance-entry depth * curve(controller) in dB. A filter
// exists only when the region has a `cutoff` (Filter::type != kNone); a tuning or a second filter on a region without a first filter is not stored.
inline constexpr unsigned kMaxFilterMods = 4;
struct FilterMod {
  std::uint8_t cc = 0, curve = 0;  // cc: a controller 0..127 or a pseudo-CC (is_cc_mod_source); curve: a <curve> index (0 = sfizz's linear default)
  float depth = 0;                 // cents for the cutoff entries (-12000..12000), dB for the resonance entries (0..96)
  friend bool operator==(const FilterMod&, const FilterMod&) = default;
};
struct FilterTune {
  float keytrack = 0;               // fil_keytrack, cents per key 0..1200
  std::uint8_t keycenter = 60;      // fil_keycenter
  float veltrack = 0;               // fil_veltrack, cents at velocity 127, -12000..12000
  std::uint8_t cutoff_count = 0, res_count = 0;  // entries, ascending by cc (unique); unused entries are zero
  FilterMod cutoff[kMaxFilterMods]{}, res[kMaxFilterMods]{};
  friend bool operator==(const FilterTune&, const FilterTune&) = default;
};
// The second filter (cutoff2, resonance2, fil2_*): in series after the first. type == kNone = none (everything else is then zero).
struct Filter2 {
  FilterType type = FilterType::kNone;
  float cutoff_hz = 0, resonance_db = 0;
  FilterTune tune;
  friend bool operator==(const Filter2&, const Filter2&) = default;
};
struct FilterMap {
  FilterTune f1;  // the tuning of Region::filter
  Filter2 f2;
  friend bool operator==(const FilterMap&, const FilterMap&) = default;
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
  // Story 13.4 extension (bundle format 2.3).
  AmpMap amp;
  // Story 13.5 extension (bundle format 2.4).
  CcMap cc;
  // Story 13.6 extension (bundle format 2.5).
  VoiceGroup vg;
  // Story 13.7 extension (bundle format 2.6).
  PitchBend bend;
  CcTriggers trig;
  // Story 13.8 extension (bundle format 2.7).
  FilterMap fmap;

  friend bool operator==(const Region&, const Region&) = default;
};

// True when the region uses any Story 5.1 feature (keyswitch, CC crossfade, xf_cccurve other than
// the default, filter), i.e. when it needs an extension record in the bundle.
bool has_extension(const Region& r);

// True when the region needs a record in the format 2.2 trigger/end table: a trigger other than attack/release, or `end`.
bool has_trigger_extension(const Region& r);

// True when the region needs a record in the format 2.3 amp table: any AmpMap field other than its default.
bool has_amp_extension(const Region& r);

// True when the region needs a record in the format 2.4 cc table: any gate or CC modulation entry.
bool has_cc_extension(const Region& r);

// True when the region needs a record in the format 2.5 voice-group table: any VoiceGroup field other than its default.
bool has_vg_extension(const Region& r);

// True when the region needs a record in the format 2.6 table: a pitch-bend range or any controller trigger.
bool has_bend_trigger_extension(const Region& r);

// True when the region needs a record in the format 2.7 filter table: a filter tuning or a second filter.
bool has_filter_extension(const Region& r);

// Checks every range invariant of a region against its sample length (`sample_frames`). Returns
// kNone or kInvalidArgument. Used by the writer, the reader (as kCorruptManifest) and the importer.
Error validate_region(const Region& r, std::uint64_t sample_frames);

}  // namespace velora::format
