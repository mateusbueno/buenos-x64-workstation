#include "util.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <ctime>

#include "velora/format/hash.hpp"

namespace velora::registry {

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string rfc3339(std::int64_t unix_ms) {
  const std::time_t t = static_cast<std::time_t>(unix_ms / 1000);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

std::string hex_encode(const std::uint8_t* p, std::size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  for (std::size_t i = 0; i < n; ++i) {
    s += d[p[i] >> 4];
    s += d[p[i] & 15];
  }
  return s;
}

static int nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool hex_decode_exact(std::string_view text, std::uint8_t* out, std::size_t n) {
  if (text.size() != n * 2) return false;
  for (std::size_t i = 0; i < n; ++i) {
    const int a = nibble(text[2 * i]), b = nibble(text[2 * i + 1]);
    if (a < 0 || b < 0) return false;
    out[i] = static_cast<std::uint8_t>(a << 4 | b);
  }
  return true;
}
bool hex_decode32(std::string_view text, Key32& out) { return hex_decode_exact(text, out.data(), out.size()); }

std::string json_escape(std::string_view s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (const unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20 || c == 0x7f) {
          char b[8];
          std::snprintf(b, sizeof b, "\\u%04x", c);
          o += b;
        } else {
          o += static_cast<char>(c);
        }
    }
  }
  return o;
}
std::string json_str(std::string_view s) { return "\"" + json_escape(s) + "\""; }

bool read_secret_file(const std::string& path, std::uint8_t* out, std::size_t n, std::string* err) {
  auto bad = [&](const std::string& m) {
    if (err) *err = path + ": " + m;
    return false;
  };
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) return bad(std::strerror(errno));
  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || static_cast<std::size_t>(st.st_size) != n) {
    ::close(fd);
    return bad("not a regular file of exactly " + std::to_string(n) + " bytes");
  }
  if ((st.st_mode & 077) != 0) {
    ::close(fd);
    return bad("permissions are too open (use chmod 600)");
  }
  std::size_t got = 0;
  while (got < n) {
    const ssize_t r = ::read(fd, out + got, n - got);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;
    got += static_cast<std::size_t>(r);
  }
  ::close(fd);
  if (got != n) return bad("short read");
  return true;
}

bool check_private_mode(const std::string& path, std::string* err) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    if (err) *err = path + ": not a readable regular file";
    return false;
  }
  if ((st.st_mode & 077) != 0) {
    if (err) *err = path + ": permissions are too open (use chmod 600)";
    return false;
  }
  return true;
}

bool valid_token(std::string_view s, std::size_t max_len) {
  if (s.empty() || s.size() > max_len) return false;
  auto alnum = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
  if (!alnum(s[0])) return false;
  for (const char c : s)
    if (!alnum(c) && c != '.' && c != '_' && c != ':' && c != '/' && c != '-') return false;
  return true;
}

bool valid_id_token(std::string_view s, std::size_t max_len) {
  if (s.empty() || s.size() > max_len) return false;
  auto alnum = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'); };
  if (!alnum(s[0])) return false;
  for (const char c : s)
    if (!alnum(c) && c != '.' && c != '_' && c != ':' && c != '-') return false;
  return true;
}

bool acceptable_x25519_public(const Key32& k, std::string* why) {
  auto no = [&](const char* m) {
    if (why) *why = m;
    return false;
  };
  if (k[31] & 0x80) return no("bit 255 must be clear");
  bool ge_p = k[31] == 0x7f && k[0] >= 0xed;
  for (int i = 1; i < 31 && ge_p; ++i) ge_p = k[i] == 0xff;
  if (ge_p) return no("the value is not below 2^255-19 (non-canonical)");
  static const char* const kSmallOrder[] = {
      "0000000000000000000000000000000000000000000000000000000000000000",
      "0100000000000000000000000000000000000000000000000000000000000000",
      "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
      "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157",
      "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
  };
  const std::string h = hex_encode(k.data(), k.size());
  for (const char* bad : kSmallOrder)
    if (h == bad) return no("a small-order point is not a device key");
  return true;
}

bool valid_date(std::string_view s) {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
  for (std::size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u})
    if (s[i] < '0' || s[i] > '9') return false;
  const int y = std::stoi(std::string(s.substr(0, 4))), m = std::stoi(std::string(s.substr(5, 2))), d = std::stoi(std::string(s.substr(8, 2)));
  if (y < 2000 || y > 2999 || m < 1 || m > 12 || d < 1) return false;
  static const int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int mx = dim[m - 1];
  if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) mx = 29;
  return d <= mx;
}

bool valid_library_id(std::string_view s) {
  velora::format::Uuid u;
  return s.size() == 36 && velora::format::uuid_from_string(s, u) && velora::format::uuid_to_string(u) == s;
}

}  // namespace velora::registry
