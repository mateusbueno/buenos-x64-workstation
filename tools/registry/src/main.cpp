// velora-registry: the vendor device registry and content-key vault (Story 6.8). Vendor server only; never part of a device image.
#include <signal.h>
#include <sys/resource.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <thread>

#include "backup.hpp"
#include "database.hpp"
#include "server.hpp"
#include "service.hpp"
#include "vault.hpp"

using namespace velora::registry;

namespace {

enum Exit : int {
  kOk = 0,
  kUsage = 2,
  kBadKeyFile = 4,
  kBadConfig = 5,
  kDbFailed = 6,
  kAuditFailed = 7,
  kBackupFailed = 8,
  kRestoreFailed = 9,
  kServerFailed = 10,
};

int usage() {
  std::fputs(
      "usage: velora-registry serve --db FILE --vault-dir DIR --kek FILE --server-cert FILE --server-key FILE --client-ca FILE\n"
      "                             [--root dev|factory-test|production] [--expect-ca-sha256 HEX] [--expect-kek-id HEX16]   (both required unless --root dev)\n"
      "                             [--listen ADDR] [--port N] [--port-file FILE] [--workers N] [--rate-limit N] [--register-limit N]\n"
      "                             [--max-body BYTES] [--max-conn-per-ip N] [--revoked-certs FILE] [--station-libraries FILE]\n"
      "                             [--anchor-dir DIR --backup-key FILE] [--allow-no-mlock]\n"
      "       velora-registry verify-audit --db FILE [--anchor-dir DIR --backup-key FILE]\n"
      "       velora-registry backup --db FILE --backup-key FILE --kek FILE --dest-dir DIR\n"
      "       velora-registry restore --backup FILE --backup-key FILE --db FILE [--force] [--anchor-dir DIR]\n"
      "exit codes: 0 ok, 2 usage, 4 bad key file, 5 bad config, 6 database failed, 7 audit chain failed, 8 backup failed, 9 restore failed, 10 server failed\n",
      stderr);
  return kUsage;
}

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

struct Args {
  std::string cmd, db, vault_dir, kek, cert, key, ca, root = "dev", listen = "127.0.0.1", port_file, backup_key, dest_dir, backup;
  std::string expect_ca, expect_kek, revoked_certs, station_libs, anchor_dir;
  long port = 8443, workers = 8, rate = 120, max_body = 65536, register_limit = 600, max_conn_per_ip = 16;
  bool force = false, allow_no_mlock = false;
};

bool parse(int argc, char** argv, Args& a) {
  if (argc < 2) return false;
  a.cmd = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string s = argv[i];
    auto val = [&](std::string& out) {
      if (i + 1 >= argc) return false;
      out = argv[++i];
      return true;
    };
    auto num = [&](long& out) {
      std::string t;
      if (!val(t)) return false;
      char* end = nullptr;
      out = std::strtol(t.c_str(), &end, 10);
      return end && *end == '\0' && !t.empty();
    };
    bool ok = true;
    if (s == "--db") ok = val(a.db);
    else if (s == "--vault-dir") ok = val(a.vault_dir);
    else if (s == "--kek") ok = val(a.kek);
    else if (s == "--server-cert") ok = val(a.cert);
    else if (s == "--server-key") ok = val(a.key);
    else if (s == "--client-ca") ok = val(a.ca);
    else if (s == "--root") ok = val(a.root);
    else if (s == "--listen") ok = val(a.listen);
    else if (s == "--port") ok = num(a.port);
    else if (s == "--port-file") ok = val(a.port_file);
    else if (s == "--workers") ok = num(a.workers);
    else if (s == "--rate-limit") ok = num(a.rate);
    else if (s == "--max-body") ok = num(a.max_body);
    else if (s == "--backup-key") ok = val(a.backup_key);
    else if (s == "--dest-dir") ok = val(a.dest_dir);
    else if (s == "--backup") ok = val(a.backup);
    else if (s == "--expect-ca-sha256") ok = val(a.expect_ca);
    else if (s == "--expect-kek-id") ok = val(a.expect_kek);
    else if (s == "--revoked-certs") ok = val(a.revoked_certs);
    else if (s == "--station-libraries") ok = val(a.station_libs);
    else if (s == "--anchor-dir") ok = val(a.anchor_dir);
    else if (s == "--register-limit") ok = num(a.register_limit);
    else if (s == "--max-conn-per-ip") ok = num(a.max_conn_per_ip);
    else if (s == "--allow-no-mlock") a.allow_no_mlock = true;
    else if (s == "--force") a.force = true;
    else ok = false;
    if (!ok) return false;
  }
  return true;
}

void log_line(const std::string& m) {
  const std::string t = rfc3339(now_ms());
  std::fprintf(stderr, "%s %s\n", t.c_str(), m.c_str());
}

bool load_backup_key(const std::string& path, velora::contentpack::Secret<Key32>& key);

