#include "velora/format/hash.hpp"

#include <cstring>
#include <vector>

namespace velora::format {
namespace {
constexpr std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
constexpr std::uint32_t rotl(std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

constexpr std::uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

// Shared Merkle-Damgard padding: appends 0x80, zeros and the 64-bit big-endian bit length.
template <class Self>
void finish_pad(Self& s, std::uint8_t* buf, std::size_t& len, std::uint64_t total_bytes) {
  const std::uint64_t bits = total_bytes * 8;
  std::uint8_t pad[72] = {0x80};
  const std::size_t padlen = (len < 56) ? (56 - len) : (120 - len);
  std::uint8_t lenb[8];
  for (int i = 0; i < 8; ++i) lenb[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
  s.update(pad, padlen);
  s.update(lenb, 8);
  (void)buf;
}
}  // namespace

Sha256::Sha256() { reset(); }

__attribute__((noinline)) void Sha256::wipe() noexcept {
  volatile std::uint8_t* v = reinterpret_cast<volatile std::uint8_t*>(h_);
  for (std::size_t i = 0; i < sizeof h_; ++i) v[i] = 0;
  volatile std::uint8_t* b = buf_;
  for (std::size_t i = 0; i < sizeof buf_; ++i) b[i] = 0;
  buf_len_ = 0;
  total_ = 0;
}

void Sha256::reset() {
  static constexpr std::uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::memcpy(h_, init, sizeof h_);
  buf_len_ = 0;
  total_ = 0;
}

void Sha256::block(const std::uint8_t* p) {
  std::uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) |
           (std::uint32_t(p[4 * i + 2]) << 8) | std::uint32_t(p[4 * i + 3]);
  for (int i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6],
                h = h_[7];
  for (int i = 0; i < 64; ++i) {
    const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ (~e & g);
    const std::uint32_t t1 = h + S1 + ch + K256[i] + w[i];
    const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t t2 = S0 + mj;
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}

void Sha256::update(const void* data, std::size_t n) {
  const auto* p = static_cast<const std::uint8_t*>(data);
  total_ += n;
  while (n > 0) {
    if (buf_len_ == 0 && n >= 64) {
      block(p);
      p += 64;
      n -= 64;
      continue;
    }
    const std::size_t take = (64 - buf_len_ < n) ? (64 - buf_len_) : n;
    std::memcpy(buf_ + buf_len_, p, take);
    buf_len_ += take;
    p += take;
    n -= take;
    if (buf_len_ == 64) {
      block(buf_);
      buf_len_ = 0;
    }
  }
}

Digest256 Sha256::finish() {
  const std::uint64_t total = total_;
  finish_pad(*this, buf_, buf_len_, total);
  Digest256 out;
  for (int i = 0; i < 8; ++i)
    for (int j = 0; j < 4; ++j) out[4 * i + j] = static_cast<std::uint8_t>(h_[i] >> (24 - 8 * j));
  return out;
}

Digest256 Sha256::hash(std::span<const std::uint8_t> d) {
  Sha256 s;
  s.update(d);
  return s.finish();
}

Sha1::Sha1() {
  h_[0] = 0x67452301; h_[1] = 0xEFCDAB89; h_[2] = 0x98BADCFE; h_[3] = 0x10325476; h_[4] = 0xC3D2E1F0;
  buf_len_ = 0;
  total_ = 0;
}

void Sha1::block(const std::uint8_t* p) {
  std::uint32_t w[80];
  for (int i = 0; i < 16; ++i)
    w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) |
           (std::uint32_t(p[4 * i + 2]) << 8) | std::uint32_t(p[4 * i + 3]);
  for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
  for (int i = 0; i < 80; ++i) {
    std::uint32_t f, k;
    if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
    else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
    else { f = b ^ c ^ d; k = 0xCA62C1D6; }
    const std::uint32_t t = rotl(a, 5) + f + e + k + w[i];
    e = d; d = c; c = rotl(b, 30); b = a; a = t;
  }
  h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e;
}

void Sha1::update(const void* data, std::size_t n) {
  const auto* p = static_cast<const std::uint8_t*>(data);
  total_ += n;
  while (n > 0) {
    const std::size_t take = (64 - buf_len_ < n) ? (64 - buf_len_) : n;
    std::memcpy(buf_ + buf_len_, p, take);
    buf_len_ += take;
    p += take;
    n -= take;
    if (buf_len_ == 64) {
      block(buf_);
      buf_len_ = 0;
    }
  }
}

Digest160 Sha1::finish() {
  const std::uint64_t total = total_;
  finish_pad(*this, buf_, buf_len_, total);
  Digest160 out;
  for (int i = 0; i < 5; ++i)
    for (int j = 0; j < 4; ++j) out[4 * i + j] = static_cast<std::uint8_t>(h_[i] >> (24 - 8 * j));
  return out;
}

Digest160 Sha1::hash(std::span<const std::uint8_t> d) {
  Sha1 s;
  s.update(d.data(), d.size());
  return s.finish();
}

Uuid uuid_v5(const Uuid& ns, std::span<const std::uint8_t> name) {
  Sha1 s;
  s.update(ns.data(), ns.size());
  s.update(name.data(), name.size());
  const Digest160 d = s.finish();
  Uuid u;
  std::memcpy(u.data(), d.data(), 16);
  u[6] = static_cast<std::uint8_t>((u[6] & 0x0F) | 0x50);
  u[8] = static_cast<std::uint8_t>((u[8] & 0x3F) | 0x80);
  return u;
}

std::string to_hex(std::span<const std::uint8_t> d) {
  static const char* hx = "0123456789abcdef";
  std::string s;
  s.reserve(d.size() * 2);
  for (std::uint8_t b : d) {
    s.push_back(hx[b >> 4]);
    s.push_back(hx[b & 15]);
  }
  return s;
}

std::string uuid_to_string(const Uuid& u) {
  const std::string h = to_hex(u);
  return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) +
         "-" + h.substr(20, 12);
}

bool uuid_from_string(std::string_view text, Uuid& out) {
  if (text.size() != 36) return false;
  Uuid u{};
  std::size_t n = 0;
  int hi = -1;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
      continue;
    }
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else return false;
    if (hi < 0) hi = v;
    else {
      u[n++] = static_cast<std::uint8_t>(hi << 4 | v);
      hi = -1;
    }
  }
  out = u;
  return n == 16;
}

