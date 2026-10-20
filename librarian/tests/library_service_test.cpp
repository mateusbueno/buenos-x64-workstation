// Story 4.11: the librarian service over a real Unix-domain-socket client. Handshake and version policy, every message, pushes to subscribers,
// concurrent clients, malformed and oversize frames, the idle-safe Recent rule (an injected write counter and the SQLite files unchanged across a
// simulated hour of Live Set switches while playing), deletion reconcile, the single-writer rules, and a kill of the service process in the middle
// of a flush. No timing assertions (waits are bounded polls for an event).
#include <grp.h>
#include <sqlite3.h>
#include <sys/wait.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>

#include "lib_test_util.hpp"
#include "velora/librarian/library_service.hpp"

using namespace velora::librarian;
using ltest::Client;
using ltest::TempDir;
using ltest::uid;
namespace fs = std::filesystem;
using Ids = std::vector<std::string>;
using W = ws::v1::LibraryServerMessage;

namespace {

struct Svc {
  TempDir d;
  std::unique_ptr<LibraryService> svc;
  std::string sock;
  explicit Svc(int handshake_timeout_ms = 5000, const std::function<void(LibraryServiceOptions&)>& tweak = nullptr,
               Catalog::FaultHook hook = nullptr, const std::function<void(const std::string&)>& prepare = nullptr) {
    sock = d.sub("s.sock");
    if (prepare) prepare(d.path);  // before the service opens the catalog
    LibraryServiceOptions o;
    o.data_dir = d.path;
    o.socket_path = sock;
    o.handshake_timeout_ms = handshake_timeout_ms;
    if (tweak) tweak(o);
    svc = std::make_unique<LibraryService>(o);
    if (hook) svc->set_catalog_fault_hook(hook);
    std::string err;
    const bool ok = svc->start(err);
    if (!ok) std::printf("start: %s\n", err.c_str());
    CHECK(ok);
  }
  void restart() {
    svc->stop();
    LibraryServiceOptions o;
    o.data_dir = d.path;
    o.socket_path = sock;
    svc = std::make_unique<LibraryService>(o);
    std::string err;
    CHECK(svc->start(err));
  }
  bool connect(Client& c) { return c.connect(sock) && c.handshake(); }
};

bool is_error(const std::optional<W>& r, ws::v1::LibraryErrorCode code) { return r && r->has_error() && r->error().code() == code; }
bool is_ack(const std::optional<W>& r, bool changed) { return r && r->has_ack() && r->ack().changed() == changed; }

// Reads the catalog file with a plain SQLite connection (the service keeps running; WAL readers do not block it).
Ids sql_ids(const std::string& db, const char* sql) {
  Ids out;
  sqlite3* h = nullptr;
  if (sqlite3_open_v2(db.c_str(), &h, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    CHECK(false);
    return out;
  }
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(h, sql, -1, &st, nullptr) == SQLITE_OK)
    while (sqlite3_step(st) == SQLITE_ROW) out.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)));
  sqlite3_finalize(st);
  sqlite3_close(h);
  return out;
}
Ids db_recent(const Svc& s) { return sql_ids(s.d.sub("catalog.db"), "SELECT live_set_id FROM live_set_recent ORDER BY last_played_seq DESC"); }
Ids db_favs(const Svc& s) { return sql_ids(s.d.sub("catalog.db"), "SELECT live_set_id FROM live_set_favorites ORDER BY added_seq"); }

