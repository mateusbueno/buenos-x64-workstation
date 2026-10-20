// Story 6.2: protected bundle verification. Links libcrypto for Ed25519 verification ONLY: no cipher is initialised here.
#include <openssl/evp.h>

#include <cstring>

#include "bundle_parse.hpp"
#include "velora/format/protected_verify.hpp"

namespace velora::format {
namespace {

// Ed25519 group order L (little-endian) and field prime p = 2^255 - 19 (little-endian).
constexpr std::uint8_t kL[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10};
constexpr std::uint8_t kP[32] = {0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f};

// a >= b for 32-byte little-endian integers.
bool ge_le(const std::uint8_t* a, const std::uint8_t* b) {
  for (int i = 31; i >= 0; --i) {
    if (a[i] != b[i]) return a[i] > b[i];
  }
  return true;
}

// y coordinate (sign bit cleared) >= p: a non-canonical field element encoding.
bool y_noncanonical(const std::uint8_t* enc) {
  std::uint8_t y[32];
  std::memcpy(y, enc, 32);
  y[31] &= 0x7f;
  return ge_le(y, kP);
}

// The 8 torsion points as canonical-y encodings: 10 byte strings (the 8 points, plus the second sign-bit spelling of the two
// points with x = 0, which is a non-canonical encoding of the same point). The remaining non-canonical spellings have y >= p and
// are rejected by y_noncanonical() before this table is consulted.
bool small_order(const std::uint8_t* enc) {
  static const char* const kHex[] = {
      "0100000000000000000000000000000000000000000000000000000000000000",  // (0, 1) identity
      "0100000000000000000000000000000000000000000000000000000000000080",  // identity, sign set (non-canonical)
      "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",  // (0, -1) order 2
      "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",  // order 2, sign set (non-canonical)
      "0000000000000000000000000000000000000000000000000000000000000000",  // (+-sqrt(-1), 0) order 4
      "0000000000000000000000000000000000000000000000000000000000000080",
      "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a",  // order 8
      "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa",
      "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc05",  // order 8
      "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc85",
  };
  for (const char* hex : kHex) {
    std::uint8_t b[32];
    for (int i = 0; i < 32; ++i) {
      auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
      b[i] = static_cast<std::uint8_t>(nib(hex[2 * i]) * 16 + nib(hex[2 * i + 1]));
    }
    if (std::memcmp(b, enc, 32) == 0) return true;
  }
  return false;
}

struct PkeyDeleter {
  void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); }
};
struct MdDeleter {
  void operator()(EVP_MD_CTX* p) const { EVP_MD_CTX_free(p); }
};

}  // namespace

bool ed25519_verify_strict(const PublicKey& pub, std::span<const std::uint8_t> message, const Signature& sig) {
  const std::uint8_t* r = sig.data();
  const std::uint8_t* s = sig.data() + 32;
  if (ge_le(s, kL)) return false;                         // S must be < L (no malleable S + L)
  if (y_noncanonical(r) || y_noncanonical(pub.data())) return false;
  if (small_order(pub.data())) return false;
  if (small_order(r)) return false;  // stricter than RFC 8032: a small-order R never occurs in an honest signature
  // OpenSSL's Ed25519 verify is cofactorless: it recomputes R' = [S]B - [k]A and compares the encoding with R.
  std::unique_ptr<EVP_PKEY, PkeyDeleter> key(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub.data(), 32));
  if (!key) return false;
  std::unique_ptr<EVP_MD_CTX, MdDeleter> ctx(EVP_MD_CTX_new());
  if (!ctx) return false;
  if (EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1) return false;
  return EVP_DigestVerify(ctx.get(), sig.data(), sig.size(), message.data(), message.size()) == 1;
}

struct ProtectedBundleView::Impl {
  std::unique_ptr<ByteSource> src;
  ParsedBundle pb;
  ProtectedInfo info;
};

ProtectedBundleView::ProtectedBundleView() = default;
ProtectedBundleView::ProtectedBundleView(ProtectedBundleView&&) noexcept = default;
ProtectedBundleView& ProtectedBundleView::operator=(ProtectedBundleView&&) noexcept = default;
ProtectedBundleView::~ProtectedBundleView() = default;

