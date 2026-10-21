#include "backup.hpp"

#include <fcntl.h>
#include <sqlite3.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <filesystem>

#include "database.hpp"
#include "velora/format/hash.hpp"

namespace velora::registry {
namespace cp = velora::contentpack;
namespace fmt = velora::format;
namespace fs = std::filesystem;
namespace {

constexpr char kInfo[] = "velora/registry-backup/v1";

void put_be(Bytes& b, std::uint64_t v, int n) {
  for (int i = n - 1; i >= 0; --i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xff));
}
std::uint64_t get_be(const std::uint8_t* p, int n) {
  std::uint64_t v = 0;
  for (int i = 0; i < n; ++i) v = v << 8 | p[i];
  return v;
}

void derive_key(const Key32& backup_key, const std::uint8_t* salt32, Key32& out) {
  cp::hkdf32(backup_key, std::span<const std::uint8_t>(salt32, 32), std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(kInfo), sizeof kInfo - 1), out);
}

cp::Nonce12 chunk_nonce(const std::uint8_t* prefix4, std::uint64_t index) {
  cp::Nonce12 n{};
  std::memcpy(n.data(), prefix4, 4);
  for (int i = 0; i < 8; ++i) n[4 + i] = static_cast<std::uint8_t>((index >> (8 * (7 - i))) & 0xff);
  return n;
}

Bytes chunk_aad(const std::uint8_t* header, std::uint64_t index, bool last) {
  Bytes aad(header, header + kBackupHeaderSize);
  put_be(aad, index, 8);
  aad.push_back(last ? 1 : 0);
  return aad;
}

bool write_atomic(const std::string& path, const Bytes& data, bool replace, std::string* err) {
  auto bad = [&](const std::string& m) {
    if (err) *err = path + ": " + m;
    return false;
  };
  std::error_code ec;
  if (!replace && fs::exists(fs::symlink_status(path, ec))) return bad("already exists");
  const std::string tmp = path + ".tmp." + std::to_string(::getpid());
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) return bad(std::strerror(errno));
  std::size_t off = 0;
  while (off < data.size()) {
    const ssize_t w = ::write(fd, data.data() + off, data.size() - off);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) {
      ::close(fd);
      ::unlink(tmp.c_str());
      return bad(std::strerror(errno));
    }
    off += static_cast<std::size_t>(w);
  }
  if (::fsync(fd) != 0 || ::close(fd) != 0) {
    ::unlink(tmp.c_str());
    return bad(std::strerror(errno));
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return bad(std::strerror(errno));
  }
  const std::string dir = fs::path(path).parent_path().empty() ? "." : fs::path(path).parent_path().string();
  const int dfd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
  if (dfd >= 0) {
    (void)::fsync(dfd);
    ::close(dfd);
  }
  return true;
}

