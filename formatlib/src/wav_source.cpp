#include "velora/format/wav_source.hpp"

#include <cstdio>
#include <cstring>

#include "layout.hpp"

namespace velora::format {
using namespace layout;

namespace {
std::uint32_t u16(const std::uint8_t* p) { return p[0] | (p[1] << 8); }
}  // namespace

Result<WavData> parse_wav(std::span<const std::uint8_t> b, WavOptions opt) {
  if (b.size() < 12) return Error::kTruncated;
  if (std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0)
    return Error::kBadMagic;
  bool have_fmt = false;
  std::uint32_t tag = 0, rate = 0, align = 0, bits = 0, chans = 0;
  std::size_t pos = 12;
  while (pos + 8 <= b.size()) {
    const std::uint8_t* ck = b.data() + pos;
    std::uint64_t size = get_u32(ck + 4);
    const std::size_t body = pos + 8;
    if (std::memcmp(ck, "fmt ", 4) == 0) {
      if (size < 16 || body + size > b.size()) return Error::kTruncated;
      const std::uint8_t* f = b.data() + body;
      tag = u16(f);
      chans = u16(f + 2);
      rate = get_u32(f + 4);
      align = u16(f + 12);
      bits = u16(f + 14);
      if (tag == 0xFFFE) {  // extensible: sub-format GUID starts with the real tag
        if (size < 40) return Error::kCorruptHeader;
        tag = u16(f + 24);
      }
      have_fmt = true;
    } else if (std::memcmp(ck, "data", 4) == 0) {
      if (!have_fmt) return Error::kCorruptHeader;
      if (size > b.size() - body) {
        if (opt.reject_truncated_data && size != 0xFFFFFFFFu) return Error::kTruncated;
        size = b.size() - body;  // clamp (also 0xFFFFFFFF streams)
      }
      WavData w;
      w.format.channels = static_cast<std::uint8_t>(chans > 255 ? 255 : chans);
      w.format.bits_per_sample = static_cast<std::uint8_t>(bits > 255 ? 255 : bits);
      w.format.sample_rate = rate;
      w.format.is_float = (tag == 3);
      if ((tag != 1 && tag != 3) || !is_supported(w.format)) return Error::kUnsupportedFormat;
      const std::uint32_t bytes_per = bits / 8;
      if (align != bytes_per * chans) return Error::kCorruptHeader;
      const std::uint64_t values = size / bytes_per;
      w.frame_count = values / chans;  // a partial trailing frame is ignored
      w.data.resize(w.frame_count * chans);
      const std::uint8_t* d = b.data() + body;
      for (std::size_t i = 0; i < w.data.size(); ++i, d += bytes_per) {
        if (bits == 16) {
          w.data[i] = static_cast<std::int16_t>(u16(d));
        } else if (bits == 24) {
          std::uint32_t v = d[0] | (d[1] << 8) | (d[2] << 16);
          w.data[i] = (v & 0x800000) ? static_cast<std::int32_t>(v | 0xFF000000u)
                                     : static_cast<std::int32_t>(v);
        } else {
          w.data[i] = static_cast<std::int32_t>(get_u32(d));
        }
      }
      return w;
    }
    size += size & 1;  // chunks are word aligned
    if (size > b.size() - body) return Error::kTruncated;
    pos = body + size;
  }
  return Error::kTruncated;  // no data chunk
}

Result<WavData> load_wav_file(const std::string& path, WavOptions opt) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return Error::kIo;
  std::vector<std::uint8_t> bytes;
  std::uint8_t buf[65536];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
  const bool bad = std::ferror(f) != 0;
  std::fclose(f);
  if (bad) return Error::kIo;
  return parse_wav(bytes, opt);
}

}  // namespace velora::format
