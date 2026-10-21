// Story 6.8: the device registry and content-key vault (tools/registry). A server on a localhost ephemeral port with a throw-away PKI built in-process:
// mutual TLS (no certificate, wrong CA, expired, TLS 1.2, wrong role), register / duplicates / revoke / supersede, wrapped-key issuance that unwraps with the device
// private key to the original content key, the append-only hash-chained audit log (tamper, truncation, triggers), encrypted backup and restore, no plaintext key
// in logs / database / backups, input fuzz, concurrency, rate limit, timeouts and the spawned CLI.
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>
#include <signal.h>
#include <sqlite3.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <functional>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "backup.hpp"
#include "crypto.hpp"
#include "database.hpp"
#include "http.hpp"
#include "server.hpp"
#include "service.hpp"
#include "test_pki.hpp"
#include "vault.hpp"
#include "velora/format/hash.hpp"
#include "velora/format/json_lite.hpp"

#ifndef REGISTRY_API_MD
#error "REGISTRY_API_MD must name tools/registry/API.md"
#endif
#ifdef REGISTRY_HAS_DEV_KEYS
#include "velora/host/dev_key_provider.hpp"
#endif
#ifndef REGISTRY_EXE
#error "REGISTRY_EXE must name velora-registry"
#endif

namespace fs = std::filesystem;
namespace fmt = velora::format;
namespace cp = velora::contentpack;
using namespace velora::registry;

