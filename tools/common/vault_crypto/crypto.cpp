#include "crypto.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sys/mman.h>

#include <cstring>
#include <memory>
#include <string>

#include "velora/format/hash.hpp"

namespace velora::contentpack {
namespace fmt = velora::format;
namespace {

struct PkeyDel {
  void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); }
};
struct CtxDel {
  void operator()(EVP_CIPHER_CTX* c) const { EVP_CIPHER_CTX_free(c); }
};
struct DerDel {
  void operator()(EVP_PKEY_CTX* c) const { EVP_PKEY_CTX_free(c); }
};
using PkeyPtr = std::unique_ptr<EVP_PKEY, PkeyDel>;

constexpr char kVaultAadTag[] = "velora/vault_entry_v1/";
constexpr char kWrapInfoTag[] = "velora/wrapped_key_v1/";

void append(Bytes& b, const void* p, std::size_t n) {
  const auto* c = static_cast<const std::uint8_t*>(p);
  b.insert(b.end(), c, c + n);
}
void append(Bytes& b, std::string_view s) { append(b, s.data(), s.size()); }

PkeyPtr x25519_private(const Key32& priv) {
  return PkeyPtr(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, priv.data(), priv.size()));
}
PkeyPtr x25519_public_key(const Key32& pub) {
  return PkeyPtr(EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, pub.data(), pub.size()));
}

// X25519 shared secret; false on failure or an all-zero result (KEY-HIERARCHY.md section 6).
bool x25519_shared(const Key32& priv, const Key32& peer_pub, Key32& out) {
  PkeyPtr a = x25519_private(priv), b = x25519_public_key(peer_pub);
  if (!a || !b) return false;
  std::unique_ptr<EVP_PKEY_CTX, DerDel> ctx(EVP_PKEY_CTX_new(a.get(), nullptr));
  std::size_t len = out.size();
  if (!ctx || EVP_PKEY_derive_init(ctx.get()) != 1 || EVP_PKEY_derive_set_peer(ctx.get(), b.get()) != 1 ||
      EVP_PKEY_derive(ctx.get(), out.data(), &len) != 1 || len != out.size()) {
    wipe(out.data(), out.size());
    return false;
  }
  std::uint8_t acc = 0;
  for (std::uint8_t v : out) acc |= v;
  if (acc == 0) return false;
  return true;
}

Bytes wrap_aad(std::span<const std::uint8_t> header45, std::string_view library_id) {
  Bytes aad(header45.begin(), header45.end());
  append(aad, library_id);
  return aad;
}

// Writes the wrapping key into `out` (the caller's Secret).
void wrapping_key(const Key32& shared, const Key32& eph_pub, const Key32& device_pub, std::string_view library_id, Key32& out) {
  Bytes salt;
  append(salt, eph_pub.data(), eph_pub.size());
  append(salt, device_pub.data(), device_pub.size());
  Bytes info;
  append(info, kWrapInfoTag, sizeof kWrapInfoTag - 1);
  append(info, library_id);
  hkdf32(shared, salt, info, out);
}

}  // namespace

// AES-256-GCM, 12-byte nonce, 16-byte tag. seal: ct (same length as pt) and tag; open: false when the tag does not verify.
bool gcm_seal(const Key32& key, const Nonce12& nonce, std::span<const std::uint8_t> aad, std::span<const std::uint8_t> pt,
              std::uint8_t* ct, std::uint8_t* tag) {
  std::unique_ptr<EVP_CIPHER_CTX, CtxDel> c(EVP_CIPHER_CTX_new());
  int l = 0;
  if (!c || EVP_EncryptInit_ex(c.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1 ||
      EVP_EncryptInit_ex(c.get(), nullptr, nullptr, key.data(), nonce.data()) != 1 ||
      EVP_EncryptUpdate(c.get(), nullptr, &l, aad.data(), static_cast<int>(aad.size())) != 1 ||
      EVP_EncryptUpdate(c.get(), ct, &l, pt.data(), static_cast<int>(pt.size())) != 1 || static_cast<std::size_t>(l) != pt.size())
    return false;
  int f = 0;
  if (EVP_EncryptFinal_ex(c.get(), ct + l, &f) != 1 || f != 0) return false;
  return EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_GCM_GET_TAG, 16, tag) == 1;
}

