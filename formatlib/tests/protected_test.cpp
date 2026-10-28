// Story 6.2: protected bundle format. Round trip, 10,000 random seeks, the tamper matrix, strict Ed25519, known-answer vectors.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <openssl/evp.h>

#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "layout.hpp"
#include "test_util.hpp"
#include "velora/format/protected_reader.hpp"
#include "velora/format/protected_writer.hpp"

using namespace velora::format;
using vt::Kind;
using Bytes = std::vector<std::uint8_t>;
namespace L = velora::format::layout;

namespace {

Bytes unhex(const std::string& h) {
  Bytes b;
  for (std::size_t i = 0; i + 1 < h.size(); i += 2) b.push_back(static_cast<std::uint8_t>(std::stoi(h.substr(i, 2), nullptr, 16)));
  return b;
}
template <std::size_t N>
std::array<std::uint8_t, N> unhex_arr(const std::string& h) {
  const Bytes b = unhex(h);
  std::array<std::uint8_t, N> a{};
  CHECK(b.size() == N);
  std::copy_n(b.begin(), std::min(N, b.size()), a.begin());
  return a;
}
std::string hex(std::span<const std::uint8_t> d) { return to_hex(d); }

SigningSeed test_seed() {
  SigningSeed s;
  for (int i = 0; i < 32; ++i) s.bytes[i] = static_cast<std::uint8_t>(0x30 + 5 * i);
  return s;
}
SigningSeed other_seed() {
  SigningSeed s;
  for (int i = 0; i < 32; ++i) s.bytes[i] = static_cast<std::uint8_t>(0x90 - 3 * i);
  return s;
}
ContentKey test_key() {
  ContentKey k;
  for (int i = 0; i < 32; ++i) k.bytes[i] = static_cast<std::uint8_t>(0xa0 + i);
  return k;
}
TrustedKeys trusted_for(const SigningSeed& s) {
  const PublicKey pub = ed25519_public_from_seed(s);
  return TrustedKeys({TrustedKey{key_id_of_public(pub), pub}});
}

struct Source {
  std::vector<std::vector<std::int32_t>> pcm;  // per flat sample
  std::vector<PcmFormat> fmt;
};

// Two instruments, three samples (16-bit stereo, 24-bit mono, float stereo), optional extension region (format minor 1).
BundleWriter make_writer(Source& src, bool ext = false) {
  BundleWriter w;
  w.set_library_id(Uuid{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
  auto i0 = w.add_instrument("Piano", "pianos/grand.sfz");
  auto i1 = w.add_instrument("Pad", "pads/warm.sfz");
  CHECK(i0.ok() && i1.ok());
  const PcmFormat a{2, 16, false, 48000}, b{1, 24, false, 44100}, c{2, 32, true, 48000};
  src.fmt = {a, b, c};
  src.pcm = {vt::make_int(a, 10000, Kind::kMixed), vt::make_int(b, 5000, Kind::kSine), vt::make_float(2, 9000)};
  CHECK_ERR(w.add_sample(i0.value(), {"grand", a, 10000, src.pcm[0]}), Error::kNone);
  CHECK_ERR(w.add_sample(i0.value(), {"soft", b, 5000, src.pcm[1]}), Error::kNone);
  CHECK_ERR(w.add_sample(i1.value(), {"warm", c, 9000, src.pcm[2]}), Error::kNone);
  Region r;
  r.sample = 0;
  CHECK_ERR(w.add_region(i0.value(), r), Error::kNone);
  if (ext) {
    r.sw = {24, 28, 26, -1, 37, -1, 26};
    CHECK_ERR(w.add_region(i0.value(), r), Error::kNone);
  }
  return w;
}

Bytes protect(const BundleWriter& w, const Nonce& nonce, const SigningSeed& seed = test_seed(), const ContentKey& key = test_key()) {
  auto b = finalize_protected(w, key, nonce, seed);
  CHECK(b.ok());
  return b.ok() ? b.take() : Bytes{};
}

Nonce nonce1() { return Nonce{0, 1, 2, 3, 4, 5, 6, 7}; }

std::uint64_t rd64(const Bytes& b, std::size_t o) { return L::get_u64(b.data() + o); }
struct Geo {
  std::size_t man_off, man_size, idx_off, idx_size, seal_off, data_off, size;
};
Geo geo(const Bytes& b) {
  Geo g;
  g.man_off = rd64(b, 8);
  g.man_size = rd64(b, 16);
  g.idx_off = rd64(b, 24);
  g.idx_size = rd64(b, 32);
  g.seal_off = g.idx_off + g.idx_size;
  g.data_off = rd64(b, 40);
  g.size = b.size();
  return g;
}

// Recompute the header's manifest and index hashes and the manifest's own index-hash field, as an attacker who edits the body would.
void refix(Bytes& b) {
  const Geo g = geo(b);
  const Digest256 ih = Sha256::hash({b.data() + g.idx_off, g.idx_size});
  std::copy(ih.begin(), ih.end(), b.begin() + 96);
  std::copy(ih.begin(), ih.end(), b.begin() + g.man_off + 36);  // 4 + 16 + 4 + 4 + 8
  const Digest256 mh = Sha256::hash({b.data() + g.man_off, g.man_size});
  std::copy(mh.begin(), mh.end(), b.begin() + 64);
}
// Only the header's manifest hash: the manifest edit is "accepted" by the outer hash, so the inner checks have to catch it.
void fix_manifest_hash(Bytes& b) {
  const Geo g = geo(b);
  const Digest256 mh = Sha256::hash({b.data() + g.man_off, g.man_size});
  std::copy(mh.begin(), mh.end(), b.begin() + 64);
}
// Plain OpenSSL Ed25519 verification, without the strictness checks of ed25519_verify_strict (proves the strict vectors are not vacuous).
bool raw_openssl_accepts(const PublicKey& pub, std::span<const std::uint8_t> msg, const Signature& sig) {
  EVP_PKEY* k = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub.data(), 32);
  if (!k) return false;
  EVP_MD_CTX* c = EVP_MD_CTX_new();
  bool ok = c && EVP_DigestVerifyInit(c, nullptr, nullptr, nullptr, k) == 1 &&
            EVP_DigestVerify(c, sig.data(), sig.size(), msg.data(), msg.size()) == 1;
  EVP_MD_CTX_free(c);
  EVP_PKEY_free(k);
  return ok;
}
// The exact error a single-bit flip at header byte `i` must produce.
Error header_flip_error(const Bytes& good, std::size_t i, std::uint8_t mask) {
  if (i < 4) return Error::kBadMagic;
  if (i < 8) {
    const std::uint32_t w = L::get_u32(good.data() + 4) ^ (std::uint32_t(mask) << (8 * (i - 4)));
    const std::uint32_t major = w & 0xffffu, minor = w >> 16;
    if (major == 2) return Error::kNotProtected;
    if (major != 3 || minor > 1) return Error::kUnsupportedVersion;
    return Error::kCorruptManifest;  // minor 1 passes the version check; the manifest's own word disagrees
  }
  if (i >= 56 && i < 64) {
    const std::uint64_t t = rd64(good, 56) ^ (std::uint64_t(mask) << (8 * (i - 56)));
    return t > good.size() ? Error::kTruncated : Error::kCorruptHeader;
  }
  if (i < 64) return Error::kCorruptHeader;
  return i < 96 ? Error::kCorruptManifest : Error::kCorruptIndex;
}
// Re-sign the (possibly edited and refixed) bundle with `seed`: an attacker holding the signing key.
void resign(Bytes& b, const SigningSeed& seed) {
  const Geo g = geo(b);
  ManifestStatement st;
  st.version_word = L::get_u32(b.data() + 4);
  std::copy_n(b.begin() + g.man_off + 4, 16, st.library_id.begin());
  std::copy_n(b.begin() + 64, 32, st.bundle_id.begin());
  std::copy_n(b.begin() + 96, 32, st.index_sha256.begin());
  std::copy_n(b.begin() + g.man_off + 96, 32, st.hash_list_root.begin());
  st.block_count = g.idx_size / L::kIndexEntrySize;
  std::copy_n(b.begin() + g.man_off + 72, 8, st.nonce.begin());
  const PublicKey pub = ed25519_public_from_seed(seed);
  st.signer_key_id = key_id_of_public(pub);
  auto sig = ed25519_sign(seed, statement_bytes(st));
  CHECK(sig.ok());
  std::copy(st.signer_key_id.begin(), st.signer_key_id.end(), b.begin() + g.seal_off + 8);
  std::copy(sig.value().begin(), sig.value().end(), b.begin() + g.seal_off + 16);
}

// ---- known answers ----------------------------------------------------------------------------------------------------
void test_known_answers() {
  // HMAC-SHA256, RFC 4231 test cases 1 and 2.
  {
    const Bytes key(20, 0x0b);
    const std::string d = "Hi There";
    const Digest256 m = hmac_sha256(key, {reinterpret_cast<const std::uint8_t*>(d.data()), d.size()});
    CHECK(hex(m) == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    const std::string k2 = "Jefe", d2 = "what do ya want for nothing?";
    const Digest256 m2 = hmac_sha256({reinterpret_cast<const std::uint8_t*>(k2.data()), k2.size()},
                                     {reinterpret_cast<const std::uint8_t*>(d2.data()), d2.size()});
    CHECK(hex(m2) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    const Bytes longkey(131, 0xaa);  // key longer than the block size (RFC 4231 case 6)
    const std::string d6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    CHECK(hex(hmac_sha256(longkey, {reinterpret_cast<const std::uint8_t*>(d6.data()), d6.size()})) ==
          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
  }
  // AES-256-CTR, NIST SP 800-38A F.5.5 (two blocks).
  {
    ContentKey k;
    k.bytes = unhex_arr<32>("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4");
    const CounterBlock ctr = unhex_arr<16>("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
    Bytes pt = unhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");
    CHECK_ERR(aes256_ctr_xor(k, ctr, pt), Error::kNone);
    CHECK(hex(pt) == "601ec313775789a5b7a7f504bbf3d228f443e3ca4d62b59aca84e990cacaf5c5");
    // The 32-bit intra-block counter carries into the block-index field after 2^32 AES blocks: that is why blocks are capped.
    CounterBlock edge{};
    edge.fill(0);
    edge[11] = 0x01;
    edge[12] = edge[13] = edge[14] = edge[15] = 0xff;
    Bytes two(32, 0), one_a(16, 0), one_b(16, 0);
    CHECK_ERR(aes256_ctr_xor(k, edge, two), Error::kNone);
    CounterBlock next{};
    next[11] = 0x01 + 1;  // carry: ...01 ffffffff + 1 -> ...02 00000000 in a 128-bit big-endian increment
    CHECK_ERR(aes256_ctr_xor(k, edge, one_a), Error::kNone);
    CHECK_ERR(aes256_ctr_xor(k, next, one_b), Error::kNone);
    CHECK(std::equal(one_a.begin(), one_a.end(), two.begin()) && std::equal(one_b.begin(), one_b.end(), two.begin() + 16));
  }
  // Ed25519, RFC 8032 section 7.1 tests 1 and 2.
  {
    SigningSeed s1;
    s1.bytes = unhex_arr<32>("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
    CHECK(hex(ed25519_public_from_seed(s1)) == "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    auto sig1 = ed25519_sign(s1, {});
    CHECK(sig1.ok() && hex(sig1.value()) == "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
    CHECK(ed25519_verify_strict(ed25519_public_from_seed(s1), {}, sig1.value()));
    SigningSeed s2;
    s2.bytes = unhex_arr<32>("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb");
    const PublicKey p2 = ed25519_public_from_seed(s2);
    CHECK(hex(p2) == "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c");
    const std::uint8_t m72[1] = {0x72};
    auto sig2 = ed25519_sign(s2, m72);
    CHECK(sig2.ok() && hex(sig2.value()) == "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00");
    CHECK(ed25519_verify_strict(p2, m72, sig2.value()));
    const std::uint8_t m73[1] = {0x73};
    CHECK(!ed25519_verify_strict(p2, m73, sig2.value()));  // wrong message
    Signature bad = sig2.value();
    bad[10] ^= 1;
    CHECK(!ed25519_verify_strict(p2, m72, bad));
    // Strictness (KEY-HIERARCHY.md section 6). Every vector below is one that a lax verifier accepts: where plain OpenSSL accepts it, the test says so
    // (non-vacuity) and then requires ed25519_verify_strict to reject it.
    {
      Signature z{};
      z[0] = 1;  // R = identity, S = 0
      PublicKey id{};
      id[0] = 1;  // A = identity: [0]B == R + [k]A for every message
      PublicKey id_nc = id;
      id_nc[31] = 0x80;  // non-canonical spelling of the identity (x = 0 with the sign bit set)
      PublicKey id_p1 = unhex_arr<32>("eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f");  // y = p + 1 = 1 mod p
      for (const PublicKey& a : {id, id_nc, id_p1}) {
        CHECK(raw_openssl_accepts(a, m72, z));
        CHECK(!ed25519_verify_strict(a, m72, z));
      }
      // Canonical, honest A with a small-order R: R = identity, S = k*a mod L (generated with tools/keys/velora_keys.py arithmetic).
      Signature r_small = unhex_arr<64>("0100000000000000000000000000000000000000000000000000000000000000675d054595fa17cb6a44e3ada6ca385e55667e41a63fdab0ac22d10000354002");
      CHECK(raw_openssl_accepts(p2, m72, r_small));
      CHECK(!ed25519_verify_strict(p2, m72, r_small));
      // Valid key and signature with S replaced by S + L: a verifier that does not require S < L accepts it.
      static const std::uint8_t kL[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                                          0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0x10};
      Signature m = sig2.value();
      unsigned carry = 0;
      for (int i = 0; i < 32; ++i) {
        const unsigned v = unsigned(m[32 + i]) + kL[i] + carry;
        m[32 + i] = static_cast<std::uint8_t>(v);
        carry = v >> 8;
      }
      CHECK(carry == 0);
      CHECK(!ed25519_verify_strict(p2, m72, m));
      std::printf("note: plain OpenSSL %s S+L\n", raw_openssl_accepts(p2, m72, m) ? "ACCEPTS" : "rejects");
      // Other small-order A (order 8 and order 4) with the honest signature.
      const std::uint8_t order8[] = {0xc7, 0x17, 0x6a, 0x70, 0x3d, 0x4d, 0xd8, 0x4f, 0xba, 0x3c, 0x0b, 0x76, 0x0d, 0x10, 0x67, 0x0f,
                                     0x2a, 0x20, 0x53, 0xfa, 0x2c, 0x39, 0xcc, 0xc6, 0x4e, 0xc7, 0xfd, 0x77, 0x92, 0xac, 0x03, 0x7a};
      PublicKey o8;
      std::copy(order8, order8 + 32, o8.begin());
      CHECK(!ed25519_verify_strict(o8, m72, sig2.value()));
      CHECK(!ed25519_verify_strict(PublicKey{}, m72, sig2.value()));  // y = 0: order 4
    }
    {  // Non-canonical A (y = p) and non-canonical R (y = p, y = p + 1): always rejected.
      PublicKey nc = unhex_arr<32>("edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f");
      CHECK(!ed25519_verify_strict(nc, m72, sig2.value()));
      for (const char* h : {"edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f", "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f"}) {
        Signature r_nc = sig2.value();
        const Bytes p = unhex(h);
        std::copy(p.begin(), p.end(), r_nc.begin());
        CHECK(!ed25519_verify_strict(p2, m72, r_nc));
      }
    }
  }
  // Counter block layout: nonce || BE32(global block index) || BE32(0).
  {
    const CounterBlock c = counter_block(Nonce{0, 1, 2, 3, 4, 5, 6, 7}, 0x01020304u);
    CHECK(hex(c) == "000102030405060701020304" "00000000");
    std::set<std::string> seen;
    for (std::uint32_t i = 0; i < 20000; ++i) seen.insert(hex(counter_block(nonce1(), i)));
    CHECK(seen.size() == 20000);
    CHECK(counter_block(Nonce{9, 1, 2, 3, 4, 5, 6, 7}, 5) != counter_block(nonce1(), 5));
    CHECK(kMaxProtectedBlockBytes >= kBlockFrames * 2 * 4 + 1024);  // the largest legitimate block fits the cap
  }
  // Key check and nonce binding.
  {
    const KeyCheck a = key_check_value(test_key().bytes, nonce1());
    CHECK(a == key_check_value(test_key().bytes, nonce1()));
    CHECK(a != key_check_value(test_key().bytes, Nonce{1, 1, 2, 3, 4, 5, 6, 7}));
    ContentKey k2 = test_key();
    k2.bytes[31] ^= 1;
    CHECK(a != key_check_value(k2.bytes, nonce1()));
  }
  // Hash-list root.
  {
    CHECK(hash_list_root({}) == Sha256::hash({}));
    const Digest256 h0 = Sha256::hash(Bytes{1}), h1 = Sha256::hash(Bytes{2}), h2 = Sha256::hash(Bytes{3});
    Sha256 leaf;
    const std::uint8_t t0 = 0;
    leaf.update(&t0, 1);
    leaf.update(h0.data(), 32);
    const Digest256 l0 = leaf.finish();
    CHECK(hash_list_root(std::span<const Digest256>(&h0, 1)) == l0);
    const Digest256 three[] = {h0, h1, h2}, swapped[] = {h1, h0, h2}, changed[] = {h0, h1, Sha256::hash(Bytes{4})};
    const Digest256 r3 = hash_list_root(three);
    CHECK(r3 != hash_list_root(swapped) && r3 != hash_list_root(changed) && r3 == hash_list_root(three));
    CHECK(hash_list_root(std::span<const Digest256>(three, 2)) != r3);
    // Pinned literals (computed independently with Python hashlib).
    const Digest256 four[] = {h0, h1, h2, Sha256::hash(Bytes{4})};
    CHECK(hex(r3) == "15681b71a5a1895616f06c60725050e8d9cd13a6ebbbd6ff8ca1a936bb504350");
    CHECK(hex(hash_list_root(four)) == "1543f7ab8a0a2bd4591b5bb77c883fb3a4560423ad9e553e398987fb1205f79a");
  }
  // Pinned key check and signed statement.
  {
    CHECK(hex(key_check_value(test_key().bytes, Nonce{0, 1, 2, 3, 4, 5, 6, 7})) == "44739fda0703f20d4f067f62da2e55cb");
    ManifestStatement st;
    st.version_word = 0x10003;
    for (int i = 0; i < 16; ++i) st.library_id[i] = static_cast<std::uint8_t>(i + 1);
    st.bundle_id.fill(0x11);
    st.index_sha256.fill(0x22);
    st.hash_list_root.fill(0x33);
    st.block_count = 0x0102030405;
    for (int i = 0; i < 8; ++i) {
      st.nonce[i] = static_cast<std::uint8_t>(i);
      st.signer_key_id[i] = static_cast<std::uint8_t>(0x80 + i);
    }
    CHECK(hex(statement_bytes(st)) ==
          "76656c6f72612f70726f7465637465642d6d616e69666573742f763100"
          "03000100"
          "0102030405060708090a0b0c0d0e0f10"
          "1111111111111111111111111111111111111111111111111111111111111111"
          "2222222222222222222222222222222222222222222222222222222222222222"
          "3333333333333333333333333333333333333333333333333333333333333333"
          "0504030201000000"
          "0001020304050607"
          "8081828384858687");
  }
}

// ---- round trip, seeks, reproducibility ------------------------------------------------------------------------------
void test_roundtrip_and_seeks(const std::string& fixture_dir) {
  Source src;
  BundleWriter w = make_writer(src);
  const Bytes b = protect(w, nonce1());
  const TrustedKeys tk = trusted_for(test_seed());
  auto r = ProtectedBundleReader::open_memory(b, tk, test_key());
  CHECK(r.ok());
  if (!r.ok()) return;
  const ProtectedBundleReader& rd = r.value();
  CHECK(rd.info().format_version == kProtectedFormatVersion && rd.info().format_minor == 0);
  CHECK(rd.info().library_id == (Uuid{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}));
  CHECK(rd.info().nonce == nonce1() && rd.info().signer_key_id == key_id_of_public(ed25519_public_from_seed(test_seed())));
  CHECK(rd.info().bundle_id == Sha256::hash({b.data() + 128, geo(b).man_size}));
  CHECK(rd.samples().size() == 3 && rd.instruments().size() == 2 && rd.view().total_blocks() == 3 + 2 + 3);
  CHECK(L::get_u32(b.data() + 4) == kProtectedFormatVersion);
  CHECK(std::memcmp(b.data() + geo(b).seal_off, "VSL1", 4) == 0);
  CHECK_ERR(rd.verify_all(), Error::kNone);
  CHECK_ERR(rd.view().verify_all_blocks(), Error::kNone);
  // Whole samples, and block-by-block, bit-identical to the source.
  for (std::size_t s = 0; s < 3; ++s) {
    std::vector<std::int32_t> all(src.pcm[s].size());
    CHECK_ERR(rd.read_frames(s, 0, rd.samples()[s].frame_count, all), Error::kNone);
    CHECK(all == src.pcm[s]);
  }
  // 10,000 random seeks (deterministic seed, no timing).
  vt::Rng rng(0x5eed2024);
  int bad = 0;
  for (int i = 0; i < 10000; ++i) {
    const std::size_t s = rng.next() % 3;
    const SampleInfo& si = rd.samples()[s];
    const std::uint64_t first = rng.next() % si.frame_count;
    const std::uint64_t count = 1 + rng.next() % std::min<std::uint64_t>(6000, si.frame_count - first);
    const std::size_t ch = si.format.channels;
    std::vector<std::int32_t> out(count * ch);
    if (rd.read_frames(s, first, count, out) != Error::kNone ||
        !std::equal(out.begin(), out.end(), src.pcm[s].begin() + first * ch))
      ++bad;
  }
  CHECK(bad == 0);
  // Single-block API and range errors.
  {
    BlockDecoder dec;
    std::vector<std::int32_t> blk(kBlockFrames * 2);
    auto n = rd.read_block(0, 1, blk, dec);
    CHECK(n.ok() && n.value() == kBlockFrames && std::equal(blk.begin(), blk.begin() + kBlockFrames * 2, src.pcm[0].begin() + kBlockFrames * 2));
    CHECK(rd.read_block(0, 3, blk, dec).error() == Error::kOutOfRange);
    CHECK(rd.read_block(9, 0, blk, dec).error() == Error::kOutOfRange);
    std::vector<std::int32_t> tiny(2);
    CHECK(rd.read_block(0, 0, tiny, dec).error() == Error::kBufferTooSmall);
    std::vector<std::int32_t> o(4);
    CHECK_ERR(rd.read_frames(0, 9999, 2, o), Error::kOutOfRange);
  }
  // The file path and the memory path agree; a verify-only view sees the same metadata without a key.
  namespace fs = std::filesystem;
  fs::create_directories(fixture_dir);
  const std::string path = fixture_dir + "/fixture.vlrp";
  CHECK_ERR(finalize_protected_to_file(w, test_key(), nonce1(), test_seed(), path), Error::kNone);
  std::ifstream f(path, std::ios::binary);
  const Bytes on_disk((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  CHECK(on_disk == b);
  auto rf = ProtectedBundleReader::open_file(path, tk, test_key());
  CHECK(rf.ok());
  auto vf = ProtectedBundleView::open_file(path, tk);
  CHECK(vf.ok() && vf.value().info().bundle_id == rd.info().bundle_id && vf.value().verify_all_blocks() == Error::kNone);
  std::ofstream(fixture_dir + "/trusted.hex") << hex(ed25519_public_from_seed(test_seed())) << "\n";

  // Reproducibility: same source + key + nonce => identical bytes; another nonce changes ciphertext, manifest and bundle_id.
  {
    Source s2;
    BundleWriter w2 = make_writer(s2);
    CHECK(protect(w2, nonce1()) == b);
    const Bytes b2 = protect(w, Nonce{7, 6, 5, 4, 3, 2, 1, 0});
    CHECK(b2.size() == b.size() && b2 != b);
    auto r2 = ProtectedBundleReader::open_memory(b2, tk, test_key());
    CHECK(r2.ok() && r2.value().info().bundle_id != rd.info().bundle_id && r2.value().verify_all() == Error::kNone);
    CHECK(!std::equal(b.begin() + geo(b).data_off, b.end(), b2.begin() + geo(b2).data_off));
    auto n1 = w.derive_nonce(), n2 = w2.derive_nonce();
    CHECK(n1.ok() && n2.ok() && n1.value() == n2.value());
    BundleWriter w3;
    CHECK(w3.derive_nonce().ok() && w3.derive_nonce().value() != n1.value());
  }
  // The block bytes are not the plaintext FLAC/raw bytes: same size as the unprotected bundle, ~all bytes differ.
  {
    auto plain = w.finalize();
    CHECK(plain.ok());
    const Bytes& pb = plain.value();
    const std::size_t pdata = rd64(pb, 40);
    CHECK(pb.size() - pdata == b.size() - geo(b).data_off);
    std::size_t diff = 0;
    for (std::size_t i = 0; i < pb.size() - pdata; ++i) diff += pb[pdata + i] != b[geo(b).data_off + i];
    CHECK(diff > (pb.size() - pdata) * 9 / 10);
    // The unprotected bundle is unchanged by Story 6.2 and refuses the protected APIs; a plain reader refuses a protected bundle.
    CHECK(L::get_u32(pb.data() + 4) == 2 || L::get_u32(pb.data() + 4) == (2u | (1u << 16)));
    CHECK(ProtectedBundleView::open_memory(pb, tk).error() == Error::kNotProtected);
    CHECK(ProtectedBundleReader::open_memory(pb, tk, test_key()).error() == Error::kNotProtected);
    CHECK(BundleReader::open_memory(b).error() == Error::kUnsupportedVersion);
  }
  // Extension regions (minor 1) work the same.
  {
    Source s3;
    BundleWriter we = make_writer(s3, true);
    const Bytes be = protect(we, nonce1());
    CHECK(L::get_u32(be.data() + 4) == (kProtectedFormatVersion | (1u << 16)));
    auto re = ProtectedBundleReader::open_memory(be, tk, test_key());
    CHECK(re.ok() && re.value().info().format_minor == 1 && re.value().instruments()[0].regions.size() == 2 &&
          re.value().verify_all() == Error::kNone);
    CHECK(re.ok() && re.value().instruments()[0].regions[1].sw.last == 26);
  }
  // Story 13.3: trigger first / legato / release_key and `end` (format minor 2) round-trip through the protected format.
  {
    Source s4;
    BundleWriter wt = make_writer(s4, true);
    Region a;
    a.trigger = TriggerMode::kFirst;
    Region b;
    b.trigger = TriggerMode::kLegato;
    b.has_end = true;
    b.end = 4000;
    Region c;
    c.trigger = TriggerMode::kReleaseKey;
    Region d;
    d.has_end = true;
    d.end = 0;
    for (const Region* r : {&a, &b, &c, &d}) CHECK_ERR(wt.add_region(0, *r), Error::kNone);
    const Bytes bt = protect(wt, nonce1());
    CHECK(L::get_u32(bt.data() + 4) == (kProtectedFormatVersion | (2u << 16)));
    auto rt = ProtectedBundleReader::open_memory(bt, tk, test_key());
    CHECK(rt.ok() && rt.value().info().format_minor == 2 && rt.value().verify_all() == Error::kNone);
    if (rt.ok()) {
      const auto& got = rt.value().instruments()[0].regions;
      CHECK(got.size() == 6 && got[2] == a && got[3] == b && got[4] == c && got[5] == d && got[1].sw.last == 26);
    }
  }
  // Story 13.4: the amp table (format minor 3), velocity curve included, round-trips through the protected format.
  {
    Source s5;
    BundleWriter wa = make_writer(s5, true);
    Region a;
    a.amp.veltrack = 30.f; a.amp.amplitude = 75.f; a.amp.keytrack = -0.5f; a.amp.keycenter = 50;
    a.amp.vel2attack = 0.1f; a.amp.vel2decay = 0.2f; a.amp.vel2sustain = -20.f; a.amp.vel2release = 0.3f;
    a.amp.curve_count = 2; a.amp.curve[0] = {1, 0.25f}; a.amp.curve[1] = {99, 0.75f};
    Region b;
    b.trigger = TriggerMode::kLegato;  // a minor 2 feature in the same bundle
    for (const Region* r : {&a, &b}) CHECK_ERR(wa.add_region(0, *r), Error::kNone);
    const Bytes ba = protect(wa, nonce1());
    CHECK(L::get_u32(ba.data() + 4) == (kProtectedFormatVersion | (3u << 16)));
    auto ra = ProtectedBundleReader::open_memory(ba, tk, test_key());
    CHECK(ra.ok() && ra.value().info().format_minor == 3 && ra.value().verify_all() == Error::kNone);
    if (ra.ok()) {
      const auto& got = ra.value().instruments()[0].regions;
      CHECK(got.size() == 4 && got[2] == a && got[3] == b && got[2].amp.curve_count == 2);
    }
  }
  // Story 13.5: the cc table (format minor 4), the instrument's CC defaults and curves round-trip through the protected format.
  {
    Source s6;
    BundleWriter wc = make_writer(s6, true);
    Region a;
    a.cc.gate_count = 1; a.cc.gates[0] = {64, 64.f, 100.f};
    a.cc.mod_count = 2; a.cc.mods[0] = {CcTarget::kAmplitude, 93, 7, 100.f}; a.cc.mods[1] = {CcTarget::kEgRelease, 131, 0, 1.5f};
    CHECK_ERR(wc.add_region(0, a), Error::kNone);
    InstrumentCcSetup setup;
    setup.defaults = {{64, 90.f}};
    setup.curves = {{7, {{0, 0.f}, {64, 0.9f}, {127, 1.f}}}};
    CHECK_ERR(wc.set_instrument_cc_setup(0, setup), Error::kNone);
    const Bytes bc = protect(wc, nonce1());
    CHECK(L::get_u32(bc.data() + 4) == (kProtectedFormatVersion | (4u << 16)));
    auto rc = ProtectedBundleReader::open_memory(bc, tk, test_key());
    CHECK(rc.ok() && rc.value().info().format_minor == 4 && rc.value().verify_all() == Error::kNone);
    if (rc.ok()) {
      const auto& got = rc.value().instruments()[0].regions;
      CHECK(!got.empty() && got.back() == a && rc.value().instruments()[0].cc_setup == setup);
    }
  }
  // Story 13.6: the voice-group table (format minor 5) round-trips through the protected format.
  {
    Source s7;
    BundleWriter wv = make_writer(s7, true);
    Region a;
    a.vg.group = 3; a.vg.has_off_by = true; a.vg.off_by = 3; a.vg.off_mode = OffMode::kTime; a.vg.off_time = 0.4f;
    a.vg.polyphony = 2; a.vg.group_polyphony = 4; a.vg.note_polyphony = 1; a.vg.note_selfmask = false;
    CHECK_ERR(wv.add_region(0, a), Error::kNone);
    const Bytes bv = protect(wv, nonce1());
    CHECK(L::get_u32(bv.data() + 4) == (kProtectedFormatVersion | (5u << 16)));
    auto rv = ProtectedBundleReader::open_memory(bv, tk, test_key());
    CHECK(rv.ok() && rv.value().info().format_minor == 5 && rv.value().verify_all() == Error::kNone);
    if (rv.ok()) {
      const auto& got = rv.value().instruments()[0].regions;
      CHECK(!got.empty() && got.back() == a);
    }
  }
  // Story 13.7: the bend / controller-trigger table (format minor 6) round-trips through the protected format.
  {
    Source s8;
    BundleWriter wb = make_writer(s8, true);
    Region a;
    a.bend.enabled = true; a.bend.up = 700.f; a.bend.down = -2400.f; a.bend.step = 50.f; a.bend.smooth_ms = 40;
    Region b;
    b.trig.no_note = true; b.trig.count = 2; b.trig.t[0] = {20, 0.f, 127.f}; b.trig.t[1] = {64, 127.f, 127.f};
    CHECK_ERR(wb.add_region(0, a), Error::kNone);
    CHECK_ERR(wb.add_region(0, b), Error::kNone);
    const Bytes bb = protect(wb, nonce1());
    CHECK(L::get_u32(bb.data() + 4) == (kProtectedFormatVersion | (6u << 16)));
    auto rb = ProtectedBundleReader::open_memory(bb, tk, test_key());
    CHECK(rb.ok() && rb.value().info().format_minor == 6 && rb.value().verify_all() == Error::kNone);
    if (rb.ok()) {
      const auto& got = rb.value().instruments()[0].regions;
      CHECK(got.size() >= 2 && got[got.size() - 2] == a && got.back() == b);
    }
  }
  // Story 13.8: the filter table (format minor 7) round-trips through the protected format.
  {
    constexpr std::uint32_t kFlMinor = 7;  // the minor that introduced the filter table
    Source s9;
    BundleWriter wb = make_writer(s9, true);
    Region a;
    a.filter = {FilterType::kLpf2p, 500.f, 3.f};
    a.fmap.f1.keytrack = 100.f; a.fmap.f1.keycenter = 48; a.fmap.f1.veltrack = 2400.f;
    a.fmap.f1.cutoff_count = 1; a.fmap.f1.cutoff[0] = {131, 10, 2400.f};
    a.fmap.f1.res_count = 1; a.fmap.f1.res[0] = {1, 0, 6.f};
    a.fmap.f2.type = FilterType::kHpf2p; a.fmap.f2.cutoff_hz = 200.f; a.fmap.f2.resonance_db = 1.f; a.fmap.f2.tune.veltrack = -600.f;
    CHECK_ERR(wb.add_region(0, a), Error::kNone);
    const Bytes bb = protect(wb, nonce1());
    CHECK(L::get_u32(bb.data() + 4) == (kProtectedFormatVersion | (kFlMinor << 16)));
    auto rb = ProtectedBundleReader::open_memory(bb, tk, test_key());
    CHECK(rb.ok() && rb.value().info().format_minor == kFlMinor && rb.value().verify_all() == Error::kNone);
    if (rb.ok()) {
      const auto& got = rb.value().instruments()[0].regions;
      CHECK(!got.empty() && got.back() == a);
    }
  }
  // An empty writer is a valid (zero block) protected bundle.
  {
    BundleWriter we;
    const Bytes be = protect(we, nonce1());
    auto re = ProtectedBundleReader::open_memory(be, tk, test_key());
    CHECK(re.ok() && re.value().view().total_blocks() == 0 && re.value().verify_all() == Error::kNone);
  }
}

// ---- tamper matrix ----------------------------------------------------------------------------------------------------
void test_tamper() {
  Source src;
  BundleWriter w = make_writer(src);
  const Bytes good = protect(w, nonce1());
  const TrustedKeys tk = trusted_for(test_seed());
  const Geo g = geo(good);
  auto open = [&](const Bytes& b) { return ProtectedBundleReader::open_memory(b, tk, test_key()); };
  CHECK(open(good).ok());
  const std::set<Error> structural = {Error::kBadMagic, Error::kUnsupportedVersion, Error::kNotProtected, Error::kTruncated,
                                      Error::kCorruptHeader, Error::kCorruptManifest, Error::kCorruptIndex};

  // Header: every byte, two bit patterns: the exact typed error per byte class (header_flip_error).
  for (std::size_t i = 0; i < L::kHeaderSize; ++i)
    for (std::uint8_t mask : {std::uint8_t(0x01), std::uint8_t(0x80)}) {
      Bytes b = good;
      b[i] ^= mask;
      auto r = open(b);
      const Error want = header_flip_error(good, i, mask);
      CHECK(!r.ok() && r.error() == want);
      if (!r.ok() && r.error() != want) std::printf("  header byte %zu mask %02x: got %s want %s\n", i, mask, to_string(r.error()), to_string(want));
    }
  // Seal: key id -> untrusted_key, signature -> signature_invalid, magic / alg / padding -> corrupt header.
  for (std::size_t i = 0; i < kSealSize; ++i) {
    Bytes b = good;
    b[g.seal_off + i] ^= 0x04;
    auto r = open(b);
    const Error want = i >= 8 && i < 16 ? Error::kUntrustedKey : i >= 16 && i < 80 ? Error::kSignatureInvalid : Error::kCorruptHeader;
    CHECK(!r.ok() && r.error() == want);
  }
  // Manifest: every byte. A raw edit is caught by the header's manifest hash (corrupt_manifest). With only that outer hash recomputed the inner checks
  // answer: structural fixed fields (version word, block count, index-hash field, protection enums) -> corrupt_manifest; every signed field and the
  // whole body -> signature_invalid (the signature is checked before the body is parsed).
  for (std::size_t i = 0; i < g.man_size; ++i) {
    Bytes b = good;
    b[g.man_off + i] ^= 0x10;
    CHECK(open(b).error() == Error::kCorruptManifest);
    fix_manifest_hash(b);
    const auto r = open(b);
    const bool structural = i < 4 || (i >= 28 && i < 72);
    const bool counts = i >= 20 && i < 28;  // instrument / sample counts: either (huge counts fail the bounds check first)
    if (counts) {
      CHECK(!r.ok() && (r.error() == Error::kSignatureInvalid || r.error() == Error::kCorruptManifest));
    } else {
      const Error want = structural ? Error::kCorruptManifest : Error::kSignatureInvalid;
      CHECK(!r.ok() && r.error() == want);
      if (!r.ok() && r.error() != want) std::printf("  manifest byte %zu: got %s want %s\n", i, to_string(r.error()), to_string(want));
    }
  }
  {  // a name character: the structure stays valid, so only the signature catches it
    Bytes b = good;
    const std::string name = "Piano";
    auto it = std::search(b.begin() + g.man_off, b.begin() + g.man_off + g.man_size, name.begin(), name.end());
    CHECK(it != b.begin() + g.man_off + g.man_size);
    *it ^= 0x01;
    refix(b);
    CHECK(open(b).error() == Error::kSignatureInvalid);
    CHECK(ProtectedBundleView::open_memory(b, tk).error() == Error::kSignatureInvalid);
  }
  // Index: every byte. Raw -> corrupt index; with all hashes recomputed -> signature_invalid (the signature binds the index hash).
  for (std::size_t i = 0; i < g.idx_size; ++i) {
    Bytes b = good;
    b[g.idx_off + i] ^= 0x20;
    CHECK(open(b).error() == Error::kCorruptIndex);
    refix(b);
    CHECK(open(b).error() == Error::kSignatureInvalid);
  }
  {  // a block hash edited by an attacker who fixes every hash but cannot sign
    Bytes b = good;
    b[g.idx_off + 16] ^= 1;
    refix(b);
    CHECK(open(b).error() == Error::kSignatureInvalid);
  }
  // Block data: open succeeds (hashes are verified at read), the touched block fails with block_hash_mismatch and yields nothing.
  {
    auto reader = open(good);
    CHECK(reader.ok());
    const std::size_t nb = reader.value().view().total_blocks();
    std::size_t smallest = 0;
    for (std::size_t blk = 1; blk < nb; ++blk)
      if (L::get_u32(good.data() + g.idx_off + blk * L::kIndexEntrySize + 8) <
          L::get_u32(good.data() + g.idx_off + smallest * L::kIndexEntrySize + 8))
        smallest = blk;
    for (std::size_t blk = 0; blk < nb; ++blk) {
      const std::size_t off = L::get_u64(good.data() + g.idx_off + blk * L::kIndexEntrySize);
      const std::size_t sz = L::get_u32(good.data() + g.idx_off + blk * L::kIndexEntrySize + 8);
      std::vector<std::size_t> positions = {off, off + sz / 2, off + sz - 1};
      if (blk == smallest)  // every byte of the smallest block
        for (std::size_t q = 0; q < sz; ++q) positions.push_back(off + q);
      for (std::size_t pos : positions) {
        Bytes b = good;
        b[pos] ^= 0x01;
        auto r = open(b);
        CHECK(r.ok());
        if (!r.ok()) continue;
        std::vector<std::uint8_t> ct;
        CHECK_ERR(r.value().view().read_ciphertext(blk, ct), Error::kBlockHashMismatch);
        CHECK(ct.empty());
        CHECK_ERR(r.value().view().verify_all_blocks(), Error::kBlockHashMismatch);
        CHECK_ERR(r.value().verify_all(), Error::kBlockHashMismatch);
        // Locate the (sample, block) of this global block and read it: typed error, output untouched (no plaintext).
        for (std::size_t s = 0; s < 3; ++s) {
          const SampleInfo& si = r.value().samples()[s];
          if (blk < si.first_block || blk >= si.first_block + si.block_count) continue;
          BlockDecoder dec;
          std::vector<std::int32_t> out(kBlockFrames * 2, 0x5a5a5a5a);
          auto rb = r.value().read_block(s, blk - si.first_block, out, dec);
          CHECK(!rb.ok() && rb.error() == Error::kBlockHashMismatch);
          CHECK(std::all_of(out.begin(), out.end(), [](std::int32_t v) { return v == 0x5a5a5a5a; }));
          // The neighbouring blocks are unaffected.
          if (si.block_count > 1) {
            const std::uint64_t other = (blk - si.first_block + 1) % si.block_count;
            CHECK(r.value().read_block(s, other, out, dec).ok());
          }
        }
      }
    }
  }
  // Truncation: every cut point of interest is a typed error (never a partial open). Trailing bytes too.
  for (std::size_t len : {std::size_t(0), std::size_t(1), std::size_t(63), std::size_t(127), std::size_t(128), std::size_t(129),
                          g.man_off + g.man_size / 2, g.idx_off, g.idx_off + 1, g.seal_off - 1, g.seal_off, g.seal_off + 64,
                          g.data_off - 1, g.data_off, g.data_off + 1, g.size - 1}) {
    Bytes b(good.begin(), good.begin() + len);
    auto r = open(b);
    CHECK(!r.ok() && r.error() == Error::kTruncated);
  }
  {
    Bytes b = good;
    b.push_back(0);
    CHECK(open(b).error() == Error::kCorruptHeader);
  }
  // Untrusted key and key list problems.
  {
    CHECK(ProtectedBundleView::open_memory(good, TrustedKeys()).error() == Error::kUntrustedKey);
    CHECK(ProtectedBundleView::open_memory(good, trusted_for(other_seed())).error() == Error::kUntrustedKey);
    // A bundle signed by another key that IS in the list opens only with the right pair.
    const Bytes other = protect(w, nonce1(), other_seed());
    CHECK(ProtectedBundleView::open_memory(other, trusted_for(other_seed())).ok());
    CHECK(ProtectedBundleView::open_memory(other, tk).error() == Error::kUntrustedKey);
    // A list entry whose id claims the signer but whose key is another key: not trusted, and a mismatching entry id never verifies.
    const PublicKey good_pub = ed25519_public_from_seed(test_seed()), other_pub = ed25519_public_from_seed(other_seed());
    CHECK(ProtectedBundleView::open_memory(good, TrustedKeys({TrustedKey{key_id_of_public(good_pub), other_pub}})).error() ==
          Error::kUntrustedKey);
    // The same key id listed twice is ambiguous and never trusted.
    CHECK(ProtectedBundleView::open_memory(good, TrustedKeys({TrustedKey{key_id_of_public(good_pub), good_pub}, TrustedKey{key_id_of_public(good_pub), good_pub}}))
              .error() == Error::kUntrustedKey);
    // Both keys listed: either signer works.
    const TrustedKeys both({TrustedKey{key_id_of_public(other_pub), other_pub}, TrustedKey{key_id_of_public(good_pub), good_pub}});
    CHECK(ProtectedBundleView::open_memory(good, both).ok() && ProtectedBundleView::open_memory(other, both).ok());
    // Signature bytes of one key under the other's id: signature_invalid.
    Bytes forged = good;
    const KeyId oid = key_id_of_public(other_pub);
    std::copy(oid.begin(), oid.end(), forged.begin() + g.seal_off + 8);
    CHECK(ProtectedBundleView::open_memory(forged, both).error() == Error::kSignatureInvalid);
  }
  // Content key.
  {
    ContentKey wrong = test_key();
    wrong.bytes[0] ^= 1;
    CHECK(ProtectedBundleReader::open_memory(good, tk, wrong).error() == Error::kWrongContentKey);
    CHECK(ProtectedBundleReader::open_memory(good, tk, ContentKey{}).error() == Error::kWrongContentKey);
    // A signed-bundle check comes first: a tampered bundle with the wrong key reports the tamper.
    Bytes b = good;
    b[g.seal_off + 20] ^= 1;
    CHECK(ProtectedBundleReader::open_memory(b, tk, wrong).error() == Error::kSignatureInvalid);
    CHECK(ProtectedBundleView::open_memory(good, tk).ok());  // the verify layer needs no key
  }
  // Nonce change: raw -> corrupt manifest; with hashes fixed -> signature_invalid; with a forged re-signature (an attacker who holds the
  // signing key but not the content key) the key check, which binds the nonce, reports wrong_content_key instead of garbage.
  {
    Bytes b = good;
    b[g.man_off + 72] ^= 0x55;
    CHECK(open(b).error() == Error::kCorruptManifest);
    refix(b);
    CHECK(open(b).error() == Error::kSignatureInvalid);
    resign(b, test_seed());
    CHECK(ProtectedBundleView::open_memory(b, tk).ok());
    CHECK(open(b).error() == Error::kWrongContentKey);
  }
  // Key check edited and re-signed: wrong_content_key. Hash-list root edited and re-signed: hash list mismatch.
  {
    Bytes b = good;
    b[g.man_off + 80] ^= 1;
    refix(b);
    resign(b, test_seed());
    CHECK(open(b).error() == Error::kWrongContentKey);
    Bytes c = good;
    c[g.man_off + 96] ^= 1;
    refix(c);
    resign(c, test_seed());
    CHECK(ProtectedBundleView::open_memory(c, tk).error() == Error::kHashListMismatch);
  }
  // Unknown versions are typed errors even when hashes and the signature are made consistent: exact codes.
  {
    struct V { std::uint32_t word; Error want; };
    for (const V& v : {V{4u, Error::kUnsupportedVersion}, V{2u, Error::kNotProtected}, V{1u, Error::kNotProtected}, V{0u, Error::kUnsupportedVersion},
                       V{3u | ((kFormatMinor + 1) << 16), Error::kUnsupportedVersion}, V{3u | (7u << 16), Error::kCorruptManifest}, V{3u | (6u << 16), Error::kCorruptManifest}, V{3u | (5u << 16), Error::kCorruptManifest}, V{3u | (4u << 16), Error::kCorruptManifest}, V{3u | (1u << 16), Error::kCorruptManifest}, V{3u | (2u << 16), Error::kCorruptManifest},
                       V{3u | (3u << 16), Error::kCorruptManifest}}) {
      Bytes b = good;
      for (int i = 0; i < 4; ++i) {
        b[4 + i] = static_cast<std::uint8_t>(v.word >> (8 * i));
        b[g.man_off + i] = static_cast<std::uint8_t>(v.word >> (8 * i));
      }
      refix(b);
      resign(b, test_seed());
      auto r = ProtectedBundleView::open_memory(b, tk);
      CHECK(!r.ok() && r.error() == v.want);
      if (!r.ok() && r.error() != v.want) std::printf("  version word %08x: got %s want %s\n", v.word, to_string(r.error()), to_string(v.want));
    }
  }
  // Precedence: the trusted-key check comes before the content-key check, the hash-list check before it too.
  {
    ContentKey wrong = test_key();
    wrong.bytes[0] ^= 1;
    CHECK(ProtectedBundleReader::open_memory(good, TrustedKeys(), wrong).error() == Error::kUntrustedKey);
    CHECK(ProtectedBundleReader::open_memory(good, trusted_for(other_seed()), wrong).error() == Error::kUntrustedKey);
    Bytes c = good;
    c[g.man_off + 96] ^= 1;
    refix(c);
    resign(c, test_seed());
    CHECK(ProtectedBundleReader::open_memory(c, tk, wrong).error() == Error::kHashListMismatch);
    CHECK(ProtectedBundleReader::open_memory(c, tk, test_key()).error() == Error::kHashListMismatch);
  }
  // Reserved fields must be zero and enumerations known, even in a signed manifest.
  {
    for (std::size_t off : {std::size_t(68), std::size_t(69), std::size_t(70), std::size_t(71)}) {
      Bytes b = good;
      b[g.man_off + off] = off < 70 ? 0x7f : 0x01;
      refix(b);
      resign(b, test_seed());
      CHECK(ProtectedBundleView::open_memory(b, tk).error() == Error::kCorruptManifest);
    }
  }
  // Block size cap: an index entry larger than kMaxProtectedBlockBytes is refused even in a signed bundle.
  {
    Bytes b = good;
    const std::uint32_t big = kMaxProtectedBlockBytes + 1;
    for (int i = 0; i < 4; ++i) b[g.idx_off + 8 + i] = static_cast<std::uint8_t>(big >> (8 * i));
    refix(b);
    resign(b, test_seed());
    CHECK(ProtectedBundleView::open_memory(b, tk).error() == Error::kCorruptIndex);
  }
  // A bad source and null source.
  CHECK(ProtectedBundleView::open_file("/nonexistent/none.vlrp", tk).error() == Error::kIo);
  CHECK(ProtectedBundleView::open_source(nullptr, tk).error() == Error::kInvalidArgument);
  // Protected writer refuses a missing callback and test hooks.
  {
    ProtectParams p;
    CHECK(w.finalize_protected(p).error() == Error::kInvalidArgument);
    WriterOptions o;
    o.format_version = 1;
    BundleWriter w1(o);
    CHECK(finalize_protected(w1, test_key(), nonce1(), test_seed()).error() == Error::kInvalidArgument);
  }
}


// ---- plaintext integrity, partial failures, many blocks, pinned fixture ----------------------------------------------------
// Ciphertext made under a DIFFERENT key but with the right key-check value, then hashed and signed consistently (a signing-key holder mistake or
// attack): the key check passes, so the decoder's own integrity checks must catch the garbage. 16/24-bit blocks are FLAC (frame CRC) -> corrupt_block;
// float blocks are raw words and are checked by size only (documented in the README), so they decode to garbage without an error.
void test_plaintext_integrity() {
  Source src;
  BundleWriter w = make_writer(src);
  const SigningSeed seed = test_seed();
  ContentKey other = test_key();
  other.bytes[5] ^= 0x40;
  ProtectParams p;
  p.nonce = nonce1();
  p.key_check = key_check_value(test_key().bytes, nonce1());  // claims the right key ...
  p.signer_key_id = key_id_of_public(ed25519_public_from_seed(seed));
  p.encrypt = [&](std::uint32_t idx, std::span<std::uint8_t> d) { return aes256_ctr_xor(other, counter_block(nonce1(), idx), d); };  // ... encrypts with another
  p.sign = [&](std::span<const std::uint8_t> m, Signature& o) {
    auto sg = ed25519_sign(seed, m);
    if (!sg) return sg.error();
    o = sg.value();
    return Error::kNone;
  };
  auto bytes = w.finalize_protected(p);
  CHECK(bytes.ok());
  if (!bytes.ok()) return;
  auto r = ProtectedBundleReader::open_memory(bytes.value(), trusted_for(seed), test_key());
  CHECK(r.ok());
  if (!r.ok()) return;
  BlockDecoder dec;
  for (std::size_t s = 0; s < 2; ++s) {  // 16-bit stereo, 24-bit mono
    std::vector<std::int32_t> out(kBlockFrames * 2, 0x5a5a5a5a);
    auto rb = r.value().read_block(s, 0, out, dec);
    CHECK(!rb.ok() && rb.error() == Error::kCorruptBlock);
    CHECK(std::all_of(out.begin(), out.end(), [](std::int32_t v) { return v == 0x5a5a5a5a; }));
    std::vector<std::int32_t> fr(100 * 2, 0x5a5a5a5a);
    CHECK_ERR(r.value().read_frames(s, 10, 100, fr), Error::kCorruptBlock);
    CHECK(std::all_of(fr.begin(), fr.end(), [](std::int32_t v) { return v == 0x5a5a5a5a; }));
  }
  CHECK_ERR(r.value().verify_all(), Error::kCorruptBlock);
  std::vector<std::int32_t> fo(kBlockFrames * 2);
  CHECK(r.value().read_block(2, 0, fo, dec).ok());  // float: size-checked only
}

// A bad block in the middle of a range: the read fails with block_hash_mismatch, the bad and later regions of the output stay untouched.
void test_good_bad_good() {
  Source src;
  BundleWriter w = make_writer(src);
  Bytes b = protect(w, nonce1());
  const Geo g = geo(b);
  const std::size_t off = L::get_u64(b.data() + g.idx_off + 1 * L::kIndexEntrySize);  // global block 1 = sample 0, block 1
  b[off + 3] ^= 0x80;
  auto r = ProtectedBundleReader::open_memory(b, trusted_for(test_seed()), test_key());
  CHECK(r.ok());
  if (!r.ok()) return;
  std::vector<std::int32_t> out(10000 * 2, 0x5a5a5a5a);
  CHECK_ERR(r.value().read_frames(0, 0, 10000, out), Error::kBlockHashMismatch);
  CHECK(std::equal(out.begin(), out.begin() + kBlockFrames * 2, src.pcm[0].begin()));  // the good block before was delivered
  CHECK(std::all_of(out.begin() + kBlockFrames * 2, out.end(), [](std::int32_t v) { return v == 0x5a5a5a5a; }));  // bad and later: untouched
  CHECK_ERR(r.value().verify_all(), Error::kBlockHashMismatch);
  std::vector<std::int32_t> good2(1808 * 2);
  CHECK_ERR(r.value().read_frames(0, 8192, 1808, good2), Error::kNone);  // the good block after still reads
  CHECK(std::equal(good2.begin(), good2.end(), src.pcm[0].begin() + 8192 * 2));
}

// More than 256 blocks (the block index spills into a second counter byte), through open_file, with deterministic seeks.
void test_many_blocks(const std::string& dir) {
  const PcmFormat f{1, 16, false, 48000};
  const std::uint64_t frames = std::uint64_t(300) * kBlockFrames + 123;
  const auto pcm = vt::make_int(f, frames, Kind::kSine);
  BundleWriter w;
  w.set_library_id(Uuid{9});
  auto i0 = w.add_instrument("Long", "long.sfz");
  CHECK(i0.ok() && w.add_sample(i0.value(), {"long", f, frames, pcm}) == Error::kNone);
  const std::string path = dir + "/big.vlrp";
  CHECK_ERR(finalize_protected_to_file(w, test_key(), nonce1(), test_seed(), path), Error::kNone);
  CHECK(::access((path + ".tmp").c_str(), F_OK) != 0);
  struct stat st;
  CHECK(::stat(path.c_str(), &st) == 0 && (st.st_mode & 0200) != 0);
  auto r = ProtectedBundleReader::open_file(path, trusted_for(test_seed()), test_key());
  CHECK(r.ok());
  if (!r.ok()) return;
  CHECK(r.value().view().total_blocks() == 301);
  vt::Rng rng(77);
  int bad = 0;
  for (int i = 0; i < 400; ++i) {
    const std::uint64_t first = (i < 20 ? frames - 1 - std::uint64_t(i) * 7 : rng.next() % frames);
    const std::uint64_t count = 1 + rng.next() % std::min<std::uint64_t>(5000, frames - first);
    std::vector<std::int32_t> out(count);
    if (r.value().read_frames(0, first, count, out) != Error::kNone || !std::equal(out.begin(), out.end(), pcm.begin() + first)) ++bad;
  }
  CHECK(bad == 0);
  CHECK_ERR(r.value().verify_all(), Error::kNone);
  // The second key writes a different file at the same path atomically (no stale temp, mode kept).
  CHECK_ERR(finalize_protected_to_file(w, test_key(), Nonce{1, 1, 1, 1, 1, 1, 1, 1}, test_seed(), path), Error::kNone);
  CHECK(finalize_protected(w, test_key(), Nonce{}, test_seed()).error() == Error::kInvalidArgument);  // all-zero nonce refused
  auto auto_nonce = finalize_protected(w, test_key(), test_seed());  // content-derived nonce overload
  CHECK(auto_nonce.ok());
}

// Float-only: no libFLAC involved, so these bytes are identical on every platform and the SHA-256 can be pinned.
void test_pinned_fixture() {
  const PcmFormat f{2, 32, true, 48000};
  const auto pcm = vt::make_float(2, 5000);
  BundleWriter w;
  w.set_library_id(Uuid{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
  auto i0 = w.add_instrument("Pad", "pads/warm.sfz");
  CHECK(i0.ok() && w.add_sample(i0.value(), {"warm", f, 5000, pcm}) == Error::kNone);
  const Bytes b = protect(w, nonce1());
  CHECK(b.size() == 128 + geo(b).man_size + geo(b).idx_size + 128 + 5000 * 8);
  CHECK(hex(Sha256::hash(b)) == "004409461620e0d97a7b1216258967d0e8190807e0b50b6caef87a22d860cf34");
  CHECK(ProtectedBundleReader::open_memory(b, trusted_for(test_seed()), test_key()).ok());
}

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : (std::filesystem::temp_directory_path() / "velora_protected_fixture").string();
  test_known_answers();
  test_roundtrip_and_seeks(dir);
  test_tamper();
  test_plaintext_integrity();
  test_good_bad_good();
  test_many_blocks(dir);
  test_pinned_fixture();
  if (g_fail) {
    std::printf("format_protected_test: %d FAILED\n", g_fail);
    return 1;
  }
  std::printf("format_protected_test: ok\n");
  return 0;
}
