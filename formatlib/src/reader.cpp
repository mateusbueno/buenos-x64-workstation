#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <set>

#include "codec.hpp"
#include "layout.hpp"

namespace velora::format {
using namespace layout;

class ByteSource {
 public:
  virtual ~ByteSource() = default;
  virtual std::uint64_t size() const = 0;
  // Pointer to [off, off+n) when the bytes are directly addressable, else nullptr.
  virtual const std::uint8_t* direct(std::uint64_t off, std::size_t n) const = 0;
  virtual Error read_at(std::uint64_t off, void* dst, std::size_t n) const = 0;
};

namespace {

class MemorySource final : public ByteSource {
 public:
  explicit MemorySource(std::span<const std::uint8_t> b) : b_(b) {}
  std::uint64_t size() const override { return b_.size(); }
  const std::uint8_t* direct(std::uint64_t off, std::size_t n) const override {
    return (off <= b_.size() && n <= b_.size() - off) ? b_.data() + off : nullptr;
  }
  Error read_at(std::uint64_t off, void* dst, std::size_t n) const override {
    const auto* p = direct(off, n);
    if (!p) return Error::kTruncated;
    std::memcpy(dst, p, n);
    return Error::kNone;
  }

 private:
  std::span<const std::uint8_t> b_;
};

class FileSource final : public ByteSource {
 public:
  FileSource(int fd, std::uint64_t size) : fd_(fd), size_(size) {}
  ~FileSource() override { ::close(fd_); }
  std::uint64_t size() const override { return size_; }
  const std::uint8_t* direct(std::uint64_t, std::size_t) const override { return nullptr; }
  Error read_at(std::uint64_t off, void* dst, std::size_t n) const override {
    if (off > size_ || n > size_ - off) return Error::kTruncated;
    auto* p = static_cast<std::uint8_t*>(dst);
    while (n > 0) {
      const ssize_t r = ::pread(fd_, p, n, static_cast<off_t>(off));
      if (r < 0) {
        if (errno == EINTR) continue;
        return Error::kIo;
      }
      if (r == 0) return Error::kTruncated;
      p += r;
      off += static_cast<std::uint64_t>(r);
      n -= static_cast<std::size_t>(r);
    }
    return Error::kNone;
  }

 private:
  int fd_;
  std::uint64_t size_;
};

class Cursor {
 public:
  Cursor(const std::uint8_t* p, std::size_t n) : p_(p), n_(n) {}
  bool ok() const { return ok_; }
  std::size_t remaining() const { return n_ - pos_; }
  bool done() const { return pos_ == n_; }
  std::uint8_t u8() { return take(1) ? p_[pos_ - 1] : 0; }
  std::uint32_t u32() { return take(4) ? get_u32(p_ + pos_ - 4) : 0; }
  std::uint64_t u64() { return take(8) ? get_u64(p_ + pos_ - 8) : 0; }
  void bytes(void* dst, std::size_t n) {
    if (take(n)) std::memcpy(dst, p_ + pos_ - n, n);
  }
  std::string str() {
    const std::uint32_t len = u32();
    if (!ok_ || len > kMaxStringLen || !take(len)) {
      ok_ = false;
      return {};
    }
    return std::string(reinterpret_cast<const char*>(p_ + pos_ - len), len);
  }

 private:
  bool take(std::size_t n) {
    if (!ok_ || n > n_ - pos_) {
      ok_ = false;
      return false;
    }
    pos_ += n;
    return true;
  }
  const std::uint8_t* p_;
  std::size_t n_, pos_ = 0;
  bool ok_ = true;
};

}  // namespace

BundleReader::BundleReader() = default;
BundleReader::BundleReader(BundleReader&&) noexcept = default;
BundleReader& BundleReader::operator=(BundleReader&&) noexcept = default;
BundleReader::~BundleReader() = default;

Result<BundleReader> BundleReader::open_memory(std::span<const std::uint8_t> bytes,
                                               ReaderOptions opt) {
  return open(std::make_unique<MemorySource>(bytes), opt);
}

Result<BundleReader> BundleReader::open_file(const std::string& path, ReaderOptions opt) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return Error::kIo;
  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    return Error::kIo;
  }
  return open(std::make_unique<FileSource>(fd, static_cast<std::uint64_t>(st.st_size)), opt);
}

