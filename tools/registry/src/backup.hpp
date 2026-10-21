#pragma once
// Story 6.8: encrypted registry backups (format "VRB1", documented in tools/registry/README.md). A consistent snapshot of the live database is taken with
// SQLite's online backup API into memory (no plaintext copy reaches the disk), encrypted with AES-256-GCM in 64 KiB chunks under a key derived by HKDF-SHA256 from a
// separate backup key file, and written atomically to a directory on another disk. restore decrypts, writes the exact snapshot bytes and verifies the audit chain.
#include <array>
#include <cstdint>
#include <string>

#include "util.hpp"

namespace velora::registry {

inline constexpr std::size_t kBackupHeaderSize = 64;
inline constexpr std::uint32_t kBackupChunkSize = 65536;

struct BackupResult {
  std::string path;                        // the backup file / the restored database
  std::array<std::uint8_t, 32> plaintext_sha256{};
  std::uint64_t plaintext_bytes = 0;
  bool audit_ok = false;                   // the snapshot's audit chain verified
  std::string audit_message;
  std::string notes;                       // non-fatal remarks (an older backup than the latest anchor, a missing anchor file)
  std::int64_t head_seq = 0;               // audit head of the snapshot
  std::string head_hash_hex;
};

// An audit-head anchor: `seq`, the chain hash at that sequence number, and an HMAC-SHA256 under the backup key (so only someone who holds the backup key can make
// one). Written next to every backup as `registry-<stamp>.anchor` in plain text:  `seq=N\nhash=<hex>\nmac=<hex>\n`. The registry can be started with --anchor-dir to refuse a
// database that is behind the newest anchor (rolled back / truncated) or whose chain at that sequence number has another hash (rewritten). The chain itself is UNKEYED:
// an attacker who can write the database file can recompute every hash; the anchors, kept on a second location with different permissions, are what makes that detectable.
struct Anchor {
  std::int64_t seq = 0;
  std::string hash_hex, mac_hex, path;
};
// 1 = found (newest by seq; each file's MAC is verified when `key` is given), 0 = no anchor file, -1 = error (`err`).
int read_latest_anchor(const std::string& dir, const Key32* key, Anchor& out, std::string* err);
bool read_anchor_file(const std::string& path, const Key32* key, Anchor& out, std::string* err);
struct AnchorCheck {
  bool ok = false;
  bool behind = false;   // the database has fewer entries than the anchor
  std::string message;
};
AnchorCheck check_anchor_file(const std::string& db_path, const Anchor& a);

// Snapshot (online backup API, safe while the server runs) -> encrypt -> `<dest_dir>/registry-<UTC time>.vrb` (0600, written to a temp file, fsynced, renamed).
// `dest_dir` must not be the database's directory or inside it (a second location); a directory that already exists keeps its permissions, a new one is created 0700. `now_unix` is stored in the header and names the file.
bool backup_database(const std::string& db_path, const Key32& backup_key, const std::string& dest_dir, std::int64_t now_unix, BackupResult& out, std::string* err);

// Pure format functions (also used by the tests).
Bytes encrypt_backup(const Bytes& plaintext, const Key32& backup_key, std::int64_t now_unix);
bool decrypt_backup(const Bytes& file, const Key32& backup_key, Bytes& plaintext, std::string* err);

// Decrypts `backup_file` and writes the database to `target_db` (0600; refuses to replace an existing file unless `force`), then opens it, runs
// `PRAGMA integrity_check` and verifies the audit chain. false + `err` when decryption (wrong key, damage, truncation) or any check fails.
// The decrypted database is written to a temporary file next to TARGET and validated (integrity_check, audit chain and schema, the backup's own anchor, and --anchor-dir's
// newest anchor: a fork is an error, a database behind the newest anchor only a note) BEFORE anything of TARGET is touched; then a `restore` audit entry is appended and the file
// is moved into place (link() without `force`, so an existing file can never be replaced by a race; rename() with it). TARGET must not be open in a running registry (flock on
// `<target>.lock` and an exclusive SQLite transaction are tried first). A failed restore leaves TARGET byte-identical.
bool restore_backup(const std::string& backup_file, const Key32& backup_key, const std::string& target_db, bool force, BackupResult& out, std::string* err,
                    const std::string& anchor_dir = "");

}  // namespace velora::registry
