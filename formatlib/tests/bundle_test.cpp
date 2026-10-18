#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "codec.hpp"
#include "layout.hpp"
#include "test_util.hpp"
#include "velora/format/bundle.hpp"

using namespace velora::format;
using vt::Kind;

namespace {

using Bytes = std::vector<std::uint8_t>;

struct Fixture {
  PcmFormat fmt;
  std::vector<std::int32_t> pcm;
};

Bytes build_single(const PcmFormat& f, const std::vector<std::int32_t>& pcm,
                   WriterOptions opt = {}) {
  BundleWriter w(opt);
  auto i = w.add_instrument("Inst", "a/b.sfz");
  CHECK(i.ok());
  CHECK_ERR(w.add_sample(i.value(), {"s", f, pcm.size() / f.channels, pcm}), Error::kNone);
  auto b = w.finalize();
  CHECK(b.ok());
  return b.ok() ? b.take() : Bytes{};
}

// Decodes everything block by block and through read_frames; true when bit-identical.
bool matches_source(const BundleReader& r, std::size_t sample, const std::vector<std::int32_t>& pcm) {
  const SampleInfo& s = r.samples()[sample];
  const std::size_t ch = s.format.channels;
  if (s.frame_count * ch != pcm.size()) return false;
  std::vector<std::int32_t> all(pcm.size());
  if (r.read_frames(sample, 0, s.frame_count, all) != Error::kNone) return false;
  if (all != pcm) return false;
  std::vector<std::int32_t> blk(kBlockFrames * ch);
  BlockDecoder dec;
  for (std::uint64_t b = 0; b < s.block_count; ++b) {
    auto n = r.read_block(sample, b, blk, dec);
    if (!n) return false;
    if (std::memcmp(blk.data(), pcm.data() + b * kBlockFrames * ch,
                    std::size_t(n.value()) * ch * 4) != 0)
      return false;
  }
  return r.verify_all() == Error::kNone;
}

void test_roundtrip_all_formats() {
  for (unsigned bits : {16u, 24u})
    for (unsigned ch : {1u, 2u})
      for (unsigned rate : {44100u, 48000u, 88200u, 96000u})
        for (Kind k : {Kind::kSine, Kind::kMixed}) {
          PcmFormat f{static_cast<std::uint8_t>(ch), static_cast<std::uint8_t>(bits), false, rate};
          const auto pcm = vt::make_int(f, 10000, k);  // 2 full blocks + a short one
          const Bytes b = build_single(f, pcm);
          auto r = BundleReader::open_memory(b);
          CHECK(r.ok());
          if (!r.ok()) continue;
          CHECK(r.value().samples()[0].format == f);
          CHECK(matches_source(r.value(), 0, pcm));
        }
}

void test_silence_noise_lengths() {
  const PcmFormat f{2, 16, false, 48000};
  {  // silence round-trips and compresses well
    const auto pcm = vt::make_int(f, 3 * kBlockFrames, Kind::kSilence);
    const Bytes b = build_single(f, pcm);
    CHECK(b.size() < pcm.size() * 2 / 10);
    auto r = BundleReader::open_memory(b);
    CHECK(r.ok() && matches_source(r.value(), 0, pcm));
  }
  for (unsigned bits : {16u, 24u})
    for (unsigned ch : {1u, 2u}) {  // incompressible
      PcmFormat nf{static_cast<std::uint8_t>(ch), static_cast<std::uint8_t>(bits), false, 96000};
      const auto pcm = vt::make_int(nf, 9000, Kind::kNoise);
      const Bytes b = build_single(nf, pcm);
      auto r = BundleReader::open_memory(b);
      CHECK(r.ok() && matches_source(r.value(), 0, pcm));
    }
  for (std::uint64_t n : {0ull, 1ull, 2ull, 15ull, 17ull, 4095ull, 4096ull, 4097ull, 8192ull, 8193ull}) {
    for (Kind k : {Kind::kSine, Kind::kNoise}) {
      const auto pcm = vt::make_int(f, n, k);
      const Bytes b = build_single(f, pcm);
      auto r = BundleReader::open_memory(b);
      CHECK(r.ok());
      if (!r.ok()) continue;
      CHECK(r.value().samples()[0].frame_count == n);
      CHECK(r.value().samples()[0].block_count == (n + kBlockFrames - 1) / kBlockFrames);
      CHECK(matches_source(r.value(), 0, pcm));
    }
  }
}

void test_seek() {
  const PcmFormat f{2, 24, false, 44100};
  const auto pcm = vt::make_int(f, 20000, Kind::kMixed);
  const Bytes b = build_single(f, pcm);
  auto r = BundleReader::open_memory(b);
  CHECK(r.ok());
  const BundleReader& rd = r.value();
  BlockDecoder dec;
  std::vector<std::int32_t> blk(kBlockFrames * 2);
  const std::uint64_t k = 9876;
  const std::uint64_t before = dec.blocks_decoded();
  auto n = rd.read_block(0, k / kBlockFrames, blk, dec);
  CHECK(n.ok());
  CHECK(dec.blocks_decoded() == before + 1);  // exactly one block decoded
  const std::uint64_t off = k % kBlockFrames;
  CHECK(off < n.value());
  CHECK(blk[off * 2] == pcm[k * 2] && blk[off * 2 + 1] == pcm[k * 2 + 1]);
  // Arbitrary range crossing a block boundary.
  std::vector<std::int32_t> out(300 * 2);
  CHECK_ERR(rd.read_frames(0, 4000, 300, out), Error::kNone);
  CHECK(std::equal(out.begin(), out.end(), pcm.begin() + 4000 * 2));
  // Last short block is the right size.
  CHECK(rd.read_block(0, 4, blk, dec).value() == 20000 - 4 * kBlockFrames);
  // Errors.
  CHECK_ERR(rd.read_block(0, 5, blk, dec).error(), Error::kOutOfRange);
  CHECK_ERR(rd.read_block(1, 0, blk, dec).error(), Error::kOutOfRange);
  CHECK_ERR(rd.read_block(0, 0, std::span<std::int32_t>(blk.data(), 10), dec).error(),
            Error::kBufferTooSmall);
  CHECK_ERR(rd.read_frames(0, 19999, 2, out), Error::kOutOfRange);
  CHECK_ERR(rd.read_frames(0, 20001, 0, out), Error::kOutOfRange);
  CHECK_ERR(rd.read_frames(0, 20000, 0, out), Error::kNone);
  CHECK_ERR(rd.read_frames(0, 0, 300, std::span<std::int32_t>(out.data(), 10)), Error::kBufferTooSmall);
}

bool contains(const Bytes& hay, const Bytes& needle) {
  return std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end();
}

void test_float() {
  for (unsigned ch : {1u, 2u})
    for (unsigned rate : {44100u, 96000u}) {
      const PcmFormat f{static_cast<std::uint8_t>(ch), 32, true, rate};
      const auto pcm = vt::make_float(ch, 5000);
      const Bytes b = build_single(f, pcm);
      auto r = BundleReader::open_memory(b);
      CHECK(r.ok() && matches_source(r.value(), 0, pcm));
      // Stored uncompressed: the raw little-endian bytes of the first block appear verbatim.
      Bytes raw;
      for (int i = 0; i < 64; ++i)
        layout::put_u32(raw, static_cast<std::uint32_t>(pcm[i]));
      CHECK(contains(b, raw));
      CHECK(b.size() > pcm.size() * 4);  // no compression
    }
  // Specific bit patterns survive (NaN payload, -0.0).
  const PcmFormat f{1, 32, true, 48000};
  const std::vector<std::int32_t> pcm = {std::int32_t(0x7fc00001), std::int32_t(0x80000000), 0,
                                         std::int32_t(0xffc12345)};
  auto bytes = build_single(f, pcm);
  auto r = BundleReader::open_memory(bytes);
  std::vector<std::int32_t> out(4);
  CHECK(r.ok() && r.value().read_frames(0, 0, 4, out) == Error::kNone && out == pcm);
}

void test_unsupported() {
  BundleWriter w;
  const std::size_t i = w.add_instrument("x", "x.sfz").value();
  const std::vector<std::int32_t> d(100, 0);
  auto try_fmt = [&](PcmFormat f) { return w.add_sample(i, {"s", f, std::uint64_t(f.channels ? 100 / f.channels : 100), d}); };
  CHECK_ERR(try_fmt({1, 8, false, 48000}), Error::kUnsupportedFormat);
  CHECK_ERR(try_fmt({5, 16, false, 48000}), Error::kUnsupportedFormat);
  CHECK_ERR(try_fmt({0, 16, false, 48000}), Error::kUnsupportedFormat);
  CHECK_ERR(try_fmt({2, 16, false, 11025}), Error::kUnsupportedFormat);
  CHECK_ERR(try_fmt({2, 32, false, 48000}), Error::kUnsupportedFormat);
  CHECK_ERR(try_fmt({2, 16, true, 48000}), Error::kUnsupportedFormat);
  CHECK_ERR(try_fmt({2, 64, true, 48000}), Error::kUnsupportedFormat);
  // Other argument errors.
  CHECK_ERR(w.add_sample(i, {"s", {2, 16, false, 48000}, 51, d}), Error::kInvalidArgument);
  CHECK_ERR(w.add_sample(7, {"s", {2, 16, false, 48000}, 50, d}), Error::kInvalidArgument);
  const std::vector<std::int32_t> big = {40000, 0};
  CHECK_ERR(w.add_sample(i, {"s", {2, 16, false, 48000}, 1, big}), Error::kInvalidArgument);
  const std::vector<std::int32_t> edge = {32767, -32768};
  CHECK_ERR(w.add_sample(i, {"s", {2, 16, false, 48000}, 1, edge}), Error::kNone);
  // A failed add leaves no partial state: the bundle still opens and verifies.
  auto b = w.finalize();
  CHECK(b.ok());
  auto r = BundleReader::open_memory(b.value());
  CHECK(r.ok() && r.value().samples().size() == 1 && r.value().verify_all() == Error::kNone);
}

Bytes small_bundle() {
  BundleWriter w;
  const std::size_t a = w.add_instrument("A", "dir/a.sfz").value();
  const std::size_t b = w.add_instrument("B", "b.sfz").value();
  const PcmFormat f{1, 16, false, 44100};
  const PcmFormat g{2, 32, true, 48000};
  CHECK_ERR(w.add_sample(a, {"n", f, 300, vt::make_int(f, 300, Kind::kNoise)}), Error::kNone);
  CHECK_ERR(w.add_sample(b, {"fl", g, 40, vt::make_float(2, 40)}), Error::kNone);
  CHECK_ERR(w.add_sample(a, {"m", f, 5000, vt::make_int(f, 5000, Kind::kSine)}), Error::kNone);
  return w.finalize().take();
}

void test_truncation_and_corruption_sweeps() {
  const Bytes good = small_bundle();
  {
    auto r = BundleReader::open_memory(good);
    CHECK(r.ok() && r.value().verify_all() == Error::kNone);
    // Instrument-major sample order regardless of add order.
    CHECK(r.ok() && r.value().instruments()[0].sample_count == 2 &&
          r.value().samples()[1].name == "m" && r.value().samples()[2].name == "fl");
  }
  // Truncation at every byte offset: typed error, never a crash.
  for (std::size_t n = 0; n < good.size(); ++n) {
    auto r = BundleReader::open_memory(std::span<const std::uint8_t>(good.data(), n));
    CHECK(!r.ok());
    if (r.ok()) break;
    CHECK(r.error() == Error::kTruncated);
  }
  // Single-bit flips at every byte: always detected by open or verify_all; never a crash.
  for (std::size_t i = 0; i < good.size(); ++i)
    for (int bit : {0, 7}) {
      Bytes bad = good;
      bad[i] ^= std::uint8_t(1u << bit);
      auto r = BundleReader::open_memory(bad);
      if (r.ok()) {
        CHECK(r.value().verify_all() != Error::kNone);
        if (r.value().verify_all() == Error::kNone) std::printf("  undetected flip at %zu\n", i);
      }
    }
  // Same sweep with block-hash checking off: any flip that still opens must either fail
  // verify_all (which always checks block and PCM hashes) or decode equal to the source.
  auto decode_all = [](const BundleReader& r) {
    std::vector<std::vector<std::int32_t>> v;
    for (std::size_t si = 0; si < r.samples().size(); ++si) {
      const auto& s = r.samples()[si];
      v.emplace_back(s.frame_count * s.format.channels);
      CHECK_ERR(r.read_frames(si, 0, s.frame_count, v.back()), Error::kNone);
    }
    return v;
  };
  const auto good_pcm = decode_all(BundleReader::open_memory(good).value());
  for (std::size_t i = 0; i < good.size(); ++i) {
    Bytes bad = good;
    bad[i] ^= 0x10;
    auto r = BundleReader::open_memory(bad, ReaderOptions{false});
    if (!r.ok()) continue;
    if (r.value().verify_all() != Error::kNone) continue;
    CHECK(r.value().samples().size() == good_pcm.size());
    std::vector<std::vector<std::int32_t>> got;
    for (std::size_t si = 0; si < r.value().samples().size(); ++si) {
      const auto& s = r.value().samples()[si];
      got.emplace_back(s.frame_count * s.format.channels);
      if (r.value().read_frames(si, 0, s.frame_count, got.back()) != Error::kNone) break;
    }
    CHECK(got == good_pcm);
  }
}

std::uint64_t rd64(const Bytes& b, std::size_t o) { return layout::get_u64(b.data() + o); }
void wr32(Bytes& b, std::size_t o, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) b[o + i] = std::uint8_t(v >> (8 * i));
}
void wr64(Bytes& b, std::size_t o, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) b[o + i] = std::uint8_t(v >> (8 * i));
}
// Recomputes index/manifest hashes so that structural checks (not hashes) are what fail.
void rehash(Bytes& b) {
  const std::size_t mo = rd64(b, 8), ms = rd64(b, 16), io = rd64(b, 24), is = rd64(b, 32);
  const Digest256 ih = Sha256::hash({b.data() + io, is});
  std::copy(ih.begin(), ih.end(), b.begin() + 96);
  std::copy(ih.begin(), ih.end(), b.begin() + mo + 36);
  const Digest256 mh = Sha256::hash({b.data() + mo, ms});
  std::copy(mh.begin(), mh.end(), b.begin() + 64);
}

