#pragma once
// Synthetic library tree generator for scale tests (Story 1.14). Writes the resolver's layout
// (see velora/host/library_resolver.hpp) with N libraries, each a small real bundle, plus an
// optional sparse padding file per library to emulate size without using disk.
#include <cstdint>
#include <string>

namespace velora::libgen {

struct Options {
  std::string root;                 // <root>/lib/<library_id>/...
  std::uint32_t libraries = 10;
  std::uint64_t pad_bytes = 0;      // sparse <bundle>/padding.bin per library (not part of the bundle)
  std::uint32_t first_index = 0;    // library i = first_index .. first_index+libraries-1
};

inline constexpr const char* kPaddingFileName = "padding.bin";
inline constexpr const char* kInstrumentA = "piano/soft.sfz";
inline constexpr const char* kInstrumentB = "piano/loud.sfz";

// Deterministic canonical library_id of library `index`.
std::string library_id_for_index(std::uint32_t index);

struct Generated {
  std::string library_id, bundle_id;
};

// Creates one library. Returns false and fills `err` on failure. A failed padding step (sparse
// files unsupported) is reported through `pad_ok` but does not fail the library.
bool generate_library(const Options& opt, std::uint32_t index, Generated* out, bool* pad_ok,
                      std::string* err);

// Creates opt.libraries libraries. Returns false on the first failure.
bool generate_all(const Options& opt, bool* pad_ok, std::string* err);

}  // namespace velora::libgen
