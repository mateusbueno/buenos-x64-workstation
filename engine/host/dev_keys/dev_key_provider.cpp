#include "velora/host/dev_key_provider.hpp"

#include <fcntl.h>
#include <openssl/evp.h>
#include <unistd.h>

#include <cstring>
#include <memory>
#include <vector>

#include "velora/format/hash.hpp"
#include "velora/format/protected_format.hpp"

namespace velora::host {

namespace fmt = velora::format;
namespace ports = velora::ports;

namespace {

struct PkeyDel {
  void operator()(EVP_PKEY* k) const { EVP_PKEY_free(k); }
};
struct PkeyCtxDel {
  void operator()(EVP_PKEY_CTX* c) const { EVP_PKEY_CTX_free(c); }
};
struct CipherDel {
  void operator()(EVP_CIPHER_CTX* c) const { EVP_CIPHER_CTX_free(c); }  // cleanses the context
};
using Pkey = std::unique_ptr<EVP_PKEY, PkeyDel>;

constexpr std::size_t kHeader = 45;  // magic .. ephemeral key: the first part of the AAD
constexpr char kInfoTag[] = "velora/wrapped_key_v1/";

// RFC 5869 HKDF-SHA256 for one output block (L = 32), on formatlib's HMAC-SHA256. `out` is the caller's locked memory. The intermediate PRK, the
// info || counter buffer and the output block are wiped.
void hkdf32(std::span<const std::uint8_t> ikm, std::span<const std::uint8_t> salt, std::span<const std::uint8_t> info, std::span<std::uint8_t, 32> out) {
  fmt::Digest256 prk = fmt::hmac_sha256(salt, ikm);
  std::vector<std::uint8_t> t(info.begin(), info.end());
  t.push_back(0x01);
  fmt::Digest256 okm = fmt::hmac_sha256(prk, t);
  std::memcpy(out.data(), okm.data(), 32);
  secure_wipe(prk.data(), prk.size());
  secure_wipe(okm.data(), okm.size());
  secure_wipe(t.data(), t.size());
}

bool x25519_public_of(std::span<const std::uint8_t, 32> priv, std::array<std::uint8_t, 32>& pub) {
  Pkey k(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, priv.data(), priv.size()));
  std::size_t len = pub.size();
  return k && EVP_PKEY_get_raw_public_key(k.get(), pub.data(), &len) == 1 && len == pub.size();
}

// shared = X25519(priv, peer) into `out`; false on failure or an all-zero result (a low-order peer key).
bool x25519_shared(std::span<const std::uint8_t, 32> priv, std::span<const std::uint8_t, 32> peer, std::span<std::uint8_t, 32> out) {
  Pkey a(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, priv.data(), priv.size()));
  Pkey b(EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peer.data(), peer.size()));
  if (!a || !b) return false;
  std::unique_ptr<EVP_PKEY_CTX, PkeyCtxDel> ctx(EVP_PKEY_CTX_new(a.get(), nullptr));
  std::size_t len = out.size();
  if (!ctx || EVP_PKEY_derive_init(ctx.get()) != 1 || EVP_PKEY_derive_set_peer(ctx.get(), b.get()) != 1 || EVP_PKEY_derive(ctx.get(), out.data(), &len) != 1 ||
      len != out.size()) {
    secure_wipe(out.data(), out.size());
    return false;
  }
  std::uint8_t acc = 0;
  for (std::uint8_t v : out) acc |= v;
  if (acc == 0) return false;
  return true;
}

// AES-256-GCM open of 32 bytes straight into `out` (the key page). The plaintext is only released by the caller when this returns true.
bool gcm_open32(std::span<const std::uint8_t, 32> key, const std::uint8_t* nonce12, std::span<const std::uint8_t> aad, const std::uint8_t* ct, const std::uint8_t* tag,
                std::span<std::uint8_t, 32> out) {
  std::unique_ptr<EVP_CIPHER_CTX, CipherDel> c(EVP_CIPHER_CTX_new());
  int l = 0;
  if (!c || EVP_DecryptInit_ex(c.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 || EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1 ||
      EVP_DecryptInit_ex(c.get(), nullptr, nullptr, key.data(), nonce12) != 1 ||
      EVP_DecryptUpdate(c.get(), nullptr, &l, aad.data(), static_cast<int>(aad.size())) != 1 ||
      EVP_DecryptUpdate(c.get(), out.data(), &l, ct, 32) != 1 || l != 32 ||
      EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_GCM_SET_TAG, 16, const_cast<std::uint8_t*>(tag)) != 1)
    return false;
  int f = 0;
  return EVP_DecryptFinal_ex(c.get(), out.data() + l, &f) == 1;
}

}  // namespace

namespace {
// The options of a page that is not a returned content key: the same lock hooks, its own wipe observer.
SecureKeyHandle::Options page_options(const DevKeyProvider::Options& opt, SecureKeyHandle::WipeObserver observer) {
  SecureKeyHandle::Options o = opt.key_memory;
  o.on_wipe = std::move(observer);
  return o;
}
}  // namespace

std::unique_ptr<DevKeyProvider> DevKeyProvider::from_bytes(std::span<const std::uint8_t, 32> priv, const Options& opt, std::string& err) {
  ports::KeyStatus st = ports::KeyStatus::kOk;
  auto h = SecureKeyHandle::allocate(page_options(opt, opt.device_key_on_wipe), st);
  if (!h) {
    err = std::string("cannot hold the device key in locked memory: ") + ports::to_string(st);
    return nullptr;
  }
  std::memcpy(h->writable().data(), priv.data(), 32);
  std::unique_ptr<DevKeyProvider> p(new DevKeyProvider());
  p->opt_ = opt;
  p->device_private_ = std::move(h);
  if (!x25519_public_of(p->device_private_->bytes(), p->device_public_)) {
    err = "not a usable X25519 private key";
    return nullptr;
  }
  fmt::PublicKey pub;
  std::copy(p->device_public_.begin(), p->device_public_.end(), pub.begin());
  p->device_key_id_ = fmt::key_id_of_public(pub);
  return p;
}

