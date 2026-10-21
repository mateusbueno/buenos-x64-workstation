#pragma once
// Story 6.9: a deterministic SYNTHETIC factory source tree and release description, standing in for the founder's content (Story 11.17, not delivered yet).
// It is laid out exactly as the founder must deliver the real one (tools/factory-release/README.md, "Founder deliverable"):
//
//   <dir>/release.json                                   the release description
//   <dir>/piano/Factory_Grand.sfz  + samples             one folder per library: an .sfz (one instrument) or an .sf2 (many presets) with its samples
//   <dir>/organ-samples/Factory_Pipe.sfz  + samples
//   <dir>/strings/Factory_Strings.sfz  + samples
//   <dir>/keys/Factory_Keys.sf2                          presets "0:0".."0:2" (bank:preset is the instrument key)
//
// Everything is generated from fixed seeds (no clock, no OS randomness), so two generations are byte-identical.
#include <filesystem>
#include <string>
#include <vector>

#include "import_fixtures.hpp"

namespace synth {

namespace fs = std::filesystem;

// Dev-range ids (root dev needs no register entry).
inline const char* kPianoId = "00000000-0000-4000-8000-0000000000f1";
inline const char* kPipeId = "00000000-0000-4000-8000-0000000000f2";
inline const char* kStringsId = "00000000-0000-4000-8000-0000000000f3";
inline const char* kKeysId = "00000000-0000-4000-8000-0000000000f4";

// Writes the source tree under `dir`.
inline void write_sources(const fs::path& dir) {
  ifx::write_sfz(dir / "piano", "Factory_Grand", 0, 6000);
  ifx::write_sfz(dir / "organ-samples", "Factory_Pipe", 1, 6000);
  ifx::write_sfz(dir / "strings", "Factory_Strings", 2, 6000);
  ifx::write_many_sf2(dir / "keys" / "Factory_Keys.sf2", 3);
}

struct Options {
  std::string root = "dev";
  std::string version = "1.0.0";
  std::string piano_id = kPianoId, pipe_id = kPipeId, strings_id = kStringsId, keys_id = kKeysId;
  std::string piano_name = "Velora Factory Piano";
  std::string piano_category = "piano";  // empty = the field is left out
  bool omit_piano_category = false;
  unsigned revision = 1;
  std::string live_set_extra;  // inserted into live_sets.entries as extra entry text (starting with a comma)
  std::string piano_key = "Factory_Grand.sfz";  // the instrument key the Live Sets use for the piano
  bool with_live_sets = true;
};

inline std::string description(const Options& o = {}) {
  auto q = [](const std::string& s) { return "\"" + s + "\""; };
  std::string d = "{\n  \"schema_version\": 1,\n  \"release\": {\"name\": \"Velora Factory Library\", \"version\": " + q(o.version) + "},\n  \"root\": " + q(o.root) + ",\n  \"libraries\": [\n";
  d += "    {\"library_id\": " + q(o.piano_id) + ", \"name\": " + q(o.piano_name) + ", \"localized_names\": {\"pt-BR\": \"Piano de Fabrica\"}, \"source\": \"piano/Factory_Grand.sfz\"" +
       (o.omit_piano_category ? "" : ", \"category\": " + q(o.piano_category)) + ", \"revision\": " + std::to_string(o.revision) + "},\n";
  d += "    {\"library_id\": " + q(o.pipe_id) + ", \"name\": \"Velora Factory Pipes\", \"source\": \"organ-samples/Factory_Pipe.sfz\", \"category\": \"organ\", \"revision\": " + std::to_string(o.revision) + "},\n";
  d += "    {\"library_id\": " + q(o.strings_id) + ", \"name\": \"Velora Factory Strings\", \"source\": \"strings/Factory_Strings.sfz\", \"category\": \"strings\", \"revision\": " + std::to_string(o.revision) + "},\n";
  d += "    {\"library_id\": " + q(o.keys_id) + ", \"name\": \"Velora Factory Keys\", \"source\": \"keys/Factory_Keys.sf2\", \"category\": \"keys\", \"revision\": " + std::to_string(o.revision) +
       ", \"instruments\": [{\"key\": \"0:0\", \"name\": \"Factory E.Piano\", \"category\": \"epiano\"}, {\"key\": \"0:1\", \"name\": \"Factory Pad\", \"category\": \"synth_pad\"}]}\n  ]";
  if (o.with_live_sets) {
    d += ",\n  \"live_sets\": {\"entries\": [\n";
    d += "    {\"program_id\": 1, \"name\": \"Factory Grand\", \"parts\": [{\"part\": 0, \"timbre\": {\"library_id\": " + q(o.piano_id) + ", \"instrument_key\": " + q(o.piano_key) + "}, \"volume\": 1.0}]},\n";
    d += "    {\"program_id\": 2, \"name\": \"Gospel Organ\", \"parts\": [{\"part\": 0, \"engine\": {\"type\": \"organ\", \"params\": [{\"param_id\": 100, \"value\": 8}, {\"param_id\": 101, \"value\": 8}, "
         "{\"param_id\": 102, \"value\": 8}]}, \"volume\": 1.0}]},\n";
    d += "    {\"program_id\": 3, \"name\": \"Piano and Strings\", \"tempo\": 96, \"parts\": [{\"part\": 0, \"timbre\": {\"library_id\": " + q(o.piano_id) + ", \"instrument_key\": " + q(o.piano_key) +
         "}, \"volume\": 0.9}, {\"part\": 1, \"timbre\": {\"library_id\": " + q(o.strings_id) + ", \"instrument_key\": \"Factory_Strings.sfz\"}, \"volume\": 0.5, \"send_reverb\": 0.3}]},\n";
    d += "    {\"program_id\": 4, \"name\": \"Split Keys\", \"parts\": [{\"part\": 0, \"timbre\": {\"library_id\": " + q(o.keys_id) + ", \"instrument_key\": \"0:0\"}, \"high_key\": 59, \"volume\": 1.0}, "
         "{\"part\": 1, \"timbre\": {\"library_id\": " + q(o.pipe_id) + ", \"instrument_key\": \"Factory_Pipe.sfz\"}, \"low_key\": 60, \"octave\": 1, \"output\": \"aux\", \"volume\": 1.0}]}" + o.live_set_extra + "\n  ]}";
  }
  d += "\n}\n";
  return d;
}

}  // namespace synth
