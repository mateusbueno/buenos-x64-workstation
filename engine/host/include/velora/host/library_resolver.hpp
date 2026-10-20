#pragma once
// Library resolver: (library_id, instrument_key) -> opened bundle + instrument entry, found only
// through exact paths, never by scanning.
//
// On-disk layout (root is a parameter, default /data):
//   <root>/lib/<library_id>/current  -> <bundle_id>      (relative symlink, single component)
//   <root>/lib/<library_id>/<bundle_id>/bundle.vlb       (kBundleFileName, a formatlib bundle)
// <library_id> is a canonical lowercase UUID string, <bundle_id> 64 lowercase hex chars.
//
// Story 6.5: a PROTECTED (factory) library installed by the librarian has bundle.vlrp (kProtectedBundleFileName, format 3) and wrapped-key.bin
// (kWrappedKeyFileName) in its <bundle_id> directory instead of bundle.vlb. The resolver finds it by the same exact paths -- only when bundle.vlb does not
// exist, so an unprotected library costs exactly the ops it always did -- and VERIFIES it (header, manifest and index hashes, the signer against the trusted
// keys set with set_protected_trust(), the Ed25519 signature, the hash-list root) before it hands out the instrument. It decrypts nothing and never sees a
// key: the content key is obtained later, through the key-provider port, by the loader (protected_head_provider.hpp).
//
// resolve() does lstat(current), readlink(current), lstat(<bundle_id> dir), open(bundle.vlb), two lstats (Story 6.5: bundle.vlrp and wrapped-key.bin must NOT
// stand beside it) and three preads (header, manifest, index) -- a constant (10 ops with the close) independent of how many libraries exist or
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
#include "velora/format/protected_verify.hpp"
#include "velora/host/fs_port.hpp"

namespace velora::host {

inline constexpr const char* kDefaultLibraryRoot = "/data";
inline constexpr const char* kLibDirName = "lib";
inline constexpr const char* kCurrentLinkName = "current";
inline constexpr const char* kBundleFileName = "bundle.vlb";
inline constexpr const char* kProtectedBundleFileName = "bundle.vlrp";  // Story 6.5: a protected (format 3) bundle installed by the librarian
inline constexpr const char* kWrappedKeyFileName = "wrapped-key.bin";  // beside it: the wrapped_key_v1 blob delivered with the pack

enum class ResolveStatus : std::uint8_t {
  kOk = 0,
  kInvalidId,         // malformed library_id / instrument_key; rejected before any filesystem access
  kMissingLibrary,    // no such library, or `current` absent/dangling
  kMissingInstrument, // bundle opened, instrument_key not in it
  kBundleError,       // bundle unreadable or corrupt; see ResolveResult::bundle_error
  kInvalidLink,       // `current` is not a symlink to a sibling <bundle_id> directory
  kIdentityMismatch,  // bundle's library_id / bundle_id disagree with its path
  // Story 6.5
  kProtectedDowngrade,   // `protected_downgrade`: a bundle.vlb stands beside bundle.vlrp or wrapped-key.bin in the same bundle directory (a planted unprotected copy of a factory library)
  kProtectedUnsupported, // `protected_unsupported`: a protected bundle, but this host has no trusted keys configured (it plays no protected library); the part stays silent
};
const char* to_string(ResolveStatus s);

struct ResolveResult {
  ResolveStatus status = ResolveStatus::kInvalidId;
  format::Error bundle_error = format::Error::kNone;  // set with kBundleError
  // Non-null only with kOk and an unprotected library. Keeps the bundle open (and its file descriptor) while held.
  std::shared_ptr<const format::BundleReader> bundle;
  // Story 6.5: non-null only with kOk and a PROTECTED library (then `bundle` is null): the verified view, which keeps bundle.vlrp open while held.
  std::shared_ptr<const format::ProtectedBundleView> protected_view;
  bool from_protected = false; // Story 6.5: the failure (kBundleError / kIdentityMismatch) came from opening a PROTECTED bundle.vlrp (set even when status != kOk)
  std::string bundle_dir;      // kOk: <root>/lib/<library_id>/<bundle_id> (where wrapped-key.bin lives for a protected library)
  std::string library_id;      // kOk: the canonical library id text the resolve was asked for
  std::size_t instrument = 0;  // index into the instruments() of whichever of the two is set

  bool ok() const { return status == ResolveStatus::kOk; }
  bool is_protected() const { return protected_view != nullptr; }
  const format::InstrumentInfo& instrument_info() const {
    assert(ok() && (bundle || protected_view));
    return protected_view ? protected_view->instruments()[instrument] : bundle->instruments()[instrument];
  }
};

// True for a canonical lowercase UUID string (8-4-4-4-12 hex). `out` is zeroed on failure.
bool parse_library_id(std::string_view s, format::Uuid& out);

class LibraryResolver {
 public:
  explicit LibraryResolver(std::shared_ptr<FsPort> fs, std::string root = kDefaultLibraryRoot);

  ResolveResult resolve(std::string_view library_id, std::string_view instrument_key) const;

  // Story 6.5: the content-manifest keys this engine trusts for protected libraries (injectable; the dev variant in engine/host/protected). Without them a
  // protected library resolves to kBundleError / kUntrustedKey: nothing is ever opened on trust. Set before the resolver is shared between threads.
  void set_protected_trust(std::shared_ptr<const format::TrustedKeys> trusted) { trusted_ = std::move(trusted); }

  const std::string& root() const { return root_; }

 private:
  ResolveResult resolve_protected(std::string_view library_id, const format::Uuid& want_lib, const std::string& bundle_dir, const std::string& bundle_dir_name,
                                  const std::string& instrument_key) const;
  std::shared_ptr<FsPort> fs_;
  std::string root_;
  std::shared_ptr<const format::TrustedKeys> trusted_;
};

}  // namespace velora::host
