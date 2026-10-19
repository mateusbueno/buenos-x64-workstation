#include "velora/format/json_lite.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <charconv>
#include <cstdint>
#include <cmath>

namespace velora::format {

const JsonValue* JsonValue::find(std::string_view k) const {
  if (type != Type::kObject) return nullptr;
  for (std::size_t i = 0; i < keys.size(); ++i)
    if (keys[i] == k) return &values[i];
  return nullptr;
}

namespace {

constexpr int kMaxDepth = 32;

void append_utf8(std::string& s, unsigned cp) {
  if (cp < 0x80) {
    s.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// Length of a valid UTF-8 sequence at s[i], or 0.
std::size_t utf8_seq_len(std::string_view s, std::size_t i) {
  const unsigned char c = static_cast<unsigned char>(s[i]);
  std::size_t n = 0;
  unsigned cp = 0, min = 0;
  if (c < 0x80) return 1;
  if (c >= 0xC2 && c <= 0xDF) { n = 2; cp = c & 0x1F; min = 0x80; }
  else if (c >= 0xE0 && c <= 0xEF) { n = 3; cp = c & 0x0F; min = 0x800; }
  else if (c >= 0xF0 && c <= 0xF4) { n = 4; cp = c & 0x07; min = 0x10000; }
  else return 0;
  if (i + n > s.size()) return 0;
  for (std::size_t k = 1; k < n; ++k) {
    const unsigned char d = static_cast<unsigned char>(s[i + k]);
    if ((d & 0xC0) != 0x80) return 0;
    cp = (cp << 6) | (d & 0x3F);
  }
  if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
  return n;
}

class Parser {
 public:
  explicit Parser(std::string_view t) : t_(t) {}
  bool run(JsonValue& out, std::string* error) {
    if (t_.size() >= 3 && static_cast<unsigned char>(t_[0]) == 0xEF && static_cast<unsigned char>(t_[1]) == 0xBB &&
        static_cast<unsigned char>(t_[2]) == 0xBF)
      p_ = 3;
    ws();
    if (!value(out, 0)) return finish(error);
    ws();
    if (p_ != t_.size()) {
      fail("trailing content");
      return finish(error);
    }
    return true;
  }

 private:
  bool finish(std::string* error) {
    if (error) *error = "offset " + std::to_string(err_pos_) + ": " + err_;
    return false;
  }
  bool fail(const char* why) {
    if (err_.empty()) {
      err_ = why;
      err_pos_ = p_;
    }
    return false;
  }
  void ws() {
    while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\t' || t_[p_] == '\n' || t_[p_] == '\r')) ++p_;
  }
  bool lit(std::string_view w) {
    if (t_.substr(p_, w.size()) != w) return fail("invalid literal");
    p_ += w.size();
    return true;
  }
  bool hex4(unsigned& v) {
    if (p_ + 4 > t_.size()) return fail("truncated \\u escape");
    v = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = t_[p_ + i];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
      else return fail("bad \\u escape");
    }
    p_ += 4;
    return true;
  }
  bool str(std::string& out) {
    ++p_;  // opening quote
    for (;;) {
      if (p_ >= t_.size()) return fail("unterminated string");
      const unsigned char c = static_cast<unsigned char>(t_[p_]);
      if (c == '"') {
        ++p_;
        return true;
      }
      if (c < 0x20) return fail("control character in string");
      if (c == '\\') {
        ++p_;
        if (p_ >= t_.size()) return fail("unterminated escape");
        const char e = t_[p_++];
        switch (e) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            unsigned u;
            if (!hex4(u)) return false;
            if (u >= 0xD800 && u <= 0xDBFF) {
              if (t_.substr(p_, 2) != "\\u") return fail("lone high surrogate");
              p_ += 2;
              unsigned lo;
              if (!hex4(lo)) return false;
              if (lo < 0xDC00 || lo > 0xDFFF) return fail("bad low surrogate");
              u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            } else if (u >= 0xDC00 && u <= 0xDFFF) {
              return fail("lone low surrogate");
            }
            append_utf8(out, u);
            break;
          }
          default: --p_; return fail("bad escape");
        }
        continue;
      }
      const std::size_t n = utf8_seq_len(t_, p_);
      if (n == 0) return fail("invalid UTF-8");
      out.append(t_.substr(p_, n));
      p_ += n;
    }
  }
  bool number(JsonValue& v) {
    const std::size_t s = p_;
    if (p_ < t_.size() && t_[p_] == '-') ++p_;
    if (p_ >= t_.size()) return fail("bad number");
    if (t_[p_] == '0') {
      ++p_;
    } else if (t_[p_] >= '1' && t_[p_] <= '9') {
      while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
    } else {
      return fail("bad number");
    }
    if (p_ < t_.size() && t_[p_] == '.') {
      ++p_;
      const std::size_t d = p_;
      while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
      if (p_ == d) return fail("bad number");
    }
    if (p_ < t_.size() && (t_[p_] == 'e' || t_[p_] == 'E')) {
      ++p_;
      if (p_ < t_.size() && (t_[p_] == '+' || t_[p_] == '-')) ++p_;
      const std::size_t d = p_;
      while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
      if (p_ == d) return fail("bad number");
    }
    // std::from_chars is locale independent (strtod would honour a decimal-comma locale).
    v.type = JsonValue::Type::kNumber;
    const char* b = t_.data() + s;
    const auto res = std::from_chars(b, t_.data() + p_, v.number);
    if (res.ec != std::errc() || res.ptr != t_.data() + p_ || !std::isfinite(v.number)) return fail("number out of range");
    return true;
  }
  bool value(JsonValue& v, int depth) {
    if (depth > kMaxDepth) return fail("nesting too deep");
    if (p_ >= t_.size()) return fail("unexpected end");
    const char c = t_[p_];
    if (c == '{') {
      v.type = JsonValue::Type::kObject;
      ++p_;
      ws();
      if (p_ < t_.size() && t_[p_] == '}') {
        ++p_;
        return true;
      }
      for (;;) {
        ws();
        if (p_ >= t_.size() || t_[p_] != '"') return fail("object key expected");
        std::string k;
        if (!str(k)) return false;
        ws();
        if (p_ >= t_.size() || t_[p_] != ':') return fail("':' expected");
        ++p_;
        ws();
        JsonValue m;
        if (!value(m, depth + 1)) return false;
        v.keys.push_back(std::move(k));
        v.values.push_back(std::move(m));
        ws();
        if (p_ < t_.size() && t_[p_] == ',') {
          ++p_;
          continue;
        }
        if (p_ < t_.size() && t_[p_] == '}') {
          ++p_;
          return true;
        }
        return fail("',' or '}' expected");
      }
    }
    if (c == '[') {
      v.type = JsonValue::Type::kArray;
      ++p_;
      ws();
      if (p_ < t_.size() && t_[p_] == ']') {
        ++p_;
        return true;
      }
      for (;;) {
        ws();
        JsonValue m;
        if (!value(m, depth + 1)) return false;
        v.array.push_back(std::move(m));
        ws();
        if (p_ < t_.size() && t_[p_] == ',') {
          ++p_;
          continue;
        }
        if (p_ < t_.size() && t_[p_] == ']') {
          ++p_;
          return true;
        }
        return fail("',' or ']' expected");
      }
    }
    if (c == '"') {
      v.type = JsonValue::Type::kString;
      return str(v.string);
    }
    if (c == 't') { v.type = JsonValue::Type::kBool; v.boolean = true; return lit("true"); }
    if (c == 'f') { v.type = JsonValue::Type::kBool; v.boolean = false; return lit("false"); }
    if (c == 'n') { v.type = JsonValue::Type::kNull; return lit("null"); }
    return number(v);
  }