void test_handshake_and_framing() {
  Svc s(/*handshake_timeout_ms=*/300);
  {
    Client c;
    CHECK(c.connect(s.sock));
    ws::v1::LibraryClientMessage m;
    m.mutable_hello()->set_version(1);
    m.mutable_hello()->add_features("whatever.unknown");
    auto r = c.call(m);
    CHECK(r && r->has_hello_ack());
    if (r && r->has_hello_ack()) {
      CHECK(r->hello_ack().version() == 1 && r->hello_ack().catalog_schema_version() == kCatalogSchemaVersion);
      const Ids f = ltest::strs(r->hello_ack().features());
      CHECK(std::find(f.begin(), f.end(), "favorites.v1") != f.end() && std::find(f.begin(), f.end(), "recent.v1") != f.end());
    }
    // A second Hello is not a valid command; the connection stays.
    CHECK(is_error(c.call(m), ws::v1::LIBRARY_ERROR_CODE_INVALID_COMMAND));
    // A frame that is not a message: error, connection stays.
    CHECK(c.send_raw(ltest::frame_of("\xff\xff\xff garbage")));
    auto e = c.recv();
    CHECK(e && e->has_error() && e->error().code() == ws::v1::LIBRARY_ERROR_CODE_MALFORMED_MESSAGE);
    CHECK(c.get().has_value());
    // An empty message has no command.
    ws::v1::LibraryClientMessage empty;
    CHECK(is_error(c.call(empty), ws::v1::LIBRARY_ERROR_CODE_INVALID_COMMAND));
  }
  {  // unsupported version: error with details, then closed
    Client c;
    CHECK(c.connect(s.sock));
    ws::v1::LibraryClientMessage m;
    m.set_request_id(7);
    m.mutable_hello()->set_version(2);
    CHECK(c.send(m));
    auto r = c.recv();
    CHECK(r && r->request_id() == 7 && r->has_error() && r->error().code() == ws::v1::LIBRARY_ERROR_CODE_UNSUPPORTED_VERSION);
    if (r && r->has_error()) CHECK(r->error().details().at("supported") == "1" && r->error().details().at("requested") == "2");
    CHECK(c.at_eof());
  }
  {  // the first message is not Hello
    Client c;
    CHECK(c.connect(s.sock));
    ws::v1::LibraryClientMessage m;
    m.mutable_get_favorites_and_recent();
    CHECK(is_error(c.call(m), ws::v1::LIBRARY_ERROR_CODE_HANDSHAKE_REQUIRED));
    CHECK(c.at_eof());
  }
  {  // an unparsable first frame
    Client c;
    CHECK(c.connect(s.sock));
    CHECK(c.send_raw(ltest::frame_of("\xff\xff\xff garbage")));
    auto e = c.recv();
    CHECK(e && e->has_error() && e->error().code() == ws::v1::LIBRARY_ERROR_CODE_MALFORMED_MESSAGE);
    CHECK(c.at_eof());
  }
  {  // a frame above the limit: error, closed (the stream cannot be resynchronised)
    Client c;
    CHECK(s.connect(c));
    const std::uint32_t n = LibraryService::kMaxFrame + 1;
    const char h[4] = {char(n >> 24), char(n >> 16), char(n >> 8), char(n)};
    CHECK(c.send_raw(std::string(h, 4)));
    auto e = c.recv();
    CHECK(e && e->has_error() && e->error().code() == ws::v1::LIBRARY_ERROR_CODE_FRAME_TOO_LARGE);
    CHECK(c.at_eof());
  }
  {  // a frame exactly at the limit is accepted (and is just not a valid message here)
    Client c;
    CHECK(s.connect(c));
    CHECK(c.send_raw(ltest::frame_of(std::string(LibraryService::kMaxFrame, '\0'))));
    auto e = c.recv();
    CHECK(e && (e->has_error() || e->has_ack() || e->has_state()));
    CHECK(c.get().has_value());
  }
  {  // a client that never handshakes is dropped
    Client c;
    CHECK(c.connect(s.sock));
    CHECK(c.at_eof(10000));
  }
  {  // the service is still fine after all of that
    Client c;
    CHECK(s.connect(c));
    CHECK(c.get().has_value());
  }
}

void test_every_message() {
  Svc s;
  Client c;
  CHECK(s.connect(c));
  // Favorites.
  CHECK(is_ack(c.add_fav(uid(1)), true));
  CHECK(is_ack(c.add_fav(uid(2)), true));
  CHECK(is_ack(c.add_fav(uid(1)), false));  // idempotent
  auto g = c.get();
  CHECK(g && g->has_state());
  if (g && g->has_state()) {
    CHECK((ltest::strs(g->state().favorites()) == Ids{uid(1), uid(2)}));
    CHECK(g->state().recent_size() == 0 && g->state().catalog_schema_version() == kCatalogSchemaVersion && !g->state().playing());
    CHECK(g->state().favorite_writes() == 2);
  }
  CHECK(is_ack(c.remove_fav(uid(1)), true));
  CHECK(is_ack(c.remove_fav(uid(1)), false));
  CHECK((db_favs(s) == Ids{uid(2)}));  // written immediately (committed through the WAL)
  // Recent.
  CHECK(is_ack(c.report(uid(5)), true));
  CHECK(is_ack(c.report(uid(6)), true));
  CHECK(is_ack(c.report(uid(5)), true));  // moved to the front
  CHECK(is_ack(c.report(uid(5)), false));  // already the most recent
  g = c.get();
  CHECK(g && (ltest::strs(g->state().recent()) == Ids{uid(5), uid(6)}));
  CHECK((db_recent(s) == Ids{uid(5), uid(6)}));  // idle: written at once
  // SetPlaying / FlushRecent.
  CHECK(is_ack(c.playing(true), true));
  CHECK(is_ack(c.playing(true), false));
  auto f = c.flush();
  CHECK(f && f->has_flush_reply() && !f->flush_reply().written());  // nothing pending
  CHECK(is_ack(c.playing(false), true));
  // LiveSetDeleted and Reconcile.
  CHECK(is_ack(c.add_fav(uid(7)), true));
  ws::v1::LibraryClientMessage m;
  m.mutable_live_set_deleted()->set_live_set_id(uid(7));
  CHECK(is_ack(c.call(m), true));
  CHECK(is_ack(c.call(m), false));  // nothing left to remove
  m.Clear();
  m.mutable_reconcile()->add_existing_live_set_ids(uid(2));
  m.mutable_reconcile()->add_existing_live_set_ids(uid(6));
  CHECK(is_ack(c.call(m), true));  // uid(5) is gone from Recent
  g = c.get();
  CHECK(g && (ltest::strs(g->state().recent()) == Ids{uid(6)}) && (ltest::strs(g->state().favorites()) == Ids{uid(2)}));
  // Subscribe.
  m.Clear();
  m.mutable_subscribe()->set_enable(true);
  CHECK(is_ack(c.call(m), false));
  auto st = c.recv();
  CHECK(st && st->request_id() == 0 && st->has_state());
  // Every id-taking command refuses a bad id and changes nothing.
  for (const std::string& bad : {std::string(), std::string(65, 'a'), std::string("a b"), std::string("../etc"), std::string("\xc3\xa9")}) {
    m.Clear();
    m.mutable_add_favorite()->set_live_set_id(bad);
    CHECK(is_error(c.call(m), ws::v1::LIBRARY_ERROR_CODE_INVALID_ID));
    m.Clear();
    m.mutable_remove_favorite()->set_live_set_id(bad);
    CHECK(is_error(c.call(m), ws::v1::LIBRARY_ERROR_CODE_INVALID_ID));
    m.Clear();
    m.mutable_report_active()->set_live_set_id(bad);
    CHECK(is_error(c.call(m), ws::v1::LIBRARY_ERROR_CODE_INVALID_ID));
    m.Clear();
    m.mutable_live_set_deleted()->set_live_set_id(bad);
    CHECK(is_error(c.call(m), ws::v1::LIBRARY_ERROR_CODE_INVALID_ID));
    m.Clear();
    m.mutable_reconcile()->add_existing_live_set_ids(uid(2));
    m.mutable_reconcile()->add_existing_live_set_ids(bad);
    CHECK(is_error(c.call(m), ws::v1::LIBRARY_ERROR_CODE_INVALID_ID));
  }
  g = c.get();  // the refused Reconcile removed nothing
  CHECK(g && (ltest::strs(g->state().recent()) == Ids{uid(6)}) && (ltest::strs(g->state().favorites()) == Ids{uid(2)}));
  // A large Reconcile (thousands of ids) fits one frame.
  m.Clear();
  for (int i = 0; i < 5000; ++i) m.mutable_reconcile()->add_existing_live_set_ids(uid(100000 + i));
  CHECK(is_ack(c.call(m), true));
  g = c.get();
  CHECK(g && g->state().favorites_size() == 0 && g->state().recent_size() == 0);
}