Result<ProtectedBundleView> ProtectedBundleView::open_memory(std::span<const std::uint8_t> bytes,
                                                             const TrustedKeys& trusted) {
  return open(make_memory_source(bytes), trusted);
}

Result<ProtectedBundleView> ProtectedBundleView::open_source(std::unique_ptr<ByteSource> src,
                                                             const TrustedKeys& trusted) {
  if (!src) return Error::kInvalidArgument;
  return open(std::move(src), trusted);
}

Result<ProtectedBundleView> ProtectedBundleView::open_file(const std::string& path, const TrustedKeys& trusted) {
  auto src = make_file_source(path);
  if (!src) return src.error();
  return open(src.take(), trusted);
}

Result<ProtectedBundleView> ProtectedBundleView::open(std::unique_ptr<ByteSource> src, const TrustedKeys& trusted) {
  const SealVerifier verify = [&trusted](const ParsedBundle& p) -> Error {
    const TrustedKey* k = trusted.find(p.seal.key_id);
    if (!k) return Error::kUntrustedKey;
    if (key_id_of_public(k->pub) != k->id) return Error::kUntrustedKey;  // a list entry whose id does not match its key
    const std::vector<std::uint8_t> msg = statement_bytes(statement_of(p));
    return ed25519_verify_strict(k->pub, msg, p.seal.signature) ? Error::kNone : Error::kSignatureInvalid;
  };
  auto parsed = parse_bundle(*src, true, &verify);
  if (!parsed) return parsed.error();
  ProtectedBundleView v;
  v.impl_ = std::make_unique<Impl>();
  v.impl_->pb = parsed.take();
  const ParsedBundle& pb = v.impl_->pb;
  ProtectedInfo& i = v.impl_->info;
  i.format_version = pb.version;
  i.format_minor = pb.minor;
  i.library_id = pb.library_id;
  i.bundle_id = pb.bundle_id;
  i.index_hash = pb.index_hash;
  i.hash_list_root = pb.prot.hash_list_root;
  i.nonce = pb.prot.nonce;
  i.key_check = pb.prot.key_check;
  i.signer_key_id = pb.seal.key_id;
  v.impl_->src = std::move(src);
  return v;
}

const ProtectedInfo& ProtectedBundleView::info() const { return impl_->info; }
std::span<const InstrumentInfo> ProtectedBundleView::instruments() const { return impl_->pb.instruments; }
std::span<const SampleInfo> ProtectedBundleView::samples() const { return impl_->pb.samples; }
std::uint64_t ProtectedBundleView::total_blocks() const { return impl_->pb.index.size(); }

Error ProtectedBundleView::read_ciphertext(std::uint64_t global_block, std::vector<std::uint8_t>& out) const {
  out.clear();
  if (global_block >= impl_->pb.index.size()) return Error::kOutOfRange;
  const detail::BlockEntry& e = impl_->pb.index[global_block];
  std::vector<std::uint8_t> tmp(e.size);
  if (Error err = impl_->src->read_at(e.offset, tmp.data(), tmp.size()); err != Error::kNone) return err;
  if (Sha256::hash(tmp) != e.sha256) return Error::kBlockHashMismatch;
  out = std::move(tmp);
  return Error::kNone;
}

Result<ProtectedBundleView::BlockRef> ProtectedBundleView::locate(std::size_t sample, std::uint64_t block) const {
  if (sample >= impl_->pb.samples.size()) return Error::kOutOfRange;
  const SampleInfo& s = impl_->pb.samples[sample];
  if (block >= s.block_count) return Error::kOutOfRange;
  const std::uint64_t g = s.first_block + block;
  return BlockRef{g, impl_->pb.index[g].size, impl_->pb.index[g].frames};
}

Error ProtectedBundleView::verify_all_blocks() const {
  std::vector<std::uint8_t> buf;
  for (std::uint64_t b = 0; b < impl_->pb.index.size(); ++b)
    if (Error e = read_ciphertext(b, buf); e != Error::kNone) return e;
  return Error::kNone;
}

}  // namespace velora::format