// Compares the database with the newest anchor in --anchor-dir. 0 = ok or no anchors yet, otherwise the exit code.
int check_anchors(const Args& a, const std::string& db_path) {
  if (a.anchor_dir.empty()) return kOk;
  if (a.backup_key.empty()) {
    std::fprintf(stderr, "error: --anchor-dir needs --backup-key (the anchors are MACed under it)\n");
    return kUsage;
  }
  velora::contentpack::Secret<Key32> key;
  if (!load_backup_key(a.backup_key, key)) return kBadKeyFile;
  Anchor an;
  std::string err;
  const int found = read_latest_anchor(a.anchor_dir, &key.get(), an, &err);
  if (found < 0) {
    std::fprintf(stderr, "error: anchors: %s\n", err.c_str());
    return kAuditFailed;
  }
  if (found == 0) {
    std::fprintf(stderr, "note: no anchor files in %s yet\n", a.anchor_dir.c_str());
    return kOk;
  }
  const AnchorCheck c = check_anchor_file(db_path, an);
  if (!c.ok) {
    std::fprintf(stderr, "AUDIT ANCHOR CHECK FAILED: %s\n", c.message.c_str());
    return kAuditFailed;
  }
  std::fprintf(stderr, "%s\n", c.message.c_str());
  return kOk;
}

int cmd_serve(const Args& a) {
  if (a.db.empty() || a.vault_dir.empty() || a.kek.empty() || a.cert.empty() || a.key.empty() || a.ca.empty()) return usage();
  if (a.root != "dev" && a.root != "factory-test" && a.root != "production") return usage();
  if (a.port < 0 || a.port > 65535 || a.workers < 1 || a.workers > 256 || a.rate < 1 || a.max_body < 1024 || a.max_body > (1 << 20) || a.register_limit < 1 ||
      a.max_conn_per_ip < 1)
    return usage();
  if (a.root != "dev" && (a.expect_ca.empty() || a.expect_kek.empty())) {
    std::fprintf(stderr, "error: --root %s requires --expect-ca-sha256 and --expect-kek-id: the root is bound to its CA and its KEK, not just named\n", a.root.c_str());
    return kBadConfig;
  }
  // No core dumps and no ptrace/proc-mem reads of a process that holds the KEK and unwraps content keys.
#ifdef RLIMIT_CORE
  struct rlimit rl{0, 0};
  setrlimit(RLIMIT_CORE, &rl);
#endif
#ifdef __linux__
  prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
#endif
  std::string err;
  auto db = Database::open(a.db, &err, /*exclusive_lock=*/true);
  if (!db) {
    std::fprintf(stderr, "error: database: %s\n", err.c_str());
    return kDbFailed;
  }
  // A database whose audit chain, schema or head does not verify is not served: its history can not be trusted.
  if (const AuditVerify v = db->verify_audit(); !v.ok) {
    std::fprintf(stderr, "error: the audit chain does not verify, refusing to start: %s\n", v.message.c_str());
    return kAuditFailed;
  }
  if (const int rc = check_anchors(a, a.db); rc != kOk) return rc;
  auto vault = KeyVault::create(a.vault_dir, a.kek, a.allow_no_mlock, &err);
  if (!vault) {
    std::fprintf(stderr, "error: kek/vault: %s\n", err.c_str());
    return kBadKeyFile;
  }
  if (!vault->locked()) std::fprintf(stderr, "WARNING: the KEK page is not locked in RAM (--allow-no-mlock): development only\n");
  if (!a.expect_kek.empty() && a.expect_kek != vault->kek_id_hex()) {
    std::fprintf(stderr, "error: the KEK id does not match --expect-kek-id (this is not the KEK of root %s)\n", a.root.c_str());
    return kBadConfig;
  }
  RateLimitConfig rlc;
  rlc.max_requests = static_cast<int>(a.rate);
  Service service(*db, *vault, rlc, log_line);
  service.set_registration_cap(static_cast<int>(a.register_limit));
  if (!a.station_libs.empty()) {
    std::map<std::string, std::set<std::string>> allow;
    if (!load_station_libraries(a.station_libs, allow, &err)) {
      std::fprintf(stderr, "error: station libraries: %s\n", err.c_str());
      return kBadConfig;
    }
    service.set_station_libraries(std::move(allow));
  }
  ServerConfig cfg;
  cfg.bind_address = a.listen;
  cfg.port = static_cast<std::uint16_t>(a.port);
  cfg.cert_file = a.cert;
  cfg.key_file = a.key;
  cfg.client_ca_file = a.ca;
  cfg.workers = static_cast<unsigned>(a.workers);
  cfg.http.max_body_bytes = static_cast<std::size_t>(a.max_body);
  cfg.max_conn_per_ip = static_cast<unsigned>(a.max_conn_per_ip);
  cfg.revoked_certs_file = a.revoked_certs;
  cfg.expect_ca_sha256 = a.expect_ca;
  Server server(cfg, service, log_line);
  if (!server.start(&err)) {
    std::fprintf(stderr, "error: server: %s\n", err.c_str());
    return err.find("--expect-ca-sha256") != std::string::npos ? kBadConfig : kServerFailed;
  }
  AuditEvent start;
  start.actor = "system";
  start.action = "service_started";
  start.outcome = "ok";
  start.detail = "root=" + a.root + " kek_id=" + vault->kek_id_hex() + (a.expect_ca.empty() ? "" : " ca_sha256=" + a.expect_ca);
  db->audit(start);
  ::signal(SIGINT, on_signal);
  ::signal(SIGTERM, on_signal);
  log_line("velora-registry listening on " + a.listen + ":" + std::to_string(server.port()) + " root=" + a.root + " kek_id=" + vault->kek_id_hex());
  if (!a.port_file.empty()) {
    if (FILE* f = std::fopen(a.port_file.c_str(), "w")) {
      std::fprintf(f, "%u\n", static_cast<unsigned>(server.port()));
      std::fclose(f);
    }
  }
  while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  log_line("stopping");
  server.stop();
  AuditEvent stop;
  stop.actor = "system";
  stop.action = "service_stopped";
  stop.outcome = "ok";
  db->audit(stop);
  return kOk;
}