bool is_storage_failed(const std::optional<W>& r, const char* reason = "fault_injected") {
  return r && r->has_error() && r->error().code() == ws::v1::LIBRARY_ERROR_CODE_STORAGE_FAILED && r->error().details().count("reason") &&
         r->error().details().at("reason") == reason;
}

void test_storage_failures() {
  std::atomic<bool> fail{false};
  Svc s(5000, nullptr, [&fail](const char*) { return fail.load(); });
  Client c;
  CHECK(s.connect(c));
  CHECK(is_ack(c.add_fav(uid(1)), true));
  CHECK(is_ack(c.add_fav(uid(2)), true));
  CHECK(is_ack(c.report(uid(1)), true));
  CHECK(is_ack(c.report(uid(2)), true));
  // The service's view must equal SQLite after every refused command.
  auto same_as_sql = [&] {
    auto g = c.get();
    CHECK(g && g->has_state());
    if (g && g->has_state()) {
      CHECK(ltest::strs(g->state().favorites()) == db_favs(s));
      CHECK(ltest::strs(g->state().recent()) == db_recent(s));
      CHECK(!g->state().recent_unwritten());
    }
  };
  same_as_sql();
  fail = true;
  CHECK(is_storage_failed(c.add_fav(uid(3))));
  same_as_sql();
  CHECK(is_storage_failed(c.remove_fav(uid(1))));
  same_as_sql();
  ws::v1::LibraryClientMessage m;
  m.mutable_live_set_deleted()->set_live_set_id(uid(1));
  CHECK(is_storage_failed(c.call(m)));
  same_as_sql();
  m.Clear();
  m.mutable_reconcile()->add_existing_live_set_ids(uid(2));
  CHECK(is_storage_failed(c.call(m)));
  same_as_sql();
  // The same while playing: the favorite rows are deleted one by one, the recent rows stay in RAM.
  fail = false;
  CHECK(is_ack(c.playing(true), true));
  fail = true;
  m.Clear();
  m.mutable_live_set_deleted()->set_live_set_id(uid(1));
  CHECK(is_storage_failed(c.call(m)));
  fail = false;
  same_as_sql();
  // A failed flush keeps Recent unwritten; the next idle-safe moment retries and writes it.
  CHECK(is_ack(c.report(uid(9)), true));
  fail = true;
  CHECK(is_storage_failed(c.flush()));
  auto g = c.get();
  CHECK(g && g->state().recent_unwritten() && ltest::strs(g->state().recent()).front() == uid(9));
  CHECK((db_recent(s) == Ids{uid(2), uid(1)}));
  CHECK(is_ack(c.playing(false), true));  // idle: tries to write, fails
  g = c.get();
  CHECK(g && g->state().recent_unwritten());
  fail = false;
  CHECK(is_ack(c.playing(true), true));
  CHECK(is_ack(c.playing(false), true));  // retried and written
  g = c.get();
  CHECK(g && !g->state().recent_unwritten());
  CHECK(db_recent(s).front() == uid(9));
}

void insert_favorites(const std::string& dir, int count) {
  {
    std::unique_ptr<Catalog> cat;
    CHECK(Catalog::open(dir, cat) == CatalogStatus::kOk);
  }
  sqlite3* h = nullptr;
  sqlite3_open((dir + "/catalog.db").c_str(), &h);
  sqlite3_exec(h, "BEGIN", nullptr, nullptr, nullptr);
  for (int i = 0; i < count; ++i) {
    std::string id = std::to_string(i);
    id = std::string(64 - id.size(), 'f') + id;  // the longest id the catalog accepts
    const std::string q = "INSERT INTO live_set_favorites VALUES('" + id + "', " + std::to_string(i + 1) + ")";
    sqlite3_exec(h, q.c_str(), nullptr, nullptr, nullptr);
  }
  sqlite3_exec(h, ("UPDATE meta SET value=" + std::to_string(count + 10) + " WHERE key='next_seq'").c_str(), nullptr, nullptr, nullptr);
  sqlite3_exec(h, "COMMIT", nullptr, nullptr, nullptr);
  sqlite3_close(h);
}

