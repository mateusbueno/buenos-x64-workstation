// Story 4.11: the librarian Catalog (SQLite WAL). Schema creation, the version policy, favorites (idempotent, ordered by added_seq), Recent (order,
// dedupe, cap of 20), persistence across reopen, the single-writer lock (same and another process), corrupt / foreign files, the delete cascade,
// fault injection and crash recovery (fork + kill mid-write). No timing assertions.
#include <fcntl.h>
#include <sqlite3.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <functional>
#include <set>
#include <memory>
#include <string>
#include <vector>

#include "lib_test_util.hpp"
#include "velora/librarian/catalog.hpp"

using namespace velora::librarian;
using ltest::TempDir;
using ltest::uid;
using Ids = std::vector<std::string>;

namespace {

std::unique_ptr<Catalog> open_ok(const std::string& dir) {
  std::unique_ptr<Catalog> c;
  const CatalogStatus st = Catalog::open(dir, c);
  if (st != CatalogStatus::kOk) std::printf("open(%s) -> %s\n", dir.c_str(), to_string(st));
  CHECK(st == CatalogStatus::kOk);
  return c;
}

Ids favs(Catalog& c) {
  Ids v;
  CHECK(c.favorites(v) == CatalogStatus::kOk);
  return v;
}
Ids recs(Catalog& c) {
  Ids v;
  CHECK(c.recent(v) == CatalogStatus::kOk);
  return v;
}

// A raw SQLite connection to the file (the Catalog must be closed or only read).
struct Raw {
  sqlite3* db = nullptr;
  explicit Raw(const std::string& path) { CHECK(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK); }
  ~Raw() { sqlite3_close(db); }
  std::string one(const char* sql) {
    sqlite3_stmt* st = nullptr;
    std::string out = "<err>";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
      const unsigned char* t = sqlite3_column_text(st, 0);
      out = t ? reinterpret_cast<const char*>(t) : "";
    }
    sqlite3_finalize(st);
    return out;
  }
  void exec(const char* sql) { CHECK(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK); }
};

void test_schema() {
  TempDir d;
  {
    auto c = open_ok(d.path);
    CHECK(c && c->schema_version() == 1 && !c->migrated());
    CHECK(c && favs(*c).empty() && recs(*c).empty());
    CHECK(c && c->total_writes() == 0);
    // Reading the file while the catalog is open: WAL, user_version, exactly the tables of this story.
    Raw r(d.sub("catalog.db"));
    CHECK(r.one("PRAGMA journal_mode") == "wal");
    CHECK(r.one("PRAGMA user_version") == "1");
    CHECK(r.one("SELECT value FROM meta WHERE key='schema_version'") == "1");
    CHECK(r.one("SELECT group_concat(name) FROM (SELECT name FROM sqlite_master WHERE type='table' ORDER BY name)") ==
          "live_set_favorites,live_set_recent,meta");
  }
  // Reopen: same schema, nothing migrated.
  auto c = open_ok(d.path);
  CHECK(c && c->schema_version() == 1 && !c->migrated());
}

void test_version_policy() {
  TempDir d;
  { auto c = open_ok(d.path); }
  {
    Raw r(d.sub("catalog.db"));
    r.exec("UPDATE meta SET value=2 WHERE key='schema_version'");
  }
  std::unique_ptr<Catalog> c;
  CHECK(Catalog::open(d.path, c) == CatalogStatus::kSchemaTooNew);
  CHECK(!c);
  {  // refused and untouched: still version 2 with its data
    Raw r(d.sub("catalog.db"));
    CHECK(r.one("SELECT value FROM meta WHERE key='schema_version'") == "2");
    r.exec("UPDATE meta SET value=0 WHERE key='schema_version'");
  }
  CHECK(Catalog::open(d.path, c) == CatalogStatus::kNotACatalog);  // below 1 is not a version this software ever wrote
  {
    Raw r(d.sub("catalog.db"));
    r.exec("DELETE FROM meta WHERE key='schema_version'");
  }
  CHECK(Catalog::open(d.path, c) == CatalogStatus::kNotACatalog);
  {  // a file the policy accepts again
    Raw r(d.sub("catalog.db"));
    r.exec("INSERT INTO meta(key, value) VALUES('schema_version', 1)");
  }
  CHECK(Catalog::open(d.path, c) == CatalogStatus::kOk);
}

