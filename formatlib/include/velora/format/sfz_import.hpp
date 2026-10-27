#pragma once
// SFZ -> instrument model (Story 1.13). Parses one .sfz file (plus its #include files, #define
// and default_path) with the vendored sfizz parser and maps the supported opcode subset onto
// Region records (instrument_model.hpp). It reads no audio; the importer loads the samples.
//
// Supported subset (anything else is reported as unsupported with file:line and the file is
// skipped as a whole, never partially imported):
//   headers   <global> <master> <group> <region> <control>
//   opcodes   lokey hikey key, lovel hivel, pitch_keycenter, tune transpose, volume pan, offset,
//             seq_length seq_position, lorand hirand, loop_mode loop_start loop_end,
//             ampeg_delay/start/attack/hold/decay/sustain/release, trigger (attack|release),
//             sample, default_path (in <control>), #define, #include
//   Story 5.1: sw_lokey sw_hikey sw_last sw_down sw_up sw_previous sw_default,
//             xfin_locc<N> xfin_hicc<N> xfout_locc<N> xfout_hicc<N> (N = 0..127, at most 4 per
//             direction and region) xf_cccurve (gain|power), fil_type (lpf_1p lpf_2p hpf_1p
//             hpf_2p bpf_2p) cutoff resonance
//   Allow-listed non-sounding opcodes (label_*, image, hint_*, ...) are accepted, not stored,
//   and reported in SfzParseResult::allowlisted. The authoritative list is sfz_opcode_table().
// Sample paths must stay inside the SFZ's directory ("..", absolute paths elsewhere and an
// absolute sample combined with default_path are issues). Parser warnings are issues too.
// Inheritance: global -> master -> group -> region; a later header of a level resets the levels
// below it; within a region, later opcodes win.
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "velora/format/instrument_model.hpp"

namespace velora::format {

enum class SfzIssueKind : std::uint8_t {
  kUnsupportedOpcode,   // opcode outside the subset
  kUnsupportedHeader,   // header outside the subset
  kUnsupportedValue,    // supported opcode with a value outside the subset (e.g. trigger=legato)
  kInvalidValue,        // unparsable or out-of-range value
  kParse,               // syntax, missing/cyclic #include, unreadable file
  kMissingSample,       // sample= points at a file that does not exist
  kMissingSampleOpcode, // region without sample=
};
const char* to_string(SfzIssueKind k);

// kUnsupportedOpcode/Header/Value mean "the file uses something outside the subset" (reason code
// unsupported_features in the import report); every other kind means the file is damaged (cannot
// be parsed, bad value, missing sample), reason code damaged.
bool is_unsupported_feature(SfzIssueKind k);

// The one authoritative opcode list (README tables are checked against it by a test).
// Names use a tiny pattern language: `<N>` = one to three digits, trailing `*` = any suffix.
enum class OpcodeClass : std::uint8_t {
  kSupported,    // mapped into the instrument model
  kAllowListed,  // purely informational, accepted and counted, never changes the sound
  kUnsupported,  // anything else (classify_opcode only; the table lists no such entries)
};
struct SfzOpcodeEntry {
  const char* pattern;
  OpcodeClass cls;
  const char* note;
};
const std::vector<SfzOpcodeEntry>& sfz_opcode_table();
// Classifies a concrete opcode name against the table.
OpcodeClass classify_opcode(const std::string& name);
// True when `name` matches `pattern` (exact, `<N>` suffix or `*` suffix).
bool opcode_matches(const std::string& pattern, const std::string& name);

struct SfzIssue {
  SfzIssueKind kind = SfzIssueKind::kParse;
  std::string file;      // as given to the parser (absolute or relative to the sfz directory)
  std::size_t line = 0;  // 1-based, 0 when unknown
  std::string message;
  std::string path;      // failing path when a file is missing/unreadable (else the file above)
  std::string opcode;    // offending opcode (or "<header>") for unsupported_* kinds, else empty
  std::string to_string() const;  // "file:line: message"
};

struct SfzSample {
  std::string name;       // normalized relative path (instrument sample name)
  std::string path;       // path used to open the file
  std::string ref_file;   // where it was first referenced
  std::size_t ref_line = 0;
};

struct SfzSource {
  std::string file;
  std::size_t line = 0;
};

// Which loop points a region gave explicitly (the parsed Region holds only those).
struct SfzLoopSpec {
  bool start_set = false, end_set = false;
};

// Applies the SFZ loop defaults once the sample length is known: a missing loop_start is 0, a
// missing loop_end is the sample's last frame, and loop_mode=loop_continuous/loop_sustain without
// points loops the whole sample. Regions without loop mode or points are left untouched. Returns
// false with `why` for an empty sample or an inverted range ("loop_start > loop_end").
bool resolve_loop_points(Region& r, const SfzLoopSpec& spec, std::uint64_t sample_frames,
                         std::string* why);

// An allow-listed opcode that was accepted and ignored.
struct SfzAllowListed {
  std::string opcode, file;
  std::size_t line = 0;
};

// A sound-affecting opcode that was accepted but has no effect (code `ignored_opcode` in the
// import report), e.g. fil_type or resonance without cutoff.
struct SfzWarning {
  std::string opcode, file;
  std::size_t line = 0;
  std::string message;
};

// Story 13.2: a value outside an opcode's range or unparseable that the importer replaced by the opcode default
// (sfizz behaviour), e.g. lokey=-1 or lokey=c-2. Reported as `ignored_value`, never a reason to skip the file.
struct SfzIgnoredValue {
  std::string opcode, value, file;
  std::size_t line = 0;
  std::string message;
};

// Story 13.2: a <curve> block, parsed and stored inert (nothing in the subset consumes a curve; an opcode that
// would, like amp_velcurve_N, is unsupported and skips the file by itself).
struct SfzCurve {
  int index = -1;                                 // curve_index, -1 when not given
  std::vector<std::pair<int, double>> points;     // (vNNN point number 0..255, value), source order
  std::string file;
  std::size_t line = 0;                           // <curve> header
};

struct SfzParseResult {
  std::vector<SfzSample> samples;          // unique, in first-use order; Region::sample indexes it
  std::vector<Region> regions;             // in source order (loops not yet resolved)
  std::vector<SfzLoopSpec> loops;          // parallel to regions
  std::vector<SfzSource> region_sources;   // <region> header location of each region
  std::vector<SfzIssue> issues;            // errors and unsupported constructs, in source order
  std::vector<SfzWarning> warnings;        // ignored_opcode warnings, in source order
  std::vector<SfzAllowListed> allowlisted; // accepted non-sounding opcodes, in source order
  std::vector<SfzIgnoredValue> ignored_values;  // Story 13.2: values replaced by the opcode default, in source order
  std::vector<SfzCurve> curves;            // Story 13.2: <curve> blocks, stored inert
  std::vector<bool> never_note_triggered;  // Story 13.2: parallel to regions; hikey=-1 (no note ever triggers it)
  std::vector<std::string> included_files; // every file the parser read (main file included)
  std::size_t source_region_count = 0;     // number of <region> headers seen
  bool ok() const { return issues.empty(); }
};

// Story 13.2: removes the regions flagged never_note_triggered (and their parallel entries) so a bundle is built
// without them, until controller-triggered voices exist (Story 13.7). Returns how many were removed. Samples used
// only by removed regions stay in `samples`.
std::size_t drop_never_note_triggered(SfzParseResult& r);

// Parses and maps `sfz_path`. Never throws; failures are listed in `issues` (the other fields
// are then incomplete and must not be used to build a bundle).
SfzParseResult parse_sfz_file(const std::string& sfz_path);

}  // namespace velora::format