// The favorites limit is sized so the State of a full list (64-byte ids) and the pushes always fit one frame.
void test_favorites_limit_and_frame() {
  Svc s(5000, nullptr, nullptr, [](const std::string& dir) { insert_favorites(dir, static_cast<int>(kMaxFavorites)); });
  Client c;
  CHECK(s.connect(c));
  ws::v1::LibraryClientMessage m;
  m.mutable_subscribe()->set_enable(true);
  CHECK(c.call(m).has_value());
  auto st = c.recv();  // the initial State push of a full list
  CHECK(st && st->has_state() && st->state().favorites_size() == static_cast<int>(kMaxFavorites));
  for (int i = 0; i < 20; ++i) c.report(uid(i));
  auto g = c.get();
  CHECK(g && g->has_state() && g->state().favorites_size() == static_cast<int>(kMaxFavorites) && g->state().recent_size() == 20);
  auto r = c.add_fav(uid(77));
  CHECK(r && r->has_error() && r->error().code() == ws::v1::LIBRARY_ERROR_CODE_LIMIT_REACHED && r->error().details().at("limit") == std::to_string(kMaxFavorites));
  g = c.get();
  CHECK(g && g->state().favorites_size() == static_cast<int>(kMaxFavorites));
  CHECK(db_favs(s).size() == kMaxFavorites);
  // One removal publishes the full list as a FavoritesChanged push without degrading to an error.
  CHECK(is_ack(c.remove_fav(std::string(63, 'f') + "0"), true));
  bool saw = false;
  for (int i = 0; i < 100 && !saw; ++i) {
    auto p = i < static_cast<int>(c.pushes.size()) ? std::optional<W>(c.pushes[i]) : c.recv(2000);
    if (!p) break;
    saw = p->has_favorites_changed() && p->favorites_changed().favorites_size() == static_cast<int>(kMaxFavorites) - 1;
  }
  CHECK(saw);
}

void test_slow_subscriber_and_client_cap() {
  {
    Svc s(5000, nullptr, nullptr, [](const std::string& dir) { insert_favorites(dir, 1500); });
    Client slow, busy;
    CHECK(s.connect(slow) && s.connect(busy));
    ws::v1::LibraryClientMessage m;
    m.mutable_subscribe()->set_enable(true);
    CHECK(slow.call(m).has_value());  // and never reads again
    int answered = 0;
    for (int i = 0; i < 90; ++i) {  // ~100 KB pushed to the subscriber per change
      auto r = (i % 2 == 0) ? busy.add_fav(uid(i)) : busy.remove_fav(uid(i - 1));
      if (r && r->has_ack()) ++answered;
    }
    CHECK(answered == 90);  // the other client was never stalled
    CHECK(ltest::wait_until([&] { return s.svc->dropped_slow() >= 1; }));
    Client late;
    CHECK(s.connect(late));  // and the service still accepts and answers
  }
  {  // max_clients: the third connection is closed right after accept, the first two keep working
    Svc s(5000, [](LibraryServiceOptions& o) { o.max_clients = 2; });
    Client a, b, c3;
    CHECK(s.connect(a) && s.connect(b));
    CHECK(c3.connect(s.sock));
    CHECK(c3.at_eof());
    CHECK(a.get().has_value() && b.get().has_value());
  }
}

void test_half_close() {
  Svc s;
  {  // command, then half-close: the command is applied AND answered
    Client c;
    CHECK(s.connect(c));
    ws::v1::LibraryClientMessage m;
    m.set_request_id(500);
    m.mutable_add_favorite()->set_live_set_id(uid(1));
    CHECK(c.send(m));
    c.shutdown_write();
    auto r = c.recv();
    CHECK(r && r->request_id() == 500 && r->has_ack() && r->ack().changed());
    CHECK(c.at_eof());
  }
  {  // command, then a full close without reading: still applied
    Client c;
    CHECK(s.connect(c));
    ws::v1::LibraryClientMessage m;
    m.set_request_id(501);
    m.mutable_add_favorite()->set_live_set_id(uid(2));
    CHECK(c.send(m));
    c.close();
    CHECK(ltest::wait_until([&] { return db_favs(s).size() == 2; }));
  }
  CHECK((db_favs(s) == Ids{uid(1), uid(2)}));
}

