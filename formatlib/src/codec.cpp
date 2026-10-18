#include "codec.hpp"

#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>

#include <cstring>
#include <memory>

namespace velora::format {

struct BlockDecoder::Impl {
  FLAC__StreamDecoder* dec = nullptr;
  std::vector<std::uint8_t> scratch;
  std::uint64_t count = 0;
  ~Impl() {
    if (dec) FLAC__stream_decoder_delete(dec);
  }
};

BlockDecoder::BlockDecoder() : impl_(std::make_unique<Impl>()) {}
BlockDecoder::~BlockDecoder() = default;
BlockDecoder::BlockDecoder(BlockDecoder&&) noexcept = default;
BlockDecoder& BlockDecoder::operator=(BlockDecoder&&) noexcept = default;
std::uint64_t BlockDecoder::blocks_decoded() const { return impl_ ? impl_->count : 0; }

std::vector<std::uint8_t>& scratch_of(BlockDecoder::Impl& ctx) { return ctx.scratch; }
void count_block(BlockDecoder::Impl& ctx) { ++ctx.count; }

namespace {

struct EncCtx {
  std::vector<std::uint8_t>* out;
};

FLAC__StreamEncoderWriteStatus enc_write(const FLAC__StreamEncoder*, const FLAC__byte buf[],
                                         size_t bytes, unsigned, unsigned, void* client) {
  auto* c = static_cast<EncCtx*>(client);
  c->out->insert(c->out->end(), buf, buf + bytes);
  return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

struct EncDeleter {
  void operator()(FLAC__StreamEncoder* e) const { FLAC__stream_encoder_delete(e); }
};

struct DecCtx {
  const std::uint8_t* p = nullptr;
  std::size_t n = 0, pos = 0;
  std::int32_t* out = nullptr;
  std::uint32_t expected_frames = 0;
  unsigned channels = 0, bits = 0;
  std::uint32_t got_frames = 0;
  unsigned frames_seen = 0;
  bool err = false;
};

FLAC__StreamDecoderReadStatus dec_read(const FLAC__StreamDecoder*, FLAC__byte buf[], size_t* bytes,
                                       void* client) {
  auto* c = static_cast<DecCtx*>(client);
  if (c->pos >= c->n) {
    *bytes = 0;
    return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
  }
  const std::size_t take = (c->n - c->pos < *bytes) ? (c->n - c->pos) : *bytes;
  std::memcpy(buf, c->p + c->pos, take);
  c->pos += take;
  *bytes = take;
  return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

FLAC__StreamDecoderWriteStatus dec_write(const FLAC__StreamDecoder*, const FLAC__Frame* frame,
                                         const FLAC__int32* const buffer[], void* client) {
  auto* c = static_cast<DecCtx*>(client);
  const std::uint32_t bs = frame->header.blocksize;
  if (c->frames_seen++ != 0 || frame->header.channels != c->channels ||
      frame->header.bits_per_sample != c->bits || bs != c->expected_frames) {
    c->err = true;
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  for (std::uint32_t i = 0; i < bs; ++i)
    for (unsigned ch = 0; ch < c->channels; ++ch)
      c->out[std::size_t(i) * c->channels + ch] = buffer[ch][i];
  c->got_frames = bs;
  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void dec_error(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus, void* client) {
  static_cast<DecCtx*>(client)->err = true;
}

// Keeps "fLaC" + STREAMINFO only: drops VORBIS_COMMENT (vendor string differs between libFLAC
// versions), padding and anything else, so the metadata part is version independent.
bool strip_metadata(std::vector<std::uint8_t>& v, std::size_t begin) {
  const std::size_t n = v.size() - begin;
  if (n < 4 + 4 + 34 || std::memcmp(v.data() + begin, "fLaC", 4) != 0) return false;
  std::size_t pos = begin + 4;
  std::uint8_t first[4 + 34];
  if ((v[pos] & 0x7f) != 0 || v[pos + 1] != 0 || v[pos + 2] != 0 || v[pos + 3] != 34) return false;
  std::memcpy(first, v.data() + pos, sizeof first);
  first[0] |= 0x80;  // STREAMINFO becomes the last metadata block
  bool last = (v[pos] & 0x80) != 0;
  pos += 4 + 34;
  while (!last) {
    if (pos + 4 > v.size()) return false;
    last = (v[pos] & 0x80) != 0;
    const std::size_t len = (std::size_t(v[pos + 1]) << 16) | (std::size_t(v[pos + 2]) << 8) | v[pos + 3];
    if (pos + 4 + len > v.size()) return false;
    pos += 4 + len;
  }
  std::vector<std::uint8_t> frames(v.begin() + pos, v.end());
  v.resize(begin + 4);
  v.insert(v.end(), first, first + sizeof first);
  v.insert(v.end(), frames.begin(), frames.end());
  return true;
}

}  // namespace

Error flac_encode_block(const PcmFormat& f, const std::int32_t* interleaved, std::uint32_t frames,
                        std::vector<std::uint8_t>& out) {
  std::unique_ptr<FLAC__StreamEncoder, EncDeleter> enc(FLAC__stream_encoder_new());
  if (!enc) return Error::kEncoderFailure;
  FLAC__stream_encoder_set_channels(enc.get(), f.channels);
  FLAC__stream_encoder_set_bits_per_sample(enc.get(), f.bits_per_sample);
  FLAC__stream_encoder_set_sample_rate(enc.get(), f.sample_rate);
  FLAC__stream_encoder_set_compression_level(enc.get(), 8);  // before blocksize: level sets it
  FLAC__stream_encoder_set_blocksize(enc.get(), kBlockFrames);
  FLAC__stream_encoder_set_streamable_subset(enc.get(), false);
  FLAC__stream_encoder_set_verify(enc.get(), true);   // encoder-side bit-exactness check
  FLAC__stream_encoder_set_total_samples_estimate(enc.get(), frames);
  const std::size_t before = out.size();
  EncCtx ctx{&out};
  if (FLAC__stream_encoder_init_stream(enc.get(), enc_write, nullptr, nullptr, nullptr, &ctx) !=
      FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
    out.resize(before);
    return Error::kEncoderFailure;
  }
  bool ok = FLAC__stream_encoder_process_interleaved(enc.get(), interleaved, frames);
  ok = FLAC__stream_encoder_finish(enc.get()) && ok;  // false also on verify mismatch
  if (!ok || !strip_metadata(out, before)) {
    out.resize(before);
    return Error::kEncoderFailure;
  }
  return Error::kNone;
}

Error flac_decode_block(BlockDecoder::Impl& impl, const std::uint8_t* data, std::size_t size,
                        const PcmFormat& f, std::uint32_t frames, std::int32_t* out) {
  if (!impl.dec) {
    impl.dec = FLAC__stream_decoder_new();
    if (!impl.dec) return Error::kIo;
  }
  DecCtx ctx;
  ctx.p = data;
  ctx.n = size;
  ctx.out = out;
  ctx.expected_frames = frames;
  ctx.channels = f.channels;
  ctx.bits = f.bits_per_sample;
  if (FLAC__stream_decoder_init_stream(impl.dec, dec_read, nullptr, nullptr, nullptr, nullptr,
                                       dec_write, nullptr, dec_error,
                                       &ctx) != FLAC__STREAM_DECODER_INIT_STATUS_OK)
    return Error::kCorruptBlock;
  const bool ok = FLAC__stream_decoder_process_until_end_of_stream(impl.dec);
  // finish() returns false when MD5 checking is on and fails; it is off here.
  FLAC__stream_decoder_finish(impl.dec);
  if (!ok || ctx.err || ctx.frames_seen != 1 || ctx.got_frames != frames)
    return Error::kCorruptBlock;
  return Error::kNone;
}

Error flac_decode_standalone(const std::uint8_t* data, std::size_t size, const PcmFormat& f,
                             std::uint32_t frames, std::int32_t* out) {
  BlockDecoder::Impl impl;
  return flac_decode_block(impl, data, size, f, frames, out);
}

}  // namespace velora::format
