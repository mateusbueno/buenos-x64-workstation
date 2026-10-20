#pragma once
// Plain data of the libraries catalog (Story 5.6): what one published library looks like to the catalog and to the Library API. No SQLite, no
// formatlib: the catalog (catalog.hpp), the bundle reader side (library_index.hpp) and the service all share these structs.
#include <cstdint>
#include <string>
#include <vector>

namespace velora::librarian {

inline constexpr unsigned kMinCategoryId = 1;   // category ids are those of formatlib's Category / proto/category.proto
inline constexpr unsigned kMaxCategoryId = 17;  // the catalog's CHECK constraint; raised by a schema migration when a category is added

struct InstrumentRecord {
  std::string key;                // instrument_key, stable across re-conversion
  std::string name;               // display name
  std::uint32_t category = 0;     // 1..17, never 0
  std::uint32_t sample_count = 0;
  std::uint32_t region_count = 0;
  std::uint64_t bytes = 0;        // decoded PCM bytes of its samples
  friend bool operator==(const InstrumentRecord&, const InstrumentRecord&) = default;
};

struct LibraryRecord {
  std::string library_id;         // canonical UUID string
  std::string name;
  std::string source_kind;        // "sfz" | "sf2"
  std::string source_path;        // where it was imported from (empty for a bundle that predates catalog.meta)
  std::string bundle_id;          // the `current` bundle (64 lowercase hex)
  std::int64_t imported_seq = 0;  // import order; assigned by the catalog (0 = not yet)
  std::vector<InstrumentRecord> instruments;  // ordered as the bundle's manifest
  std::uint32_t instrument_count() const { return static_cast<std::uint32_t>(instruments.size()); }
};

// The same library content, ignoring only imported_seq (used to decide whether a catalog row needs a write).
inline bool same_content(const LibraryRecord& a, const LibraryRecord& b) {
  return a.library_id == b.library_id && a.name == b.name && a.source_kind == b.source_kind && a.source_path == b.source_path &&
         a.bundle_id == b.bundle_id && a.instruments == b.instruments;
}

}  // namespace velora::librarian