void test_favorites() {
  TempDir d;
  auto c = open_ok(d.path);
  bool ch = false;
  CHECK(c->add_favorite("c", ch) == CatalogStatus::kOk && ch);
  CHECK(c->add_favorite("a", ch) == CatalogStatus::kOk && ch);
  CHECK(c->add_favorite("b", ch) == CatalogStatus::kOk && ch);
  CHECK((favs(*c) == Ids{"c", "a", "b"}));  // ascending added_seq
  CHECK(c->favorite_writes() == 3);
  // Idempotent: adding again changes nothing, keeps the position and does not write.
  CHECK(c->add_favorite("a", ch) == CatalogStatus::kOk && !ch);
  CHECK((favs(*c) == Ids{"c", "a", "b"}));
  CHECK(c->favorite_writes() == 3);
  CHECK(c->remove_favorite("a", ch) == CatalogStatus::kOk && ch);
  CHECK(c->remove_favorite("a", ch) == CatalogStatus::kOk && !ch);  // idempotent
  CHECK(c->remove_favorite("zzz", ch) == CatalogStatus::kOk && !ch);
  CHECK(c->favorite_writes() == 4);
  CHECK(c->add_favorite("a", ch) == CatalogStatus::kOk && ch);  // re-added: now last
  CHECK((favs(*c) == Ids{"c", "b", "a"}));
  // Ids: UUIDs and the allowed alphabet pass; everything else is a typed refusal that writes nothing.
  CHECK(c->add_favorite(uid(1), ch) == CatalogStatus::kOk);
  const std::uint64_t w = c->total_writes();
  for (const std::string& bad : {std::string(), std::string(65, 'x'), std::string("a b"), std::string("../x"), std::string("a/b"),
                                  std::string("caf\xc3\xa9"), std::string("a\0b", 3), std::string("x'; DROP TABLE meta;--")}) {
    CHECK(c->add_favorite(bad, ch) == CatalogStatus::kInvalidId);
    CHECK(c->remove_favorite(bad, ch) == CatalogStatus::kInvalidId);
  }
  CHECK(c->add_favorite(std::string(64, 'x'), ch) == CatalogStatus::kOk);
  CHECK(c->total_writes() == w + 1);
  CHECK(favs(*c).size() == 5);
}

void test_favorite_limit() {
  TempDir d;
  { auto c = open_ok(d.path); }
  {
    Raw r(d.sub("catalog.db"));
    r.exec("BEGIN");
    for (std::size_t i = 0; i < kMaxFavorites; ++i) {
      const std::string sql = "INSERT INTO live_set_favorites VALUES('f" + std::to_string(i) + "', " + std::to_string(i + 1) + ")";
      r.exec(sql.c_str());
    }
    r.exec("UPDATE meta SET value=100000 WHERE key='next_seq'");
    r.exec("COMMIT");
  }
  auto c = open_ok(d.path);
  bool ch = false;
  CHECK(c->add_favorite("one-too-many", ch) == CatalogStatus::kLimitReached && !ch);
  CHECK(favs(*c).size() == kMaxFavorites);
  CHECK(c->add_favorite("f0", ch) == CatalogStatus::kOk && !ch);  // an existing one is still an idempotent no-op
  CHECK(c->remove_favorite("f0", ch) == CatalogStatus::kOk && ch);
  CHECK(c->add_favorite("one-too-many", ch) == CatalogStatus::kOk && ch);
}

