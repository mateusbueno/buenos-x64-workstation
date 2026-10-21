#pragma once
// Story 6.2: verification of protected bundles WITHOUT any decrypt path (target velora_format_verify). This is what
// the desktop build, the librarian (Story 6.4) and every other non-playback consumer links: Ed25519 manifest
// signature verification (strict, see ed25519_verify_strict), index hash, hash-list root and ciphertext
// block-hash verification. The content key is never seen here. tools/ci/check_format_no_decrypt.py proves from the
// symbols of this target that no cipher or decrypt entry point is linked.
//
// Thread-safety: ProtectedBundleView is immutable after open(); const methods may run on several threads.
// RT-safety: NOT real-time safe (allocates, does file I/O, hashes). Call it from I/O or control threads only.
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "velora/format/bundle.hpp"

namespace velora::format {

struct TrustedKey {
  KeyId id{};        // first 8 bytes of SHA-256(pub)
  PublicKey pub{};   // Ed25519 content-manifest public key
  std::string root{};  // Story 6.9: the root of the trust-list line this key came from ("dev", "factory-test", "production"); empty = not bound to a root
};

// The trusted content-manifest keys of an image variant (tools/keys: velora_trusted_keys_<variant>.hpp).
class TrustedKeys {
 public:
  TrustedKeys() = default;
  explicit TrustedKeys(std::vector<TrustedKey> keys) : keys_(std::move(keys)) {}
  // Builds from the generated header's `kTrustedContentKeys` (any container of {id, pub}).
  template <class Container>
  static TrustedKeys from_generated(const Container& c) {
    TrustedKeys t;
    for (const auto& k : c) t.keys_.push_back(TrustedKey{k.id, k.pub});
    return t;
  }
  // The entry for `id`, or nullptr when there is none or when the id is listed more than once: a duplicated key id is ambiguous
  // (two keys claiming one id) and is never trusted.
  const TrustedKey* find(const KeyId& id) const {
    const TrustedKey* hit = nullptr;
    for (const TrustedKey& k : keys_) {
      if (k.id != id) continue;
      if (hit) return nullptr;
      hit = &k;
    }
    return hit;
  }
  std::size_t size() const { return keys_.size(); }

 private:
  std::vector<TrustedKey> keys_;
};

// Pure Ed25519 (RFC 8032) verification with the strictness of tools/keys/KEY-HIERARCHY.md section 6: rejects
// S >= L, a non-canonical R or A (y >= p), a small-order A, and checks cofactorless ([S]B == R + [k]A exactly).
bool ed25519_verify_strict(const PublicKey& pub, std::span<const std::uint8_t> message, const Signature& sig);

// Story 6.4: checks the detached seal of a catalog.meta (protected_format.hpp, "catalog.meta binding") for the bundle `bundle_id`: kCatalogMetaInvalid for a
// malformed seal, kUntrustedKey when the signer is not in `trusted`, kSignatureInvalid when the signature does not verify (catalog.meta or seal altered, or
// made for another bundle).
// `signer_out` / `revision_out` (optional) receive the seal's signer key id and pack_revision once it decodes.
Error verify_catalog_meta_seal(const TrustedKeys& trusted, const Digest256& bundle_id, std::span<const std::uint8_t> meta_bytes,
                               std::span<const std::uint8_t> seal_bytes, KeyId* signer_out = nullptr, std::uint32_t* revision_out = nullptr);

struct ProtectedInfo {
  std::uint32_t format_version = 0;  // major, 3
  std::uint32_t format_minor = 0;
  Uuid library_id{};                 // vendor library id
  Digest256 bundle_id{};             // SHA-256 of the manifest bytes
  Digest256 index_hash{};
  Digest256 hash_list_root{};
  Nonce nonce{};
  KeyCheck key_check{};
  KeyId signer_key_id{};
};

class ProtectedBundleView {
 public:
  // Verifies, in this order: header layout, manifest and index hashes, the signer key id against `trusted`
  // (kUntrustedKey), the Ed25519 signature over the manifest statement (kSignatureInvalid), the manifest body and
  // index structure, and the hash-list root (kHashListMismatch). kNotProtected for an unprotected bundle,
  // kUnsupportedVersion for a version this reader does not know.
  static Result<ProtectedBundleView> open_memory(std::span<const std::uint8_t> bytes, const TrustedKeys& trusted);
  static Result<ProtectedBundleView> open_source(std::unique_ptr<ByteSource> src, const TrustedKeys& trusted);
  static Result<ProtectedBundleView> open_file(const std::string& path, const TrustedKeys& trusted);

  ProtectedBundleView(ProtectedBundleView&&) noexcept;
  ProtectedBundleView& operator=(ProtectedBundleView&&) noexcept;
  ~ProtectedBundleView();

  const ProtectedInfo& info() const;
  std::span<const InstrumentInfo> instruments() const;
  std::span<const SampleInfo> samples() const;
  std::uint64_t total_blocks() const;

  // One block as stored: `out` is resized to the ciphertext and filled, but only after its SHA-256 matched the
  // index; on kBlockHashMismatch `out` is cleared. `global_block` is the ordinal into the index table.
  Error read_ciphertext(std::uint64_t global_block, std::vector<std::uint8_t>& out) const;
  // Location of block `block` of sample `sample`.
  struct BlockRef {
    std::uint64_t global = 0;
    std::uint32_t size = 0;
    std::uint32_t frames = 0;
  };
  Result<BlockRef> locate(std::size_t sample, std::uint64_t block) const;
  // Hashes every ciphertext block against the index (no decrypt). First failure wins.
  Error verify_all_blocks() const;

 private:
  struct Impl;
  ProtectedBundleView();
  static Result<ProtectedBundleView> open(std::unique_ptr<ByteSource> src, const TrustedKeys& trusted);
  std::unique_ptr<Impl> impl_;
};

}  // namespace velora::format