// Snapshot of `db_path` as the bytes of a SQLite database file.
bool snapshot(const std::string& db_path, Bytes& out, AuditVerify& av, AuditHead& head, std::string* err) {
  auto bad = [&](const std::string& m) {
    if (err) *err = m;
    return false;
  };
  sqlite3 *src = nullptr, *mem = nullptr;
  if (sqlite3_open_v2(db_path.c_str(), &src, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    sqlite3_close(src);
    return bad(db_path + ": cannot open the database");
  }
  sqlite3_busy_timeout(src, 10000);
  if (sqlite3_open_v2(":memory:", &mem, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
    sqlite3_close(src);
    sqlite3_close(mem);
    return bad("cannot open the in-memory snapshot database");
  }
  bool ok = false;
  if (sqlite3_backup* b = sqlite3_backup_init(mem, "main", src, "main")) {
    int rc;
    while ((rc = sqlite3_backup_step(b, -1)) == SQLITE_BUSY || rc == SQLITE_LOCKED) sqlite3_sleep(50);
    ok = sqlite3_backup_finish(b) == SQLITE_OK && rc == SQLITE_DONE;
  }
  if (!ok) {
    const std::string m = sqlite3_errmsg(mem);
    sqlite3_close(src);
    sqlite3_close(mem);
    return bad("the online backup failed: " + m);
  }
  sqlite3_int64 n = 0;
  unsigned char* p = sqlite3_serialize(mem, "main", &n, 0);
  if (!p) {
    sqlite3_close(src);
    sqlite3_close(mem);
    return bad("cannot serialize the snapshot");
  }
  out.assign(p, p + n);
  sqlite3_free(p);
  av = Database::verify_audit(mem);
  head = Database::head_of(mem);
  sqlite3_close(src);
  sqlite3_close(mem);
  return true;
}

std::string stamp(std::int64_t unix_s) {
  const std::time_t t = static_cast<std::time_t>(unix_s);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y%m%dT%H%M%SZ", &tm);
  return buf;
}

}  // namespace

Bytes encrypt_backup(const Bytes& pt, const Key32& backup_key, std::int64_t now_unix) {
  Bytes file;
  file.insert(file.end(), {'V', 'R', 'B', '1', 0x01, 0x01, 0, 0});
  put_be(file, kBackupChunkSize, 4);
  std::uint8_t salt[32], prefix[4];
  cp::random_bytes(salt);
  cp::random_bytes(prefix);
  file.insert(file.end(), salt, salt + 32);
  file.insert(file.end(), prefix, prefix + 4);
  put_be(file, pt.size(), 8);
  put_be(file, static_cast<std::uint64_t>(now_unix), 8);
  cp::Secret<Key32> key;
  derive_key(backup_key, salt, key.get());
  const Bytes header(file.begin(), file.end());
  const std::uint64_t chunks = pt.empty() ? 1 : (pt.size() + kBackupChunkSize - 1) / kBackupChunkSize;
  for (std::uint64_t i = 0; i < chunks; ++i) {
    const std::size_t off = static_cast<std::size_t>(i) * kBackupChunkSize;
    const std::size_t len = std::min<std::size_t>(kBackupChunkSize, pt.size() - off);
    const bool last = i + 1 == chunks;
    const Bytes aad = chunk_aad(header.data(), i, last);
    const std::size_t at = file.size();
    file.resize(at + len + 16);
    cp::gcm_seal(key.get(), chunk_nonce(prefix, i), aad, std::span<const std::uint8_t>(pt.data() + off, len), file.data() + at, file.data() + at + len);
  }
  return file;
}

bool decrypt_backup(const Bytes& f, const Key32& backup_key, Bytes& pt, std::string* err) {
  auto bad = [&](const char* m) {
    if (err) *err = m;
    pt.clear();
    return false;
  };
  pt.clear();
  if (f.size() < kBackupHeaderSize + 16 || std::memcmp(f.data(), "VRB1", 4) != 0) return bad("not a registry backup (bad magic or too short)");
  if (f[4] != 0x01 || f[5] != 0x01 || f[6] != 0 || f[7] != 0) return bad("unsupported backup version or algorithm");
  const std::uint64_t chunk = get_be(f.data() + 8, 4);
  if (chunk != kBackupChunkSize) return bad("unsupported chunk size");
  const std::uint64_t len = get_be(f.data() + 48, 8);
  if (len > (std::uint64_t{1} << 36)) return bad("implausible plaintext length");
  const std::uint64_t chunks = len == 0 ? 1 : (len + chunk - 1) / chunk;
  if (f.size() != kBackupHeaderSize + len + 16 * chunks) return bad("the backup file has the wrong size (truncated or extended)");
  cp::Secret<Key32> key;
  derive_key(backup_key, f.data() + 12, key.get());
  const std::uint8_t* prefix = f.data() + 44;
  pt.resize(static_cast<std::size_t>(len));
  std::size_t in = kBackupHeaderSize;
  for (std::uint64_t i = 0; i < chunks; ++i) {
    const std::size_t off = static_cast<std::size_t>(i * chunk);
    const std::size_t n = std::min<std::uint64_t>(chunk, len - off);
    const Bytes aad = chunk_aad(f.data(), i, i + 1 == chunks);
    if (!cp::gcm_open(key.get(), chunk_nonce(prefix, i), aad, f.data() + in, n, f.data() + in + n, pt.data() + off)) {
      cp::wipe(pt.data(), pt.size());
      return bad("authentication failed: wrong backup key, or the file is damaged");
    }
    in += n + 16;
  }
  return true;
}

namespace {

// "a/b" is inside "a" (or equal to it), compared as canonical path components.
bool path_within(const fs::path& inner, const fs::path& outer) {
  auto i = inner.begin(), o = outer.begin();
  for (; o != outer.end(); ++o, ++i)
    if (i == inner.end() || *i != *o) return false;
  return true;
}

std::string anchor_mac(const Key32& key, std::int64_t seq, const std::string& hash_hex) {
  const std::string m = std::string("velora/registry-anchor/v1") + '\0' + std::to_string(seq) + '\0' + hash_hex;
  const fmt::Digest256 d = fmt::hmac_sha256(key, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(m.data()), m.size()));
  return hex_encode(d.data(), d.size());
}

}  // namespace