void test_recent() {
  TempDir d;
  auto c = open_ok(d.path);
  CHECK(c->write_recent({"a", "b", "c"}) == CatalogStatus::kOk);
  CHECK((recs(*c) == Ids{"a", "b", "c"}));  // most recent first
  CHECK(c->recent_writes() == 1);
  CHECK(c->write_recent({"c", "a"}) == CatalogStatus::kOk);
  CHECK((recs(*c) == Ids{"c", "a"}));  // replaced, not merged
  CHECK(c->write_recent({}) == CatalogStatus::kOk);
  CHECK(recs(*c).empty());
  // Duplicates and bad ids are refused and change nothing.
  CHECK(c->write_recent({"x", "y"}) == CatalogStatus::kOk);
  const std::uint64_t w = c->recent_writes();
  CHECK(c->write_recent({"p", "q", "p"}) == CatalogStatus::kInvalidId);
  CHECK(c->write_recent({"p", "bad id"}) == CatalogStatus::kInvalidId);
  CHECK((recs(*c) == Ids{"x", "y"}));
  CHECK(c->recent_writes() == w);
  // The cap: 25 ids keep the 20 most recent, in order.
  Ids many;
  for (int i = 0; i < 25; ++i) many.push_back(uid(i));
  CHECK(c->write_recent(many) == CatalogStatus::kOk);
  const Ids got = recs(*c);
  CHECK(got.size() == kMaxRecent);
  CHECK((got == Ids(many.begin(), many.begin() + 20)));
  Raw r(d.sub("catalog.db"));
  CHECK(r.one("SELECT count(*) FROM live_set_recent") == "20");
  CHECK(r.one("SELECT live_set_id FROM live_set_recent ORDER BY last_played_seq DESC LIMIT 1") == uid(0));
}

void test_persistence() {
  TempDir d;
  {
    auto c = open_ok(d.path);
    bool ch = false;
    c->add_favorite("x", ch);
    c->add_favorite("y", ch);
    c->write_recent({"r2", "r1"});
  }
  auto c = open_ok(d.path);
  CHECK((favs(*c) == Ids{"x", "y"}));
  CHECK((recs(*c) == Ids{"r2", "r1"}));
  // The sequence counter is persisted: a favorite added now sorts after the old ones, and a new Recent write keeps its own order.
  bool ch = false;
  c->add_favorite("z", ch);
  CHECK((favs(*c) == Ids{"x", "y", "z"}));
  c->write_recent({"n3", "n2", "r2"});
  CHECK((recs(*c) == Ids{"n3", "n2", "r2"}));
  Raw r(d.sub("catalog.db"));
  CHECK(std::stoll(r.one("SELECT value FROM meta WHERE key='next_seq'")) > 7);
}

void test_single_writer() {
  TempDir d;
  auto c = open_ok(d.path);
  std::unique_ptr<Catalog> second;
  CHECK(Catalog::open(d.path, second) == CatalogStatus::kBusy);  // same process
  CHECK(!second);
  const pid_t pid = ::fork();
  if (pid == 0) {  // another process
    std::unique_ptr<Catalog> other;
    _exit(Catalog::open(d.path, other) == CatalogStatus::kBusy ? 0 : 1);
  }
  int st = 0;
  ::waitpid(pid, &st, 0);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  c.reset();  // the lock goes with the object
  CHECK(Catalog::open(d.path, second) == CatalogStatus::kOk);
}

