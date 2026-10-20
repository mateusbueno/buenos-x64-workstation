#pragma once
// Story 6.2: the protected variant of the block-indexed bundle (format_version 3). Pure data and
// hashing helpers: nothing in this header or in the `velora_format` core target encrypts, decrypts
// or signs; those live in the separate verify / decrypt / protected-writer targets (see README).
//
// File layout (little-endian):
//   header (128 B, same fields as format 2, version word = 3 | minor << 16)
//   manifest   (format 2/2.1 manifest with a protection section after the index hash)
//   index      (48 B per block; the SHA-256 is over the CIPHERTEXT bytes of the block)
//   seal       (128 B: signing key id and Ed25519 signature)
//   blocks     (AES-256-CTR ciphertext of the FLAC / raw float block bytes)
// Byte-exact field tables are in formatlib/README.md.
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "velora/format/error.hpp"
#include "velora/format/hash.hpp"

namespace velora::format {

inline constexpr std::uint32_t kProtectedFormatVersion = 3;
inline constexpr std::uint8_t kCipherAes256Ctr = 1;
inline constexpr std::uint8_t kHashListMerkleSha256 = 1;
inline constexpr std::uint8_t kSealAlgEd25519 = 1;
inline constexpr std::size_t kSealSize = 128;
inline constexpr char kSealMagic[4] = {'V', 'S', 'L', '1'};

// Counter block (16 bytes) of AES-256-CTR for byte 0 of global block `b`:
//   nonce[8] || big-endian u32 global_block_index || big-endian u32 intra-block counter (0 at the block start).
// The CTR increment runs over the whole 128-bit block, i.e. it carries from the intra-block counter into the
// block-index field only after 2^32 AES blocks (64 GiB) inside one block. kMaxProtectedBlockBytes caps every
// block far below that, so a counter block is never reused: (nonce, global block index, 16-byte offset) is
// unique, provided the nonce is unique per content key (writer contract) and the index fits 32 bits.
inline constexpr std::uint32_t kMaxProtectedBlockBytes = 1u << 20;  // FLAC block <= 4096 frames * 2 ch * 3 B + 1 KiB framing
inline constexpr std::uint64_t kMaxProtectedBlocks = std::uint64_t(1) << 32;
static_assert(std::uint64_t(kMaxProtectedBlockBytes) / 16 <= (std::uint64_t(1) << 32) - 1,
              "intra-block counter must not carry into the block-index field");

using Nonce = std::array<std::uint8_t, 8>;
using KeyId = std::array<std::uint8_t, 8>;
using Signature = std::array<std::uint8_t, 64>;
using PublicKey = std::array<std::uint8_t, 32>;
using KeyCheck = std::array<std::uint8_t, 16>;
using CounterBlock = std::array<std::uint8_t, 16>;

// The per-library 256-bit content key (raw bytes).
struct ContentKey {
  std::array<std::uint8_t, 32> bytes{};
};
// The Ed25519 signing seed (RFC 8032, 32 bytes). Only the signing target and tools use it.
struct SigningSeed {
  std::array<std::uint8_t, 32> bytes{};
};

CounterBlock counter_block(const Nonce& nonce, std::uint32_t global_block_index);

// HMAC-SHA256 (RFC 2104) built on the in-house SHA-256.
Digest256 hmac_sha256(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data);

// key_check = first 16 bytes of HMAC-SHA256(content_key, "velora/key-check/v1" || 0x00 || nonce).
KeyCheck key_check_value(std::span<const std::uint8_t, 32> content_key, const Nonce& nonce);

// Merkle root over the per-block ciphertext hashes in global block order:
//   leaf = SHA-256(0x00 || block_hash), node = SHA-256(0x01 || left || right); an odd node at the end of a
//   level is promoted unchanged; the root of an empty list is SHA-256 of the empty string.
Digest256 hash_list_root(std::span<const Digest256> block_hashes);

// First 8 bytes of SHA-256(public key): the key id used by the trusted-key lists (tools/keys).
KeyId key_id_of_public(const PublicKey& pub);

// Constant-time equality for secrets and check values.
bool ct_equal(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b);

// The signed statement (domain separated, starts with an ASCII context string unique to this type):
//   "velora/protected-manifest/v1" 0x00 | u32 version_word | library_id[16] | bundle_id[32] | index_sha256[32] |
//   hash_list_root[32] | u64 block_count | nonce[8] | signer_key_id[8]
// bundle_id is SHA-256 of the manifest bytes (which already contain the other fields; they are repeated so the
// statement reads as the manifest summary the story asks for).
struct ManifestStatement {
  std::uint32_t version_word = 0;
  Uuid library_id{};
  Digest256 bundle_id{};
  Digest256 index_sha256{};
  Digest256 hash_list_root{};
  std::uint64_t block_count = 0;
  Nonce nonce{};
  KeyId signer_key_id{};
};
std::vector<std::uint8_t> statement_bytes(const ManifestStatement& s);

// Story 6.4: the signed binding of `catalog.meta` (the librarian's display metadata: library name, instrument names and categories) to ONE protected bundle, and
// the pack revision. catalog.meta is not part of the signed manifest (it can be refreshed without making a new bundle), so a pack carries `catalog.meta.sig`, a
// detached seal made with the same content-manifest key as the bundle:
//   file (84 B): "VCM2" | u8 alg = 1 (Ed25519) | u8[3] zero | u32 pack_revision (LE) | signer key id[8] | Ed25519 signature[64]
//   signed statement: "velora/catalog-meta/v1" 0x00 | bundle_id[32] | SHA-256(catalog.meta bytes)[32] | signer key id[8] | u32 pack_revision (LE)
// bundle_id is in the statement, so a seal made for one bundle never fits another, and one flipped bit in catalog.meta or in the seal breaks the signature.
// `pack_revision` (>= 1) is a monotonic per-library counter the vendor raises with every release: a device installs a revision only when it is not lower than the one
// installed (replaying an older signed bundle is a downgrade), and the same revision only when it is the same bundle. A deliberate vendor rollback is a NEW,
// higher revision that carries the old content.
inline constexpr std::size_t kCatalogMetaSealSize = 84;
inline constexpr char kCatalogMetaSealMagic[4] = {'V', 'C', 'M', '2'};
inline constexpr const char* kCatalogMetaSealFileName = "catalog.meta.sig";
std::vector<std::uint8_t> catalog_meta_statement(const Digest256& bundle_id, std::span<const std::uint8_t> meta_bytes, const KeyId& signer, std::uint32_t pack_revision);
std::array<std::uint8_t, kCatalogMetaSealSize> encode_catalog_meta_seal(const KeyId& signer, std::uint32_t pack_revision, const Signature& sig);
// false: wrong size, magic, algorithm, non-zero reserved bytes or a revision of 0.
bool decode_catalog_meta_seal(std::span<const std::uint8_t> bytes, KeyId& signer, std::uint32_t& pack_revision, Signature& sig);

// What BundleWriter::finalize_protected needs from the signing/encrypting layer. The core writer builds the
// bytes; the callbacks do the cryptography.
struct ProtectParams {
  // Unique per content key and never all zero (kInvalidArgument otherwise). The nonce is 64 bits, so random nonces under ONE key collide with probability
  // ~n^2/2^65 for n bundles: stay far below 2^32 bundles per key (derive_nonce() is content-derived and has the same bound).
  Nonce nonce{};
  KeyCheck key_check{};   // key_check_value(content_key, nonce)
  KeyId signer_key_id{};  // key_id_of_public(signing public key)
  // Encrypts one block in place; `global_block_index` selects the counter block.
  std::function<Error(std::uint32_t global_block_index, std::span<std::uint8_t> data)> encrypt;
  // Signs the statement bytes (pure Ed25519).
  std::function<Error(std::span<const std::uint8_t> message, Signature& out)> sign;
};

}  // namespace velora::format