bool read_anchor_file(const std::string& path, const Key32* key, Anchor& out, std::string* err) {
  auto bad = [&](const std::string& m) {
    if (err) *err = path + ": " + m;
    return false;
  };
  const Bytes raw = [&] {
    Bytes b;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return b;
    char buf[512];
    ssize_t n;
    while (b.size() < 4096 && (n = ::read(fd, buf, sizeof buf)) > 0) b.insert(b.end(), buf, buf + n);
    ::close(fd);
    return b;
  }();
  if (raw.empty()) return bad("cannot read the anchor");
  out = Anchor{};
  out.path = path;
  std::string text(raw.begin(), raw.end());
  bool have_seq = false;
  for (std::size_t pos = 0; pos < text.size();) {
    std::size_t e = text.find('\n', pos);
    if (e == std::string::npos) e = text.size();
    const std::string line = text.substr(pos, e - pos);
    pos = e + 1;
    if (line.rfind("seq=", 0) == 0) {
      const std::string v = line.substr(4);
      if (v.empty() || v.size() > 15 || v.find_first_not_of("0123456789") != std::string::npos) return bad("malformed seq");
      out.seq = std::stoll(v);
      have_seq = true;
    } else if (line.rfind("hash=", 0) == 0) out.hash_hex = line.substr(5);
    else if (line.rfind("mac=", 0) == 0) out.mac_hex = line.substr(4);
  }
  Key32 tmp;
  if (!have_seq || !hex_decode32(out.hash_hex, tmp) || !hex_decode32(out.mac_hex, tmp)) return bad("malformed anchor");
  if (key && anchor_mac(*key, out.seq, out.hash_hex) != out.mac_hex) return bad("the anchor's MAC does not verify (forged, damaged, or another backup key)");
  return true;
}

int read_latest_anchor(const std::string& dir, const Key32* key, Anchor& out, std::string* err) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    if (err) *err = dir + ": not a directory";
    return -1;
  }
  int found = 0;
  for (const auto& de : fs::directory_iterator(dir, ec)) {
    if (de.path().extension() != ".anchor") continue;
    Anchor a;
    if (!read_anchor_file(de.path().string(), key, a, err)) return -1;
    if (!found || a.seq > out.seq || (a.seq == out.seq && a.path > out.path)) out = a;
    found = 1;
  }
  return found;
}

AnchorCheck check_anchor_file(const std::string& db_path, const Anchor& a) {
  AnchorCheck r;
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    r.message = "cannot open the database to compare it with the anchor";
    return r;
  }
  const AuditHead head = Database::head_of(db);
  if (head.seq < a.seq) {
    r.behind = true;
    r.message = "the database has " + std::to_string(head.seq) + " audit entries but the anchor " + a.path + " records " + std::to_string(a.seq) + ": it was rolled back or truncated";
  } else {
    sqlite3_stmt* st = nullptr;
    std::string have;
    if (sqlite3_prepare_v2(db, "SELECT hash FROM audit WHERE seq = ?1", -1, &st, nullptr) == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, a.seq);
      if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_bytes(st, 0) == 32) have = hex_encode(static_cast<const std::uint8_t*>(sqlite3_column_blob(st, 0)), 32);
    }
    sqlite3_finalize(st);
    if (a.seq == 0 ? head.seq >= 0 : have == a.hash_hex) {
      r.ok = true;
      r.message = "audit chain agrees with anchor " + a.path + " at entry " + std::to_string(a.seq);
    } else {
      r.message = "the audit chain differs from anchor " + a.path + " at entry " + std::to_string(a.seq) + ": the log was rewritten";
    }
  }
  sqlite3_close(db);
  return r;
}

