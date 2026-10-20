#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <set>

#include "bundle_parse.hpp"
#include "codec.hpp"
#include "layout.hpp"

namespace velora::format {
using namespace layout;

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
  float f32() { return bits_to_float(static_cast<std::int32_t>(u32())); }
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

std::unique_ptr<ByteSource> make_memory_source(std::span<const std::uint8_t> bytes) {
  return std::make_unique<MemorySource>(bytes);
}

Result<std::unique_ptr<ByteSource>> make_file_source(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return Error::kIo;
  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    return Error::kIo;
  }
  return std::unique_ptr<ByteSource>(std::make_unique<FileSource>(fd, static_cast<std::uint64_t>(st.st_size)));
}

ManifestStatement statement_of(const ParsedBundle& p) {
  ManifestStatement s;
  s.version_word = p.version_word;
  s.library_id = p.library_id;
  s.bundle_id = p.bundle_id;
  s.index_sha256 = p.index_hash;
  s.hash_list_root = p.prot.hash_list_root;
  s.block_count = p.block_count;
  s.nonce = p.prot.nonce;
  s.signer_key_id = p.seal.key_id;
  return s;
}

BundleReader::BundleReader() = default;
BundleReader::BundleReader(BundleReader&&) noexcept = default;
BundleReader& BundleReader::operator=(BundleReader&&) noexcept = default;
BundleReader::~BundleReader() = default;

Result<BundleReader> BundleReader::open_memory(std::span<const std::uint8_t> bytes,
                                               ReaderOptions opt) {
  return open(std::make_unique<MemorySource>(bytes), opt);
}

Result<BundleReader> BundleReader::open_source(std::unique_ptr<ByteSource> src, ReaderOptions opt) {
  if (!src) return Error::kInvalidArgument;
  return open(std::move(src), opt);
}

Result<BundleReader> BundleReader::open_file(const std::string& path, ReaderOptions opt) {
  auto src = make_file_source(path);
  if (!src) return src.error();
  return open(src.take(), opt);
}

Result<BundleReader> BundleReader::open(std::unique_ptr<ByteSource> src, ReaderOptions opt) {
  auto parsed = parse_bundle(*src, false, nullptr);
  if (!parsed) return parsed.error();
  ParsedBundle pb = parsed.take();
  BundleReader r;
  r.opt_ = opt;
  r.version_ = pb.version;
  r.minor_ = pb.minor;
  r.bundle_id_ = pb.bundle_id;
  r.library_id_ = pb.library_id;
  r.instruments_ = std::move(pb.instruments);
  r.samples_ = std::move(pb.samples);
  r.index_ = std::move(pb.index);
  r.src_ = std::move(src);
  return r;
}

// Story 6.4: sizes that are accepted before anything is allocated for them. A signer-less or untrusted file must not be able to make a reader allocate a
// file-sized manifest or index: the manifest of a real library is well below a megabyte per thousand instruments, the index is 48 bytes per block.
inline constexpr std::uint64_t kMaxManifestBytes = 64ull << 20;
inline constexpr std::uint64_t kMaxProtectedIndexBytes = 512ull << 20;