std::unique_ptr<DevKeyProvider> DevKeyProvider::from_file(const std::string& path, const Options& opt, std::string& err) {
  // Read straight into the locked page: the key is in no other buffer of ours.
  ports::KeyStatus st = ports::KeyStatus::kOk;
  auto h = SecureKeyHandle::allocate(page_options(opt, opt.device_key_on_wipe), st);
  if (!h) {
    err = std::string("cannot hold the device key in locked memory: ") + ports::to_string(st);
    return nullptr;
  }
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    err = path + ": cannot be opened";
    return nullptr;
  }
  std::uint8_t* dst = h->writable().data();
  std::size_t got = 0;
  while (got < 32) {
    const ssize_t n = ::read(fd, dst + got, 32 - got);
    if (n <= 0) break;
    got += static_cast<std::size_t>(n);
  }
  std::uint8_t extra = 0;
  const bool more = got == 32 && ::read(fd, &extra, 1) > 0;
  ::close(fd);
  if (got != 32 || more) {
    err = path + ": a device key file is exactly 32 raw bytes (device.x25519.priv)";
    return nullptr;
  }
  std::unique_ptr<DevKeyProvider> p(new DevKeyProvider());
  p->opt_ = opt;
  p->device_private_ = std::move(h);
  if (!x25519_public_of(p->device_private_->bytes(), p->device_public_)) {
    err = path + ": not a usable X25519 private key";
    return nullptr;
  }
  fmt::PublicKey pub;
  std::copy(p->device_public_.begin(), p->device_public_.end(), pub.begin());
  p->device_key_id_ = fmt::key_id_of_public(pub);
  return p;
}

DevKeyProvider::~DevKeyProvider() = default;

ports::KeyResult DevKeyProvider::unwrap_content_key(std::string_view library_id, std::span<const std::uint8_t> blob) {
  ports::KeyResult res;
  // The library id is bound into the wrap as bytes; real libraries use the canonical lower-case UUID text (the pinned vector with a free-form id is for tests, which
  // switch the rule off with Options::require_uuid_library_id = false).
  fmt::Uuid parsed{};
  if (library_id.empty() || library_id.size() > 1024 || (opt_.require_uuid_library_id && !fmt::uuid_from_string(library_id, parsed))) {
    res.status = ports::KeyStatus::kInvalidArgument;
    return res;
  }
  if (opt_.require_uuid_library_id && fmt::uuid_to_string(parsed) != library_id) {  // lower-case canonical only: one spelling per library
    res.status = ports::KeyStatus::kInvalidArgument;
    return res;
  }
  // A reader MUST check the magic, the algorithm, the total length and the recipient key id before unwrapping (KEY-HIERARCHY.md).
  if (blob.size() != kWrappedKeyBytes || std::memcmp(blob.data(), "VWK1", 4) != 0 || blob[4] != 0x01) {
    res.status = ports::KeyStatus::kMalformedBlob;
    return res;
  }
  if (std::memcmp(blob.data() + 5, device_key_id_.data(), 8) != 0) {
    res.status = ports::KeyStatus::kWrongDevice;
    return res;
  }
  // ONE locked page holds everything secret about this unwrap: the content key (the first 32 bytes, kept) and, in the rest of the page, the shared secret and the
  // wrapping key, which are wiped with the page whatever the outcome.
  ports::KeyStatus st = ports::KeyStatus::kOk;
  auto out = SecureKeyHandle::allocate(opt_.key_memory, st);
  if (!out) {
    res.status = st == ports::KeyStatus::kOk ? ports::KeyStatus::kInternal : st;
    return res;
  }
  const std::span<std::uint8_t, 32> shared_w(out->extra(0, 32).data(), 32), wrap_w(out->extra(32, 32).data(), 32);
  struct Temps {
    SecureKeyHandle& h;
    ~Temps() { secure_wipe(h.extra(0, 64).data(), 64); }
  } temps{*out};
  std::array<std::uint8_t, 32> eph_pub;
  std::memcpy(eph_pub.data(), blob.data() + 13, 32);
  if (!x25519_shared(device_private_->bytes(), eph_pub, shared_w)) {
    res.status = ports::KeyStatus::kUnwrapFailed;
    return res;
  }
  std::vector<std::uint8_t> salt(eph_pub.begin(), eph_pub.end());
  salt.insert(salt.end(), device_public_.begin(), device_public_.end());
  std::vector<std::uint8_t> info(kInfoTag, kInfoTag + sizeof kInfoTag - 1);
  info.insert(info.end(), library_id.begin(), library_id.end());
  hkdf32(shared_w, salt, info, wrap_w);
  std::vector<std::uint8_t> aad(blob.begin(), blob.begin() + kHeader);
  aad.insert(aad.end(), library_id.begin(), library_id.end());
  if (!gcm_open32(wrap_w, blob.data() + 45, aad, blob.data() + 57, blob.data() + 89, out->writable())) {
    res.status = ports::KeyStatus::kUnwrapFailed;  // `out` dies here, wiped: GCM may have released plaintext before the tag failed
    return res;
  }
  res.status = ports::KeyStatus::kOk;
  res.key = std::move(out);
  return res;
}

}  // namespace velora::host