static int g_fail = 0;
#define CHECK(c)                                               \
  do {                                                         \
    if (!(c)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

namespace {

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// small helpers

Bytes read_all(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
void put_file(const fs::path& p, const void* data, std::size_t n, mode_t mode) {
  fs::create_directories(p.parent_path());
  const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (fd < 0) return;
  (void)!::write(fd, data, n);
  ::fchmod(fd, mode);
  ::close(fd);
}
bool contains(const Bytes& hay, const void* needle, std::size_t n) {
  if (n == 0 || hay.size() < n) return false;
  return ::memmem(hay.data(), hay.size(), needle, n) != nullptr;
}
bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

std::string random_uuid() {
  fmt::Uuid u;
  cp::random_bytes(u);
  u[6] = static_cast<std::uint8_t>((u[6] & 0x0f) | 0x40);
  u[8] = static_cast<std::uint8_t>((u[8] & 0x3f) | 0x80);
  return fmt::uuid_to_string(u);
}
Key32 random_key() {
  Key32 k;
  cp::random_bytes(k);
  return k;
}
// A random but valid X25519 public key (a random 32-byte string has bit 255 set half the time, which the registry refuses).
Key32 random_pub();
std::string hex(const Key32& k) { return hex_encode(k.data(), k.size()); }

Key32 random_pub() {
  Key32 priv = random_key();
  return cp::x25519_public(priv);
}

bool wait_until(const std::function<bool()>& pred, int ms = 5000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return pred();
}

int run_cmd(const std::string& cmd, std::string* out = nullptr) {
  std::fflush(nullptr);  // a forked child must not inherit (and later flush) our buffered output
  FILE* p = ::popen((cmd + " 2>&1").c_str(), "r");
  if (!p) return -1;
  char buf[512];
  std::string o;
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) o.append(buf, n);
  const int st = ::pclose(p);
  if (out) *out = o;
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// Every secret any test created (content keys, KEKs, backup keys, device private keys): the final scan looks for all of them in everything the registry wrote.
std::mutex g_secret_mu;
std::vector<Bytes> g_secrets;
void note_secret(const std::uint8_t* p, std::size_t n) {
  std::lock_guard<std::mutex> lk(g_secret_mu);
  g_secrets.emplace_back(p, p + n);
}
// Every typed error code any test saw: compared with API.md at the end.
std::set<std::string> g_seen_codes;
void note_code(const std::string& body) {
  fmt::JsonValue v;
  if (!fmt::parse_json(body, v) || !v.is_object()) return;
  const fmt::JsonValue* e = v.find("error");
  const fmt::JsonValue* c = e ? e->find("code") : nullptr;
  if (c && c->is_string()) {
    std::lock_guard<std::mutex> lk(g_secret_mu);
    g_seen_codes.insert(c->string);
  }
}

std::mutex g_log_mu;
std::string g_log;
void capture_log(const std::string& m) {
  std::lock_guard<std::mutex> lk(g_log_mu);
  g_log += m + "\n";
}
std::string log_copy() {
  std::lock_guard<std::mutex> lk(g_log_mu);
  return g_log;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// the environment: PKI, vault, database, service, server

struct Lib {
  std::string id;
  Key32 key;
};

struct Env {
  fs::path dir;
  pki::Ident root, server_id, st1, st2, admin, expired, other_ou, evil_root, evil_station, wrong_eku, revoked_station;
  Key32 kek, backup_key;
  std::vector<Lib> libs;
  std::unique_ptr<Database> db;
  std::unique_ptr<KeyVault> vault;
  std::unique_ptr<Service> service;
  std::unique_ptr<Server> server;
  std::string db_path() const { return (dir / "registry.db").string(); }
  std::string vault_dir() const { return (dir / "vault").string(); }
};

struct EnvOptions {
  int io_timeout_ms = 5000;
  int rate = 100000;
  unsigned conn_cap = 64;   // concurrent connections per peer IP (the production default, 16, is tested on its own)
  bool revoked_file = true; // a (initially empty) --revoked-certs deny-list
};

void make_env(Env& e, const fs::path& dir, const EnvOptions& o = EnvOptions()) {
  const int io_timeout_ms = o.io_timeout_ms, rate = o.rate;
  e.dir = dir;
  fs::create_directories(dir);
  e.root = pki::make_ident("Velora Test Factory Root", "velora-root", nullptr, true, -86400, 86400 * 30, nullptr, nullptr);
  e.server_id = pki::make_ident("registry.test", "velora-server", &e.root, false, -86400, 86400 * 30, "serverAuth", "127.0.0.1");
  e.st1 = pki::make_ident("station-01", kStationOu, &e.root, false, -86400, 86400 * 30, "clientAuth", nullptr);
  e.st2 = pki::make_ident("station-02", kStationOu, &e.root, false, -86400, 86400 * 30, "clientAuth", nullptr);
  e.admin = pki::make_ident("admin-01", kAdminOu, &e.root, false, -86400, 86400 * 30, "clientAuth", nullptr);
  e.expired = pki::make_ident("station-old", kStationOu, &e.root, false, -86400 * 3, -86400, "clientAuth", nullptr);
  e.other_ou = pki::make_ident("someone", "velora-intern", &e.root, false, -86400, 86400 * 30, "clientAuth", nullptr);
  e.evil_root = pki::make_ident("Evil Root", "velora-root", nullptr, true, -86400, 86400 * 30, nullptr, nullptr);
  e.evil_station = pki::make_ident("station-01", kStationOu, &e.evil_root, false, -86400, 86400 * 30, "clientAuth", nullptr);
  e.wrong_eku = pki::make_ident("station-eku", kStationOu, &e.root, false, -86400, 86400 * 30, "serverAuth", nullptr);
  e.revoked_station = pki::make_ident("station-rev", kStationOu, &e.root, false, -86400, 86400 * 30, "clientAuth", nullptr);
  pki::write_cert(e.root, (dir / "root.crt").string());
  pki::write_cert(e.server_id, (dir / "server.crt").string());
  pki::write_key(e.server_id, (dir / "server.key").string());
  // vault: three libraries sealed under the KEK exactly as `velora-content-pack pack` writes them
  e.kek = random_key();
  e.backup_key = random_key();
  put_file(dir / "kek.aes256", e.kek.data(), 32, 0600);
  put_file(dir / "backup.key", e.backup_key.data(), 32, 0600);
  note_secret(e.kek.data(), 32);
  note_secret(e.backup_key.data(), 32);
  if (o.revoked_file) {
    const std::string t = "# revoked station certificates (hex serial numbers)\n";
    put_file(dir / "revoked-certs.txt", t.data(), t.size(), 0600);
  }
  for (int i = 0; i < 3; ++i) {
    Lib l{random_uuid(), random_key()};
    cp::Nonce12 nonce;
    cp::random_bytes(nonce);
    const auto entry = cp::vault_entry_seal(e.kek, l.key, l.id, nonce);
    put_file(dir / "vault" / l.id / "content-key.vault", entry.data(), entry.size(), 0600);
    note_secret(l.key.data(), 32);
    e.libs.push_back(l);
  }
  std::string err;
  e.db = Database::open(e.db_path(), &err);
  CHECK(e.db != nullptr);
  e.vault = KeyVault::create(e.vault_dir(), (dir / "kek.aes256").string(), /*allow_no_mlock=*/false, &err);
  CHECK(e.vault != nullptr);
  RateLimitConfig rl;
  rl.max_requests = rate;
  e.service = std::make_unique<Service>(*e.db, *e.vault, rl, capture_log);
  ServerConfig cfg;
  cfg.cert_file = (dir / "server.crt").string();
  cfg.key_file = (dir / "server.key").string();
  cfg.client_ca_file = (dir / "root.crt").string();
  cfg.io_timeout_ms = io_timeout_ms;
  cfg.request_deadline_ms = 2 * io_timeout_ms;
  cfg.max_conn_per_ip = o.conn_cap;
  if (o.revoked_file) cfg.revoked_certs_file = (dir / "revoked-certs.txt").string();
  e.server = std::make_unique<Server>(cfg, *e.service, capture_log);
  CHECK(e.server->start(&err));
  if (!err.empty()) std::printf("server start: %s\n", err.c_str());
  CHECK(e.vault && e.vault->locked());
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// TLS client

struct Reply {
  bool tls_ok = false;        // the client side of the handshake completed
  bool got_response = false;  // an HTTP response arrived
  int status = 0;
  std::string body;
  std::string code() const {  // error.code of a typed error
    fmt::JsonValue v;
    if (!fmt::parse_json(body, v) || !v.is_object()) return "";
    const fmt::JsonValue* e = v.find("error");
    const fmt::JsonValue* c = e ? e->find("code") : nullptr;
    return c && c->is_string() ? c->string : "";
  }
  fmt::JsonValue json() const {
    fmt::JsonValue v;
    fmt::parse_json(body, v);
    return v;
  }
};

std::string http_request(const std::string& method, const std::string& target, const std::string& body) {
  return method + " " + target + " HTTP/1.1\r\nHost: registry.test\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\n\r\n" + body;
}

Reply send_raw(Env& e, const pki::Ident* client, const std::string& raw, int max_tls = 0, bool half_close = false, int read_timeout_ms = 8000) {
  Reply r;
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  if (max_tls) SSL_CTX_set_max_proto_version(ctx, max_tls);
  SSL_CTX_load_verify_locations(ctx, (e.dir / "root.crt").c_str(), nullptr);
  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
  if (client) {
    SSL_CTX_use_certificate(ctx, client->cert.get());
    SSL_CTX_use_PrivateKey(ctx, client->key.get());
  }
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(e.server->port());
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  timeval tv{read_timeout_ms / 1000, (read_timeout_ms % 1000) * 1000};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    ::close(fd);
    SSL_CTX_free(ctx);
    return r;
  }
  SSL* ssl = SSL_new(ctx);
  SSL_set_fd(ssl, fd);
  X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), "127.0.0.1");
  if (SSL_connect(ssl) == 1) {
    r.tls_ok = true;
    std::size_t off = 0;
    while (off < raw.size()) {
      const int w = SSL_write(ssl, raw.data() + off, static_cast<int>(raw.size() - off));
      if (w <= 0) break;
      off += static_cast<std::size_t>(w);
    }
    (void)half_close;
    std::string in;
    char buf[4096];
    int n;
    while ((n = SSL_read(ssl, buf, sizeof buf)) > 0) in.append(buf, static_cast<std::size_t>(n));
    if (in.rfind("HTTP/1.1 ", 0) == 0) {
      r.got_response = true;
      r.status = std::atoi(in.c_str() + 9);
      const std::size_t h = in.find("\r\n\r\n");
      if (h != std::string::npos) r.body = in.substr(h + 4);
      note_code(r.body);
    }
  }
  ERR_clear_error();
  SSL_free(ssl);
  ::close(fd);
  SSL_CTX_free(ctx);
  return r;
}

Reply call(Env& e, const pki::Ident& who, const std::string& method, const std::string& target, const std::string& body = "") {
  return send_raw(e, &who, http_request(method, target, body));
}

std::string register_body(const std::string& id, const Key32& pub, const std::string& serial, const std::string& replaces = "") {
  return "{\"device_id\":\"" + id + "\",\"device_public_key\":\"" + hex(pub) + "\",\"som_id\":\"som-" + id + "\",\"serial_number\":\"" + serial +
         "\",\"tier\":\"standard\",\"provisioning_date\":\"2026-10-20\"" + (replaces.empty() ? "" : ",\"replaces\":\"" + replaces + "\"") + "}";
}

struct DevKeys {
  cp::Secret<Key32> priv;
  Key32 pub{};
  DevKeys() {
    cp::random_bytes(priv.get());
    pub = cp::x25519_public(priv.get());
    note_secret(priv.get().data(), 32);
  }
};

// Service-level call (no TLS), noting the typed error code.
HttpResponse direct(Service& svc, const Principal& who, const std::string& method, const std::string& target, const std::string& body = "") {
  HttpRequest req;
  req.method = method;
  req.target = target;
  const std::size_t q = target.find('?');
  req.path = target.substr(0, q);
  if (q != std::string::npos) req.query = target.substr(q + 1);
  req.body = body;
  HttpResponse r = svc.handle(req, who);
  note_code(r.body);
  return r;
}
const Principal kStationPrincipal{Role::kStation, "station-01", "127.0.0.1"};

std::vector<AuditRow> rows_of(Env& e) { return e.db->audit_rows(1, 100000); }
int count_rows(Env& e, const std::string& action, const std::string& code = "", const std::string& outcome = "") {
  int n = 0;
  for (const AuditRow& r : rows_of(e))
    if (r.action == action && (code.empty() || r.code == code) && (outcome.empty() || r.outcome == outcome)) ++n;
  return n;
}
std::vector<std::uint8_t> unhex(const std::string& s) {
  std::vector<std::uint8_t> out(s.size() / 2);
  hex_decode_exact(s, out.data(), out.size());
  return out;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// tests

void test_register_and_wrap(Env& e) {
  DevKeys d1;
  Reply r = call(e, e.st1, "POST", "/v1/devices", register_body("dev-0001", d1.pub, "SN-0001"));
  CHECK(r.got_response && r.status == 201);
  fmt::JsonValue j = r.json();
  CHECK(j.find("device_id") && j.find("device_id")->string == "dev-0001");
  CHECK(j.find("status") && j.find("status")->string == "active");
  CHECK(j.find("station_id") && j.find("station_id")->string == "station-01");  // from the certificate
  CHECK(j.find("device_public_key") && j.find("device_public_key")->string == hex(d1.pub));
  CHECK(j.find("som_id") && j.find("serial_number") && j.find("tier") && j.find("provisioning_date"));

  // read it back as station and as admin
  r = call(e, e.st2, "GET", "/v1/devices/dev-0001");
  CHECK(r.status == 200 && r.json().find("serial_number")->string == "SN-0001");
  r = call(e, e.admin, "GET", "/v1/devices/dev-0001");
  CHECK(r.status == 200);

  // wrapped keys for two libraries (POST) and one (GET): each unwraps with the device private key to the original content key
  const std::string body = "{\"library_ids\":[\"" + e.libs[0].id + "\",\"" + e.libs[1].id + "\"]}";
  r = call(e, e.st1, "POST", "/v1/devices/dev-0001/wrapped-keys", body);
  CHECK(r.status == 200);
  fmt::JsonValue k = r.json();
  const fmt::JsonValue* keys = k.find("keys");
  CHECK(keys && keys->is_array() && keys->array.size() == 2);
  if (keys && keys->array.size() == 2) {
    for (std::size_t i = 0; i < 2; ++i) {
      const auto& item = keys->array[i];
      CHECK(item.find("library_id")->string == e.libs[i].id);
      const auto blob = unhex(item.find("wrapped_key")->string);
      CHECK(blob.size() == cp::kWrappedKeySize);
      Key32 got{};
      CHECK(cp::wrapped_key_open(d1.priv.get(), blob, e.libs[i].id, got) && got == e.libs[i].key);
      // bound to its library and to its device
      Key32 other{};
      CHECK(!cp::wrapped_key_open(d1.priv.get(), blob, e.libs[2].id, other));
      DevKeys stranger;
      CHECK(!cp::wrapped_key_open(stranger.priv.get(), blob, e.libs[i].id, other));
    }
  }
  // the response carries wrapped blobs only: no content key, in raw or hex form
  for (const Lib& l : e.libs) {
    CHECK(!contains(r.body, hex(l.key)));
    CHECK(!contains(Bytes(r.body.begin(), r.body.end()), l.key.data(), 32));
  }
  r = call(e, e.st1, "GET", "/v1/devices/dev-0001/wrapped-keys?library_id=" + e.libs[2].id);
  CHECK(r.status == 200);
  {
    fmt::JsonValue g = r.json();
    const auto blob = unhex(g.find("keys")->array[0].find("wrapped_key")->string);
    Key32 got{};
    CHECK(cp::wrapped_key_open(d1.priv.get(), blob, e.libs[2].id, got) && got == e.libs[2].key);
  }
  // a fresh ephemeral key per wrap: two requests give different blobs for the same key
  Reply r2 = call(e, e.st1, "GET", "/v1/devices/dev-0001/wrapped-keys?library_id=" + e.libs[2].id);
  CHECK(r2.status == 200 && r2.body != r.body);

  // audit: the registration and every issuance (device, library, station), no key
  CHECK(count_rows(e, "register_device", "", "ok") == 1);
  CHECK(count_rows(e, "wrapped_key_issued", "", "ok") == 4);
  bool seen = false;
  for (const AuditRow& a : rows_of(e))
    if (a.action == "wrapped_key_issued" && a.device_id == "dev-0001" && a.library_id == e.libs[0].id && a.actor == "station:station-01" && a.ts_ms > 0) seen = true;
  CHECK(seen);
}

void test_duplicates_and_validation(Env& e) {
  DevKeys d;
  Reply r = call(e, e.st1, "POST", "/v1/devices", register_body("dev-0001", d.pub, "SN-9999"));
  CHECK(r.status == 409 && r.code() == "duplicate_device_id");
  r = call(e, e.st1, "POST", "/v1/devices", register_body("dev-0002", d.pub, "SN-0001"));
  CHECK(r.status == 409 && r.code() == "duplicate_serial");
  // the same public key twice (even under another id and serial)
  DevKeys d2;
  r = call(e, e.st1, "POST", "/v1/devices", register_body("dev-0010", d2.pub, "SN-0010"));
  CHECK(r.status == 201);
  r = call(e, e.st1, "POST", "/v1/devices", register_body("dev-0011", d2.pub, "SN-0011"));
  CHECK(r.status == 409 && r.code() == "duplicate_public_key");
  // validation
  const std::string ok = register_body("dev-0020", random_pub(), "SN-0020");
  auto field_error = [&](std::string body, const char* field) {
    Reply x = call(e, e.st1, "POST", "/v1/devices", body);
    return x.status == 400 && x.code() == "invalid_field" && contains(x.body, std::string("\"field\":\"") + field + "\"");
  };
  auto replace = [&](std::string s, const std::string& a, const std::string& b) { s.replace(s.find(a), a.size(), b); return s; };
  CHECK(field_error(replace(ok, "\"dev-0020\"", "\"bad id!\""), "device_id"));
  CHECK(field_error(replace(ok, "\"serial_number\":\"SN-0020\"", "\"serial_number\":\"\""), "serial_number"));
  CHECK(field_error(replace(ok, "2026-10-20", "2026-02-30"), "provisioning_date"));
  CHECK(field_error(replace(ok, "\"tier\":\"standard\"", "\"tier\":7"), "tier"));
  CHECK(field_error(replace(ok, "\"device_public_key\":\"", "\"device_public_key\":\"zz"), "device_public_key"));
  r = call(e, e.st1, "POST", "/v1/devices", register_body("dev-0021", Key32{}, "SN-0021"));
  CHECK(r.status == 400 && r.code() == "invalid_field");  // the all-zero key
  r = call(e, e.st1, "POST", "/v1/devices", ok.substr(0, ok.size() - 1) + ",\"extra\":1}");
  CHECK(r.status == 400 && r.code() == "invalid_request");
  r = call(e, e.st1, "POST", "/v1/devices", "{\"device_id\":\"a\",\"device_id\":\"b\"}");
  CHECK(r.status == 400);
  // every refusal is in the audit log
  CHECK(count_rows(e, "POST /v1/devices", "duplicate_serial", "refused") == 1);
  CHECK(count_rows(e, "POST /v1/devices", "duplicate_device_id", "refused") == 1);
  CHECK(count_rows(e, "POST /v1/devices", "duplicate_public_key", "refused") == 1);
}

void test_revoke_and_supersede(Env& e) {
  DevKeys d;
  CHECK(call(e, e.st1, "POST", "/v1/devices", register_body("dev-0100", d.pub, "SN-0100")).status == 201);
  // a station may not revoke
  Reply r = call(e, e.st1, "POST", "/v1/devices/dev-0100/revoke", "{\"reason\":\"test\"}");
  CHECK(r.status == 403 && r.code() == "forbidden");
  CHECK(call(e, e.st1, "GET", "/v1/devices/dev-0100").json().find("status")->string == "active");
  // an admin may not register or request keys
  r = call(e, e.admin, "POST", "/v1/devices", register_body("dev-0101", random_pub(), "SN-0101"));
  CHECK(r.status == 403 && r.code() == "forbidden");
  r = call(e, e.admin, "GET", "/v1/devices/dev-0100/wrapped-keys?library_id=" + e.libs[0].id);
  CHECK(r.status == 403);
  // unknown device
  r = call(e, e.admin, "POST", "/v1/devices/nope/revoke");
  CHECK(r.status == 404 && r.code() == "unknown_device");
  // the admin revokes (no body is fine)
  r = call(e, e.admin, "POST", "/v1/devices/dev-0100/revoke", "{\"reason\":\"SoM swap\"}");
  CHECK(r.status == 200);
  CHECK(r.json().find("status")->string == "revoked" && r.json().find("revoked_by")->string == "admin:admin-01" && r.json().find("revoke_reason")->string == "SoM swap");
  r = call(e, e.admin, "POST", "/v1/devices/dev-0100/revoke");
  CHECK(r.status == 409 && r.code() == "already_revoked");
  // a revoked device receives no keys, and the refusal is audited
  r = call(e, e.st1, "POST", "/v1/devices/dev-0100/wrapped-keys", "{\"library_ids\":[\"" + e.libs[0].id + "\"]}");
  CHECK(r.status == 403 && r.code() == "device_revoked");
  CHECK(count_rows(e, "POST /v1/devices/{id}/wrapped-keys", "device_revoked", "refused") == 1);
  // an unknown device receives none either
  r = call(e, e.st1, "POST", "/v1/devices/ghost/wrapped-keys", "{\"library_ids\":[\"" + e.libs[0].id + "\"]}");
  CHECK(r.status == 404 && r.code() == "unknown_device");
  // the revoked key can not come back; the serial can be used again, superseding the revoked record
  r = call(e, e.st1, "POST", "/v1/devices", register_body("dev-0102", d.pub, "SN-0100"));
  CHECK(r.status == 409 && r.code() == "duplicate_public_key");
  DevKeys d2;
  r = call(e, e.st1, "POST", "/v1/devices", register_body("dev-0102", d2.pub, "SN-0100", "dev-0100"));
  CHECK(r.status == 201 && r.json().find("replaces")->string == "dev-0100");
  r = call(e, e.st1, "GET", "/v1/devices/dev-0100");  // the revoked record stays readable
  CHECK(r.status == 200 && r.json().find("status")->string == "revoked");
  DevKeys d3;
  r = call(e, e.st1, "POST", "/v1/devices", register_body("dev-0103", d3.pub, "SN-0103", "dev-0100"));
  CHECK(r.status == 409 && r.code() == "invalid_replaces");  // already superseded
  r = call(e, e.st1, "POST", "/v1/devices", register_body("dev-0103", d3.pub, "SN-0103", "dev-0102"));
  CHECK(r.status == 409 && r.code() == "invalid_replaces");  // not revoked
  r = call(e, e.st1, "POST", "/v1/devices", register_body("dev-0103", d3.pub, "SN-0103", "never-existed"));
  CHECK(r.status == 409 && r.code() == "invalid_replaces");
  // the replacement receives keys
  r = call(e, e.st1, "POST", "/v1/devices/dev-0102/wrapped-keys", "{\"library_ids\":[\"" + e.libs[1].id + "\"]}");
  CHECK(r.status == 200);
  // the same device_id may be registered again after a revoke, as a superseding record
  DevKeys d4;
  CHECK(call(e, e.st1, "POST", "/v1/devices", register_body("dev-0110", d4.pub, "SN-0110")).status == 201);
  CHECK(call(e, e.admin, "POST", "/v1/devices/dev-0110/revoke").status == 200);
  DevKeys d5;
  r = call(e, e.st1, "POST", "/v1/devices", register_body("dev-0110", d5.pub, "SN-0110", "dev-0110"));
  CHECK(r.status == 201 && r.json().find("status")->string == "active");
  CHECK(call(e, e.st1, "GET", "/v1/devices/dev-0110").json().find("device_public_key")->string == hex(d5.pub));
  // GET wrapped-keys for a revoked record is refused and logged too; a superseded id (dev-0100) stays refused; GET of a revoked record is allowed and says "revoked"
  const int gets_before = count_rows(e, "GET /v1/devices/{id}/wrapped-keys", "device_revoked", "refused");
  r = call(e, e.st1, "GET", "/v1/devices/dev-0100/wrapped-keys?library_id=" + e.libs[0].id);
  CHECK(r.status == 403 && r.code() == "device_revoked" && !contains(r.body, "wrapped_key\""));
  CHECK(count_rows(e, "GET /v1/devices/{id}/wrapped-keys", "device_revoked", "refused") == gets_before + 1);
  r = call(e, e.st1, "GET", "/v1/devices/dev-0100");
  CHECK(r.status == 200 && r.json().find("status")->string == "revoked" && r.json().find("replaces")->type == fmt::JsonValue::Type::kNull);
  // audit coverage of revoke and supersede: actor (the station / admin identity), device and the station recorded in the row
  bool revoke_row = false, supersede_row = false;
  for (const AuditRow& a : rows_of(e)) {
    if (a.action == "revoke_device" && a.device_id == "dev-0100" && a.actor == "admin:admin-01" && contains(a.detail, "station=station-01") && contains(a.detail, "reason=SoM swap")) revoke_row = true;
    if (a.action == "register_device" && a.device_id == "dev-0102" && a.actor == "station:station-01" && contains(a.detail, "station=station-01") && contains(a.detail, "supersedes=dev-0100")) supersede_row = true;
  }
  CHECK(revoke_row && supersede_row);
}

void test_wrapped_key_errors(Env& e) {
  DevKeys d;
  CHECK(call(e, e.st1, "POST", "/v1/devices", register_body("dev-0200", d.pub, "SN-0200")).status == 201);
  const int before = count_rows(e, "wrapped_key_issued");
  const std::string unknown = random_uuid();
  Reply r = call(e, e.st1, "POST", "/v1/devices/dev-0200/wrapped-keys", "{\"library_ids\":[\"" + unknown + "\"]}");
  CHECK(r.status == 404 && r.code() == "unknown_library" && contains(r.body, unknown));
  // all or nothing: a known and an unknown library issue nothing
  r = call(e, e.st1, "POST", "/v1/devices/dev-0200/wrapped-keys", "{\"library_ids\":[\"" + e.libs[0].id + "\",\"" + unknown + "\"]}");
  CHECK(r.status == 404 && r.code() == "unknown_library" && !contains(r.body, "wrapped_key"));
  CHECK(count_rows(e, "wrapped_key_issued") == before);
  CHECK(call(e, e.st1, "POST", "/v1/devices/dev-0200/wrapped-keys", "{\"library_ids\":[\"../../etc/passwd\"]}").code() == "invalid_library_id");
  CHECK(call(e, e.st1, "POST", "/v1/devices/dev-0200/wrapped-keys", "{\"library_ids\":[\"" + e.libs[0].id.substr(0, 35) + "G\"]}").code() == "invalid_library_id");
  CHECK(call(e, e.st1, "POST", "/v1/devices/dev-0200/wrapped-keys", "{\"library_ids\":[]}").code() == "invalid_request");
  CHECK(call(e, e.st1, "POST", "/v1/devices/dev-0200/wrapped-keys", "{\"library_ids\":[\"" + e.libs[0].id + "\",\"" + e.libs[0].id + "\"]}").code() == "invalid_request");
  CHECK(call(e, e.st1, "POST", "/v1/devices/dev-0200/wrapped-keys", "{\"library_ids\":\"x\"}").code() == "invalid_request");
  CHECK(call(e, e.st1, "GET", "/v1/devices/dev-0200/wrapped-keys").code() == "invalid_request");
  CHECK(call(e, e.st1, "GET", "/v1/devices/dev-0200/wrapped-keys?library_id=" + e.libs[0].id + "&x=1").code() == "invalid_request");
  {
    std::string many = "{\"library_ids\":[";
    for (int i = 0; i < 65; ++i) many += (i ? ",\"" : "\"") + random_uuid() + "\"";
    many += "]}";
    CHECK(call(e, e.st1, "POST", "/v1/devices/dev-0200/wrapped-keys", many).code() == "too_many_libraries");
  }
  CHECK(call(e, e.st1, "DELETE", "/v1/devices/dev-0200").status == 405);
  CHECK(call(e, e.st1, "GET", "/v1/nothing").status == 404);
  CHECK(call(e, e.st1, "GET", "/v1/devices/dev-0200/extra").status == 404);
  CHECK(call(e, e.st1, "GET", "/v1/devices/bad%20id").code() == "invalid_device_id");
  CHECK(count_rows(e, "wrapped_key_issued") == before);

  // a vault entry sealed under another KEK, or damaged, is a typed server error and leaks nothing
  const std::string bad_id = random_uuid();
  const Key32 other_kek = random_key(), ck = random_key();
  cp::Nonce12 n;
  cp::random_bytes(n);
  auto entry = cp::vault_entry_seal(other_kek, ck, bad_id, n);
  put_file(e.dir / "vault" / bad_id / "content-key.vault", entry.data(), entry.size(), 0600);
  r = call(e, e.st1, "POST", "/v1/devices/dev-0200/wrapped-keys", "{\"library_ids\":[\"" + bad_id + "\"]}");
  CHECK(r.status == 500 && r.code() == "vault_entry_invalid" && !contains(r.body, hex(ck)));
  const std::string short_id = random_uuid();
  put_file(e.dir / "vault" / short_id / "content-key.vault", entry.data(), 40, 0600);
  r = call(e, e.st1, "POST", "/v1/devices/dev-0200/wrapped-keys", "{\"library_ids\":[\"" + short_id + "\"]}");
  CHECK(r.status == 500 && r.code() == "vault_entry_invalid");
  // a vault entry for library A copied under the id of library B does not open (the library id is bound as AAD)
  const std::string swapped = random_uuid();
  const auto good = read_all(e.dir / "vault" / e.libs[0].id / "content-key.vault");
  put_file(e.dir / "vault" / swapped / "content-key.vault", good.data(), good.size(), 0600);
  CHECK(call(e, e.st1, "POST", "/v1/devices/dev-0200/wrapped-keys", "{\"library_ids\":[\"" + swapped + "\"]}").code() == "vault_entry_invalid");
  CHECK(count_rows(e, "wrapped_key_issued") == before);
}

std::string serial_hex_of(const pki::Ident& id) {
  BIGNUM* bn = ASN1_INTEGER_to_BN(X509_get_serialNumber(id.cert.get()), nullptr);
  char* h = BN_bn2hex(bn);
  std::string out = h;
  OPENSSL_free(h);
  BN_free(bn);
  return out;
}

std::string lower(std::string v) {
  for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return v;
}

std::vector<AuditRow> handshake_rows(Env& e) {
  std::vector<AuditRow> v;
  for (const AuditRow& r : rows_of(e))
    if (r.action == "tls_handshake_failed") v.push_back(r);
  return v;
}

void test_authentication(Env& e) {
  DevKeys d;
  const std::string reg = http_request("POST", "/v1/devices", register_body("dev-0300", d.pub, "SN-0300"));
  // Each refusal produces EXACTLY ONE new tls_handshake_failed row, naming the reason, and is followed by a successful request (positive control).
  auto expect_one = [&](const char* label, const std::function<Reply()>& attempt, const std::vector<std::string>& needles) {
    const std::size_t before = handshake_rows(e).size();
    const Reply r = attempt();
    CHECK(!r.got_response);
    CHECK(wait_until([&] { return handshake_rows(e).size() >= before + 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const std::vector<AuditRow> v = handshake_rows(e);
    if (v.size() != before + 1) std::printf("  [%s] %zu new handshake rows\n", label, v.size() - before);
    CHECK(v.size() == before + 1);
    if (!v.empty()) {
      const std::string detail = lower(v.back().detail);
      for (const std::string& n : needles) {
        const bool ok = detail.find(lower(n)) != std::string::npos;
        if (!ok) std::printf("  [%s] detail '%s' lacks '%s'\n", label, v.back().detail.c_str(), n.c_str());
        CHECK(ok);
      }
      CHECK(v.back().actor.rfind("anonymous:127.0.0.1", 0) == 0 && v.back().outcome == "refused" && v.back().code == "handshake_failed");
    }
    CHECK(call(e, e.st1, "GET", "/v1/devices/dev-0001").status == 200);
  };
  expect_one("no certificate", [&] { return send_raw(e, nullptr, reg); }, {"claimed_cn=-"});
  expect_one("wrong CA", [&] { return send_raw(e, &e.evil_station, reg); }, {"claimed_cn=station-01", "unable to get local issuer certificate"});
  expect_one("expired", [&] { return send_raw(e, &e.expired, reg); }, {"claimed_cn=station-old", "certificate has expired"});
  expect_one("TLS 1.2", [&] { return send_raw(e, &e.st1, reg, TLS1_2_VERSION); }, {"protocol"});
  expect_one("wrong key usage", [&] { return send_raw(e, &e.wrong_eku, reg); }, {"claimed_cn=station-eku", "unsuitable certificate purpose"});
  // A certificate on the deny-list (--revoked-certs): fine until its serial is listed, then refused at the handshake.
  CHECK(call(e, e.revoked_station, "GET", "/v1/devices/dev-0001").status == 200);
  {
    std::ofstream f(e.dir / "revoked-certs.txt", std::ios::app);
    f << "# station-rev\n  0x" << serial_hex_of(e.revoked_station) << "  \n";
  }
  expect_one("revoked certificate", [&] { return send_raw(e, &e.revoked_station, reg); }, {"claimed_cn=station-rev", "certificate revoked"});
  // none of them registered anything
  CHECK(call(e, e.st1, "GET", "/v1/devices/dev-0300").status == 404);
  // a valid certificate of the right CA with a subject that is neither station nor admin: refused and logged (this one completes the handshake)
  const std::size_t hs = handshake_rows(e).size();
  Reply r = call(e, e.other_ou, "GET", "/v1/devices/dev-0001");
  CHECK(r.status == 403 && r.code() == "forbidden");
  CHECK(count_rows(e, "request", "forbidden", "refused") >= 1);
  CHECK(handshake_rows(e).size() == hs);
  CHECK(call(e, e.st1, "GET", "/v1/devices/dev-0001").status == 200);
}

void test_audit_integrity(Env& e) {
  CHECK(e.db->verify_audit().ok);
  const AuditVerify live = e.db->verify_audit();
  CHECK(live.entries > 10);
  // the append-only triggers refuse UPDATE and DELETE through the live connection
  sqlite3* raw = nullptr;
  CHECK(sqlite3_open_v2(e.db_path().c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
  char* msg = nullptr;
  CHECK(sqlite3_exec(raw, "UPDATE audit SET detail = 'x' WHERE seq = 1", nullptr, nullptr, &msg) != SQLITE_OK);
  CHECK(msg && std::strstr(msg, "append-only"));
  sqlite3_free(msg);
  msg = nullptr;
  CHECK(sqlite3_exec(raw, "DELETE FROM audit WHERE seq = 2", nullptr, nullptr, &msg) != SQLITE_OK);
  CHECK(msg && std::strstr(msg, "append-only"));
  sqlite3_free(msg);
  CHECK(sqlite3_exec(raw, "DELETE FROM audit", nullptr, nullptr, nullptr) != SQLITE_OK);
  sqlite3_close(raw);
  CHECK(e.db->verify_audit().ok && e.db->verify_audit().entries == live.entries);

  // tamper detection on copies: someone with file access drops the trigger and edits, deletes or truncates
  e.db->audit(AuditEvent{"system", "noop", "", "", "ok", "", ""});  // make sure the copy has the very latest state
  auto tampered = [&](const char* name, const char* sql) {
    const fs::path copy = e.dir / name;
    fs::copy_file(e.db_path(), copy, fs::copy_options::overwrite_existing);
    sqlite3* t = nullptr;
    sqlite3_open_v2(copy.c_str(), &t, SQLITE_OPEN_READWRITE, nullptr);
    sqlite3_exec(t, "DROP TRIGGER audit_no_update; DROP TRIGGER audit_no_delete;", nullptr, nullptr, nullptr);
    char* em = nullptr;
    const int rc = sqlite3_exec(t, sql, nullptr, nullptr, &em);
    if (rc != SQLITE_OK) std::printf("tamper sql failed: %s\n", em ? em : "?");
    sqlite3_free(em);
    sqlite3_close(t);
    return Database::verify_audit_file(copy.string());
  };
  AuditVerify v = tampered("t1.db", "UPDATE audit SET actor = 'station:mallory' WHERE seq = 3");
  CHECK(!v.ok && v.bad_seq == 3 && contains(v.message, "modified"));
  v = tampered("t2.db", "UPDATE audit SET outcome = 'ok', code = '' WHERE outcome = 'refused' AND seq = (SELECT MIN(seq) FROM audit WHERE outcome = 'refused')");
  CHECK(!v.ok);
  v = tampered("t3.db", "DELETE FROM audit WHERE seq = 4");
  CHECK(!v.ok && v.bad_seq == 4);
  v = tampered("t4.db", "DELETE FROM audit WHERE seq = (SELECT MAX(seq) FROM audit)");
  CHECK(!v.ok && contains(v.message, "truncated"));
  v = tampered("t5.db", "UPDATE audit SET ts = ts + 1 WHERE seq = 1");
  CHECK(!v.ok && v.bad_seq == 1);
  {  // dropping the triggers alone is reported
    const fs::path copy = e.dir / "t6.db";
    fs::copy_file(e.db_path(), copy, fs::copy_options::overwrite_existing);
    sqlite3* t = nullptr;
    sqlite3_open_v2(copy.c_str(), &t, SQLITE_OPEN_READWRITE, nullptr);
    sqlite3_exec(t, "DROP TRIGGER audit_no_delete", nullptr, nullptr, nullptr);
    sqlite3_close(t);
    v = Database::verify_audit_file(copy.string());
    CHECK(!v.ok && contains(v.message, "trigger"));
  }
  // the CLI: 0 on the live database, 7 on a tampered copy
  std::string out;
  CHECK(run_cmd(std::string(REGISTRY_EXE) + " verify-audit --db '" + e.db_path() + "'", &out) == 0 && contains(out, "audit chain ok"));
  CHECK(run_cmd(std::string(REGISTRY_EXE) + " verify-audit --db '" + (e.dir / "t1.db").string() + "'", &out) == 7 && contains(out, "AUDIT CHAIN FAILED"));
  CHECK(run_cmd(std::string(REGISTRY_EXE) + " verify-audit --db '" + (e.dir / "missing.db").string() + "'", &out) == 7);
  for (const char* n : {"t1.db", "t2.db", "t3.db", "t4.db", "t5.db", "t6.db"}) fs::remove(e.dir / n);
}

std::string dump_tables(const std::string& path) {
  sqlite3* db = nullptr;
  sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
  std::string out;
  for (const char* sql : {"SELECT * FROM devices ORDER BY id", "SELECT * FROM audit ORDER BY seq"}) {
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
    while (sqlite3_step(st) == SQLITE_ROW) {
      for (int i = 0; i < sqlite3_column_count(st); ++i) {
        if (sqlite3_column_type(st, i) == SQLITE_BLOB) out += hex_encode(static_cast<const std::uint8_t*>(sqlite3_column_blob(st, i)), static_cast<std::size_t>(sqlite3_column_bytes(st, i)));
        else if (const unsigned char* t = sqlite3_column_text(st, i)) out += reinterpret_cast<const char*>(t);
        out += '|';
      }
      out += '\n';
    }
    sqlite3_finalize(st);
  }
  sqlite3_close(db);
  return out;
}

void test_backup_restore(Env& e) {
  const fs::path second = e.dir.parent_path() / ("second-location." + std::to_string(::getpid()));
  BackupResult b;
  std::string err;
  // a second location is required
  CHECK(!backup_database(e.db_path(), e.backup_key, e.dir.string(), 1800000000, b, &err) && contains(err, "second location"));
  // the server is running and writing while the backup is taken
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    int i = 0;
    while (!stop) {
      e.db->audit(AuditEvent{"system", "load", "", "", "ok", "", std::to_string(i++)});
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });
  CHECK(backup_database(e.db_path(), e.backup_key, second.string(), 1800000000, b, &err));
  stop = true;
  writer.join();
  CHECK(b.audit_ok && b.path == (second / "registry-20270115T080000Z.vrb").string());
  struct stat st{};
  CHECK(::stat(b.path.c_str(), &st) == 0 && (st.st_mode & 077) == 0);
  const Bytes file = read_all(b.path);
  CHECK(file.size() == kBackupHeaderSize + b.plaintext_bytes + 16 * ((b.plaintext_bytes + kBackupChunkSize - 1) / kBackupChunkSize));
  CHECK(std::memcmp(file.data(), "VRB1", 4) == 0);
  CHECK(!contains(file, "SQLite format 3", 15));  // encrypted: no plaintext database header ...
  CHECK(!contains(file, "SN-0001", 7) && !contains(file, "station-01", 10));  // ... and no row data

  // restore to a fresh path: byte-equivalent to the snapshot, rows and chain verify
  BackupResult rr;
  const std::string target = (e.dir / "restored.db").string();
  CHECK(restore_backup(b.path, e.backup_key, target, false, rr, &err));
  CHECK(rr.audit_ok && rr.plaintext_sha256 == b.plaintext_sha256 && rr.plaintext_bytes == b.plaintext_bytes);
  const Bytes restored = read_all(target);
  CHECK(restored.size() >= b.plaintext_bytes);  // the snapshot, plus the appended `restore` audit entry
  CHECK(Database::verify_audit_file(target).ok);
  {  // every device and audit row of the original up to the snapshot survives, in order; the last row is the restore entry
    std::string a = dump_tables(target);
    CHECK(!a.empty() && a.back() == '\n');
    const std::size_t last = a.rfind('\n', a.size() - 2);
    CHECK(contains(a.substr(last + 1), "|restore|") && contains(a.substr(last + 1), "head_seq=" + std::to_string(b.head_seq)));
    a.resize(last + 1);
    const std::string orig = dump_tables(e.db_path());
    CHECK(orig.compare(0, a.size(), a) == 0);
  }
  CHECK(fs::exists(second / "registry-20270115T080000Z.anchor"));
  {  // the restored database opens and keeps working: a new audit row chains onto the old ones
    auto again = Database::open(target, &err);
    CHECK(again && again->verify_audit().ok);
    const auto n = again->verify_audit().entries;
    CHECK(again->audit(AuditEvent{"system", "after_restore", "", "", "ok", "", ""}) && again->verify_audit().ok && again->verify_audit().entries == n + 1);
    CHECK(again->find_device("dev-0001").has_value());
  }
  fs::remove(target);
  // an existing target is not replaced without --force
  put_file(target, "x", 1, 0600);
  CHECK(!restore_backup(b.path, e.backup_key, target, false, rr, &err) && contains(err, "already exists"));
  CHECK(restore_backup(b.path, e.backup_key, target, true, rr, &err));
  fs::remove(target);

  // wrong key, damaged file, truncation, extension: all refused, nothing written
  Key32 wrong = e.backup_key;
  wrong[0] ^= 1;
  CHECK(!restore_backup(b.path, wrong, target, false, rr, &err) && contains(err, "authentication failed") && !fs::exists(target));
  Key32 kek_as_backup = e.kek;
  CHECK(!restore_backup(b.path, kek_as_backup, target, false, rr, &err) && !fs::exists(target));
  Bytes dmg = file;
  for (std::size_t pos : {std::size_t{0}, std::size_t{9}, std::size_t{20}, std::size_t{50}, std::size_t{64}, file.size() / 2, file.size() - 1}) {
    dmg = file;
    dmg[pos] ^= 0x01;
    put_file(e.dir / "dmg.vrb", dmg.data(), dmg.size(), 0600);
    CHECK(!restore_backup((e.dir / "dmg.vrb").string(), e.backup_key, target, false, rr, &err) && !fs::exists(target));
  }
  dmg.assign(file.begin(), file.end() - 1);
  put_file(e.dir / "dmg.vrb", dmg.data(), dmg.size(), 0600);
  CHECK(!restore_backup((e.dir / "dmg.vrb").string(), e.backup_key, target, false, rr, &err) && contains(err, "wrong size"));
  dmg = file;
  dmg.push_back(0);
  put_file(e.dir / "dmg.vrb", dmg.data(), dmg.size(), 0600);
  CHECK(!restore_backup((e.dir / "dmg.vrb").string(), e.backup_key, target, false, rr, &err));
  // dropping whole chunks (with a patched length) is detected too: the header is bound into every chunk
  {
    Bytes big(kBackupChunkSize * 3 + 17, 0x5a), out;
    const Bytes enc = encrypt_backup(big, e.backup_key, 1);
    CHECK(decrypt_backup(enc, e.backup_key, out, &err) && out == big);
    Bytes cut(enc.begin(), enc.begin() + kBackupHeaderSize + (kBackupChunkSize + 16) * 2);
    for (int i = 0; i < 8; ++i) cut[48 + i] = 0;
    const std::uint64_t len = static_cast<std::uint64_t>(kBackupChunkSize) * 2;
    for (int i = 0; i < 8; ++i) cut[48 + i] = static_cast<std::uint8_t>((len >> (8 * (7 - i))) & 0xff);
    CHECK(!decrypt_backup(cut, e.backup_key, out, &err));
    Bytes empty_enc = encrypt_backup(Bytes{}, e.backup_key, 1);
    CHECK(empty_enc.size() == kBackupHeaderSize + 16 && decrypt_backup(empty_enc, e.backup_key, out, &err) && out.empty());
  }
  fs::remove(e.dir / "dmg.vrb");

  // a database whose audit chain is broken is still backed up, but reported
  {
    const fs::path bad_db = e.dir / "bad.db";
    fs::copy_file(e.db_path(), bad_db, fs::copy_options::overwrite_existing);
    sqlite3* t = nullptr;
    sqlite3_open_v2(bad_db.c_str(), &t, SQLITE_OPEN_READWRITE, nullptr);
    sqlite3_exec(t, "DROP TRIGGER audit_no_update; UPDATE audit SET detail = 'tampered' WHERE seq = 2", nullptr, nullptr, nullptr);
    sqlite3_close(t);
    BackupResult bb;
    CHECK(backup_database(bad_db.string(), e.backup_key, (e.dir.parent_path() / "second-bad").string(), 1800000001, bb, &err) && !bb.audit_ok);
    std::string out;
    CHECK(run_cmd(std::string(REGISTRY_EXE) + " backup --db '" + bad_db.string() + "' --backup-key '" + (e.dir / "backup.key").string() + "' --kek '" + (e.dir / "kek.aes256").string() + "' --dest-dir '" + (e.dir.parent_path() / "second-bad2").string() + "'", &out) == 7 &&
          contains(out, "WARNING"));
    fs::remove_all(e.dir.parent_path() / "second-bad");
    fs::remove_all(e.dir.parent_path() / "second-bad2");
    fs::remove(bad_db);
  }

  // the CLI round trip, and a backup key with open permissions is refused
  std::string out;
  const std::string bk = (e.dir / "backup.key").string();
  CHECK(run_cmd(std::string(REGISTRY_EXE) + " backup --db '" + e.db_path() + "' --backup-key '" + bk + "' --kek '" + (e.dir / "kek.aes256").string() + "' --dest-dir '" + (e.dir.parent_path() / "second-cli").string() + "'", &out) == 0 && contains(out, "backup written"));
  std::string newest;
  for (const auto& de : fs::directory_iterator(e.dir.parent_path() / "second-cli"))
    if (de.path().extension() == ".vrb") newest = de.path().string();
  CHECK(!newest.empty());
  CHECK(run_cmd(std::string(REGISTRY_EXE) + " restore --backup '" + newest + "' --backup-key '" + bk + "' --db '" + (e.dir / "cli-restored.db").string() + "'", &out) == 0 && contains(out, "restored"));
  CHECK(Database::verify_audit_file((e.dir / "cli-restored.db").string()).ok);
  CHECK(run_cmd(std::string(REGISTRY_EXE) + " restore --backup '" + newest + "' --backup-key '" + (e.dir / "kek.aes256").string() + "' --db '" + (e.dir / "cli-restored2.db").string() + "'", &out) == 9);
  ::chmod(bk.c_str(), 0644);
  CHECK(run_cmd(std::string(REGISTRY_EXE) + " backup --db '" + e.db_path() + "' --backup-key '" + bk + "' --kek '" + (e.dir / "kek.aes256").string() + "' --dest-dir '" + (e.dir.parent_path() / "second-cli2").string() + "'", &out) == 4 && contains(out, "too open"));
  ::chmod(bk.c_str(), 0600);
  fs::remove_all(e.dir.parent_path() / "second-cli");
  fs::remove(e.dir / "cli-restored.db");
}

std::string b64(const Bytes& in, bool url, bool pad) {
  static const char* std_tab = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  static const char* url_tab = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  const char* t = url ? url_tab : std_tab;
  std::string out;
  std::size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const std::uint32_t v = in[i] << 16 | in[i + 1] << 8 | in[i + 2];
    out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
  }
  if (i + 1 == in.size()) {
    const std::uint32_t v = in[i] << 16;
    out += t[v >> 18]; out += t[(v >> 12) & 63];
    if (pad) out += "==";
  } else if (i + 2 == in.size()) {
    const std::uint32_t v = in[i] << 16 | in[i + 1] << 8;
    out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63];
    if (pad) out += "=";
  }
  return out;
}

// All the ways a 32-byte secret could be written down: raw, hex (both cases), base64 and base64url (padded or not). base64 has three alignments, so each secret is
// searched with a one- and two-byte prefix too (the encoded middle is the same for every alignment of the same bytes).
std::vector<std::string> needles_of(const Bytes& s) {
  std::vector<std::string> n;
  n.emplace_back(s.begin(), s.end());
  const std::string h = hex_encode(s.data(), s.size());
  n.push_back(h);
  std::string up = h;
  for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  n.push_back(up);
  for (const bool url : {false, true}) {
    n.push_back(b64(s, url, true));
    n.push_back(b64(s, url, false));
    for (std::size_t skip = 1; skip <= 2; ++skip) {  // the 4-character groups fully inside the secret, for the other two alignments
      Bytes padded(skip, 0);
      padded.insert(padded.end(), s.begin(), s.end());
      std::string e = b64(padded, url, false);
      n.push_back(e.substr(4, 40));  // 30 bytes' worth of groups that no longer depend on the bytes before the secret
    }
  }
  return n;
}

// Scans every regular file under `base` (and `extra`, e.g. the captured log) for every secret in every encoding. Excluded: the PKI private keys, the KEK file and the
// backup key file (those are the secrets' legitimate homes). Returns the files that hit.
std::vector<std::string> scan_tree(const fs::path& base, const std::string& extra, const std::set<std::string>& exclude_names) {
  std::vector<Bytes> secrets;
  {
    std::lock_guard<std::mutex> lk(g_secret_mu);
    secrets = g_secrets;
  }
  std::vector<std::string> needles;
  for (const Bytes& sec : secrets)
    for (std::string& n : needles_of(sec)) needles.push_back(std::move(n));
  std::vector<std::string> hits;
  auto check = [&](const std::string& name, const Bytes& data) {
    for (const std::string& n : needles)
      if (!n.empty() && contains(data, n.data(), n.size())) {
        hits.push_back(name);
        return;
      }
  };
  for (const auto& de : fs::recursive_directory_iterator(base)) {
    if (!de.is_regular_file()) continue;
    if (exclude_names.count(de.path().filename().string())) continue;
    check(de.path().string(), read_all(de.path()));
  }
  if (!extra.empty()) check("<log>", Bytes(extra.begin(), extra.end()));
  return hits;
}

const std::set<std::string> kLegitimateSecretFiles = {"server.key", "other.key", "kek.aes256", "backup.key", "kek-open", "kek-short"};

void test_no_plaintext_keys(Env& e) {
  // after everything above (issuance, backup, restore, failures): no content key, KEK, backup key or device private key, in any encoding, in any file the registry or the
  // tests wrote (database, journal, -wal/-shm, temp files, backups, anchors, captured server output) nor in the log
  const fs::path base = e.dir.parent_path();
  const std::vector<std::string> hits = scan_tree(base, log_copy(), kLegitimateSecretFiles);
  for (const std::string& h : hits) std::printf("  plaintext secret found in %s\n", h.c_str());
  CHECK(hits.empty());
  // negative control: the scan really finds a planted key, in each encoding
  const Bytes planted(e.libs[0].key.begin(), e.libs[0].key.end());
  const std::vector<std::string> forms = {std::string(planted.begin(), planted.end()), hex_encode(planted.data(), 32), b64(planted, false, true), b64(planted, true, false)};
  const fs::path plant_dir = base.parent_path() / ("plant." + std::to_string(::getpid()));
  fs::create_directories(plant_dir);
  for (std::size_t i = 0; i < forms.size(); ++i) {
    const fs::path f = plant_dir / ("planted-" + std::to_string(i) + ".txt");
    put_file(f, forms[i].data(), forms[i].size(), 0600);
    CHECK(!scan_tree(plant_dir, "", kLegitimateSecretFiles).empty());
    fs::remove(f);
  }
  CHECK(scan_tree(plant_dir, "", kLegitimateSecretFiles).empty());  // and an empty tree is clean
  CHECK(!scan_tree(plant_dir, "prefix " + hex_encode(planted.data(), 32), kLegitimateSecretFiles).empty());
  fs::remove_all(plant_dir);
  CHECK(!log_copy().empty());
}

void test_fuzz(Env& e) {
  // 1. the HTTP parser on random bytes, random prefixes of valid requests and single-byte mutations: never crashes, never completes beyond the limits
  std::mt19937_64 rng(0x6008);
  const HttpLimits lim;
  const std::string valid = http_request("POST", "/v1/devices", register_body("dev-f", random_pub(), "SN-F"));
  int completes = 0;
  for (int i = 0; i < 4000; ++i) {
    std::string s;
    switch (i % 4) {
      case 0: {
        s.resize(rng() % 300);
        for (char& c : s) c = static_cast<char>(rng());
        break;
      }
      case 1: s = valid.substr(0, rng() % (valid.size() + 1)); break;
      case 2: {
        s = valid;
        for (int k = 0; k < 3; ++k) s[rng() % s.size()] = static_cast<char>(rng());
        break;
      }
      default: {
        s = valid + std::string(rng() % 4, 'x');
        break;
      }
    }
    HttpRequest req;
    const ParseResult pr = parse_http_request(s, lim, req);
    if (pr.state == ParseState::kComplete) {
      ++completes;
      CHECK(req.body.size() <= lim.max_body_bytes && !req.method.empty() && req.target[0] == '/');
    } else if (pr.state == ParseState::kError) {
      CHECK(pr.status >= 400 && pr.code && *pr.code);
    }
  }
  CHECK(completes > 100);  // the unmodified and some mutated requests are still complete
  {
    HttpRequest req;
    CHECK(parse_http_request("POST /v1/devices HTTP/1.1\r\nContent-Length: 70000\r\n\r\n", lim, req).status == 413);
    CHECK(parse_http_request("POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx", lim, req).state == ParseState::kError);
    CHECK(parse_http_request("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", lim, req).status == 501);
    CHECK(parse_http_request("GET / HTTP/1.0\r\n\r\n", lim, req).status == 505);
    CHECK(parse_http_request("GET /\r\n\r\n", lim, req).state == ParseState::kError);
    CHECK(parse_http_request("GET /a b HTTP/1.1\r\n\r\n", lim, req).state == ParseState::kError);
    CHECK(parse_http_request("GET / HTTP/1.1\r\n folded: x\r\n\r\n", lim, req).state == ParseState::kError);
    CHECK(parse_http_request("GET / HTTP/1.1\r\nA: b\r\n\r\nEXTRA", lim, req).state == ParseState::kError);
    CHECK(parse_http_request(std::string(9000, 'a'), lim, req).status == 431);
    CHECK(parse_http_request("GET / HTTP/1.1\r\n\r\n", lim, req).state == ParseState::kComplete);
    // bare CR / LF inside the head (header splitting) is refused
    CHECK(parse_http_request("GET / HTTP/1.1\r\nA: b\rX-Injected: 1\r\n\r\n", lim, req).state == ParseState::kError);
    CHECK(parse_http_request("GET / HTTP/1.1\r\nA: b\nX-Injected: 1\r\n\r\n", lim, req).state == ParseState::kError);
    CHECK(parse_http_request("GET /\n HTTP/1.1\r\n\r\n", lim, req).state == ParseState::kError);
  }
  // 2. the service on mutated JSON bodies: only 201 / 400 / 409, always a JSON body, never a 500
  Principal station{Role::kStation, "station-fuzz", "127.0.0.1"};
  std::set<int> seen;
  for (int i = 0; i < 1500; ++i) {
    std::string body = register_body("fz-" + std::to_string(i), random_pub(), "FZ-" + std::to_string(i));
    switch (i % 5) {
      case 0: for (int k = 0; k < 1 + i % 4; ++k) body[rng() % body.size()] = static_cast<char>(rng()); break;
      case 1: body = body.substr(0, rng() % body.size()); break;
      case 2: body.insert(rng() % body.size(), std::string(1 + rng() % 3, "{}[]\",:\\"[rng() % 8])); break;
      case 3: body = std::string(1 + rng() % 40, "[{\"0-eE.,:tfn"[rng() % 14]); break;
      default: break;  // valid
    }
    HttpRequest req;
    req.method = "POST";
    req.path = req.target = "/v1/devices";
    req.body = body;
    const HttpResponse resp = e.service->handle(req, station);
    seen.insert(resp.status);
    CHECK(resp.status == 201 || resp.status == 400 || resp.status == 409);
    fmt::JsonValue v;
    CHECK(fmt::parse_json(resp.body, v) && v.is_object());
  }
  CHECK(seen.count(201) && seen.count(400));
  {  // deep nesting and huge numbers
    HttpRequest req;
    req.method = "POST";
    req.path = req.target = "/v1/devices";
    req.body = std::string(5000, '[') + std::string(5000, ']');
    CHECK(e.service->handle(req, station).status == 400);
    req.body = "{\"device_id\":" + std::string(300, '9') + "}";
    CHECK(e.service->handle(req, station).status == 400);
  }
  // 3. over TLS: oversized body, oversized header, wrong transfer encoding, garbage after the handshake: typed errors, and the server keeps serving
  Reply r = send_raw(e, &e.st1, http_request("POST", "/v1/devices", std::string(70000, 'a')));
  CHECK(r.got_response && r.status == 413 && r.code() == "body_too_large");
  r = send_raw(e, &e.st1, "GET /v1/devices/x HTTP/1.1\r\nX-Big: " + std::string(9000, 'a') + "\r\n\r\n");
  CHECK(r.got_response && r.status == 431);
  r = send_raw(e, &e.st1, "POST /v1/devices HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
  CHECK(r.got_response && r.status == 501);
  r = send_raw(e, &e.st1, "GET /v1/devices/x HTTP/1.0\r\n\r\n");
  CHECK(r.got_response && r.status == 505);
  r = send_raw(e, &e.st1, std::string("\x16\x03\x01\x00\x05garbage\r\n\r\n", 18));
  CHECK(r.got_response && r.status == 400);
  r = send_raw(e, &e.st1, http_request("POST", "/v1/devices", "{not json"));
  CHECK(r.got_response && r.status == 400 && r.code() == "invalid_request");
  for (int i = 0; i < 20; ++i) {
    std::string junk(1 + rng() % 200, 'x');
    for (char& c : junk) c = static_cast<char>(rng());
    r = send_raw(e, &e.st1, junk + "\r\n\r\n");
    CHECK(!r.got_response || r.status >= 400);
  }
  CHECK(call(e, e.st1, "GET", "/v1/devices/dev-0001").status == 200);
  CHECK(wait_until([&] { return count_rows(e, "request", "body_too_large", "refused") >= 1; }));
  CHECK(e.db->verify_audit().ok);
}

void test_concurrency(Env& e) {
  // 12 parallel TLS registrations of ONE serial under different device ids and keys: exactly one wins
  std::vector<std::thread> th;
  std::atomic<int> created{0}, conflicts{0}, other{0};
  std::vector<DevKeys> keys(12);
  for (int i = 0; i < 12; ++i)
    th.emplace_back([&, i] {
      Reply r = call(e, i % 2 ? e.st1 : e.st2, "POST", "/v1/devices", register_body("race-" + std::to_string(i), keys[i].pub, "SN-RACE"));
      if (r.status == 201) ++created;
      else if (r.status == 409 && r.code() == "duplicate_serial") ++conflicts;
      else {
        ++other;
        std::printf("race: unexpected reply tls=%d status=%d code=%s\n", r.tls_ok, r.status, r.code().c_str());
      }
    });
  for (auto& t : th) t.join();
  CHECK(created == 1 && conflicts == 11 && other == 0);
  // and the same device id
  th.clear();
  created = conflicts = other = 0;
  std::vector<DevKeys> keys2(12);
  for (int i = 0; i < 12; ++i)
    th.emplace_back([&, i] {
      Reply r = call(e, e.st1, "POST", "/v1/devices", register_body("race-id", keys2[i].pub, "SN-RACE-ID-" + std::to_string(i)));
      if (r.status == 201) ++created;
      else if (r.status == 409 && r.code() == "duplicate_device_id") ++conflicts;
      else ++other;
    });
  for (auto& t : th) t.join();
  CHECK(created == 1 && conflicts == 11 && other == 0);
  // directly on the database from many threads (no TLS in between)
  th.clear();
  std::atomic<int> won{0};
  std::vector<DevKeys> dk(16);
  for (int i = 0; i < 16; ++i)
    th.emplace_back([&, i] {
      NewDevice n;
      n.device_id = "dbrace-" + std::to_string(i);
      n.public_key = dk[i].pub;
      n.som_id = "som";
      n.serial_number = "SN-DBRACE";
      n.tier = "standard";
      n.provisioning_date = "2026-10-20";
      n.station_id = "st";
      Device out;
      if (e.db->register_device(n, "station:st", out) == DbStatus::kOk) ++won;
    });
  for (auto& t : th) t.join();
  CHECK(won == 1);
  // parallel issuance while registrations run: every response unwraps
  DevKeys d;
  CHECK(call(e, e.st1, "POST", "/v1/devices", register_body("dev-par", d.pub, "SN-PAR")).status == 201);
  th.clear();
  std::atomic<int> good{0};
  for (int i = 0; i < 16; ++i)
    th.emplace_back([&, i] {
      const Lib& l = e.libs[static_cast<std::size_t>(i) % e.libs.size()];
      Reply r = call(e, e.st1, "GET", "/v1/devices/dev-par/wrapped-keys?library_id=" + l.id);
      if (r.status != 200) return;
      Key32 got{};
      if (cp::wrapped_key_open(d.priv.get(), unhex(r.json().find("keys")->array[0].find("wrapped_key")->string), l.id, got) && got == l.key) ++good;
    });
  for (auto& t : th) t.join();
  CHECK(good == 16);
  CHECK(e.db->verify_audit().ok);
}

void test_rate_limit_and_limits(Env& e) {
  // a separate Service with a tiny limit on the same database
  RateLimitConfig rl;
  rl.max_requests = 3;
  Service limited(*e.db, *e.vault, rl, capture_log);
  Principal p{Role::kStation, "station-rl", "127.0.0.1"};
  int ok = 0, limited_n = 0;
  for (int i = 0; i < 8; ++i) {
    const HttpResponse r = direct(limited, p, "GET", "/v1/devices/dev-0001");
    if (r.status == 200) ++ok;
    if (r.status == 429) {
      ++limited_n;
      CHECK(r.headers.size() == 1 && r.headers[0].first == "Retry-After");
    }
  }
  CHECK(ok == 3 && limited_n == 5);
  CHECK(count_rows(e, "request", "rate_limited", "refused") == 1);  // the first refusal of the window; the rest only counted
  // pre-router refusals: the first per (actor, code) per window is audited, the rest only counted
  {
    const std::string who = "station:throttled";
    const int before_rows = count_rows(e, "request", "bad_request", "refused");
    for (int i = 0; i < 6; ++i) e.service->audit_refusal_throttled(who, "request", "bad_request", "http 400");
    e.service->audit_refusal_throttled(who, "request", "body_too_large", "http 413");
    e.service->audit_refusal_throttled("station:other", "request", "bad_request", "http 400");
    int mine = 0, other_code = 0, other_actor = 0;
    for (const AuditRow& a : rows_of(e)) {
      if (a.actor == who && a.code == "bad_request" && a.action == "request") ++mine;
      if (a.actor == who && a.code == "body_too_large") ++other_code;
      if (a.actor == "station:other" && a.code == "bad_request") ++other_actor;
    }
    CHECK(mine == 1 && other_code == 1 && other_actor == 1);
    CHECK(count_rows(e, "request", "bad_request", "refused") == before_rows + 2);
  }
  // another principal is not affected
  Principal q{Role::kStation, "station-other", "127.0.0.1"};
  CHECK(direct(limited, q, "GET", "/v1/devices/dev-0001").status == 200);
  // the limiter with an injected clock: the window rolls over
  std::int64_t now = 100;
  RateLimiter lim(2, 10, [&] { return now; });
  CHECK(lim.check("a") == 0 && lim.check("a") == 0 && lim.check("a") == 1 && lim.check("a") == 2);
  now = 111;
  CHECK(lim.check("a") == 0);
  // an unauthenticated principal is refused by the service itself too
  Principal anon;
  anon.remote = "10.0.0.9";
  CHECK(direct(*e.service, anon, "GET", "/v1/devices/dev-0001").status == 401);
  CHECK(count_rows(e, "request", "unauthenticated", "refused") >= 1);
}

void test_timeout(const fs::path& dir) {
  Env e;
  EnvOptions opt;
  opt.io_timeout_ms = 600;
  make_env(e, dir, opt);
  // a client that completes the handshake and then sends nothing gets 408 after the I/O timeout
  const auto t0 = std::chrono::steady_clock::now();
  Reply r = send_raw(e, &e.st1, "");
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  CHECK(r.got_response && r.status == 408 && r.code() == "request_timeout");
  CHECK(ms >= 400 && ms < 5000);
  // a client that dribbles a partial request is cut off too
  r = send_raw(e, &e.st1, "POST /v1/devices HTTP/1.1\r\nContent-Length: 100\r\n\r\n{\"a\":");
  CHECK(r.got_response && r.status == 408);
  CHECK(call(e, e.st1, "GET", "/v1/devices/none").status == 404);
  CHECK(e.db->verify_audit().ok);
  e.server->stop();
}

void test_config_refusals(Env& e) {
  std::string err;
  // KEK: wrong size, open permissions, missing
  put_file(e.dir / "kek-short", "short", 5, 0600);
  CHECK(!KeyVault::create(e.vault_dir(), (e.dir / "kek-short").string(), false, &err) && contains(err, "exactly 32"));
  put_file(e.dir / "kek-open", e.kek.data(), 32, 0644);
  CHECK(!KeyVault::create(e.vault_dir(), (e.dir / "kek-open").string(), false, &err) && contains(err, "too open"));
  CHECK(!contains(err, hex(e.kek)));
  CHECK(!KeyVault::create(e.vault_dir(), (e.dir / "kek-missing").string(), false, &err));
  CHECK(!KeyVault::create((e.dir / "no-such-dir").string(), (e.dir / "kek.aes256").string(), false, &err));
  // server: private key with open permissions, certificate that does not match the key, no CA
  ServerConfig cfg;
  cfg.cert_file = (e.dir / "server.crt").string();
  cfg.key_file = (e.dir / "server.key").string();
  cfg.client_ca_file = (e.dir / "root.crt").string();
  ::chmod(cfg.key_file.c_str(), 0644);
  {
    Server s(cfg, *e.service, capture_log);
    CHECK(!s.start(&err) && contains(err, "too open"));
  }
  ::chmod(cfg.key_file.c_str(), 0600);
  pki::write_key(e.st1, (e.dir / "other.key").string());
  cfg.key_file = (e.dir / "other.key").string();
  {
    Server s(cfg, *e.service, capture_log);
    CHECK(!s.start(&err) && (contains(err, "does not match") || contains(err, "mismatch")));
  }
  cfg.key_file = (e.dir / "server.key").string();
  cfg.client_ca_file = (e.dir / "missing.crt").string();
  {
    Server s(cfg, *e.service, capture_log);
    CHECK(!s.start(&err));
  }
  // the database: a newer schema version is refused, nothing is overwritten
  {
    const fs::path p = e.dir / "future.db";
    sqlite3* t = nullptr;
    sqlite3_open(p.c_str(), &t);
    sqlite3_exec(t, "PRAGMA user_version = 99", nullptr, nullptr, nullptr);
    sqlite3_close(t);
    CHECK(Database::open(p.string(), &err) == nullptr && contains(err, "schema version"));
    struct stat st{};
    auto fresh = Database::open((e.dir / "fresh.db").string(), &err);
    CHECK(fresh && ::stat((e.dir / "fresh.db").c_str(), &st) == 0 && (st.st_mode & 077) == 0);  // created 0600
  }
  // CLI usage errors
  std::string out;
  CHECK(run_cmd(std::string(REGISTRY_EXE), &out) == 2);
  CHECK(run_cmd(std::string(REGISTRY_EXE) + " serve --db x", &out) == 2);
  CHECK(run_cmd(std::string(REGISTRY_EXE) + " frobnicate", &out) == 2);
  CHECK(run_cmd(std::string(REGISTRY_EXE) + " serve --db '" + e.db_path() + "' --vault-dir '" + e.vault_dir() + "' --kek '" + (e.dir / "kek-open").string() + "' --server-cert x --server-key y --client-ca z", &out) == 4);
}

// The shipped binary: serve on an ephemeral port (port file), take a request over mutual TLS, stop on SIGTERM.
void test_cli_serve(const fs::path& dir) {
  Env e;
  make_env(e, dir);
  e.server->stop();  // use the environment's PKI, vault and database files, not its in-process server
  e.service.reset();
  e.vault.reset();
  e.db.reset();
  const fs::path portfile = dir / "port";
  std::fflush(nullptr);
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::freopen((dir / "serve.stderr").c_str(), "w", stderr);
    ::freopen((dir / "serve.stdout").c_str(), "w", stdout);
    ::execl(REGISTRY_EXE, REGISTRY_EXE, "serve", "--db", e.db_path().c_str(), "--vault-dir", e.vault_dir().c_str(), "--kek", (dir / "kek.aes256").c_str(), "--server-cert",
            (dir / "server.crt").c_str(), "--server-key", (dir / "server.key").c_str(), "--client-ca", (dir / "root.crt").c_str(), "--port", "0", "--port-file",
            portfile.c_str(), "--root", "dev", static_cast<char*>(nullptr));
    ::_exit(127);
  }
  CHECK(pid > 0);
  CHECK(wait_until([&] { return fs::exists(portfile) && fs::file_size(portfile) > 1; }, 8000));
  const Bytes port_text = read_all(portfile);
  const unsigned port = static_cast<unsigned>(std::atoi(std::string(port_text.begin(), port_text.end()).c_str()));
  CHECK(port > 0);
  // reuse the in-process client by pointing a throw-away Server object's port: build the request by hand
  struct Fake { std::uint16_t p; };
  // (a minimal client: same code path as send_raw but with an explicit port)
  auto request_on = [&](const pki::Ident* id, const std::string& raw) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_load_verify_locations(ctx, (dir / "root.crt").c_str(), nullptr);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    if (id) {
      SSL_CTX_use_certificate(ctx, id->cert.get());
      SSL_CTX_use_PrivateKey(ctx, id->key.get());
    }
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<std::uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    std::string in;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
      SSL* ssl = SSL_new(ctx);
      SSL_set_fd(ssl, fd);
      X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), "127.0.0.1");
      if (SSL_connect(ssl) == 1) {
        SSL_write(ssl, raw.data(), static_cast<int>(raw.size()));
        char buf[2048];
        int n;
        while ((n = SSL_read(ssl, buf, sizeof buf)) > 0) in.append(buf, static_cast<std::size_t>(n));
      }
      SSL_free(ssl);
    }
    ::close(fd);
    SSL_CTX_free(ctx);
    ERR_clear_error();
    return in;
  };
  DevKeys d;
  std::string in = request_on(&e.st1, http_request("POST", "/v1/devices", register_body("cli-dev", d.pub, "SN-CLI")));
  CHECK(in.rfind("HTTP/1.1 201", 0) == 0);
  in = request_on(&e.st1, http_request("POST", "/v1/devices/cli-dev/wrapped-keys", "{\"library_ids\":[\"" + e.libs[0].id + "\"]}"));
  CHECK(in.rfind("HTTP/1.1 200", 0) == 0);
  {
    fmt::JsonValue v;
    fmt::parse_json(in.substr(in.find("\r\n\r\n") + 4), v);
    Key32 got{};
    CHECK(cp::wrapped_key_open(d.priv.get(), unhex(v.find("keys")->array[0].find("wrapped_key")->string), e.libs[0].id, got) && got == e.libs[0].key);
  }
  CHECK(request_on(nullptr, http_request("GET", "/v1/devices/cli-dev", "")).rfind("HTTP/1.1", 0) != 0);
  ::kill(pid, SIGTERM);
  int st = 0;
  CHECK(::waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
  std::string err;
  auto db = Database::open(e.db_path(), &err);
  CHECK(db && db->verify_audit().ok);
  bool started = false, stopped = false, handshake = false;
  for (const AuditRow& a : db->audit_rows(1, 1000)) {
    if (a.action == "service_started" && contains(a.detail, "root=dev kek_id=")) started = true;
    if (a.action == "service_stopped") stopped = true;
    if (a.action == "tls_handshake_failed") handshake = true;
  }
  CHECK(started && stopped && handshake);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// Review round: ids, keys, authorisation, races, append-only, backup matrix, limits

Key32 key_from_hex(const char* h) {
  Key32 k{};
  hex_decode32(h, k);
  return k;
}

std::string register_body2(const std::string& id, const Key32& pub, const std::string& serial, const std::string& som, const std::string& replaces = "") {
  return "{\"device_id\":\"" + id + "\",\"device_public_key\":\"" + hex(pub) + "\",\"som_id\":\"" + som + "\",\"serial_number\":\"" + serial +
         "\",\"tier\":\"standard\",\"provisioning_date\":\"2026-10-20\"" + (replaces.empty() ? "" : ",\"replaces\":\"" + replaces + "\"") + "}";
}

void test_ids_keys_and_supersede(Env& e) {
  // X25519 public key validation: bit 255, non-canonical values, every known small-order encoding
  Key32 bit255 = random_key();
  bit255[31] |= 0x80;
  Key32 all_ff;
  all_ff.fill(0xff);
  Key32 p;
  p.fill(0xff);
  p[31] = 0x7f;
  p[0] = 0xed;
  Key32 p_plus_1 = p, p_minus_1 = p;
  p_plus_1[0] = 0xee;
  p_minus_1[0] = 0xec;
  Key32 zero{}, one{};
  one[0] = 1;
  const std::vector<std::pair<const char*, Key32>> bad_keys = {
      {"bit 255 set", bit255}, {"all ones", all_ff}, {"p", p}, {"p+1", p_plus_1}, {"p-1", p_minus_1}, {"zero", zero}, {"one", one},
      {"order-8 a", key_from_hex("e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800")},
      {"order-8 b", key_from_hex("5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157")}};
  int n = 0;
  for (const auto& [label, k] : bad_keys) {
    const Reply r = call(e, e.st1, "POST", "/v1/devices", register_body("badkey-" + std::to_string(n), k, "SN-BADKEY-" + std::to_string(n)));
    if (r.status != 400 || r.code() != "invalid_field") std::printf("  bad key '%s' got %d %s\n", label, r.status, r.code().c_str());
    CHECK(r.status == 400 && r.code() == "invalid_field" && contains(r.body, "device_public_key"));
    ++n;
  }
  CHECK(call(e, e.st1, "POST", "/v1/devices", register_body("goodkey-0", random_pub(), "SN-GOODKEY")).status == 201);  // control: a random point passes
  CHECK(acceptable_x25519_public(cp::x25519_public(random_key()), nullptr));

  // device_id / replaces: no '/', no upper case; routing safe
  for (const char* id : {"dev/x", "DEV-UP", "dev x", "-dev", "d?x", "d%x", "../x"}) {
    const Reply r = call(e, e.st1, "POST", "/v1/devices", register_body(id, random_pub(), "SN-ID-X"));
    CHECK(r.status == 400 && r.code() == "invalid_field");
  }
  CHECK(call(e, e.st1, "POST", "/v1/devices", register_body("ok-id", random_pub(), "SN-ID-OK", "a/b")).code() == "invalid_field");
  CHECK(call(e, e.st1, "POST", "/v1/devices", register_body("ok-id", random_pub(), "SN-ID-OK", "UP")).code() == "invalid_field");
  CHECK(call(e, e.st1, "GET", "/v1/devices/UPPER").code() == "invalid_device_id");
  CHECK(call(e, e.st1, "GET", "/v1/devices/a%2Fb").code() == "invalid_device_id");
  // serial numbers are unique case-insensitively
  CHECK(call(e, e.st1, "POST", "/v1/devices", register_body("fold-1", random_pub(), "SN-FoLd")).status == 201);
  Reply r = call(e, e.st1, "POST", "/v1/devices", register_body("fold-2", random_pub(), "sn-fold"));
  CHECK(r.status == 409 && r.code() == "duplicate_serial");

  // supersede rule: the replacement must carry the replaced unit's serial number (any case) or its SoM id
  CHECK(call(e, e.st1, "POST", "/v1/devices", register_body2("sup-a", random_pub(), "SN-SUP-A", "som-a")).status == 201);
  CHECK(call(e, e.admin, "POST", "/v1/devices/sup-a/revoke").status == 200);
  r = call(e, e.st1, "POST", "/v1/devices", register_body2("sup-x", random_pub(), "SN-UNRELATED", "som-unrelated", "sup-a"));
  CHECK(r.status == 409 && r.code() == "invalid_replaces");
  r = call(e, e.st1, "POST", "/v1/devices", register_body2("sup-b", random_pub(), "SN-SUP-B", "som-a", "sup-a"));  // same SoM, new serial
  CHECK(r.status == 201 && r.json().find("replaces")->string == "sup-a");
  CHECK(call(e, e.admin, "POST", "/v1/devices/sup-b/revoke").status == 200);
  r = call(e, e.st1, "POST", "/v1/devices", register_body2("sup-c", random_pub(), "sn-sup-b", "som-new", "sup-b"));  // same serial in another case, new SoM
  CHECK(r.status == 201);
  // the mapping of a SQLite unique-constraint message to the right duplicate code
  CHECK(map_unique_violation("UNIQUE constraint failed: devices.device_id") == DbStatus::kDuplicateDeviceId);
  CHECK(map_unique_violation("UNIQUE constraint failed: index 'devices_active_serial'") == DbStatus::kDuplicateSerial);
  CHECK(map_unique_violation("UNIQUE constraint failed: devices.device_public_key") == DbStatus::kDuplicatePublicKey);
  CHECK(map_unique_violation("UNIQUE constraint failed: devices.replaces") == DbStatus::kInvalidReplaces);
  CHECK(map_unique_violation("disk I/O error") == DbStatus::kError);
}

void test_authorisation_config(Env& e) {
  // --station-libraries: a station sees only the libraries listed for it
  const fs::path f = e.dir / "station-libs.txt";
  {
    std::ofstream o(f);
    o << "# station library allow-list\nstation-01 " << e.libs[0].id << "  # one library\nstation-02 *\n";
  }
  std::map<std::string, std::set<std::string>> allow;
  std::string err;
  CHECK(load_station_libraries(f.string(), allow, &err) && allow.size() == 2 && allow["station-02"].count("*") && allow["station-01"].count(e.libs[0].id));
  for (const char* bad : {"station-01\n", "station-01 not-a-uuid\n", "bad!id *\n", "station-01 *\nstation-01 *\n"}) {
    std::ofstream(e.dir / "bad-libs.txt") << bad;
    std::map<std::string, std::set<std::string>> m;
    CHECK(!load_station_libraries((e.dir / "bad-libs.txt").string(), m, &err));
  }
  Service restricted(*e.db, *e.vault, RateLimitConfig{}, capture_log);
  restricted.set_station_libraries(allow);
  const Principal s1{Role::kStation, "station-01", "127.0.0.1"}, s2{Role::kStation, "station-02", "127.0.0.1"}, s3{Role::kStation, "station-03", "127.0.0.1"};
  DevKeys d;
  CHECK(direct(restricted, s1, "POST", "/v1/devices", register_body("libdev-1", d.pub, "SN-LIBDEV-1")).status == 201);
  auto ask = [&](const Principal& who, const std::string& lib) {
    return direct(restricted, who, "POST", "/v1/devices/libdev-1/wrapped-keys", "{\"library_ids\":[\"" + lib + "\"]}");
  };
  CHECK(ask(s1, e.libs[0].id).status == 200);
  HttpResponse r = ask(s1, e.libs[1].id);
  CHECK(r.status == 403 && contains(r.body, "library_not_allowed") && contains(r.body, e.libs[1].id));
  r = direct(restricted, s1, "POST", "/v1/devices/libdev-1/wrapped-keys", "{\"library_ids\":[\"" + e.libs[0].id + "\",\"" + e.libs[1].id + "\"]}");
  CHECK(r.status == 403 && !contains(r.body, "wrapped_key\""));  // all or nothing
  CHECK(ask(s2, e.libs[1].id).status == 200 && ask(s2, e.libs[2].id).status == 200);
  CHECK(ask(s3, e.libs[0].id).status == 403);  // a file is configured and station-03 is not in it
  CHECK(count_rows(e, "POST /v1/devices/{id}/wrapped-keys", "library_not_allowed", "refused") == 3);
  // without a file every station may ask for every library (the default)
  CHECK(direct(*e.service, s3, "POST", "/v1/devices/libdev-1/wrapped-keys", "{\"library_ids\":[\"" + e.libs[1].id + "\"]}").status == 200);

  // registration cap per station
  Service capped(*e.db, *e.vault, RateLimitConfig{}, capture_log);
  capped.set_registration_cap(2);
  const Principal busy{Role::kStation, "station-busy", "127.0.0.1"};
  int created = 0, limited = 0;
  for (int i = 0; i < 5; ++i) {
    const HttpResponse x = direct(capped, busy, "POST", "/v1/devices", register_body("capdev-" + std::to_string(i), random_pub(), "SN-CAPDEV-" + std::to_string(i)));
    if (x.status == 201) ++created;
    if (x.status == 429 && contains(x.body, "registration_rate_limited")) ++limited;
  }
  CHECK(created == 2 && limited == 3);
  CHECK(direct(capped, s2, "POST", "/v1/devices", register_body("capdev-other", random_pub(), "SN-CAPDEV-OTHER")).status == 201);  // another station is unaffected
}

void test_issue_revoke_race(Env& e) {
  // The device is revoked AFTER the issuance looked it up and wrapped the key, but BEFORE the issuance was recorded: the revoke wins, no key leaves, nothing is recorded.
  for (int i = 0; i < 20; ++i) {
    const std::string id = "toctou-" + std::to_string(i);
    DevKeys d;
    CHECK(direct(*e.service, kStationPrincipal, "POST", "/v1/devices", register_body(id, d.pub, "SN-TOCTOU-" + std::to_string(i))).status == 201);
    std::atomic<bool> at_hook{false}, revoked{false};
    e.service->before_issue_hook = [&] {
      at_hook = true;
      while (!revoked) std::this_thread::yield();
    };
    HttpResponse resp;
    std::thread issuer([&] { resp = direct(*e.service, kStationPrincipal, "POST", "/v1/devices/" + id + "/wrapped-keys", "{\"library_ids\":[\"" + e.libs[0].id + "\"]}"); });
    while (!at_hook) std::this_thread::yield();
    Device out;
    CHECK(e.db->revoke_device(id, "admin:admin-01", "race", out) == DbStatus::kOk);
    revoked = true;
    issuer.join();
    e.service->before_issue_hook = nullptr;
    CHECK(resp.status == 403 && contains(resp.body, "device_revoked") && !contains(resp.body, "wrapped_key\""));
    int issued = 0;
    for (const AuditRow& a : rows_of(e))
      if (a.action == "wrapped_key_issued" && a.device_id == id) ++issued;
    CHECK(issued == 0);
  }
  // free running: whichever wins, "key returned" and "issuance recorded" are the same fact
  for (int i = 0; i < 20; ++i) {
    const std::string id = "freerace-" + std::to_string(i);
    CHECK(direct(*e.service, kStationPrincipal, "POST", "/v1/devices", register_body(id, random_pub(), "SN-FREERACE-" + std::to_string(i))).status == 201);
    std::atomic<int> go{0};
    HttpResponse resp;
    std::thread issuer([&] {
      go.fetch_add(1);
      while (go < 2) std::this_thread::yield();
      resp = direct(*e.service, kStationPrincipal, "POST", "/v1/devices/" + id + "/wrapped-keys", "{\"library_ids\":[\"" + e.libs[1].id + "\"]}");
    });
    std::thread revoker([&] {
      go.fetch_add(1);
      while (go < 2) std::this_thread::yield();
      Device out;
      e.db->revoke_device(id, "admin:admin-01", "race", out);
    });
    issuer.join();
    revoker.join();
    int issued = 0;
    for (const AuditRow& a : rows_of(e))
      if (a.action == "wrapped_key_issued" && a.device_id == id) ++issued;
    CHECK((resp.status == 200 && issued == 1) || (resp.status == 403 && issued == 0));
    CHECK(e.db->find_device(id)->status == "revoked");
  }
}

sqlite3* raw_open(const std::string& path) {
  sqlite3* db = nullptr;
  sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr);
  return db;
}
bool raw_exec(sqlite3* db, const char* sql, std::string* err = nullptr) {
  char* m = nullptr;
  const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &m);
  if (err) *err = m ? m : "";
  sqlite3_free(m);
  return rc == SQLITE_OK;
}

void test_audit_atomicity(Env& e) {
  // Break the audit log's consistency under the server's feet (the AUTOINCREMENT counter no longer matches the last row): every write must refuse, and a device write
  // and its audit row are one unit, so no device row appears and no key leaves.
  DevKeys d;
  CHECK(call(e, e.st1, "POST", "/v1/devices", register_body("atomic-ok", d.pub, "SN-ATOMIC-OK")).status == 201);
  sqlite3* raw = raw_open(e.db_path());
  CHECK(raw_exec(raw, "UPDATE sqlite_sequence SET seq = seq + 5 WHERE name = 'audit'"));
  CHECK(!e.db->verify_audit().ok);
  Reply r = call(e, e.st1, "POST", "/v1/devices", register_body("atomic-new", random_pub(), "SN-ATOMIC-NEW"));
  CHECK(r.status == 500 && r.code() == "audit_failed");
  CHECK(!e.db->find_device("atomic-new").has_value());  // no device row without its audit row
  r = call(e, e.st1, "POST", "/v1/devices/atomic-ok/wrapped-keys", "{\"library_ids\":[\"" + e.libs[0].id + "\"]}");
  CHECK(r.status == 500 && r.code() == "audit_failed" && !contains(r.body, "wrapped_key\""));
  r = call(e, e.admin, "POST", "/v1/devices/atomic-ok/revoke");
  CHECK(r.status == 500 && r.code() == "audit_failed");
  CHECK(e.db->find_device("atomic-ok")->status == "active");  // not revoked without its audit row
  CHECK(!e.db->audit(AuditEvent{"system", "x", "", "", "ok", "", ""}));
  // repaired: everything works again and the chain verifies
  CHECK(raw_exec(raw, "UPDATE sqlite_sequence SET seq = seq - 5 WHERE name = 'audit'"));
  sqlite3_close(raw);
  CHECK(e.db->verify_audit().ok);
  CHECK(call(e, e.st1, "POST", "/v1/devices/atomic-ok/wrapped-keys", "{\"library_ids\":[\"" + e.libs[0].id + "\"]}").status == 200);
  CHECK(call(e, e.st1, "POST", "/v1/devices", register_body("atomic-new", random_pub(), "SN-ATOMIC-NEW")).status == 201);
}

void test_append_only_attacks(Env& e) {
  sqlite3* raw = raw_open(e.db_path());
  std::string msg;
  const std::int64_t before = e.db->verify_audit().entries;
  const char* kInsert = "INSERT INTO audit (seq, ts, actor, action, device_id, library_id, outcome, code, detail, prev_hash, hash) VALUES (%s, 0, 'x', 'x', '', '', 'ok', '', '', zeroblob(32), zeroblob(32))";
  char sql[512];
  // REPLACE INTO an existing sequence number (REPLACE does not fire DELETE triggers), an out-of-order insert, a gap
  std::snprintf(sql, sizeof sql, "REPLACE INTO audit (seq, ts, actor, action, device_id, library_id, outcome, code, detail, prev_hash, hash) VALUES (1, 0, 'x', 'x', '', '', 'ok', '', '', zeroblob(32), zeroblob(32))");
  CHECK(!raw_exec(raw, sql, &msg) && contains(msg, "append-only"));
  std::snprintf(sql, sizeof sql, kInsert, "3");
  CHECK(!raw_exec(raw, sql, &msg) && contains(msg, "append-only"));
  std::snprintf(sql, sizeof sql, kInsert, std::to_string(before + 7).c_str());
  CHECK(!raw_exec(raw, sql, &msg) && contains(msg, "append-only"));
  CHECK(!raw_exec(raw, "UPDATE audit_head SET seq = 0", &msg) || !e.db->verify_audit().ok);   // the head row may be edited (it is just a row) but then the chain no longer verifies ...
  raw_exec(raw, "UPDATE audit_head SET seq = (SELECT MAX(seq) FROM audit), hash = (SELECT hash FROM audit ORDER BY seq DESC LIMIT 1)");  // ... until it is put back
  CHECK(!raw_exec(raw, "DELETE FROM audit_head", &msg) && contains(msg, "append-only"));
  sqlite3_close(raw);
  CHECK(e.db->verify_audit().ok && e.db->verify_audit().entries == before);

  // schema attacks on copies of the database: each is DETECTED by verify-audit and refused by a writer that opens the copy
  auto attacked = [&](const char* name, const char* sql) {
    const fs::path copy = e.dir / name;
    fs::copy_file(e.db_path(), copy, fs::copy_options::overwrite_existing);
    sqlite3* t = raw_open(copy.string());
    std::string em;
    const bool ok = raw_exec(t, sql, &em);
    if (!ok) std::printf("  attack '%s' failed to apply: %s\n", name, em.c_str());
    sqlite3_close(t);
    return copy;
  };
  {
    const fs::path c = attacked("a-drop.db", "DROP TABLE audit");
    const AuditVerify v = Database::verify_audit_file(c.string());
    CHECK(!v.ok);
    std::string err;
    auto w = Database::open(c.string(), &err);  // re-creates an empty audit table ...
    CHECK(w && !w->verify_audit().ok && !w->audit(AuditEvent{"system", "x", "", "", "ok", "", ""}));  // ... which does not match the stored head: the writer refuses
  }
  CHECK(!Database::verify_audit_file(attacked("a-rename.db", "ALTER TABLE audit RENAME TO audit_old").string()).ok);
  CHECK(!Database::verify_audit_file(attacked("a-addcol.db", "ALTER TABLE audit ADD COLUMN extra TEXT").string()).ok);
  {
    // PRAGMA writable_schema: this SQLite build refuses it ("may not be modified"); where it is allowed the edited trigger text is caught
    const fs::path copy = e.dir / "a-schema.db";
    fs::copy_file(e.db_path(), copy, fs::copy_options::overwrite_existing);
    sqlite3* t = raw_open(copy.string());
    std::string em;
    const bool applied = raw_exec(t, "PRAGMA writable_schema = ON; UPDATE sqlite_master SET sql = replace(sql, 'ABORT', 'IGNORE') WHERE name = 'audit_no_update'", &em);
    sqlite3_close(t);
    if (applied) {
      const AuditVerify v = Database::verify_audit_file(copy.string());
      CHECK(!v.ok);  // either the altered text is caught, or SQLite itself calls the schema malformed (RAISE(IGNORE, 'text') does not parse)
    } else {
      CHECK(contains(em, "sqlite_master"));
    }
  }
  {
    // a trigger with the right NAME and a different body is caught by comparing the SQL text
    const AuditVerify v = Database::verify_audit_file(attacked("a-trigger.db", "DROP TRIGGER audit_no_delete; CREATE TRIGGER audit_no_delete BEFORE DELETE ON audit BEGIN SELECT 1; END;").string());
    CHECK(!v.ok && contains(v.message, "altered"));
  }
  {
    const AuditVerify v = Database::verify_audit_file(attacked("a-seq.db", "DELETE FROM sqlite_sequence").string());
    CHECK(!v.ok && contains(v.message, "truncated"));
  }
  CHECK(!Database::verify_audit_file(attacked("a-head.db", "DROP TABLE audit_head").string()).ok);
  for (const char* n : {"a-drop.db", "a-rename.db", "a-addcol.db", "a-schema.db", "a-trigger.db", "a-seq.db", "a-head.db"}) fs::remove(e.dir / n);
}

void test_two_process_race(const fs::path& base) {
  // two PROCESSES, each with its own SQLite connection, register the same serial at the same instant: exactly one wins; 20 repetitions
  int won_total = 0, lost_total = 0;
  for (int rep = 0; rep < 20; ++rep) {
    const std::string path = (base / ("race" + std::to_string(rep) + ".db")).string();
    std::string err;
    { auto init = Database::open(path, &err); CHECK(init != nullptr); }
    int ready[2], go[2];
    CHECK(::pipe(ready) == 0 && ::pipe(go) == 0);
    pid_t pids[2];
    std::fflush(nullptr);
    for (int c = 0; c < 2; ++c) {
      pids[c] = ::fork();
      if (pids[c] == 0) {
        std::string cerr_;
        auto db = Database::open(path, &cerr_);
        if (!db) ::_exit(9);
        const char b = 'r';
        (void)!::write(ready[1], &b, 1);
        char g;
        (void)!::read(go[0], &g, 1);
        NewDevice n;
        n.device_id = "pr-" + std::to_string(c);
        n.public_key = random_pub();
        n.som_id = "som";
        n.serial_number = "SN-TWOPROC";
        n.tier = "standard";
        n.provisioning_date = "2026-10-20";
        n.station_id = "st";
        Device out;
        const DbStatus st = db->register_device(n, "station:st", out);
        ::_exit(st == DbStatus::kOk ? 0 : st == DbStatus::kDuplicateSerial ? 3 : 4);
      }
    }
    char buf[2];
    (void)!::read(ready[0], buf, 1);
    (void)!::read(ready[0], buf, 1);
    (void)!::write(go[1], "gg", 2);
    int ok = 0, dup = 0, other = 0;
    for (int c = 0; c < 2; ++c) {
      int st = 0;
      ::waitpid(pids[c], &st, 0);
      const int code = WIFEXITED(st) ? WEXITSTATUS(st) : 99;
      if (code == 0) ++ok;
      else if (code == 3) ++dup;
      else ++other;
    }
    ::close(ready[0]); ::close(ready[1]); ::close(go[0]); ::close(go[1]);
    CHECK(ok == 1 && dup == 1 && other == 0);
    won_total += ok;
    lost_total += dup;
    auto check = Database::open(path, &err);
    CHECK(check && check->verify_audit().ok && check->verify_audit().entries == 1);
  }
  CHECK(won_total == 20 && lost_total == 20);
}

void test_handshake_flood(const fs::path& dir) {
  Env e;
  make_env(e, dir);
  const int limit = 30;
  for (int i = 0; i < limit + 3; ++i) send_raw(e, nullptr, "GET / HTTP/1.1\r\n\r\n");
  CHECK(wait_until([&] { return log_copy().find("exceed the audit limit") != std::string::npos; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(static_cast<int>(handshake_rows(e).size()) == limit);  // the 31st-33rd are not audited ...
  const std::string log = log_copy();
  CHECK(log.find("exceed the audit limit") != std::string::npos);  // ... but the suppression is logged, once
  CHECK(log.find("exceed the audit limit") == log.rfind("exceed the audit limit"));
  CHECK(call(e, e.st1, "GET", "/v1/devices/none").status == 404);  // a good client is not affected
  CHECK(e.db->verify_audit().ok);
  e.server->stop();
}

int tcp_connect(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}
bool peer_closed(int fd) {
  char c;
  const ssize_t n = ::recv(fd, &c, 1, MSG_DONTWAIT | MSG_PEEK);
  return n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK);
}

void test_connection_limits(const fs::path& dir) {
  Env e;
  EnvOptions opt;
  opt.io_timeout_ms = 600;   // per-step 600 ms, whole connection 1200 ms
  opt.conn_cap = 16;         // the production default
  make_env(e, dir, opt);
  // 1. at most 16 concurrent connections per peer address: the rest are closed at once
  std::vector<int> fds;
  for (int i = 0; i < 24; ++i) fds.push_back(tcp_connect(e.server->port()));
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  int closed_early = 0;
  for (int fd : fds)
    if (fd >= 0 && peer_closed(fd)) ++closed_early;
  CHECK(closed_early >= 6 && closed_early <= 10);  // 24 - 16 = 8 refused (the others are still inside their handshake deadline)
  // 2. a stalled handshake is cut at the absolute deadline: every one of them is closed shortly after 1.2 s
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  int still_open = 0;
  for (int fd : fds)
    if (fd >= 0 && !peer_closed(fd)) ++still_open;
  CHECK(still_open == 0);
  for (int fd : fds)
    if (fd >= 0) ::close(fd);
  CHECK(wait_until([&] { return log_copy().find("handshake timed out") != std::string::npos; }));
  // 3. a client that dribbles one byte every 150 ms is cut at the absolute deadline, not served for as long as it keeps dribbling
  {
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_load_verify_locations(ctx, (dir / "root.crt").c_str(), nullptr);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    SSL_CTX_use_certificate(ctx, e.st1.cert.get());
    SSL_CTX_use_PrivateKey(ctx, e.st1.key.get());
    const int fd = tcp_connect(e.server->port());
    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), "127.0.0.1");
    CHECK(SSL_connect(ssl) == 1);
    const std::string req = "POST /v1/devices HTTP/1.1\r\nContent-Length: 500\r\nX-Pad: " + std::string(100, 'a') + "\r\n\r\n";
    const auto t0 = std::chrono::steady_clock::now();
    std::string in;
    for (std::size_t i = 0; i < req.size() && in.empty(); ++i) {
      SSL_write(ssl, &req[i], 1);
      pollfd p{fd, POLLIN, 0};
      if (::poll(&p, 1, 150) > 0) {
        char buf[1024];
        const int n = SSL_read(ssl, buf, sizeof buf);
        if (n > 0) in.append(buf, static_cast<std::size_t>(n));
        else break;
      }
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(in.rfind("HTTP/1.1 408", 0) == 0);
    CHECK(ms < 2500);  // the dribble would have lasted 100 * 150 ms = 15 s
    SSL_free(ssl);
    ::close(fd);
    SSL_CTX_free(ctx);
    ERR_clear_error();
  }
  CHECK(call(e, e.st1, "GET", "/v1/devices/none").status == 404);
  e.server->stop();
}

// The format-level backup matrix.
void test_backup_format_matrix(Env& e) {
  Bytes plain(kBackupChunkSize * 2 + 100);
  std::mt19937_64 rng(77);
  for (auto& b : plain) b = static_cast<std::uint8_t>(rng());
  const Bytes enc = encrypt_backup(plain, e.backup_key, 1800000000);
  Bytes out;
  std::string err;
  CHECK(decrypt_backup(enc, e.backup_key, out, &err) && out == plain);
  // every one of the 64 header bytes is covered
  int detected = 0;
  for (std::size_t i = 0; i < kBackupHeaderSize; ++i) {
    Bytes m = enc;
    m[i] ^= 0x01;
    if (!decrypt_backup(m, e.backup_key, out, &err)) ++detected;
    else std::printf("  header byte %zu flip not detected\n", i);
  }
  CHECK(detected == static_cast<int>(kBackupHeaderSize));
  // one byte in every chunk and in every tag
  const std::size_t full = kBackupChunkSize + 16;
  const std::size_t chunk_starts[3] = {kBackupHeaderSize, kBackupHeaderSize + full, kBackupHeaderSize + 2 * full};
  const std::size_t chunk_sizes[3] = {kBackupChunkSize, kBackupChunkSize, 100};
  for (int c = 0; c < 3; ++c) {
    Bytes m = enc;
    m[chunk_starts[c] + chunk_sizes[c] / 2] ^= 0x80;
    CHECK(!decrypt_backup(m, e.backup_key, out, &err));
    m = enc;
    m[chunk_starts[c] + chunk_sizes[c] + 5] ^= 0x01;  // inside the tag
    CHECK(!decrypt_backup(m, e.backup_key, out, &err));
    m = enc;
    m[chunk_starts[c] + chunk_sizes[c] + 15] ^= 0x01;  // last tag byte
    CHECK(!decrypt_backup(m, e.backup_key, out, &err));
  }
  // truncation: empty, shorter than the header, the header alone, at a chunk boundary (length unpatched), and with a PATCHED length
  CHECK(!decrypt_backup(Bytes{}, e.backup_key, out, &err));
  CHECK(!decrypt_backup(Bytes(enc.begin(), enc.begin() + 10), e.backup_key, out, &err));
  CHECK(!decrypt_backup(Bytes(enc.begin(), enc.begin() + 63), e.backup_key, out, &err));
  CHECK(!decrypt_backup(Bytes(enc.begin(), enc.begin() + 64), e.backup_key, out, &err));
  CHECK(!decrypt_backup(Bytes(enc.begin(), enc.begin() + chunk_starts[1]), e.backup_key, out, &err) && contains(err, "wrong size"));
  CHECK(!decrypt_backup(Bytes(enc.begin(), enc.begin() + chunk_starts[2]), e.backup_key, out, &err));
  auto patched = [&](std::uint64_t len, std::size_t keep) {
    Bytes m(enc.begin(), enc.begin() + keep);
    for (int i = 0; i < 8; ++i) m[48 + i] = static_cast<std::uint8_t>((len >> (8 * (7 - i))) & 0xff);
    return m;
  };
  CHECK(!decrypt_backup(patched(kBackupChunkSize, chunk_starts[1]), e.backup_key, out, &err));                         // patched, at a chunk boundary
  CHECK(!decrypt_backup(patched(kBackupChunkSize * 2, chunk_starts[2]), e.backup_key, out, &err));                     // patched, two full chunks
  CHECK(!decrypt_backup(patched(kBackupChunkSize + 7, chunk_starts[1] + 7 + 16), e.backup_key, out, &err));            // patched, NOT at a boundary
  CHECK(!decrypt_backup(patched(0, kBackupHeaderSize + 16), e.backup_key, out, &err));                                  // "empty database"
  // reordered chunks
  {
    Bytes m = enc;
    std::swap_ranges(m.begin() + chunk_starts[0], m.begin() + chunk_starts[1], m.begin() + chunk_starts[1]);
    CHECK(!decrypt_backup(m, e.backup_key, out, &err));
  }
  // wrong key: refused with the authentication error, and the output buffer is empty
  Key32 wrong = e.backup_key;
  wrong[31] ^= 1;
  CHECK(!decrypt_backup(enc, wrong, out, &err) && contains(err, "authentication failed") && out.empty());
}

void test_backup_locations_and_restore_safety(Env& e) {
  std::string err;
  BackupResult b;
  const fs::path sub = e.dir / "sub";
  fs::create_directories(sub);
  fs::create_symlink(e.dir, e.dir.parent_path() / ("link-to-db-dir." + std::to_string(::getpid())));
  const fs::path link = e.dir.parent_path() / ("link-to-db-dir." + std::to_string(::getpid()));
  // not a second location: inside the database's directory, through a symlink, through `..`, or the directory itself: refused, and nothing is created
  CHECK(!backup_database(e.db_path(), e.backup_key, (e.dir / "inside" / "deeper").string(), 1800000100, b, &err) && contains(err, "second location"));
  CHECK(!fs::exists(e.dir / "inside"));
  CHECK(!backup_database(e.db_path(), e.backup_key, link.string(), 1800000100, b, &err) && contains(err, "second location"));
  CHECK(!backup_database(e.db_path(), e.backup_key, (sub / "..").string(), 1800000100, b, &err) && contains(err, "second location"));
  CHECK(!backup_database(e.db_path(), e.backup_key, (e.dir / "sub" / ".." / "inside2").string(), 1800000100, b, &err) && contains(err, "second location"));
  CHECK(!fs::exists(e.dir / "inside2"));
  fs::remove(link);
  // an existing destination keeps its permissions; a new one is created 0700
  const fs::path existing = e.dir.parent_path() / ("existing-dest." + std::to_string(::getpid()));
  fs::create_directories(existing);
  ::chmod(existing.c_str(), 0755);
  CHECK(backup_database(e.db_path(), e.backup_key, existing.string(), 1800000200, b, &err));
  const std::string src = b.path;
  struct stat st{};
  CHECK(::stat(existing.c_str(), &st) == 0 && (st.st_mode & 0777) == 0755);
  const fs::path fresh_dest = e.dir.parent_path() / ("fresh-dest." + std::to_string(::getpid()));
  CHECK(backup_database(e.db_path(), e.backup_key, fresh_dest.string(), 1800000300, b, &err));
  CHECK(::stat(fresh_dest.c_str(), &st) == 0 && (st.st_mode & 0777) == 0700);
  fs::remove_all(fresh_dest);

  // --- restore safety, with --force over a live-looking target
  const fs::path target = e.dir / "safety.db";
  fs::copy_file(e.db_path(), target, fs::copy_options::overwrite_existing);
  const auto hash_of = [&](const fs::path& p) { return fmt::Sha256::hash(read_all(p)); };
  const auto before = hash_of(target);
  BackupResult rr;
  auto no_leftovers = [&] {
    int n = 0;
    for (const auto& de : fs::directory_iterator(e.dir))
      if (de.path().filename().string().find("safety.db.restore") == 0) ++n;
    return n == 0;
  };
  // (a) decrypts to garbage that is not a database
  {
    const Bytes junk(5000, 'x');
    const Bytes enc = encrypt_backup(junk, e.backup_key, 1);
    put_file(e.dir / "junk.vrb", enc.data(), enc.size(), 0600);
    CHECK(!restore_backup((e.dir / "junk.vrb").string(), e.backup_key, target.string(), true, rr, &err));
    CHECK(hash_of(target) == before && no_leftovers());
  }
  // (b) a real database whose audit chain was tampered with
  {
    const fs::path bad = e.dir / "bad-chain.db";
    fs::copy_file(e.db_path(), bad, fs::copy_options::overwrite_existing);
    sqlite3* t = raw_open(bad.string());
    raw_exec(t, "DROP TRIGGER audit_no_update; UPDATE audit SET detail = 'tampered' WHERE seq = 2");
    sqlite3_close(t);
    const Bytes enc = encrypt_backup(read_all(bad), e.backup_key, 1);
    put_file(e.dir / "bad-chain.vrb", enc.data(), enc.size(), 0600);
    CHECK(!restore_backup((e.dir / "bad-chain.vrb").string(), e.backup_key, target.string(), true, rr, &err) && contains(err, "audit chain"));
    CHECK(hash_of(target) == before && no_leftovers());
    fs::remove(bad);
  }
  // (c) wrong key
  {
    Key32 wrong = e.backup_key;
    wrong[3] ^= 0x10;
    CHECK(!restore_backup(src, wrong, target.string(), true, rr, &err));
    CHECK(hash_of(target) == before && no_leftovers());
  }
  // (d) a stale journal next to the target
  put_file(target.string() + "-journal", "x", 1, 0600);
  CHECK(!restore_backup(src, e.backup_key, target.string(), true, rr, &err) && contains(err, "-journal"));
  fs::remove(target.string() + "-journal");
  CHECK(hash_of(target) == before);
  // (e) the target is open in a running registry (flock held) ...
  {
    std::string derr;
    auto holder = Database::open(target.string(), &derr, /*exclusive_lock=*/true);
    CHECK(holder != nullptr);
    CHECK(!restore_backup(src, e.backup_key, target.string(), true, rr, &err) && contains(err, "in use"));
    CHECK(Database::open(target.string(), &derr, true) == nullptr && contains(derr, "in use"));  // and a second server on it is refused
    CHECK(hash_of(target) == before && no_leftovers());
  }
  // ... or has an open write transaction in another process (SQLite lock, no flock)
  {
    sqlite3* locker = raw_open(target.string());
    CHECK(raw_exec(locker, "BEGIN EXCLUSIVE"));
    CHECK(!restore_backup(src, e.backup_key, target.string(), true, rr, &err) && contains(err, "locked"));
    raw_exec(locker, "ROLLBACK");
    sqlite3_close(locker);
    CHECK(hash_of(target) == before && no_leftovers());
  }
  // (f) the good restore over it
  CHECK(restore_backup(src, e.backup_key, target.string(), true, rr, &err));
  CHECK(Database::verify_audit_file(target.string()).ok && hash_of(target) != before && no_leftovers());
  // no --force: an existing file is never replaced (link() fails), a new path works
  CHECK(!restore_backup(src, e.backup_key, target.string(), false, rr, &err) && contains(err, "already exists"));
  fs::remove(target);
  CHECK(restore_backup(src, e.backup_key, target.string(), false, rr, &err));
  fs::remove(target);

  // --- anchors: the audit head of a snapshot, MACed under the backup key
  const fs::path anchors = e.dir.parent_path() / ("anchors." + std::to_string(::getpid()));
  BackupResult b1, b2;
  CHECK(backup_database(e.db_path(), e.backup_key, anchors.string(), 1800001000, b1, &err));
  e.db->audit(AuditEvent{"system", "between_backups", "", "", "ok", "", ""});
  CHECK(backup_database(e.db_path(), e.backup_key, anchors.string(), 1800002000, b2, &err));
  Anchor an;
  CHECK(read_latest_anchor(anchors.string(), &e.backup_key, an, &err) == 1 && an.seq == b2.head_seq && an.hash_hex == b2.head_hash_hex);
  CHECK(b2.head_seq == b1.head_seq + 1);
  Key32 other_key = e.backup_key;
  other_key[0] ^= 1;
  CHECK(read_latest_anchor(anchors.string(), &other_key, an, &err) < 0 && contains(err, "MAC"));
  // restoring the OLDER backup with --anchor-dir succeeds but warns that it is behind the newest anchor
  CHECK(restore_backup(b1.path, e.backup_key, target.string(), false, rr, &err, anchors.string()) && contains(rr.notes, "older than the newest anchor"));
  fs::remove(target);
  CHECK(restore_backup(b2.path, e.backup_key, target.string(), false, rr, &err, anchors.string()) && !contains(rr.notes, "WARNING") && contains(rr.notes, "matches its anchor"));
  fs::remove(target);
  // a forged anchor (hash edited) fails its MAC
  {
    const fs::path anchor_path = fs::path(b2.path).replace_extension(".anchor");
    const Bytes raw_anchor = read_all(anchor_path);
    std::string txt(raw_anchor.begin(), raw_anchor.end());
    const std::size_t h = txt.find("hash=") + 5;
    txt[h] = txt[h] == '0' ? '1' : '0';
    put_file(anchor_path, txt.data(), txt.size(), 0600);
    CHECK(!restore_backup(b2.path, e.backup_key, target.string(), false, rr, &err) && contains(err, "MAC"));
    CHECK(!fs::exists(target));
  }
  // the registry compares itself with the newest anchor: rolled back / rewritten databases are refused (CLI exit 7)
  {
    fs::remove(fs::path(b2.path).replace_extension(".anchor"));
    BackupResult b3;
    e.db->audit(AuditEvent{"system", "after_anchor_2", "", "", "ok", "", ""});
    e.db->audit(AuditEvent{"system", "after_anchor_3", "", "", "ok", "", ""});
    CHECK(backup_database(e.db_path(), e.backup_key, anchors.string(), 1800003000, b3, &err));
    const fs::path rolled_back = e.dir / "rolled-back.db";
    BackupResult rr2;
    CHECK(restore_backup(b1.path, e.backup_key, rolled_back.string(), false, rr2, &err));  // an older state of the same database
    const std::string base = std::string(REGISTRY_EXE) + " verify-audit --anchor-dir '" + anchors.string() + "' --backup-key '" + (e.dir / "backup.key").string() + "' --db '";
    std::string out;
    CHECK(run_cmd(base + e.db_path() + "'", &out) == 0);
    CHECK(run_cmd(base + rolled_back.string() + "'", &out) == 7 && contains(out, "rolled back or truncated"));
    // a different history with at least as many entries: the hash at the anchored sequence number differs
    const fs::path forged = e.dir / "forged.db";
    {
      auto f = Database::open(forged.string(), &err);
      while (f->head().seq < b3.head_seq) f->audit(AuditEvent{"station:mallory", "forged", "", "", "ok", "", "rewritten history"});
    }
    CHECK(run_cmd(base + forged.string() + "'", &out) == 7 && contains(out, "rewritten"));
    fs::remove(rolled_back);
    fs::remove(forged);
  }
  fs::remove_all(anchors);
  fs::remove_all(existing);
  fs::remove(e.dir / "junk.vrb");
  fs::remove(e.dir / "bad-chain.vrb");
  fs::remove(target);
}

// A restored database serves: keys are issued from it and unwrap, and the constraints of the original still hold.
void test_restore_then_serve(Env& e) {
  const fs::path second = e.dir.parent_path() / ("serve-backup." + std::to_string(::getpid()));
  BackupResult b, rr;
  std::string err;
  CHECK(backup_database(e.db_path(), e.backup_key, second.string(), 1800004000, b, &err));
  const std::string restored = (e.dir / "served.db").string();
  CHECK(restore_backup(b.path, e.backup_key, restored, true, rr, &err));
  e.server->stop();
  e.server.reset();
  e.service.reset();
  e.db = Database::open(restored, &err, true);
  CHECK(e.db != nullptr);
  RateLimitConfig rl;
  rl.max_requests = 100000;
  e.service = std::make_unique<Service>(*e.db, *e.vault, rl, capture_log);
  ServerConfig cfg;
  cfg.cert_file = (e.dir / "server.crt").string();
  cfg.key_file = (e.dir / "server.key").string();
  cfg.client_ca_file = (e.dir / "root.crt").string();
  cfg.max_conn_per_ip = 64;
  e.server = std::make_unique<Server>(cfg, *e.service, capture_log);
  CHECK(e.server->start(&err));
  // dev-0001 was registered in the original (SN-0001): it is in the restored database, still active, and gets a key that unwraps ...
  Reply r = call(e, e.st1, "GET", "/v1/devices/dev-0001");
  CHECK(r.status == 200 && r.json().find("status")->string == "active");
  // ... for which we hold no private key here; use a device registered fresh in the restored database instead
  DevKeys d;
  CHECK(call(e, e.st1, "POST", "/v1/devices", register_body("restored-dev", d.pub, "SN-RESTORED")).status == 201);
  r = call(e, e.st1, "POST", "/v1/devices/restored-dev/wrapped-keys", "{\"library_ids\":[\"" + e.libs[2].id + "\"]}");
  CHECK(r.status == 200);
  Key32 got{};
  CHECK(cp::wrapped_key_open(d.priv.get(), unhex(r.json().find("keys")->array[0].find("wrapped_key")->string), e.libs[2].id, got) && got == e.libs[2].key);
  // the unique constraints and the revocations survived
  r = call(e, e.st1, "POST", "/v1/devices", register_body("another", random_pub(), "SN-0001"));
  CHECK(r.status == 409 && r.code() == "duplicate_serial");
  r = call(e, e.st1, "POST", "/v1/devices/dev-0100/wrapped-keys", "{\"library_ids\":[\"" + e.libs[0].id + "\"]}");
  CHECK(r.status == 403 && r.code() == "device_revoked");
  CHECK(e.db->verify_audit().ok);
  bool restore_entry = false;
  for (const AuditRow& a : rows_of(e))
    if (a.action == "restore" && contains(a.detail, "head_seq=" + std::to_string(b.head_seq))) restore_entry = true;
  CHECK(restore_entry);
  fs::remove_all(second);
}

void test_blob_layout_and_independent_unwrap(Env& e) {
  DevKeys d;
  CHECK(call(e, e.st1, "POST", "/v1/devices", register_body("unwrap-dev", d.pub, "SN-UNWRAP")).status == 201);
  Bytes blobs[2];
  for (int i = 0; i < 2; ++i) {
    const Reply r = call(e, e.st1, "GET", "/v1/devices/unwrap-dev/wrapped-keys?library_id=" + e.libs[0].id);
    CHECK(r.status == 200);
    blobs[i] = unhex(r.json().find("keys")->array[0].find("wrapped_key")->string);
  }
  for (const Bytes& b : blobs) {
    CHECK(b.size() == 105);                                               // total size
    CHECK(std::memcmp(b.data(), "VWK1", 4) == 0 && b[4] == 0x01);        // magic and the version/algorithm byte
    const auto rid = fmt::key_id_of_public(d.pub);
    CHECK(std::memcmp(b.data() + 5, rid.data(), 8) == 0);                 // recipient key id
  }
  CHECK(std::memcmp(blobs[0].data() + 13, blobs[1].data() + 13, 32) != 0);  // a fresh ephemeral public key per issuance
  CHECK(std::memcmp(blobs[0].data() + 45, blobs[1].data() + 45, 12) != 0);  // and a fresh nonce
  CHECK(std::memcmp(blobs[0].data() + 57, blobs[1].data() + 57, 48) != 0);
#ifdef REGISTRY_HAS_DEV_KEYS
  // An independent implementation of the device side: the engine's dev key provider (engine/host/dev_keys), which does not share code with tools/common/vault_crypto.
  velora::host::DevKeyProvider::Options opt;
  std::string err;
  auto provider = velora::host::DevKeyProvider::from_bytes(std::span<const std::uint8_t, 32>(d.priv.get().data(), 32), opt, err);
  CHECK(provider != nullptr);
  if (provider) {
    for (const Bytes& b : blobs) {
      const auto res = provider->unwrap_content_key(e.libs[0].id, b);
      CHECK(res.ok() && std::memcmp(res.key->bytes().data(), e.libs[0].key.data(), 32) == 0);
      CHECK(!provider->unwrap_content_key(e.libs[1].id, b).ok());  // bound to its library
    }
  }
#endif
  // A pinned known-answer vector (tools/keys/KEY-HIERARCHY.md section 6, UUID library id): fixes the format independently of anything the registry produced.
  Key32 dev_priv, want;
  for (int i = 0; i < 32; ++i) {
    dev_priv[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(1 + i);
    want[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(0xc0 + i);
  }
  const Bytes pinned = unhex(
      "56574b3101aaa8fff703b50b225869aff450549732cbaaed5e5df9b30a6da31cb0e5742bad5ad4a1a768f1a67ba0a1a2a3a4a5a6a7a8a9aaabb27952d3c6f1222c0cfd438d34d043034a13df431b860479b07ba77078486cc141bd3487091063b292da3d3b6643ee5d");
  Key32 got{};
  CHECK(pinned.size() == 105 && cp::wrapped_key_open(dev_priv, pinned, "3f1d6a52-9b7e-4c1a-8d2e-5a6b7c8d9e0f", got) && got == want);
}

void test_cli_codes(Env& e) {
  const std::string base = std::string(REGISTRY_EXE);
  const std::string db_copy = (e.dir / "cli-codes.db").string();
  fs::copy_file(e.db_path(), db_copy, fs::copy_options::overwrite_existing);
  const std::string common = " --vault-dir '" + e.vault_dir() + "' --kek '" + (e.dir / "kek.aes256").string() + "' --server-cert '" + (e.dir / "server.crt").string() +
                             "' --server-key '" + (e.dir / "server.key").string() + "' --client-ca '" + (e.dir / "root.crt").string() + "'";
  std::string out;
  // 2 usage
  CHECK(run_cmd(base + " backup --db x --backup-key y --dest-dir z", &out) == 2);  // --kek is required
  // 5 bad config: a non-dev root needs its CA and KEK pinned; pinned values must match
  CHECK(run_cmd(base + " serve --db '" + db_copy + "'" + common + " --root production", &out) == 5 && contains(out, "--expect-ca-sha256"));
  CHECK(run_cmd(base + " serve --db '" + db_copy + "'" + common + " --expect-kek-id 0000000000000000", &out) == 5 && contains(out, "--expect-kek-id"));
  CHECK(run_cmd(base + " serve --db '" + db_copy + "'" + common + " --expect-ca-sha256 " + std::string(64, 'a') + " --port 0 --allow-no-mlock", &out) == 5 &&
        contains(out, "--expect-ca-sha256"));
  {
    std::ofstream(e.dir / "bad-libs2.txt") << "station-01\n";
    CHECK(run_cmd(base + " serve --db '" + db_copy + "'" + common + " --station-libraries '" + (e.dir / "bad-libs2.txt").string() + "'", &out) == 5);
  }
  // 6 database failed
  CHECK(run_cmd(base + " serve --db '/nonexistent-dir/registry.db'" + common, &out) == 6);
  // 7 audit chain failed (startup verification)
  {
    const std::string bad = (e.dir / "cli-bad.db").string();
    fs::copy_file(e.db_path(), bad, fs::copy_options::overwrite_existing);
    sqlite3* t = raw_open(bad);
    raw_exec(t, "DROP TRIGGER audit_no_update; UPDATE audit SET detail = 'x' WHERE seq = 2");
    sqlite3_close(t);
    CHECK(run_cmd(base + " serve --db '" + bad + "'" + common, &out) == 7 && contains(out, "refusing to start"));
    fs::remove(bad);
  }
  // 8 backup failed (destination is the database's own directory)
  CHECK(run_cmd(base + " backup --db '" + db_copy + "' --backup-key '" + (e.dir / "backup.key").string() + "' --kek '" + (e.dir / "kek.aes256").string() + "' --dest-dir '" + e.dir.string() + "'", &out) == 8);
  // 4 the backup key must differ from the KEK
  CHECK(run_cmd(base + " backup --db '" + db_copy + "' --backup-key '" + (e.dir / "kek.aes256").string() + "' --kek '" + (e.dir / "kek.aes256").string() + "' --dest-dir '" + (e.dir.parent_path() / "x-dest").string() + "'", &out) == 4 &&
        contains(out, "same as the KEK"));
  CHECK(!fs::exists(e.dir.parent_path() / "x-dest"));
  // 10 server failed (cannot load the TLS certificate)
  CHECK(run_cmd(base + " serve --db '" + db_copy + "' --vault-dir '" + e.vault_dir() + "' --kek '" + (e.dir / "kek.aes256").string() + "' --server-cert /nonexistent.crt --server-key '" +
                    (e.dir / "server.key").string() + "' --client-ca '" + (e.dir / "root.crt").string() + "'",
                &out) == 10);
  // 9 restore failed
  CHECK(run_cmd(base + " restore --backup /nonexistent.vrb --backup-key '" + (e.dir / "backup.key").string() + "' --db '" + (e.dir / "r.db").string() + "'", &out) == 9);
  fs::remove(db_copy);
}

void test_mlock_policy(Env& e) {
  // The KEK page must be lockable or the server does not start (unless --allow-no-mlock). Provoke the failure with RLIMIT_MEMLOCK = 0 in a child.
  std::fflush(nullptr);
  const pid_t pid = ::fork();
  if (pid == 0) {
    struct rlimit rl{0, 0};
    ::setrlimit(RLIMIT_MEMLOCK, &rl);
    void* page = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (::mlock(page, 4096) == 0) ::_exit(77);  // this platform does not enforce the limit: nothing to test
    std::string err;
    auto strict = KeyVault::create(e.vault_dir(), (e.dir / "kek.aes256").string(), false, &err);
    if (strict || err.find("locked in RAM") == std::string::npos) ::_exit(1);
    auto lax = KeyVault::create(e.vault_dir(), (e.dir / "kek.aes256").string(), true, &err);
    if (!lax || lax->locked()) ::_exit(2);
    ::_exit(0);
  }
  int st = 0;
  ::waitpid(pid, &st, 0);
  const int code = WIFEXITED(st) ? WEXITSTATUS(st) : 99;
  CHECK(code == 0 || code == 77);
  if (code == 77) std::printf("  (mlock limit not enforced on this platform: policy test skipped)\n");
}

void test_service_edge_codes(Env& e) {
  // codes that need a direct hand: vault_unavailable (a symlinked vault entry), key_wrap_failed (a small-order key that got into the database some other way),
  // internal_error (a broken database), unauthenticated (no identity at all)
  DevKeys d;
  CHECK(direct(*e.service, kStationPrincipal, "POST", "/v1/devices", register_body("edge-dev", d.pub, "SN-EDGE")).status == 201);
  const std::string link_id = random_uuid();
  fs::create_directories(e.dir / "vault" / link_id);
  fs::create_symlink(e.dir / "vault" / e.libs[0].id / "content-key.vault", e.dir / "vault" / link_id / "content-key.vault");
  HttpResponse r = direct(*e.service, kStationPrincipal, "POST", "/v1/devices/edge-dev/wrapped-keys", "{\"library_ids\":[\"" + link_id + "\"]}");
  CHECK(r.status == 500 && contains(r.body, "vault_unavailable"));
  NewDevice n;
  n.device_id = "edge-weak";
  n.public_key = Key32{};
  n.public_key[0] = 1;  // the small-order point u = 1: the registration API refuses it, a direct database write does not
  n.som_id = "som";
  n.serial_number = "SN-EDGE-WEAK";
  n.tier = "standard";
  n.provisioning_date = "2026-10-20";
  n.station_id = "station-01";
  Device out;
  CHECK(e.db->register_device(n, "station:station-01", out) == DbStatus::kOk);
  r = direct(*e.service, kStationPrincipal, "POST", "/v1/devices/edge-weak/wrapped-keys", "{\"library_ids\":[\"" + e.libs[0].id + "\"]}");
  CHECK(r.status == 500 && contains(r.body, "key_wrap_failed") && !contains(r.body, "wrapped_key\""));
  bool audited_500 = false;  // 5xx answers are audited too (outcome error)
  for (const AuditRow& a : rows_of(e))
    if (a.code == "key_wrap_failed" && a.outcome == "error" && a.device_id == "edge-weak") audited_500 = true;
  CHECK(audited_500);
  {  // a database whose devices table is gone: typed internal_error, no crash
    const fs::path copy = e.dir / "broken.db";
    fs::copy_file(e.db_path(), copy, fs::copy_options::overwrite_existing);
    std::string err;
    auto broken = Database::open(copy.string(), &err);
    sqlite3* t = raw_open(copy.string());
    raw_exec(t, "DROP INDEX devices_active_device_id; DROP INDEX devices_active_serial; DROP INDEX devices_public_key; DROP INDEX devices_replaces; DROP TABLE devices");
    sqlite3_close(t);
    Service svc(*broken, *e.vault, RateLimitConfig{}, capture_log);
    r = direct(svc, kStationPrincipal, "POST", "/v1/devices", register_body("broken-1", random_pub(), "SN-BROKEN"));
    CHECK(r.status == 500 && contains(r.body, "internal_error"));
    broken.reset();
    fs::remove(copy);
  }
  Principal anon;
  anon.remote = "10.9.9.9";
  r = direct(*e.service, anon, "GET", "/v1/devices/edge-dev");
  CHECK(r.status == 401 && contains(r.body, "unauthenticated"));
  CHECK(direct(*e.service, kStationPrincipal, "GET", "/v1/devices/edge-dev/wrapped-keys?library_id=" + e.libs[0].id).status == 200);
}

// The codes of API.md's error table are all produced by this test run.
void test_api_md_codes_covered() {
  const Bytes raw = read_all(REGISTRY_API_MD);
  const std::string text(raw.begin(), raw.end());
  const std::size_t a = text.find("## Errors"), b = text.find("## `POST /v1/devices`");
  CHECK(a != std::string::npos && b != std::string::npos && a < b);
  std::set<std::string> documented;
  std::size_t pos = a;
  while (pos < b) {
    const std::size_t o = text.find('`', pos);
    if (o == std::string::npos || o >= b) break;
    const std::size_t c = text.find('`', o + 1);
    const std::string tok = text.substr(o + 1, c - o - 1);
    pos = c + 1;
    bool ident = !tok.empty();
    for (const char ch : tok)
      if (!(ch >= 'a' && ch <= 'z') && ch != '_') ident = false;
    if (ident && tok != "field" && tok != "device_id" && tok != "library_id" && tok != "code" && tok != "tls_handshake_failed" && tok != "library_ids") documented.insert(tok);
  }
  CHECK(documented.size() >= 25);
  std::lock_guard<std::mutex> lk(g_secret_mu);
  for (const std::string& code : documented) {
    if (!g_seen_codes.count(code)) std::printf("  API.md error code never produced by the tests: %s\n", code.c_str());
    CHECK(g_seen_codes.count(code) == 1);
  }
  for (const std::string& code : g_seen_codes)
    if (!documented.count(code)) std::printf("  test produced an undocumented error code: %s\n", code.c_str());
}

}  // namespace

#define TIMED(call)                                                                                                                                      \
  do {                                                                                                                                                  \
    const auto t0_ = std::chrono::steady_clock::now();                                                                                                  \
    call;                                                                                                                                               \
    if (std::getenv("REGISTRY_TEST_TIMING"))                                                                                                            \
      std::printf("  %-40s %6lld ms\n", #call, static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0_).count())); \
  } while (0)

int main() {
  ::signal(SIGPIPE, SIG_IGN);
  std::error_code ec;
  const fs::path base = fs::temp_directory_path() / ("velora-registry-test." + std::to_string(::getpid()));
  fs::remove_all(base, ec);
  fs::create_directories(base);
  {
    Env e;
    make_env(e, base / "main");
    TIMED(test_register_and_wrap(e));
    TIMED(test_duplicates_and_validation(e));
    TIMED(test_revoke_and_supersede(e));
    TIMED(test_wrapped_key_errors(e));
    TIMED(test_ids_keys_and_supersede(e));
    TIMED(test_authorisation_config(e));
    TIMED(test_blob_layout_and_independent_unwrap(e));
    TIMED(test_authentication(e));
    TIMED(test_service_edge_codes(e));
    TIMED(test_fuzz(e));
    TIMED(test_concurrency(e));
    TIMED(test_rate_limit_and_limits(e));
    TIMED(test_issue_revoke_race(e));
    TIMED(test_audit_atomicity(e));
    TIMED(test_audit_integrity(e));
    TIMED(test_append_only_attacks(e));
    TIMED(test_backup_restore(e));
    TIMED(test_backup_format_matrix(e));
    TIMED(test_backup_locations_and_restore_safety(e));
    TIMED(test_config_refusals(e));
    TIMED(test_cli_codes(e));
    TIMED(test_mlock_policy(e));
    TIMED(test_restore_then_serve(e));
    TIMED(test_no_plaintext_keys(e));
    e.server->stop();
  }
  TIMED(test_timeout(base / "timeout"));
  TIMED(test_handshake_flood(base / "flood"));
  TIMED(test_connection_limits(base / "limits"));
  TIMED(test_cli_serve(base / "cli"));
  TIMED(test_two_process_race(base));
  TIMED(test_api_md_codes_covered());
  {  // the secrets scan once more over everything every test wrote (the main environment, the server stderr / stdout of the spawned CLI, the race databases)
    const std::vector<std::string> hits = scan_tree(base, log_copy(), kLegitimateSecretFiles);
    for (const std::string& h : hits) std::printf("  plaintext secret found in %s\n", h.c_str());
    CHECK(hits.empty());
  }
  if (g_fail == 0) fs::remove_all(base, ec);
  else std::printf("kept %s\n", base.c_str());
  std::printf(g_fail ? "registry_test: %d FAILED\n" : "registry_test: ok\n", g_fail);
  return g_fail ? 1 : 0;
}