void test_structural_corruption() {
  const Bytes good = small_bundle();
  const std::size_t io = rd64(good, 24), is = rd64(good, 32), data_off = rd64(good, 40);
  CHECK(is / 48 >= 3);
  auto expect = [&](Bytes b, Error want, const char* what) {
    rehash(b);
    auto r = BundleReader::open_memory(b);
    if (r.ok() || r.error() != want) {
      std::printf("FAIL structural '%s': got %s want %s\n", what,
                  r.ok() ? "ok" : to_string(r.error()), to_string(want));
      ++g_fail;
    }
  };
  { Bytes b = good; wr64(b, io, good.size() + 100); expect(b, Error::kCorruptIndex, "offset past eof"); }
  { Bytes b = good; wr64(b, io, data_off - 1); expect(b, Error::kCorruptIndex, "offset before data"); }
  { Bytes b = good; wr64(b, io + 48, 0); expect(b, Error::kCorruptIndex, "non-monotonic offset"); }
  { Bytes b = good;  // swap first two entries' offsets
    const auto o0 = rd64(b, io), o1 = rd64(b, io + 48);
    wr64(b, io, o1); wr64(b, io + 48, o0); expect(b, Error::kCorruptIndex, "swapped offsets"); }
  { Bytes b = good; wr32(b, io + 8, 0xFFFFFFF0u); expect(b, Error::kCorruptIndex, "huge size"); }
  { Bytes b = good; wr32(b, io + 8, 0); expect(b, Error::kCorruptIndex, "zero size"); }
  { Bytes b = good; wr32(b, io + 12, 7); expect(b, Error::kCorruptIndex, "wrong frames"); }
  { Bytes b = good; wr32(b, 40 + 0, 0); b[40] = 0; wr64(b, 56, good.size() + 1);
    auto r = BundleReader::open_memory(b); CHECK(!r.ok() && r.error() == Error::kTruncated); }
  { Bytes b = good; b.push_back(0);
    auto r = BundleReader::open_memory(b); CHECK(!r.ok() && r.error() == Error::kCorruptHeader); }
  { Bytes b = good; b[0] = 'X';
    auto r = BundleReader::open_memory(b); CHECK(!r.ok() && r.error() == Error::kBadMagic); }
  { Bytes b = good; wr64(b, 16, rd64(good, 16) + 1);  // manifest size lies
    auto r = BundleReader::open_memory(b); CHECK(!r.ok() && r.error() == Error::kCorruptHeader); }
  { Bytes b = good; wr64(b, 8, 0);
    auto r = BundleReader::open_memory(b); CHECK(!r.ok() && r.error() == Error::kCorruptHeader); }
  {  // manifest: block_frames of the first sample set to 0 (hashes fixed up)
    Bytes b = good;
    const std::size_t mo = rd64(b, 8);
    // key "dir/a.sfz"(4+9) name "A"(4+1) count(4) sample name "n"(4+1) fmt(4) rate(4) frames(8)
    const std::size_t p = mo + 4 + 16 + 4 + 4 + 8 + 32 + (4 + 9) + (4 + 1) + 4 + (4 + 1) + 4 + 4 + 8;
    CHECK(layout::get_u32(b.data() + p) == kBlockFrames);
    wr32(b, p, 0);
    expect(b, Error::kCorruptManifest, "block_frames 0");
  }
  {  // manifest content flip without fixing the manifest hash
    Bytes b = good;
    b[rd64(good, 8) + 40] ^= 1;
    auto r = BundleReader::open_memory(b); CHECK(!r.ok() && r.error() == Error::kCorruptManifest);
  }
  {  // index flip without fixing the index hash
    Bytes b = good;
    b[io + 1] ^= 1;
    auto r = BundleReader::open_memory(b); CHECK(!r.ok() && r.error() == Error::kCorruptIndex);
  }
}

