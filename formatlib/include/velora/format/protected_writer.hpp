#pragma once
// Story 6.2: producing protected bundles (target velora_format_protected_writer, used by tools/content-pack, Story
// 6.3, and by tests). Shipped device code never links this: it holds the signing and encrypting entry points.
#include <string>

#include "velora/format/bundle.hpp"

namespace velora::format {

// Ed25519 public key of an RFC 8032 seed.
PublicKey ed25519_public_from_seed(const SigningSeed& seed);
// Pure Ed25519 signature (deterministic).
Result<Signature> ed25519_sign(const SigningSeed& seed, std::span<const std::uint8_t> message);

// Story 6.4: the detached seal (catalog.meta.sig) that binds a catalog.meta to the protected bundle `bundle_id`, signed with the bundle's own key.
Result<std::array<std::uint8_t, kCatalogMetaSealSize>> sign_catalog_meta(const SigningSeed& seed, const Digest256& bundle_id,
                                                                         std::span<const std::uint8_t> meta_bytes, std::uint32_t pack_revision = 1);

// AES-256-CTR with a full 16-byte counter block (also decryption). Exposed for the known-answer tests and the
// content-pack tool; the bundle path derives the counter block with counter_block().
Error aes256_ctr_xor(const ContentKey& key, const CounterBlock& counter, std::span<std::uint8_t> data);

// Finalizes `w` as a protected bundle: blocks encrypted under `key` with `nonce`, manifest signed with `seed`.
// PRECONDITION: `nonce` is unique per content key (a repeated nonce under one key reuses the AES-CTR keystream and
// leaks the XOR of two plaintexts) and is never all zero (kInvalidArgument). 64-bit nonces: keep far below 2^32
// bundles per key. Same inputs, same bytes.
Result<std::vector<std::uint8_t>> finalize_protected(const BundleWriter& w, const ContentKey& key, const Nonce& nonce,
                                                     const SigningSeed& seed);
// Same, with the content-derived nonce of BundleWriter::derive_nonce() (reproducible builds).
Result<std::vector<std::uint8_t>> finalize_protected(const BundleWriter& w, const ContentKey& key, const SigningSeed& seed);
// Writes through a fresh temp file (O_EXCL | O_NOFOLLOW, mode 0644), fsyncs it, renames it over `path` and fsyncs the directory.
Error finalize_protected_to_file(const BundleWriter& w, const ContentKey& key, const Nonce& nonce,
                                 const SigningSeed& seed, const std::string& path);

}  // namespace velora::format
