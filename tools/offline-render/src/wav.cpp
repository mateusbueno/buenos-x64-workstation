#include "wav.hpp"

#include <cstring>
#include <fstream>
#include <iterator>

namespace velora::offline {
namespace {
void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}
void put16(std::vector<std::uint8_t>& v, std::uint16_t x) {
  v.push_back(static_cast<std::uint8_t>(x));
  v.push_back(static_cast<std::uint8_t>(x >> 8));
}
void tag(std::vector<std::uint8_t>& v, const char* t) { v.insert(v.end(), t, t + 4); }
std::uint32_t get32(const std::uint8_t* p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t get16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
}  // namespace

std::vector<std::uint8_t> encode_wav(const WavData& wav) {
  const std::uint32_t data_bytes = static_cast<std::uint32_t>(wav.samples.size() * 4);
  std::vector<std::uint8_t> v;
  v.reserve(44 + data_bytes);
  tag(v, "RIFF");
  put32(v, 36 + data_bytes);
  tag(v, "WAVE");
  tag(v, "fmt ");
  put32(v, 16);
  put16(v, 3);  // IEEE float
  put16(v, wav.channels);
  put32(v, wav.sample_rate);
  put32(v, wav.sample_rate * wav.channels * 4);
  put16(v, static_cast<std::uint16_t>(wav.channels * 4));
  put16(v, 32);
  tag(v, "data");
  put32(v, data_bytes);
  for (float f : wav.samples) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, 4);
    put32(v, bits);
  }
  return v;
}

bool write_wav(const std::string& path, const WavData& wav, std::string& error) {
  const auto bytes = encode_wav(wav);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) { error = "cannot open output file: " + path; return false; }
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  out.close();
  if (!out) { error = "write failed: " + path; return false; }
  return true;
}

bool decode_wav(const std::vector<std::uint8_t>& b, WavData& wav, std::string& error) {
  if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0) {
    error = "not a RIFF/WAVE file";
    return false;
  }
  bool have_fmt = false, pcm16 = false;
  std::size_t pos = 12;
  while (pos + 8 <= b.size()) {
    const std::uint32_t len = get32(&b[pos + 4]);
    const std::size_t body = pos + 8;
    if (std::memcmp(&b[pos], "fmt ", 4) == 0) {
      if (len < 16 || body + 16 > b.size()) { error = "bad fmt chunk"; return false; }
      const std::uint16_t fmt = get16(&b[body]);
      wav.channels = get16(&b[body + 2]);
      wav.sample_rate = get32(&b[body + 4]);
      const std::uint16_t bits = get16(&b[body + 14]);
      if (!((fmt == 3 && bits == 32) || (fmt == 1 && bits == 16))) {
        error = "only 32-bit float and 16-bit PCM WAV are supported";
        return false;
      }
      pcm16 = fmt == 1;
      if (wav.channels == 0) { error = "zero channels"; return false; }
      have_fmt = true;
    } else if (std::memcmp(&b[pos], "data", 4) == 0) {
      if (!have_fmt) { error = "data chunk before fmt chunk"; return false; }
      if (body + len > b.size()) { error = "truncated data chunk"; return false; }
      const std::size_t width = pcm16 ? 2 : 4;
      const std::size_t count = len / width;
      wav.samples.resize(count - count % wav.channels);
      wav.from_pcm16 = pcm16;
      for (std::size_t i = 0; i < wav.samples.size(); ++i) {
        if (pcm16) {
          wav.samples[i] = static_cast<float>(static_cast<std::int16_t>(get16(&b[body + i * 2]))) / 32768.0f;
        } else {
          const std::uint32_t bits = get32(&b[body + i * 4]);
          std::memcpy(&wav.samples[i], &bits, 4);
        }
      }
      return true;
    }
    pos = body + len + (len & 1);
  }
  error = "no data chunk";
  return false;
}

bool undo_sfizz_s16(WavData& wav, std::string& error) {
  if (!wav.from_pcm16) { error = "sfizz s16 decode needs a 16-bit PCM file"; return false; }
  for (float& x : wav.samples) x = (x * 32768.0f + 1.0f) / 32767.5f;
  return true;
}

bool read_wav(const std::string& path, WavData& wav, std::string& error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) { error = "cannot open file: " + path; return false; }
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return decode_wav(bytes, wav, error);
}

}  // namespace velora::offline
