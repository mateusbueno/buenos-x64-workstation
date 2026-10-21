#pragma once
// Story 6.9: the release description file (JSON, schema_version 1; documented in README.md). It lists the vendor libraries of one factory release and the factory
// Live Sets that reference them.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "pack_description.hpp"
#include "release_errors.hpp"

namespace velora::factoryrelease {

inline constexpr int kReleaseSchemaVersion = 1;
inline constexpr std::size_t kMaxLibraries = 256;

struct LibraryEntry {
  velora::contentpack::PackDescription pack;  // library_id, name, localized_names, source (absolute or resolved), category, instrument overrides; root = the release's
  std::uint32_t revision = 1;                 // the pack revision signed into catalog.meta.sig (rises with every release of the library)
};

struct EngineParam {
  std::uint32_t param_id = 0;
  float value = 0;
};

struct PartSpec {
  std::uint32_t part = 0;
  bool has_timbre = false;
  std::string library_id, instrument_key;  // the ONLY way a factory Live Set names an instrument
  std::uint32_t engine_type = 1;           // core EngineTypeId: Sample = 1, Organ = 2, FM = 3, VA = 4
  std::vector<EngineParam> params;         // engine parameters (registry part-scope ids)
  // Optional part fields; unset ones keep the default of liveset::make_default_live_set.
  bool has_low_key = false, has_high_key = false, has_octave = false, has_transpose = false, has_keyboard_on = false, has_volume = false, has_send_reverb = false,
       has_send_delay = false, has_output = false;
  std::uint32_t low_key = 0, high_key = 127;
  int octave = 0, transpose = 0;
  bool keyboard_on = true;
  float volume = 0, send_reverb = 0, send_delay = 0;
  bool output_aux = false;
};

struct LiveSetSpec {
  std::uint32_t program_id = 0;
  std::string name;
  float tempo = 120.0f;
  std::vector<PartSpec> parts;
  std::vector<EngineParam> master;  // master-scope registry ids
};

struct ReleaseDescription {
  std::string name, version;                 // the release: display name and MAJOR.MINOR.PATCH
  velora::contentpack::Root root = velora::contentpack::Root::kDev;
  std::vector<LibraryEntry> libraries;
  std::string live_set_file;                 // output file name inside unit/ (default factory-live-sets.vfls); empty when there are no Live Sets
  std::vector<LiveSetSpec> live_sets;
  std::string base_dir;                      // the directory of the description (relative sources resolve against it)
};

// Strict parse: unknown fields, duplicate fields, wrong types, bad values are kBadDescription; a repeated library id or (normalised) name is kDuplicateLibrary; a
// program id of 0 or used twice is kDuplicateProgramId; a library or instrument without an assignable category is kMissingCategory. References of the Live Sets to
// libraries are checked here (library_id must be one of the release's); the instrument keys are checked against the packed libraries by the build.
Failure parse_release_description(const std::string& text, const std::string& base_dir, ReleaseDescription& out);
Failure load_release_description(const std::string& path, ReleaseDescription& out);

}  // namespace velora::factoryrelease
