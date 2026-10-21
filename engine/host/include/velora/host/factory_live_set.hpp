#pragma once
// Factory Live Sets (Story 6.9: FR34, AR31). The factory library ships with Live Sets (organ presets, piano setups) that carry vendor-assigned program ids
// and reference instruments only by (library_id, instrument_key) or by engine parameters. They travel in ONE signed file, `factory-live-sets.vfls`, made by
// tools/factory-release under the release root's content-manifest key. On first boot the engine's control thread verifies the file against its trusted keys,
// checks that every referenced instrument resolves, and imports the Live Sets through the single writer (LiveSetStore).
//
// The file (all integers little-endian), "byte-exact" so a second implementation can read and write it:
//   offset  size  field
//        0     4  magic "VFLS"
//        4     2  container_version = 1
//        6     2  reserved = 0
//        8     8  signer key id (first 8 bytes of SHA-256 of the Ed25519 public key; the release root's content-manifest key)
//       16    64  Ed25519 signature (RFC 8032, deterministic) over the signed message below
//       80     4  payload_length N (<= kMaxPayloadBytes)
//       84     N  payload = the serialised `velora.liveset.v1.FactoryLiveSets` message (proto/factory_live_set.proto), exactly the signed bytes
//   total = 84 + N; anything after the payload is refused.
// signed message = "velora/factory-live-sets/v1" 0x00 || signer key id (8) || payload_length (4, LE) || payload.
// The verifier hashes/verifies the payload bytes as stored and never re-serialises them. Ed25519 verification is the strict one of formatlib
// (ed25519_verify_strict); the signer must be in the injected TrustedKeys.
//
// Identity of a factory Live Set: its program id. Its Live Set id is derived from the program id alone (derive_live_set_id: a UUIDv4-shaped value from
// SHA-256("velora/factory-live-set-id/v1" 0x00 || program_id), so a later release replaces the same file, and a user copy ("Save as new") never collides.
// Mark: LiveSet.factory (FactoryInfo{program_id, release_version}) is set on every factory Live Set; the engine never overwrites such a file with a Write.
//
// Install (install_factory_live_sets), control thread only (the LiveSetStore's single writer), in this order, every failure a typed status with nothing
// written (except where stated):
//   1. size, magic, version, length; the signer against the trusted keys; the signature;
//   2. the payload: format_version 1, release name, semantic version, root, 1..kMaxEntries entries, program ids > 0 and unique, each entry's Live Set valid for the
//      tier (liveset::validate), id == derive_live_set_id(program_id), factory mark == {program_id, release version};
//   3. every (library_id, instrument_key) of every part resolves through the injected resolver; otherwise kUnresolvedReferences listing each missing pair;
//   (the signer's trust-list line carries a root; when it has one, the payload's root must be the same: kRootMismatch. A trust built from a bare key has no root.)
//   4. the state file `<dir>/factory-live-sets.state` (text: header line, `release V`, `root R`, `program N` lines; unknown `key value` lines are ignored): it is per root --
//      a state of another root is not used (a fresh state; no file is deleted). A missing or unreadable state is rebuilt from the factory marks in the store (programs
//      present, the highest release version as the floor). A release OLDER than the recorded one is refused (kDowngrade); the same or a newer one imports only what is not there:
//        - no file and the program id is not recorded: imported;
//        - no file but the program id IS recorded: the user deleted it; it is not brought back;
//        - a file carrying the factory mark of this program id: replaced when this release is newer, left alone when equal (idempotent);
//        - a file with that id but no factory mark (not ours): never touched, reported as a conflict;
//        - a file at a factory id that cannot be read or parsed (damaged): rewritten (counted as updated).
//   5. the state file is rewritten (atomic replace) after the Live Sets, also after a partial failure (it then records the ids actually written), so a crash or an error
//      in between is repaired by running the install again.
// Nothing here runs on the audio thread.
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "factory_live_set.pb.h"
#include "velora/format/protected_verify.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/host/live_set_store.hpp"