void test_corrupt_block() {
  const PcmFormat f{2, 16, false, 48000};
  const auto pcm = vt::make_int(f, 9000, Kind::kMixed);
  const Bytes good = build_single(f, pcm);
  const std::size_t data_off = rd64(good, 40);
  {  // every byte of block 1 (index entry 1)
    auto r0 = BundleReader::open_memory(good);
    CHECK(r0.ok());
    const std::size_t io = rd64(good, 24);
    const std::size_t off = rd64(good, io + 48), size = layout::get_u32(good.data() + io + 48 + 8);
    CHECK(off >= data_off);
    std::vector<std::int32_t> out(kBlockFrames * 2);
    for (std::size_t i = off; i < off + size; i += (size > 200 ? 7 : 1)) {
      Bytes bad = good;
      bad[i] ^= 0x04;
      auto r = BundleReader::open_memory(bad);
      CHECK(r.ok());
      CHECK(r.value().read_block(0, 1, out).error() == Error::kBlockHashMismatch);
      // Hash check off: the FLAC decoder's CRCs must still not yield garbage.
      auto r2 = BundleReader::open_memory(bad, ReaderOptions{false});
      auto n = r2.value().read_block(0, 1, out);
      if (n.ok())
        CHECK(std::equal(out.begin(), out.begin() + std::size_t(n.value()) * 2,
                         pcm.begin() + kBlockFrames * 2));
      // Neighbouring blocks are unaffected.
      CHECK(r.value().read_block(0, 0, out).ok());
      CHECK(r.value().read_block(0, 2, out).ok());
    }
    // Truncated FLAC payload with a re-hashed index is rejected by the decoder.
  }
}