// playing=true is a lease of the connection that set it: its disconnect, or the lease timeout (a fake clock here), ends it and flushes Recent.
void test_playing_lease() {
  {
    Svc s;
    Client a;
    CHECK(s.connect(a));
    CHECK(is_ack(a.playing(true), true));
    CHECK(is_ack(a.report(uid(1)), true));
    CHECK(db_recent(s).empty());
    a.close();  // the engine died while "performing"
    CHECK(ltest::wait_until([&] { return db_recent(s) == Ids{uid(1)}; }));
    Client b;
    CHECK(s.connect(b));
    auto g = b.get();
    CHECK(g && !g->state().playing() && !g->state().recent_unwritten());
  }
  {  // another connection's lease is not ended by an unrelated disconnect
    Svc s;
    Client a, b;
    CHECK(s.connect(a) && s.connect(b));
    CHECK(is_ack(a.playing(true), true));
    CHECK(is_ack(a.report(uid(1)), true));
    b.close();
    CHECK(a.get().has_value());
    auto g = a.get();
    CHECK(g && g->state().playing() && g->state().recent_unwritten());
  }
  {  // the lease timeout, on an injected clock
    auto fake = std::make_shared<std::atomic<long long>>(0);
    const auto base = std::chrono::steady_clock::now();
    Svc s(5000, [&](LibraryServiceOptions& o) {
      o.playing_lease_ms = 1000;
      o.clock = [fake, base] { return base + std::chrono::milliseconds(fake->load()); };
    });
    Client a;
    CHECK(s.connect(a));
    CHECK(is_ack(a.playing(true), true));
    CHECK(is_ack(a.report(uid(1)), true));
    fake->store(999);
    auto g = a.get();
    CHECK(g && g->state().playing() && g->state().recent_unwritten());
    fake->store(1000);
    CHECK(ltest::wait_until([&] { return db_recent(s) == Ids{uid(1)}; }));
    g = a.get();
    CHECK(g && !g->state().playing());
    // A renewed SetPlaying(true) restarts the lease.
    CHECK(is_ack(a.playing(true), true));
    fake->store(1500);
    CHECK(is_ack(a.report(uid(2)), true));
    g = a.get();
    CHECK(g && g->state().playing() && g->state().recent_unwritten());
  }
}

void test_reconcile_safety() {
  Svc s;
  Client c;
  CHECK(s.connect(c));
  for (int i = 1; i <= 3; ++i) {
    c.add_fav(uid(i));
    c.report(uid(i));
  }
  ws::v1::LibraryClientMessage m;
  m.mutable_reconcile();  // an empty list
  CHECK(c.call(m) && c.call(m)->error().code() == ws::v1::LIBRARY_ERROR_CODE_INVALID_COMMAND);
  auto r = c.call(m);
  CHECK(r && r->has_error() && r->error().details().at("reason") == "empty_reconcile");
  CHECK((db_favs(s).size() == 3) && (db_recent(s).size() == 3));
  m.mutable_reconcile()->set_allow_empty(true);  // explicit
  CHECK(is_ack(c.call(m), true));
  CHECK(db_favs(s).empty() && db_recent(s).empty());
  CHECK(is_ack(c.call(m), false));  // nothing to delete: empty is fine when the catalog is empty anyway
  // Chunked: 10000 ids in three chunks; the union is applied by the last one only.
  for (int i = 1; i <= 3; ++i) c.add_fav(uid(i));
  auto chunk = [&](int from, int to, bool more) {
    ws::v1::LibraryClientMessage k;
    for (int i = from; i < to; ++i) k.mutable_reconcile()->add_existing_live_set_ids(uid(100000 + i));
    k.mutable_reconcile()->set_more(more);
    return c.call(k);
  };
  CHECK(is_ack(chunk(0, 4000, true), false));
  CHECK(db_favs(s).size() == 3);  // nothing applied yet
  {
    ws::v1::LibraryClientMessage k;
    k.mutable_reconcile()->add_existing_live_set_ids(uid(2));  // the survivor lives in the middle chunk
    k.mutable_reconcile()->set_more(true);
    CHECK(is_ack(c.call(k), false));
  }
  CHECK(is_ack(chunk(4000, 10000, false), true));
  CHECK((db_favs(s) == Ids{uid(2)}));
  // A bad id in a chunk drops the accumulation and the connection starts over.
  {
    ws::v1::LibraryClientMessage k;
    k.mutable_reconcile()->add_existing_live_set_ids(uid(2));
    k.mutable_reconcile()->set_more(true);
    CHECK(is_ack(c.call(k), false));
    k.Clear();
    k.mutable_reconcile()->add_existing_live_set_ids("bad id");
    CHECK(is_error(c.call(k), ws::v1::LIBRARY_ERROR_CODE_INVALID_ID));
    k.Clear();
    k.mutable_reconcile()->add_existing_live_set_ids(uid(9));
    CHECK(is_ack(c.call(k), true));  // only uid(9) exists now: uid(2) goes
  }
  CHECK(db_favs(s).empty());
  // A connection that closes before the final chunk applies nothing.
  c.add_fav(uid(5));
  {
    Client d;
    CHECK(s.connect(d));
    ws::v1::LibraryClientMessage k;
    k.mutable_reconcile()->add_existing_live_set_ids(uid(6));
    k.mutable_reconcile()->set_more(true);
    CHECK(is_ack(d.call(k), false));
  }
  CHECK((db_favs(s) == Ids{uid(5)}));
}

void test_socket_group() {
  const struct group* g = ::getgrgid(::getegid());
  CHECK(g != nullptr);
  if (!g) return;
  {
    Svc s(5000, [&](LibraryServiceOptions& o) { o.socket_group = g->gr_name; });
    struct stat sb{};
    CHECK(::stat(s.sock.c_str(), &sb) == 0 && (sb.st_mode & 0777) == 0660 && sb.st_gid == ::getegid());
    Client c;
    CHECK(s.connect(c));
  }
  TempDir d;
  LibraryServiceOptions o;
  o.data_dir = d.path;
  o.socket_path = d.sub("s.sock");
  o.socket_group = "no-such-group-velora-test";
  LibraryService svc(o);
  std::string err;
  CHECK(!svc.start(err));
  CHECK(!fs::exists(o.socket_path));
}

