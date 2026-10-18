#include <cstring>

#include "layout.hpp"

namespace velora::format {

bool is_supported(const PcmFormat& f) {
  if (f.channels < 1 || f.channels > 2) return false;
  if (f.sample_rate != 44100 && f.sample_rate != 48000 && f.sample_rate != 88200 &&
      f.sample_rate != 96000)
    return false;
  if (f.is_float) return f.bits_per_sample == 32;
  return f.bits_per_sample == 16 || f.bits_per_sample == 24;
}

std::int32_t float_to_bits(float f) {
  std::int32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}
float bits_to_float(std::int32_t b) {
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}

Result<std::string> normalize_instrument_key(std::string_view path) {
  if (path.empty() || path.size() > layout::kMaxStringLen) return Error::kInvalidPath;
  std::string s(path);
  for (char& c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f) return Error::kInvalidPath;
    if (c == '\\') c = '/';
  }
  if (s[0] == '/') return Error::kInvalidPath;  // absolute
  if (s.size() >= 2 && s[1] == ':' &&
      ((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z')))
    return Error::kInvalidPath;  // drive prefix
  std::string out;
  std::size_t i = 0;
  while (i <= s.size()) {
    std::size_t j = s.find('/', i);
    if (j == std::string::npos) j = s.size();
    const std::string_view comp(s.data() + i, j - i);
    if (comp == "..") return Error::kInvalidPath;
    if (!comp.empty() && comp != ".") {
      if (!out.empty()) out.push_back('/');
      out.append(comp);
    }
    i = j + 1;
  }
  if (out.empty()) return Error::kInvalidPath;
  return out;
}

namespace layout {
void pcm_hash_update(Sha256& h, const PcmFormat& f, const std::int32_t* v, std::size_t count) {
  const unsigned nbytes = f.bits_per_sample / 8;
  std::uint8_t buf[4096];
  std::size_t n = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const auto u = static_cast<std::uint32_t>(v[i]);
    for (unsigned b = 0; b < nbytes; ++b) buf[n++] = static_cast<std::uint8_t>(u >> (8 * b));
    if (n + 4 > sizeof buf) {
      h.update(buf, n);
      n = 0;
    }
  }
  if (n) h.update(buf, n);
}
}  // namespace layout

}  // namespace velora::format