const Uuid kLibraryNamespace = [] {
  const Uuid dns = {0x6b, 0xa7, 0xb8, 0x10, 0x9d, 0xad, 0x11, 0xd1,
                    0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8};
  const char* name = "library.velora.invalid";
  return uuid_v5(dns, {reinterpret_cast<const std::uint8_t*>(name), 22});
}();

Uuid library_id_from_fingerprint(std::span<const std::uint8_t> fingerprint) {
  return uuid_v5(kLibraryNamespace, fingerprint);
}

Uuid library_id_from_source(std::string_view source_id) {
  std::vector<std::uint8_t> fp;
  static const char kTag[] = "velora.source.v1";
  fp.insert(fp.end(), kTag, kTag + sizeof kTag - 1);
  fp.push_back(0);
  fp.insert(fp.end(), source_id.begin(), source_id.end());
  const Digest256 d = Sha256::hash(fp);
  return library_id_from_fingerprint(d);
}

}  // namespace velora::format

// ---- Story 6.2: HMAC, key check, hash-list root, statement, counter block (no cipher, no signing) ----
#include "velora/format/protected_format.hpp"

namespace velora::format {

// Pure data layout of the AES-CTR counter block (bytes, no cipher call): it stays in the core so the writer, the decrypt reader and the tests share
// one definition; check_format_no_decrypt.py scans symbols, and this name is not a cipher entry point.
CounterBlock counter_block(const Nonce& nonce, std::uint32_t idx) {
  CounterBlock c{};
  std::memcpy(c.data(), nonce.data(), 8);
  for (int i = 0; i < 4; ++i) c[8 + i] = static_cast<std::uint8_t>(idx >> (24 - 8 * i));  // big-endian
  return c;                                                                                // intra-block counter 0
}

namespace {
// Key-derived stack material is wiped through a volatile pointer so the compiler cannot elide the stores.
void wipe(void* p, std::size_t n) {
  volatile std::uint8_t* v = static_cast<volatile std::uint8_t*>(p);
  while (n--) *v++ = 0;
}
}  // namespace

Digest256 hmac_sha256(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data) {
  std::uint8_t k[64] = {};
  if (key.size() > 64) {
    Digest256 d = Sha256::hash(key);
    std::memcpy(k, d.data(), 32);
    wipe(d.data(), d.size());
  } else if (!key.empty()) {
    std::memcpy(k, key.data(), key.size());
  }
  std::uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; ++i) {
    ipad[i] = k[i] ^ 0x36;
    opad[i] = k[i] ^ 0x5c;
  }
  Sha256 in;
  in.update(ipad, 64);
  in.update(data);
  Digest256 inner = in.finish();
  Sha256 out;
  out.update(opad, 64);
  out.update(inner.data(), 32);
  const Digest256 mac = out.finish();
  in.wipe();
  out.wipe();
  wipe(inner.data(), inner.size());
  wipe(k, sizeof k);
  wipe(ipad, sizeof ipad);
  wipe(opad, sizeof opad);
  return mac;
}

KeyCheck key_check_value(std::span<const std::uint8_t, 32> content_key, const Nonce& nonce) {
  static constexpr char kLabel[] = "velora/key-check/v1";
  std::vector<std::uint8_t> msg(kLabel, kLabel + sizeof kLabel - 1);  // label, then a 0x00 separator
  msg.push_back(0);
  msg.insert(msg.end(), nonce.begin(), nonce.end());
  const Digest256 mac = hmac_sha256(content_key, msg);
  KeyCheck k;
  std::memcpy(k.data(), mac.data(), 16);
  return k;
}

