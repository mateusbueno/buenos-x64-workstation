#include "vendor_register.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "velora/format/json_lite.hpp"

namespace velora::contentpack {
namespace fmt = velora::format;

const RegisterEntry* VendorRegister::find_id(const std::string& id) const {
  for (const auto& e : entries)
    if (e.library_id == id) return &e;
  return nullptr;
}
const RegisterEntry* VendorRegister::find_name(const std::string& name) const {
  const std::string key = normalize_name(name);
  for (const auto& e : entries)
    if (normalize_name(e.name) == key) return &e;
  return nullptr;
}

bool is_random_v4(const fmt::Uuid& u) { return (u[6] >> 4) == 4 && (u[8] & 0xc0) == 0x80; }

namespace {

Failure io(const std::string& m) { return fail(Code::kRegisterIo, m); }

bool field_ok(const std::string& s) {
  if (s.empty() || s.size() > 256) return false;
  if (has_format_chars(s) || normalize_name(s).empty()) return false;
  for (unsigned char c : s)
    if (c < 0x20 || c == 0x7f) return false;  // TAB and newline included
  return true;
}

bool date_ok(const std::string& d) {
  if (d.size() != 10 || d[4] != '-' || d[7] != '-') return false;
  for (std::size_t i = 0; i < 10; ++i)
    if (i != 4 && i != 7 && (d[i] < '0' || d[i] > '9')) return false;
  const int m = (d[5] - '0') * 10 + (d[6] - '0'), day = (d[8] - '0') * 10 + (d[9] - '0');
  return m >= 1 && m <= 12 && day >= 1 && day <= 31;
}

std::string line_of(const RegisterEntry& e) { return e.library_id + "\t" + e.name + "\t" + e.owner + "\t" + e.date + "\n"; }

Failure parse_register(const std::string& text, const std::string& path, VendorRegister& out) {
  out.entries.clear();
  std::size_t pos = 0, line_no = 0;
  while (pos < text.size()) {
    std::size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    std::string line = text.substr(pos, nl - pos);
    pos = nl + 1;
    ++line_no;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> f;
    std::size_t b = 0;
    for (;;) {
      const std::size_t t = line.find('\t', b);
      f.push_back(line.substr(b, t == std::string::npos ? std::string::npos : t - b));
      if (t == std::string::npos) break;
      b = t + 1;
    }
    const std::string where = path + ":" + std::to_string(line_no) + ": ";
    if (f.size() != 4) return io(where + "expected 4 tab-separated fields (library_id, name, owner, date)");
    fmt::Uuid u{};
    if (!fmt::uuid_from_string(f[0], u) || fmt::uuid_to_string(u) != f[0]) return io(where + "library_id must be a lower-case canonical UUID");
    if (u == fmt::Uuid{}) return io(where + "the nil UUID cannot be assigned");
    if (is_dev_range_id(u)) return io(where + "the dev/test range 00000000-0000-4000-8000-xxxxxxxxxxxx is never registered");
    if (!is_random_v4(u)) return io(where + "library_id must be a random (version 4) UUID");
    if (!field_ok(f[1]) || !field_ok(f[2])) return io(where + "name and owner must be 1 to 256 characters without control characters");
    if (!date_ok(f[3])) return io(where + "date must be YYYY-MM-DD");
    RegisterEntry e{f[0], f[1], f[2], f[3]};
    if (out.find_id(e.library_id)) return io(where + "library_id " + e.library_id + " is listed twice");
    if (const RegisterEntry* n = out.find_name(e.name)) return io(where + "name '" + e.name + "' is already assigned to " + n->library_id + " (names compare trimmed, space-collapsed and ASCII case-folded)");
    out.entries.push_back(std::move(e));
  }
  return {};
}

}  // namespace

Failure load_register(const std::string& path, VendorRegister& out, bool must_exist) {
  out.entries.clear();
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    if (!must_exist) return {};
    return io(path + ": the vendor ID register does not exist");
  }
  std::string text, err;
  if (!fmt::read_text_file(path, text, &err)) return io(path + ": " + err);
  return parse_register(text, path, out);
}

