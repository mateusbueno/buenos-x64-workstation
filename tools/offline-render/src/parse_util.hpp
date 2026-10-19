#pragma once

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace velora::offline {

// Strict numeric parsing shared by flags and case files: whole string must be consumed, value finite
// and within [lo, hi].
inline bool parse_double(const std::string& s, double lo, double hi, double& out) {
  if (s.empty()) return false;
  char* e = nullptr;
  errno = 0;
  const double v = std::strtod(s.c_str(), &e);
  if (*e != '\0' || errno == ERANGE || !std::isfinite(v) || v < lo || v > hi) return false;
  out = v;
  return true;
}

inline bool parse_uint(const std::string& s, std::uint64_t hi, std::uint64_t& out) {
  if (s.empty() || s[0] < '0' || s[0] > '9') return false;
  char* e = nullptr;
  errno = 0;
  const unsigned long long v = std::strtoull(s.c_str(), &e, 10);
  if (*e != '\0' || errno == ERANGE || v > hi) return false;
  out = v;
  return true;
}

// A whole signed number in [lo, hi] (optional leading '-', digits only).
inline bool parse_int(const std::string& s, std::int64_t lo, std::int64_t hi, std::int64_t& out) {
  if (s.empty()) return false;
  std::size_t i = (s[0] == '-') ? 1 : 0;
  if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
  char* e = nullptr;
  errno = 0;
  const long long v = std::strtoll(s.c_str(), &e, 10);
  if (*e != '\0' || errno == ERANGE || v < lo || v > hi) return false;
  out = v;
  return true;
}

}  // namespace velora::offline