Bytes forge_one_block(const PcmFormat& f, std::uint32_t frames, const Bytes& payload) {
  // Hand-made valid container (all hashes right) around an arbitrary block payload.
  Bytes m;
  layout::put_u32(m, kFormatVersion);
  m.insert(m.end(), 16, 0);
  layout::put_u32(m, 1); layout::put_u32(m, 1); layout::put_u64(m, 1);
  m.insert(m.end(), 32, 0);
  auto str = [&](const char* t) { layout::put_u32(m, std::uint32_t(std::strlen(t))); m.insert(m.end(), t, t + std::strlen(t)); };
  str("k.sfz"); str("i"); layout::put_u32(m, 1); str("s");
  m.push_back(f.channels); m.push_back(f.bits_per_sample); m.push_back(0); m.push_back(0);
  layout::put_u32(m, f.sample_rate); layout::put_u64(m, frames);
  layout::put_u32(m, kBlockFrames); layout::put_u32(m, 1); layout::put_u64(m, 0);
  m.insert(m.end(), 32, 0);
  const std::uint64_t io = 128 + m.size(), data_off = io + 48;
  Bytes idx;
  layout::put_u64(idx, data_off); layout::put_u32(idx, std::uint32_t(payload.size()));
  layout::put_u32(idx, frames);
  const Digest256 bh = Sha256::hash(payload);
  idx.insert(idx.end(), bh.begin(), bh.end());
  Bytes out(kBlockFrames, 0);
  out.clear();
  out.insert(out.end(), {'V', 'L', 'R', 'B'});
  layout::put_u32(out, kFormatVersion);
  layout::put_u64(out, 128); layout::put_u64(out, m.size()); layout::put_u64(out, io);
  layout::put_u64(out, 48); layout::put_u64(out, data_off); layout::put_u64(out, payload.size());
  layout::put_u64(out, data_off + payload.size());
  out.insert(out.end(), 64, 0);
  out.insert(out.end(), m.begin(), m.end());
  out.insert(out.end(), idx.begin(), idx.end());
  out.insert(out.end(), payload.begin(), payload.end());
  rehash(out);
  return out;
}