  std::string_view t_;
  std::size_t p_ = 0, err_pos_ = 0;
  std::string err_;
};

}  // namespace

bool parse_json(std::string_view text, JsonValue& out, std::string* error) {
  out = JsonValue{};
  if (text.size() > kMaxJsonBytes) {
    if (error) *error = "offset 0: input too large";
    return false;
  }
  Parser p(text);
  return p.run(out, error);
}

std::size_t utf8_length(std::string_view s) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < s.size(); ++n) {
    const std::size_t l = utf8_seq_len(s, i);
    i += l ? l : 1;
  }
  return n;
}

}  // namespace velora::format

namespace velora::format {

bool read_text_file(const std::string& path, std::string& out, std::string* error, std::size_t max_bytes) {
  auto fail = [&](const std::string& why) {
    if (error) *error = path + ": " + why;
    return false;
  };
  const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) return fail("cannot open");
  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    return fail("not a regular file");
  }
  if (static_cast<std::uint64_t>(st.st_size) > max_bytes) {
    ::close(fd);
    return fail("file too large");
  }
  out.clear();
  char buf[65536];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n < 0) {
      ::close(fd);
      return fail("read error");
    }
    if (n == 0) break;
    out.append(buf, static_cast<std::size_t>(n));
    if (out.size() > max_bytes) {
      ::close(fd);
      return fail("file too large");
    }
  }
  ::close(fd);
  return true;
}

}  // namespace velora::format