void test_pushes() {
  Svc s;
  Client a, b;
  CHECK(s.connect(a) && s.connect(b));
  ws::v1::LibraryClientMessage m;
  m.mutable_subscribe()->set_enable(true);
  CHECK(a.call(m).has_value());
  auto first = a.recv();
  CHECK(first && first->has_state() && first->request_id() == 0);
  // b changes things; a (subscribed) hears about it, b (not subscribed) only gets replies.
  CHECK(is_ack(b.add_fav(uid(1)), true));
  auto p = a.recv();
  CHECK(p && p->request_id() == 0 && p->has_favorites_changed() && (ltest::strs(p->favorites_changed().favorites()) == Ids{uid(1)}));
  CHECK(is_ack(b.add_fav(uid(1)), false));  // no change, no push
  CHECK(is_ack(b.report(uid(9)), true));
  p = a.recv();
  CHECK(p && p->has_recent_changed() && (ltest::strs(p->recent_changed().recent()) == Ids{uid(9)}));
  CHECK(is_ack(b.remove_fav(uid(1)), true));
  p = a.recv();
  CHECK(p && p->has_favorites_changed() && p->favorites_changed().favorites_size() == 0);
  // A deletion publishes both.
  b.add_fav(uid(9));
  a.recv();  // FavoritesChanged for the add
  ws::v1::LibraryClientMessage d;
  d.mutable_live_set_deleted()->set_live_set_id(uid(9));
  CHECK(is_ack(b.call(d), true));
  auto p1 = a.recv();
  auto p2 = a.recv();
  CHECK(p1 && p2 && p1->has_favorites_changed() && p2->has_recent_changed());
  CHECK(b.pushes.empty());
  // Unsubscribe: no more pushes (the next message a sees is the reply to its own call).
  m.mutable_subscribe()->set_enable(false);
  CHECK(is_ack(a.call(m), false));
  CHECK(is_ack(b.add_fav(uid(3)), true));
  CHECK(a.get().has_value());
  CHECK(a.pushes.empty());
}

void test_concurrent_clients() {
  Svc s;
  constexpr int kClients = 8, kEach = 25;
  std::atomic<int> failures{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < kClients; ++t)
    ts.emplace_back([&, t] {
      Client c;
      if (!s.connect(c)) {
        ++failures;
        return;
      }
      for (int i = 0; i < kEach; ++i) {
        auto r = c.add_fav(uid(t * 1000 + i));
        if (!r || !r->has_ack() || !r->ack().changed()) ++failures;
        r = c.report(uid(t * 1000 + i));
        if (!r || !r->has_ack()) ++failures;
      }
    });
  for (auto& t : ts) t.join();
  CHECK(failures == 0);
  Client c;
  CHECK(s.connect(c));
  auto g = c.get();
  CHECK(g && g->state().favorites_size() == kClients * kEach);
  CHECK(g && g->state().favorite_writes() == kClients * kEach);  // one committed write per distinct favorite: no lost or doubled update
  CHECK(g && g->state().recent_size() == static_cast<int>(kMaxRecent));
  // Each client's own favorites are in the order that client added them (the service serialised them).
  if (g) {
    const Ids all = ltest::strs(g->state().favorites());
    for (int t = 0; t < kClients; ++t) {
      int last = -1;
      for (const std::string& id : all) {
        const int v = std::stoi(id.substr(id.size() - 12));
        if (v / 1000 != t) continue;
        CHECK(v % 1000 > last);
        last = v % 1000;
      }
    }
  }
}

// "No write during playing": a simulated hour of Live Set switches (3600 one-per-second reports, no real time involved) with playing=true writes
// nothing: the write counter stays 0 and the SQLite files are unchanged; then exactly one write when playing becomes false.
void test_no_write_while_playing() {
  Svc s;
  Client c;
  CHECK(s.connect(c));
  CHECK(is_ack(c.add_fav(uid(1)), true));
  CHECK(is_ack(c.report(uid(1)), true));  // idle: one Recent write
  CHECK(is_ack(c.playing(true), true));
  const std::string db = s.d.sub("catalog.db"), wal = s.d.sub("catalog.db-wal");
  auto snap = [&](const std::string& p) {
    std::error_code ec;
    return std::make_pair(fs::exists(p, ec) ? fs::file_size(p, ec) : std::uintmax_t(-1), fs::exists(p, ec) ? fs::last_write_time(p, ec) : fs::file_time_type{});
  };
  const auto db0 = snap(db), wal0 = snap(wal);
  auto g0 = c.get();
  CHECK(g0 && g0->state().recent_writes() == 1 && g0->state().playing());
  for (int second = 0; second < 3600; ++second) {
    auto r = c.report(uid(10 + second % 40));
    CHECK(r && r->has_ack());
    if (!r || !r->has_ack()) break;
  }
  auto g = c.get();
  CHECK(g && g->has_state());
  if (g && g->has_state()) {
    CHECK(g->state().recent_writes() == 1);          // the counter did not move
    CHECK(g->state().recent_unwritten());
    CHECK(g->state().recent_size() == static_cast<int>(kMaxRecent));
    // RAM Recent: the last 20 distinct, most recent first.
    Ids want;
    for (int second = 3599; want.size() < kMaxRecent; --second) want.push_back(uid(10 + second % 40));
    CHECK(ltest::strs(g->state().recent()) == want);
  }
  CHECK(snap(db) == db0 && snap(wal) == wal0);       // not a byte, not a timestamp
  CHECK((db_recent(s) == Ids{uid(1)}));              // SQLite still holds the pre-performance Recent
  CHECK(s.svc->recent_writes() == 1);
  // Favorites are explicit commands and still go straight to disk while playing.
  CHECK(is_ack(c.add_fav(uid(2)), true));
  CHECK((db_favs(s) == Ids{uid(1), uid(2)}));
  // The idle-safe moment: exactly one write.
  CHECK(is_ack(c.playing(false), true));
  g = c.get();
  CHECK(g && g->state().recent_writes() == 2 && !g->state().recent_unwritten());
  CHECK(db_recent(s).size() == kMaxRecent);
  CHECK((db_recent(s) == ltest::strs(g->state().recent())));
  CHECK(is_ack(c.playing(false), false));  // no further write
  CHECK(is_ack(c.playing(true), true));
  CHECK(is_ack(c.playing(false), true));   // nothing pending: still no write
  g = c.get();
  CHECK(g && g->state().recent_writes() == 2);
  // An explicit FlushRecent is a safe-flush call and is honoured even while playing.
  CHECK(is_ack(c.playing(true), true));
  CHECK(is_ack(c.report(uid(500)), true));
  auto f = c.flush();
  CHECK(f && f->has_flush_reply() && f->flush_reply().written() && f->flush_reply().recent_writes() == 3);
  CHECK(db_recent(s).front() == uid(500));
  // Orderly shutdown writes what is pending even when playing.
  CHECK(is_ack(c.report(uid(501)), true));
  CHECK(db_recent(s).front() == uid(500));
  c.close();
  s.svc->stop();
  CHECK(db_recent(s).front() == uid(501));
}