void test_decoder_only_corruption() {
  const PcmFormat f{2, 16, false, 48000};
  const auto pcm = vt::make_int(f, 1000, Kind::kMixed);
  Bytes enc;
  CHECK_ERR(flac_encode_block(f, pcm.data(), 1000, enc), Error::kNone);
  CHECK(enc.size() > 60 && std::memcmp(enc.data(), "fLaC", 4) == 0);
  // Only STREAMINFO is emitted: the first metadata block is flagged last and 34 bytes long.
  CHECK(enc[4] == 0x80 && enc[7] == 34);
  std::vector<std::int32_t> out(1000 * 2);
  auto read_forged = [&](const PcmFormat& rf, std::uint32_t frames, const Bytes& payload) {
    const Bytes b = forge_one_block(rf, frames, payload);
    auto r = BundleReader::open_memory(b);
    if (!r.ok()) return r.error();
    return r.value().read_block(0, 0, out).error();
  };
  // Sanity: the forged container with the real payload decodes.
  { const Bytes b = forge_one_block(f, 1000, enc);
    auto r = BundleReader::open_memory(b);
    CHECK(r.ok() && r.value().read_block(0, 0, out).ok() && out == pcm); }
  Bytes cut(enc.begin(), enc.end() - 20);
  CHECK_ERR(read_forged(f, 1000, cut), Error::kCorruptBlock);           // truncated payload
  Bytes tail = enc; tail[tail.size() - 3] ^= 0x55;
  CHECK_ERR(read_forged(f, 1000, tail), Error::kCorruptBlock);          // frame CRC
  Bytes two = enc;                                                       // two frames in a block
  two.insert(two.end(), enc.begin() + 42, enc.end());
  CHECK_ERR(read_forged(f, 1000, two), Error::kCorruptBlock);
  CHECK_ERR(read_forged({1, 16, false, 48000}, 1000, enc), Error::kCorruptBlock);   // channels
  CHECK_ERR(read_forged({2, 24, false, 48000}, 1000, enc), Error::kCorruptBlock);   // bit depth
  CHECK_ERR(read_forged(f, 999, enc), Error::kCorruptBlock);            // frame count
  CHECK_ERR(read_forged(f, 1000, Bytes{'f', 'L', 'a', 'C'}), Error::kCorruptBlock);
  CHECK_ERR(read_forged(f, 1000, Bytes(100, 0xAA)), Error::kCorruptBlock);
  // Same through the standalone helper.
  CHECK_ERR(flac_decode_standalone(enc.data(), enc.size(), f, 1000, out.data()), Error::kNone);
  CHECK_ERR(flac_decode_standalone(cut.data(), cut.size(), f, 1000, out.data()), Error::kCorruptBlock);
}