void test_bad_files() {
  {  // a directory that does not exist, and a path that is a file
    std::unique_ptr<Catalog> c;
    CHECK(Catalog::open("/tmp/vl.no-such-dir.xyz", c) == CatalogStatus::kIo);
    TempDir d;
    { std::ofstream(d.sub("f")) << "x"; }
    CHECK(Catalog::open(d.sub("f"), c) == CatalogStatus::kIo);
  }
  {  // a foreign (text) file named catalog.db is refused and left byte-identical
    TempDir d;
    const std::string text = "this is not a database, it is somebody's notes\n";
    { std::ofstream(d.sub("catalog.db")) << text; }
    std::unique_ptr<Catalog> c;
    CHECK(Catalog::open(d.path, c) == CatalogStatus::kNotACatalog);
    CHECK(ltest::slurp(d.sub("catalog.db")) == text);
  }
  {  // a SQLite database that is not a catalog
    TempDir d;
    {
      sqlite3* db = nullptr;
      sqlite3_open(d.sub("catalog.db").c_str(), &db);
      sqlite3_exec(db, "CREATE TABLE notes(x TEXT); INSERT INTO notes VALUES('keep me')", nullptr, nullptr, nullptr);
      sqlite3_close(db);
    }
    std::unique_ptr<Catalog> c;
    CHECK(Catalog::open(d.path, c) == CatalogStatus::kNotACatalog);
    Raw r(d.sub("catalog.db"));
    CHECK(r.one("SELECT x FROM notes") == "keep me");
    CHECK(r.one("SELECT count(*) FROM sqlite_master WHERE name='meta'") == "0");
  }
  {  // a catalog whose pages were damaged
    TempDir d;
    {
      auto c = open_ok(d.path);
      bool ch = false;
      for (int i = 0; i < 200; ++i) c->add_favorite(uid(i), ch);
    }
    const std::string path = d.sub("catalog.db");
    std::string bytes = ltest::slurp(path);
    CHECK(bytes.size() > 4096);
    for (std::size_t i = 100; i < 4096 && i < bytes.size(); ++i) bytes[i] = char(0xFF);  // keep the 100-byte header, wreck the schema page
    { std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes; }
    ::unlink(d.sub("catalog.db-wal").c_str());
    ::unlink(d.sub("catalog.db-shm").c_str());
    std::unique_ptr<Catalog> c;
    const CatalogStatus st = Catalog::open(d.path, c);
    CHECK(st == CatalogStatus::kCorrupt || st == CatalogStatus::kNotACatalog);
    CHECK(!c);
  }
}

bool exists(const std::string& p) { return ::access(p.c_str(), F_OK) == 0; }