void test_deletion_reconcile() {
  Svc s;
  Client c;
  CHECK(s.connect(c));
  for (int i = 1; i <= 4; ++i) c.add_fav(uid(i));
  for (int i = 1; i <= 3; ++i) c.report(uid(i));  // recent: 3 2 1
  ws::v1::LibraryClientMessage m;
  m.mutable_live_set_deleted()->set_live_set_id(uid(2));
  CHECK(is_ack(c.call(m), true));
  CHECK((db_favs(s) == Ids{uid(1), uid(3), uid(4)}) && (db_recent(s) == Ids{uid(3), uid(1)}));
  m.Clear();
  m.mutable_reconcile()->add_existing_live_set_ids(uid(1));
  m.mutable_reconcile()->add_existing_live_set_ids(uid(4));
  CHECK(is_ack(c.call(m), true));
  CHECK((db_favs(s) == Ids{uid(1), uid(4)}) && (db_recent(s) == Ids{uid(1)}));
  auto g = c.get();
  CHECK(g && (ltest::strs(g->state().favorites()) == Ids{uid(1), uid(4)}) && (ltest::strs(g->state().recent()) == Ids{uid(1)}));
  // While playing: the favorite row goes at once, the recent row waits for the idle-safe write.
  c.report(uid(4));
  c.playing(true);
  m.Clear();
  m.mutable_live_set_deleted()->set_live_set_id(uid(4));
  CHECK(is_ack(c.call(m), true));
  CHECK((db_favs(s) == Ids{uid(1)}));
  CHECK((db_recent(s) == Ids{uid(4), uid(1)}));
  g = c.get();
  CHECK(g && (ltest::strs(g->state().recent()) == Ids{uid(1)}) && g->state().recent_unwritten());
  c.playing(false);
  CHECK((db_recent(s) == Ids{uid(1)}));
  // Restart: the reconciled state is what comes back.
  c.close();
  s.restart();
  Client c2;
  CHECK(s.connect(c2));
  g = c2.get();
  CHECK(g && (ltest::strs(g->state().favorites()) == Ids{uid(1)}) && (ltest::strs(g->state().recent()) == Ids{uid(1)}));
}

void test_persistence_and_single_writer() {
  Svc s;
  {
    Client c;
    CHECK(s.connect(c));
    c.add_fav(uid(1));
    c.add_fav(uid(2));
    c.report(uid(3));
    c.playing(true);
    c.report(uid(4));  // unwritten when the service stops: the orderly shutdown writes it
  }
  // A second service on the same data directory is refused (the flock), even with another socket.
  {
    LibraryServiceOptions o;
    o.data_dir = s.d.path;
    o.socket_path = s.d.sub("other.sock");
    LibraryService second(o);
    std::string err;
    CatalogStatus st = CatalogStatus::kOk;
    CHECK(!second.start(err, &st));
    CHECK(st == CatalogStatus::kBusy);
    CHECK(!fs::exists(o.socket_path));
  }
  // A live listener on the socket path is never stolen (other data directory, same socket).
  {
    TempDir d2;
    LibraryServiceOptions o;
    o.data_dir = d2.path;
    o.socket_path = s.sock;
    LibraryService second(o);
    std::string err;
    CHECK(!second.start(err));
    Client c;
    CHECK(s.connect(c));  // the first one still serves
  }
  s.restart();
  Client c;
  CHECK(s.connect(c));
  auto g = c.get();
  CHECK(g && (ltest::strs(g->state().favorites()) == Ids{uid(1), uid(2)}));
  CHECK(g && (ltest::strs(g->state().recent()) == Ids{uid(4), uid(3)}));
  // The socket is 0600; a stale socket file from a crashed run is replaced; a file that is not a socket is never touched.
  struct stat sb{};
  CHECK(::stat(s.sock.c_str(), &sb) == 0 && (sb.st_mode & 0777) == 0600);
  c.close();
  s.svc->stop();
  CHECK(!fs::exists(s.sock));
  {
    Client dummy;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, s.sock.c_str(), sizeof a.sun_path - 1);
    CHECK(::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
    ::close(fd);  // a socket file with nobody listening
    CHECK(fs::exists(s.sock));
    s.restart();
    Client c2;
    CHECK(s.connect(c2));
  }
  s.svc->stop();
  { std::ofstream(s.sock) << "precious"; }
  LibraryServiceOptions o;
  o.data_dir = s.d.path;
  o.socket_path = s.sock;
  LibraryService third(o);
  std::string err;
  CHECK(!third.start(err));
  CHECK(ltest::slurp(s.sock) == "precious");
}

