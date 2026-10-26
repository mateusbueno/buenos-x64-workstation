#include "velora/sysd/pairing.hpp"

#include <unistd.h>

#if defined(__APPLE__) || (defined(__linux__) && __has_include(<sys/random.h>))
#include <sys/random.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "velora/host/live_set_store.hpp"

namespace velora::sysd {

namespace {
constexpr const char* kPairedFile = "paired_computers.txt";
constexpr const char* kPairedTmp = "paired_computers.txt.tmp";
constexpr const char* kHeaderV2 = "velora-paired-computers v2";
constexpr const char* kHeaderV1 = "velora-paired-computers v1";  // no tokens: its entries are ignored

std::string to_hex(const unsigned char* b, std::size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (std::size_t i = 0; i < n; ++i) {
    s += d[b[i] >> 4];
    s += d[b[i] & 15];
  }
  return s;
}

bool is_hex64(const std::string& s) {
  if (s.size() != 64) return false;
  for (unsigned char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

// ---- SHA-256 (FIPS 180-4) ---------------------------------------------------------------------------------------------------------------------------
struct Sha256 {
  std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  unsigned char buf[64];
  std::size_t fill = 0;
  std::uint64_t total = 0;
  static std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
  void block(const unsigned char* p) {
    static const std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74,
        0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d,
        0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e,
        0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
        0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) | (std::uint32_t(p[4 * i + 2]) << 8) | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
      const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const std::uint32_t ch = (e & f) ^ (~e & g);
      const std::uint32_t t1 = hh + S1 + ch + k[i] + w[i];
      const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t t2 = S0 + mj;
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  void update(const unsigned char* p, std::size_t n) {
    total += n;
    while (n > 0) {
      const std::size_t take = std::min<std::size_t>(64 - fill, n);
      std::memcpy(buf + fill, p, take);
      fill += take; p += take; n -= take;
      if (fill == 64) { block(buf); fill = 0; }
    }
  }
  std::string finish() {
    const std::uint64_t bits = total * 8;
    unsigned char pad = 0x80;
    update(&pad, 1);
    pad = 0;
    while (fill != 56) update(&pad, 1);
    unsigned char len[8];
    for (int i = 0; i < 8; ++i) len[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
    update(len, 8);
    unsigned char out[32];
    for (int i = 0; i < 8; ++i) { out[4 * i] = h[i] >> 24; out[4 * i + 1] = h[i] >> 16; out[4 * i + 2] = h[i] >> 8; out[4 * i + 3] = static_cast<unsigned char>(h[i]); }
    return to_hex(out, 32);
  }
};

std::string hex_random(std::size_t bytes) {  // "" when there is no entropy
  unsigned char b[32] = {};
  if (bytes > sizeof b || !secure_random(b, bytes)) return {};
  return to_hex(b, bytes);
}
}  // namespace

bool secure_random(void* out, std::size_t n) {
  auto* p = static_cast<unsigned char*>(out);
  while (n > 0) {
    const std::size_t take = std::min<std::size_t>(n, 256);  // getentropy's per-call limit
    if (::getentropy(p, take) != 0) return false;
    p += take;
    n -= take;
  }
  return true;
}

std::string sha256_hex(const std::string& data) {
  Sha256 s;
  s.update(reinterpret_cast<const unsigned char*>(data.data()), data.size());
  return s.finish();
}

// --- PairingWindow ---------------------------------------------------------------------------------------------------------------------------------
void PairingWindow::open(const std::string& code) {
  std::lock_guard<std::mutex> lk(mu_);
  code_ = code;
  open_ = true;
  opened_ms_ = now_();
  wrong_ = 0;
  closed_by_attempts_ = false;
}

void PairingWindow::close() {
  std::lock_guard<std::mutex> lk(mu_);
  open_ = false;
}

bool PairingWindow::constant_time_equals(const std::string& a, const std::string& b) {
  unsigned diff = static_cast<unsigned>(a.size() ^ b.size());
  const std::size_t n = std::max(a.size(), b.size());
  for (std::size_t i = 0; i < n; ++i) {
    const unsigned char x = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
    const unsigned char y = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
    diff |= static_cast<unsigned>(x ^ y);
  }
  return diff == 0;
}

PairingWindow::Result PairingWindow::attempt(const std::string& code) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!open_) return closed_by_attempts_ ? Result::kClosedAttempts : Result::kNotOpen;
  if (now_() - opened_ms_ >= kWindowMs) {
    open_ = false;
    return Result::kClosedExpired;
  }
  if (constant_time_equals(code, code_)) {
    open_ = false;  // a successful pairing closes the window: one code pairs one computer
    return Result::kOk;
  }
  if (++wrong_ >= kMaxWrong) {
    open_ = false;
    closed_by_attempts_ = true;
    return Result::kClosedAttempts;
  }
  return Result::kWrong;
}

int PairingWindow::attempts_left() const {
  std::lock_guard<std::mutex> lk(mu_);
  return open_ ? kMaxWrong - wrong_ : 0;
}

bool PairingWindow::is_open() const {
  std::lock_guard<std::mutex> lk(mu_);
  return open_ && now_() - opened_ms_ < kWindowMs;
}

bool PairingWindow::random_code(std::string& out) {
  for (int tries = 0; tries < 64; ++tries) {
    std::uint32_t v = 0;
    if (!secure_random(&v, sizeof v)) return false;
    if (v >= 4294000000u) continue;  // the largest multiple of 1e6 below 2^32: no modulo bias
    char buf[16];
    std::snprintf(buf, sizeof buf, "%06u", v % 1000000u);
    out = buf;
    return true;
  }
  return false;
}

bool PairingWindow::valid_code(const std::string& code) {
  if (code.size() < kMinCodeLength) return false;
  for (unsigned char c : code)
    if (c < '0' || c > '9') return false;
  return true;
}

// --- PairedStore -----------------------------------------------------------------------------------------------------------------------------------
std::string PairedStore::sanitize_name(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c < 0x20 || c == 0x7f) continue;  // control characters (this also keeps the file format's tab / newline separators out of names)
    if (o.size() >= kMaxNameBytes) break;
    o += static_cast<char>(c);
  }
  return o;
}

