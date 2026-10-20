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
  layout::put_u32(m, 1);  // hand-built in the v1 manifest layout (no region table)
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
  layout::put_u32(out, 1);
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

// ByteSource with no direct addressing: every byte goes through read_at.
class IndirectSource final : public ByteSource {
 public:
  explicit IndirectSource(Bytes b) : b_(std::move(b)) {}
  std::uint64_t size() const override { return b_.size(); }
  const std::uint8_t* direct(std::uint64_t, std::size_t) const override { return nullptr; }
  Error read_at(std::uint64_t off, void* dst, std::size_t n) const override {
    if (off > b_.size() || n > b_.size() - off) return Error::kTruncated;
    if (n) std::memcpy(dst, b_.data() + off, n);
    return Error::kNone;
  }

 private:
  Bytes b_;
};

void test_open_source() {
  BundleWriter w;
  const PcmFormat f{2, 16, false, 48000};
  const auto pcm = vt::make_int(f, 6000, Kind::kMixed);
  const std::size_t i = w.add_instrument("a", "a.sfz").value();
  CHECK_ERR(w.add_sample(i, {"s", f, 6000, pcm}), Error::kNone);
  const Bytes bytes = w.finalize().take();
  auto r = BundleReader::open_source(std::make_unique<IndirectSource>(bytes));
  CHECK(r.ok());
  if (r.ok()) {
    CHECK(r.value().instruments().size() == 1 && r.value().instruments()[0].key == "a.sfz");
    CHECK(matches_source(r.value(), 0, pcm));
  }
  CHECK_ERR(BundleReader::open_source(nullptr).error(), Error::kInvalidArgument);
  Bytes cut(bytes.begin(), bytes.begin() + 100);
  CHECK_ERR(BundleReader::open_source(std::make_unique<IndirectSource>(cut)).error(),
            Error::kTruncated);
}

}  // namespace

Region sample_region(std::uint32_t sample) {
  Region r;
  r.sample = sample;
  r.key_lo = 36; r.key_hi = 48; r.key_center = 40; r.vel_lo = 10; r.vel_hi = 100;
  r.transpose = -3; r.tune_cents = 12.5f; r.volume_db = -6.25f; r.pan = 33.f;
  r.offset = 7; r.seq_length = 3; r.seq_position = 2; r.lorand = 0.125f; r.hirand = 0.875f;
  r.loop_mode = LoopMode::kLoopSustain; r.has_loop_points = true; r.loop_start = 100; r.loop_end = 900;
  r.ampeg = {0.01f, 5.f, 0.02f, 0.03f, 0.4f, 70.f, 1.5f};
  r.trigger = TriggerMode::kRelease;
  return r;
}

// Story 5.1: format 2.1 extension table (keyswitches, CC crossfades, filters).
Region ext_region(std::uint32_t sample) {
  Region r = sample_region(sample);
  r.sw = {24, 28, 26, -1, 37, -1, 26};
  r.xf.in_count = 2; r.xf.in[0] = {1, 0, 64}; r.xf.in[1] = {11, 10, 20};
  r.xf.out_count = 1; r.xf.out[0] = {74, 64, 127};
  r.xf.curve = XfCurve::kGain;
  r.filter = {FilterType::kHpf2p, 1200.5f, 3.5f};
  return r;
}