Failure check_register(const VendorRegister& reg, const PackDescription& d, RegisterState& state) {
  const RegisterEntry* byid = reg.find_id(d.library_id);
  const RegisterEntry* byname = reg.find_name(d.name);
  if (byid && normalize_name(byid->name) != normalize_name(d.name))
    return fail(Code::kLibraryIdConflict, "library_id " + d.library_id + " is already assigned to the library '" + byid->name + "' (owner " + byid->owner +
                                              "), not '" + d.name + "'");
  if (is_dev_range_id(d.library_uuid)) {
    state = RegisterState::kDevRange;  // never in the register (parse_register refuses it)
    return {};
  }
  if (byname && byname->library_id != d.library_id)
    return fail(Code::kLibraryIdConflict, "the library '" + d.name + "' is already registered under library_id " + byname->library_id +
                                              ", not " + d.library_id);
  if (byid) state = RegisterState::kRegistered;
  else if (is_dev_range_id(d.library_uuid)) state = RegisterState::kDevRange;
  else state = RegisterState::kNew;
  return {};
}

RegisterLock::~RegisterLock() {
  if (fd_ >= 0) ::close(fd_);
}

Failure RegisterLock::acquire(const std::string& register_path) {
  const std::string lock_path = register_path + ".lock";
  fd_ = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
  if (fd_ < 0) return io(lock_path + ": " + std::strerror(errno));
  if (::flock(fd_, LOCK_EX) != 0) {
    const std::string why = std::strerror(errno);
    ::close(fd_);
    fd_ = -1;
    return io(lock_path + ": " + why);
  }
  return {};
}

Failure append_register_locked(const std::string& path, const PackDescription& d, const std::string& owner, const std::string& date) {
  if (!field_ok(owner)) return io("owner must be 1 to 256 characters without control or invisible characters");
  if (!date_ok(date)) return io("date must be YYYY-MM-DD");
  if (is_dev_range_id(d.library_uuid)) return fail(Code::kLibraryIdConflict, "the dev/test id range (00000000-0000-4000-8000-...) is never registered");
  if (!is_random_v4(d.library_uuid)) return fail(Code::kBadDescription, "only a random (version 4) UUID can be registered as a vendor library_id");
  VendorRegister reg;
  if (Failure f = load_register(path, reg, true)) return f;
  RegisterState st;
  if (Failure f = check_register(reg, d, st)) return f;
  if (st == RegisterState::kRegistered) return {};  // identical entry: nothing to do
  std::string text, err;
  if (!fmt::read_text_file(path, text, &err)) return io(path + ": " + err);
  if (!text.empty() && text.back() != '\n') text.push_back('\n');
  text += line_of(RegisterEntry{d.library_id, d.name, owner, date});

  const std::string tmp = path + ".tmp";
  ::unlink(tmp.c_str());
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
  if (fd < 0) return io(tmp + ": " + std::strerror(errno));
  std::size_t done = 0;
  bool ok = true;
  while (ok && done < text.size()) {
    const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) ok = false;
    else done += static_cast<std::size_t>(n);
  }
  ok = ::fsync(fd) == 0 && ok;
  ok = ::close(fd) == 0 && ok;
  if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return io(path + ": cannot write the register");
  }
  std::string dir = std::filesystem::path(path).parent_path().string();
  if (dir.empty()) dir = ".";
  const int dfd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
  const bool synced = dfd >= 0 && ::fsync(dfd) == 0;
  if (dfd >= 0) ::close(dfd);
  if (!synced) return io(dir + ": cannot fsync the register directory");
  return {};
}

Failure append_register(const std::string& path, const PackDescription& d, const std::string& owner, const std::string& date) {
  RegisterLock lock;
  if (Failure f = lock.acquire(path)) return f;
  return append_register_locked(path, d, owner, date);
}

}  // namespace velora::contentpack