bool PairedStore::valid_id(const std::string& s) {
  if (s.empty() || s.size() > kMaxIdBytes) return false;
  for (unsigned char c : s)
    if (!(std::isalnum(c) || c == '-' || c == '_' || c == '.')) return false;
  return true;
}

bool PairedStore::valid_token(const std::string& s) { return is_hex64(s); }

PairedStore::PairedStore(std::string dir, std::function<void(const std::string&)> log) : dir_(std::move(dir)), log_(std::move(log)) {
  if (!log_) log_ = [](const std::string&) {};
  std::string bytes;
  const auto st = host::liveset::read_regular_file(dir_ + "/" + kPairedFile, kMaxFileBytes, bytes);
  bool bad = false;
  if (st == host::liveset::Status::kOk) {
    std::istringstream in(bytes);
    std::string line;
    if (!std::getline(in, line) || (line != kHeaderV2 && line != kHeaderV1)) {
      bad = true;
    } else if (line == kHeaderV1) {
      log_("paired computers: the list is the old format without pairing tokens; every computer must pair again");
      while (std::getline(in, line))
        if (line.rfind("device_id=", 0) == 0 && valid_id(line.substr(10))) device_id_ = line.substr(10);
    } else {
      while (!bad && std::getline(in, line)) {
        if (line.empty()) continue;
        if (line.rfind("device_id=", 0) == 0) {
          device_id_ = line.substr(10);
          if (!valid_id(device_id_)) bad = true;
          continue;
        }
        std::vector<std::string> f;
        std::size_t p = 0;
        for (;;) {
          const std::size_t q = line.find('\t', p);
          f.push_back(line.substr(p, q == std::string::npos ? q : q - p));
          if (q == std::string::npos) break;
          p = q + 1;
        }
        if (f.size() != 4 || !valid_id(f[0]) || !is_hex64(f[3]) || by_id_.size() >= kMaxComputers) {
          bad = true;
          break;
        }
        PairedComputer pc;
        pc.id = f[0];
        pc.name = sanitize_name(f[1]);
        pc.last_connected_unix_s = std::strtoll(f[2].c_str(), nullptr, 10);
        pc.token_hash = f[3];
        by_id_[pc.id] = pc;
      }
    }
    if (bad) {
      // Chosen policy: a corrupt list also gets a NEW device id. Every computer's token was bound to the old id, so none is ever presented again: they
      // pair again instead of a stale token meeting an empty list.
      log_("paired computers: " + std::string(kPairedFile) + " is corrupt; starting with an empty list and a new device id (every computer must pair again)");
      by_id_.clear();
      device_id_.clear();
    }
  } else if (st != host::liveset::Status::kNotFound) {
    log_("paired computers: cannot read " + std::string(kPairedFile) + "; starting with an empty list and a new device id");
  }
  if (device_id_.empty()) {
    const std::string r = hex_random(6);
    if (r.empty()) {
      log_("paired computers: no entropy for the device id");
      return;  // ok() is false
    }
    device_id_ = "velora-" + r;
  }
  std::lock_guard<std::mutex> lk(mu_);
  if (!save_locked()) log_("paired computers: cannot persist the list");
}

