#include <cstdio>
#include <cstring>
#include <filesystem>

#include "test_util.hpp"
#include "velora/format/wav_source.hpp"

using namespace velora::format;

namespace {
using Bytes = std::vector<std::uint8_t>;
void w16(Bytes& b, std::uint32_t v) { b.push_back(v & 255); b.push_back((v >> 8) & 255); }
void w32(Bytes& b, std::uint32_t v) { w16(b, v & 0xFFFF); w16(b, v >> 16); }
void tag(Bytes& b, const char* t) { b.insert(b.end(), t, t + 4); }

Bytes make_wav(unsigned format_tag, unsigned ch, unsigned rate, unsigned bits,
               const Bytes& payload, bool extensible = false, bool extra_chunk = false) {
  Bytes b;
  tag(b, "RIFF"); w32(b, 0); tag(b, "WAVE");
  if (extra_chunk) { tag(b, "LIST"); w32(b, 3); b.insert(b.end(), {1, 2, 3, 0}); }  // odd + pad
  tag(b, "fmt ");
  w32(b, extensible ? 40 : 16);
  w16(b, extensible ? 0xFFFE : format_tag); w16(b, ch); w32(b, rate);
  w32(b, rate * ch * bits / 8); w16(b, ch * bits / 8); w16(b, bits);
  if (extensible) {
    w16(b, 22); w16(b, bits); w32(b, 3);
    w16(b, format_tag); b.insert(b.end(), 14, 0);
  }
  tag(b, "data"); w32(b, static_cast<std::uint32_t>(payload.size()));
  b.insert(b.end(), payload.begin(), payload.end());
  return b;
}
}  // namespace

