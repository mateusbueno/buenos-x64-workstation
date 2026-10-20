#include <unistd.h>

#include <cstdio>

#include "codec.hpp"
#include "layout.hpp"

namespace velora::format {
using namespace layout;

namespace {
void put_str(std::vector<std::uint8_t>& v, const std::string& s) {
  put_u32(v, static_cast<std::uint32_t>(s.size()));
  v.insert(v.end(), s.begin(), s.end());
}
void put_f32(std::vector<std::uint8_t>& v, float f) { put_u32(v, static_cast<std::uint32_t>(float_to_bits(f))); }

void put_region(std::vector<std::uint8_t>& v, const Region& r) {
  put_u32(v, r.sample);
  put_u8(v, r.key_lo); put_u8(v, r.key_hi); put_u8(v, r.key_center);
  put_u8(v, r.vel_lo); put_u8(v, r.vel_hi);
  put_u8(v, static_cast<std::uint8_t>(r.loop_mode));
  put_u8(v, static_cast<std::uint8_t>(r.trigger));
  put_u8(v, r.has_loop_points ? 1 : 0);
  put_u32(v, static_cast<std::uint32_t>(r.transpose));
  put_f32(v, r.tune_cents); put_f32(v, r.volume_db); put_f32(v, r.pan);
  put_u64(v, r.offset);
  put_u32(v, r.seq_length); put_u32(v, r.seq_position);
  put_f32(v, r.lorand); put_f32(v, r.hirand);
  put_u64(v, r.loop_start); put_u64(v, r.loop_end);
  put_f32(v, r.ampeg.delay); put_f32(v, r.ampeg.start); put_f32(v, r.ampeg.attack);
  put_f32(v, r.ampeg.hold); put_f32(v, r.ampeg.decay); put_f32(v, r.ampeg.sustain);
  put_f32(v, r.ampeg.release);
}

void put_ext(std::vector<std::uint8_t>& v, std::uint32_t index, const Region& r) {
  const Keyswitch& k = r.sw;
  const std::int16_t notes[7] = {k.lokey, k.hikey, k.last, k.down, k.up, k.previous, k.default_key};
  std::uint8_t flags = 0;
  for (int i = 0; i < 7; ++i)
    if (notes[i] >= 0) flags = static_cast<std::uint8_t>(flags | (1u << i));
  put_u32(v, index);
  put_u8(v, flags);
  for (int i = 0; i < 7; ++i) put_u8(v, notes[i] >= 0 ? static_cast<std::uint8_t>(notes[i]) : 0);
  put_u8(v, r.xf.in_count); put_u8(v, r.xf.out_count);
  put_u8(v, static_cast<std::uint8_t>(r.xf.curve));
  put_u8(v, static_cast<std::uint8_t>(r.filter.type));
  for (const CcRange* set : {r.xf.in, r.xf.out})
    for (unsigned i = 0; i < kMaxCcCrossfades; ++i) {
      put_u8(v, set[i].cc); put_u8(v, set[i].lo); put_u8(v, set[i].hi); put_u8(v, 0);
    }
  put_f32(v, r.filter.cutoff_hz);
  put_f32(v, r.filter.resonance_db);
}
}  // namespace

BundleWriter::BundleWriter(WriterOptions opt) : opt_(opt) {}

void BundleWriter::set_library_id(const Uuid& id) {
  library_id_ = id;
  has_library_id_ = true;
}

Error BundleWriter::set_source_id(std::string_view source_id) {
  if (source_id.empty()) return Error::kInvalidArgument;
  source_id_ = std::string(source_id);
  has_source_id_ = true;
  return Error::kNone;
}

Error BundleWriter::add_region(std::size_t instrument, const Region& region) {
  if (instrument >= instruments_.size()) return Error::kInvalidArgument;
  auto& ins = instruments_[instrument];
  if (region.sample >= ins.samples.size()) return Error::kInvalidArgument;
  if (ins.regions.size() >= (1u << 24)) return Error::kInvalidArgument;
  if (validate_region(region, ins.samples[region.sample].frames) != Error::kNone)
    return Error::kInvalidArgument;
  ins.regions.push_back(region);
  return Error::kNone;
}

Result<std::size_t> BundleWriter::add_instrument(std::string_view name,
                                                 std::string_view relative_path) {
  if (name.size() > kMaxStringLen) return Error::kInvalidArgument;
  auto key = normalize_instrument_key(relative_path);
  if (!key) return key.error();
  for (const auto& i : instruments_)
    if (i.key == key.value()) return Error::kDuplicateKey;
  InstrumentRec r;
  r.name = std::string(name);
  r.key = key.take();
  instruments_.push_back(std::move(r));
  return instruments_.size() - 1;
}

Error BundleWriter::add_sample(std::size_t instrument, const SampleInput& in) {
  if (instrument >= instruments_.size()) return Error::kInvalidArgument;
  if (in.name.size() > kMaxStringLen) return Error::kInvalidArgument;
  if (!is_supported(in.format)) return Error::kUnsupportedFormat;
  const std::size_t ch = in.format.channels;
  if (in.frame_count > (std::uint64_t(1) << 40)) return Error::kInvalidArgument;
  if (in.data.size() != in.frame_count * ch) return Error::kInvalidArgument;
  if (!in.format.is_float) {
    const std::int32_t lim = std::int32_t(1) << (in.format.bits_per_sample - 1);
    for (std::int32_t v : in.data)
      if (v < -lim || v >= lim) return Error::kInvalidArgument;
  }

  const std::size_t blocks_before = blocks_.size();
  const std::size_t data_before = block_data_.size();
  SampleRec rec;
  rec.name = in.name;
  rec.format = in.format;
  rec.frames = in.frame_count;
  rec.first_block = blocks_before;
  rec.block_count = block_count_for(in.frame_count);

  Sha256 pcm;
  pcm_hash_update(pcm, in.format, in.data.data(), in.data.size());
  rec.pcm_sha256 = pcm.finish();

  for (std::uint32_t b = 0; b < rec.block_count; ++b) {
    const std::uint64_t start = std::uint64_t(b) * kBlockFrames;
    const std::uint32_t frames =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(kBlockFrames, in.frame_count - start));
    const std::int32_t* src = in.data.data() + start * ch;
    BlockRec br;
    br.data_offset = block_data_.size();
    br.frames = frames;
    if (in.format.is_float) {
      for (std::size_t i = 0; i < std::size_t(frames) * ch; ++i)
        put_u32(block_data_, static_cast<std::uint32_t>(src[i]));
    } else {
      const Error e = flac_encode_block(in.format, src, frames, block_data_);
      if (e != Error::kNone) {
        blocks_.resize(blocks_before);
        block_data_.resize(data_before);
        return e;
      }
    }
    br.size = static_cast<std::uint32_t>(block_data_.size() - br.data_offset);
    br.sha256 = Sha256::hash({block_data_.data() + br.data_offset, br.size});
    blocks_.push_back(br);
  }
  instruments_[instrument].samples.push_back(std::move(rec));
  return Error::kNone;
}