Digest256 hash_list_root(std::span<const Digest256> block_hashes) {
  if (block_hashes.empty()) return Sha256::hash({});
  std::vector<Digest256> level;
  level.reserve(block_hashes.size());
  for (const Digest256& h : block_hashes) {
    Sha256 s;
    const std::uint8_t tag = 0x00;
    s.update(&tag, 1);
    s.update(h.data(), 32);
    level.push_back(s.finish());
  }
  while (level.size() > 1) {
    std::vector<Digest256> next;
    next.reserve((level.size() + 1) / 2);
    for (std::size_t i = 0; i + 1 < level.size(); i += 2) {
      Sha256 s;
      const std::uint8_t tag = 0x01;
      s.update(&tag, 1);
      s.update(level[i].data(), 32);
      s.update(level[i + 1].data(), 32);
      next.push_back(s.finish());
    }
    if (level.size() % 2) next.push_back(level.back());
    level = std::move(next);
  }
  return level[0];
}

KeyId key_id_of_public(const PublicKey& pub) {
  const Digest256 d = Sha256::hash(pub);
  KeyId id;
  std::memcpy(id.data(), d.data(), 8);
  return id;
}

bool ct_equal(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) {
  if (a.size() != b.size()) return false;
  std::uint8_t acc = 0;
  for (std::size_t i = 0; i < a.size(); ++i) acc = static_cast<std::uint8_t>(acc | (a[i] ^ b[i]));
  return acc == 0;
}

std::vector<std::uint8_t> statement_bytes(const ManifestStatement& s) {
  static constexpr char kCtx[] = "velora/protected-manifest/v1";
  std::vector<std::uint8_t> m(kCtx, kCtx + sizeof kCtx - 1);
  m.push_back(0);
  for (int i = 0; i < 4; ++i) m.push_back(static_cast<std::uint8_t>(s.version_word >> (8 * i)));
  m.insert(m.end(), s.library_id.begin(), s.library_id.end());
  m.insert(m.end(), s.bundle_id.begin(), s.bundle_id.end());
  m.insert(m.end(), s.index_sha256.begin(), s.index_sha256.end());
  m.insert(m.end(), s.hash_list_root.begin(), s.hash_list_root.end());
  for (int i = 0; i < 8; ++i) m.push_back(static_cast<std::uint8_t>(s.block_count >> (8 * i)));
  m.insert(m.end(), s.nonce.begin(), s.nonce.end());
  m.insert(m.end(), s.signer_key_id.begin(), s.signer_key_id.end());
  return m;
}

std::vector<std::uint8_t> catalog_meta_statement(const Digest256& bundle_id, std::span<const std::uint8_t> meta_bytes, const KeyId& signer, std::uint32_t pack_revision) {
  static constexpr char kCtx[] = "velora/catalog-meta/v1";
  std::vector<std::uint8_t> m(kCtx, kCtx + sizeof kCtx - 1);
  m.push_back(0);
  m.insert(m.end(), bundle_id.begin(), bundle_id.end());
  const Digest256 h = Sha256::hash(meta_bytes);
  m.insert(m.end(), h.begin(), h.end());
  m.insert(m.end(), signer.begin(), signer.end());
  for (int i = 0; i < 4; ++i) m.push_back(static_cast<std::uint8_t>(pack_revision >> (8 * i)));
  return m;
}

std::array<std::uint8_t, kCatalogMetaSealSize> encode_catalog_meta_seal(const KeyId& signer, std::uint32_t pack_revision, const Signature& sig) {
  std::array<std::uint8_t, kCatalogMetaSealSize> o{};
  for (int i = 0; i < 4; ++i) o[i] = static_cast<std::uint8_t>(kCatalogMetaSealMagic[i]);
  o[4] = kSealAlgEd25519;
  for (int i = 0; i < 4; ++i) o[8 + i] = static_cast<std::uint8_t>(pack_revision >> (8 * i));
  for (std::size_t i = 0; i < signer.size(); ++i) o[12 + i] = signer[i];
  for (std::size_t i = 0; i < sig.size(); ++i) o[20 + i] = sig[i];
  return o;
}

bool decode_catalog_meta_seal(std::span<const std::uint8_t> b, KeyId& signer, std::uint32_t& pack_revision, Signature& sig) {
  if (b.size() != kCatalogMetaSealSize) return false;
  for (int i = 0; i < 4; ++i)
    if (b[i] != static_cast<std::uint8_t>(kCatalogMetaSealMagic[i])) return false;
  if (b[4] != kSealAlgEd25519 || b[5] != 0 || b[6] != 0 || b[7] != 0) return false;
  pack_revision = 0;
  for (int i = 0; i < 4; ++i) pack_revision |= static_cast<std::uint32_t>(b[8 + i]) << (8 * i);
  if (pack_revision == 0) return false;
  for (std::size_t i = 0; i < signer.size(); ++i) signer[i] = b[12 + i];
  for (std::size_t i = 0; i < sig.size(); ++i) sig[i] = b[20 + i];
  return true;
}

}  // namespace velora::format