Result<ParsedBundle> parse_bundle(ByteSource& srcref, bool want_protected, const SealVerifier* verify, const SealPrecheck* precheck) {
  ByteSource* src = &srcref;
  const std::uint64_t file_size = src->size();
  if (file_size < kHeaderSize) return Error::kTruncated;
  std::uint8_t h[kHeaderSize];
  if (Error e = src->read_at(0, h, kHeaderSize); e != Error::kNone) return e;
  if (std::memcmp(h, kMagic, 4) != 0) return Error::kBadMagic;
  const std::uint32_t version_word = get_u32(h + 4);
  const std::uint32_t version = version_word & 0xFFFFu, minor = version_word >> kMinorShift;
  if (want_protected) {
    if (version == 1 || version == 2) return Error::kNotProtected;
    if (version != kProtectedFormatVersion || minor > kFormatMinor) return Error::kUnsupportedVersion;
  } else if (version < kMinReadableVersion || version > kFormatVersion || minor > kFormatMinor ||
             (version < 2 && minor != 0)) {
    return Error::kUnsupportedVersion;
  }
  const std::uint64_t seal_size = want_protected ? kSealSize : 0;
  const std::uint64_t manifest_off = get_u64(h + 8), manifest_size = get_u64(h + 16),
                      index_off = get_u64(h + 24), index_size = get_u64(h + 32),
                      data_off = get_u64(h + 40), data_size = get_u64(h + 48),
                      total_size = get_u64(h + 56);
  if (total_size > file_size) return Error::kTruncated;
  if (total_size < file_size) return Error::kCorruptHeader;  // trailing bytes
  // Strict layout: header | manifest | index | [seal] | blocks, no gaps. Overflow-safe step by step.
  if (manifest_off != kHeaderSize || manifest_size > total_size ||
      index_off != manifest_off + manifest_size || index_size > total_size ||
      data_off != index_off + index_size + seal_size || data_size > total_size ||
      total_size != data_off + data_size || index_size % kIndexEntrySize != 0)
    return Error::kCorruptHeader;
  if (manifest_size > kMaxManifestBytes || (want_protected && index_size > kMaxProtectedIndexBytes)) return Error::kCorruptHeader;
  Digest256 manifest_hash, index_hash;
  std::memcpy(manifest_hash.data(), h + 64, 32);
  std::memcpy(index_hash.data(), h + 96, 32);
  // Protected: the seal (128 bytes at a position the header fixes) is read FIRST and its signer is checked against the trust list before the manifest and the
  // index are allocated, read and hashed; the signature itself needs the manifest and is verified below.
  SealFields seal;
  if (want_protected) {
    // Seal: magic "VSL1", alg u8, 3 zero bytes, key id[8], signature[64], 48 zero bytes.
    std::uint8_t sb[kSealSize];
    if (Error e = src->read_at(index_off + index_size, sb, kSealSize); e != Error::kNone) return e;
    bool zero_ok = true;
    for (int i = 5; i < 8; ++i) zero_ok = zero_ok && sb[i] == 0;
    for (std::size_t i = 80; i < kSealSize; ++i) zero_ok = zero_ok && sb[i] == 0;
    if (std::memcmp(sb, kSealMagic, 4) != 0 || sb[4] != kSealAlgEd25519 || !zero_ok) return Error::kCorruptHeader;
    seal.alg = sb[4];
    std::memcpy(seal.key_id.data(), sb + 8, 8);
    std::memcpy(seal.signature.data(), sb + 16, 64);
    if (precheck)
      if (Error e = (*precheck)(seal.key_id); e != Error::kNone) return e;
  }

  std::vector<std::uint8_t> manifest(manifest_size);
  if (Error e = src->read_at(manifest_off, manifest.data(), manifest.size()); e != Error::kNone)
    return e;
  if (Sha256::hash(manifest) != manifest_hash) return Error::kCorruptManifest;
  std::vector<std::uint8_t> index_bytes(index_size);
  if (Error e = src->read_at(index_off, index_bytes.data(), index_bytes.size()); e != Error::kNone)
    return e;
  if (Sha256::hash(index_bytes) != index_hash) return Error::kCorruptIndex;

  ParsedBundle r;
  r.version_word = version_word;
  r.version = version;
  r.minor = minor;
  r.bundle_id = manifest_hash;
  r.index_hash = index_hash;
  r.is_protected = want_protected;

  Cursor c(manifest.data(), manifest.size());
  if (c.u32() != version_word) return Error::kCorruptManifest;
  c.bytes(r.library_id.data(), 16);
  const std::uint32_t n_instr = c.u32(), n_samples = c.u32();
  const std::uint64_t n_blocks = c.u64();
  Digest256 manifest_index_hash{};
  c.bytes(manifest_index_hash.data(), 32);
  if (!c.ok() || manifest_index_hash != index_hash || n_blocks != index_size / kIndexEntrySize ||
      n_instr > c.remaining() || n_samples > c.remaining())
    return Error::kCorruptManifest;
  r.block_count = n_blocks;
  if (want_protected) {
    // Protection section (format 3): cipher u8, hash-list alg u8, 2 zero bytes, nonce[8], key_check[16], root[32].
    r.prot.cipher = c.u8();
    r.prot.hash_list_alg = c.u8();
    const std::uint8_t pad0 = c.u8(), pad1 = c.u8();
    c.bytes(r.prot.nonce.data(), 8);
    c.bytes(r.prot.key_check.data(), 16);
    c.bytes(r.prot.hash_list_root.data(), 32);
    if (!c.ok() || r.prot.cipher != kCipherAes256Ctr || r.prot.hash_list_alg != kHashListMerkleSha256 ||
        pad0 != 0 || pad1 != 0 || n_blocks >= kMaxProtectedBlocks)
      return Error::kCorruptManifest;
    r.seal = seal;
    if (!verify) return Error::kInvalidArgument;
    if (Error e = (*verify)(r); e != Error::kNone) return e;
  }
  r.instruments.reserve(n_instr);
  r.samples.reserve(n_samples);
  std::uint64_t next_block = 0;
  std::set<std::string> seen_keys;
  std::uint64_t total_ext = 0;
  for (std::uint32_t i = 0; i < n_instr; ++i) {
    InstrumentInfo ins;
    ins.key = c.str();
    ins.name = c.str();
    const std::uint32_t cnt = c.u32();
    if (!c.ok() || cnt > n_samples - r.samples.size()) return Error::kCorruptManifest;
    auto norm = normalize_instrument_key(ins.key);
    if (!norm || norm.value() != ins.key || !seen_keys.insert(ins.key).second)
      return Error::kCorruptManifest;
    ins.first_sample = r.samples.size();
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
      r.samples.push_back(std::move(si));
    }
    if (version >= 2) {
      const std::uint32_t n_regions = c.u32();
      if (!c.ok() || n_regions > c.remaining() / kRegionRecordSize) return Error::kCorruptManifest;
      ins.regions.reserve(n_regions);
      for (std::uint32_t k = 0; k < n_regions; ++k) {
        Region rg;
        rg.sample = c.u32();
        rg.key_lo = c.u8(); rg.key_hi = c.u8(); rg.key_center = c.u8();
        rg.vel_lo = c.u8(); rg.vel_hi = c.u8();
        const std::uint8_t lm = c.u8(), tr = c.u8(), hl = c.u8();
        rg.transpose = static_cast<std::int32_t>(c.u32());
        rg.tune_cents = c.f32(); rg.volume_db = c.f32(); rg.pan = c.f32();
        rg.offset = c.u64();
        rg.seq_length = c.u32(); rg.seq_position = c.u32();
        rg.lorand = c.f32(); rg.hirand = c.f32();
        rg.loop_start = c.u64(); rg.loop_end = c.u64();
        rg.ampeg.delay = c.f32(); rg.ampeg.start = c.f32(); rg.ampeg.attack = c.f32();
        rg.ampeg.hold = c.f32(); rg.ampeg.decay = c.f32(); rg.ampeg.sustain = c.f32();
        rg.ampeg.release = c.f32();
        if (!c.ok() || lm > 3 || tr > 1 || hl > 1 || rg.sample >= cnt) return Error::kCorruptManifest;
        rg.loop_mode = static_cast<LoopMode>(lm);
        rg.trigger = static_cast<TriggerMode>(tr);
        rg.has_loop_points = hl == 1;
        if (validate_region(rg, r.samples[ins.first_sample + rg.sample].frame_count) != Error::kNone)
          return Error::kCorruptManifest;
        ins.regions.push_back(rg);
      }
      if (minor >= 1) {
        const std::uint32_t n_ext = c.u32();
        if (!c.ok() || n_ext > n_regions || n_ext > c.remaining() / kExtRecordSize)
          return Error::kCorruptManifest;
        total_ext += n_ext;
        std::int64_t prev = -1;
        for (std::uint32_t k = 0; k < n_ext; ++k) {
          const std::uint32_t idx = c.u32();
          if (!c.ok() || idx >= n_regions || std::int64_t(idx) <= prev) return Error::kCorruptManifest;
          prev = idx;
          Region& rg = ins.regions[idx];
          const std::uint8_t flags = c.u8();
          std::uint8_t notes[7];
          for (auto& n : notes) n = c.u8();
          rg.xf.in_count = c.u8(); rg.xf.out_count = c.u8();
          const std::uint8_t curve = c.u8(), ftype = c.u8();
          std::uint8_t pad_bad = 0;
          for (CcRange* set : {rg.xf.in, rg.xf.out})
            for (unsigned e = 0; e < kMaxCcCrossfades; ++e) {
              set[e].cc = c.u8(); set[e].lo = c.u8(); set[e].hi = c.u8();
              pad_bad |= c.u8();
            }
          rg.filter.cutoff_hz = c.f32(); rg.filter.resonance_db = c.f32();
          if (!c.ok() || (flags & 0x80) || pad_bad || curve > 1 || ftype > 5) return Error::kCorruptManifest;
          std::int16_t* dst[7] = {&rg.sw.lokey, &rg.sw.hikey, &rg.sw.last, &rg.sw.down,
                                  &rg.sw.up, &rg.sw.previous, &rg.sw.default_key};
          for (int n = 0; n < 7; ++n) {
            if (flags & (1u << n)) *dst[n] = notes[n];
            else if (notes[n] != 0) return Error::kCorruptManifest;
          }
          rg.xf.curve = static_cast<XfCurve>(curve);
          rg.filter.type = static_cast<FilterType>(ftype);
          // Canonical: a record exists only for a region that really uses an extension feature.
          if (!has_extension(rg) ||
              validate_region(rg, r.samples[ins.first_sample + rg.sample].frame_count) != Error::kNone)
            return Error::kCorruptManifest;
        }
      }
    }
    r.instruments.push_back(std::move(ins));
  }
  if (!c.ok() || !c.done() || r.samples.size() != n_samples || next_block != n_blocks)
    return Error::kCorruptManifest;
  // Canonical: minor 1 exists only for bundles that carry at least one extension record (the same
  // content must not have two encodings, hence two bundle ids).
  if (minor >= 1 && total_ext == 0) return Error::kCorruptManifest;

  // Index: contiguous blocks tiling [data_off, total_size), sizes/frames consistent with samples.
  r.index.resize(n_blocks);
  std::uint64_t expect_off = data_off;
  std::size_t si = 0;
  std::uint64_t in_sample = 0;
  for (std::uint64_t b = 0; b < n_blocks; ++b) {
    while (r.samples[si].block_count == 0 || in_sample == r.samples[si].block_count) {
      ++si;
      in_sample = 0;
    }
    const SampleInfo& s = r.samples[si];
    const std::uint8_t* p = index_bytes.data() + b * kIndexEntrySize;
    detail::BlockEntry& e = r.index[b];
    e.offset = get_u64(p);
    e.size = get_u32(p + 8);
    e.frames = get_u32(p + 12);
    if (want_protected && e.size > kMaxProtectedBlockBytes) return Error::kCorruptIndex;
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

  if (want_protected) {
    std::vector<Digest256> hashes;
    hashes.reserve(r.index.size());
    for (const auto& e : r.index) hashes.push_back(e.sha256);
    if (hash_list_root(hashes) != r.prot.hash_list_root) return Error::kHashListMismatch;
  }
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

Error BundleReader::decode_block_bytes(BlockDecoder& dec, const PcmFormat& f, std::uint32_t frames,
                                       std::span<const std::uint8_t> bytes, std::span<std::int32_t> out) {
  if (!dec.impl_) return Error::kInvalidArgument;
  const std::size_t ch = f.channels;
  if (out.size() < std::size_t(frames) * ch) return Error::kBufferTooSmall;
  if (f.is_float) {
    if (bytes.size() != std::size_t(frames) * ch * 4) return Error::kCorruptBlock;
    for (std::size_t i = 0; i < std::size_t(frames) * ch; ++i)
      out[i] = static_cast<std::int32_t>(get_u32(bytes.data() + 4 * i));
  } else if (Error err = flac_decode_block(*dec.impl_, bytes.data(), bytes.size(), f, frames, out.data());
             err != Error::kNone) {
    return err;
  }
  count_block(*dec.impl_);
  return Error::kNone;
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