bool gcm_open(const Key32& key, const Nonce12& nonce, std::span<const std::uint8_t> aad, const std::uint8_t* ct, std::size_t n,
              const std::uint8_t* tag, std::uint8_t* pt) {
  std::unique_ptr<EVP_CIPHER_CTX, CtxDel> c(EVP_CIPHER_CTX_new());
  int l = 0;
  std::uint8_t t[16];
  std::memcpy(t, tag, 16);
  if (!c || EVP_DecryptInit_ex(c.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1 ||
      EVP_DecryptInit_ex(c.get(), nullptr, nullptr, key.data(), nonce.data()) != 1 ||
      EVP_DecryptUpdate(c.get(), nullptr, &l, aad.data(), static_cast<int>(aad.size())) != 1 ||
      EVP_DecryptUpdate(c.get(), pt, &l, ct, static_cast<int>(n)) != 1 || static_cast<std::size_t>(l) != n ||
      EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_GCM_SET_TAG, 16, t) != 1)
    return false;
  int f = 0;
  return EVP_DecryptFinal_ex(c.get(), pt + l, &f) == 1;
}

// RFC 5869 HKDF-SHA256 for one output block (L = 32), on the in-house HMAC. The intermediate PRK, the info||counter buffer and the output block are wiped.
void hkdf32(std::span<const std::uint8_t> ikm, std::span<const std::uint8_t> salt, std::span<const std::uint8_t> info, Key32& out) {
  fmt::Digest256 prk = fmt::hmac_sha256(salt, ikm);
  Bytes t(info.begin(), info.end());
  t.push_back(0x01);
  fmt::Digest256 okm = fmt::hmac_sha256(prk, t);
  out = okm;
  wipe(prk.data(), prk.size());
  wipe(okm.data(), okm.size());
  wipe(t.data(), t.size());
}

void wipe(void* p, std::size_t n) { OPENSSL_cleanse(p, n); }
void lock_memory(void* p, std::size_t n) { (void)::mlock(p, n); }  // best effort: a low RLIMIT_MEMLOCK must not stop an offline tool
void unlock_memory(void* p, std::size_t n) { (void)::munlock(p, n); }

bool random_bytes(std::span<std::uint8_t> out) { return RAND_bytes(out.data(), static_cast<int>(out.size())) == 1; }

fmt::KeyId kek_id(const Key32& kek) {
  Bytes m;
  append(m, std::string_view("velora-kek-id:"));
  append(m, kek.data(), kek.size());
  const fmt::Digest256 d = fmt::Sha256::hash(m);
  wipe(m.data(), m.size());
  fmt::KeyId id{};
  std::copy_n(d.begin(), id.size(), id.begin());
  return id;
}

std::array<std::uint8_t, kVaultEntrySize> vault_entry_seal(const Key32& kek, const Key32& content_key, std::string_view library_id,
                                                           const Nonce12& nonce) {
  std::array<std::uint8_t, kVaultEntrySize> e{};
  std::memcpy(e.data(), "VKV1", 4);
  const fmt::KeyId id = kek_id(kek);
  std::memcpy(e.data() + 4, id.data(), 8);
  std::memcpy(e.data() + 12, nonce.data(), 12);
  Bytes aad(e.begin(), e.begin() + 12);
  append(aad, std::string_view(kVaultAadTag));
  append(aad, library_id);
  if (!gcm_seal(kek, nonce, aad, content_key, e.data() + 24, e.data() + 56)) e.fill(0);
  return e;
}

bool vault_entry_open(const Key32& kek, std::span<const std::uint8_t> entry, std::string_view library_id, Key32& out) {
  wipe(out.data(), out.size());
  if (entry.size() != kVaultEntrySize || std::memcmp(entry.data(), "VKV1", 4) != 0) return false;
  const fmt::KeyId id = kek_id(kek);
  if (std::memcmp(entry.data() + 4, id.data(), 8) != 0) return false;
  Nonce12 nonce;
  std::memcpy(nonce.data(), entry.data() + 12, 12);
  Bytes aad(entry.begin(), entry.begin() + 12);
  append(aad, std::string_view(kVaultAadTag));
  append(aad, library_id);
  Key32 pt{};
  const bool ok = gcm_open(kek, nonce, aad, entry.data() + 24, 32, entry.data() + 56, pt.data());
  if (ok) out = pt;
  wipe(pt.data(), pt.size());
  return ok;
}