bool backup_database(const std::string& db_path, const Key32& key, const std::string& dest_dir, std::int64_t now_unix, BackupResult& out, std::string* err) {
  auto bad = [&](const std::string& m) {
    if (err) *err = m;
    return false;
  };
  std::error_code ec;
  const fs::path db_dir = fs::weakly_canonical(fs::path(db_path).parent_path().empty() ? fs::path(".") : fs::path(db_path).parent_path(), ec);
  // Judge the destination BEFORE creating anything: equal to, inside, or reached through a symlink or `..` into the database's directory is not a second location.
  const fs::path dest = fs::weakly_canonical(dest_dir, ec);
  if (path_within(dest, db_dir)) return bad("the backup directory is the database's own directory (or inside it); it must be a second location");
  const bool existed = fs::exists(dest_dir, ec);
  fs::create_directories(dest_dir, ec);
  if (ec) return bad(dest_dir + ": " + ec.message());
  if (!existed) ::chmod(dest_dir.c_str(), 0700);  // only a directory we made; an existing one keeps the permissions its owner chose
  Bytes snap;
  AuditVerify av;
  AuditHead head;
  if (!snapshot(db_path, snap, av, head, err)) return false;
  out = BackupResult{};
  out.plaintext_sha256 = fmt::Sha256::hash(snap);
  out.plaintext_bytes = snap.size();
  out.audit_ok = av.ok;
  out.audit_message = av.message;
  out.head_seq = head.seq;
  out.head_hash_hex = hex_encode(head.hash.data(), head.hash.size());
  const Bytes file = encrypt_backup(snap, key, now_unix);
  cp::wipe(snap.data(), snap.size());
  const std::string base = (fs::path(dest_dir) / ("registry-" + stamp(now_unix))).string();
  out.path = base + ".vrb";
  if (!write_atomic(out.path, file, false, err)) return false;
  // the anchor: the audit head of this snapshot, MACed under the backup key
  const std::string anchor = "seq=" + std::to_string(head.seq) + "\nhash=" + out.head_hash_hex + "\nmac=" + anchor_mac(key, head.seq, out.head_hash_hex) + "\n";
  return write_atomic(base + ".anchor", Bytes(anchor.begin(), anchor.end()), false, err);
}