void test_hash_policy_and_structure() {
  // PCM differs but block/index/manifest hashes are valid: only the PCM hash catches it.
  const PcmFormat f{1, 32, true, 48000};
  const auto pcm = vt::make_float(1, 100);
  Bytes b = build_single(f, pcm);
  const std::size_t io = rd64(b, 24), data_off = rd64(b, 40);
  b[data_off + 1] ^= 0x01;
  { // block hash left stale: verify_all must flag it even with block hashing off
    auto r = BundleReader::open_memory(b, ReaderOptions{false});
    CHECK(r.ok() && r.value().verify_all() == Error::kBlockHashMismatch);
    auto r2 = BundleReader::open_memory(b);
    CHECK(r2.ok() && r2.value().verify_all() == Error::kBlockHashMismatch);
  }
  const Digest256 nh = Sha256::hash({b.data() + data_off, std::size_t(layout::get_u32(b.data() + io + 8))});
  std::copy(nh.begin(), nh.end(), b.begin() + io + 16);
  rehash(b);
  for (bool vh : {false, true}) {
    auto r = BundleReader::open_memory(b, ReaderOptions{vh});
    CHECK(r.ok() && r.value().verify_all() == Error::kPcmHashMismatch);
  }
  // Duplicate instrument_key in the manifest (hashes recomputed).
  {
    BundleWriter w;
    CHECK(w.add_instrument("n1", "k1.sfz").ok());
    CHECK(w.add_instrument("n2", "k2.sfz").ok());
    Bytes d = w.finalize().take();
    const std::string k2 = "k2.sfz";
    auto it = std::search(d.begin(), d.end(), k2.begin(), k2.end());
    CHECK(it != d.end());
    std::copy_n("k1.sfz", 6, it);
    rehash(d);
    auto r = BundleReader::open_memory(d);
    CHECK(!r.ok() && r.error() == Error::kCorruptManifest);
  }
  // Oversized index entry for a FLAC block (file extended, hashes fixed): kCorruptIndex.
  {
    const PcmFormat g{1, 16, false, 48000};
    Bytes d = build_single(g, vt::make_int(g, 100, Kind::kSine));
    const std::size_t dio = rd64(d, 24);
    wr32(d, dio + 8, layout::get_u32(d.data() + dio + 8) + 5000);
    d.insert(d.end(), 5000, 0);
    wr64(d, 48, rd64(d, 48) + 5000);
    wr64(d, 56, d.size());
    rehash(d);
    auto r = BundleReader::open_memory(d);
    CHECK(!r.ok() && r.error() == Error::kCorruptIndex);
  }
  // Moved-from BlockDecoder is rejected, not dereferenced.
  {
    const PcmFormat g{1, 16, false, 48000};
    const Bytes d = build_single(g, vt::make_int(g, 100, Kind::kSine));
    auto r = BundleReader::open_memory(d);
    BlockDecoder d1;
    BlockDecoder d2(std::move(d1));
    std::vector<std::int32_t> out(100);
    CHECK(r.ok() && r.value().read_block(0, 0, out, d1).error() == Error::kInvalidArgument);
    CHECK(r.value().read_block(0, 0, out, d2).ok());
  }
}