bool PairedStore::save_locked() {
  std::string out = std::string(kHeaderV2) + "\ndevice_id=" + device_id_ + "\n";
  for (const auto& kv : by_id_)
    out += kv.second.id + "\t" + kv.second.name + "\t" + std::to_string(kv.second.last_connected_unix_s) + "\t" + kv.second.token_hash + "\n";
  const auto st = host::liveset::atomic_replace_file(dir_, kPairedFile, kPairedTmp, out, nullptr, false, 0, 0600);
  return st == host::liveset::Status::kOk || st == host::liveset::Status::kCommittedNotDurable;
}

bool PairedStore::contains(const std::string& id) const {
  std::lock_guard<std::mutex> lk(mu_);
  return by_id_.count(id) != 0;
}

bool PairedStore::verify(const std::string& id, const std::string& token) const {
  const std::string h = sha256_hex(token);  // always hashed: the work does not depend on the id
  std::lock_guard<std::mutex> lk(mu_);
  auto it = by_id_.find(id);
  const std::string stored = it == by_id_.end() ? std::string(64, '0') : it->second.token_hash;
  const bool eq = PairingWindow::constant_time_equals(h, stored);
  return eq && it != by_id_.end() && is_hex64(token);
}

bool PairedStore::add(const std::string& id, const std::string& name, std::int64_t now, const std::string& token_hash) {
  if (!valid_id(id) || !is_hex64(token_hash)) return false;
  std::lock_guard<std::mutex> lk(mu_);
  auto it = by_id_.find(id);
  const bool existed = it != by_id_.end();
  const PairedComputer old = existed ? it->second : PairedComputer{};
  PairedComputer evicted;
  bool did_evict = false;
  if (!existed && by_id_.size() >= kMaxComputers) {  // evict the least recently connected
    auto lru = std::min_element(by_id_.begin(), by_id_.end(), [](const auto& a, const auto& b) { return a.second.last_connected_unix_s < b.second.last_connected_unix_s; });
    evicted = lru->second;
    by_id_.erase(lru);
    did_evict = true;
  }
  by_id_[id] = PairedComputer{id, sanitize_name(name), now, token_hash};
  if (!save_locked()) {
    if (existed) by_id_[id] = old; else by_id_.erase(id);
    if (did_evict) by_id_[evicted.id] = evicted;
    return false;
  }
  return true;
}

bool PairedStore::touch(const std::string& id, std::int64_t now) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = by_id_.find(id);
  if (it == by_id_.end()) return false;
  const std::int64_t old = it->second.last_connected_unix_s;
  it->second.last_connected_unix_s = now;
  if (!save_locked()) {
    it->second.last_connected_unix_s = old;
    return false;
  }
  return true;
}

bool PairedStore::remove(const std::string& id) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = by_id_.find(id);
  if (it == by_id_.end()) return false;
  const PairedComputer old = it->second;
  by_id_.erase(it);
  if (!save_locked()) {
    by_id_[id] = old;
    return false;
  }
  return true;
}

std::vector<PairedComputer> PairedStore::list() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<PairedComputer> v;
  for (const auto& kv : by_id_) v.push_back(kv.second);
  return v;
}

}  // namespace velora::sysd
