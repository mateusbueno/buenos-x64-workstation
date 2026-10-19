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
// Sample paths must stay inside the SFZ's directory ("..", absolute paths elsewhere and an
// absolute sample combined with default_path are issues). Parser warnings are issues too.
// Inheritance: global -> master -> group -> region; a later header of a level resets the levels
// below it; within a region, later opcodes win.
#include <cstddef>
#include <string>
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

struct SfzIssue {
  SfzIssueKind kind = SfzIssueKind::kParse;
  std::string file;      // as given to the parser (absolute or relative to the sfz directory)
  std::size_t line = 0;  // 1-based, 0 when unknown
  std::string message;
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

struct SfzParseResult {
  std::vector<SfzSample> samples;          // unique, in first-use order; Region::sample indexes it
  std::vector<Region> regions;             // in source order (loops not yet resolved)
  std::vector<SfzLoopSpec> loops;          // parallel to regions
  std::vector<SfzSource> region_sources;   // <region> header location of each region
  std::vector<SfzIssue> issues;            // errors and unsupported constructs, in source order
  std::vector<std::string> included_files; // every file the parser read (main file included)
  std::size_t source_region_count = 0;     // number of <region> headers seen
  bool ok() const { return issues.empty(); }
};

// Parses and maps `sfz_path`. Never throws; failures are listed in `issues` (the other fields
// are then incomplete and must not be used to build a bundle).
SfzParseResult parse_sfz_file(const std::string& sfz_path);

}  // namespace velora::format
