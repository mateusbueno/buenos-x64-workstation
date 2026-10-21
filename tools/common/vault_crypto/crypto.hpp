#pragma once
// Story 6.3 (moved to tools/common/vault_crypto in Story 6.8 so the content-pack tool and the device registry share ONE implementation, unchanged): the
// key-handling primitives of the vendor tools: vault_entry_v1 and wrapped_key_v1 (tools/keys/KEY-HIERARCHY.md section 6, the
// pinned vectors there are checked by tests/content_pack_test.cpp), the CSPRNG and secret hygiene (mlock, wipe). Vendor-side tooling only (the offline
// content-pack tool and the registry server, which is online but on the vendor's own server): nothing in a shipped target links this (tools/ci/check_format_no_decrypt.py proves the cipher symbols stay out of the shipped libraries).
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "velora/format/protected_format.hpp"

namespace velora::contentpack {

using Bytes = std::vector<std::uint8_t>;
using Key32 = std::array<std::uint8_t, 32>;
using Nonce12 = std::array<std::uint8_t, 12>;

inline constexpr std::size_t kVaultEntrySize = 72;
inline constexpr std::size_t kWrappedKeySize = 105;

// OPENSSL_cleanse: not optimised away.
void wipe(void* p, std::size_t n);

// A secret that is locked in RAM (best effort, no swap) while it lives and wiped when it dies. Not copyable or movable: secrets stay where they are made.
// CAUTION: it locks the pages its bytes sit on (a stack page, typically) and munlock()s them again at the end, and munlock does not nest: any other locked data
// on those pages is unlocked too. So a long-lived secret (the registry's KEK) must NOT be a Secret on a shared page; the registry keeps it in a dedicated mapping.
template <class T>
class Secret {
 public:
  Secret();
  ~Secret();
  Secret(const Secret&) = delete;
  Secret& operator=(const Secret&) = delete;
  T& get() { return v_; }
  const T& get() const { return v_; }
  T* operator->() { return &v_; }
  const T* operator->() const { return &v_; }

 private:
  T v_{};
};
void lock_memory(void* p, std::size_t n);
void unlock_memory(void* p, std::size_t n);
template <class T>
Secret<T>::Secret() {
  lock_memory(&v_, sizeof v_);
}
template <class T>
Secret<T>::~Secret() {
  wipe(&v_, sizeof v_);
  unlock_memory(&v_, sizeof v_);
}

// OS CSPRNG (OpenSSL RAND_bytes, seeded from the OS). false on failure.
bool random_bytes(std::span<std::uint8_t> out);

// First 8 bytes of SHA-256("velora-kek-id:" || KEK).
velora::format::KeyId kek_id(const Key32& kek);

// vault_entry_v1: AES-256-GCM(KEK) over the content key. `library_id` is the canonical lower-case UUID text (36 bytes), bound as AAD.
std::array<std::uint8_t, kVaultEntrySize> vault_entry_seal(const Key32& kek, const Key32& content_key, std::string_view library_id,
                                                           const Nonce12& nonce);
// false for a wrong magic/size/KEK id/library_id/tag; `out` is wiped on failure.
bool vault_entry_open(const Key32& kek, std::span<const std::uint8_t> entry, std::string_view library_id, Key32& out);
// Nonce for the --deterministic-vault mode: first 12 bytes of HMAC-SHA256(content_key, "velora/vault-nonce/v1" 0x00 || library_id). Unique per (content key,
// library); the same key sealed again for the same library gives the same bytes, which reveal nothing (the plaintext is the same too).
Nonce12 vault_nonce_deterministic(const Key32& content_key, std::string_view library_id);

// AES-256-GCM, 12-byte nonce, 16-byte tag. seal: `ct` (same length as `pt`) and `tag`; open: false when the tag does not verify (the plaintext buffer is then
// meaningless and the caller wipes it). Exposed for the registry's backup format (Story 6.8); vault_entry_* and wrapped_key_* are built on them.
bool gcm_seal(const Key32& key, const Nonce12& nonce, std::span<const std::uint8_t> aad, std::span<const std::uint8_t> pt, std::uint8_t* ct,
              std::uint8_t* tag);
bool gcm_open(const Key32& key, const Nonce12& nonce, std::span<const std::uint8_t> aad, const std::uint8_t* ct, std::size_t n, const std::uint8_t* tag,
              std::uint8_t* pt);
// RFC 5869 HKDF-SHA256 for one 32-byte output block; intermediates are wiped.
void hkdf32(std::span<const std::uint8_t> ikm, std::span<const std::uint8_t> salt, std::span<const std::uint8_t> info, Key32& out);

// X25519 public key of a (raw, unclamped) private scalar. All zero on failure.
Key32 x25519_public(const Key32& priv);

// wrapped_key_v1 for one device. `eph_priv` and `nonce` are fresh random per wrap (the explicit form exists for the pinned vector test).
bool wrapped_key_seal(const Key32& content_key, std::string_view library_id, const Key32& device_pub, const Key32& eph_priv, const Nonce12& nonce,
                      std::array<std::uint8_t, kWrappedKeySize>& out);
bool wrapped_key_seal_random(const Key32& content_key, std::string_view library_id, const Key32& device_pub,
                             std::array<std::uint8_t, kWrappedKeySize>& out);
// The device side (dev device key on the Mac in tests; Story 6.5 reimplements it in OP-TEE). false on any failure; `out` is wiped on failure.
bool wrapped_key_open(const Key32& device_priv, std::span<const std::uint8_t> blob, std::string_view library_id, Key32& out);

}  // namespace velora::contentpack
