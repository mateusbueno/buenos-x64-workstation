#pragma once
// Bundle-side metadata of the libraries catalog (Story 5.6): everything the catalog knows about a library can be rebuilt from the bundles under
// `<root>/lib/<library_id>/current`.
//
// `catalog.meta` (kCatalogMetaFileName) sits in the bundle directory next to `bundle.vlb` and `category`, is written inside the staging directory
// BEFORE the rename (so it is part of the atomic publish) and holds the display data of the library and of each instrument: library name, source
// kind and path, and per instrument key its display name and category. It is NOT in the binary manifest: the manifest is hashed into `bundle_id`,
// has a format version the engine reads, and adding names/categories there would bump the bundle format and change the bytes (and ids) of every
// bundle; the display name and category are librarian metadata that may be refreshed by an identical re-import (like the Story 5.5 `category`
// file) without making a new bundle. Like those files it is rewritten atomically (tmp + rename) and the published bundle's `bundle.vlb` stays
// immutable.
//
// Text format (UTF-8, one record per line, TAB separated, bytes < 0x20, 0x7f and '%' written as %XX):
//   format_version TAB 1
//   library_name TAB <name>
//   source_kind TAB sfz|sf2
//   source_path TAB <canonical path of the source file>
//   instrument TAB <instrument_key> TAB <display name> TAB <category key, e.g. category.piano>
// A reader ignores unknown record types and refuses a format_version above kCatalogMetaFormatVersion (the file is then treated as absent).
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "velora/format/category.hpp"
#include "velora/format/protected_verify.hpp"
#include "velora/librarian/library_records.hpp"

namespace velora::librarian {

inline constexpr const char* kCatalogMetaFileName = "catalog.meta";
inline constexpr std::uint32_t kCatalogMetaFormatVersion = 1;
inline constexpr std::size_t kMaxCatalogMetaBytes = 4u << 20;

struct CatalogMetaInstrument {
  std::string key, name;
  velora::format::Category category = velora::format::Category::kNone;
};
struct CatalogMeta {
  std::uint32_t format_version = kCatalogMetaFormatVersion;
  std::string library_name, source_kind, source_path;
  std::vector<CatalogMetaInstrument> instruments;
};

std::string catalog_meta_text(const CatalogMeta& m);
// false: not a valid catalog.meta (bad version, malformed record, a category that is not assignable, a duplicate key).
bool parse_catalog_meta(std::string_view text, CatalogMeta& out);
enum class MetaState : std::uint8_t { kAbsent, kValid, kUnreadable };
MetaState read_catalog_meta(const std::string& bundle_dir, CatalogMeta& out);

enum class DescribeStatus : std::uint8_t {
  kOk = 0,
  kUnreadable,        // the bundle does not open
  kIdentityMismatch,  // the bundle's library_id / bundle_id disagree with its path
  kMetaUnreadable,    // catalog.meta exists but is unreadable, oversized, malformed or from a newer format: the categories are unknown, NOT guessed from the legacy file
  kUncategorised,     // some instrument has no valid category (no catalog.meta entry and no valid legacy `category` file): not cataloged
};
struct DescribeResult {
  DescribeStatus status = DescribeStatus::kUnreadable;
  LibraryRecord record;   // valid with kOk
  std::string why;
  bool from_meta = false; // a valid catalog.meta was found
};
// Story 6.4. A PROTECTED bundle directory holds `bundle.vlrp` (kProtectedBundleFileName) instead of `bundle.vlb`, plus `wrapped-key.bin`, `catalog.meta` and
// `catalog.meta.sig`. Its catalog row is Factory. It is described WITHOUT decrypting: the manifest is read through ProtectedBundleView (which verifies the
// signature against `trusted`), the names and categories come from catalog.meta, and catalog.meta is accepted only when its detached seal verifies for this
// bundle_id (see protected_format.hpp, "catalog.meta binding"). Without trusted keys, or with a signature that does not verify, the bundle is kUnreadable
// (a row it already has is kept, like any bundle that cannot be examined).
inline constexpr const char* kProtectedBundleFileName = "bundle.vlrp";
inline constexpr const char* kWrappedKeyFileName = "wrapped-key.bin";
inline constexpr std::size_t kWrappedKeyBlobSize = 105;  // wrapped_key_v1 (tools/keys/KEY-HIERARCHY.md section 6)
// Story 6.4: true when `<data_root>/lib/<library_id>/` holds ANY protected bundle (a `<bundle_id>/bundle.vlrp`), whether `current` names it or not: the library is a
// Factory library and a user import must never touch it. Looks at the file system only (so it also covers the window between an install and its catalog row).
bool factory_library_installed(const std::string& data_root, const std::string& library_id);
struct DescribeOptions {
  const velora::format::TrustedKeys* trusted = nullptr;  // content-manifest keys of this image variant; needed for protected bundles
};
// Reads `<bundle_dir>/bundle.vlb` (header, manifest, index; no sample blocks) and the metadata files. `bundle_dir` = <root>/lib/<library_id>/<bundle_id>.
// Names come from catalog.meta, else from the manifest; categories from catalog.meta, else the legacy `category` file (Story 5.5, via
// read_category_file); a bundle without any valid category for some instrument is kUncategorised.
DescribeResult describe_bundle(const std::string& bundle_dir, const DescribeOptions& opt = {});

}  // namespace velora::librarian