Nonce12 vault_nonce_deterministic(const Key32& content_key, std::string_view library_id) {
  Bytes m;
  append(m, std::string_view("velora/vault-nonce/v1"));
  m.push_back(0);
  append(m, library_id);
  fmt::Digest256 d = fmt::hmac_sha256(content_key, m);
  Nonce12 n;
  std::copy_n(d.begin(), n.size(), n.begin());
  wipe(d.data(), d.size());
  return n;
}

Key32 x25519_public(const Key32& priv) {
  Key32 pub{};
  PkeyPtr k = x25519_private(priv);
  std::size_t len = pub.size();
  if (!k || EVP_PKEY_get_raw_public_key(k.get(), pub.data(), &len) != 1 || len != pub.size()) pub.fill(0);
  return pub;
}

bool wrapped_key_seal(const Key32& content_key, std::string_view library_id, const Key32& device_pub, const Key32& eph_priv, const Nonce12& nonce,
                      std::array<std::uint8_t, kWrappedKeySize>& out) {
  out.fill(0);
  const Key32 eph_pub = x25519_public(eph_priv);
  Secret<Key32> shared;
  if (!x25519_shared(eph_priv, device_pub, shared.get())) return false;
  Secret<Key32> wk;
  wrapping_key(shared.get(), eph_pub, device_pub, library_id, wk.get());
  std::memcpy(out.data(), "VWK1", 4);
  out[4] = 0x01;
  const fmt::KeyId rid = fmt::key_id_of_public(device_pub);
  std::memcpy(out.data() + 5, rid.data(), 8);
  std::memcpy(out.data() + 13, eph_pub.data(), 32);
  std::memcpy(out.data() + 45, nonce.data(), 12);
  const Bytes aad = wrap_aad(std::span<const std::uint8_t>(out.data(), 45), library_id);
  if (!gcm_seal(wk.get(), nonce, aad, content_key, out.data() + 57, out.data() + 89)) {
    out.fill(0);
    return false;
  }
  return true;
}

bool wrapped_key_seal_random(const Key32& content_key, std::string_view library_id, const Key32& device_pub,
                             std::array<std::uint8_t, kWrappedKeySize>& out) {
  Secret<Key32> eph;
  Nonce12 nonce{};
  if (!random_bytes(eph.get()) || !random_bytes(nonce)) return false;
  return wrapped_key_seal(content_key, library_id, device_pub, eph.get(), nonce, out);
}

bool wrapped_key_open(const Key32& device_priv, std::span<const std::uint8_t> blob, std::string_view library_id, Key32& out) {
  wipe(out.data(), out.size());
  if (blob.size() != kWrappedKeySize || std::memcmp(blob.data(), "VWK1", 4) != 0 || blob[4] != 0x01) return false;
  const Key32 device_pub = x25519_public(device_priv);
  const fmt::KeyId rid = fmt::key_id_of_public(device_pub);
  if (std::memcmp(blob.data() + 5, rid.data(), 8) != 0) return false;
  Key32 eph_pub;
  std::memcpy(eph_pub.data(), blob.data() + 13, 32);
  Secret<Key32> shared;
  if (!x25519_shared(device_priv, eph_pub, shared.get())) return false;
  Secret<Key32> wk;
  wrapping_key(shared.get(), eph_pub, device_pub, library_id, wk.get());
  Nonce12 nonce;
  std::memcpy(nonce.data(), blob.data() + 45, 12);
  const Bytes aad = wrap_aad(blob.first(45), library_id);
  Key32 pt{};
  const bool ok = gcm_open(wk.get(), nonce, aad, blob.data() + 57, 32, blob.data() + 89, pt.data());
  if (ok) out = pt;
  wipe(pt.data(), pt.size());
  return ok;
}

}  // namespace velora::contentpack
