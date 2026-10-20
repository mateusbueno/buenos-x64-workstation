// Story 6.2: the decrypting reader. This translation unit is the ONLY place of the read side that initialises a cipher; it is
// compiled into velora_format_decrypt, which desktop/librarian targets never link.
#include <openssl/evp.h>
#include <openssl/crypto.h>

#include <algorithm>
#include <cstring>

#include "layout.hpp"
#include "velora/format/protected_reader.hpp"

namespace velora::format {
namespace {

struct CipherDeleter {
  void operator()(EVP_CIPHER_CTX* c) const { EVP_CIPHER_CTX_free(c); }
};

// AES-256-CTR decryption of one block: counter block = nonce || BE32(global block index) || BE32(0).
Error decrypt_block(const std::array<std::uint8_t, 32>& key, const Nonce& nonce, std::uint32_t global_block,
                    std::span<std::uint8_t> data) {
  const CounterBlock iv = counter_block(nonce, global_block);
  std::unique_ptr<EVP_CIPHER_CTX, CipherDeleter> ctx(EVP_CIPHER_CTX_new());
  if (!ctx) return Error::kIo;
  if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_ctr(), nullptr, key.data(), iv.data()) != 1) return Error::kIo;
  int outl = 0;
  if (EVP_DecryptUpdate(ctx.get(), data.data(), &outl, data.data(), static_cast<int>(data.size())) != 1 ||
      static_cast<std::size_t>(outl) != data.size())
    return Error::kIo;
  return Error::kNone;
}

}  // namespace

struct ProtectedBundleReader::Impl {
  ProtectedBundleView view;
  ContentKey key;
  explicit Impl(ProtectedBundleView v, const ContentKey& k) : view(std::move(v)), key(k) {}
  ~Impl() { OPENSSL_cleanse(key.bytes.data(), key.bytes.size()); }
};

ProtectedBundleReader::ProtectedBundleReader() = default;
ProtectedBundleReader::ProtectedBundleReader(ProtectedBundleReader&&) noexcept = default;
ProtectedBundleReader& ProtectedBundleReader::operator=(ProtectedBundleReader&&) noexcept = default;
ProtectedBundleReader::~ProtectedBundleReader() = default;

const ProtectedBundleView& ProtectedBundleReader::view() const { return impl_->view; }

Result<ProtectedBundleReader> ProtectedBundleReader::finish(Result<ProtectedBundleView> v, const ContentKey& key) {
  if (!v) return v.error();
  const KeyCheck want = key_check_value(key.bytes, v.value().info().nonce);
  if (!ct_equal(want, v.value().info().key_check)) return Error::kWrongContentKey;
  ProtectedBundleReader r;
  r.impl_ = std::make_unique<Impl>(v.take(), key);
  return r;
}

Result<ProtectedBundleReader> ProtectedBundleReader::open_memory(std::span<const std::uint8_t> bytes,
                                                                 const TrustedKeys& trusted, const ContentKey& key) {
  return finish(ProtectedBundleView::open_memory(bytes, trusted), key);
}
Result<ProtectedBundleReader> ProtectedBundleReader::open_source(std::unique_ptr<ByteSource> src,
                                                                 const TrustedKeys& trusted, const ContentKey& key) {
  return finish(ProtectedBundleView::open_source(std::move(src), trusted), key);
}
Result<ProtectedBundleReader> ProtectedBundleReader::open_file(const std::string& path, const TrustedKeys& trusted,
                                                               const ContentKey& key) {
  return finish(ProtectedBundleView::open_file(path, trusted), key);
}

Result<std::uint32_t> ProtectedBundleReader::read_block(std::size_t sample, std::uint64_t block,
                                                        std::span<std::int32_t> out, BlockDecoder& dec) const {
  auto ref = impl_->view.locate(sample, block);
  if (!ref) return ref.error();
  const SampleInfo& s = impl_->view.samples()[sample];
  if (out.size() < std::size_t(ref.value().frames) * s.format.channels) return Error::kBufferTooSmall;
  std::vector<std::uint8_t> bytes;
  if (Error e = impl_->view.read_ciphertext(ref.value().global, bytes); e != Error::kNone) return e;  // hash first
  if (Error e = decrypt_block(impl_->key.bytes, impl_->view.info().nonce,
                              static_cast<std::uint32_t>(ref.value().global), bytes);
      e != Error::kNone) {
    OPENSSL_cleanse(bytes.data(), bytes.size());
    return e;
  }
  // Decode into scratch so `out` stays untouched when the plaintext fails the decoder's integrity checks.
  std::vector<std::int32_t> tmp(std::size_t(ref.value().frames) * s.format.channels);
  const Error derr = BundleReader::decode_block_bytes(dec, s.format, ref.value().frames, bytes, tmp);
  OPENSSL_cleanse(bytes.data(), bytes.size());  // decrypted FLAC bytes
  if (derr == Error::kNone) std::copy(tmp.begin(), tmp.end(), out.begin());
  OPENSSL_cleanse(tmp.data(), tmp.size() * sizeof(std::int32_t));  // decoded PCM copy
  if (derr != Error::kNone) return derr;
  return ref.value().frames;
}

Error ProtectedBundleReader::read_frames(std::size_t sample, std::uint64_t first, std::uint64_t count,
                                         std::span<std::int32_t> out) const {
  const auto samples = impl_->view.samples();
  if (sample >= samples.size()) return Error::kOutOfRange;
  const SampleInfo& s = samples[sample];
  if (first > s.frame_count || count > s.frame_count - first) return Error::kOutOfRange;
  const std::size_t ch = s.format.channels;
  if (out.size() < count * ch) return Error::kBufferTooSmall;
  if (count == 0) return Error::kNone;
  BlockDecoder dec;
  std::vector<std::int32_t> tmp(std::size_t(kBlockFrames) * ch);
  std::uint64_t pos = first;
  const std::uint64_t end = first + count;
  while (pos < end) {
    const std::uint64_t b = pos / kBlockFrames;
    auto r = read_block(sample, b, tmp, dec);
    if (!r) return r.error();
    const std::uint64_t bstart = b * kBlockFrames;
    const std::uint64_t from = pos - bstart;
    const std::uint64_t to = std::min<std::uint64_t>(r.value(), end - bstart);
    std::memcpy(out.data() + (pos - first) * ch, tmp.data() + from * ch, (to - from) * ch * sizeof(std::int32_t));
    pos = bstart + to;
  }
  return Error::kNone;
}

Error ProtectedBundleReader::verify_all() const {
  BlockDecoder dec;
  std::vector<std::int32_t> tmp;
  const auto samples = impl_->view.samples();
  for (std::size_t si = 0; si < samples.size(); ++si) {
    const SampleInfo& s = samples[si];
    tmp.assign(std::size_t(kBlockFrames) * s.format.channels, 0);
    Sha256 pcm;
    for (std::uint64_t b = 0; b < s.block_count; ++b) {
      auto r = read_block(si, b, tmp, dec);
      if (!r) return r.error();
      layout::pcm_hash_update(pcm, s.format, tmp.data(), std::size_t(r.value()) * s.format.channels);
    }
    if (pcm.finish() != s.pcm_sha256) return Error::kPcmHashMismatch;
  }
  return Error::kNone;
}

}  // namespace velora::format
