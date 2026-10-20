// Story 6.2: encryption and signing for protected bundles. Used by tools (content-pack) and tests; never shipped on a device.
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <memory>

#include "velora/format/protected_writer.hpp"

namespace velora::format {
namespace {
struct PkeyDeleter {
  void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); }
};
struct MdDeleter {
  void operator()(EVP_MD_CTX* p) const { EVP_MD_CTX_free(p); }
};
struct CipherDeleter {
  void operator()(EVP_CIPHER_CTX* c) const { EVP_CIPHER_CTX_free(c); }
};
std::unique_ptr<EVP_PKEY, PkeyDeleter> private_key(const SigningSeed& seed) {
  return std::unique_ptr<EVP_PKEY, PkeyDeleter>(
      EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.bytes.data(), seed.bytes.size()));
}
}  // namespace

PublicKey ed25519_public_from_seed(const SigningSeed& seed) {
  PublicKey pub{};
  auto key = private_key(seed);
  std::size_t len = pub.size();
  if (!key || EVP_PKEY_get_raw_public_key(key.get(), pub.data(), &len) != 1 || len != pub.size()) pub.fill(0);
  return pub;
}

Result<Signature> ed25519_sign(const SigningSeed& seed, std::span<const std::uint8_t> message) {
  auto key = private_key(seed);
  if (!key) return Error::kInvalidArgument;
  std::unique_ptr<EVP_MD_CTX, MdDeleter> ctx(EVP_MD_CTX_new());
  if (!ctx || EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1) return Error::kIo;
  Signature sig{};
  std::size_t len = sig.size();
  if (EVP_DigestSign(ctx.get(), sig.data(), &len, message.data(), message.size()) != 1 || len != sig.size())
    return Error::kIo;
  return sig;
}

Result<std::array<std::uint8_t, kCatalogMetaSealSize>> sign_catalog_meta(const SigningSeed& seed, const Digest256& bundle_id,
                                                                         std::span<const std::uint8_t> meta_bytes, std::uint32_t pack_revision) {
  if (pack_revision == 0) return Error::kInvalidArgument;
  const KeyId signer = key_id_of_public(ed25519_public_from_seed(seed));
  auto sig = ed25519_sign(seed, catalog_meta_statement(bundle_id, meta_bytes, signer, pack_revision));
  if (!sig) return sig.error();
  return encode_catalog_meta_seal(signer, pack_revision, sig.value());
}

Error aes256_ctr_xor(const ContentKey& key, const CounterBlock& counter, std::span<std::uint8_t> data) {
  std::unique_ptr<EVP_CIPHER_CTX, CipherDeleter> ctx(EVP_CIPHER_CTX_new());
  if (!ctx || EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_ctr(), nullptr, key.bytes.data(), counter.data()) != 1)
    return Error::kIo;
  if (data.empty()) return Error::kNone;
  int outl = 0;
  if (EVP_EncryptUpdate(ctx.get(), data.data(), &outl, data.data(), static_cast<int>(data.size())) != 1 ||
      static_cast<std::size_t>(outl) != data.size())
    return Error::kIo;
  return Error::kNone;
}

Result<std::vector<std::uint8_t>> finalize_protected(const BundleWriter& w, const ContentKey& key, const Nonce& nonce,
                                                     const SigningSeed& seed) {
  ProtectParams p;
  p.nonce = nonce;
  p.key_check = key_check_value(key.bytes, nonce);
  p.signer_key_id = key_id_of_public(ed25519_public_from_seed(seed));
  p.encrypt = [&key, nonce](std::uint32_t idx, std::span<std::uint8_t> data) {
    return aes256_ctr_xor(key, counter_block(nonce, idx), data);
  };
  p.sign = [&seed](std::span<const std::uint8_t> msg, Signature& out) {
    auto s = ed25519_sign(seed, msg);
    if (!s) return s.error();
    out = s.value();
    return Error::kNone;
  };
  return w.finalize_protected(p);
}

Result<std::vector<std::uint8_t>> finalize_protected(const BundleWriter& w, const ContentKey& key, const SigningSeed& seed) {
  auto nonce = w.derive_nonce();
  if (!nonce) return nonce.error();
  return finalize_protected(w, key, nonce.value(), seed);
}

Error finalize_protected_to_file(const BundleWriter& w, const ContentKey& key, const Nonce& nonce,
                                 const SigningSeed& seed, const std::string& path) {
  auto bytes = finalize_protected(w, key, nonce, seed);
  if (!bytes) return bytes.error();
  const std::string tmp = path + ".tmp";
  ::unlink(tmp.c_str());  // a stale temp from a crash; unlink never follows a symlink
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
  if (fd < 0) return Error::kIo;
  const std::vector<std::uint8_t>& v = bytes.value();
  std::size_t done = 0;
  bool ok = true;
  while (ok && done < v.size()) {
    const ssize_t n = ::write(fd, v.data() + done, v.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) ok = false;
    else done += static_cast<std::size_t>(n);
  }
  ok = ::fsync(fd) == 0 && ok;
  ok = ::close(fd) == 0 && ok;
  if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return Error::kIo;
  }
  std::filesystem::path dir = std::filesystem::path(path).parent_path();
  if (dir.empty()) dir = ".";
  const int dfd = ::open(dir.c_str(), O_RDONLY);
  if (dfd >= 0) {
    const bool synced = ::fsync(dfd) == 0;
    ::close(dfd);
    if (!synced) return Error::kIo;
  }
  return Error::kNone;
}

}  // namespace velora::format
