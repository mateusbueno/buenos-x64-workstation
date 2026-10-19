#pragma once
// Library resolver: (library_id, instrument_key) -> opened bundle + instrument entry, found only
// through exact paths, never by scanning.
//
// On-disk layout (root is a parameter, default /data):
//   <root>/lib/<library_id>/current  -> <bundle_id>      (relative symlink, single component)
//   <root>/lib/<library_id>/<bundle_id>/bundle.vlb       (kBundleFileName, a formatlib bundle)
// <library_id> is a canonical lowercase UUID string, <bundle_id> 64 lowercase hex chars.
//
// resolve() does lstat(current), readlink(current), lstat(<bundle_id> dir), open(bundle.vlb) and
// three preads (header, manifest, index) -- a constant independent of how many libraries exist or
// how large they are. It never lists a directory and reads no sample blocks. A failed resolve never
// substitutes another instrument or library.
//
// Threading: runs on a control or I/O thread, never on the audio thread (it allocates and does
// file I/O). Stateless and thread-safe if the FsPort is.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "velora/format/bundle.hpp"
#include "velora/host/fs_port.hpp"

namespace velora::host {

inline constexpr const char* kDefaultLibraryRoot = "/data";
inline constexpr const char* kLibDirName = "lib";
inline constexpr const char* kCurrentLinkName = "current";
inline constexpr const char* kBundleFileName = "bundle.vlb";

enum class ResolveStatus : std::uint8_t {
  kOk = 0,
  kInvalidId,         // malformed library_id / instrument_key; rejected before any filesystem access
  kMissingLibrary,    // no such library, or `current` absent/dangling
  kMissingInstrument, // bundle opened, instrument_key not in it
  kBundleError,       // bundle unreadable or corrupt; see ResolveResult::bundle_error
  kInvalidLink,       // `current` is not a symlink to a sibling <bundle_id> directory
  kIdentityMismatch,  // bundle's library_id / bundle_id disagree with its path
};
const char* to_string(ResolveStatus s);

struct ResolveResult {
  ResolveStatus status = ResolveStatus::kInvalidId;
  format::Error bundle_error = format::Error::kNone;  // set with kBundleError
  // Non-null only with kOk. Keeps the bundle open (and its file descriptor) while held.
  std::shared_ptr<const format::BundleReader> bundle;
  std::size_t instrument = 0;  // index into bundle->instruments()

  bool ok() const { return status == ResolveStatus::kOk; }
  const format::InstrumentInfo& instrument_info() const {
    assert(ok() && bundle);
    return bundle->instruments()[instrument];
  }
};

// True for a canonical lowercase UUID string (8-4-4-4-12 hex). `out` is zeroed on failure.
bool parse_library_id(std::string_view s, format::Uuid& out);

class LibraryResolver {
 public:
  explicit LibraryResolver(std::shared_ptr<FsPort> fs, std::string root = kDefaultLibraryRoot);

  ResolveResult resolve(std::string_view library_id, std::string_view instrument_key) const;

  const std::string& root() const { return root_; }

 private:
  std::shared_ptr<FsPort> fs_;
  std::string root_;
};

}  // namespace velora::host
