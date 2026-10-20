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