// libFLAC-independent identity: these literals must never change with the encoder version.
void test_golden_identity() {
  CHECK(uuid_to_string(kLibraryNamespace) == "3b6aa869-030a-5cfd-97cc-957357048384");
  const PcmFormat f{1, 16, false, 48000};
  const std::vector<std::int32_t> pcm = {0, 1, -1, 32767, -32768, 100};
  BundleWriter w;
  const std::size_t i = w.add_instrument("Name is not identity", "a/b.sfz").value();
  CHECK_ERR(w.add_sample(i, {"anything", f, 6, pcm}), Error::kNone);
  const Bytes b = w.finalize().take();
  auto r = BundleReader::open_memory(b);
  CHECK(r.ok() && uuid_to_string(r.value().library_id()) == "9564e223-7ec9-5c36-8d4e-68c0a954f057");
  CHECK(r.ok() && to_hex(r.value().samples()[0].pcm_sha256) == "029a9d4699a07b1398c46dc67105a8f6f77ffd2d8b5788b522f2a9938cc9a644");
  // Names do not influence library_id.
  BundleWriter w2;
  const std::size_t i2 = w2.add_instrument("Other", "a/b.sfz").value();
  CHECK_ERR(w2.add_sample(i2, {"different", f, 6, pcm}), Error::kNone);
  const Bytes b2 = w2.finalize().take();
  CHECK(BundleReader::open_memory(b2).value().library_id() == r.value().library_id());
}

void test_atomic_file_write() {
  const std::string path = (std::filesystem::temp_directory_path() / "velora_atomic.vlb").string();
  BundleWriter w;
  CHECK(w.add_instrument("a", "a.sfz").ok());
  CHECK_ERR(w.finalize_to_file(path), Error::kNone);
  CHECK(std::filesystem::exists(path) && !std::filesystem::exists(path + ".tmp"));
  const std::string bad = (std::filesystem::temp_directory_path() / "no_such_dir_x" / "a.vlb").string();
  CHECK_ERR(w.finalize_to_file(bad), Error::kIo);
  CHECK(!std::filesystem::exists(bad + ".tmp"));
  std::filesystem::remove(path);
}

void test_version() {
  const PcmFormat f{1, 16, false, 48000};
  const auto pcm = vt::make_int(f, 500, Kind::kSine);
  for (std::uint32_t v : {kFormatVersion - 1, kFormatVersion}) {
    const Bytes b = build_single(f, pcm, WriterOptions{v});
    auto r = BundleReader::open_memory(b);
    CHECK(r.ok() && r.value().format_version() == v && matches_source(r.value(), 0, pcm));
  }
  const Bytes next = build_single(f, pcm, WriterOptions{kFormatVersion + 1});
  CHECK(!next.empty());
  auto r = BundleReader::open_memory(next);
  CHECK(!r.ok() && r.error() == Error::kUnsupportedVersion);
  // Also well beyond N+1 via header patch.
  Bytes far = build_single(f, pcm);
  wr32(far, 4, 0xFFFFFFFFu);
  CHECK_ERR(BundleReader::open_memory(far).error(), Error::kUnsupportedVersion);
  // Writer refuses versions it cannot produce.
  BundleWriter w(WriterOptions{kFormatVersion + 9});
  CHECK_ERR(w.finalize().error(), Error::kInvalidArgument);
}

