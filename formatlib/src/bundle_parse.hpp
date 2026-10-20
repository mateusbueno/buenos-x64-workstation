#pragma once
// Internal: the one parser of the bundle container, shared by BundleReader (unprotected, format 1/2) and the
// protected-bundle readers (format 3, Story 6.2). No cryptography here: signature verification is a callback
// supplied by the verify target.
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "velora/format/bundle.hpp"

namespace velora::format {

struct ProtectionFields {
  std::uint8_t cipher = 0;
  std::uint8_t hash_list_alg = 0;
  Nonce nonce{};
  KeyCheck key_check{};
  Digest256 hash_list_root{};
};
struct SealFields {
  std::uint8_t alg = 0;
  KeyId key_id{};
  Signature signature{};
};

struct ParsedBundle {
  std::uint32_t version_word = 0, version = 0, minor = 0;
  Digest256 bundle_id{};     // SHA-256 of the manifest bytes
  Digest256 index_hash{};
  Uuid library_id{};
  std::uint64_t block_count = 0;
  std::vector<InstrumentInfo> instruments;
  std::vector<SampleInfo> samples;
  std::vector<detail::BlockEntry> index;
  bool is_protected = false;
  ProtectionFields prot;
  SealFields seal;
};

// Called for a protected bundle after header, manifest hash, index hash, the fixed manifest fields and the seal
// were read and BEFORE the manifest body is parsed; `partial` carries version_word, library_id, bundle_id,
// index_hash, block_count, prot and seal. A non-kNone result aborts the open with that error.
using SealVerifier = std::function<Error(const ParsedBundle& partial)>;

// want_protected = false: formats 1 and 2 (kUnsupportedVersion for 3 and above, as before Story 6.2).
// want_protected = true: format 3 only (kNotProtected for an unprotected bundle, kUnsupportedVersion otherwise);
// `verify` must be set.
Result<ParsedBundle> parse_bundle(ByteSource& src, bool want_protected, const SealVerifier* verify);

std::unique_ptr<ByteSource> make_memory_source(std::span<const std::uint8_t> bytes);
Result<std::unique_ptr<ByteSource>> make_file_source(const std::string& path);

ManifestStatement statement_of(const ParsedBundle& p);

}  // namespace velora::format