namespace velora::host::factory {

inline constexpr char kMagic[4] = {'V', 'F', 'L', 'S'};
inline constexpr std::uint16_t kContainerVersion = 1;
inline constexpr std::size_t kHeaderBytes = 84;
inline constexpr std::size_t kMaxPayloadBytes = 4u << 20;  // 4 MiB
inline constexpr std::size_t kMaxEntries = 256;
inline constexpr std::size_t kMaxFileBytes = kHeaderBytes + kMaxPayloadBytes;
inline constexpr std::uint32_t kFormatVersion = 1;  // FactoryLiveSets.format_version
inline constexpr const char* kSignatureDomain = "velora/factory-live-sets/v1";
inline constexpr const char* kLiveSetIdDomain = "velora/factory-live-set-id/v1";
inline constexpr const char* kDefaultFileName = "factory-live-sets.vfls";
inline constexpr const char* kStateFileName = "factory-live-sets.state";
inline constexpr std::size_t kMaxReleaseNameBytes = 128;

namespace fmt = ::velora::format;
namespace lsp = ::velora::liveset::v1;

// ---- container -----------------------------------------------------------------------------------------------------------------------------------
// The message the signature covers.
std::vector<std::uint8_t> signing_statement(const fmt::KeyId& signer, std::string_view payload);
// header + payload (the signature is made by the caller: tools/factory-release holds the key).
std::string assemble_container(const fmt::KeyId& signer, const fmt::Signature& signature, std::string_view payload);
// The deterministic serialisation of the payload message (the bytes that are signed).
bool serialize_payload(const lsp::FactoryLiveSets& sets, std::string& out);

// Live Set id of a program id (see above). Always satisfies liveset::is_uuid_v4.
std::string derive_live_set_id(std::uint32_t program_id);

// "MAJOR.MINOR.PATCH", three decimal numbers of 1..9 digits without leading zeros (except 0 itself). compare: <0, 0, >0.
bool valid_release_version(std::string_view v);
int compare_release_versions(std::string_view a, std::string_view b);

enum class ContainerStatus : std::uint8_t {
  kOk = 0,
  kTooLarge,           // the file (or its declared payload) is above the limit
  kTruncated,          // shorter than the header or than the declared payload
  kBadMagic,
  kUnsupportedVersion, // container_version (or payload format_version) not 1, or reserved != 0
  kBadLength,          // bytes after the declared payload
  kUntrustedKey,       // the signer is not in the trusted keys
  kSignatureInvalid,
  kBadPayload,         // the payload does not parse as FactoryLiveSets
  kRootMismatch,       // the signer's trust-list line is for another root than the payload says (a dev key claiming production)
};
const char* to_string(ContainerStatus s);

struct ParsedContainer {
  fmt::KeyId signer{};
  fmt::Signature signature{};
  std::string_view payload;  // points into the caller's bytes
};
// Header checks only (no key needed): magic, versions, lengths. Nothing is verified.
ContainerStatus parse_container(std::string_view bytes, ParsedContainer& out);
// parse_container + trusted signer + strict Ed25519 signature + payload parse.
ContainerStatus verify_container(std::string_view bytes, const fmt::TrustedKeys& trusted, lsp::FactoryLiveSets& out, fmt::KeyId* signer = nullptr);

// ---- install -------------------------------------------------------------------------------------------------------------------------------------
enum class FactoryStatus : std::uint8_t {
  kOk = 0,              // processed: see imported / updated / unchanged / deleted_by_user / conflicts
  kIo,                  // the file could not be read
  kBadContainer,        // container problems other than trust (see container)
  kUntrustedKey,
  kSignatureInvalid,
  kBadRelease,          // release name / version / root / entry count / format version of the payload
  kDuplicateProgramId,  // a program id is 0 or repeated
  kInvalidLiveSet,      // an entry fails validation, has the wrong id or a wrong / missing factory mark (see program_id)
  kRootMismatch,        // the signer is trusted for another root than the release says
  kUnresolvedReferences,// instruments the resolver does not find (see missing); nothing written
  kDowngrade,           // the release is older than the one recorded in the state file
  kStore,               // the store or the state file could not be written (see detail); a rerun repairs it
};
const char* to_string(FactoryStatus s);

struct MissingReference {
  std::uint32_t program_id = 0;
  std::uint32_t part = 0;
  std::string library_id, instrument_key;
};

using ReferenceResolver = std::function<bool(const std::string& library_id, const std::string& instrument_key)>;
// The resolver of the installed libraries: resolve() of the engine's LibraryResolver (verified protected bundles included).
ReferenceResolver resolver_of(const LibraryResolver& resolver);

struct InstallOptions {
  const fmt::TrustedKeys* trusted = nullptr;  // required: the content-manifest keys the engine trusts
  ReferenceResolver resolves;                 // required
  std::uint32_t part_count = liveset::kV1PartCount;
};

struct InstallResult {
  FactoryStatus status = FactoryStatus::kIo;
  ContainerStatus container = ContainerStatus::kOk;  // with kBadContainer / kUntrustedKey / kSignatureInvalid
  std::string detail;
  std::string release_name, release_version;
  std::uint32_t program_id = 0;                      // kInvalidLiveSet / kDuplicateProgramId: the entry
  std::vector<MissingReference> missing;             // kUnresolvedReferences
  std::vector<std::uint32_t> imported;               // program ids written for the first time
  std::vector<std::uint32_t> updated;                // replaced by a newer release
  std::vector<std::uint32_t> unchanged;              // already there with this (or a newer) release
  std::vector<std::uint32_t> deleted_by_user;        // recorded in the state, file gone: not brought back
  std::vector<std::uint32_t> conflicts;              // a file with the id but no factory mark: not touched
  bool ok() const { return status == FactoryStatus::kOk; }
  bool changed() const { return !imported.empty() || !updated.empty(); }
};

// Control thread only (the LiveSetStore's owner). `container_bytes` is the content of factory-live-sets.vfls.
InstallResult install_factory_live_sets(liveset::LiveSetStore& store, std::string_view container_bytes, const InstallOptions& opt);
// Reads `path` (a regular file of at most kMaxFileBytes) and installs it.
InstallResult install_factory_live_sets_file(liveset::LiveSetStore& store, const std::string& path, const InstallOptions& opt);

// True when the Live Set carries the factory mark (LiveSet.factory is present and its program id > 0).
bool is_factory(const lsp::LiveSet& ls);

}  // namespace velora::host::factory