bool restore_backup(const std::string& backup_file, const Key32& key, const std::string& target_db, bool force, BackupResult& out, std::string* err,
                    const std::string& anchor_dir) {
  auto bad = [&](const std::string& m) {
    if (err) *err = m;
    return false;
  };
  Bytes file;
  {
    const int fd = ::open(backup_file.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return bad(backup_file + ": " + std::strerror(errno));
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > (std::int64_t{1} << 34)) {
      ::close(fd);
      return bad(backup_file + ": not a regular file");
    }
    file.resize(static_cast<std::size_t>(st.st_size));
    std::size_t got = 0;
    while (got < file.size()) {
      const ssize_t r = ::read(fd, file.data() + got, file.size() - got);
      if (r < 0 && errno == EINTR) continue;
      if (r <= 0) break;
      got += static_cast<std::size_t>(r);
    }
    ::close(fd);
    if (got != file.size()) return bad(backup_file + ": short read");
  }
  Bytes pt;
  std::string why;
  if (!decrypt_backup(file, key, pt, &why)) return bad(backup_file + ": " + why);
  out = BackupResult{};
  out.plaintext_sha256 = fmt::Sha256::hash(pt);
  out.plaintext_bytes = pt.size();
  out.path = target_db;

  std::error_code ec;
  // Refuse to put a restored database next to a live journal of another one.
  for (const char* suffix : {"-journal", "-wal", "-shm"})
    if (fs::exists(target_db + suffix, ec)) return bad(target_db + suffix + " exists: remove the stale journal of the old database first");
  if (!force && fs::exists(fs::symlink_status(target_db, ec))) return bad(target_db + ": already exists (use --force to replace it)");

  // A running registry holds an flock on <db>.lock for its whole life: never swap a database file out from under it.
  const int lock_fd = ::open((target_db + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lock_fd < 0) return bad(target_db + ".lock: " + std::strerror(errno));
  struct LockGuard { int fd; ~LockGuard() { ::close(fd); } } lock_guard{lock_fd};
  if (::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) return bad(target_db + " is in use by a running registry (lock held); stop it first");
  if (fs::exists(target_db, ec)) {
    sqlite3* cur = nullptr;
    bool free_to_swap = true;
    if (sqlite3_open_v2(target_db.c_str(), &cur, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK) {
      sqlite3_busy_timeout(cur, 200);
      const int rc = sqlite3_exec(cur, "BEGIN EXCLUSIVE", nullptr, nullptr, nullptr);
      if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) free_to_swap = false;  // someone else holds a lock; a file that is not a database at all is simply replaced
      else if (rc == SQLITE_OK) sqlite3_exec(cur, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    sqlite3_close(cur);
    if (!free_to_swap) return bad(target_db + " is locked by another process; nothing was changed");
  }

  // Write the snapshot to a private temp file and validate THAT; the target is not touched until everything passed.
  const std::string tmp = target_db + ".restore." + std::to_string(::getpid()) + ".tmp";
  struct TmpGuard { std::string p; bool armed = true; ~TmpGuard() { if (armed) { ::unlink(p.c_str()); ::unlink((p + "-journal").c_str()); } } } tmp_guard{tmp};
  {
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return bad(tmp + ": " + std::strerror(errno));
    std::size_t off = 0;
    while (off < pt.size()) {
      const ssize_t w = ::write(fd, pt.data() + off, pt.size() - off);
      if (w < 0 && errno == EINTR) continue;
      if (w <= 0) {
        ::close(fd);
        return bad(tmp + ": " + std::strerror(errno));
      }
      off += static_cast<std::size_t>(w);
    }
    if (::fsync(fd) != 0 || ::close(fd) != 0) return bad(tmp + ": " + std::strerror(errno));
  }
  cp::wipe(pt.data(), pt.size());
  {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(tmp.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
      sqlite3_close(db);
      return bad("the decrypted backup does not open as a SQLite database");
    }
    bool integrity = false;
    {
      sqlite3_stmt* st = nullptr;
      if (sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
        integrity = std::strcmp(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)), "ok") == 0;
      sqlite3_finalize(st);
    }
    const AuditVerify av = Database::verify_audit(db);
    const AuditHead head = Database::head_of(db);
    sqlite3_close(db);
    out.audit_ok = av.ok;
    out.audit_message = av.message;
    out.head_seq = head.seq;
    out.head_hash_hex = hex_encode(head.hash.data(), head.hash.size());
    if (!integrity) return bad("SQLite integrity_check failed on the decrypted backup; " + target_db + " was not touched");
    if (!av.ok) return bad("the decrypted backup's audit chain does not verify (" + av.message + "); " + target_db + " was not touched");
  }
  // anchors: the backup's own, then the newest in --anchor-dir
  {
    std::string name = fs::path(backup_file).filename().string();
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".vrb") == 0) {
      const fs::path own = fs::path(backup_file).parent_path() / (name.substr(0, name.size() - 4) + ".anchor");
      if (fs::exists(own, ec)) {
        Anchor a;
        if (!read_anchor_file(own.string(), &key, a, &why)) return bad(why);
        const AnchorCheck c = check_anchor_file(tmp, a);
        if (!c.ok || a.seq != out.head_seq) return bad("the backup does not match its anchor: " + c.message);
        out.notes += "matches its anchor " + own.string() + "; ";
      } else {
        out.notes += "no .anchor file next to the backup (not checked); ";
      }
    }
    if (!anchor_dir.empty()) {
      Anchor a;
      const int found = read_latest_anchor(anchor_dir, &key, a, &why);
      if (found < 0) return bad(why);
      if (found == 0) {
        out.notes += "no anchor in " + anchor_dir + "; ";
      } else {
        const AnchorCheck c = check_anchor_file(tmp, a);
        if (c.behind) out.notes += "WARNING: this backup is older than the newest anchor (" + c.message + "); ";
        else if (!c.ok) return bad(c.message);
        else out.notes += c.message + "; ";
      }
    }
  }
  // the restore itself is an audited event: the chain continues across it
  {
    std::string derr;
    auto d = Database::open(tmp, &derr);
    if (!d) return bad(derr);
    AuditEvent ev;
    ev.actor = "system";
    ev.action = "restore";
    ev.outcome = "ok";
    ev.detail = "from " + fs::path(backup_file).filename().string() + " head_seq=" + std::to_string(out.head_seq) + " head_hash=" + out.head_hash_hex;
    if (!d->audit(ev)) return bad("could not append the restore entry to the audit log");
  }
  if (force) {
    if (std::rename(tmp.c_str(), target_db.c_str()) != 0) return bad(target_db + ": " + std::strerror(errno));
  } else {
    // link() fails with EEXIST if anything appeared at TARGET since the check above: it can never replace a file
    if (::link(tmp.c_str(), target_db.c_str()) != 0) return bad(target_db + ": " + std::strerror(errno));
    ::unlink(tmp.c_str());
  }
  tmp_guard.armed = false;
  const std::string dir = fs::path(target_db).parent_path().empty() ? "." : fs::path(target_db).parent_path().string();
  if (const int dfd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC); dfd >= 0) {
    (void)::fsync(dfd);
    ::close(dfd);
  }
  out.audit_message += "; restore entry appended";
  return true;
}

}  // namespace velora::registry
