#include <algorithm>
#include <cmath>
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
    { Bytes b = good; wr32(b, 4, 2u | ((kFormatMinor + 1) << 16)); CHECK_ERR(BundleReader::open_memory(b).error(), Error::kUnsupportedVersion); }
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
  {  // Story 13.3: format 2.2, trigger first/legato/release_key and `end`
    Region first = plain; first.trigger = TriggerMode::kFirst;
    Region legato = plain; legato.trigger = TriggerMode::kLegato; legato.has_end = true; legato.end = 700;
    Region rkey = ext_region(0); rkey.trigger = TriggerMode::kReleaseKey;
    Region only_end = sample_region(0); only_end.has_end = true; only_end.end = 5000;  // beyond the file: stored as given
    Region off = plain; off.has_end = true; off.end = 0;                                // disabled region
    const std::vector<Region> a = {plain, first, legato, rkey, only_end, off};
    BundleWriter w;
    const std::size_t i0 = w.add_instrument("A", "a.sfz").value(), i1 = w.add_instrument("B", "b.sfz").value();
    CHECK_ERR(w.add_sample(i0, {"s0", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(w.add_sample(i1, {"s0", f, 1000, pcm}), Error::kNone);
    for (const auto& r : a) CHECK_ERR(w.add_region(i0, r), Error::kNone);
    CHECK_ERR(w.add_region(i1, plain), Error::kNone);  // an instrument without trigger/end records still carries `u32 0`
    const Bytes b = w.finalize().take();
    CHECK(version_word(b) == (2u | (2u << 16)));
    auto r = BundleReader::open_memory(b);
    CHECK(r.ok() && r.value().format_minor() == 2);
    if (r.ok()) {
      CHECK(r.value().instruments()[0].regions == a);
      CHECK(r.value().instruments()[1].regions.size() == 1 && r.value().instruments()[1].regions[0] == plain);
      CHECK(r.value().verify_all() == Error::kNone);
    }
    CHECK(w.finalize().take() == b);  // deterministic
    // N-1 writer: refuses trigger/end regions, and writes any other content as before (minor 1 or 0).
    BundleWriter w1(WriterOptions{kFormatVersion, 1});
    const std::size_t j0 = w1.add_instrument("A", "a.sfz").value();
    CHECK_ERR(w1.add_sample(j0, {"s0", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(w1.add_region(j0, first), Error::kNone);
    CHECK_ERR(w1.finalize().error(), Error::kInvalidArgument);
    // Content without them is byte-identical whatever the writer's minor ceiling (existing bundles do not change).
    for (const Region& rg : {plain, sample_region(0), ext_region(0)}) {
      Bytes by[3];
      for (std::uint32_t m = 0; m < 3; ++m) {
        if (m == 0 && has_extension(rg)) continue;
        BundleWriter x(WriterOptions{kFormatVersion, m});
        const std::size_t k = x.add_instrument("A", "a.sfz").value();
        CHECK_ERR(x.add_sample(k, {"s0", f, 1000, pcm}), Error::kNone);
        CHECK_ERR(x.add_region(k, rg), Error::kNone);
        by[m] = x.finalize().take();
      }
      CHECK(by[2] == by[1] || by[1].empty());
      CHECK(by[2] == by[0] || by[0].empty());
      CHECK(version_word(by[2]) == (has_extension(rg) ? (2u | (1u << 16)) : 2u));
    }
    // The N-1 reader view: a minor 1 bundle still opens (kFormatMinor is 2).
    {
      BundleWriter x;
      const std::size_t k = x.add_instrument("A", "a.sfz").value();
      CHECK_ERR(x.add_sample(k, {"s0", f, 1000, pcm}), Error::kNone);
      CHECK_ERR(x.add_region(k, ext_region(0)), Error::kNone);
      auto rr = BundleReader::open_memory(x.finalize().take());
      CHECK(rr.ok() && rr.value().format_minor() == 1);
    }
    // Writer-side validation.
    BundleWriter v;
    const std::size_t vi = v.add_instrument("A", "a.sfz").value();
    CHECK_ERR(v.add_sample(vi, {"s", f, 1000, pcm}), Error::kNone);
    { Region x = plain; x.end = 5; CHECK_ERR(v.add_region(vi, x), Error::kInvalidArgument); }  // end without has_end
    { Region x = plain; x.trigger = static_cast<TriggerMode>(5); CHECK_ERR(v.add_region(vi, x), Error::kInvalidArgument); }
    // Corruption of the trigger table (the last instrument's table is the tail of the manifest only when it is the last record).
    BundleWriter u;
    const std::size_t ui = u.add_instrument("A", "a.sfz").value();
    CHECK_ERR(u.add_sample(ui, {"s", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(u.add_region(ui, plain), Error::kNone);
    CHECK_ERR(u.add_region(ui, legato), Error::kNone);
    const Bytes good = u.finalize().take();
    const std::size_t end = rd64(good, 8) + rd64(good, 16), rec = end - layout::kTrigRecordSize;
    CHECK(layout::kTrigRecordSize == 16 && layout::get_u32(good.data() + rec - 4) == 1u && layout::get_u32(good.data() + rec) == 1u);
    CHECK(good[rec + 4] == 3 && good[rec + 5] == 1 && layout::get_u64(good.data() + rec + 8) == 700u);
    auto expect_bad = [&](Bytes bad, const char* what) {
      rehash(bad);
      auto rb = BundleReader::open_memory(bad);
      if (rb.ok() || rb.error() != Error::kCorruptManifest) { std::printf("FAIL trigger table corruption not rejected: %s\n", what); ++g_fail; }
    };
    { Bytes b = good; b[rec + 4] = 5; expect_bad(b, "trigger 5"); }
    { Bytes b = good; b[rec + 4] = 1; expect_bad(b, "trigger byte disagrees with the region record"); }
    { Bytes b = good; b[rec + 5] = 3; expect_bad(b, "reserved flag bit"); }
    { Bytes b = good; b[rec + 6] = 1; expect_bad(b, "non-zero padding"); }
    { Bytes b = good; b[rec + 5] = 0; expect_bad(b, "end without has_end"); }
    { Bytes b = good; wr32(b, rec, 9); expect_bad(b, "region index out of range"); }
    { Bytes b = good; wr32(b, rec - 4, 3); expect_bad(b, "n_trig larger than regions"); }
    { Bytes b = good; b[rec + 4] = 0; b[rec + 5] = 0; std::fill(b.begin() + rec + 8, b.begin() + rec + 16, std::uint8_t(0)); expect_bad(b, "all-default record"); }
    // The region record's own trigger byte is 0/1 only: first/legato/release_key there would be a second encoding (every minor).
    {
      const std::size_t reg1 = end - layout::kTrigRecordSize - 4 - 4 - layout::kRegionRecordSize;  // region 1 (legato): its record precedes n_ext, n_trig, the record
      CHECK(good[reg1 + 10] == 0);
      for (std::uint8_t t : {2, 3, 4}) { Bytes b = good; b[reg1 + 10] = t; expect_bad(b, "region record trigger byte > 1 (minor 2)"); }
      BundleWriter m0;
      const std::size_t k = m0.add_instrument("A", "a.sfz").value();
      CHECK_ERR(m0.add_sample(k, {"s", f, 1000, pcm}), Error::kNone);
      CHECK_ERR(m0.add_region(k, plain), Error::kNone);
      Bytes b0 = m0.finalize().take();  // minor 0: the region record is the manifest tail
      const std::size_t r0 = rd64(b0, 8) + rd64(b0, 16) - layout::kRegionRecordSize;
      for (std::uint8_t t : {2, 4}) { Bytes b = b0; b[r0 + 10] = t; expect_bad(b, "region record trigger byte > 1 (minor 0)"); }
    }
    for (std::size_t n = 0; n < good.size(); n += (n < 500 ? 1 : 17))
      CHECK(!BundleReader::open_memory(std::span<const std::uint8_t>(good.data(), n)).ok());
    for (std::size_t off = rec - 4; off < end; ++off)
      for (int bit = 0; bit < 8; ++bit) {
        Bytes bad = good;
        bad[off] ^= std::uint8_t(1 << bit);
        rehash(bad);
        auto r3 = BundleReader::open_memory(bad);
        if (!r3.ok() && r3.error() != Error::kCorruptManifest) { std::printf("FAIL trig flip at %zu bit %d -> %s\n", off, bit, to_string(r3.error())); ++g_fail; }
      }
    // A minor 2 stamp on a bundle with no trigger record is non-canonical.
    {
      BundleWriter x;
      const std::size_t k = x.add_instrument("A", "a.sfz").value();
      CHECK_ERR(x.add_sample(k, {"s", f, 1000, pcm}), Error::kNone);
      CHECK_ERR(x.add_region(k, ext_region(0)), Error::kNone);
      Bytes b = x.finalize().take();  // minor 1
      const std::size_t mo = rd64(b, 8), ms = rd64(b, 16), io = rd64(b, 24), is = rd64(b, 32);
      b.insert(b.begin() + mo + ms, 4, std::uint8_t(0));  // n_trig = 0
      wr64(b, 16, ms + 4); wr64(b, 24, io + 4); wr64(b, 40, rd64(b, 40) + 4); wr64(b, 56, rd64(b, 56) + 4);
      for (std::size_t e = 0; e < is / 48; ++e) wr64(b, io + 4 + e * 48, rd64(b, io + 4 + e * 48) + 4);
      wr32(b, 4, 2u | (2u << 16));
      wr32(b, mo, 2u | (2u << 16));
      rehash(b);
      CHECK_ERR(BundleReader::open_memory(b).error(), Error::kCorruptManifest);
    }
  }
  {  // Story 13.4: format 2.3, the amp table (veltrack, velocity curve, amplitude, keytrack, vel2 scalars)
    Region veltrack = plain; veltrack.amp.veltrack = 25.f;
    Region curve = sample_region(0);
    curve.amp.curve_count = 3; curve.amp.curve[0] = {1, 0.25f}; curve.amp.curve[1] = {64, 0.5f}; curve.amp.curve[2] = {127, 1.f};
    curve.amp.amplitude = 80.f; curve.amp.keytrack = -0.5f; curve.amp.keycenter = 48;
    Region all = ext_region(0);  // minor 1 + 2 + 3 content in one region
    all.trigger = TriggerMode::kLegato; all.has_end = true; all.end = 900;
    all.amp.veltrack = -100.f; all.amp.vel2attack = 0.1f; all.amp.vel2decay = -0.2f; all.amp.vel2sustain = -30.f; all.amp.vel2release = 1.5f;
    for (unsigned k = 0; k < kMaxVelPoints; ++k) all.amp.curve[k] = {std::uint8_t(4 * k), 0.5f};
    all.amp.curve_count = kMaxVelPoints;
    const std::vector<Region> a = {plain, veltrack, curve, all, ext_region(0)};
    BundleWriter w;
    const std::size_t i0 = w.add_instrument("A", "a.sfz").value(), i1 = w.add_instrument("B", "b.sfz").value();
    CHECK_ERR(w.add_sample(i0, {"s0", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(w.add_sample(i1, {"s0", f, 1000, pcm}), Error::kNone);
    for (const auto& r : a) CHECK_ERR(w.add_region(i0, r), Error::kNone);
    CHECK_ERR(w.add_region(i1, plain), Error::kNone);  // an instrument without amp records still carries `u32 0`
    const Bytes b = w.finalize().take();
    CHECK(version_word(b) == (2u | (3u << 16)));
    auto r = BundleReader::open_memory(b);
    CHECK(r.ok() && r.value().format_minor() == 3);
    if (r.ok()) {
      CHECK(r.value().instruments()[0].regions == a);
      CHECK(r.value().instruments()[1].regions.size() == 1 && r.value().instruments()[1].regions[0] == plain);
      CHECK(r.value().verify_all() == Error::kNone);
    }
    CHECK(w.finalize().take() == b);  // deterministic
    // N-1 writer (minor ceiling 2) refuses amp regions.
    BundleWriter w2(WriterOptions{kFormatVersion, 2});
    const std::size_t j0 = w2.add_instrument("A", "a.sfz").value();
    CHECK_ERR(w2.add_sample(j0, {"s0", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(w2.add_region(j0, veltrack), Error::kNone);
    CHECK_ERR(w2.finalize().error(), Error::kInvalidArgument);
    // Content without amp fields keeps its exact bytes whatever the writer's ceiling: existing bundles do not change.
    {
      Region trig = plain; trig.trigger = TriggerMode::kReleaseKey;
      for (const Region& rg : {plain, sample_region(0), ext_region(0), trig}) {
        Bytes by[4];
        for (std::uint32_t m = 0; m < 4; ++m) {
          if ((m == 0 && has_extension(rg)) || (m < 2 && has_trigger_extension(rg))) continue;
          BundleWriter x(WriterOptions{kFormatVersion, m});
          const std::size_t k = x.add_instrument("A", "a.sfz").value();
          CHECK_ERR(x.add_sample(k, {"s0", f, 1000, pcm}), Error::kNone);
          CHECK_ERR(x.add_region(k, rg), Error::kNone);
          by[m] = x.finalize().take();
        }
        for (std::uint32_t m = 0; m < 3; ++m) CHECK(by[3] == by[m] || by[m].empty());
        const std::uint32_t want = has_trigger_extension(rg) ? 2u : has_extension(rg) ? 1u : 0u;
        CHECK(version_word(by[3]) == (2u | (want << 16)));
      }
    }
    // N-1 reader view: a minor 2 bundle still opens (kFormatMinor is 3).
    {
      BundleWriter x;
      const std::size_t k = x.add_instrument("A", "a.sfz").value();
      CHECK_ERR(x.add_sample(k, {"s0", f, 1000, pcm}), Error::kNone);
      Region trig = plain; trig.trigger = TriggerMode::kFirst;
      CHECK_ERR(x.add_region(k, trig), Error::kNone);
      auto rr = BundleReader::open_memory(x.finalize().take());
      CHECK(rr.ok() && rr.value().format_minor() == 2);
    }
    // Writer-side validation.
    BundleWriter v;
    const std::size_t vi = v.add_instrument("A", "a.sfz").value();
    CHECK_ERR(v.add_sample(vi, {"s", f, 1000, pcm}), Error::kNone);
    auto bad = [&](auto mutate) { Region x = plain; mutate(x); return v.add_region(vi, x); };
    CHECK_ERR(bad([](Region& x) { x.amp.veltrack = 101.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.amp.amplitude = -1.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.amp.keytrack = 13.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.amp.keycenter = 128; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.amp.vel2release = 100.5f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.amp.vel2sustain = std::nanf(""); }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.amp.curve_count = 1; x.amp.curve[0] = {5, 1.5f}; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.amp.curve_count = 2; x.amp.curve[0] = {9, 0.5f}; x.amp.curve[1] = {9, 0.6f}; }), Error::kInvalidArgument);  // not ascending
    CHECK_ERR(bad([](Region& x) { x.amp.curve_count = 1; x.amp.curve[0] = {128, 0.5f}; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.amp.curve[3] = {7, 0.5f}; }), Error::kInvalidArgument);  // an entry past curve_count must be zero
    CHECK_ERR(bad([](Region& x) { x.amp.curve_count = kMaxVelPoints + 1; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.amp.vel2decay = -0.0f; }), Error::kInvalidArgument);  // -0.0 would be a second encoding of the default
    CHECK_ERR(bad([](Region& x) { x.amp.veltrack = 50.f; x.amp.keytrack = -0.0f; }), Error::kInvalidArgument);
    // Corruption of the amp table (the last instrument's amp table is the tail of the manifest).
    BundleWriter u;
    const std::size_t ui = u.add_instrument("A", "a.sfz").value();
    CHECK_ERR(u.add_sample(ui, {"s", f, 1000, pcm}), Error::kNone);
    Region cv = plain;
    cv.amp.veltrack = 50.f; cv.amp.amplitude = 120.f; cv.amp.keytrack = 0.25f; cv.amp.keycenter = 61;
    cv.amp.vel2attack = 0.5f; cv.amp.vel2decay = 0.25f; cv.amp.vel2sustain = -10.f; cv.amp.vel2release = 0.125f;
    cv.amp.curve_count = 2; cv.amp.curve[0] = {10, 0.5f}; cv.amp.curve[1] = {100, 0.75f};
    CHECK_ERR(u.add_region(ui, plain), Error::kNone);
    CHECK_ERR(u.add_region(ui, cv), Error::kNone);
    const Bytes good = u.finalize().take();
    const std::size_t end = rd64(good, 8) + rd64(good, 16);
    const std::size_t rec = end - (layout::kAmpRecordFixedSize + 2 * layout::kAmpPointSize);
    CHECK(layout::kAmpRecordFixedSize == 36 && layout::kAmpPointSize == 8);
    CHECK(layout::get_u32(good.data() + rec - 4) == 1u && layout::get_u32(good.data() + rec) == 1u);  // n_amp, region index 1
    auto f32at = [&](const Bytes& by, std::size_t at) { std::uint32_t u32 = layout::get_u32(by.data() + at); float v; std::memcpy(&v, &u32, 4); return v; };
    CHECK(f32at(good, rec + 4) == 50.f && f32at(good, rec + 8) == 120.f && f32at(good, rec + 12) == 0.25f);
    CHECK(good[rec + 16] == 61 && good[rec + 17] == 2 && good[rec + 18] == 0 && good[rec + 19] == 0);
    CHECK(f32at(good, rec + 20) == 0.5f && f32at(good, rec + 24) == 0.25f && f32at(good, rec + 28) == -10.f && f32at(good, rec + 32) == 0.125f);
    CHECK(good[rec + 36] == 10 && f32at(good, rec + 40) == 0.5f && good[rec + 44] == 100 && f32at(good, rec + 48) == 0.75f);
    auto put_f32 = [](Bytes& by, std::size_t at, float v) { std::uint32_t u32; std::memcpy(&u32, &v, 4); wr32(by, at, u32); };
    auto expect_bad = [&](Bytes bad_bytes, const char* what) {
      rehash(bad_bytes);
      auto rb = BundleReader::open_memory(bad_bytes);
      if (rb.ok() || rb.error() != Error::kCorruptManifest) { std::printf("FAIL amp table corruption not rejected: %s\n", what); ++g_fail; }
    };
    { Bytes b = good; put_f32(b, rec + 4, 150.f); expect_bad(b, "veltrack above 100"); }
    { Bytes b = good; put_f32(b, rec + 8, -1.f); expect_bad(b, "negative amplitude"); }
    { Bytes b = good; put_f32(b, rec + 12, 20.f); expect_bad(b, "keytrack above 12"); }
    { Bytes b = good; b[rec + 16] = 200; expect_bad(b, "keycenter above 127"); }
    { Bytes b = good; b[rec + 18] = 1; expect_bad(b, "non-zero padding"); }
    { Bytes b = good; b[rec + 17] = 33; expect_bad(b, "more curve points than the bound"); }
    { Bytes b = good; b[rec + 17] = 3; expect_bad(b, "curve count beyond the record (reads into nothing)"); }
    { Bytes b = good; b[rec + 17] = 1; expect_bad(b, "curve count smaller than the data (non-canonical tail)"); }
    { Bytes b = good; b[rec + 44] = 10; expect_bad(b, "curve points not ascending"); }
    { Bytes b = good; b[rec + 37] = 1; expect_bad(b, "point padding"); }
    { Bytes b = good; put_f32(b, rec + 48, 2.f); expect_bad(b, "curve gain above 1"); }
    { Bytes b = good; put_f32(b, rec + 20, std::nanf("")); expect_bad(b, "NaN vel2attack"); }
    { Bytes b = good; put_f32(b, rec + 24, -0.0f); put_f32(b, rec + 20, 0.0f); expect_bad(b, "negative zero vel2decay"); }
    { Bytes b = good; wr32(b, rec, 5); expect_bad(b, "region index out of range"); }
    { Bytes b = good; wr32(b, rec - 4, 3); expect_bad(b, "n_amp larger than regions"); }
    {  // an all-default record is non-canonical
      Bytes b = good;
      put_f32(b, rec + 4, 100.f); put_f32(b, rec + 8, 100.f); put_f32(b, rec + 12, 0.f); b[rec + 16] = 60; b[rec + 17] = 0;
      for (std::size_t o = rec + 20; o < rec + 36; ++o) b[o] = 0;
      b.erase(b.begin() + rec + 36, b.begin() + end);  // the two points are gone: shrink the manifest
      const std::size_t mo = rd64(b, 8), ms = rd64(b, 16), io = rd64(b, 24), is = rd64(b, 32);
      const std::size_t cut = 2 * layout::kAmpPointSize;
      wr64(b, 16, ms - cut); wr64(b, 24, io - cut); wr64(b, 40, rd64(b, 40) - cut); wr64(b, 56, rd64(b, 56) - cut);
      for (std::size_t e = 0; e < is / 48; ++e) wr64(b, io - cut + e * 48, rd64(b, io - cut + e * 48) - cut);
      (void)mo;
      expect_bad(b, "all-default amp record");
    }
    for (std::size_t n = 0; n < good.size(); n += (n < 500 ? 1 : 17))
      CHECK(!BundleReader::open_memory(std::span<const std::uint8_t>(good.data(), n)).ok());
    for (std::size_t off = rec - 4; off < end; ++off)
      for (int bit = 0; bit < 8; ++bit) {
        Bytes bad_bytes = good;
        bad_bytes[off] ^= std::uint8_t(1 << bit);
        rehash(bad_bytes);
        auto r3 = BundleReader::open_memory(bad_bytes);
        if (!r3.ok() && r3.error() != Error::kCorruptManifest) { std::printf("FAIL amp flip at %zu bit %d -> %s\n", off, bit, to_string(r3.error())); ++g_fail; }
      }
    // A minor 3 stamp on a bundle with no amp record is non-canonical.
    {
      BundleWriter x;
      const std::size_t k = x.add_instrument("A", "a.sfz").value();
      CHECK_ERR(x.add_sample(k, {"s", f, 1000, pcm}), Error::kNone);
      Region trig = plain; trig.trigger = TriggerMode::kFirst;
      CHECK_ERR(x.add_region(k, trig), Error::kNone);
      Bytes b = x.finalize().take();  // minor 2
      const std::size_t mo = rd64(b, 8), ms = rd64(b, 16), io = rd64(b, 24), is = rd64(b, 32);
      b.insert(b.begin() + mo + ms, 4, std::uint8_t(0));  // n_amp = 0
      wr64(b, 16, ms + 4); wr64(b, 24, io + 4); wr64(b, 40, rd64(b, 40) + 4); wr64(b, 56, rd64(b, 56) + 4);
      for (std::size_t e = 0; e < is / 48; ++e) wr64(b, io + 4 + e * 48, rd64(b, io + 4 + e * 48) + 4);
      wr32(b, 4, 2u | (3u << 16));
      wr32(b, mo, 2u | (3u << 16));
      rehash(b);
      CHECK_ERR(BundleReader::open_memory(b).error(), Error::kCorruptManifest);
    }
  }
}

// Story 13.5: format 2.4, the cc table (CC gates, CC modulation entries) and the instrument CC setup (set_cc defaults, <curve> tables).
void test_cc_table() {
  const PcmFormat f{2, 16, false, 48000};
  const auto pcm = vt::make_int(f, 1000, Kind::kMixed);
  auto version_word = [](const Bytes& b) { return layout::get_u32(b.data() + 4); };
  Region plain;
  Region gated = plain;
  gated.cc.gate_count = 2;
  gated.cc.gates[0] = {64, 64.f, 100.f};
  gated.cc.gates[1] = {70, 0.f, std::nextafter(11.f, 0.f)};
  Region modded = sample_region(0);
  modded.cc.mod_count = 4;
  modded.cc.mods[0] = {CcTarget::kAmplitude, 93, 7, 100.f};
  modded.cc.mods[1] = {CcTarget::kTune, 94, 0, -1200.f};
  modded.cc.mods[2] = {CcTarget::kOffset, 131, 0, 5000.f};
  modded.cc.mods[3] = {CcTarget::kEgRelease, 137, 9, 2.5f};
  Region all = ext_region(0);  // minors 1, 2, 3 and 4 content in one region
  all.trigger = TriggerMode::kLegato; all.has_end = true; all.end = 900; all.amp.veltrack = 40.f;
  all.cc = modded.cc; all.cc.gate_count = 1; all.cc.gates[0] = {1, 10.f, 20.f};
  const std::vector<Region> regs = {plain, gated, modded, all};
  InstrumentCcSetup setup;
  setup.defaults = {{64, 90.f}, {102, 127.f}};
  setup.curves = {{7, {{0, 0.f}, {64, 0.9f}, {127, 1.f}}}, {9, {{10, -0.5f}}}, {200, {}}};
  {  // round trip: two instruments, only the first has a setup; the second still carries the empty tables
    BundleWriter w;
    const std::size_t i0 = w.add_instrument("A", "a.sfz").value(), i1 = w.add_instrument("B", "b.sfz").value();
    CHECK_ERR(w.add_sample(i0, {"s0", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(w.add_sample(i1, {"s0", f, 1000, pcm}), Error::kNone);
    for (const auto& r : regs) CHECK_ERR(w.add_region(i0, r), Error::kNone);
    CHECK_ERR(w.add_region(i1, plain), Error::kNone);
    CHECK_ERR(w.set_instrument_cc_setup(i0, setup), Error::kNone);
    const Bytes b = w.finalize().take();
    CHECK(version_word(b) == (2u | (4u << 16)));
    auto r = BundleReader::open_memory(b);
    CHECK(r.ok() && r.value().format_minor() == 4);
    if (r.ok()) {
      CHECK(r.value().instruments()[0].regions == regs);
      CHECK(r.value().instruments()[0].cc_setup == setup);
      CHECK(r.value().instruments()[1].regions.size() == 1 && !r.value().instruments()[1].cc_setup.any());
      CHECK(r.value().verify_all() == Error::kNone);
    }
    CHECK(w.finalize().take() == b);  // deterministic
    // The N-1 writer (minor ceiling 3) refuses cc regions and a setup.
    BundleWriter w3(WriterOptions{kFormatVersion, 3});
    const std::size_t j0 = w3.add_instrument("A", "a.sfz").value();
    CHECK_ERR(w3.add_sample(j0, {"s0", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(w3.add_region(j0, gated), Error::kNone);
    CHECK_ERR(w3.finalize().error(), Error::kInvalidArgument);
    BundleWriter w3b(WriterOptions{kFormatVersion, 3});
    const std::size_t j1 = w3b.add_instrument("A", "a.sfz").value();
    CHECK_ERR(w3b.add_sample(j1, {"s0", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(w3b.add_region(j1, plain), Error::kNone);
    CHECK_ERR(w3b.set_instrument_cc_setup(j1, setup), Error::kNone);
    CHECK_ERR(w3b.finalize().error(), Error::kInvalidArgument);
  }
  {  // a setup alone (no cc region) stamps minor 4; an empty setup stamps nothing
    BundleWriter w;
    const std::size_t i0 = w.add_instrument("A", "a.sfz").value();
    CHECK_ERR(w.add_sample(i0, {"s0", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(w.add_region(i0, plain), Error::kNone);
    const Bytes none = w.finalize().take();
    CHECK(version_word(none) == 2u);
    CHECK_ERR(w.set_instrument_cc_setup(i0, InstrumentCcSetup{}), Error::kNone);
    CHECK(w.finalize().take() == none);
    InstrumentCcSetup only;
    only.defaults = {{1, 5.5f}};
    CHECK_ERR(w.set_instrument_cc_setup(i0, only), Error::kNone);
    const Bytes b = w.finalize().take();
    CHECK(version_word(b) == (2u | (4u << 16)));
    auto r = BundleReader::open_memory(b);
    CHECK(r.ok() && r.value().instruments()[0].cc_setup == only);
  }
  {  // bundles without the new fields keep their exact bytes whatever the writer's ceiling
    Region trig = plain; trig.trigger = TriggerMode::kReleaseKey;
    Region amp = plain; amp.amp.veltrack = 10.f;
    for (const Region& rg : {plain, sample_region(0), ext_region(0), trig, amp}) {
      Bytes by[5];
      for (std::uint32_t m = 0; m < 5; ++m) {
        if ((m == 0 && has_extension(rg)) || (m < 2 && has_trigger_extension(rg)) || (m < 3 && has_amp_extension(rg))) continue;
        BundleWriter x(WriterOptions{kFormatVersion, m});
        const std::size_t k = x.add_instrument("A", "a.sfz").value();
        CHECK_ERR(x.add_sample(k, {"s0", f, 1000, pcm}), Error::kNone);
        CHECK_ERR(x.add_region(k, rg), Error::kNone);
        by[m] = x.finalize().take();
      }
      for (std::uint32_t m = 0; m < 4; ++m) CHECK(by[4] == by[m] || by[m].empty());
      const std::uint32_t want = has_amp_extension(rg) ? 3u : has_trigger_extension(rg) ? 2u : has_extension(rg) ? 1u : 0u;
      CHECK(version_word(by[4]) == (2u | (want << 16)));
    }
  }
  // Writer-side validation.
  {
    BundleWriter v;
    const std::size_t vi = v.add_instrument("A", "a.sfz").value();
    CHECK_ERR(v.add_sample(vi, {"s", f, 1000, pcm}), Error::kNone);
    auto bad = [&](auto mutate) { Region x = gated; x.cc.mod_count = 1; x.cc.mods[0] = {CcTarget::kPan, 7, 0, 10.f}; mutate(x); return v.add_region(vi, x); };
    CHECK_ERR(bad([](Region&) {}), Error::kNone);
    CHECK_ERR(bad([](Region& x) { x.cc.gates[0].cc = 128; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.cc.gates[1].cc = 64; }), Error::kInvalidArgument);          // not ascending
    CHECK_ERR(bad([](Region& x) { x.cc.gates[0].hi = 128.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.cc.gates[0].lo = -1.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.cc.gates[0].lo = std::nanf(""); }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.cc.gates[0].lo = 101.f; }), Error::kInvalidArgument);        // lo > hi: an inverted gate
    CHECK_ERR(bad([](Region& x) { x.cc.gates[0].lo = -0.f; }), Error::kInvalidArgument);        // -0.0 would be a second encoding
    CHECK_ERR(bad([](Region& x) { x.cc.gates[3] = {9, 1.f, 2.f}; }), Error::kInvalidArgument);  // unused entry not default
    CHECK_ERR(bad([](Region& x) { x.cc.gate_count = kMaxCcGates + 1; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.cc.mods[0].cc = 128; }), Error::kInvalidArgument);          // pitch bend is not a modulation source
    CHECK_ERR(bad([](Region& x) { x.cc.mods[0].cc = 133; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.cc.mods[0].cc = 141; }), Error::kNone);
    CHECK_ERR(bad([](Region& x) { x.cc.mods[0].target = static_cast<CcTarget>(12); }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.cc.mods[0].depth = 201.f; }), Error::kInvalidArgument);     // pan range
    CHECK_ERR(bad([](Region& x) { x.cc.mods[0].target = CcTarget::kAmplitude; x.cc.mods[0].depth = -1.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.cc.mods[0].target = CcTarget::kEgAttack; x.cc.mods[0].depth = 101.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.cc.mods[0].depth = std::nanf(""); }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.cc.mod_count = 2; x.cc.mods[1] = x.cc.mods[0]; }), Error::kInvalidArgument);  // duplicate (target, cc)
    CHECK_ERR(bad([](Region& x) { x.cc.mod_count = 2; x.cc.mods[1] = {CcTarget::kAmplitude, 1, 0, 1.f}; }), Error::kInvalidArgument);  // not ascending by target
    CHECK_ERR(bad([](Region& x) { x.cc.mod_count = kMaxCcMods + 1; }), Error::kInvalidArgument);
    // setups
    auto set_bad = [&](auto mutate) { InstrumentCcSetup s = setup; mutate(s); return v.set_instrument_cc_setup(vi, s); };
    CHECK_ERR(set_bad([](InstrumentCcSetup&) {}), Error::kNone);
    CHECK_ERR(set_bad([](InstrumentCcSetup& s) { s.defaults[0].cc = 128; }), Error::kInvalidArgument);
    CHECK_ERR(set_bad([](InstrumentCcSetup& s) { s.defaults[1].cc = 64; }), Error::kInvalidArgument);
    CHECK_ERR(set_bad([](InstrumentCcSetup& s) { s.defaults[0].value = 128.f; }), Error::kInvalidArgument);
    CHECK_ERR(set_bad([](InstrumentCcSetup& s) { s.curves[1].index = 7; }), Error::kInvalidArgument);  // not ascending
    CHECK_ERR(set_bad([](InstrumentCcSetup& s) { s.curves[0].points[1].x = 0; }), Error::kInvalidArgument);
    CHECK_ERR(set_bad([](InstrumentCcSetup& s) { s.curves[0].points[2].x = 128; }), Error::kInvalidArgument);
    CHECK_ERR(set_bad([](InstrumentCcSetup& s) { s.curves[0].points[0].y = std::nanf(""); }), Error::kInvalidArgument);
    CHECK_ERR(set_bad([](InstrumentCcSetup& s) { s.curves[0].points.assign(129, CurvePoint{}); }), Error::kInvalidArgument);
    CHECK_ERR(v.set_instrument_cc_setup(99, setup), Error::kInvalidArgument);
  }
  // Corruption of the cc table (the tail of the manifest: the last region record, then the defaults and the curves).
  BundleWriter u;
  const std::size_t ui = u.add_instrument("A", "a.sfz").value();
  CHECK_ERR(u.add_sample(ui, {"s", f, 1000, pcm}), Error::kNone);
  Region g1 = plain;
  g1.cc.gate_count = 1; g1.cc.gates[0] = {64, 64.f, 100.f};
  g1.cc.mod_count = 1; g1.cc.mods[0] = {CcTarget::kPan, 7, 3, 25.f};
  InstrumentCcSetup su;
  su.defaults = {{64, 90.f}};
  su.curves = {{3, {{0, 0.f}, {127, 1.f}}}};
  CHECK_ERR(u.add_region(ui, plain), Error::kNone);
  CHECK_ERR(u.add_region(ui, g1), Error::kNone);
  CHECK_ERR(u.set_instrument_cc_setup(ui, su), Error::kNone);
  const Bytes good = u.finalize().take();
  const std::size_t end = rd64(good, 8) + rd64(good, 16);
  const std::size_t setup_size = 4 + layout::kCcDefaultSize + 4 + layout::kCcCurveFixedSize + 2 * layout::kCcPointSize;
  const std::size_t rec = end - setup_size - (layout::kCcRecordFixedSize + layout::kCcGateSize + layout::kCcModSize);
  CHECK(layout::kCcRecordFixedSize == 8 && layout::kCcGateSize == 12 && layout::kCcModSize == 8 && layout::kCcDefaultSize == 8);
  CHECK(layout::get_u32(good.data() + rec - 4) == 1u && layout::get_u32(good.data() + rec) == 1u);  // n_cc, region index 1
  CHECK(good[rec + 4] == 1 && good[rec + 5] == 1 && good[rec + 6] == 0 && good[rec + 7] == 0);
  auto put_f32 = [](Bytes& by, std::size_t at, float v) { std::uint32_t u32; std::memcpy(&u32, &v, 4); wr32(by, at, u32); };
  auto expect_bad = [&](Bytes bad_bytes, const char* what) {
    rehash(bad_bytes);
    auto rb = BundleReader::open_memory(bad_bytes);
    if (rb.ok() || rb.error() != Error::kCorruptManifest) { std::printf("FAIL cc table corruption not rejected: %s\n", what); ++g_fail; }
  };
  const std::size_t gate0 = rec + 8, mod0 = gate0 + 12, def0 = end - setup_size + 4, cur0 = def0 + 8 + 4;
  { Bytes b = good; b[gate0] = 200; expect_bad(b, "gate controller above 127"); }
  { Bytes b = good; put_f32(b, gate0 + 4, 200.f); expect_bad(b, "gate lo above 127"); }
  { Bytes b = good; put_f32(b, gate0 + 8, std::nanf("")); expect_bad(b, "NaN gate hi"); }
  { Bytes b = good; b[gate0 + 1] = 1; expect_bad(b, "gate padding"); }
  { Bytes b = good; b[rec + 4] = 9; expect_bad(b, "gate count above the bound"); }
  { Bytes b = good; b[rec + 5] = 17; expect_bad(b, "mod count above the bound"); }
  { Bytes b = good; b[rec + 6] = 1; expect_bad(b, "record padding"); }
  { Bytes b = good; b[mod0] = 12; expect_bad(b, "target above 11"); }
  { Bytes b = good; b[mod0 + 1] = 133; expect_bad(b, "unsupported modulation source"); }
  { Bytes b = good; b[mod0 + 3] = 1; expect_bad(b, "modulation padding"); }
  { Bytes b = good; put_f32(b, mod0 + 4, 500.f); expect_bad(b, "depth beyond the target's range"); }
  { Bytes b = good; wr32(b, rec, 5); expect_bad(b, "region index out of range"); }
  { Bytes b = good; wr32(b, rec - 4, 3); expect_bad(b, "n_cc larger than regions"); }
  { Bytes b = good; b[def0] = 200; expect_bad(b, "default controller above 127"); }
  { Bytes b = good; put_f32(b, def0 + 4, 300.f); expect_bad(b, "default value above 127"); }
  { Bytes b = good; wr32(b, def0 - 4, 200); expect_bad(b, "n_defaults beyond the bound"); }
  { Bytes b = good; b[cur0 + 1] = 1; expect_bad(b, "curve padding"); }
  { Bytes b = good; b[cur0 + 2] = 9; expect_bad(b, "curve point count beyond the record"); }
  { Bytes b = good; b[cur0 + 4 + 8] = 0; expect_bad(b, "curve points not ascending"); }
  { Bytes b = good; put_f32(b, cur0 + 4 + 4, std::nanf("")); expect_bad(b, "NaN curve value"); }
  {  // a record for a region with no gate or entry is non-canonical
    Bytes b = good;
    b[rec + 4] = 0; b[rec + 5] = 0;
    b.erase(b.begin() + rec + 8, b.begin() + rec + 8 + layout::kCcGateSize + layout::kCcModSize);
    const std::size_t cut = layout::kCcGateSize + layout::kCcModSize;
    const std::size_t io = rd64(b, 24), is = rd64(b, 32);
    wr64(b, 16, rd64(b, 16) - cut); wr64(b, 24, io - cut); wr64(b, 40, rd64(b, 40) - cut); wr64(b, 56, rd64(b, 56) - cut);
    for (std::size_t e = 0; e < is / 48; ++e) wr64(b, io - cut + e * 48, rd64(b, io - cut + e * 48) - cut);
    expect_bad(b, "empty cc record");
  }
  for (std::size_t n = 0; n < good.size(); n += (n < 500 ? 1 : 17))
    CHECK(!BundleReader::open_memory(std::span<const std::uint8_t>(good.data(), n)).ok());
  for (std::size_t off = rec - 4; off < end; ++off)
    for (int bit = 0; bit < 8; ++bit) {
      Bytes bad_bytes = good;
      bad_bytes[off] ^= std::uint8_t(1 << bit);
      rehash(bad_bytes);
      auto r3 = BundleReader::open_memory(bad_bytes);
      if (!r3.ok() && r3.error() != Error::kCorruptManifest) { std::printf("FAIL cc flip at %zu bit %d -> %s\n", off, bit, to_string(r3.error())); ++g_fail; }
    }
  // A minor 4 stamp on a bundle with no cc record, default or curve is non-canonical; so is a newer minor.
  {
    BundleWriter x;
    const std::size_t k = x.add_instrument("A", "a.sfz").value();
    CHECK_ERR(x.add_sample(k, {"s", f, 1000, pcm}), Error::kNone);
    Region a = plain; a.amp.veltrack = 10.f;
    CHECK_ERR(x.add_region(k, a), Error::kNone);
    Bytes b = x.finalize().take();  // minor 3
    const std::size_t mo = rd64(b, 8), ms = rd64(b, 16), io = rd64(b, 24), is = rd64(b, 32);
    b.insert(b.begin() + mo + ms, 12, std::uint8_t(0));  // n_cc = 0, n_defaults = 0, n_curves = 0
    wr64(b, 16, ms + 12); wr64(b, 24, io + 12); wr64(b, 40, rd64(b, 40) + 12); wr64(b, 56, rd64(b, 56) + 12);
    for (std::size_t e = 0; e < is / 48; ++e) wr64(b, io + 12 + e * 48, rd64(b, io + 12 + e * 48) + 12);
    wr32(b, 4, 2u | (4u << 16));
    wr32(b, mo, 2u | (4u << 16));
    rehash(b);
    CHECK_ERR(BundleReader::open_memory(b).error(), Error::kCorruptManifest);
    wr32(b, 4, 2u | ((kFormatMinor + 1) << 16));
    CHECK_ERR(BundleReader::open_memory(b).error(), Error::kUnsupportedVersion);
  }
}

// Story 13.6: format 2.5, the voice-group table (group, off_by, off_mode / off_time, polyphony limits).
void test_vg_table() {
  const PcmFormat f{2, 16, false, 48000};
  const auto pcm = vt::make_int(f, 1000, Kind::kMixed);
  auto version_word = [](const Bytes& b) { return layout::get_u32(b.data() + 4); };
  Region plain;
  Region choke = plain;
  choke.vg.group = 2; choke.vg.has_off_by = true; choke.vg.off_by = 1; choke.vg.off_mode = OffMode::kNormal;
  Region timed = sample_region(0);
  timed.vg.group = -7; timed.vg.has_off_by = true; timed.vg.off_by = -7; timed.vg.off_mode = OffMode::kTime; timed.vg.off_time = 0.25f;
  timed.vg.polyphony = 3; timed.vg.group_polyphony = 255; timed.vg.note_polyphony = 1; timed.vg.note_selfmask = false;
  Region all = ext_region(0);  // minors 1..5 content in one region
  all.trigger = TriggerMode::kLegato; all.has_end = true; all.end = 900; all.amp.veltrack = 40.f;
  all.cc.gate_count = 1; all.cc.gates[0] = {1, 10.f, 20.f};
  all.vg = timed.vg;
  Region mono = plain;
  mono.vg.polyphony = 1;
  const std::vector<Region> regs = {plain, choke, timed, all, mono};
  {  // round trip: two instruments, only the first has records; the second still carries the empty table
    BundleWriter w;
    const std::size_t i0 = w.add_instrument("A", "a.sfz").value(), i1 = w.add_instrument("B", "b.sfz").value();
    CHECK_ERR(w.add_sample(i0, {"s0", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(w.add_sample(i1, {"s0", f, 1000, pcm}), Error::kNone);
    for (const auto& r : regs) CHECK_ERR(w.add_region(i0, r), Error::kNone);
    CHECK_ERR(w.add_region(i1, plain), Error::kNone);
    const Bytes b = w.finalize().take();
    CHECK(version_word(b) == (2u | (5u << 16)));
    auto r = BundleReader::open_memory(b);
    CHECK(r.ok() && r.value().format_minor() == 5);
    if (r.ok()) {
      CHECK(r.value().instruments()[0].regions == regs);
      CHECK(r.value().instruments()[1].regions.size() == 1 && r.value().instruments()[1].regions[0] == plain);
      CHECK(r.value().verify_all() == Error::kNone);
    }
    CHECK(w.finalize().take() == b);  // deterministic
    // The N-1 writer (minor ceiling 4) refuses voice-group regions.
    BundleWriter w4(WriterOptions{kFormatVersion, 4});
    const std::size_t j0 = w4.add_instrument("A", "a.sfz").value();
    CHECK_ERR(w4.add_sample(j0, {"s0", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(w4.add_region(j0, choke), Error::kNone);
    CHECK_ERR(w4.finalize().error(), Error::kInvalidArgument);
  }
  {  // bundles without voice groups keep their exact bytes whatever the writer's ceiling (minor 0..4 content)
    Region trig = plain; trig.trigger = TriggerMode::kReleaseKey;
    Region amp = plain; amp.amp.veltrack = 10.f;
    Region ccr = plain; ccr.cc.gate_count = 1; ccr.cc.gates[0] = {64, 64.f, 100.f};
    for (const Region& rg : {plain, sample_region(0), ext_region(0), trig, amp, ccr}) {
      Bytes by[6];
      for (std::uint32_t m = 0; m < 6; ++m) {
        if ((m == 0 && has_extension(rg)) || (m < 2 && has_trigger_extension(rg)) || (m < 3 && has_amp_extension(rg)) || (m < 4 && has_cc_extension(rg))) continue;
        BundleWriter x(WriterOptions{kFormatVersion, m});
        const std::size_t k = x.add_instrument("A", "a.sfz").value();
        CHECK_ERR(x.add_sample(k, {"s0", f, 1000, pcm}), Error::kNone);
        CHECK_ERR(x.add_region(k, rg), Error::kNone);
        by[m] = x.finalize().take();
      }
      for (std::uint32_t m = 0; m < 5; ++m) CHECK(by[5] == by[m] || by[m].empty());
      const std::uint32_t want = has_cc_extension(rg) ? 4u : has_amp_extension(rg) ? 3u : has_trigger_extension(rg) ? 2u : has_extension(rg) ? 1u : 0u;
      CHECK(version_word(by[5]) == (2u | (want << 16)));
    }
  }
  // Writer-side validation.
  {
    BundleWriter v;
    const std::size_t vi = v.add_instrument("A", "a.sfz").value();
    CHECK_ERR(v.add_sample(vi, {"s", f, 1000, pcm}), Error::kNone);
    auto bad = [&](auto mutate) { Region x = timed; mutate(x); return v.add_region(vi, x); };
    CHECK_ERR(bad([](Region&) {}), Error::kNone);
    CHECK_ERR(bad([](Region& x) { x.vg.has_off_by = false; }), Error::kInvalidArgument);               // off_by without the flag: two encodings
    CHECK_ERR(bad([](Region& x) { x.vg.off_mode = OffMode::kFast; }), Error::kInvalidArgument);        // off_time without off_mode time
    CHECK_ERR(bad([](Region& x) { x.vg.off_mode = static_cast<OffMode>(3); }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.vg.off_time = 100.5f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.vg.off_time = -0.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.vg.off_time = std::nanf(""); }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.vg.polyphony = 256; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.vg.group_polyphony = 256; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.vg.note_polyphony = 1000; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.vg.group = 0; }), Error::kInvalidArgument);  // a group limit needs a group
    CHECK_ERR(bad([](Region& x) { x.vg.group = -2147483647 - 1; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.vg.off_mode = OffMode::kTime; x.vg.off_time = 0.f; }), Error::kNone);  // time with 0 s is a legal instant cut
  }
  // Corruption of the table (the tail of the manifest: one record).
  BundleWriter u;
  const std::size_t ui = u.add_instrument("A", "a.sfz").value();
  CHECK_ERR(u.add_sample(ui, {"s", f, 1000, pcm}), Error::kNone);
  CHECK_ERR(u.add_region(ui, plain), Error::kNone);
  CHECK_ERR(u.add_region(ui, timed), Error::kNone);
  const Bytes good = u.finalize().take();
  const std::size_t end = rd64(good, 8) + rd64(good, 16);
  const std::size_t rec = end - layout::kVgRecordSize;
  CHECK(layout::kVgRecordSize == 28);
  CHECK(layout::get_u32(good.data() + rec - 4) == 1u && layout::get_u32(good.data() + rec) == 1u);  // n_vg, region index 1
  auto put_f32 = [](Bytes& by, std::size_t at, float v) { std::uint32_t u32; std::memcpy(&u32, &v, 4); wr32(by, at, u32); };
  auto expect_bad = [&](Bytes bad_bytes, const char* what) {
    rehash(bad_bytes);
    auto rb = BundleReader::open_memory(bad_bytes);
    if (rb.ok() || rb.error() != Error::kCorruptManifest) { std::printf("FAIL vg table corruption not rejected: %s\n", what); ++g_fail; }
  };
  { Bytes b = good; b[rec + 12] = 4; expect_bad(b, "unknown flag bit"); }
  { Bytes b = good; b[rec + 13] = 3; expect_bad(b, "unknown off_mode"); }
  { Bytes b = good; b[rec + 14] = 1; expect_bad(b, "padding"); }
  { Bytes b = good; b[rec + 12] = 0; expect_bad(b, "off_by without its flag"); }
  { Bytes b = good; b[rec + 13] = 0; expect_bad(b, "off_time without off_mode time"); }
  { Bytes b = good; put_f32(b, rec + 16, 200.f); expect_bad(b, "off_time above 100"); }
  { Bytes b = good; put_f32(b, rec + 16, std::nanf("")); expect_bad(b, "NaN off_time"); }
  { Bytes b = good; b[rec + 20] = 0; b[rec + 21] = 1; expect_bad(b, "polyphony above 255"); }
  { Bytes b = good; b[rec + 26] = 1; expect_bad(b, "trailing padding"); }
  { Bytes b = good; wr32(b, rec + 4, 0); expect_bad(b, "group_polyphony with group 0"); }
  { Bytes b = good; wr32(b, rec, 5); expect_bad(b, "region index out of range"); }
  { Bytes b = good; wr32(b, rec - 4, 3); expect_bad(b, "n_vg larger than regions"); }
  for (std::size_t n = 0; n < good.size(); n += (n < 500 ? 1 : 17))
    CHECK(!BundleReader::open_memory(std::span<const std::uint8_t>(good.data(), n)).ok());
  for (std::size_t off = rec - 4; off < end; ++off)
    for (int bit = 0; bit < 8; ++bit) {
      Bytes bad_bytes = good;
      bad_bytes[off] ^= std::uint8_t(1 << bit);
      rehash(bad_bytes);
      auto r3 = BundleReader::open_memory(bad_bytes);
      if (!r3.ok() && r3.error() != Error::kCorruptManifest) { std::printf("FAIL vg flip at %zu bit %d -> %s\n", off, bit, to_string(r3.error())); ++g_fail; }
    }
  // A record that holds the default VoiceGroup is non-canonical.
  { Bytes b = good; for (std::size_t i = rec + 4; i < end; ++i) b[i] = 0; expect_bad(b, "record with the default values"); }
  // A minor 5 stamp on a bundle with no voice-group record is non-canonical; so is a newer minor.
  {
    BundleWriter x;
    const std::size_t k = x.add_instrument("A", "a.sfz").value();
    CHECK_ERR(x.add_sample(k, {"s", f, 1000, pcm}), Error::kNone);
    Region a = plain; a.cc.gate_count = 1; a.cc.gates[0] = {64, 64.f, 100.f};
    CHECK_ERR(x.add_region(k, a), Error::kNone);
    Bytes b = x.finalize().take();  // minor 4
    const std::size_t mo = rd64(b, 8), ms = rd64(b, 16), io = rd64(b, 24), is = rd64(b, 32);
    b.insert(b.begin() + mo + ms, 4, std::uint8_t(0));  // n_vg = 0
    wr64(b, 16, ms + 4); wr64(b, 24, io + 4); wr64(b, 40, rd64(b, 40) + 4); wr64(b, 56, rd64(b, 56) + 4);
    for (std::size_t e = 0; e < is / 48; ++e) wr64(b, io + 4 + e * 48, rd64(b, io + 4 + e * 48) + 4);
    wr32(b, 4, 2u | (5u << 16));
    wr32(b, mo, 2u | (5u << 16));
    rehash(b);
    CHECK_ERR(BundleReader::open_memory(b).error(), Error::kCorruptManifest);
    wr32(b, 4, 2u | ((kFormatMinor + 1) << 16));
    CHECK_ERR(BundleReader::open_memory(b).error(), Error::kUnsupportedVersion);
  }
}

// Story 13.7: format 2.6, the pitch-bend / controller-trigger table.
void test_bt_table() {
  const PcmFormat f{2, 16, false, 48000};
  const auto pcm = vt::make_int(f, 1000, Kind::kMixed);
  auto version_word = [](const Bytes& b) { return layout::get_u32(b.data() + 4); };
  Region plain;
  Region bend = plain;
  bend.bend.enabled = true; bend.bend.up = 1200.f; bend.bend.down = -300.5f; bend.bend.step = 25.f; bend.bend.smooth_ms = 40;
  Region only_up = plain;
  only_up.bend.enabled = true;  // every value at the sfizz default: still a record (the region follows the wheel)
  Region trig = sample_region(0);
  trig.trig.no_note = true; trig.trig.count = 2;
  trig.trig.t[0] = {20, 0.f, 127.f};
  trig.trig.t[1] = {64, 127.f, 127.f};
  Region note_and_cc = plain;  // a region with a key range that a controller also starts
  note_and_cc.trig.count = 1; note_and_cc.trig.t[0] = {1, 30.f, std::nextafter(61.f, 0.f)};
  Region all = ext_region(0);  // minors 1..5 content in one region
  all.trigger = TriggerMode::kLegato; all.has_end = true; all.end = 900; all.amp.veltrack = 40.f;
  all.cc.gate_count = 1; all.cc.gates[0] = {1, 10.f, 20.f};
  all.vg.group = 4; all.vg.polyphony = 1; all.vg.group_polyphony = 1;
  all.bend = bend.bend; all.trig = trig.trig;
  const std::vector<Region> regs = {plain, bend, only_up, trig, note_and_cc, all};
  {  // round trip: two instruments, only the first has records; the second still carries the empty table
    BundleWriter w;
    const std::size_t i0 = w.add_instrument("A", "a.sfz").value(), i1 = w.add_instrument("B", "b.sfz").value();
    CHECK_ERR(w.add_sample(i0, {"s0", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(w.add_sample(i1, {"s0", f, 1000, pcm}), Error::kNone);
    for (const auto& r : regs) CHECK_ERR(w.add_region(i0, r), Error::kNone);
    CHECK_ERR(w.add_region(i1, plain), Error::kNone);
    const Bytes b = w.finalize().take();
    CHECK(version_word(b) == (2u | (6u << 16)));
    auto r = BundleReader::open_memory(b);
    CHECK(r.ok() && r.value().format_minor() == 6);
    if (r.ok()) {
      CHECK(r.value().instruments()[0].regions == regs);
      CHECK(r.value().instruments()[1].regions.size() == 1 && r.value().instruments()[1].regions[0] == plain);
      CHECK(r.value().verify_all() == Error::kNone);
    }
    CHECK(w.finalize().take() == b);  // deterministic
    // The N-1 writer (minor ceiling 5) refuses bend / trigger regions.
    BundleWriter w5(WriterOptions{kFormatVersion, 5});
    const std::size_t j0 = w5.add_instrument("A", "a.sfz").value();
    CHECK_ERR(w5.add_sample(j0, {"s0", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(w5.add_region(j0, bend), Error::kNone);
    CHECK_ERR(w5.finalize().error(), Error::kInvalidArgument);
  }
  {  // bundles without bend or triggers keep their exact bytes whatever the writer's ceiling (minor 0..5 content)
    Region trg = plain; trg.trigger = TriggerMode::kReleaseKey;
    Region amp = plain; amp.amp.veltrack = 10.f;
    Region ccr = plain; ccr.cc.gate_count = 1; ccr.cc.gates[0] = {64, 64.f, 100.f};
    Region vgr = plain; vgr.vg.polyphony = 2;
    for (const Region& rg : {plain, sample_region(0), ext_region(0), trg, amp, ccr, vgr}) {
      Bytes by[7];
      for (std::uint32_t m = 0; m < 7; ++m) {
        if ((m == 0 && has_extension(rg)) || (m < 2 && has_trigger_extension(rg)) || (m < 3 && has_amp_extension(rg)) || (m < 4 && has_cc_extension(rg)) || (m < 5 && has_vg_extension(rg))) continue;
        BundleWriter x(WriterOptions{kFormatVersion, m});
        const std::size_t k = x.add_instrument("A", "a.sfz").value();
        CHECK_ERR(x.add_sample(k, {"s0", f, 1000, pcm}), Error::kNone);
        CHECK_ERR(x.add_region(k, rg), Error::kNone);
        by[m] = x.finalize().take();
      }
      for (std::uint32_t m = 0; m < 6; ++m) CHECK(by[6] == by[m] || by[m].empty());
      const std::uint32_t want = has_vg_extension(rg) ? 5u : has_cc_extension(rg) ? 4u : has_amp_extension(rg) ? 3u : has_trigger_extension(rg) ? 2u : has_extension(rg) ? 1u : 0u;
      CHECK(version_word(by[6]) == (2u | (want << 16)));
    }
  }
  // Writer-side validation.
  {
    BundleWriter v;
    const std::size_t vi = v.add_instrument("A", "a.sfz").value();
    CHECK_ERR(v.add_sample(vi, {"s", f, 1000, pcm}), Error::kNone);
    auto bad = [&](auto mutate) { Region x = all; mutate(x); return v.add_region(vi, x); };
    CHECK_ERR(bad([](Region&) {}), Error::kNone);
    CHECK_ERR(bad([](Region& x) { x.bend.up = 12001.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.bend.down = -12001.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.bend.up = 12000.f; x.bend.down = -12000.f; }), Error::kNone);
    CHECK_ERR(bad([](Region& x) { x.bend.up = std::nanf(""); }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.bend.up = -0.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.bend.step = 0.5f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.bend.step = 12001.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.bend.enabled = false; }), Error::kInvalidArgument);  // values without the flag: two encodings
    CHECK_ERR(bad([](Region& x) { x.trig.count = 5; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.trig.t[0].cc = 128; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.trig.t[1].cc = 20; }), Error::kInvalidArgument);  // not ascending
    CHECK_ERR(bad([](Region& x) { x.trig.t[0].lo = 100.f; x.trig.t[0].hi = 50.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.trig.t[0].hi = 127.5f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.trig.t[0].lo = -0.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.trig.t[2].cc = 9; }), Error::kInvalidArgument);  // an unused entry must be zero
    CHECK_ERR(bad([](Region& x) { x.trig.count = 0; x.trig.t[0] = {}; x.trig.t[1] = {}; }), Error::kInvalidArgument);  // no_note without a trigger: nothing can start it
    CHECK_ERR(bad([](Region& x) { x.trig.count = 0; x.trig.t[0] = {}; x.trig.t[1] = {}; x.trig.no_note = false; }), Error::kNone);
  }
  // Corruption of the table (the tail of the manifest: one record of a region with 2 triggers).
  BundleWriter u;
  const std::size_t ui = u.add_instrument("A", "a.sfz").value();
  CHECK_ERR(u.add_sample(ui, {"s", f, 1000, pcm}), Error::kNone);
  CHECK_ERR(u.add_region(ui, plain), Error::kNone);
  CHECK_ERR(u.add_region(ui, trig), Error::kNone);
  const Bytes good = u.finalize().take();
  const std::size_t end = rd64(good, 8) + rd64(good, 16);
  CHECK(layout::kBtRecordSize == 20 && layout::kBtTriggerSize == 12);
  const std::size_t rec = end - layout::kBtRecordSize - 2 * layout::kBtTriggerSize;
  CHECK(layout::get_u32(good.data() + rec - 4) == 1u && layout::get_u32(good.data() + rec) == 1u);  // n_bt, region index 1
  CHECK(good[rec + 4] == 2 && good[rec + 5] == 2);                                                    // flags: no_note only, 2 triggers
  auto put_f32 = [](Bytes& by, std::size_t at, float v) { std::uint32_t u32; std::memcpy(&u32, &v, 4); wr32(by, at, u32); };
  auto expect_bad = [&](Bytes bad_bytes, const char* what) {
    rehash(bad_bytes);
    auto rb = BundleReader::open_memory(bad_bytes);
    if (rb.ok() || rb.error() != Error::kCorruptManifest) { std::printf("FAIL bend/trigger table corruption not rejected: %s\n", what); ++g_fail; }
  };
  { Bytes b = good; b[rec + 4] = 4; expect_bad(b, "unknown flag bit"); }
  {  // clearing the no_note flag leaves a valid record: the region then starts from notes as well
    Bytes b = good; b[rec + 4] = 0; rehash(b);
    auto rb = BundleReader::open_memory(b);
    CHECK(rb.ok());
    if (rb.ok()) CHECK(!rb.value().instruments()[0].regions[1].trig.no_note && rb.value().instruments()[0].regions[1].trig.count == 2);
  }
  { Bytes b = good; b[rec + 5] = 5; expect_bad(b, "trigger count above 4"); }
  { Bytes b = good; b[rec + 5] = 0; expect_bad(b, "no_note without a trigger (and trailing bytes)"); }
  { Bytes b = good; b[rec + 6] = 5; expect_bad(b, "smooth without the bend flag"); }
  { Bytes b = good; put_f32(b, rec + 8, 5.f); expect_bad(b, "bend values without the flag"); }
  { Bytes b = good; b[rec + 20 + 1] = 1; expect_bad(b, "trigger padding"); }
  { Bytes b = good; b[rec + 20] = 130; expect_bad(b, "trigger controller above 127"); }
  { Bytes b = good; b[rec + 20 + 12] = 10; expect_bad(b, "triggers not ascending"); }
  { Bytes b = good; put_f32(b, rec + 20 + 4, 200.f); expect_bad(b, "trigger lo above 127"); }
  { Bytes b = good; put_f32(b, rec + 20 + 8, std::nanf("")); expect_bad(b, "NaN trigger hi"); }
  { Bytes b = good; wr32(b, rec, 5); expect_bad(b, "region index out of range"); }
  { Bytes b = good; wr32(b, rec - 4, 3); expect_bad(b, "n_bt larger than regions"); }
  for (std::size_t n = 0; n < good.size(); n += (n < 500 ? 1 : 17))
    CHECK(!BundleReader::open_memory(std::span<const std::uint8_t>(good.data(), n)).ok());
  for (std::size_t off = rec - 4; off < end; ++off)
    for (int bit = 0; bit < 8; ++bit) {
      Bytes bad_bytes = good;
      bad_bytes[off] ^= std::uint8_t(1 << bit);
      rehash(bad_bytes);
      auto r3 = BundleReader::open_memory(bad_bytes);
      if (!r3.ok() && r3.error() != Error::kCorruptManifest) { std::printf("FAIL bt flip at %zu bit %d -> %s\n", off, bit, to_string(r3.error())); ++g_fail; }
    }
  // A record that holds the default bend and no trigger is non-canonical.
  { Bytes b = good; for (std::size_t i = rec + 4; i < rec + 20; ++i) b[i] = 0; put_f32(b, rec + 8, 200.f); put_f32(b, rec + 12, -200.f); put_f32(b, rec + 16, 1.f); expect_bad(b, "record with the default values"); }
  // A minor 6 stamp on a bundle with no record is non-canonical; so is a newer minor.
  {
    BundleWriter x;
    const std::size_t k = x.add_instrument("A", "a.sfz").value();
    CHECK_ERR(x.add_sample(k, {"s", f, 1000, pcm}), Error::kNone);
    Region a = plain; a.vg.polyphony = 2;
    CHECK_ERR(x.add_region(k, a), Error::kNone);
    Bytes b = x.finalize().take();  // minor 5
    const std::size_t mo = rd64(b, 8), ms = rd64(b, 16), io = rd64(b, 24), is = rd64(b, 32);
    b.insert(b.begin() + mo + ms, 4, std::uint8_t(0));  // n_bt = 0
    wr64(b, 16, ms + 4); wr64(b, 24, io + 4); wr64(b, 40, rd64(b, 40) + 4); wr64(b, 56, rd64(b, 56) + 4);
    for (std::size_t e = 0; e < is / 48; ++e) wr64(b, io + 4 + e * 48, rd64(b, io + 4 + e * 48) + 4);
    wr32(b, 4, 2u | (6u << 16));
    wr32(b, mo, 2u | (6u << 16));
    rehash(b);
    CHECK_ERR(BundleReader::open_memory(b).error(), Error::kCorruptManifest);
    wr32(b, 4, 2u | ((kFormatMinor + 1) << 16));
    CHECK_ERR(BundleReader::open_memory(b).error(), Error::kUnsupportedVersion);
  }
}

// Story 13.8: format 2.7, the filter table (key track, velocity track, CC entries, second filter).
void test_fl_table() {
  constexpr std::uint32_t kFlMinor = 7;  // the minor that introduced the filter table: what a filter record stamps, whatever kFormatMinor becomes
  const PcmFormat f{2, 16, false, 48000};
  const auto pcm = vt::make_int(f, 1000, Kind::kMixed);
  auto version_word = [](const Bytes& b) { return layout::get_u32(b.data() + 4); };
  Region plain;
  Region filt = plain;  // a filter with a velocity track only
  filt.filter = {FilterType::kLpf2p, 500.f, 0.f};
  filt.fmap.f1.veltrack = 2400.f;
  Region tuned = filt;  // every field of the first filter's tuning
  tuned.filter.resonance_db = 3.f;
  tuned.fmap.f1.keytrack = 50.5f; tuned.fmap.f1.keycenter = 72; tuned.fmap.f1.veltrack = -1200.f;
  tuned.fmap.f1.cutoff_count = 2; tuned.fmap.f1.cutoff[0] = {74, 7, 1200.f}; tuned.fmap.f1.cutoff[1] = {131, 0, -2400.5f};
  tuned.fmap.f1.res_count = 1; tuned.fmap.f1.res[0] = {1, 3, 12.f};
  Region second = tuned;  // plus a second filter with its own tuning
  second.fmap.f2.type = FilterType::kHpf2p; second.fmap.f2.cutoff_hz = 250.f; second.fmap.f2.resonance_db = 1.5f;
  second.fmap.f2.tune.keytrack = 100.f; second.fmap.f2.tune.cutoff_count = 1; second.fmap.f2.tune.cutoff[0] = {1, 0, 600.f};
  Region only2 = ext_region(0);  // a second filter and no tuning on the first
  only2.fmap.f2.type = FilterType::kBpf2p; only2.fmap.f2.cutoff_hz = 1000.f;
  Region all = second;  // minors 1..6 content in one region
  all.trigger = TriggerMode::kLegato; all.has_end = true; all.end = 900; all.amp.veltrack = 40.f;
  all.cc.gate_count = 1; all.cc.gates[0] = {1, 10.f, 20.f};
  all.vg.group = 4; all.vg.polyphony = 1; all.vg.group_polyphony = 1;
  all.bend.enabled = true; all.bend.up = 300.f;
  const std::vector<Region> regs = {plain, filt, tuned, second, only2, all};
  {  // round trip: two instruments, only the first has records; the second still carries the empty table
    BundleWriter w;
    const std::size_t i0 = w.add_instrument("A", "a.sfz").value(), i1 = w.add_instrument("B", "b.sfz").value();
    CHECK_ERR(w.add_sample(i0, {"s0", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(w.add_sample(i1, {"s0", f, 1000, pcm}), Error::kNone);
    for (const auto& r : regs) CHECK_ERR(w.add_region(i0, r), Error::kNone);
    CHECK_ERR(w.add_region(i1, plain), Error::kNone);
    const Bytes b = w.finalize().take();
    CHECK(version_word(b) == (2u | (kFlMinor << 16)));
    auto r = BundleReader::open_memory(b);
    CHECK(r.ok() && r.value().format_minor() == kFlMinor);
    if (r.ok()) {
      CHECK(r.value().instruments()[0].regions == regs);
      CHECK(r.value().instruments()[1].regions.size() == 1 && r.value().instruments()[1].regions[0] == plain);
      CHECK(r.value().verify_all() == Error::kNone);
    }
    CHECK(w.finalize().take() == b);  // deterministic
    // The N-1 writer (minor ceiling 6) refuses filter-table regions.
    BundleWriter w6(WriterOptions{kFormatVersion, 6});
    const std::size_t j0 = w6.add_instrument("A", "a.sfz").value();
    CHECK_ERR(w6.add_sample(j0, {"s0", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(w6.add_region(j0, filt), Error::kNone);
    CHECK_ERR(w6.finalize().error(), Error::kInvalidArgument);
  }
  {  // bundles without a filter map keep their exact bytes whatever the writer's ceiling (minor 0..6 content)
    Region trg = plain; trg.trigger = TriggerMode::kReleaseKey;
    Region amp = plain; amp.amp.veltrack = 10.f;
    Region ccr = plain; ccr.cc.gate_count = 1; ccr.cc.gates[0] = {64, 64.f, 100.f};
    Region vgr = plain; vgr.vg.polyphony = 2;
    Region bnd = plain; bnd.bend.enabled = true;
    Region plain_filter = ext_region(0);  // a Story 5.1 filter with no tuning is still minor 1
    for (const Region& rg : {plain, sample_region(0), plain_filter, trg, amp, ccr, vgr, bnd}) {
      Bytes by[8];
      for (std::uint32_t m = 0; m < 8; ++m) {
        if ((m == 0 && has_extension(rg)) || (m < 2 && has_trigger_extension(rg)) || (m < 3 && has_amp_extension(rg)) || (m < 4 && has_cc_extension(rg)) ||
            (m < 5 && has_vg_extension(rg)) || (m < 6 && has_bend_trigger_extension(rg)))
          continue;
        BundleWriter x(WriterOptions{kFormatVersion, m});
        const std::size_t k = x.add_instrument("A", "a.sfz").value();
        CHECK_ERR(x.add_sample(k, {"s0", f, 1000, pcm}), Error::kNone);
        CHECK_ERR(x.add_region(k, rg), Error::kNone);
        by[m] = x.finalize().take();
      }
      for (std::uint32_t m = 0; m < 7; ++m) CHECK(by[7] == by[m] || by[m].empty());
      const std::uint32_t want = has_bend_trigger_extension(rg) ? 6u : has_vg_extension(rg) ? 5u : has_cc_extension(rg) ? 4u : has_amp_extension(rg) ? 3u
                                 : has_trigger_extension(rg) ? 2u : has_extension(rg) ? 1u : 0u;
      CHECK(version_word(by[7]) == (2u | (want << 16)));
    }
  }
  // Writer-side validation.
  {
    BundleWriter v;
    const std::size_t vi = v.add_instrument("A", "a.sfz").value();
    CHECK_ERR(v.add_sample(vi, {"s", f, 1000, pcm}), Error::kNone);
    auto bad = [&](auto mutate) { Region x = second; mutate(x); return v.add_region(vi, x); };
    CHECK_ERR(bad([](Region&) {}), Error::kNone);
    CHECK_ERR(bad([](Region& x) { x.fmap.f1.keytrack = 1201.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.fmap.f1.keytrack = -1.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.fmap.f1.keytrack = 1200.f; x.fmap.f1.veltrack = 12000.f; }), Error::kNone);
    CHECK_ERR(bad([](Region& x) { x.fmap.f1.veltrack = -12001.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.fmap.f1.veltrack = std::nanf(""); }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.fmap.f1.veltrack = -0.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.fmap.f1.keycenter = 128; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.fmap.f1.keytrack = 0.f; x.fmap.f1.keycenter = 48; }), Error::kInvalidArgument);  // a centre without a key track: two encodings
    CHECK_ERR(bad([](Region& x) { x.fmap.f2.tune.keytrack = 0.f; x.fmap.f2.tune.keycenter = 48; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.fmap.f1.cutoff_count = 5; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.fmap.f1.cutoff[1].cc = 74; }), Error::kInvalidArgument);  // not ascending
    CHECK_ERR(bad([](Region& x) { x.fmap.f1.cutoff[0].cc = 132; }), Error::kInvalidArgument);  // not a source
    CHECK_ERR(bad([](Region& x) { x.fmap.f1.cutoff[0].depth = 12001.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.fmap.f1.res[0].depth = -1.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.fmap.f1.res[0].depth = 97.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.fmap.f1.res[3].cc = 9; }), Error::kInvalidArgument);  // an unused entry must be zero
    CHECK_ERR(bad([](Region& x) { x.fmap.f2.cutoff_hz = 48001.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.fmap.f2.type = static_cast<FilterType>(6); }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.fmap.f2.type = FilterType::kNone; }), Error::kInvalidArgument);  // values without a type: two encodings
    CHECK_ERR(bad([](Region& x) { x.filter = Filter(); }), Error::kInvalidArgument);                  // a second filter and a tuning need the first filter
    CHECK_ERR(bad([](Region& x) { x.fmap.f1 = FilterTune(); x.fmap.f2 = Filter2(); x.filter = Filter(); }), Error::kNone);
  }
  // Corruption of the table (the tail of the manifest: one record, a first tuning with 2 + 1 entries and a second filter).
  BundleWriter u;
  const std::size_t ui = u.add_instrument("A", "a.sfz").value();
  CHECK_ERR(u.add_sample(ui, {"s", f, 1000, pcm}), Error::kNone);
  CHECK_ERR(u.add_region(ui, plain), Error::kNone);
  CHECK_ERR(u.add_region(ui, second), Error::kNone);
  const Bytes good = u.finalize().take();
  const std::size_t end = rd64(good, 8) + rd64(good, 16);
  CHECK(layout::kFlRecordFixedSize == 8 && layout::kFlTuneFixedSize == 12 && layout::kFlModSize == 8 && layout::kFlSecondFixedSize == 12);
  const std::size_t rec_len = layout::kFlRecordFixedSize + layout::kFlTuneFixedSize + 3 * layout::kFlModSize + layout::kFlSecondFixedSize + layout::kFlTuneFixedSize + layout::kFlModSize;
  const std::size_t rec = end - rec_len;
  CHECK(layout::get_u32(good.data() + rec - 4) == 1u && layout::get_u32(good.data() + rec) == 1u);  // n_fl, region index 1
  CHECK(good[rec + 4] == 1);                                                                          // flags: second filter
  auto put_f32 = [](Bytes& by, std::size_t at, float v) { std::uint32_t u32; std::memcpy(&u32, &v, 4); wr32(by, at, u32); };
  auto expect_bad = [&](Bytes bad_bytes, const char* what) {
    rehash(bad_bytes);
    auto rb = BundleReader::open_memory(bad_bytes);
    if (rb.ok() || rb.error() != Error::kCorruptManifest) { std::printf("FAIL filter table corruption not rejected: %s\n", what); ++g_fail; }
  };
  const std::size_t t1 = rec + 8, m1 = t1 + 12, s2 = m1 + 3 * 8;  // first tune, its entries, the second filter
  { Bytes b = good; b[rec + 4] = 2; expect_bad(b, "unknown flag bit"); }
  { Bytes b = good; b[rec + 5] = 1; expect_bad(b, "flag padding"); }
  { Bytes b = good; b[t1 + 9] = 5; expect_bad(b, "cutoff count above 4"); }
  { Bytes b = good; b[t1 + 11] = 1; expect_bad(b, "tune padding"); }
  { Bytes b = good; put_f32(b, t1, 2000.f); expect_bad(b, "keytrack above 1200"); }
  { Bytes b = good; put_f32(b, t1 + 4, std::nanf("")); expect_bad(b, "NaN veltrack"); }
  { Bytes b = good; b[t1 + 8] = 200; expect_bad(b, "keycenter above 127"); }
  { Bytes b = good; b[m1] = 132; expect_bad(b, "entry controller that is not a source"); }
  { Bytes b = good; b[m1 + 8] = 1; expect_bad(b, "entries not ascending"); }
  { Bytes b = good; b[m1 + 2] = 1; expect_bad(b, "entry padding"); }
  { Bytes b = good; put_f32(b, m1 + 4, 50000.f); expect_bad(b, "cutoff depth out of range"); }
  { Bytes b = good; put_f32(b, m1 + 16 + 4, -3.f); expect_bad(b, "negative resonance depth"); }
  { Bytes b = good; b[s2] = 0; expect_bad(b, "second filter flagged with type none"); }
  { Bytes b = good; b[s2] = 6; expect_bad(b, "second filter type above 5"); }
  { Bytes b = good; b[s2 + 1] = 1; expect_bad(b, "second filter padding"); }
  { Bytes b = good; put_f32(b, s2 + 4, 60000.f); expect_bad(b, "second cutoff out of range"); }
  { Bytes b = good; wr32(b, rec, 5); expect_bad(b, "region index out of range"); }
  { Bytes b = good; wr32(b, rec - 4, 3); expect_bad(b, "n_fl larger than regions"); }
  for (std::size_t n = 0; n < good.size(); n += (n < 500 ? 1 : 17))
    CHECK(!BundleReader::open_memory(std::span<const std::uint8_t>(good.data(), n)).ok());
  for (std::size_t off = rec - 4; off < end; ++off)
    for (int bit = 0; bit < 8; ++bit) {
      Bytes bad_bytes = good;
      bad_bytes[off] ^= std::uint8_t(1 << bit);
      rehash(bad_bytes);
      auto r3 = BundleReader::open_memory(bad_bytes);
      if (!r3.ok() && r3.error() != Error::kCorruptManifest) { std::printf("FAIL fl flip at %zu bit %d -> %s\n", off, bit, to_string(r3.error())); ++g_fail; }
    }
  // A record that holds the default map is non-canonical (a veltrack-only record whose veltrack is set back to 0).
  {
    BundleWriter d;
    const std::size_t di = d.add_instrument("A", "a.sfz").value();
    CHECK_ERR(d.add_sample(di, {"s", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(d.add_region(di, plain), Error::kNone);
    CHECK_ERR(d.add_region(di, filt), Error::kNone);
    Bytes b = d.finalize().take();
    const std::size_t dend = rd64(b, 8) + rd64(b, 16), drec = dend - (layout::kFlRecordFixedSize + layout::kFlTuneFixedSize);
    CHECK(layout::get_u32(b.data() + drec) == 1u && b[drec + 4] == 0);
    put_f32(b, drec + 8 + 4, 0.f);
    expect_bad(b, "record with the default map");
  }
  // A minor 7 stamp on a bundle with no record is non-canonical; so is a newer minor.
  {
    BundleWriter x;
    const std::size_t k = x.add_instrument("A", "a.sfz").value();
    CHECK_ERR(x.add_sample(k, {"s", f, 1000, pcm}), Error::kNone);
    Region a = plain; a.bend.enabled = true;
    CHECK_ERR(x.add_region(k, a), Error::kNone);
    Bytes b = x.finalize().take();  // minor 6
    const std::size_t mo = rd64(b, 8), ms = rd64(b, 16), io = rd64(b, 24), is = rd64(b, 32);
    b.insert(b.begin() + mo + ms, 4, std::uint8_t(0));  // n_fl = 0
    wr64(b, 16, ms + 4); wr64(b, 24, io + 4); wr64(b, 40, rd64(b, 40) + 4); wr64(b, 56, rd64(b, 56) + 4);
    for (std::size_t e = 0; e < is / 48; ++e) wr64(b, io + 4 + e * 48, rd64(b, io + 4 + e * 48) + 4);
    wr32(b, 4, 2u | (kFlMinor << 16));
    wr32(b, mo, 2u | (kFlMinor << 16));
    rehash(b);
    CHECK_ERR(BundleReader::open_memory(b).error(), Error::kCorruptManifest);
    wr32(b, 4, 2u | ((kFormatMinor + 1) << 16));
    CHECK_ERR(BundleReader::open_memory(b).error(), Error::kUnsupportedVersion);
  }
}

// Story 13.9: format 2.8, the modulation table (region EQ, amplitude / pitch / filter LFO, pitch / filter EG).
void test_md_table() {
  constexpr std::uint32_t kMdMinor = 8;  // the minor that introduced the modulation table
  const PcmFormat f{2, 16, false, 48000};
  const auto pcm = vt::make_int(f, 1000, Kind::kMixed);
  auto version_word = [](const Bytes& b) { return layout::get_u32(b.data() + 4); };
  Region plain;
  Region lfo = plain;
  lfo.mod.amp = {true, LfoWave::kSine, 3.f, 5.5f, 0.25f, 1.f};
  Region eg = plain;
  eg.filter = {FilterType::kLpf2p, 800.f, 0.f};
  eg.mod.pitch_eg = {true, 0.01f, 0.1f, 0.02f, 0.3f, 0.4f, 5.f, 20.f, 1200.f, -300.f};
  eg.mod.fil_eg = {true, 0.f, 0.2f, 0.f, 0.f, 0.f, 0.f, 100.f, 0.f, 2400.f};
  Region eq = plain;
  eq.mod.eq_count = 3;
  eq.mod.eq[0].type = EqType::kLowShelf; eq.mod.eq[0].freq = 100.f; eq.mod.eq[0].bw = 0.5f; eq.mod.eq[0].gain = 3.f; eq.mod.eq[0].vel2freq = 200.f; eq.mod.eq[0].vel2gain = -6.f;
  eq.mod.eq[0].freq_count = 2; eq.mod.eq[0].freq_mods[0] = {80, 0, 500.f}; eq.mod.eq[0].freq_mods[1] = {131, 7, -100.f};
  eq.mod.eq[0].bw_count = 1; eq.mod.eq[0].bw_mods[0] = {92, 0, 0.25f};
  eq.mod.eq[0].gain_count = 1; eq.mod.eq[0].gain_mods[0] = {86, 3, 2.f};
  eq.mod.eq[1].gain = -4.f;  // freq stays 0, as sfizz leaves a band whose predecessor has a frequency
  eq.mod.eq[2].type = EqType::kHighShelf; eq.mod.eq[2].freq = 5000.f; eq.mod.eq[2].gain = 1.f;
  Region all = eq;  // everything, plus minors 1..7 content
  all.mod.amp = lfo.mod.amp; all.mod.pitch = {true, LfoWave::kSaw, -50.f, 6.f, 0.f, 0.f}; all.mod.fil = {true, LfoWave::kTriangle, 1200.f, 0.5f, 0.f, 0.3f};
  all.mod.pitch_eg = eg.mod.pitch_eg; all.mod.fil_eg = eg.mod.fil_eg;
  all.filter = {FilterType::kLpf2p, 500.f, 3.f};
  all.fmap.f1.veltrack = 2400.f;
  all.trigger = TriggerMode::kLegato; all.has_end = true; all.end = 900; all.amp.veltrack = 40.f;
  all.cc.gate_count = 1; all.cc.gates[0] = {1, 10.f, 20.f};
  all.vg.group = 4; all.vg.polyphony = 1; all.vg.group_polyphony = 1;
  all.bend.enabled = true; all.bend.up = 300.f;
  const std::vector<Region> regs = {plain, lfo, eg, eq, all};
  {  // round trip: two instruments, only the first has records; the second still carries the empty table
    BundleWriter w;
    const std::size_t i0 = w.add_instrument("A", "a.sfz").value(), i1 = w.add_instrument("B", "b.sfz").value();
    CHECK_ERR(w.add_sample(i0, {"s0", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(w.add_sample(i1, {"s0", f, 1000, pcm}), Error::kNone);
    for (const auto& r : regs) CHECK_ERR(w.add_region(i0, r), Error::kNone);
    CHECK_ERR(w.add_region(i1, plain), Error::kNone);
    const Bytes b = w.finalize().take();
    CHECK(version_word(b) == (2u | (kMdMinor << 16)));
    auto r = BundleReader::open_memory(b);
    CHECK(r.ok() && r.value().format_minor() == kMdMinor);
    if (r.ok()) {
      CHECK(r.value().instruments()[0].regions == regs);
      CHECK(r.value().instruments()[1].regions.size() == 1 && r.value().instruments()[1].regions[0] == plain);
      CHECK(r.value().verify_all() == Error::kNone);
    }
    CHECK(w.finalize().take() == b);  // deterministic
    BundleWriter w7(WriterOptions{kFormatVersion, 7});  // the N-1 writer refuses modulation regions
    const std::size_t j0 = w7.add_instrument("A", "a.sfz").value();
    CHECK_ERR(w7.add_sample(j0, {"s0", f, 1000, pcm}), Error::kNone);
    CHECK_ERR(w7.add_region(j0, lfo), Error::kNone);
    CHECK_ERR(w7.finalize().error(), Error::kInvalidArgument);
  }
  {  // bundles without a ModMap keep their exact bytes whatever the writer's ceiling (minor 0..7 content)
    Region trg = plain; trg.trigger = TriggerMode::kReleaseKey;
    Region amp = plain; amp.amp.veltrack = 10.f;
    Region ccr = plain; ccr.cc.gate_count = 1; ccr.cc.gates[0] = {64, 64.f, 100.f};
    Region vgr = plain; vgr.vg.polyphony = 2;
    Region bnd = plain; bnd.bend.enabled = true;
    Region fl = ext_region(0); fl.fmap.f1.veltrack = 100.f;
    for (const Region& rg : {plain, sample_region(0), ext_region(0), trg, amp, ccr, vgr, bnd, fl}) {
      Bytes by[9];
      for (std::uint32_t m = 0; m < 9; ++m) {
        if ((m == 0 && has_extension(rg)) || (m < 2 && has_trigger_extension(rg)) || (m < 3 && has_amp_extension(rg)) || (m < 4 && has_cc_extension(rg)) ||
            (m < 5 && has_vg_extension(rg)) || (m < 6 && has_bend_trigger_extension(rg)) || (m < 7 && has_filter_extension(rg)))
          continue;
        BundleWriter x(WriterOptions{kFormatVersion, m});
        const std::size_t k = x.add_instrument("A", "a.sfz").value();
        CHECK_ERR(x.add_sample(k, {"s0", f, 1000, pcm}), Error::kNone);
        CHECK_ERR(x.add_region(k, rg), Error::kNone);
        by[m] = x.finalize().take();
      }
      for (std::uint32_t m = 0; m < 8; ++m) CHECK(by[8] == by[m] || by[m].empty());
      const std::uint32_t want = has_filter_extension(rg) ? 7u : has_bend_trigger_extension(rg) ? 6u : has_vg_extension(rg) ? 5u : has_cc_extension(rg) ? 4u : has_amp_extension(rg) ? 3u
                                 : has_trigger_extension(rg) ? 2u : has_extension(rg) ? 1u : 0u;
      CHECK(version_word(by[8]) == (2u | (want << 16)));
    }
  }
  {  // writer-side validation
    BundleWriter v;
    const std::size_t vi = v.add_instrument("A", "a.sfz").value();
    CHECK_ERR(v.add_sample(vi, {"s", f, 1000, pcm}), Error::kNone);
    auto bad = [&](auto mutate) { Region x = all; mutate(x); return v.add_region(vi, x); };
    CHECK_ERR(bad([](Region&) {}), Error::kNone);
    CHECK_ERR(bad([](Region& x) { x.mod.amp.depth = 10.5f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.mod.amp.depth = 0.f; }), Error::kInvalidArgument);        // a stored LFO has a depth
    CHECK_ERR(bad([](Region& x) { x.mod.amp.freq = 100.5f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.mod.amp.delay = -1.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.mod.pitch.depth = 1201.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.mod.pitch.wave = static_cast<LfoWave>(8); }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.mod.fil.fade = std::nanf(""); }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.mod.fil.fade = -0.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.mod.pitch_eg.on = false; }), Error::kInvalidArgument);    // an off EG must be the default
    CHECK_ERR(bad([](Region& x) { x.mod.pitch_eg.depth = 0.f; x.mod.pitch_eg.vel2depth = 0.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.mod.pitch_eg.attack = 101.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.mod.fil_eg.sustain = 101.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.mod.fil_eg.vel2depth = 12001.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.mod.eq_count = 4; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.mod.eq_count = 2; }), Error::kInvalidArgument);           // band 2 is beyond the count and not the default
    CHECK_ERR(bad([](Region& x) { x.mod.eq[0].type = static_cast<EqType>(0); }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.mod.eq[0].type = static_cast<EqType>(4); }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.mod.eq[0].bw = 0.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.mod.eq[0].bw = 4.5f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.mod.eq[0].gain = 97.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.mod.eq[0].freq = 20001.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.mod.eq[0].vel2freq = -30001.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.mod.eq[0].freq_count = 5; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.mod.eq[0].freq_mods[1].cc = 80; }), Error::kInvalidArgument);  // not ascending
    CHECK_ERR(bad([](Region& x) { x.mod.eq[0].freq_mods[0].cc = 132; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.mod.eq[0].bw_mods[0].depth = 4.5f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.mod.eq[0].gain_mods[0].depth = 0.f; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.mod.eq[0].gain_mods[3].cc = 9; }), Error::kInvalidArgument);    // an unused entry must be zero
    CHECK_ERR(bad([](Region& x) { x.mod.eq[0].bw = 0.005f; }), Error::kInvalidArgument);  // the DSP honours 0.01 octave and up
    CHECK_ERR(bad([](Region& x) { x.mod.eq[0].bw = 0.01f; }), Error::kNone);
    CHECK_ERR(bad([](Region& x) { x.mod.eq[0].gain = 60.5f; }), Error::kInvalidArgument);  // ... and +60 dB at most
    CHECK_ERR(bad([](Region& x) { x.mod.eq[0].gain = 60.f; }), Error::kNone);
    CHECK_ERR(bad([](Region& x) { x.mod.eq[2] = EqBand(); }), Error::kInvalidArgument);  // a trailing default band is no band
    CHECK_ERR(bad([](Region& x) { x.mod.eq[1] = EqBand(); }), Error::kNone);               // a default band in the middle keeps the index
    for (unsigned cc : {131u, 135u, 136u, 137u, 140u, 141u}) {  // the pseudo-controllers are sources for every EQ entry
      CHECK_ERR(bad([cc](Region& x) { x.mod.eq[0].gain_mods[0].cc = static_cast<std::uint8_t>(cc); }), Error::kNone);
      CHECK_ERR(bad([cc](Region& x) { x.mod.eq[0].freq_mods[1].cc = static_cast<std::uint8_t>(cc); }), Error::kNone);
      CHECK_ERR(bad([cc](Region& x) { x.mod.eq[0].bw_mods[0].cc = static_cast<std::uint8_t>(cc); }), Error::kNone);
    }
    CHECK_ERR(bad([](Region& x) { x.mod.eq[0].gain_mods[0].cc = 132; }), Error::kInvalidArgument);
    CHECK_ERR(bad([](Region& x) { x.mod = ModMap(); }), Error::kNone);
  }
  // Corruption of the table (the tail of the manifest: one record = an amplitude LFO and one EQ band with 1 + 1 + 1 entries).
  Region rec_region = lfo;
  rec_region.mod.eq_count = 1;
  rec_region.mod.eq[0].freq = 200.f; rec_region.mod.eq[0].gain = 2.f;
  rec_region.mod.eq[0].freq_count = 1; rec_region.mod.eq[0].freq_mods[0] = {74, 0, 100.f};
  rec_region.mod.eq[0].bw_count = 1; rec_region.mod.eq[0].bw_mods[0] = {75, 0, 0.5f};
  rec_region.mod.eq[0].gain_count = 1; rec_region.mod.eq[0].gain_mods[0] = {76, 0, 3.f};
  BundleWriter u;
  const std::size_t ui = u.add_instrument("A", "a.sfz").value();
  CHECK_ERR(u.add_sample(ui, {"s", f, 1000, pcm}), Error::kNone);
  CHECK_ERR(u.add_region(ui, plain), Error::kNone);
  CHECK_ERR(u.add_region(ui, rec_region), Error::kNone);
  const Bytes good = u.finalize().take();
  const std::size_t end = rd64(good, 8) + rd64(good, 16);
  CHECK(layout::kMdRecordFixedSize == 8 && layout::kMdLfoSize == 20 && layout::kMdEgSize == 36 && layout::kMdEqFixedSize == 24 && layout::kMdModSize == 8);
  const std::size_t rec_len = layout::kMdRecordFixedSize + layout::kMdLfoSize + layout::kMdEqFixedSize + 3 * layout::kMdModSize;
  const std::size_t rec = end - rec_len;
  CHECK(layout::get_u32(good.data() + rec - 4) == 1u && layout::get_u32(good.data() + rec) == 1u && good[rec + 4] == 1 && good[rec + 5] == 1);  // n_md, region 1, flags: amp LFO, 1 band
  auto put_f32 = [](Bytes& by, std::size_t at, float v) { std::uint32_t u32; std::memcpy(&u32, &v, 4); wr32(by, at, u32); };
  auto expect_bad = [&](Bytes bad_bytes, const char* what) {
    rehash(bad_bytes);
    auto rb = BundleReader::open_memory(bad_bytes);
    if (rb.ok() || rb.error() != Error::kCorruptManifest) { std::printf("FAIL modulation table corruption not rejected: %s\n", what); ++g_fail; }
  };
  const std::size_t lfo_at = rec + 8, eq_at = lfo_at + 20, mods_at = eq_at + 24;
  { Bytes b = good; b[rec + 4] = 33; expect_bad(b, "unknown flag bit"); }
  { Bytes b = good; b[rec + 5] = 4; expect_bad(b, "eq count above 3"); }
  { Bytes b = good; b[rec + 6] = 1; expect_bad(b, "record padding"); }
  { Bytes b = good; b[lfo_at] = 8; expect_bad(b, "wave above 7"); }
  { Bytes b = good; b[lfo_at + 1] = 1; expect_bad(b, "lfo padding"); }
  { Bytes b = good; put_f32(b, lfo_at + 4, 11.f); expect_bad(b, "amp depth above 10"); }
  { Bytes b = good; put_f32(b, lfo_at + 8, std::nanf("")); expect_bad(b, "NaN frequency"); }
  { Bytes b = good; b[eq_at] = 0; expect_bad(b, "eq type 0"); }
  { Bytes b = good; b[eq_at + 1] = 5; expect_bad(b, "freq entry count above 4"); }
  { Bytes b = good; put_f32(b, eq_at + 8, 5.f); expect_bad(b, "bandwidth above 4"); }
  { Bytes b = good; b[mods_at] = 132; expect_bad(b, "entry controller that is not a source"); }
  { Bytes b = good; b[mods_at + 2] = 1; expect_bad(b, "entry padding"); }
  { Bytes b = good; put_f32(b, mods_at + 4, 0.f); expect_bad(b, "zero entry depth"); }
  { Bytes b = good; put_f32(b, mods_at + 16 + 4, 97.f); expect_bad(b, "gain entry depth above 96"); }
  { Bytes b = good; wr32(b, rec, 5); expect_bad(b, "region index out of range"); }
  { Bytes b = good; wr32(b, rec - 4, 3); expect_bad(b, "n_md larger than regions"); }
  for (std::size_t n = 0; n < good.size(); n += (n < 500 ? 1 : 17))
    CHECK(!BundleReader::open_memory(std::span<const std::uint8_t>(good.data(), n)).ok());
  for (std::size_t off = rec - 4; off < end; ++off)
    for (int bit = 0; bit < 8; ++bit) {
      Bytes bad_bytes = good;
      bad_bytes[off] ^= std::uint8_t(1 << bit);
      rehash(bad_bytes);
      auto r3 = BundleReader::open_memory(bad_bytes);
      if (!r3.ok() && r3.error() != Error::kCorruptManifest) { std::printf("FAIL md flip at %zu bit %d -> %s\n", off, bit, to_string(r3.error())); ++g_fail; }
    }
  {  // a record that holds the default map is non-canonical (the LFO region with its LFO switched off in the bytes: flags 0, no payload would be a size change, so zero the depth instead)
    Bytes b = good;
    put_f32(b, lfo_at + 4, 0.f);
    expect_bad(b, "stored LFO without a depth");
  }
  {  // a minor 8 stamp on a bundle with no record is non-canonical; so is a newer minor
    BundleWriter x;
    const std::size_t k = x.add_instrument("A", "a.sfz").value();
    CHECK_ERR(x.add_sample(k, {"s", f, 1000, pcm}), Error::kNone);
    Region a = plain; a.fmap.f1.veltrack = 100.f; a.filter = {FilterType::kLpf2p, 500.f, 0.f};
    CHECK_ERR(x.add_region(k, a), Error::kNone);
    Bytes b = x.finalize().take();  // minor 7
    const std::size_t mo = rd64(b, 8), ms = rd64(b, 16), io = rd64(b, 24), is = rd64(b, 32);
    b.insert(b.begin() + mo + ms, 4, std::uint8_t(0));  // n_md = 0
    wr64(b, 16, ms + 4); wr64(b, 24, io + 4); wr64(b, 40, rd64(b, 40) + 4); wr64(b, 56, rd64(b, 56) + 4);
    for (std::size_t e = 0; e < is / 48; ++e) wr64(b, io + 4 + e * 48, rd64(b, io + 4 + e * 48) + 4);
    wr32(b, 4, 2u | (kMdMinor << 16));
    wr32(b, mo, 2u | (kMdMinor << 16));
    rehash(b);
    CHECK_ERR(BundleReader::open_memory(b).error(), Error::kCorruptManifest);
    wr32(b, 4, 2u | ((kFormatMinor + 1) << 16));
    CHECK_ERR(BundleReader::open_memory(b).error(), Error::kUnsupportedVersion);
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
  test_cc_table();
  test_vg_table();
  test_bt_table();
  test_fl_table();
  test_md_table();
  test_float_goldens();
  test_identity();
  test_file_io();
  if (g_fail == 0) std::printf("bundle_test OK\n");
  return g_fail == 0 ? 0 : 1;
}
