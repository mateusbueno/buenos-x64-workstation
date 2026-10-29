#pragma once
// Pairing for the sysd TCP bridge (Foundation: Velora Studio device connection; Story 9.6 Mac scope, "pairing-lite").
//   * PairingWindow: the code and its rules. One code; compared in constant time; five wrong attempts close the window; the window also closes after
//     kWindowMs (2 minutes). The clock is injected so tests do not sleep. A closed window refuses every attempt until reopen().
//   * PairedStore: the paired computers (id, name, last connected, SHA-256 of the pairing token) in `<dir>/paired_computers.txt` (mode 0600), written with the
//     data root's atomic replace (temp file, fsync, rename, directory fsync) like the config store. A computer is paired only by PairRequest with the code; it
//     then holds a random 256-bit pairing token (returned once, only its hash is stored) that it must present in every later Hello. A client_id alone proves
//     nothing. A corrupt or oversize file reads as an empty list AND a new device id (logged: every token is invalid, computers pair again); a v1 file (no tokens)
//     is ignored the same way. Randomness is getentropy() only and fails closed.
// The code is a physical-presence check with ~20 bits (1e6 codes, 5 wrong attempts GLOBAL per window), not cryptography: the USB link is the trust boundary
// (architecture; no TLS).
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace velora::sysd {

// Fills `out` from the OS CSPRNG (getentropy). False when it is unavailable: callers fail closed (no fallback generator exists).
bool secure_random(void* out, std::size_t n);
std::string sha256_hex(const std::string& data);  // lowercase hex of SHA-256

class PairingWindow {
 public:
  static constexpr int kMaxWrong = 5;
  static constexpr std::int64_t kWindowMs = 2 * 60 * 1000;
  static constexpr std::size_t kMinCodeLength = 6;
  enum class Result : std::uint8_t { kOk = 0, kWrong, kClosedAttempts, kClosedExpired, kNotOpen };

  explicit PairingWindow(std::function<std::int64_t()> now_ms) : now_(std::move(now_ms)) {}
  // (Re)opens: a fresh window of kWindowMs, the wrong counter reset. There is no reopen path on a unit before Story 9.6 (the unit-side pairing UI).
  void open(const std::string& code);
  void close();
  Result attempt(const std::string& code);
  int attempts_left() const;
  bool is_open() const;  // open and not expired
  std::string code() const;
  std::int64_t remaining_ms() const;
  void set_active_computer_name(const std::string& name);
  std::string active_computer_name() const;
  static bool random_code(std::string& out);  // six random digits, uniform (rejection sampling); false when no entropy
  static bool valid_code(const std::string& code);  // at least kMinCodeLength characters, digits only
  static bool constant_time_equals(const std::string& a, const std::string& b);

 private:
  std::function<std::int64_t()> now_;
  mutable std::mutex mu_;
  std::string code_;
  bool open_ = false;
  std::int64_t opened_ms_ = 0;
  int wrong_ = 0;
  bool closed_by_attempts_ = false;
  std::string active_computer_name_;
};

struct PairedComputer {
  std::string id, name;
  std::int64_t last_connected_unix_s = 0;
  std::string token_hash;  // SHA-256 hex of the pairing token
};

class PairedStore {
 public:
  static constexpr std::size_t kMaxComputers = 32;
  static constexpr std::size_t kMaxFileBytes = 16384;
  static constexpr std::size_t kMaxNameBytes = 64;
  static constexpr std::size_t kMaxIdBytes = 64;
  PairedStore(std::string dir, std::function<void(const std::string&)> log);
  bool ok() const { return !device_id_.empty(); }  // false: no entropy for the device id (the caller must not start the bridge)
  bool contains(const std::string& id) const;
  // Pairing token check: true only when `id` exists and SHA-256(token) equals its stored hash (constant time). Writes nothing.
  bool verify(const std::string& id, const std::string& token) const;
  // Adds or replaces (new token hash), stamps `now_unix_s`, persists. At the cap the least recently connected computer is evicted. False (nothing changed) when
  // the file cannot be written.
  bool add(const std::string& id, const std::string& name, std::int64_t now_unix_s, const std::string& token_hash);
  bool touch(const std::string& id, std::int64_t now_unix_s);  // last connected; call only after verify()
  bool remove(const std::string& id);
  std::vector<PairedComputer> list() const;
  const std::string& device_id() const { return device_id_; }  // stable id of this unit, created on first use and persisted with the list
  static std::string sanitize_name(const std::string& s);
  static bool valid_id(const std::string& s);
  static bool valid_token(const std::string& s);  // 64 lowercase hex characters

 private:
  bool save_locked();
  std::string dir_;
  std::function<void(const std::string&)> log_;
  mutable std::mutex mu_;
  std::map<std::string, PairedComputer> by_id_;
  std::string device_id_;
};

}  // namespace velora::sysd