void test_catalog_refusals_at_start() {
  {  // a foreign file as the catalog
    TempDir d;
    { std::ofstream(d.sub("catalog.db")) << "not sqlite"; }
    LibraryServiceOptions o;
    o.data_dir = d.path;
    o.socket_path = d.sub("s.sock");
    LibraryService svc(o);
    std::string err;
    CatalogStatus st = CatalogStatus::kOk;
    CHECK(!svc.start(err, &st) && st == CatalogStatus::kNotACatalog);
    CHECK(!fs::exists(o.socket_path));
  }
  {  // a catalog from a newer librarian
    TempDir d;
    {
      std::unique_ptr<Catalog> c;
      CHECK(Catalog::open(d.path, c) == CatalogStatus::kOk);
    }
    sqlite3* h = nullptr;
    sqlite3_open(d.sub("catalog.db").c_str(), &h);
    sqlite3_exec(h, "UPDATE meta SET value=9 WHERE key='schema_version'", nullptr, nullptr, nullptr);
    sqlite3_close(h);
    LibraryServiceOptions o;
    o.data_dir = d.path;
    o.socket_path = d.sub("s.sock");
    LibraryService svc(o);
    std::string err;
    CatalogStatus st = CatalogStatus::kOk;
    CHECK(!svc.start(err, &st) && st == CatalogStatus::kSchemaTooNew);
  }
}

// kill the service process in the middle of a Recent flush (fork + _exit / SIGKILL from the catalog fault hook) and recover from the WAL.
void test_crash_mid_flush() {
  for (const bool use_sigkill : {false, true}) {
    TempDir d;
    const std::string sock = d.sub("s.sock");
    const pid_t pid = ::fork();
    if (pid == 0) {
      LibraryServiceOptions o;
      o.data_dir = d.path;
      o.socket_path = sock;
      LibraryService svc(o);
      int calls = 0;
      svc.set_catalog_fault_hook([&calls, use_sigkill](const char* p) {
        if (std::string(p) == "recent.before_commit" && ++calls == 2) {  // the second Recent write dies with its rows written, uncommitted
          if (use_sigkill) ::kill(::getpid(), SIGKILL);
          _exit(9);
        }
        return false;
      });
      std::string err;
      if (!svc.start(err)) _exit(10);
      for (;;) ::pause();
    }
    Client c;
    CHECK(ltest::wait_until([&] { return c.connect(sock); }));
    CHECK(c.handshake());
    CHECK(is_ack(c.add_fav(uid(1)), true));
    CHECK(is_ack(c.report(uid(2)), true));  // write 1: committed
    CHECK(c.send([] {
      ws::v1::LibraryClientMessage m;
      m.set_request_id(99);
      m.mutable_report_active()->set_live_set_id(uid(3));  // write 2: the process dies inside it
      return m;
    }()));
    int st = 0;
    ::waitpid(pid, &st, 0);
    CHECK((WIFEXITED(st) && WEXITSTATUS(st) == 9) || (WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL));
    CHECK(!c.recv(500).has_value());  // no reply ever came
    c.close();
    // The catalog is consistent: the committed favorite and the first Recent, none of the half-written second one.
    {
      std::unique_ptr<Catalog> cat;
      CHECK(Catalog::open(d.path, cat) == CatalogStatus::kOk);  // the writer lock died with the process
      Ids f, r;
      if (cat) {
        CHECK(cat->favorites(f) == CatalogStatus::kOk && cat->recent(r) == CatalogStatus::kOk);
        CHECK((f == Ids{uid(1)}) && (r == Ids{uid(2)}));
      }
    }
    // The service restarts on it (stale socket file replaced) and serves the recovered state.
    LibraryServiceOptions o;
    o.data_dir = d.path;
    o.socket_path = sock;
    LibraryService svc(o);
    std::string err;
    CHECK(svc.start(err));
    Client c2;
    CHECK(c2.connect(sock) && c2.handshake());
    auto g = c2.get();
    CHECK(g && (ltest::strs(g->state().favorites()) == Ids{uid(1)}) && (ltest::strs(g->state().recent()) == Ids{uid(2)}));
  }
}

}  // namespace

int main() {
  ::signal(SIGPIPE, SIG_IGN);
  test_crash_mid_flush();  // forks: first, while this process has no other thread
  test_handshake_and_framing();
  test_half_close();
  test_storage_failures();
  test_favorites_limit_and_frame();
  test_slow_subscriber_and_client_cap();
  test_playing_lease();
  test_reconcile_safety();
  test_socket_group();
  test_every_message();
  test_pushes();
  test_concurrent_clients();
  test_no_write_while_playing();
  test_deletion_reconcile();
  test_persistence_and_single_writer();
  test_catalog_refusals_at_start();
  if (g_fail) {
    std::printf("librarian_service_test: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("librarian_service_test: ok\n");
  return 0;
}
