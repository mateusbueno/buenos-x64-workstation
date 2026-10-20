#pragma once
// Story 6.4: the librarian installs a PROTECTED bundle (a vendor pack made by tools/content-pack, Story 6.3) and writes it under
// <root>/lib/<vendor library_id>/<bundle_id>/ with the same atomic publish as a user import (stage, verify, fsync, rename, flip `current`).
//
// The librarian only VERIFIES. It never decrypts a block and never sees an unwrapped content key: it links `velora_format_verify` (Ed25519 signature check and
// SHA-256 hashing) and nothing that can run a cipher (tools/ci/check_format_no_decrypt.py and check_librarian_no_crypto.py prove it from the symbols of the
// real binaries). The wrapped key blob is stored next to the bundle byte for byte; it is checked by STRUCTURE only (size, magic, algorithm, a non-zero
// ephemeral key), never unwrapped.
//
// What is checked, in this order (every failure is a typed InstallReason, nothing is published, the staging area is cleaned):
//   1. the bundle opens as a protected bundle (header, manifest and index hashes, trusted signer, Ed25519 signature over the manifest statement, hash-list root);
//   2. the vendor library_id matches the expected one (explicit, or the name of the pack directory);
//   3. the wrapped-key blob is a well-formed wrapped_key_v1;
//   4. catalog.meta is sealed for THIS bundle (catalog.meta.sig, formatlib protected_format.hpp: the same signer as the bundle) and names exactly the
//      bundle's instruments with assignable categories;
//   5. the library is not a user library, and the pack is not a replay: its signed pack_revision must not be LOWER than the current bundle's (downgrade), and
//      the same revision must be the same bundle (downgrade); the same bundle_id already installed is idempotent (unchanged); a lower format minor is a downgrade too;
//   6. there is room for the bundle plus the reserve;
//   7. the bundle is copied into the staging directory in bounded chunks and EVERY ciphertext block of the staged copy is hashed against the signed index.
// Memory is bounded: the copy chunk (kInstallChunkBytes) and one ciphertext block (kMaxProtectedBlockBytes) at a time, plus the manifest and the index that
// the verifier keeps; the file is never loaded whole.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "velora/format/byte_source.hpp"
#include "velora/format/protected_verify.hpp"
#include "velora/librarian/import.hpp"