Result<std::vector<std::uint8_t>> BundleWriter::finalize() const {
  if (opt_.format_version < kMinReadableVersion || opt_.format_version > kFormatVersion + 1)
    return Error::kInvalidArgument;  // only the documented test hook range is producible

  // Fingerprint of the source content (used for library_id unless vendor-assigned).
  if (opt_.format_version < 2)
    for (const auto& ins : instruments_)
      if (!ins.regions.empty()) return Error::kInvalidArgument;  // v1 has no region table
  // Minor 1 only for bundles that need the extension table (everything else stays byte-identical
  // to the Epic 1 output).
  bool needs_ext = false;
  for (const auto& ins : instruments_)
    for (const auto& r : ins.regions) needs_ext = needs_ext || has_extension(r);
  if (needs_ext && opt_.format_minor < 1) return Error::kInvalidArgument;
  if (opt_.format_minor > kFormatMinor) return Error::kInvalidArgument;
  const std::uint32_t version_word = opt_.format_version | ((needs_ext ? 1u : 0u) << kMinorShift);
  Uuid library_id = library_id_;
  if (!has_library_id_ && has_source_id_) {
    library_id = library_id_from_source(source_id_);
  } else if (!has_library_id_) {
    std::vector<std::uint8_t> fp;
    put_u32(fp, static_cast<std::uint32_t>(instruments_.size()));
    for (const auto& ins : instruments_) {
      put_str(fp, ins.key);
      put_u32(fp, static_cast<std::uint32_t>(ins.samples.size()));
      for (const auto& s : ins.samples) {
        put_u8(fp, s.format.channels);
        put_u8(fp, s.format.bits_per_sample);
        put_u8(fp, s.format.is_float ? 1 : 0);
        put_u32(fp, s.format.sample_rate);
        put_u64(fp, s.frames);
        fp.insert(fp.end(), s.pcm_sha256.begin(), s.pcm_sha256.end());
      }
    }
    const Digest256 d = Sha256::hash(fp);
    library_id = library_id_from_fingerprint(d);
  }

  // Flatten blocks in instrument-major sample order.
  std::vector<BlockRec> ordered;
  std::vector<std::uint8_t> data;
  ordered.reserve(blocks_.size());
  data.reserve(block_data_.size());
  std::vector<std::uint64_t> first_block;  // per flat sample
  for (const auto& ins : instruments_)
    for (const auto& s : ins.samples) {
      first_block.push_back(ordered.size());
      for (std::uint32_t b = 0; b < s.block_count; ++b) {
        BlockRec br = blocks_[s.first_block + b];
        const std::uint64_t off = data.size();
        data.insert(data.end(), block_data_.begin() + br.data_offset,
                    block_data_.begin() + br.data_offset + br.size);
        br.data_offset = off;
        ordered.push_back(br);
      }
    }

  std::vector<std::uint8_t> manifest;
  put_u32(manifest, version_word);
  manifest.insert(manifest.end(), library_id.begin(), library_id.end());
  std::uint32_t total_samples = 0;
  for (const auto& ins : instruments_) total_samples += static_cast<std::uint32_t>(ins.samples.size());
  put_u32(manifest, static_cast<std::uint32_t>(instruments_.size()));
  put_u32(manifest, total_samples);
  put_u64(manifest, ordered.size());
  const std::size_t index_hash_pos = manifest.size();
  manifest.insert(manifest.end(), 32, 0);  // patched below
  std::size_t flat = 0;
  for (const auto& ins : instruments_) {
    put_str(manifest, ins.key);
    put_str(manifest, ins.name);
    put_u32(manifest, static_cast<std::uint32_t>(ins.samples.size()));
    for (const auto& s : ins.samples) {
      put_str(manifest, s.name);
      put_u8(manifest, s.format.channels);
      put_u8(manifest, s.format.bits_per_sample);
      put_u8(manifest, s.format.is_float ? 1 : 0);
      put_u8(manifest, 0);
      put_u32(manifest, s.format.sample_rate);
      put_u64(manifest, s.frames);
      put_u32(manifest, kBlockFrames);
      put_u32(manifest, s.block_count);
      put_u64(manifest, first_block[flat++]);
      manifest.insert(manifest.end(), s.pcm_sha256.begin(), s.pcm_sha256.end());
    }
    if (opt_.format_version >= 2) {
      put_u32(manifest, static_cast<std::uint32_t>(ins.regions.size()));
      for (const auto& r : ins.regions) put_region(manifest, r);
      if (needs_ext) {
        std::uint32_t n_ext = 0;
        for (const auto& r : ins.regions) n_ext += has_extension(r) ? 1u : 0u;
        put_u32(manifest, n_ext);
        for (std::size_t k = 0; k < ins.regions.size(); ++k)
          if (has_extension(ins.regions[k])) put_ext(manifest, static_cast<std::uint32_t>(k), ins.regions[k]);
      }
    }
  }

  const std::uint64_t manifest_off = kHeaderSize;
  const std::uint64_t index_off = manifest_off + manifest.size();
  const std::uint64_t index_size = ordered.size() * kIndexEntrySize;
  const std::uint64_t data_off = index_off + index_size;
  std::vector<std::uint8_t> index;
  index.reserve(index_size);
  for (const auto& b : ordered) {
    put_u64(index, data_off + b.data_offset);
    put_u32(index, b.size);
    put_u32(index, b.frames);
    index.insert(index.end(), b.sha256.begin(), b.sha256.end());
  }
  const Digest256 index_hash = Sha256::hash(index);
  std::copy(index_hash.begin(), index_hash.end(), manifest.begin() + index_hash_pos);
  const Digest256 manifest_hash = Sha256::hash(manifest);

  std::vector<std::uint8_t> out;
  out.reserve(data_off + data.size());
  out.insert(out.end(), kMagic, kMagic + 4);
  put_u32(out, version_word);
  put_u64(out, manifest_off);
  put_u64(out, manifest.size());
  put_u64(out, index_off);
  put_u64(out, index_size);
  put_u64(out, data_off);
  put_u64(out, data.size());
  put_u64(out, data_off + data.size());
  out.insert(out.end(), manifest_hash.begin(), manifest_hash.end());
  out.insert(out.end(), index_hash.begin(), index_hash.end());
  out.insert(out.end(), manifest.begin(), manifest.end());
  out.insert(out.end(), index.begin(), index.end());
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

Error BundleWriter::finalize_to_file(const std::string& path) const {
  auto bytes = finalize();
  if (!bytes) return bytes.error();
  const std::string tmp = path + ".tmp";
  std::FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return Error::kIo;
  bool ok = std::fwrite(bytes.value().data(), 1, bytes.value().size(), f) == bytes.value().size();
  ok = std::fflush(f) == 0 && ok;
  ok = ::fsync(::fileno(f)) == 0 && ok;
  ok = std::fclose(f) == 0 && ok;
  if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    return Error::kIo;
  }
  return Error::kNone;
}

}  // namespace velora::format