void test_extensions() {
  const PcmFormat f{2, 16, false, 48000};
  const auto pcm = vt::make_int(f, 1000, Kind::kMixed);
  auto version_word = [](const Bytes& b) { return layout::get_u32(b.data() + 4); };
  Region plain;
  Region sw_only = plain; sw_only.sw.last = 36;
  Region filt_only = plain; filt_only.filter = {FilterType::kBpf2p, 440.f, 0.f};
  Region curve_only = plain; curve_only.xf.curve = XfCurve::kGain;
  Region four = plain;
  four.xf.in_count = four.xf.out_count = 4;
  for (unsigned k = 0; k < 4; ++k) { four.xf.in[k] = {std::uint8_t(2 * k), 1, 2}; four.xf.out[k] = {std::uint8_t(100 + k), 0, 127}; }

  {  // round trip: mixed plain and extended regions in two instruments
    BundleWriter w;
    const std::size_t i0 = w.add_instrument("A", "a.sfz").value(), i1 = w.add_instrument("B", "b.sfz").value();
    CHECK_ERR(w.add_sample(i0, {"s0", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(w.add_sample(i1, {"s0", f, 1000, pcm}), Error::kNone);
    const std::vector<Region> a = {plain, ext_region(0), sample_region(0), sw_only, filt_only, curve_only, four};
    for (const auto& r : a) CHECK_ERR(w.add_region(i0, r), Error::kNone);
    CHECK_ERR(w.add_region(i1, ext_region(0)), Error::kNone);
    const Bytes b = w.finalize().take();
    CHECK(version_word(b) == (2u | (1u << 16)));
    auto r = BundleReader::open_memory(b);
    CHECK(r.ok() && r.value().format_version() == 2 && r.value().format_minor() == 1);
    if (r.ok()) {
      CHECK(r.value().instruments()[0].regions == a);
      CHECK(r.value().instruments()[1].regions.size() == 1 && r.value().instruments()[1].regions[0] == ext_region(0));
      CHECK(r.value().verify_all() == Error::kNone);
    }
    // Deterministic.
    CHECK(w.finalize().take() == b);
  }
  {  // a bundle without extension regions is minor 0 and has no extension bytes at all
    BundleWriter w;
    const std::size_t i0 = w.add_instrument("A", "a.sfz").value();
    CHECK_ERR(w.add_sample(i0, {"s0", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(w.add_region(i0, sample_region(0)), Error::kNone);
    const Bytes b = w.finalize().take();
    CHECK(version_word(b) == 2u);
    auto r = BundleReader::open_memory(b);
    CHECK(r.ok() && r.value().format_minor() == 0);
    // The manifest really ends with the 96-byte region record: no extension bytes follow it.
    {
      const std::size_t end = rd64(b, 8) + rd64(b, 16), rec = end - layout::kRegionRecordSize;
      CHECK(layout::get_u32(b.data() + rec) == 0u);          // sample index 0
      CHECK(b[rec + 4] == 36 && b[rec + 5] == 48 && b[rec + 6] == 40);  // key_lo, key_hi, key_center of sample_region
      CHECK(rec > rd64(b, 8) && end == rd64(b, 24));         // manifest is followed directly by the index
    }
    // Same bundle written by a "minor 0 only" writer: identical bytes.
    BundleWriter w0(WriterOptions{kFormatVersion, 0});
    const std::size_t j0 = w0.add_instrument("A", "a.sfz").value();
    CHECK_ERR(w0.add_sample(j0, {"s0", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(w0.add_region(j0, sample_region(0)), Error::kNone);
    CHECK(w0.finalize().take() == b);
  }
  {  // writer policy
    BundleWriter w(WriterOptions{kFormatVersion, 0});
    const std::size_t i0 = w.add_instrument("A", "a.sfz").value();
    CHECK_ERR(w.add_sample(i0, {"s0", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(w.add_region(i0, ext_region(0)), Error::kNone);
    CHECK_ERR(w.finalize().error(), Error::kInvalidArgument);  // minor 0 cannot carry it
    BundleWriter w2(WriterOptions{kFormatVersion, kFormatMinor + 1});
    CHECK_ERR(w2.finalize().error(), Error::kInvalidArgument);
    BundleWriter w3(WriterOptions{1, 1});  // version 1 has no regions at all
    const std::size_t k0 = w3.add_instrument("A", "a.sfz").value();
    CHECK_ERR(w3.add_sample(k0, {"s0", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(w3.add_region(k0, ext_region(0)), Error::kNone);
    CHECK_ERR(w3.finalize().error(), Error::kInvalidArgument);
    // Writer-side validation of the extension fields.
    BundleWriter v;
    const std::size_t i = v.add_instrument("A", "a.sfz").value();
    CHECK_ERR(v.add_sample(i, {"s", f, 1000, pcm}), Error::kNone);
    auto bad = [&](auto mutate) { Region x = ext_region(0); mutate(x); return v.add_region(i, x); };
    CHECK_ERR(bad([](Region& x) { x.sw.last = 128; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.sw.lokey = 40; x.sw.hikey = 30; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.xf.in_count = 5; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.xf.in[1].cc = 1; }), Error::kInvalidArgument);        // not ascending
    CHECK_ERR(bad([](Region& x) { x.xf.in[0].lo = 70; }), Error::kInvalidArgument);       // lo > hi
    CHECK_ERR(bad([](Region& x) { x.xf.in[2].cc = 3; }), Error::kInvalidArgument);        // unused entry not zero
    CHECK_ERR(bad([](Region& x) { x.xf.out[0].cc = 128; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.filter.cutoff_hz = 48001.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.filter.resonance_db = 41.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.filter.cutoff_hz = std::nanf(""); }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.filter.type = FilterType::kNone; }), Error::kInvalidArgument);  // values without a type
    CHECK_ERR(bad([](Region& x) { x.filter.type = static_cast<FilterType>(6); }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.xf.curve = static_cast<XfCurve>(2); }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region&) {}), Error::kNone);
  }
  {  // reader version policy: minor N and N-1 accepted, newer rejected
    BundleWriter w;
    const std::size_t i0 = w.add_instrument("A", "a.sfz").value();
    CHECK_ERR(w.add_sample(i0, {"s0", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(w.add_region(i0, ext_region(0)), Error::kNone);
    const Bytes good = w.finalize().take();
    CHECK(BundleReader::open_memory(good).ok());
    { Bytes b = good; wr32(b, 4, 2u | (2u << 16)); CHECK_ERR(BundleReader::open_memory(b).error(), Error::kUnsupportedVersion); }
    { Bytes b = good; wr32(b, 4, 3u | (1u << 16)); CHECK_ERR(BundleReader::open_memory(b).error(), Error::kUnsupportedVersion); }
    { Bytes b = good; wr32(b, 4, 1u | (1u << 16)); CHECK_ERR(BundleReader::open_memory(b).error(), Error::kUnsupportedVersion); }
    { Bytes b = good; wr32(b, 4, 2u); CHECK(!BundleReader::open_memory(b).ok()); }  // word differs from the manifest's
    // An Epic 1 reader (major 2, minor 0 only) would reject minor 1 outright; the word proves it
    // is not mistaken for a minor 0 bundle.
    CHECK(layout::get_u32(good.data() + 4) != 2u);
  }
  {  // a minor 1 bundle whose instruments all have n_ext == 0 is non-canonical
    BundleWriter v;
    const std::size_t i = v.add_instrument("A", "a.sfz").value();
    CHECK_ERR(v.add_sample(i, {"s", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(v.add_region(i, plain), Error::kNone);
    Bytes b = v.finalize().take();  // minor 0
    const std::size_t mo = rd64(b, 8), ms = rd64(b, 16), io = rd64(b, 24), is = rd64(b, 32);
    b.insert(b.begin() + mo + ms, 4, std::uint8_t(0));  // n_ext = 0
    wr64(b, 16, ms + 4); wr64(b, 24, io + 4); wr64(b, 40, rd64(b, 40) + 4); wr64(b, 56, rd64(b, 56) + 4);
    for (std::size_t e = 0; e < is / 48; ++e) wr64(b, io + 4 + e * 48, rd64(b, io + 4 + e * 48) + 4);
    wr32(b, 4, 2u | (1u << 16));
    wr32(b, mo, 2u | (1u << 16));
    rehash(b);
    CHECK_ERR(BundleReader::open_memory(b).error(), Error::kCorruptManifest);
  }
  {  // corruption: every violation of the canonical form is kCorruptManifest
    BundleWriter v;
    const std::size_t i = v.add_instrument("A", "a.sfz").value();
    CHECK_ERR(v.add_sample(i, {"s", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(v.add_region(i, plain), Error::kNone);
    CHECK_ERR(v.add_region(i, ext_region(0)), Error::kNone);
    const Bytes good = v.finalize().take();
    const std::size_t end = rd64(good, 8) + rd64(good, 16);
    const std::size_t rec = end - layout::kExtRecordSize;
    CHECK(layout::kExtRecordSize == 56 && layout::get_u32(good.data() + rec - 4) == 1u);
    auto expect_bad = [&](Bytes bad, const char* what) {
      rehash(bad);
      auto rb = BundleReader::open_memory(bad);
      if (rb.ok() || rb.error() != Error::kCorruptManifest) { std::printf("FAIL ext corruption not rejected: %s\n", what); ++g_fail; }
    };
    { Bytes b = good; wr32(b, rec, 5); expect_bad(b, "region index out of range"); }
    { Bytes b = good; b[rec + 4] |= 0x80; expect_bad(b, "reserved flag bit"); }
    { Bytes b = good; b[rec + 4] &= ~0x04; expect_bad(b, "sw_last value while its flag is clear"); }
    { Bytes b = good; b[rec + 5 + 3] = 99; expect_bad(b, "sw_down value while flag clear"); }
    { Bytes b = good; b[rec + 12] = 5; expect_bad(b, "xfin count 5"); }
    { Bytes b = good; b[rec + 14] = 2; expect_bad(b, "curve 2"); }
    { Bytes b = good; b[rec + 15] = 9; expect_bad(b, "filter type 9"); }
    { Bytes b = good; b[rec + 16 + 3] = 1; expect_bad(b, "non-zero padding"); }
    { Bytes b = good; b[rec + 16] = 12; expect_bad(b, "xfin cc not ascending (12 then 11)"); }
    { Bytes b = good; wr32(b, rec + 48, 0x7fc00000u); expect_bad(b, "NaN cutoff"); }
    { Bytes b = good; wr32(b, rec - 4, 7); expect_bad(b, "n_ext larger than regions"); }
    { Bytes b = good; wr32(b, rec - 4, 0xffffffffu); expect_bad(b, "huge n_ext"); }
    { Bytes b = good; std::fill(b.begin() + rec + 4, b.begin() + rec + 56, std::uint8_t(0)); b[rec + 14] = 1; expect_bad(b, "record without any extension feature"); }
    { Bytes b = good; std::fill(b.begin() + rec + 4, b.begin() + rec + 56, std::uint8_t(0)); b[rec + 14] = 1;
      wr32(b, rec - 4, 1); expect_bad(b, "all-default record"); }
    // Truncations always fail; any bit flip in the extension table is rejected or still valid.
    for (std::size_t n = 0; n < good.size(); n += (n < 500 ? 1 : 17))
      CHECK(!BundleReader::open_memory(std::span<const std::uint8_t>(good.data(), n)).ok());
    for (std::size_t off = rec - 4; off < end; ++off)
      for (int bit = 0; bit < 8; ++bit) {
        Bytes bad = good;
        bad[off] ^= std::uint8_t(1 << bit);
        rehash(bad);
        auto r3 = BundleReader::open_memory(bad);
        if (!r3.ok() && r3.error() != Error::kCorruptManifest) {
          std::printf("FAIL ext flip at %zu bit %d -> %s\n", off, bit, to_string(r3.error()));
          ++g_fail;
        }
      }
  }
  {  // two records in region order: indexes must ascend
    BundleWriter v;
    const std::size_t i = v.add_instrument("A", "a.sfz").value();
    CHECK_ERR(v.add_sample(i, {"s", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(v.add_region(i, ext_region(0)), Error::kNone);
    CHECK_ERR(v.add_region(i, sw_only), Error::kNone);
    const Bytes good = v.finalize().take();
    const std::size_t end = rd64(good, 8) + rd64(good, 16), r1 = end - layout::kExtRecordSize;
    CHECK(layout::get_u32(good.data() + r1) == 1u && layout::get_u32(good.data() + r1 - layout::kExtRecordSize) == 0u);
    Bytes bad = good;
    wr32(bad, r1, 0);  // duplicate index
    rehash(bad);
    CHECK_ERR(BundleReader::open_memory(bad).error(), Error::kCorruptManifest);
  }
  {  // golden: the extension record serialization is pinned (independent of libFLAC)
    BundleWriter v;
    const std::size_t i = v.add_instrument("A", "a.sfz").value();
    CHECK_ERR(v.add_sample(i, {"s", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(v.add_region(i, ext_region(0)), Error::kNone);
    const Bytes b = v.finalize().take();
    const std::size_t end = rd64(b, 8) + rd64(b, 16);
    std::string got;
    static const char* hexd = "0123456789abcdef";
    for (std::size_t k = end - layout::kExtRecordSize; k < end; ++k) { got += hexd[b[k] >> 4]; got += hexd[b[k] & 15]; }
    CHECK(got == "0000000057181c1a0025001a02010004010040000b0a140000000000000000004a407f000000000000000000000000000010964400006040");
    CHECK(layout::kExtRecordSize == 56);
  }
}

void test_regions() {
  const PcmFormat f{2, 16, false, 48000};
  const auto pcm = vt::make_int(f, 1000, Kind::kMixed);
  BundleWriter w;
  const std::size_t i0 = w.add_instrument("A", "a.sfz").value(), i1 = w.add_instrument("B", "b.sfz").value();
  CHECK_ERR(w.add_sample(i0, {"s0", f, 1000, pcm}), Error::kNone);
  CHECK_ERR(w.add_sample(i0, {"s1", f, 1000, pcm}), Error::kNone);
  CHECK_ERR(w.add_sample(i1, {"s0", f, 1000, pcm}), Error::kNone);
  Region plain;  // all defaults
  const Region full = sample_region(1);
  CHECK_ERR(w.add_region(i0, plain), Error::kNone);
  CHECK_ERR(w.add_region(i0, full), Error::kNone);
  CHECK_ERR(w.add_region(i1, sample_region(0)), Error::kNone);
  const Bytes b = w.finalize().take();
  auto r = BundleReader::open_memory(b);
  CHECK(r.ok() && r.value().format_version() == 2);
  if (r.ok()) {
    CHECK(r.value().instruments()[0].regions.size() == 2 && r.value().instruments()[1].regions.size() == 1);
    CHECK(r.value().instruments()[0].regions[0] == plain);
    CHECK(r.value().instruments()[0].regions[1] == full);
    CHECK(r.value().instruments()[1].regions[0] == sample_region(0));
    CHECK(r.value().verify_all() == Error::kNone);
  }
  // Regions are part of the manifest, hence of bundle_id; the sample content fingerprint is not.
  BundleWriter w2;
  const std::size_t j0 = w2.add_instrument("A", "a.sfz").value(), j1 = w2.add_instrument("B", "b.sfz").value();
  CHECK_ERR(w2.add_sample(j0, {"s0", f, 1000, pcm}), Error::kNone);
  CHECK_ERR(w2.add_sample(j0, {"s1", f, 1000, pcm}), Error::kNone);
  CHECK_ERR(w2.add_sample(j1, {"s0", f, 1000, pcm}), Error::kNone);
  const Bytes b2 = w2.finalize().take();
  auto r2 = BundleReader::open_memory(b2);
  CHECK(r2.ok() && r2.value().library_id() == r.value().library_id());
  CHECK(r2.ok() && r2.value().bundle_id() != r.value().bundle_id());
  CHECK(r2.ok() && r2.value().instruments()[0].regions.empty());

  // Writer-side validation.
  { BundleWriter v; const auto i = v.add_instrument("A", "a.sfz").value();
    CHECK_ERR(v.add_sample(i, {"s", f, 1000, pcm}), Error::kNone);
    Region bad;
    bad = plain; bad.sample = 1; CHECK_ERR(v.add_region(i, bad), Error::kInvalidArgument);
    bad = plain; bad.key_lo = 90; bad.key_hi = 80; CHECK_ERR(v.add_region(i, bad), Error::kInvalidArgument);
    bad = plain; bad.vel_hi = 128; CHECK_ERR(v.add_region(i, bad), Error::kInvalidArgument);
    bad = plain; bad.seq_position = 2; CHECK_ERR(v.add_region(i, bad), Error::kInvalidArgument);
    bad = plain; bad.lorand = 0.9f; bad.hirand = 0.1f; CHECK_ERR(v.add_region(i, bad), Error::kInvalidArgument);
    bad = plain; bad.offset = 1001; CHECK_ERR(v.add_region(i, bad), Error::kInvalidArgument);
    bad = plain; bad.has_loop_points = true; bad.loop_end = 1000; CHECK_ERR(v.add_region(i, bad), Error::kInvalidArgument);
    bad = plain; bad.has_loop_points = true; bad.loop_start = 5; bad.loop_end = 4; CHECK_ERR(v.add_region(i, bad), Error::kInvalidArgument);
    bad = plain; bad.tune_cents = std::nanf(""); CHECK_ERR(v.add_region(i, bad), Error::kInvalidArgument);
    bad = plain; bad.ampeg.release = -1.f; CHECK_ERR(v.add_region(i, bad), Error::kInvalidArgument);
    CHECK_ERR(v.add_region(7, plain), Error::kInvalidArgument);
    CHECK_ERR(v.add_region(i, plain), Error::kNone);
  }

  // Format version 1 (N-1): no region table is written, regions are refused, reads give empty tables.
  { BundleWriter v(WriterOptions{1}); const auto i = v.add_instrument("A", "a.sfz").value();
    CHECK_ERR(v.add_sample(i, {"s", f, 1000, pcm}), Error::kNone);
    const Bytes v1 = v.finalize().take();
    auto rv = BundleReader::open_memory(v1);
    CHECK(rv.ok() && rv.value().format_version() == 1 && rv.value().instruments()[0].regions.empty());
    CHECK(rv.ok() && matches_source(rv.value(), 0, pcm));
    CHECK_ERR(v.add_region(i, plain), Error::kNone);
    CHECK_ERR(v.finalize().error(), Error::kInvalidArgument);
  }

  // Corrupt region records with valid hashes are rejected by the reader.
  {
    BundleWriter v; const auto i = v.add_instrument("A", "a.sfz").value();
    CHECK_ERR(v.add_sample(i, {"s", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(v.add_region(i, plain), Error::kNone);
    const Bytes good = v.finalize().take();
    const std::size_t rec = rd64(good, 8) + rd64(good, 16) - layout::kRegionRecordSize;  // the only region
    auto expect_bad = [&](Bytes bad, const char* what) {
      rehash(bad);
      auto rb = BundleReader::open_memory(bad);
      if (rb.ok() || rb.error() != Error::kCorruptManifest) { std::printf("FAIL region corruption not rejected: %s\n", what); ++g_fail; }
    };
    { Bytes b3 = good; wr32(b3, rec, 5); expect_bad(b3, "sample index"); }
    { Bytes b3 = good; b3[rec + 4] = 100; b3[rec + 5] = 50; expect_bad(b3, "key range"); }
    { Bytes b3 = good; b3[rec + 9] = 9; expect_bad(b3, "loop mode"); }
    { Bytes b3 = good; b3[rec + 11] = 2; expect_bad(b3, "loop flag"); }
    { Bytes b3 = good; wr32(b3, rec - 4, 0x00ffffffu); expect_bad(b3, "region count"); }
    { Bytes b3 = good; wr64(b3, rec + 28, 5000); expect_bad(b3, "offset beyond sample"); }
    { Bytes b3 = good; wr32(b3, rec + 24, 0x7fc00000u); expect_bad(b3, "NaN pan"); }
    // Truncated record: cut the manifest and fix sizes is covered by the generic truncation sweep.
    CHECK(BundleReader::open_memory(good).ok());
  }

  // Ranges match what the SFZ importer enforces.
  { BundleWriter v; const auto i = v.add_instrument("A", "a.sfz").value();
    CHECK_ERR(v.add_sample(i, {"s", f, 1000, pcm}), Error::kNone);
    auto bad_region = [&](auto mutate) { Region x = plain; mutate(x); return v.add_region(i, x); };
    CHECK_ERR(bad_region([](Region& x) { x.transpose = 128; }), Error::kInvalidArgument);
    CHECK_ERR(bad_region([](Region& x) { x.tune_cents = 1201.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad_region([](Region& x) { x.volume_db = -145.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad_region([](Region& x) { x.volume_db = 49.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad_region([](Region& x) { x.offset = 1000; }), Error::kInvalidArgument);
    CHECK_ERR(bad_region([](Region& x) { x.ampeg.attack = 101.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad_region([](Region& x) { x.transpose = -127; x.tune_cents = -1200.f; x.volume_db = 48.f; x.offset = 999; }), Error::kNone);
  }

  // Sweeps over a bundle that has regions: no crash, truncation always fails, and any single bit
  // flip inside the region records (hashes recomputed so only the container checks can object) is
  // either still a valid bundle or kCorruptManifest.
  {
    BundleWriter v; const auto i = v.add_instrument("A", "a.sfz").value();
    CHECK_ERR(v.add_sample(i, {"s", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(v.add_region(i, plain), Error::kNone);
    CHECK_ERR(v.add_region(i, sample_region(0)), Error::kNone);
    const Bytes good = v.finalize().take();
    for (std::size_t n = 0; n < good.size(); n += (n < 400 ? 1 : 13)) {
      auto r3 = BundleReader::open_memory(std::span<const std::uint8_t>(good.data(), n));
      CHECK(!r3.ok());
    }
    const std::size_t mo = rd64(good, 8), ms = rd64(good, 16);
    const std::size_t first = mo + ms - 2 * layout::kRegionRecordSize;
    for (std::size_t off = first - 4; off < mo + ms; ++off)
      for (int bit = 0; bit < 8; ++bit) {
        Bytes bad = good;
        bad[off] ^= std::uint8_t(1 << bit);
        rehash(bad);
        auto r3 = BundleReader::open_memory(bad);
        if (!r3.ok() && r3.error() != Error::kCorruptManifest) {
          std::printf("FAIL region flip at %zu bit %d -> %s\n", off, bit, to_string(r3.error()));
          ++g_fail;
        }
      }
  }

  // Golden v2: the region record serialization is pinned (it does not depend on libFLAC).
  {
    BundleWriter v; const auto i = v.add_instrument("A", "a.sfz").value();
    CHECK_ERR(v.add_sample(i, {"s", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(v.add_region(i, sample_region(0)), Error::kNone);
    const Bytes b4 = v.finalize().take();
    const std::size_t end = rd64(b4, 8) + rd64(b4, 16);
    std::string got;
    static const char* hexd = "0123456789abcdef";
    for (std::size_t k = end - layout::kRegionRecordSize; k < end; ++k) { got += hexd[b4[k] >> 4]; got += hexd[b4[k] & 15]; }
    CHECK(got == "000000002430280a64030101fdffffff000048410000c8c000000442070000000000000003000000020000000000003e0000603f640000000000000084030000000000000ad7233c0000a0400ad7a33c8fc2f53ccdcccc3e00008c420000c03f");
    CHECK(layout::kRegionRecordSize == 96);
  }

  // library_id from a source id: stable across content changes, distinct per source.
  { BundleWriter e; CHECK_ERR(e.set_source_id(""), Error::kInvalidArgument); }  // empty is refused
  {
    auto bytes = [&](const char* src, std::uint64_t seed, bool vendor) {
      BundleWriter v; CHECK_ERR(v.set_source_id(src), Error::kNone);
      if (vendor) { Uuid u{}; u[0] = 9; v.set_library_id(u); }
      const auto i = v.add_instrument("A", "a.sfz").value();
      const auto p = vt::make_int(f, 1000, Kind::kMixed, seed);
      CHECK_ERR(v.add_sample(i, {"s", f, 1000, p}), Error::kNone);
      return v.finalize().take(); };
    const Bytes x1 = bytes("/lib/Piano.sfz", 1, false), x2 = bytes("/lib/Piano.sfz", 2, false), y = bytes("/lib/Other.sfz", 1, false);
    const Bytes vend = bytes("/lib/Piano.sfz", 1, true);
    auto a = BundleReader::open_memory(x1), b3 = BundleReader::open_memory(x2), c = BundleReader::open_memory(y), d = BundleReader::open_memory(vend);
    CHECK(a.ok() && b3.ok() && c.ok() && d.ok());
    CHECK(a.value().library_id() == b3.value().library_id() && a.value().bundle_id() != b3.value().bundle_id());
    CHECK(a.value().library_id() != c.value().library_id());
    CHECK((a.value().library_id()[6] >> 4) == 5);
    CHECK(d.value().library_id()[0] == 9);  // vendor-assigned wins
  }
}

// Story 5.8: discard_last_instrument() leaves the writer exactly as if the instrument had never been added.
void test_discard_last_instrument() {
  const PcmFormat f{1, 16, false, 48000};
  const auto a = vt::make_int(f, 5000, Kind::kSine);
  const auto b = vt::make_int(f, 9000, Kind::kNoise);
  const auto c = vt::make_int(f, 300, Kind::kSine);
  auto reference = [&]() {
    BundleWriter w;
    CHECK_ERR(w.set_source_id("/src/x.sf2"), Error::kNone);
    auto i = w.add_instrument("A", "0:0");
    CHECK(i.ok());
    CHECK_ERR(w.add_sample(i.value(), {"a", f, a.size(), a}), Error::kNone);
    auto fin = w.finalize();
    CHECK(fin.ok());
    return fin.ok() ? fin.take() : Bytes{};
  }();
  BundleWriter w;
  CHECK_ERR(w.discard_last_instrument(), Error::kInvalidArgument);  // nothing to drop
  CHECK_ERR(w.set_source_id("/src/x.sf2"), Error::kNone);
  auto ia = w.add_instrument("A", "0:0");
  CHECK_ERR(w.add_sample(ia.value(), {"a", f, a.size(), a}), Error::kNone);
  auto ib = w.add_instrument("B", "0:1");
  CHECK_ERR(w.add_sample(ib.value(), {"b1", f, b.size(), b}), Error::kNone);
  CHECK_ERR(w.add_sample(ib.value(), {"b2", f, c.size(), c}), Error::kNone);
  CHECK_ERR(w.discard_last_instrument(), Error::kNone);
  auto fin = w.finalize();
  CHECK(fin.ok() && fin.value() == reference);
  // The key is free again, and the writer keeps working after the drop.
  auto ib2 = w.add_instrument("B", "0:1");
  CHECK(ib2.ok());
  CHECK_ERR(w.add_sample(ib2.value(), {"b1", f, b.size(), b}), Error::kNone);
  BundleWriter ref2;
  CHECK_ERR(ref2.set_source_id("/src/x.sf2"), Error::kNone);
  auto ra = ref2.add_instrument("A", "0:0");
  CHECK_ERR(ref2.add_sample(ra.value(), {"a", f, a.size(), a}), Error::kNone);
  auto rb = ref2.add_instrument("B", "0:1");
  CHECK_ERR(ref2.add_sample(rb.value(), {"b1", f, b.size(), b}), Error::kNone);
  auto f1 = w.finalize(), f2 = ref2.finalize();
  CHECK(f1.ok() && f2.ok() && f1.value() == f2.value());
  // An instrument without samples can be dropped too; one that is not at the tail of the block store cannot.
  auto empty = w.add_instrument("E", "0:2");
  CHECK(empty.ok());
  CHECK_ERR(w.discard_last_instrument(), Error::kNone);
  BundleWriter x;
  auto xa = x.add_instrument("A", "k/a");
  auto xb = x.add_instrument("B", "k/b");
  CHECK_ERR(x.add_sample(xa.value(), {"a", f, a.size(), a}), Error::kNone);
  CHECK_ERR(x.add_sample(xb.value(), {"b", f, b.size(), b}), Error::kNone);
  CHECK_ERR(x.add_sample(xa.value(), {"a2", f, c.size(), c}), Error::kNone);  // A's blocks now follow B's
  CHECK_ERR(x.discard_last_instrument(), Error::kInvalidArgument);
}

// Story 6.2 guard: the unprotected format is unchanged. Float-only bundles involve no libFLAC, so their bytes are the same on every platform and their
// SHA-256 is pinned for format 2 (minor 0) and format 2.1 (minor 1, extension table). The values were taken from the pre-Story-6.2 tree (HEAD 8c1be3a).
void test_float_goldens() {
  for (bool ext : {false, true}) {
    const PcmFormat f{2, 32, true, 48000};
    const auto pcm = vt::make_float(2, 5000);
    BundleWriter w;
    w.set_library_id(Uuid{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
    auto i0 = w.add_instrument("Pad", "pads/warm.sfz");
    CHECK(i0.ok());
    CHECK_ERR(w.add_sample(i0.value(), {"warm", f, 5000, pcm}), Error::kNone);
    Region r;
    r.sample = 0;
    CHECK_ERR(w.add_region(i0.value(), r), Error::kNone);
    if (ext) {
      r.sw = {24, 28, 26, -1, 37, -1, 26};
      CHECK_ERR(w.add_region(i0.value(), r), Error::kNone);
    }
    auto b = w.finalize();
    CHECK(b.ok());
    if (!b.ok()) continue;
    CHECK(layout::get_u32(b.value().data() + 4) == (ext ? (2u | (1u << 16)) : 2u));
    const std::string h = to_hex(Sha256::hash(b.value()));
    CHECK(h == (ext ? "e63a91c4091b49b552e1e927314f244f06f56cc31cb6bc8a7c98d17a190e2a9b" : "4014b5788d6a090bb28fa4f187823457aa098021540024d4b5c78dde7cae70b7"));
    if (h != (ext ? "e63a91c4091b49b552e1e927314f244f06f56cc31cb6bc8a7c98d17a190e2a9b" : "4014b5788d6a090bb28fa4f187823457aa098021540024d4b5c78dde7cae70b7")) std::printf("  %s golden: %s\n", ext ? "2.1" : "2", h.c_str());
  }
}

int main() {
  test_discard_last_instrument();
  test_open_source();
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
  test_regions();
  test_extensions();
  test_float_goldens();
  test_identity();
  test_file_io();
  if (g_fail == 0) std::printf("bundle_test OK\n");
  return g_fail == 0 ? 0 : 1;
}