namespace velora::librarian {

inline constexpr std::size_t kInstallChunkBytes = 256u << 10;  // the copy buffer

// The closed set of reasons an install is refused. The text of to_string() is the stable wire/CLI spelling (proto: InstallReason).
enum class InstallReason : std::uint8_t {
  kNone = 0,
  kSignatureInvalid,      // the Ed25519 signature over the manifest statement does not verify
  kUntrustedKey,          // the signer's key id is not in the trusted-key list
  kBlockHashMismatch,     // a ciphertext block does not match its index hash
  kHashListMismatch,      // the hash-list root of the manifest does not match the index
  kCorruptBundle,         // header, manifest or index damaged or truncated
  kWrappedKeyInvalid,     // missing, ambiguous or not a well-formed wrapped_key_v1
  kAlreadyInstalled,      // this bundle_id is already installed and intact (the install is a no-op: status unchanged)
  kLibraryIdMismatch,     // the bundle's library_id is not the one the install was asked for (or the pack directory's)
  kNotProtected,          // an unprotected (format 1 / 2) bundle: never installed as a Factory library
  kUnsupportedVersion,    // a format version this librarian does not know
  kDowngrade,             // older than the bundle that is current for this library: a lower pack_revision, the same revision with another bundle, or a lower format minor
  kCatalogMetaInvalid,    // catalog.meta / catalog.meta.sig missing, altered, sealed for another bundle, or not matching the bundle's instruments
  kLibraryIdInUse,        // the library_id is a user library (an unprotected `current` bundle)
  kMissingFile,           // the bundle file (or the pack directory) does not exist
  kIoError,               // the file system failed
};
const char* to_string(InstallReason r);  // "signature_invalid", "untrusted_key", "block_hash_mismatch", ...

enum class InstallStatus : std::uint8_t {
  kInstalled = 0,      // published and `current` flipped
  kUnchanged,          // this bundle was already installed (reason already_installed)
  kRejected,           // refused with a typed reason; nothing published
  kInvalidArgument,    // unusable options (no data root, no trusted keys list)
  kInsufficientSpace,  // required_bytes > free_bytes
  kIo,                 // the file system failed; nothing published
  kBusy,               // another importer / installer holds the staging lock
  kAborted,            // fault injection or a cancel stopped the run
  kDriveRemoved,       // the source disappeared while it was read; nothing published
};
const char* to_string(InstallStatus s);

struct InstallOptions {
  std::string root;  // existing data root: the bundle goes to <root>/lib/<library_id>/<bundle_id>/
  // The pack: either a content-pack output directory (bundle.vlrp, catalog.meta, catalog.meta.sig, wrapped-key.<device key id>.bin), or explicit paths. An
  // explicit path wins over the directory. Without a wrapped-key path the directory must hold exactly one `wrapped-key.*.bin`.
  std::string source_dir;
  std::string bundle_path, wrapped_key_path, catalog_meta_path, catalog_meta_seal_path;
  // The library_id the caller expects (canonical UUID text). Empty: the name of `source_dir` when that is a UUID, else none.
  std::string expected_library_id;
  const velora::format::TrustedKeys* trusted = nullptr;  // required
  FaultInjection fault;  // on_stage only (stage-created, staged, verified, fsynced, renamed, link-created, flipped)
  // Polled between chunks and blocks; true stops the install (kAborted, staging removed).
  std::function<bool()> cancelled;
  // Extra "the source is gone" signal (the board's unmount event, a test's injected fault); a failing read of the source also counts when the file or its
  // device vanished.
  std::function<bool()> source_gone;
  // Test hook: called after each chunk of the bundle was copied into the staging directory, with the chunk's 1-based number (a kill point mid-copy).
  std::function<void(std::uint64_t chunk)> on_copy_chunk;
  // Tests: replaces how a file is opened for reading (a counting or failing byte source).
  std::function<std::unique_ptr<velora::format::ByteSource>(const std::string& path)> open_source;
  // Free bytes of the file system that holds `dir` (statvfs when empty); false = unknown, the space check is skipped.
  std::function<bool(const std::string& dir, std::uint64_t& free_bytes)> free_bytes_fn;
};

struct InstallResult {
  InstallStatus status = InstallStatus::kInvalidArgument;
  InstallReason reason = InstallReason::kNone;
  std::string detail;        // one line for the log / the report
  std::string library_id;    // canonical UUID text (empty before the bundle opened)
  std::string bundle_id;     // 64 hex
  std::string bundle_dir;    // <root>/lib/<library_id>/<bundle_id>
  bool bundle_created = false, current_flipped = false;
  std::uint64_t bundle_bytes = 0;     // size of bundle.vlrp
  std::uint64_t required_bytes = 0, free_bytes = 0;  // kInsufficientSpace
  std::uint32_t instruments = 0, samples = 0, regions = 0;
  std::uint64_t blocks = 0;
  std::uint64_t peak_buffer_bytes = 0;  // the largest buffer the install held (copy chunk or one block): bounded memory, whatever the bundle size
  std::size_t reconciled = 0;           // staging leftovers of earlier runs removed under the lock
  std::uint32_t pack_revision = 0;      // the pack's signed revision (catalog.meta.sig)
  bool wrapped_key_updated = false;     // the bundle was already installed; a different wrapped-key blob (per-device redelivery) replaced the stored one
  std::size_t pruned = 0;               // older non-current bundle directories removed after the flip
  bool ok() const { return status == InstallStatus::kInstalled || status == InstallStatus::kUnchanged; }
};

// Space rule: the staged copy of the bundle plus the fixed reserve of an import (kImportReserveBytes, preflight.hpp).
std::uint64_t required_install_bytes(std::uint64_t pack_bytes);

// The structural check of a wrapped_key_v1 blob (tools/keys/KEY-HIERARCHY.md section 6): 105 bytes, magic "VWK1", algorithm 0x01, a non-zero ephemeral public
// key. "" when well formed, else why not. It cannot tell a forged or damaged payload from a good one (only the device can, by unwrapping).
std::string wrapped_key_problem(const std::uint8_t* data, std::size_t size);

InstallResult install_protected(const InstallOptions& opt);

// The trusted content-manifest keys of a `tools/keys/trust/<root>.pub-list` file (public keys only): one entry per line,
// `<root> <role> <key_id hex16> <public_key hex64>`, `#` starts a comment, role content-manifest. A key id that is not the first 8 bytes of the SHA-256 of its
// key, a malformed line or a repeated id is an error (false, `err` says which line).
bool load_trust_list(const std::string& path, velora::format::TrustedKeys& out, std::string& err);

}  // namespace velora::librarian

namespace velora::librarian {
// The trusted keys of a build for the image variant `root` ("dev", "factory-test", "production"): `embedded`, or, when `override_path` is not empty, a trust list
// file - which only the dev variant accepts. false with `err` when refused or unreadable.
bool resolve_trusted_keys_for_variant(const char* root, bool is_production, const velora::format::TrustedKeys& embedded, const std::string& override_path,
                                      velora::format::TrustedKeys& out, std::string& err);
}  // namespace velora::librarian