void test_identity() {
  const PcmFormat f{2, 24, false, 48000};
  const auto p1 = vt::make_int(f, 6000, Kind::kMixed, 1);
  const auto p2 = vt::make_int(f, 6000, Kind::kMixed, 2);
  auto build = [&](const std::vector<std::int32_t>& p, const char* path) {
    BundleWriter w;
    const std::size_t i = w.add_instrument("Piano", path).value();
    CHECK_ERR(w.add_sample(i, {"s", f, 6000, p}), Error::kNone);
    return w.finalize().take();
  };
  const Bytes a = build(p1, "x/y.sfz"), a2 = build(p1, "x/y.sfz");
  CHECK(a == a2);  // identical bytes
  auto ra = BundleReader::open_memory(a), ra2 = BundleReader::open_memory(a2);
  CHECK(ra.value().bundle_id() == ra2.value().bundle_id());
  CHECK(ra.value().library_id() == ra2.value().library_id());
  CHECK(ra.value().bundle_id() ==
        Sha256::hash({a.data() + rd64(a, 8), std::size_t(rd64(a, 16))}));  // manifest hash
  const Bytes c = build(p2, "x/y.sfz");
  auto rc = BundleReader::open_memory(c);
  CHECK(rc.value().bundle_id() != ra.value().bundle_id());
  CHECK(rc.value().library_id() != ra.value().library_id());
  const Bytes d = build(p1, "x/z.sfz");
  CHECK(BundleReader::open_memory(d).value().library_id() != ra.value().library_id());
  // library_id is a v5 UUID.
  CHECK((ra.value().library_id()[6] >> 4) == 5);
  // Vendor-assigned library_id is carried verbatim.
  {
    BundleWriter w;
    Uuid vendor{};
    for (int i = 0; i < 16; ++i) vendor[i] = std::uint8_t(i + 1);
    w.set_library_id(vendor);
    CHECK(w.add_instrument("a", "a.sfz").ok());
    const Bytes vb = w.finalize().take();
    auto r = BundleReader::open_memory(vb);
    CHECK(r.ok() && r.value().library_id() == vendor);
  }
  // Empty bundle is valid.
  { BundleWriter w; auto b = w.finalize().take(); auto r = BundleReader::open_memory(b);
    CHECK(r.ok() && r.value().samples().empty() && r.value().verify_all() == Error::kNone); }

  // instrument_key normalisation.
  auto key = [](std::string_view p) { return normalize_instrument_key(p); };
  CHECK(key("a/b.sfz").value() == "a/b.sfz");
  CHECK(key("./a//b/./c.sfz").value() == "a/b/c.sfz");
  CHECK(key("A\\B\\c.SFZ").value() == "A/B/c.SFZ");  // case preserved
  CHECK(key("a/b/").value() == "a/b");
  CHECK_ERR(key("../a.sfz").error(), Error::kInvalidPath);
  CHECK_ERR(key("a/../b.sfz").error(), Error::kInvalidPath);
  CHECK_ERR(key("/abs/a.sfz").error(), Error::kInvalidPath);
  CHECK_ERR(key("C:\\a.sfz").error(), Error::kInvalidPath);
  CHECK_ERR(key("").error(), Error::kInvalidPath);
  CHECK_ERR(key(".").error(), Error::kInvalidPath);
  CHECK_ERR(key("./").error(), Error::kInvalidPath);
  CHECK_ERR(key(std::string_view("a\0b", 3)).error(), Error::kInvalidPath);
  CHECK_ERR(key("a\nb").error(), Error::kInvalidPath);
  BundleWriter w;
  CHECK(w.add_instrument("1", "a/b.sfz").ok());
  CHECK_ERR(w.add_instrument("2", "./a//b.sfz").error(), Error::kDuplicateKey);
  CHECK(w.add_instrument("3", "A/b.sfz").ok());  // case differs: distinct
  CHECK_ERR(w.add_instrument("4", "../x").error(), Error::kInvalidPath);
}

void test_file_io() {
  const auto dir = std::filesystem::temp_directory_path();
  const std::string path = (dir / "velora_format_test.vlb").string();
  BundleWriter w;
  const PcmFormat f{2, 16, false, 48000};
  const auto pcm = vt::make_int(f, 10000, Kind::kMixed);
  const std::size_t i = w.add_instrument("a", "a.sfz").value();
  CHECK_ERR(w.add_sample(i, {"s", f, 10000, pcm}), Error::kNone);
  CHECK_ERR(w.finalize_to_file(path), Error::kNone);
  auto r = BundleReader::open_file(path);
  CHECK(r.ok() && matches_source(r.value(), 0, pcm));
  // Same identity as the in-memory bundle.
  const Bytes mb = w.finalize().take();
  auto mem = BundleReader::open_memory(mb);
  CHECK(mem.ok() && r.ok() && mem.value().bundle_id() == r.value().bundle_id());
  // Truncated files.
  const Bytes all = w.finalize().take();
  for (std::size_t n : {std::size_t(0), std::size_t(10), std::size_t(127), std::size_t(128),
                        std::size_t(200), all.size() / 2, all.size() - 1}) {
    std::FILE* fp = std::fopen(path.c_str(), "wb");
    std::fwrite(all.data(), 1, n, fp);
    std::fclose(fp);
    auto t = BundleReader::open_file(path);
    CHECK(!t.ok() && t.error() == Error::kTruncated);
  }
  CHECK_ERR(BundleReader::open_file((dir / "does_not_exist.vlb").string()).error(), Error::kIo);
  CHECK_ERR(w.finalize_to_file((dir / "no_such_dir" / "x.vlb").string()), Error::kIo);
  std::filesystem::remove(path);
}

}  // namespace

int main() {
  test_roundtrip_all_formats();
  test_silence_noise_lengths();
  test_seek();
  test_float();
  test_unsupported();
  test_truncation_and_corruption_sweeps();
  test_structural_corruption();
  test_corrupt_block();
  test_decoder_only_corruption();
  test_hash_policy_and_structure();
  test_golden_identity();
  test_atomic_file_write();
  test_version();
  test_identity();
  test_file_io();
  if (g_fail == 0) std::printf("bundle_test OK\n");
  return g_fail == 0 ? 0 : 1;
}
