#pragma once
// Story 6.3: the packing pipeline. Imports a source library through the librarian importer (the same code as a user import, in build-only mode), turns the
// result into a protected bundle (Story 6.2), vaults the content key and writes the output directory.
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "crypto.hpp"
#include "errors.hpp"
#include "pack_description.hpp"
#include "velora/format/protected_format.hpp"

namespace velora::contentpack {

inline constexpr const char* kBundleFileName = "bundle.vlrp";
inline constexpr const char* kVaultEntryFileName = "content-key.vault";
inline constexpr const char* kCatalogMetaFileName = "catalog.meta";
inline constexpr const char* kPackReportFileName = "pack.json";
inline constexpr const char* kToolName = "velora-content-pack";
inline constexpr const char* kToolVersion = "1";

struct PackRequest {
  PackDescription desc;
  std::string out_dir;  // parent directory; the pack goes to <out_dir>/<library_id>/
  bool overwrite = false;
  // Story 6.4: the pack revision (>= 1) signed into catalog.meta.sig. A device installs a revision only if it is not lower than the installed one, and an equal one only
  // if it is the same bundle: raise it with every release; a deliberate rollback is a NEW higher revision with the old content.
  std::uint32_t revision = 1;

  // Secrets (borrowed; the caller owns and wipes them).
  const Key32* kek = nullptr;
  const velora::format::SigningSeed* signing_seed = nullptr;
  // nullptr = a fresh random 256-bit key from the OS CSPRNG. Otherwise the key to reuse (reproducible builds): read from an earlier vault entry or a key file.
  const velora::format::ContentKey* content_key = nullptr;
  std::vector<Key32> device_pubs;  // one wrapped_key_v1 per device public key
  bool deterministic_vault = false;

  // Vendor ID register.
  std::string register_path;
  bool register_new = false;  // append the id to the register after a successful pack
  std::string owner, date;    // for register_new

  // Test hook: called with the final name and bytes of every file the tool writes, before it is written.
  std::function<void(const std::string& name, std::span<const std::uint8_t> bytes)> write_monitor;
};

struct PackReport {
  std::string library_id, bundle_id, plain_bundle_id, nonce_hex, signer_key_id, kek_id;
  std::string kept_old;  // set when an existing pack with another content key was kept as .old-<id>.<time>.<pid> instead of deleted
  std::string dir;  // <out_dir>/<library_id>
  std::vector<std::string> files;
  std::size_t instruments = 0;
  std::uint64_t blocks = 0;
};

Failure run_pack(const PackRequest& req, PackReport& report);

// `verify`: opens a protected bundle with the key from a vault entry (unwrapped with the KEK) and checks everything (signature against `trusted_pub`,
// hash list, every block hash, decrypt + decode, per-sample PCM hashes).
Failure verify_pack(const std::string& bundle_path, std::span<const std::uint8_t> vault_entry, const Key32& kek, const Key32& trusted_pub,
                    std::string* summary);

}  // namespace velora::contentpack