// A refused file is left exactly as it was: same bytes, and no -wal / -shm / lock side effects on the database.
void test_refusal_touches_nothing() {
  auto check = [](const char* what, const std::function<void(const std::string&)>& make, CatalogStatus want) {
    TempDir d;
    make(d.sub("catalog.db"));
    ::unlink(d.sub("catalog.db-wal").c_str());
    ::unlink(d.sub("catalog.db-shm").c_str());
    const std::string before = ltest::slurp(d.sub("catalog.db"));
    std::unique_ptr<Catalog> c;
    const CatalogStatus st = Catalog::open(d.path, c);
    if (st != want) std::printf("%s: got %s want %s\n", what, to_string(st), to_string(want));
    CHECK(st == want);
    CHECK(!c);
    CHECK(ltest::slurp(d.sub("catalog.db")) == before);
    CHECK(!exists(d.sub("catalog.db-wal")) && !exists(d.sub("catalog.db-shm")));
  };
  auto sql = [](const std::string& path, const char* text, bool wal) {
    sqlite3* db = nullptr;
    sqlite3_open(path.c_str(), &db);
    if (wal) sqlite3_exec(db, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    sqlite3_exec(db, text, nullptr, nullptr, nullptr);
    sqlite3_close(db);
  };
  check("foreign sqlite", [&](const std::string& p) { sql(p, "CREATE TABLE notes(x TEXT); INSERT INTO notes VALUES('keep me')", false); }, CatalogStatus::kNotACatalog);
  check("foreign sqlite in WAL mode", [&](const std::string& p) { sql(p, "CREATE TABLE notes(x TEXT); INSERT INTO notes VALUES('keep me')", true); },
        CatalogStatus::kNotACatalog);
  check("views and triggers, no tables", [&](const std::string& p) { sql(p, "CREATE VIEW v AS SELECT 1 AS one", true); }, CatalogStatus::kNotACatalog);
  check("newer schema", [&](const std::string& p) {
    const std::string dir = p.substr(0, p.rfind('/'));
    { std::unique_ptr<Catalog> c; CHECK(Catalog::open(dir, c) == CatalogStatus::kOk); }
    sql(p, "UPDATE meta SET value=2 WHERE key='schema_version'", false);
  }, CatalogStatus::kSchemaTooNew);
  check("meta without a valid version", [&](const std::string& p) { sql(p, "CREATE TABLE meta(key TEXT PRIMARY KEY, value INTEGER)", true); },
        CatalogStatus::kNotACatalog);
}

void test_missing_next_seq_and_big_reconcile() {
  TempDir d;
  {
    auto c = open_ok(d.path);
    bool ch = false;
    c->add_favorite("a", ch);
    c->add_favorite("b", ch);
    c->write_recent({"r"});
  }
  {
    Raw r(d.sub("catalog.db"));
    r.exec("DELETE FROM meta WHERE key='next_seq'");
  }
  auto c = open_ok(d.path);  // the counter is rebuilt above everything stored
  bool ch = false;
  CHECK(c && c->add_favorite("c", ch) == CatalogStatus::kOk);
  CHECK(c && (favs(*c) == Ids{"a", "b", "c"}));
  // Reconcile covers every recent row, not just the 20 the listing shows.
  c.reset();
  {
    Raw r(d.sub("catalog.db"));
    r.exec("BEGIN");
    for (int i = 0; i < 60; ++i) {
      const std::string q = "INSERT OR REPLACE INTO live_set_recent VALUES('x" + std::to_string(i) + "', " + std::to_string(1000 + i) + ")";
      r.exec(q.c_str());
    }
    r.exec("UPDATE meta SET value=5000 WHERE key='next_seq'");
    r.exec("COMMIT");
  }
  c = open_ok(d.path);
  ReconcileResult res;
  std::set<std::string> keep{"a", "x3", "x55"};
  CHECK(c && c->reconcile(keep, res) == CatalogStatus::kOk);
  CHECK(res.recent_removed.size() == 59);  // r and x0..x59 are 61 rows; x3 and x55 stay
  Raw r(d.sub("catalog.db"));
  CHECK(r.one("SELECT count(*) FROM live_set_recent") == "2");
  CHECK(res.favorites_removed.size() == 2);
}

void test_delete_cascade() {
  TempDir d;
  auto c = open_ok(d.path);
  bool ch = false;
  for (const char* id : {"a", "b", "c", "d"}) c->add_favorite(id, ch);
  c->write_recent({"d", "b", "x"});
  RemoveResult rr;
  CHECK(c->remove_live_set("b", rr) == CatalogStatus::kOk && rr.favorite_removed && rr.recent_removed);
  CHECK((favs(*c) == Ids{"a", "c", "d"}) && (recs(*c) == Ids{"d", "x"}));
  CHECK(c->remove_live_set("x", rr) == CatalogStatus::kOk && !rr.favorite_removed && rr.recent_removed);
  const std::uint64_t w = c->total_writes();
  CHECK(c->remove_live_set("never-there", rr) == CatalogStatus::kOk && !rr.favorite_removed && !rr.recent_removed);
  CHECK(c->total_writes() == w);  // nothing to delete, nothing written
  CHECK(c->remove_live_set("bad id", rr) == CatalogStatus::kInvalidId);
  // Reconcile with the list of Live Sets that still exist.
  c->write_recent({"d", "a", "gone1"});
  c->add_favorite("gone2", ch);
  ReconcileResult res;
  CHECK(c->reconcile({"a", "d", "newcomer"}, res) == CatalogStatus::kOk);
  CHECK((res.favorites_removed == Ids{"c", "gone2"}) && (res.recent_removed == Ids{"gone1"}));
  CHECK((favs(*c) == Ids{"a", "d"}) && (recs(*c) == Ids{"d", "a"}));
  const std::uint64_t w2 = c->total_writes();
  CHECK(c->reconcile({"a", "d"}, res) == CatalogStatus::kOk && res.favorites_removed.empty() && res.recent_removed.empty());
  CHECK(c->total_writes() == w2);
}

void test_fault_injection() {
  TempDir d;
  auto c = open_ok(d.path);
  bool ch = false;
  c->add_favorite("keep", ch);
  c->write_recent({"r1", "r2"});
  const std::uint64_t w = c->total_writes();
  for (const char* point : {"favorite.before_commit", "recent.begin", "recent.before_commit", "remove.before_commit"}) {
    c->set_fault_hook([point](const char* p) { return std::string(p) == point; });
    if (std::string(point).rfind("favorite", 0) == 0) CHECK(c->add_favorite("new", ch) == CatalogStatus::kFaultInjected && !ch);
    if (std::string(point).rfind("recent", 0) == 0) CHECK(c->write_recent({"n1", "n2", "n3"}) == CatalogStatus::kFaultInjected);
    if (std::string(point).rfind("remove", 0) == 0) {
      RemoveResult rr;
      CHECK(c->remove_live_set("keep", rr) == CatalogStatus::kFaultInjected);
    }
    c->set_fault_hook(nullptr);
    CHECK((favs(*c) == Ids{"keep"}) && (recs(*c) == Ids{"r1", "r2"}));  // rolled back
    CHECK(c->total_writes() == w);
  }
  CHECK(c->write_recent({"n1"}) == CatalogStatus::kOk);  // the connection is healthy afterwards
}

// kill -9 / _exit in the middle of a write transaction, in another process; the next open recovers the last committed state from the WAL.
void test_crash_recovery() {
  struct Case { const char* point; bool favorite_op; };
  for (const Case cs : {Case{"favorite.before_commit", true}, Case{"recent.begin", false}, Case{"recent.before_commit", false}}) {
    for (const bool use_sigkill : {false, true}) {
      TempDir d;
      const pid_t pid = ::fork();
      if (pid == 0) {
        std::unique_ptr<Catalog> c;
        if (Catalog::open(d.path, c) != CatalogStatus::kOk) _exit(10);
        bool ch = false;
        c->add_favorite("committed-fav", ch);
        c->write_recent({"committed-r2", "committed-r1"});
        const std::string point = cs.point;
        c->set_fault_hook([point, use_sigkill](const char* p) {
          if (point == p) {
            if (use_sigkill) ::kill(::getpid(), SIGKILL);
            _exit(9);
          }
          return false;
        });
        if (cs.favorite_op) c->add_favorite("lost-fav", ch);
        else c->write_recent({"lost-1", "lost-2", "lost-3"});
        _exit(11);  // the hook should have ended the process
      }
      int st = 0;
      ::waitpid(pid, &st, 0);
      CHECK((WIFEXITED(st) && WEXITSTATUS(st) == 9) || (WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL));
      // The writer lock died with the process; the WAL (if any survived) is recovered by this open.
      auto c = open_ok(d.path);
      if (!c) continue;
      CHECK((favs(*c) == Ids{"committed-fav"}));
      CHECK((recs(*c) == Ids{"committed-r2", "committed-r1"}));
      bool ch = false;  // and it is writable and consistent afterwards
      CHECK(c->add_favorite("after", ch) == CatalogStatus::kOk && ch);
      Raw r(d.sub("catalog.db"));
      CHECK(r.one("PRAGMA integrity_check") == "ok");
    }
  }
}

}  // namespace

int main() {
  test_schema();
  test_version_policy();
  test_favorites();
  test_favorite_limit();
  test_recent();
  test_persistence();
  test_single_writer();
  test_bad_files();
  test_refusal_touches_nothing();
  test_missing_next_seq_and_big_reconcile();
  test_delete_cascade();
  test_fault_injection();
  test_crash_recovery();
  if (g_fail) {
    std::printf("librarian_catalog_test: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("librarian_catalog_test: ok\n");
  return 0;
}