int cmd_verify_audit(const Args& a) {
  if (a.db.empty()) return usage();
  const AuditVerify v = Database::verify_audit_file(a.db);
  if (v.ok) {
    std::printf("%s\n", v.message.c_str());
    return check_anchors(a, a.db);
  }
  std::fprintf(stderr, "AUDIT CHAIN FAILED: %s\n", v.message.c_str());
  return kAuditFailed;
}

bool load_backup_key(const std::string& path, velora::contentpack::Secret<Key32>& key) {
  std::string err;
  if (!read_secret_file(path, key.get().data(), 32, &err)) {
    std::fprintf(stderr, "error: backup key: %s\n", err.c_str());
    return false;
  }
  return true;
}

int cmd_backup(const Args& a) {
  if (a.db.empty() || a.backup_key.empty() || a.dest_dir.empty() || a.kek.empty()) return usage();
  velora::contentpack::Secret<Key32> key;
  if (!load_backup_key(a.backup_key, key)) return kBadKeyFile;
  {
    // The backup key must be a different secret from the KEK: a backup (and its anchors) must not be openable by whoever can read the KEK, or the reverse.
    velora::contentpack::Secret<Key32> kek;
    std::string kerr;
    if (!read_secret_file(a.kek, kek.get().data(), 32, &kerr)) {
      std::fprintf(stderr, "error: kek: %s\n", kerr.c_str());
      return kBadKeyFile;
    }
    if (kek.get() == key.get()) {
      std::fprintf(stderr, "error: the backup key is the same as the KEK: use a separate key\n");
      return kBadKeyFile;
    }
  }
  BackupResult r;
  std::string err;
  if (!backup_database(a.db, key.get(), a.dest_dir, now_ms() / 1000, r, &err)) {
    std::fprintf(stderr, "error: backup: %s\n", err.c_str());
    return kBackupFailed;
  }
  std::printf("backup written: %s (%llu bytes of database, sha256 %s)\n", r.path.c_str(), static_cast<unsigned long long>(r.plaintext_bytes),
              hex_encode(r.plaintext_sha256.data(), 32).c_str());
  if (!r.audit_ok) {
    std::fprintf(stderr, "WARNING: the backup was written, but the audit chain of the database does not verify: %s\n", r.audit_message.c_str());
    return kAuditFailed;
  }
  return kOk;
}

int cmd_restore(const Args& a) {
  if (a.backup.empty() || a.backup_key.empty() || a.db.empty()) return usage();
  velora::contentpack::Secret<Key32> key;
  if (!load_backup_key(a.backup_key, key)) return kBadKeyFile;
  BackupResult r;
  std::string err;
  if (!restore_backup(a.backup, key.get(), a.db, a.force, r, &err, a.anchor_dir)) {
    std::fprintf(stderr, "error: restore: %s\n", err.c_str());
    return kRestoreFailed;
  }
  std::printf("restored: %s (%llu bytes, sha256 %s); %s\n", r.path.c_str(), static_cast<unsigned long long>(r.plaintext_bytes),
              hex_encode(r.plaintext_sha256.data(), 32).c_str(), (r.audit_message + "; " + r.notes).c_str());
  return kOk;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!parse(argc, argv, a)) return usage();
  if (a.cmd == "serve") return cmd_serve(a);
  if (a.cmd == "verify-audit") return cmd_verify_audit(a);
  if (a.cmd == "backup") return cmd_backup(a);
  if (a.cmd == "restore") return cmd_restore(a);
  return usage();
}
