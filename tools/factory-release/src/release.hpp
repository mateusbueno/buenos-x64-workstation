#pragma once
// Story 6.9: the factory release pipeline. `build` packs every library of a release description with tools/content-pack's run_pack (the same code path as
// `velora-content-pack pack`), signs the factory Live Sets, and writes one versioned release directory with a signed index. `verify` checks a release directory
// against its index. Layout, formats and exit codes: README.md.
#include <cstdint>
#include <string>
#include <vector>

#include "crypto.hpp"
#include "release_description.hpp"
#include "velora/format/protected_verify.hpp"

namespace velora::factoryrelease {

// Names inside a release directory (all paths in the index are relative to it).
inline constexpr const char* kIndexFileName = "release-index.json";
inline constexpr const char* kIndexSigFileName = "release-index.sig";
inline constexpr const char* kUnitDir = "unit";                   // what goes onto a unit's image
inline constexpr const char* kLibrariesDir = "unit/libraries";    // unit/libraries/<library_id>/{bundle.vlrp, catalog.meta, catalog.meta.sig, pack.json}
inline constexpr const char* kRegistryVaultDir = "registry-vault";  // registry-vault/<library_id>/content-key.vault: for the registry, NEVER for a unit image
inline constexpr const char* kIndexSchema = "velora.factory-release-index/1";
inline constexpr const char* kToolName = "velora-factory-release";
inline constexpr const char* kToolVersion = "1";

// release-index.sig (80 bytes): "VRIS" | version u8 = 1 | algorithm u8 = 1 (Ed25519) | reserved u16 = 0 | signer key id [8] | signature [64].
// signed message = "velora/factory-release-index/v1" 0x00 || signer key id (8) || SHA-256(release-index.json bytes).
inline constexpr std::size_t kIndexSigSize = 80;
inline constexpr const char* kIndexSigDomain = "velora/factory-release-index/v1";

struct BuildRequest {
  ReleaseDescription desc;
  std::string out_dir;                  // parent directory: the release goes to <out_dir>/<version>/
  bool overwrite = false;
  // Secrets (borrowed; the caller owns and wipes them).
  const velora::contentpack::Key32* kek = nullptr;
  const velora::format::SigningSeed* signing_seed = nullptr;
  // A previous release's registry-vault/ directory: libraries found there keep their content key (a rebuild must reuse it). Others get a fresh random key.
  std::string reuse_vaults_dir;
  // With reuse_vaults_dir set, a library that has no entry there is an error (vault_failed) unless this is set: a rebuild must not silently change a content key.
  bool allow_new_keys = false;
  // The index of the previous release (optional): removed program ids, and program ids / libraries whose content changed without a version / revision bump, are refused.
  std::string previous_index_path;
  // Vendor ID register (tools/content-pack/vendor-ids).
  std::string register_path;
  bool register_new = false;
  std::string owner, date;
};

struct InstrumentRow {
  std::string key, name, category;  // category: the key, e.g. "category.piano"
};
struct LibraryRow {
  std::string library_id, name, bundle_id;
  std::uint32_t revision = 1;
  std::vector<InstrumentRow> instruments;
};
struct ProgramRow {
  std::uint32_t program_id = 0;
  std::string name, live_set_id, sha256;  // sha256: of the entry with the release version left out of its factory mark (a content hash that survives a version bump)
};
struct BuildReport {
  std::string release_dir, signer_key_id, kek_id, index_sha256, kept_old;
  std::vector<LibraryRow> libraries;
  std::vector<ProgramRow> programs;
  std::size_t files = 0;
};

Failure build_release(const BuildRequest& req, BuildReport& report);

struct VerifyOptions {
  const velora::format::TrustedKeys* trusted = nullptr;  // required: the keys the index signature (and everything else) is checked against
  bool check_bundles = false;                             // re-verify every bundle with velora_format_verify (signature, hash list, every block) and its catalog seal
};
struct Problem {
  std::string code;    // stable name: index_missing, index_malformed, index_sig_malformed, index_untrusted_key, index_signature_invalid, file_missing, file_not_regular,
                       // size_mismatch, hash_mismatch, unexpected_file, unexpected_entry, bad_path, bundle_*, live_sets_*
  std::string path;
  std::string detail;
};
struct VerifyReport {
  std::string release_name, release_version, root, signer_key_id;
  std::size_t files_checked = 0, bundles_checked = 0, live_sets = 0;
  std::vector<Problem> problems;  // at most 200
  bool ok() const { return problems.empty(); }
};
// kVerifyFailed (14) when anything is wrong; `report.problems` lists what.
Failure verify_release(const std::string& release_dir, const VerifyOptions& opt, VerifyReport& report);

// Test and tool helpers.
std::string sha256_file_hex(const std::string& path, std::uint64_t* size_out = nullptr);

}  // namespace velora::factoryrelease