int main() {
  {  // PCM16 stereo: values pass through unchanged, including extremes
    Bytes p;
    const std::int16_t v[] = {0, -1, 32767, -32768, 1234, -4321};
    for (auto x : v) w16(p, static_cast<std::uint16_t>(x));
    auto w = parse_wav(make_wav(1, 2, 44100, 16, p));
    CHECK(w.ok());
    if (w.ok()) {
      CHECK(w.value().frame_count == 3 && w.value().format == (PcmFormat{2, 16, false, 44100}));
      CHECK(w.value().data == (std::vector<std::int32_t>{0, -1, 32767, -32768, 1234, -4321}));
    }
  }
  {  // PCM24 mono with sign extension, extensible header, extra odd chunk
    Bytes p = {0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF};
    auto w = parse_wav(make_wav(1, 1, 96000, 24, p, true, true));
    CHECK(w.ok());
    if (w.ok())
      CHECK(w.value().data == (std::vector<std::int32_t>{8388607, -8388608, 1, -1}));
  }
  {  // float32 bit patterns incl. NaN payload, then through a bundle round trip
    Bytes p;
    const std::uint32_t v[] = {0x7fc00001u, 0x80000000u, 0x3f800000u, 0xffc12345u};
    for (auto x : v) w32(p, x);
    auto w = parse_wav(make_wav(3, 2, 48000, 32, p));
    CHECK(w.ok());
    if (w.ok()) {
      CHECK(w.value().format == (PcmFormat{2, 32, true, 48000}));
      BundleWriter bw;
      const auto i = bw.add_instrument("n", "n.sfz").value();
      CHECK_ERR(bw.add_sample(i, w.value().as_input("s")), Error::kNone);
      const auto bytes = bw.finalize().take();
      auto r = BundleReader::open_memory(bytes);
      CHECK(r.ok());
      std::vector<std::int32_t> out(4);
      CHECK(r.ok() && r.value().read_frames(0, 0, 2, out) == Error::kNone);
      CHECK(std::memcmp(out.data(), w.value().data.data(), 16) == 0);
    }
  }
  {  // errors
    CHECK_ERR(parse_wav(Bytes{1, 2, 3}).error(), Error::kTruncated);
    Bytes junk(64, 0);
    CHECK_ERR(parse_wav(junk).error(), Error::kBadMagic);
    CHECK_ERR(parse_wav(make_wav(1, 1, 48000, 8, Bytes(8, 0))).error(), Error::kUnsupportedFormat);
    CHECK_ERR(parse_wav(make_wav(1, 2, 11025, 16, Bytes(8, 0))).error(), Error::kUnsupportedFormat);
    CHECK_ERR(parse_wav(make_wav(1, 1, 48000, 32, Bytes(8, 0))).error(), Error::kUnsupportedFormat);
    CHECK_ERR(parse_wav(make_wav(1, 0, 48000, 16, Bytes(8, 0))).error(), Error::kUnsupportedFormat);
    Bytes ok = make_wav(1, 1, 48000, 16, Bytes(8, 0));
    // Truncation never crashes; before the data payload it always errors, inside it the data
    // size is clamped to what is there (partial frames dropped).
    for (std::size_t n = 0; n < ok.size(); ++n) {
      auto t = parse_wav(std::span<const std::uint8_t>(ok.data(), n));
      if (n < 44) CHECK(!t.ok());
      else CHECK(t.ok() && t.value().frame_count == (n - 44) / 2);
    }
    // kCorruptHeader branches.
    { Bytes b = make_wav(1, 2, 48000, 16, Bytes(8, 0)); b[32] = 3;  // block align mismatch
      CHECK_ERR(parse_wav(b).error(), Error::kCorruptHeader); }
    { Bytes b; tag(b, "RIFF"); w32(b, 0); tag(b, "WAVE"); tag(b, "data"); w32(b, 4);
      b.insert(b.end(), 4, 0);  // data before fmt
      CHECK_ERR(parse_wav(b).error(), Error::kCorruptHeader); }
    { Bytes b = make_wav(1, 1, 48000, 16, Bytes(4, 0), true);  // extensible with fmt size < 40
      b[16] = 18; b[17] = 0;
      b.erase(b.begin() + 20 + 18, b.begin() + 20 + 40);  // keep chunk consistent
      CHECK_ERR(parse_wav(b).error(), Error::kCorruptHeader); }
    { // partial trailing frame is dropped (stereo 16-bit, 2 frames + 3 stray bytes)
      Bytes p; for (int i = 0; i < 8; ++i) p.push_back(std::uint8_t(i + 1));
      p.insert(p.end(), {9, 9, 9});
      auto t = parse_wav(make_wav(1, 2, 48000, 16, p));
      CHECK(t.ok() && t.value().frame_count == 2 && t.value().data.size() == 4); }
    { // data size 0xFFFFFFFF (streamed WAV): clamped to the remaining bytes
      Bytes b = make_wav(1, 1, 48000, 16, Bytes(10, 1));
      for (int i = 0; i < 4; ++i) b[40 + i] = 0xFF;
      auto t = parse_wav(b);
      CHECK(t.ok() && t.value().frame_count == 5); }
  }
  {  // Story 5.1: strict mode reports a cut-off data chunk instead of clamping it
    Bytes ok = make_wav(1, 1, 48000, 16, Bytes(8, 0));
    WavOptions strict{true};
    CHECK(parse_wav(ok, strict).ok() && parse_wav(ok, strict).value().frame_count == 4);
    for (std::size_t n = 44; n < ok.size(); ++n) {
      const std::span<const std::uint8_t> cut(ok.data(), n);
      CHECK_ERR(parse_wav(cut, strict).error(), Error::kTruncated);
      CHECK(parse_wav(cut).ok());  // default behaviour unchanged
    }
    Bytes stream = make_wav(1, 1, 48000, 16, Bytes(10, 1));
    for (int i = 0; i < 4; ++i) stream[40 + i] = 0xFF;
    CHECK(parse_wav(stream, strict).ok() && parse_wav(stream, strict).value().frame_count == 5);
  }
  {  // files: real temp file and missing file
    const auto dir = std::filesystem::temp_directory_path();
    const std::string path = (dir / "velora_wav_test.wav").string();
    Bytes p; for (int i = 0; i < 8; ++i) p.push_back(std::uint8_t(i));
    const Bytes wav = make_wav(1, 2, 44100, 16, p);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(wav.data(), 1, wav.size(), f);
    std::fclose(f);
    auto w = load_wav_file(path);
    CHECK(w.ok() && w.value().frame_count == 2);
    std::filesystem::remove(path);
    CHECK_ERR(load_wav_file(path).error(), Error::kIo);
  }
  // WAV (PCM16 and PCM24) -> bundle -> decode is bit-identical to the WAV samples.
  for (unsigned bits : {16u, 24u}) {
    const PcmFormat pf{2, static_cast<std::uint8_t>(bits), false, 44100};
    const auto pcm = vt::make_int(pf, 9000, vt::Kind::kNoise);
    Bytes payload;
    for (std::int32_t v : pcm) {
      const auto u = static_cast<std::uint32_t>(v);
      for (unsigned k = 0; k < bits / 8; ++k) payload.push_back(std::uint8_t(u >> (8 * k)));
    }
    auto w = parse_wav(make_wav(1, 2, 44100, bits, payload));
    CHECK(w.ok() && w.value().data == pcm);
    if (!w.ok()) continue;
    BundleWriter bw;
    const auto i = bw.add_instrument("n", "n.sfz").value();
    const WavData wd = w.take();
    CHECK_ERR(bw.add_sample(i, wd.as_input("s")), Error::kNone);
    const Bytes bytes = bw.finalize().take();
    auto r = BundleReader::open_memory(bytes);
    std::vector<std::int32_t> out(pcm.size());
    CHECK(r.ok() && r.value().read_frames(0, 0, 9000, out) == Error::kNone && out == pcm);
    CHECK(r.ok() && r.value().verify_all() == Error::kNone);
  }
  if (g_fail == 0) std::printf("wav_test OK\n");
  return g_fail == 0 ? 0 : 1;
}