Result<BundleReader> BundleReader::open(std::unique_ptr<ByteSource> src, ReaderOptions opt) {
  const std::uint64_t file_size = src->size();
  if (file_size < kHeaderSize) return Error::kTruncated;
  std::uint8_t h[kHeaderSize];
  if (Error e = src->read_at(0, h, kHeaderSize); e != Error::kNone) return e;
  if (std::memcmp(h, kMagic, 4) != 0) return Error::kBadMagic;
  const std::uint32_t version = get_u32(h + 4);
  if (version < kMinReadableVersion || version > kFormatVersion) return Error::kUnsupportedVersion;
  const std::uint64_t manifest_off = get_u64(h + 8), manifest_size = get_u64(h + 16),
                      index_off = get_u64(h + 24), index_size = get_u64(h + 32),
                      data_off = get_u64(h + 40), data_size = get_u64(h + 48),
                      total_size = get_u64(h + 56);
  if (total_size > file_size) return Error::kTruncated;
  if (total_size < file_size) return Error::kCorruptHeader;  // trailing bytes
  // Strict layout: header | manifest | index | blocks, no gaps. Overflow-safe step by step.
  if (manifest_off != kHeaderSize || manifest_size > total_size ||
      index_off != manifest_off + manifest_size || index_size > total_size ||
      data_off != index_off + index_size || data_size > total_size ||
      total_size != data_off + data_size || index_size % kIndexEntrySize != 0)
    return Error::kCorruptHeader;
  Digest256 manifest_hash, index_hash;
  std::memcpy(manifest_hash.data(), h + 64, 32);
  std::memcpy(index_hash.data(), h + 96, 32);

  std::vector<std::uint8_t> manifest(manifest_size);
  if (Error e = src->read_at(manifest_off, manifest.data(), manifest.size()); e != Error::kNone)
    return e;
  if (Sha256::hash(manifest) != manifest_hash) return Error::kCorruptManifest;
  std::vector<std::uint8_t> index_bytes(index_size);
  if (Error e = src->read_at(index_off, index_bytes.data(), index_bytes.size()); e != Error::kNone)
    return e;
  if (Sha256::hash(index_bytes) != index_hash) return Error::kCorruptIndex;

  BundleReader r;
  r.opt_ = opt;
  r.version_ = version;
  r.bundle_id_ = manifest_hash;

  Cursor c(manifest.data(), manifest.size());
  if (c.u32() != version) return Error::kCorruptManifest;
  c.bytes(r.library_id_.data(), 16);
  const std::uint32_t n_instr = c.u32(), n_samples = c.u32();
  const std::uint64_t n_blocks = c.u64();
  Digest256 manifest_index_hash{};
  c.bytes(manifest_index_hash.data(), 32);
  if (!c.ok() || manifest_index_hash != index_hash || n_blocks != index_size / kIndexEntrySize ||
      n_instr > c.remaining() || n_samples > c.remaining())
    return Error::kCorruptManifest;
  r.instruments_.reserve(n_instr);
  r.samples_.reserve(n_samples);
  std::uint64_t next_block = 0;
  std::set<std::string> seen_keys;
  for (std::uint32_t i = 0; i < n_instr; ++i) {
    InstrumentInfo ins;
    ins.key = c.str();
    ins.name = c.str();
    const std::uint32_t cnt = c.u32();
    if (!c.ok() || cnt > n_samples - r.samples_.size()) return Error::kCorruptManifest;
    auto norm = normalize_instrument_key(ins.key);
    if (!norm || norm.value() != ins.key || !seen_keys.insert(ins.key).second)
      return Error::kCorruptManifest;
    ins.first_sample = r.samples_.size();
    ins.sample_count = cnt;
    for (std::uint32_t s = 0; s < cnt; ++s) {
      SampleInfo si;
      si.name = c.str();
      si.format.channels = c.u8();
      si.format.bits_per_sample = c.u8();
      const std::uint8_t fl = c.u8();
      const std::uint8_t pad = c.u8();
      si.format.sample_rate = c.u32();
      si.frame_count = c.u64();
      const std::uint32_t block_frames = c.u32();
      si.block_count = c.u32();
      si.first_block = c.u64();
      c.bytes(si.pcm_sha256.data(), 32);
      si.instrument = i;
      si.format.is_float = fl == 1;
      if (!c.ok() || fl > 1 || pad != 0 || !is_supported(si.format) ||
          block_frames != kBlockFrames || si.frame_count > (std::uint64_t(1) << 40) ||
          si.block_count != block_count_for(si.frame_count) || si.first_block != next_block)
        return Error::kCorruptManifest;
      next_block += si.block_count;
      r.samples_.push_back(std::move(si));
    }
    r.instruments_.push_back(std::move(ins));
  }
  if (!c.ok() || !c.done() || r.samples_.size() != n_samples || next_block != n_blocks)
    return Error::kCorruptManifest;

  // Index: contiguous blocks tiling [data_off, total_size), sizes/frames consistent with samples.
  r.index_.resize(n_blocks);
  std::uint64_t expect_off = data_off;
  std::size_t si = 0;
  std::uint64_t in_sample = 0;
  for (std::uint64_t b = 0; b < n_blocks; ++b) {
    while (r.samples_[si].block_count == 0 || in_sample == r.samples_[si].block_count) {
      ++si;
      in_sample = 0;
    }
    const SampleInfo& s = r.samples_[si];
    const std::uint8_t* p = index_bytes.data() + b * kIndexEntrySize;
    IndexEntry& e = r.index_[b];
    e.offset = get_u64(p);
    e.size = get_u32(p + 8);
    e.frames = get_u32(p + 12);
    std::memcpy(e.sha256.data(), p + 16, 32);
    const std::uint64_t start = in_sample * kBlockFrames;
    const std::uint32_t want_frames =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(kBlockFrames, s.frame_count - start));
    if (e.offset != expect_off || e.size == 0 || e.size > total_size - e.offset ||
        e.frames != want_frames)
      return Error::kCorruptIndex;
    if (s.format.is_float && e.size != std::uint64_t(e.frames) * s.format.channels * 4)
      return Error::kCorruptIndex;
    // FLAC blocks: raw PCM size plus generous framing overhead (stream header, frame/subframe
    // headers, CRCs). Anything larger cannot be a valid encoding of this block.
    if (!s.format.is_float &&
        e.size > std::uint64_t(e.frames) * s.format.channels * (s.format.bits_per_sample / 8) + 1024)
      return Error::kCorruptIndex;
    expect_off += e.size;
    ++in_sample;
  }
  if (expect_off != total_size) return Error::kCorruptIndex;

  r.src_ = std::move(src);
  return r;
}

Result<std::uint32_t> BundleReader::read_block(std::size_t sample, std::uint64_t block,
                                               std::span<std::int32_t> out,
                                               BlockDecoder& dec) const {
  return read_block_impl(sample, block, out, dec, opt_.verify_block_hash);
}

Result<std::uint32_t> BundleReader::read_block_impl(std::size_t sample, std::uint64_t block,
                                                    std::span<std::int32_t> out,
                                                    BlockDecoder& dec, bool verify_hash) const {
  if (!dec.impl_) return Error::kInvalidArgument;  // moved-from decoder
  if (sample >= samples_.size()) return Error::kOutOfRange;
  const SampleInfo& s = samples_[sample];
  if (block >= s.block_count) return Error::kOutOfRange;
  const IndexEntry& e = index_[s.first_block + block];
  const std::size_t ch = s.format.channels;
  if (out.size() < std::size_t(e.frames) * ch) return Error::kBufferTooSmall;

  const std::uint8_t* bytes = src_->direct(e.offset, e.size);
  BlockDecoder::Impl& ctx = *dec.impl_;
  if (!bytes) {
    auto& scratch = scratch_of(ctx);
    if (scratch.size() < e.size) scratch.resize(e.size);
    if (Error err = src_->read_at(e.offset, scratch.data(), e.size); err != Error::kNone)
      return err;
    bytes = scratch.data();
  }
  if (verify_hash && Sha256::hash({bytes, e.size}) != e.sha256)
    return Error::kBlockHashMismatch;

  if (s.format.is_float) {
    for (std::size_t i = 0; i < std::size_t(e.frames) * ch; ++i)
      out[i] = static_cast<std::int32_t>(get_u32(bytes + 4 * i));
  } else if (Error err = flac_decode_block(ctx, bytes, e.size, s.format, e.frames, out.data());
             err != Error::kNone) {
    return err;
  }
  count_block(ctx);
  return e.frames;
}

Result<std::uint32_t> BundleReader::read_block(std::size_t sample, std::uint64_t block,
                                               std::span<std::int32_t> out) const {
  BlockDecoder dec;
  return read_block(sample, block, out, dec);
}

Error BundleReader::read_frames(std::size_t sample, std::uint64_t first, std::uint64_t count,
                                std::span<std::int32_t> out) const {
  if (sample >= samples_.size()) return Error::kOutOfRange;
  const SampleInfo& s = samples_[sample];
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
    std::memcpy(out.data() + (pos - first) * ch, tmp.data() + from * ch,
                (to - from) * ch * sizeof(std::int32_t));
    pos = bstart + to;
  }
  return Error::kNone;
}

Error BundleReader::verify_all() const {
  BlockDecoder dec;
  std::vector<std::int32_t> tmp(std::size_t(kBlockFrames) * 2);
  for (std::size_t si = 0; si < samples_.size(); ++si) {
    const SampleInfo& s = samples_[si];
    Sha256 pcm;
    for (std::uint64_t b = 0; b < s.block_count; ++b) {
      auto r = read_block_impl(si, b, tmp, dec, true);  // always hash-checked
      if (!r) return r.error();
      pcm_hash_update(pcm, s.format, tmp.data(), std::size_t(r.value()) * s.format.channels);
    }
    if (pcm.finish() != s.pcm_sha256) return Error::kPcmHashMismatch;
  }
  return Error::kNone;
}

}  // namespace velora::format
