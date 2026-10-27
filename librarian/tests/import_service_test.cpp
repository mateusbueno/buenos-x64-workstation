// Story 5.6 tests: the libraries catalog, the import API and the atomic publish, through a real Unix socket client. Waits are bounded polls for an event;
// there is no timing assertion anywhere. Fixtures are generated (import_fixtures.hpp), nothing is committed.
//
// Covered: the order of the story (stage -> verify -> fsync -> rename -> flip `current` -> catalog rows) observed from outside at every boundary, the
// category-required refusals with nothing written, duplicate imports (unchanged), a changed source, cancel (running and queued), the bounded queue,
// monotone progress events ending in one terminal state, skipped files with their Story 5.1 reasons, unreadable sources, SF2 per-instrument categories
// and names, list APIs and paging, the single writer (second service, the CLI), concurrent clients, the startup reconcile pass and the catalog
// rebuilt from bundles alone.
#include <sqlite3.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <future>
#include <map>
#include <mutex>
#include <set>

#include "import_fixtures.hpp"
#include "lib_test_util.hpp"
#include "velora/format/bundle.hpp"
#include "velora/librarian/import.hpp"
#include "velora/librarian/library_index.hpp"
#include "velora/librarian/library_reconcile.hpp"
#include "velora/librarian/library_service.hpp"

using namespace velora::librarian;
namespace fs = std::filesystem;
namespace fmt = velora::format;
using ltest::Client;
using ltest::TempDir;

namespace {

constexpr unsigned kPiano = 1, kOrgan = 3, kDrums = 15;
constexpr unsigned kExplicitNone = 9999;  // in Item::cat / Ov::cat: the field is SET to 0 (cat == 0 means the field is not set)

bool terminal(ws::v1::ImportState s) {
  return s == ws::v1::IMPORT_STATE_PUBLISHED || s == ws::v1::IMPORT_STATE_FAILED || s == ws::v1::IMPORT_STATE_CANCELLED;
}

struct Item {
  std::string path;
  unsigned cat = 0;
  std::string name;
  struct Ov {
    std::string key, name;
    unsigned cat = 0;
  };
  std::vector<Ov> ov;
};

ws::v1::LibraryClientMessage import_msg(const std::string& source, const std::vector<Item>& items) {
  ws::v1::LibraryClientMessage m;
  auto* r = m.mutable_import_request();
  r->set_source(source);
  for (const Item& i : items) {
    auto* it = r->add_items();
    it->set_path(i.path);
    if (i.cat == kExplicitNone) it->set_category(0);
    else if (i.cat != 0) it->set_category(i.cat);
    it->set_name(i.name);
    for (const auto& o : i.ov) {
      auto* po = it->add_instruments();
      po->set_key(o.key);
      po->set_name(o.name);
      if (o.cat == kExplicitNone) po->set_category(0);
      else if (o.cat != 0) po->set_category(o.cat);
    }
  }
  return m;
}

// Sends an ImportRequest and returns the job id (0 when refused; `err` receives the refusal).
std::uint64_t submit(Client& c, const std::string& source, const std::vector<Item>& items, ws::v1::LibraryError* err = nullptr) {
  auto r = c.call(import_msg(source, items));
  if (!r) return 0;
  if (r->has_import_accepted()) return r->import_accepted().job_id();
  if (err && r->has_error()) *err = r->error();
  return 0;
}

// All events of `job` up to and including the terminal one (events that arrived before are taken from c.pushes; other messages stay in c.pushes).
std::vector<ws::v1::ImportProgress> collect(Client& c, std::uint64_t job, int timeout_ms = 30000) {
  std::vector<ws::v1::ImportProgress> ev;
  std::deque<ws::v1::LibraryServerMessage> keep;
  auto take = [&](const ws::v1::LibraryServerMessage& m) {
    if (m.has_import_progress() && m.import_progress().job_id() == job) ev.push_back(m.import_progress());
    else keep.push_back(m);
  };
  for (const auto& m : c.pushes) take(m);
  c.pushes.clear();
  while (ev.empty() || !terminal(ev.back().state())) {
    auto r = c.recv(timeout_ms);
    if (!r) break;
    take(*r);
  }
  for (auto& m : keep) c.pushes.push_back(std::move(m));
  return ev;
}

std::optional<ws::v1::LibraryServerMessage> list_libs(Client& c, std::uint32_t off = 0, std::uint32_t limit = 0) {
  ws::v1::LibraryClientMessage m;
  m.mutable_list_libraries()->set_offset(off);
  m.mutable_list_libraries()->set_limit(limit);
  return c.call(m);
}
std::optional<ws::v1::LibraryServerMessage> list_inst(Client& c, const std::string& id, std::uint32_t off = 0, std::uint32_t limit = 0) {
  ws::v1::LibraryClientMessage m;
  m.mutable_list_instruments()->set_library_id(id);
  m.mutable_list_instruments()->set_offset(off);
  m.mutable_list_instruments()->set_limit(limit);
  return c.call(m);
}
std::optional<ws::v1::LibraryServerMessage> cancel(Client& c, std::uint64_t job) {
  ws::v1::LibraryClientMessage m;
  m.mutable_cancel_import()->set_job_id(job);
  return c.call(m);
}

bool is_error(const std::optional<ws::v1::LibraryServerMessage>& r, ws::v1::LibraryErrorCode code) {
  return r && r->has_error() && r->error().code() == code;
}

// The service on a fresh data directory and a short socket path.
struct Svc {
  TempDir td;
  LibraryServiceOptions o;
  std::unique_ptr<LibraryService> s;
  std::string err;
  CatalogStatus st = CatalogStatus::kOk;
  explicit Svc(const std::function<void(LibraryServiceOptions&)>& cfg = {}, const std::string& data = {}) {
    o.data_dir = data.empty() ? td.sub("data") : data;
    if (data.empty()) fs::create_directories(o.data_dir);
    o.socket_path = td.sub("s.sock");
    if (cfg) cfg(o);
  }
  bool start() {
    s = std::make_unique<LibraryService>(o);
    return s->start(err, &st);
  }
  void stop() {
    if (s) s->stop();
    s.reset();
  }
  std::string data() const { return o.data_dir; }
  std::string lib() const { return o.data_dir + "/lib"; }
  bool connect(Client& c) {
    return c.connect(o.socket_path) && c.handshake("import-test");
  }
};

std::set<std::string> names_in(const fs::path& d) {
  std::set<std::string> s;
  std::error_code ec;
  for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) s.insert(it->path().filename().string());
  return s;
}

// Number of libraries / instruments in the catalog, through a separate read-only connection (the WAL lets it read while the service writes).
long count_rows(const std::string& data, const char* table) {
  sqlite3* h = nullptr;
  long n = -1;
  if (sqlite3_open_v2((data + "/catalog.db").c_str(), &h, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
    sqlite3_busy_timeout(h, 2000);
    sqlite3_stmt* q = nullptr;
    if (sqlite3_prepare_v2(h, (std::string("SELECT count(*) FROM ") + table).c_str(), -1, &q, nullptr) == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW)
      n = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
  }
  if (h) sqlite3_close(h);
  return n;
}

struct LibDirCounts {
  int bundles = 0, currents = 0, libs = 0;
};
LibDirCounts count_lib_dir(const std::string& lib) {
  LibDirCounts c;
  std::error_code ec;
  for (fs::directory_iterator it(lib, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string n = it->path().filename().string();
    if (n.size() != 36) continue;
    ++c.libs;
    std::error_code e2;
    if (fs::is_symlink(it->path() / "current", e2)) ++c.currents;
    for (fs::directory_iterator b(it->path(), e2), bend; !e2 && b != bend; b.increment(e2))
      if (b->path().filename().string().size() == 64 && fs::is_directory(b->path(), e2)) ++c.bundles;
  }
  return c;
}

bool nothing_written(const std::string& data) {
  auto top = names_in(data + "/lib");
  top.erase(kServiceLockFileName);  // the service's own lock file is not a write of an import
  if (!(top.empty() || top == std::set<std::string>{".staging"})) return false;
  return count_rows(data, "libraries") == 0 && count_rows(data, "instruments") == 0;
}

std::string run_cli(const std::string& args, int& status, std::string* err_text = nullptr) {
  TempDir t;
  const std::string errf = t.sub("err.txt");
  const std::string cmd = std::string(LIBRARIAN_EXE) + " " + args + " > " + t.sub("out.txt") + " 2> " + errf;
  const int rc = std::system(cmd.c_str());
  status = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
  if (err_text) *err_text = ltest::slurp(errf);
  return ltest::slurp(t.sub("out.txt"));
}

// --- tests --------------------------------------------------------------------------------------------------------------------------------

void test_order_of_the_story() {
  std::mutex m;
  struct Snap {
    std::string point;
    long db_libs;
    LibDirCounts dir;
  };
  std::vector<Snap> snaps;
  std::string data_for_hook;
  Svc svc([&](LibraryServiceOptions& o) {
    o.import_point_hook = [&](const char* p, std::uint64_t, std::size_t) {
      if (std::string(p).rfind("reconcile.", 0) == 0) return;
      std::lock_guard<std::mutex> lk(m);
      snaps.push_back({p, count_rows(data_for_hook, "libraries"), count_lib_dir(data_for_hook + "/lib")});
    };
  });
  data_for_hook = svc.data();
  CHECK(svc.start());
  TempDir src;
  const fs::path sfz = ifx::write_sfz(src.path, "Piano");
  Client c;
  CHECK(svc.connect(c));
  ws::v1::LibraryError e;
  const auto job = submit(c, sfz.string(), {{"", kPiano, "", {}}}, &e);
  CHECK(job == 1);
  const auto ev = collect(c, job);
  CHECK(!ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_PUBLISHED);
  {
    std::lock_guard<std::mutex> lk(m);
    std::vector<std::string> names;
    for (const auto& s : snaps) names.push_back(s.point);
    const std::vector<std::string> want = {"stage-created", "samples-written", "verified",    "fsynced",
                                           "renamed",       "link-created",    "link-flipped", "catalog-row-written",
                                           "catalog-committed", "job-finished"};
    CHECK(names == want);
    for (const auto& s : snaps) {
      if (s.point == "stage-created" || s.point == "samples-written" || s.point == "verified" || s.point == "fsynced")
        CHECK(s.dir.bundles == 0 && s.dir.currents == 0 && s.db_libs == 0);  // nothing published yet
      if (s.point == "renamed") CHECK(s.dir.bundles == 1 && s.dir.currents == 0 && s.db_libs == 0);  // in place, `current` not flipped
      if (s.point == "link-created") CHECK(s.dir.bundles == 1 && s.dir.currents == 0 && s.db_libs == 0);
      if (s.point == "link-flipped") CHECK(s.dir.bundles == 1 && s.dir.currents == 1 && s.db_libs == 0);  // flipped, still no catalog row
      if (s.point == "catalog-row-written") CHECK(s.dir.currents == 1 && s.db_libs == 0);               // inside the transaction: not visible yet
      if (s.point == "catalog-committed" || s.point == "job-finished") CHECK(s.db_libs == 1);
    }
  }
  // The events: QUEUED first, then RUNNING ..., one item result IMPORTED, one terminal event last.
  CHECK(ev.front().state() == ws::v1::IMPORT_STATE_QUEUED && ev.front().event_seq() == 1);
  int item_events = 0, terminals = 0;
  for (const auto& p : ev) {
    if (p.has_item()) {
      ++item_events;
      CHECK(p.item().status() == ws::v1::IMPORT_ITEM_IMPORTED && p.item().instruments() == 1 && p.item().samples() == 1);
      CHECK(p.item().path() == fs::canonical(sfz).string());
    }
    if (terminal(p.state())) ++terminals;
  }
  CHECK(item_events == 1 && terminals == 1);
  // Lists.
  auto l = list_libs(c);
  CHECK(l && l->has_library_list() && l->library_list().libraries_size() == 1 && l->library_list().total() == 1 && !l->library_list().more());
  if (l && l->library_list().libraries_size() == 1) {
    const auto& li = l->library_list().libraries(0);
    CHECK(li.name() == "Piano" && li.source_kind() == "sfz" && li.instrument_count() == 1 && li.imported_seq() > 0);
    CHECK(li.source_path() == fs::canonical(sfz).string());
    auto in = list_inst(c, li.library_id());
    CHECK(in && in->has_instrument_list() && in->instrument_list().instruments_size() == 1);
    if (in && in->instrument_list().instruments_size() == 1) {
      const auto& ii = in->instrument_list().instruments(0);
      CHECK(ii.instrument_key() == "Piano.sfz" && ii.name() == "Piano" && ii.category() == kPiano && ii.sample_count() == 1 && ii.region_count() == 1);
      CHECK(ii.bytes() == 300 * 2);
    }
    // The published bundle verifies, `current` names it, catalog.meta sits next to it.
    const std::string cur = svc.lib() + "/" + li.library_id() + "/current";
    auto rd = fmt::BundleReader::open_file(cur + "/bundle.vlb");
    CHECK(rd.ok() && rd.value().verify_all() == fmt::Error::kNone && fmt::to_hex(rd.value().bundle_id()) == li.bundle_id());
    CatalogMeta meta;
    CHECK(read_catalog_meta(cur, meta) == MetaState::kValid && meta.instruments.size() == 1 && meta.instruments[0].category == fmt::Category::kPiano);
    CHECK(meta.library_name == "Piano" && meta.source_kind == "sfz");
  }
  CHECK(names_in(svc.lib() + "/.staging") == std::set<std::string>{".lock"});
}

void test_category_required_writes_nothing() {
  Svc svc;
  CHECK(svc.start());
  TempDir src;
  const fs::path a = ifx::write_sfz(src.path, "A"), b = ifx::write_sfz(src.path, "B");
  const fs::path gm = ifx::write_sf2(fs::path(src.path) / "gm.sf2");
  Client c;
  CHECK(svc.connect(c));
  auto expect_required = [&](const char* what, const std::string& source, const std::vector<Item>& items, const char* reason) {
    ws::v1::LibraryError e;
    const auto job = submit(c, source, items, &e);
    if (job != 0 || e.code() != ws::v1::LIBRARY_ERROR_CODE_CATEGORY_REQUIRED || e.details().at("reason") != reason) {
      std::printf("FAIL category required: %s (job %llu, code %d, reason %s)\n", what, (unsigned long long)job, int(e.code()),
                  e.details().count("reason") ? e.details().at("reason").c_str() : "?");
      ++g_fail;
    }
    CHECK(nothing_written(svc.data()));
    CHECK(c.pushes.empty());  // not even a QUEUED event
  };
  expect_required("category not set", a.string(), {{"", 0, "", {}}}, "missing");
  expect_required("category set to 0 (none)", a.string(), {{"", kExplicitNone, "", {}}}, "none");
  expect_required("an override set to none", gm.string(), {{"", kPiano, "", {{"0:0", "", kExplicitNone}}}}, "none");
  expect_required("unknown category 99", a.string(), {{"", 99, "", {}}}, "unknown");
  expect_required("category 18 (first id beyond the table)", a.string(), {{"", 18, "", {}}}, "unknown");
  expect_required("no items", a.string(), {}, "missing");
  expect_required("one of two items lacks a category", src.path, {{"A.sfz", kPiano, "", {}}, {"B.sfz", 0, "", {}}}, "missing");
  expect_required("an override with an unknown category", gm.string(), {{"", kPiano, "", {{"0:0", "", 99}}}}, "unknown");
  CHECK(svc.s->accepted() >= 1);
  // The job counter did not move: the next accepted job is still job 1, and it was never created before.
  const auto job = submit(c, a.string(), {{"", kPiano, "", {}}});
  CHECK(job == 1);
  collect(c, job);
  (void)b;
}

void test_duplicate_changed_and_metadata_refresh() {
  Svc svc;
  CHECK(svc.start());
  TempDir src;
  const fs::path sfz = ifx::write_sfz(src.path, "Piano");
  Client c;
  CHECK(svc.connect(c));
  auto run = [&](unsigned cat, const std::string& name) {
    const auto job = submit(c, sfz.string(), {{"", cat, name, {}}});
    return collect(c, job);
  };
  auto first = run(kPiano, "");
  CHECK(!first.empty() && first.back().state() == ws::v1::IMPORT_STATE_PUBLISHED && first.back().items_imported() == 1);
  auto libs1 = list_libs(c);
  CHECK(libs1 && libs1->library_list().libraries_size() == 1);
  const auto info1 = libs1->library_list().libraries(0);
  // The same source again: unchanged, nothing new anywhere.
  const auto dir_before = count_lib_dir(svc.lib());
  auto second = run(kPiano, "");
  CHECK(!second.empty() && second.back().state() == ws::v1::IMPORT_STATE_PUBLISHED && second.back().items_unchanged() == 1 && second.back().items_imported() == 0);
  bool saw_unchanged = false;
  for (const auto& p : second) saw_unchanged |= p.has_item() && p.item().status() == ws::v1::IMPORT_ITEM_UNCHANGED;
  CHECK(saw_unchanged);
  const auto dir_after = count_lib_dir(svc.lib());
  CHECK(dir_before.bundles == dir_after.bundles && dir_after.bundles == 1 && dir_after.libs == 1);
  auto libs2 = list_libs(c);
  CHECK(libs2 && libs2->library_list().libraries_size() == 1);
  CHECK(libs2->library_list().libraries(0).imported_seq() == info1.imported_seq() && libs2->library_list().libraries(0).bundle_id() == info1.bundle_id());
  // The same bundle with another category and name: the metadata is refreshed (still "unchanged" as a bundle) and the catalog follows.
  auto third = run(kOrgan, "Church Organ");
  CHECK(!third.empty() && third.back().items_unchanged() == 1);
  auto libs3 = list_libs(c);
  CHECK(libs3 && libs3->library_list().libraries_size() == 1 && libs3->library_list().libraries(0).name() == "Church Organ");
  CHECK(libs3->library_list().libraries(0).bundle_id() == info1.bundle_id());
  auto in3 = list_inst(c, info1.library_id());
  CHECK(in3 && in3->instrument_list().instruments_size() == 1 && in3->instrument_list().instruments(0).category() == kOrgan &&
        in3->instrument_list().instruments(0).name() == "Church Organ");
  // A changed source: same library, a new bundle, one catalog row.
  ifx::write_sfz(src.path, "Piano", 1);
  auto fourth = run(kOrgan, "Church Organ");
  CHECK(!fourth.empty() && fourth.back().items_imported() == 1);
  auto libs4 = list_libs(c);
  CHECK(libs4 && libs4->library_list().libraries_size() == 1 && libs4->library_list().libraries(0).library_id() == info1.library_id());
  CHECK(libs4->library_list().libraries(0).bundle_id() != info1.bundle_id() && libs4->library_list().libraries(0).imported_seq() > info1.imported_seq());
  CHECK(count_rows(svc.data(), "instruments") == 1);
  CHECK(count_lib_dir(svc.lib()).bundles == 2);  // the old bundle stays (leases, Story 5.8)
}

void test_progress_skips_and_reasons() {
  Svc svc;
  CHECK(svc.start());
  TempDir src;
  const fs::path dir = fs::path(src.path) / "pack";
  ifx::write_sfz(dir, "Good1");
  ifx::write_sfz(dir / "sub", "Good2");
  ifx::write_unsupported_sfz(dir, "Fancy");
  ifx::write_damaged_sfz(dir, "Broken");
  ifx::write_sf2(dir / "gm.sf2");
  Client c;
  CHECK(svc.connect(c));
  std::vector<Item> items = {{"Good1.sfz", kPiano, "", {}},      {"sub/Good2.sfz", kOrgan, "", {}}, {"Fancy.sfz", kPiano, "", {}},
                             {"Broken.sfz", kPiano, "", {}},     {"gm.sf2", kPiano, "", {}}};
  const auto job = submit(c, dir.string(), items);
  CHECK(job != 0);
  const auto ev = collect(c, job);
  CHECK(ev.size() >= 7);
  // Monotone: the sequence grows by one, the counters never decrease, bytes_done reaches bytes_total, exactly one terminal event and it is last.
  std::uint64_t seq = 0, bytes = 0;
  std::uint32_t done = 0, imp = 0, sk = 0;
  int terminals = 0;
  std::map<std::string, ws::v1::ImportItemResult> by_name;
  for (const auto& p : ev) {
    CHECK(p.event_seq() == seq + 1);
    seq = p.event_seq();
    CHECK(p.items_total() == 5 && p.items_done() >= done && p.bytes_done() >= bytes && p.items_imported() >= imp && p.items_skipped() >= sk);
    done = p.items_done();
    bytes = p.bytes_done();
    imp = p.items_imported();
    sk = p.items_skipped();
    CHECK(p.items_done() == p.items_imported() + p.items_unchanged() + p.items_skipped() + p.items_failed());
    if (terminal(p.state())) ++terminals;
    if (p.has_item()) by_name[fs::path(p.item().path()).filename().string()] = p.item();
  }
  CHECK(terminals == 1 && terminal(ev.back().state()) && ev.back().current_item().empty());
  CHECK(ev.back().state() == ws::v1::IMPORT_STATE_PUBLISHED && ev.back().items_done() == 5 && ev.back().items_imported() == 3 && ev.back().items_skipped() == 2);
  CHECK(ev.back().bytes_done() == ev.back().bytes_total() && ev.back().bytes_total() > 0);
  // The Story 5.1 reasons per item.
  CHECK(by_name.size() == 5);
  CHECK(by_name["Good1.sfz"].status() == ws::v1::IMPORT_ITEM_IMPORTED && by_name["Good1.sfz"].reason() == ws::v1::IMPORT_REASON_NONE);
  CHECK(by_name["Good2.sfz"].status() == ws::v1::IMPORT_ITEM_IMPORTED);
  CHECK(by_name["gm.sf2"].status() == ws::v1::IMPORT_ITEM_IMPORTED && by_name["gm.sf2"].instruments() == 6);
  const auto& fancy = by_name["Fancy.sfz"];
  CHECK(fancy.status() == ws::v1::IMPORT_ITEM_SKIPPED && fancy.reason() == ws::v1::IMPORT_REASON_UNSUPPORTED_FEATURES && fancy.offenders_size() == 1);
  if (fancy.offenders_size() == 1) CHECK(fancy.offenders(0).opcode() == "amp_random" && fancy.offenders(0).line() == 1 && !fancy.offenders(0).file().empty());
  const auto& broken = by_name["Broken.sfz"];
  CHECK(broken.status() == ws::v1::IMPORT_ITEM_SKIPPED && broken.reason() == ws::v1::IMPORT_REASON_DAMAGED && !broken.failing_path().empty() && broken.messages_size() > 0);
  CHECK(fancy.library_id().empty() && broken.library_id().empty());
  // Only the three good ones are libraries; nothing of the skipped files exists on disk.
  auto l = list_libs(c);
  CHECK(l && l->library_list().total() == 3);
  const auto counts = count_lib_dir(svc.lib());
  CHECK(counts.libs == 3 && counts.currents == 3 && counts.bundles == 3);
  CHECK(names_in(svc.lib() + "/.staging") == std::set<std::string>{".lock"});
  // ImportStatus answers the last event.
  ws::v1::LibraryClientMessage sm;
  sm.mutable_import_status()->set_job_id(job);
  auto st = c.call(sm);
  CHECK(st && st->has_import_progress() && st->import_progress().state() == ws::v1::IMPORT_STATE_PUBLISHED && st->import_progress().event_seq() == seq);
  sm.mutable_import_status()->set_job_id(999);
  CHECK(is_error(c.call(sm), ws::v1::LIBRARY_ERROR_CODE_UNKNOWN_JOB));
}

void test_sf2_instrument_categories_and_paging() {
  Svc svc;
  CHECK(svc.start());
  TempDir src;
  const fs::path gm = ifx::write_sf2(fs::path(src.path) / "gm.sf2");
  Client c;
  CHECK(svc.connect(c));
  Item it{"", kPiano, "My GM", {{"128:0", "Standard Kit", kDrums}, {"0:48", "", kOrgan}}};
  const auto job = submit(c, gm.string(), {it});
  const auto ev = collect(c, job);
  CHECK(!ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_PUBLISHED);
  auto l = list_libs(c);
  CHECK(l && l->library_list().libraries_size() == 1);
  const auto li = l->library_list().libraries(0);
  CHECK(li.name() == "My GM" && li.source_kind() == "sf2" && li.instrument_count() == 6);
  auto all = list_inst(c, li.library_id());
  CHECK(all && all->instrument_list().instruments_size() == 6 && all->instrument_list().total() == 6 && !all->instrument_list().more());
  std::map<std::string, ws::v1::InstrumentInfo> by_key;
  std::vector<std::string> in_order;
  for (const auto& i : all->instrument_list().instruments()) {
    by_key[i.instrument_key()] = i;
    in_order.push_back(i.instrument_key());
  }
  CHECK(std::is_sorted(in_order.begin(), in_order.end()));  // documented order: by instrument_key (the manifest order is 0:0 0:1 0:48 8:0 128:0 1:5)
  CHECK(by_key.size() == 6);
  CHECK(by_key["0:0"].category() == kPiano && by_key["0:1"].category() == kPiano && by_key["8:0"].category() == kPiano && by_key["1:5"].category() == kPiano);
  CHECK(by_key["128:0"].category() == kDrums && by_key["128:0"].name() == "Standard Kit");
  CHECK(by_key["0:48"].category() == kOrgan && !by_key["0:48"].name().empty() && by_key["0:48"].name() != "Standard Kit");
  for (const auto& kv : by_key) CHECK(kv.second.category() >= 1 && kv.second.category() <= 17 && kv.second.region_count() > 0 && kv.second.bytes() > 0);
  // Paging: 2 + 2 + 2, `more` until the last page, offset beyond the end is empty.
  std::set<std::string> seen;
  std::uint32_t off = 0;
  int pages = 0;
  for (;; ++pages) {
    auto pg = list_inst(c, li.library_id(), off, 2);
    CHECK(pg && pg->has_instrument_list() && pg->instrument_list().total() == 6 && pg->instrument_list().offset() == off);
    if (!pg) break;
    for (const auto& i : pg->instrument_list().instruments()) seen.insert(i.instrument_key());
    off += static_cast<std::uint32_t>(pg->instrument_list().instruments_size());
    if (!pg->instrument_list().more()) break;
    if (pages > 5) break;
  }
  CHECK(pages == 2 && seen.size() == 6 && off == 6);
  auto beyond = list_inst(c, li.library_id(), 100, 2);
  CHECK(beyond && beyond->instrument_list().instruments_size() == 0 && !beyond->instrument_list().more());
  CHECK(is_error(list_inst(c, "00000000-0000-4000-8000-000000000000"), ws::v1::LIBRARY_ERROR_CODE_UNKNOWN_LIBRARY));
  auto pg1 = list_libs(c, 5, 10);
  CHECK(pg1 && pg1->library_list().libraries_size() == 0 && pg1->library_list().total() == 1);
  // The same SF2 with a different category for one preset only: the catalog and catalog.meta follow, the bundle bytes do not change.
  Item it2{"", kPiano, "My GM", {{"128:0", "Standard Kit", kPiano}}};
  const auto job2 = submit(c, gm.string(), {it2});
  const auto ev2 = collect(c, job2);
  CHECK(!ev2.empty() && ev2.back().items_unchanged() == 1);
  auto all2 = list_inst(c, li.library_id());
  for (const auto& i : all2->instrument_list().instruments()) CHECK(i.category() == kPiano);
}

// A gate that blocks the worker at a point of a given job until released, so cancel and queue tests need no timing.
struct Gate {
  std::promise<void> arrived_p, release_p;
  std::shared_future<void> release = release_p.get_future().share();
  std::atomic<bool> hit{false};
  std::future<void> arrived = arrived_p.get_future();
  void reach() {
    if (hit.exchange(true)) return;
    arrived_p.set_value();
    release.wait();
  }
  void open() {
    try {
      release_p.set_value();
    } catch (...) {
    }
  }
};

void test_cancel_and_queue() {
  Gate gate;
  Svc svc([&](LibraryServiceOptions& o) {
    o.max_queued_jobs = 2;
    o.import_point_hook = [&](const char* p, std::uint64_t job, std::size_t) {
      if (job == 1 && std::string(p) == "stage-created") gate.reach();
    };
  });
  CHECK(svc.start());
  TempDir src;
  const fs::path a = ifx::write_sfz(src.path, "A"), b = ifx::write_sfz(src.path, "B"), cc = ifx::write_sfz(src.path, "C"), d = ifx::write_sfz(src.path, "D");
  Client c;
  CHECK(svc.connect(c));
  const auto j1 = submit(c, a.string(), {{"", kPiano, "", {}}});
  CHECK(j1 == 1);
  CHECK(gate.arrived.wait_for(std::chrono::seconds(20)) == std::future_status::ready);  // the worker is inside job 1, before anything was written
  const auto j2 = submit(c, b.string(), {{"", kPiano, "", {}}});
  const auto j3 = submit(c, cc.string(), {{"", kPiano, "", {}}});
  CHECK(j2 == 2 && j3 == 3);
  ws::v1::LibraryError e;
  CHECK(submit(c, d.string(), {{"", kPiano, "", {}}}, &e) == 0 && e.code() == ws::v1::LIBRARY_ERROR_CODE_QUEUE_FULL && e.details().at("limit") == "2");
  // Cancel the queued job 2: it ends at once and never runs.
  auto ack = cancel(c, j2);
  CHECK(ack && ack->has_ack() && ack->ack().changed());
  {
    const auto ev2 = collect(c, j2);
    CHECK(!ev2.empty() && ev2.back().state() == ws::v1::IMPORT_STATE_CANCELLED && ev2.back().items_done() == 0);
  }
  // Room in the queue again (job 2 left it).
  const auto j4 = submit(c, d.string(), {{"", kPiano, "", {}}});
  CHECK(j4 == 4);
  // Cancel the running job 1 while it is blocked, then let it go: it stops before anything is renamed into place.
  ack = cancel(c, j1);
  CHECK(ack && ack->ack().changed());
  gate.open();
  const auto ev1 = collect(c, j1);
  CHECK(!ev1.empty() && ev1.back().state() == ws::v1::IMPORT_STATE_CANCELLED && ev1.back().items_imported() == 0);
  bool item_cancelled = false;
  for (const auto& p : ev1) item_cancelled |= p.has_item() && p.item().status() == ws::v1::IMPORT_ITEM_CANCELLED;
  CHECK(item_cancelled);
  // Jobs 3 and 4 run normally afterwards.
  const auto ev3 = collect(c, j3), ev4 = collect(c, j4);
  CHECK(!ev3.empty() && ev3.back().state() == ws::v1::IMPORT_STATE_PUBLISHED && !ev4.empty() && ev4.back().state() == ws::v1::IMPORT_STATE_PUBLISHED);
  auto l = list_libs(c);
  CHECK(l && l->library_list().total() == 2);  // C and D only: A was cancelled mid-flight, B was cancelled in the queue
  const auto counts = count_lib_dir(svc.lib());
  CHECK(counts.libs == 2 && counts.bundles == 2 && counts.currents == 2);
  CHECK(names_in(svc.lib() + "/.staging") == std::set<std::string>{".lock"});  // the aborted staging directory is gone
  // A finished job cannot be cancelled; an unknown one is an error.
  ack = cancel(c, j3);
  CHECK(ack && ack->has_ack() && !ack->ack().changed());
  CHECK(is_error(cancel(c, 12345), ws::v1::LIBRARY_ERROR_CODE_UNKNOWN_JOB));
  gate.open();
}

void test_unreadable_and_invalid_sources() {
  Svc svc;
  CHECK(svc.start());
  TempDir src;
  const fs::path a = ifx::write_sfz(src.path, "A");
  const fs::path nested = fs::path(src.path) / "folder";
  ifx::write_sfz(nested, "N");
  ifx::put(fs::path(src.path) / "notes.txt", "x");
  Client c;
  CHECK(svc.connect(c));
  auto expect_invalid = [&](const char* what, const std::string& source, const std::vector<Item>& items, const char* reason) {
    ws::v1::LibraryError e;
    const auto job = submit(c, source, items, &e);
    if (job != 0 || e.code() != ws::v1::LIBRARY_ERROR_CODE_INVALID_SOURCE || e.details().at("reason") != reason) {
      std::printf("FAIL invalid source: %s (job %llu, code %d, reason %s)\n", what, (unsigned long long)job, int(e.code()),
                  e.details().count("reason") ? e.details().at("reason").c_str() : "?");
      ++g_fail;
    }
    CHECK(nothing_written(svc.data()));
  };
  expect_invalid("missing source", src.path + "/nope.sfz", {{"", kPiano, "", {}}}, "source_unreadable");
  expect_invalid("empty source", "", {{"", kPiano, "", {}}}, "source_missing");
  expect_invalid("not an SFZ/SF2", src.path + "/notes.txt", {{"", kPiano, "", {}}}, "not_importable");
  expect_invalid("an item that does not exist", src.path + "/folder", {{"Nope.sfz", kPiano, "", {}}}, "item_unreadable");
  expect_invalid("an item outside the folder", src.path + "/folder", {{a.string(), kPiano, "", {}}}, "outside_source");
  expect_invalid("an item that is not importable", src.path, {{"notes.txt", kPiano, "", {}}}, "not_importable");
  expect_invalid("the same item twice", src.path + "/folder", {{"N.sfz", kPiano, "", {}}, {(nested / "N.sfz").string(), kPiano, "", {}}}, "duplicate_item");
  expect_invalid("a folder item without a path", src.path + "/folder", {{"", kPiano, "", {}}}, "item_path_missing");
  expect_invalid("an item that is not the file source", a.string(), {{(nested / "N.sfz").string(), kPiano, "", {}}}, "outside_source");
  if (::geteuid() != 0) {  // permissions mean nothing to root
    ::chmod((nested / "N.sfz").c_str(), 0);
    expect_invalid("an unreadable file", src.path + "/folder", {{"N.sfz", kPiano, "", {}}}, "item_unreadable");
    ::chmod((nested / "N.sfz").c_str(), 0644);
  }
  {  // invalid names
    ws::v1::LibraryError e;
    CHECK(submit(c, a.string(), {{"", kPiano, std::string("bad\nname"), {}}}, &e) == 0 && e.code() == ws::v1::LIBRARY_ERROR_CODE_INVALID_COMMAND);
    CHECK(nothing_written(svc.data()));
  }
  // A relative item path inside a folder is fine, and the damaged-sample file inside is a per-item skip, not a refusal.
  ifx::write_damaged_sfz(nested, "Bad");
  const auto job = submit(c, nested.string(), {{"N.sfz", kPiano, "", {}}, {"Bad.sfz", kPiano, "", {}}});
  CHECK(job != 0);
  const auto ev = collect(c, job);
  CHECK(!ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_PUBLISHED && ev.back().items_skipped() == 1 && ev.back().items_imported() == 1);
  // A source whose only item is skipped ends FAILED (nothing reached the catalog) and leaves nothing behind.
  const auto lone = submit(c, (nested / "Bad.sfz").string(), {{"", kPiano, "", {}}});
  const auto ev2 = collect(c, lone);
  CHECK(!ev2.empty() && ev2.back().state() == ws::v1::IMPORT_STATE_FAILED && ev2.back().items_skipped() == 1);
  CHECK(count_lib_dir(svc.lib()).libs == 1);
}

void test_single_writer() {
  Svc svc;
  CHECK(svc.start());
  // A second service on the same data directory is refused (catalog lock) and the first keeps working.
  {
    Svc other([&](LibraryServiceOptions& o) { o.socket_path = o.socket_path + "2"; }, svc.data());
    CHECK(!other.start() && other.st == CatalogStatus::kBusy);
  }
  Client c;
  CHECK(svc.connect(c));
  TempDir src;
  const fs::path sfz = ifx::write_sfz(src.path, "Piano");
  // The CLI refuses while the service runs and writes nothing.
  int status = 0;
  std::string err;
  run_cli("import " + sfz.string() + " --root " + svc.data() + " --category piano", status, &err);
  CHECK(status == 1);
  CHECK(err.find("librarian service is running") != std::string::npos);
  CHECK(names_in(svc.lib()) == std::set<std::string>{kServiceLockFileName});  // the CLI wrote nothing
  // The same import through the API works.
  const auto job = submit(c, sfz.string(), {{"", kPiano, "", {}}});
  const auto ev = collect(c, job);
  CHECK(!ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_PUBLISHED);
  svc.stop();
  // Stopped: the CLI works standalone again (the lock file stays, unlocked) and its library is picked up by the next start.
  ifx::write_sfz(src.path, "Other");
  run_cli("import " + (fs::path(src.path) / "Other.sfz").string() + " --root " + svc.data() + " --category organ", status, &err);
  CHECK(status == 0);
  CHECK(svc.start());
  Client c2;
  CHECK(svc.connect(c2));
  auto l = list_libs(c2);
  CHECK(l && l->library_list().total() == 2);
  CHECK(svc.s->startup_reconcile().rows_added == 1);  // "Other" came from the CLI: cataloged by the startup pass from its legacy category file
  svc.stop();
  // Another writer holding lib/.service.lock (a CLI import in progress) keeps the service from starting, and releases the catalog lock again.
  {
    const int fd = ::open((svc.lib() + "/" + kServiceLockFileName).c_str(), O_RDONLY);
    CHECK(fd >= 0 && ::flock(fd, LOCK_SH | LOCK_NB) == 0);
    CHECK(!svc.start() && svc.st == CatalogStatus::kBusy && svc.err.find(kServiceLockFileName) != std::string::npos);
    ::close(fd);
    std::unique_ptr<Catalog> cat;
    CHECK(Catalog::open(svc.data(), cat) == CatalogStatus::kOk);  // the failed start left no catalog lock behind
  }
  CHECK(svc.start());
}

void test_concurrent_clients() {
  Svc svc;
  CHECK(svc.start());
  TempDir src;
  const fs::path dir = fs::path(src.path) / "pack";
  std::vector<Item> items;
  for (int i = 0; i < 6; ++i) {
    ifx::write_sfz(dir, "Lib" + std::to_string(i));
    items.push_back({"Lib" + std::to_string(i) + ".sfz", kPiano, "", {}});
  }
  Client importer;
  CHECK(svc.connect(importer));
  std::atomic<bool> stop{false};
  std::atomic<int> bad{0}, replies{0};
  auto reader = [&] {
    Client r;
    if (!svc.connect(r)) {
      ++bad;
      return;
    }
    std::uint32_t last_total = 0;
    while (!stop.load()) {
      auto l = list_libs(r);
      if (!l || !l->has_library_list()) {
        ++bad;
        return;
      }
      ++replies;
      const auto& ll = l->library_list();
      if (ll.total() < last_total) ++bad;  // libraries never disappear while importing
      last_total = ll.total();
      for (const auto& li : ll.libraries()) {
        auto in = list_inst(r, li.library_id());  // a listed library always has its instruments: no partial library is visible
        if (!in || !in->has_instrument_list() || in->instrument_list().instruments_size() != static_cast<int>(li.instrument_count()) ||
            li.instrument_count() != 1)
          ++bad;
      }
    }
  };
  std::vector<std::thread> readers;
  for (int i = 0; i < 3; ++i) readers.emplace_back(reader);
  const auto job = submit(importer, dir.string(), items);
  const auto ev = collect(importer, job);
  CHECK(!ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_PUBLISHED && ev.back().items_imported() == 6);
  // Readers must have seen the final state at least once after the job ended.
  const int before = replies.load();
  CHECK(ltest::wait_until([&] { return replies.load() >= before + 6; }));
  stop.store(true);
  for (auto& t : readers) t.join();
  CHECK(bad.load() == 0 && replies.load() > 0);
  auto l = list_libs(importer);
  CHECK(l && l->library_list().total() == 6);
}

void test_startup_reconcile_and_rebuild_from_bundles() {
  TempDir src;
  const fs::path dir = fs::path(src.path) / "pack";
  ifx::write_sfz(dir, "P1");
  ifx::write_sfz(dir, "P2");
  const fs::path gm = ifx::write_sf2(fs::path(dir) / "gm.sf2");
  Svc svc;
  CHECK(svc.start());
  std::map<std::string, LibraryRecord> before;
  {
    Client c;
    CHECK(svc.connect(c));
    Item it_gm{"gm.sf2", kPiano, "General", {{"128:0", "Kit", kDrums}}};
    const auto job = submit(c, dir.string(), {{"P1.sfz", kPiano, "First", {}}, {"P2.sfz", kOrgan, "", {}}, it_gm});
    const auto ev = collect(c, job);
    CHECK(!ev.empty() && ev.back().items_imported() == 3);
  }
  svc.stop();
  auto snapshot = [&](std::map<std::string, LibraryRecord>& out) {
    std::unique_ptr<Catalog> cat;
    CHECK(Catalog::open(svc.data(), cat) == CatalogStatus::kOk);
    std::vector<LibraryRecord> rows;
    CHECK(cat && cat->list_libraries(rows, true) == CatalogStatus::kOk);
    out.clear();
    for (auto& r : rows) out[r.library_id] = r;
  };
  snapshot(before);
  CHECK(before.size() == 3);
  // 1. The catalog is gone altogether: the next start rebuilds it from the bundles alone (names, categories, kinds, sources, instrument rows).
  fs::remove(svc.data() + "/catalog.db");
  fs::remove(svc.data() + "/catalog.db-wal");
  fs::remove(svc.data() + "/catalog.db-shm");
  CHECK(svc.start());
  CHECK(svc.s->startup_reconcile().rows_added == 3 && svc.s->startup_reconcile().skipped.empty());
  svc.stop();
  std::map<std::string, LibraryRecord> after;
  snapshot(after);
  CHECK(after.size() == 3);
  for (const auto& kv : before) {
    CHECK(after.count(kv.first) == 1);
    if (after.count(kv.first)) CHECK(same_content(kv.second, after[kv.first]));
  }
  // 2. Leftovers: a staging directory, an orphan `.current.tmp.*` link, a row whose `current` is gone, a row for a library whose directory is gone.
  fs::create_directories(svc.lib() + "/.staging/12345-1-1");
  ifx::put(fs::path(svc.lib()) / ".staging/12345-1-1/bundle.vlb", "half a bundle");
  std::string victim, gone;
  {
    int i = 0;
    for (const auto& kv : before) {
      if (i == 0) {
        victim = kv.first;
        fs::remove(svc.lib() + "/" + victim + "/current");  // a library without `current` is not listed (the bundle directory itself stays)
      } else if (i == 1) {
        gone = kv.first;
        fs::remove_all(svc.lib() + "/" + gone);
      } else {
        std::error_code ec;
        fs::create_symlink("0000", svc.lib() + "/" + kv.first + "/.current.tmp.4242", ec);
      }
      ++i;
    }
  }
  CHECK(svc.start());
  const ReconcileReport rep = svc.s->startup_reconcile();
  CHECK(rep.staging_removed == 1 && rep.tmp_links_removed == 1 && rep.rows_removed == 2 && rep.rows_added == 0 && rep.errors.empty());
  {
    Client c;
    CHECK(svc.connect(c));
    auto l = list_libs(c);
    CHECK(l && l->library_list().total() == 1);
    CHECK(is_error(list_inst(c, victim), ws::v1::LIBRARY_ERROR_CODE_UNKNOWN_LIBRARY) && is_error(list_inst(c, gone), ws::v1::LIBRARY_ERROR_CODE_UNKNOWN_LIBRARY));
  }
  svc.stop();
  CHECK(names_in(svc.lib() + "/.staging") == std::set<std::string>{".lock"});
  // 3. Idempotence: the pass over a consistent tree changes nothing and writes nothing.
  CHECK(svc.start());
  CHECK(!svc.s->startup_reconcile().changed());
  svc.stop();
  {
    std::unique_ptr<Catalog> cat;
    CHECK(Catalog::open(svc.data(), cat) == CatalogStatus::kOk);
    const auto writes = cat->total_writes();
    ReconcileReport r1, r2;
    CHECK(reconcile_libraries(*cat, svc.data(), r1) == CatalogStatus::kOk && !r1.changed());
    CHECK(reconcile_libraries(*cat, svc.data(), r2) == CatalogStatus::kOk && !r2.changed() && cat->total_writes() == writes);
    // Put the `current` link back: the pass lists the library again, exactly once.
    ReconcileReport r3, r4;
    {
      std::error_code ec;
      std::string bundle;
      for (const auto& e : fs::directory_iterator(svc.lib() + "/" + victim)) bundle = e.path().filename().string();
      fs::create_symlink(bundle, svc.lib() + "/" + victim + "/current", ec);
      CHECK(!ec);
    }
    CHECK(reconcile_libraries(*cat, svc.data(), r3) == CatalogStatus::kOk && r3.rows_added == 1 && r3.rows_removed == 0);
    CHECK(reconcile_libraries(*cat, svc.data(), r4) == CatalogStatus::kOk && !r4.changed());
    // rebuild_libraries: drop every row, list them again from the bundles: the same content.
    std::vector<LibraryRecord> rows_a, rows_b;
    CHECK(cat->list_libraries(rows_a, true) == CatalogStatus::kOk);
    ReconcileReport rb;
    CHECK(rebuild_libraries(*cat, svc.data(), rb) == CatalogStatus::kOk && rb.rows_added == rows_a.size());
    CHECK(cat->list_libraries(rows_b, true) == CatalogStatus::kOk && rows_a.size() == rows_b.size());
    for (const auto& a : rows_a) {
      bool found = false;
      for (const auto& b : rows_b) found |= a.library_id == b.library_id && same_content(a, b);
      CHECK(found);
    }
  }
}

void test_reconcile_legacy_uncategorised_and_corrupt() {
  TempDir src;
  const fs::path dir = fs::path(src.path) / "pack";
  ifx::write_sfz(dir, "Cat");
  ifx::write_sfz(dir, "NoCat");
  ifx::write_sfz(dir, "Rotten");
  TempDir data;
  fs::create_directories(data.path);
  int status = 0;
  // Epic 1 / CLI style bundles: no catalog.meta. One with the legacy category file, one without any category, one corrupted after the import.
  run_cli("import " + (dir / "Cat.sfz").string() + " --root " + data.path + " --category 'E.Piano'", status);
  CHECK(status == 0);
  run_cli("import " + (dir / "NoCat.sfz").string() + " --root " + data.path, status);
  CHECK(status == 0);
  run_cli("import " + (dir / "Rotten.sfz").string() + " --root " + data.path + " --category piano", status);
  CHECK(status == 0);
  std::string rotten_dir;
  for (const auto& e : fs::directory_iterator(data.path + "/lib"))
    if (e.path().filename().string().size() == 36) {
      const std::string cur = e.path().string() + "/current/bundle.vlb";
      const std::string meta = e.path().string() + "/current/" + kCatalogMetaFileName;
      CHECK(!fs::exists(meta));  // the CLI does not write catalog.meta: its bundles stay what they were
    }
  // Corrupt one bundle's sample data: flip a byte near the end of the file.
  for (const auto& e : fs::directory_iterator(data.path + "/lib")) {
    if (e.path().filename().string().size() != 36) continue;
    auto desc = describe_bundle(fs::canonical(e.path() / "current").string());
    if (desc.status == DescribeStatus::kOk && desc.record.instruments[0].key == "Rotten.sfz") {
      rotten_dir = e.path().string();
      const std::string f = fs::canonical(e.path() / "current").string() + "/bundle.vlb";
      std::fstream io(f, std::ios::in | std::ios::out | std::ios::binary);
      io.seekg(0, std::ios::end);
      const auto size = io.tellg();
      io.seekg(size - std::streamoff(3));
      char b = 0;
      io.read(&b, 1);
      b = static_cast<char>(b ^ 0x5a);
      io.seekp(size - std::streamoff(3));
      io.write(&b, 1);
    }
  }
  CHECK(!rotten_dir.empty());
  std::unique_ptr<Catalog> cat;
  CHECK(Catalog::open(data.path, cat) == CatalogStatus::kOk);
  ReconcileReport rep;
  CHECK(reconcile_libraries(*cat, data.path, rep) == CatalogStatus::kOk);
  CHECK(rep.rows_added == 1 && rep.skipped.size() == 2 && rep.errors.empty());
  std::vector<LibraryRecord> rows;
  CHECK(cat->list_libraries(rows, true) == CatalogStatus::kOk && rows.size() == 1);
  if (rows.size() == 1) {
    CHECK(rows[0].instruments.size() == 1 && rows[0].instruments[0].key == "Cat.sfz" && rows[0].instruments[0].category == 2);  // E.Piano
    CHECK(rows[0].name == "Cat" && rows[0].source_kind == "sfz" && rows[0].source_path.empty());  // legacy: name from the manifest, no source info
  }
  bool saw_uncategorised = false, saw_rotten = false;
  for (const auto& s : rep.skipped) {
    saw_uncategorised |= s.find("no valid category") != std::string::npos;
    saw_rotten |= s.find("verification") != std::string::npos;
  }
  CHECK(saw_uncategorised && saw_rotten);
  // The skipped bundles are still on disk (the pass lists and removes rows, it never deletes bundles).
  CHECK(fs::exists(rotten_dir + "/current"));
  // Idempotent: the same pass again changes nothing (the skipped ones are skipped again).
  ReconcileReport again;
  CHECK(reconcile_libraries(*cat, data.path, again) == CatalogStatus::kOk && !again.changed() && again.skipped.size() == 2);
}

// --- review follow-ups ----------------------------------------------------------------------------------------------------------------------

void test_catalog_dir_split() {
  TempDir cat;
  Svc svc([&](LibraryServiceOptions& o) { o.catalog_dir = cat.path; });
  CHECK(svc.start());
  TempDir src;
  const fs::path sfz = ifx::write_sfz(src.path, "Piano");
  Client c;
  CHECK(svc.connect(c));
  const auto ev = collect(c, submit(c, sfz.string(), {{"", kPiano, "", {}}}));
  CHECK(!ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_PUBLISHED);
  CHECK(fs::exists(cat.path + "/catalog.db") && fs::exists(cat.path + "/catalog.lock"));
  CHECK(!fs::exists(svc.data() + "/catalog.db") && !fs::exists(svc.data() + "/catalog.lock") && !fs::exists(cat.path + "/lib"));
  CHECK(fs::exists(svc.lib() + "/" + kServiceLockFileName) && count_lib_dir(svc.lib()).libs == 1);
  svc.stop();
  CHECK(count_rows(cat.path, "libraries") == 1);
}

void test_subscribers_get_progress() {
  Svc svc;
  CHECK(svc.start());
  TempDir src;
  const fs::path sfz = ifx::write_sfz(src.path, "Piano");
  Client owner, subscriber, bystander;
  CHECK(svc.connect(owner) && svc.connect(subscriber) && svc.connect(bystander));
  ws::v1::LibraryClientMessage sub;
  sub.mutable_subscribe()->set_enable(true);
  CHECK(subscriber.call(sub));
  const auto job = submit(owner, sfz.string(), {{"", kPiano, "", {}}});
  const auto own = collect(owner, job);
  const auto got = collect(subscriber, job);
  CHECK(!own.empty() && !got.empty() && own.size() == got.size());
  bool queued = false, running = false;
  for (const auto& p : got) {
    queued |= p.state() == ws::v1::IMPORT_STATE_QUEUED;
    running |= p.state() == ws::v1::IMPORT_STATE_RUNNING;
  }
  CHECK(queued && running && got.back().state() == ws::v1::IMPORT_STATE_PUBLISHED);
  // The bystander (neither owner nor subscriber) gets nothing: after a round trip of its own its inbox holds no progress event.
  CHECK(bystander.get());
  for (const auto& m : bystander.pushes) CHECK(!m.has_import_progress());
}

void test_cancel_multi_item_after_first_commit() {
  Gate gate;
  Svc svc([&](LibraryServiceOptions& o) {
    o.import_point_hook = [&](const char* p, std::uint64_t job, std::size_t item) {
      if (job == 1 && item == 1 && std::string(p) == "stage-created") gate.reach();
    };
  });
  CHECK(svc.start());
  TempDir src;
  std::vector<Item> items;
  for (const char* n : {"One", "Two", "Three"}) {
    ifx::write_sfz(src.path, n);
    items.push_back({std::string(n) + ".sfz", kPiano, "", {}});
  }
  Client c;
  CHECK(svc.connect(c));
  const auto job = submit(c, src.path, items);
  CHECK(gate.arrived.wait_for(std::chrono::seconds(20)) == std::future_status::ready);  // item 0 is published and cataloged; item 1 is about to stage
  CHECK(count_rows(svc.data(), "libraries") == 1);
  auto ack = cancel(c, job);
  CHECK(ack && ack->ack().changed());
  gate.open();
  const auto ev = collect(c, job);
  CHECK(!ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_CANCELLED && ev.back().items_imported() == 1 && ev.back().items_cancelled() == 1);
  CHECK(ev.back().items_done() == 2 && ev.back().items_total() == 3);  // item 3 never started
  svc.stop();
  const auto counts = count_lib_dir(svc.lib());
  CHECK(counts.libs == 1 && counts.currents == 1 && counts.bundles == 1);
  CHECK(names_in(svc.lib() + "/.staging") == std::set<std::string>{".lock"});
  CHECK(count_rows(svc.data(), "libraries") == 1);
  std::unique_ptr<Catalog> cat;
  CHECK(Catalog::open(svc.data(), cat) == CatalogStatus::kOk);
  ReconcileReport rep;
  CHECK(cat && reconcile_libraries(*cat, svc.data(), rep) == CatalogStatus::kOk && !rep.changed());  // catalog and lib already agree
}

void test_stop_while_a_job_runs() {
  Gate gate;
  Svc svc([&](LibraryServiceOptions& o) {
    o.max_queued_jobs = 1000;
    o.import_point_hook = [&](const char* p, std::uint64_t job, std::size_t) {
      if (job == 1 && std::string(p) == "stage-created") gate.reach();
    };
  });
  CHECK(svc.start());
  TempDir src;
  const fs::path a = ifx::write_sfz(src.path, "A"), b = ifx::write_sfz(src.path, "B");
  Client c;
  CHECK(svc.connect(c));
  const auto j1 = submit(c, a.string(), {{"", kPiano, "", {}}});
  CHECK(gate.arrived.wait_for(std::chrono::seconds(20)) == std::future_status::ready);
  const auto j2 = submit(c, b.string(), {{"", kPiano, "", {}}});
  CHECK(j1 == 1 && j2 == 2);
  std::thread stopper([&] { svc.s->stop(); });
  // New imports are refused as soon as stop() has begun (the service thread keeps answering until the worker is gone). Requests that slip in before are
  // queued and cancelled with the rest.
  bool refused = false;
  for (int i = 0; i < 4000 && !refused; ++i) {
    ws::v1::LibraryError e;
    const auto j = submit(c, a.string(), {{"", kPiano, "", {}}}, &e);
    if (j == 0 && e.code() == ws::v1::LIBRARY_ERROR_CODE_SHUTTING_DOWN) refused = true;
    else std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(refused);
  gate.open();
  stopper.join();  // returns: no hang
  const auto ev1 = collect(c, j1, 5000), ev2 = collect(c, j2, 5000);
  CHECK(!ev1.empty() && ev1.back().state() == ws::v1::IMPORT_STATE_CANCELLED);
  CHECK(!ev2.empty() && ev2.back().state() == ws::v1::IMPORT_STATE_CANCELLED);  // a queued job ends with a terminal CANCELLED event too
  svc.s.reset();
  CHECK(svc.start());  // restart: consistent, nothing to repair, nothing listed
  CHECK(!svc.s->startup_reconcile().changed() && svc.s->startup_reconcile().errors.empty());
  CHECK(names_in(svc.lib() + "/.staging") == std::set<std::string>{".lock"});
  CHECK(count_rows(svc.data(), "libraries") == 0 && count_lib_dir(svc.lib()).libs == 0);
}

void test_list_paging_and_reply_budget() {
  Svc svc([](LibraryServiceOptions& o) { o.reply_budget_bytes = 200; });
  CHECK(svc.start());
  TempDir src;
  const fs::path dir = fs::path(src.path) / "pack";
  std::vector<Item> items;
  for (const char* n : {"L1", "L2", "L3", "L4"}) {
    ifx::write_sfz(dir, n);
    items.push_back({std::string(n) + ".sfz", kPiano, "", {}});
  }
  Client c;
  CHECK(svc.connect(c));
  CHECK(!collect(c, submit(c, dir.string(), items)).empty());
  // limit 2 over 4 libraries: 2 entries, more=true; the next page: 2 entries, more=false (the budget of 200 bytes is not reached by two small entries... or it is).
  auto p1 = list_libs(c, 0, 2);
  CHECK(p1 && p1->library_list().total() == 4 && p1->library_list().libraries_size() >= 1 && p1->library_list().more());
  std::set<std::string> seen;
  std::uint32_t off = 0;
  int pages = 0;
  for (; pages < 10; ++pages) {  // follow `more` with the offset + size rule until the end: every library exactly once, at least one per page
    auto pg = list_libs(c, off, 2);
    CHECK(pg && pg->has_library_list());
    if (!pg) break;
    CHECK(pg->library_list().libraries_size() >= 1);
    for (const auto& l : pg->library_list().libraries()) CHECK(seen.insert(l.library_id()).second);
    off += static_cast<std::uint32_t>(pg->library_list().libraries_size());
    if (!pg->library_list().more()) break;
  }
  CHECK(seen.size() == 4 && off == 4);
  svc.stop();
  // Without the tiny budget: limit 2 => 2 entries and more=true, then the second page ends it.
  Svc svc2;
  CHECK(svc2.start());
  Client c2;
  CHECK(svc2.connect(c2));
  CHECK(!collect(c2, submit(c2, dir.string(), {items[0], items[1], items[2]})).empty());
  auto q1 = list_libs(c2, 0, 2);
  CHECK(q1 && q1->library_list().libraries_size() == 2 && q1->library_list().more() && q1->library_list().total() == 3);
  auto q2 = list_libs(c2, 2, 2);
  CHECK(q2 && q2->library_list().libraries_size() == 1 && !q2->library_list().more());
}

void test_item_report_bounds_and_utf8() {
  Svc svc;
  CHECK(svc.start());
  TempDir src;
  ifx::put(fs::path(src.path) / "w.wav", ifx::wav16(100, 3));
  std::string text;
  for (int i = 0; i < 300; ++i) text += "<region> sample=w.wav key=60 bad_op_" + std::to_string(i) + "=1\n";
  text += "<region> sample=w.wav key=61 \xff\xfe_broken=2\n";  // an opcode name that is not UTF-8
  ifx::put(fs::path(src.path) / "Many.sfz", text);
  Client c;
  CHECK(svc.connect(c));
  const auto job = submit(c, (fs::path(src.path) / "Many.sfz").string(), {{"", kPiano, "", {}}});
  CHECK(job != 0);
  const auto ev = collect(c, job);  // every frame parsed (protobuf validates UTF-8 in proto3 strings) and stayed under the frame limit
  CHECK(!ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_FAILED);
  bool seen_item = false;
  for (const auto& p : ev)
    if (p.has_item()) {
      seen_item = true;
      CHECK(p.item().status() == ws::v1::IMPORT_ITEM_SKIPPED && p.item().offenders_size() <= 100 && p.item().offenders_size() >= 50);
      CHECK(p.item().messages_size() <= 101);
      for (const auto& o : p.item().offenders()) CHECK(o.message().size() <= 400 && o.opcode().size() <= 400);
    }
  CHECK(seen_item);
}

void test_unknown_override_key() {
  Svc svc;
  CHECK(svc.start());
  TempDir src;
  const fs::path gm = ifx::write_sf2(fs::path(src.path) / "gm.sf2");
  const fs::path sfz = ifx::write_sfz(src.path, "Piano");
  Client c;
  CHECK(svc.connect(c));
  for (const auto& path : {gm, sfz}) {
    const auto job = submit(c, path.string(), {{"", kPiano, "", {{"9:99", "x", kDrums}}}});
    CHECK(job != 0);
    const auto ev = collect(c, job);
    CHECK(!ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_FAILED);
    bool saw = false;
    for (const auto& p : ev) saw |= p.has_item() && p.item().status() == ws::v1::IMPORT_ITEM_FAILED && p.item().detail().find("unknown_instrument_key") != std::string::npos;
    CHECK(saw);
    CHECK(nothing_written(svc.data()));
  }
}

// A CLI import in progress (an in-process guard held by a child process until its pipe closes) keeps a service from starting, and with it from reconciling
// `.staging` under the running import; with the guard gone the service starts.
void test_cli_import_blocks_service_start() {
  Svc svc;
  int hold[2], ready[2];
  CHECK(::pipe(hold) == 0 && ::pipe(ready) == 0);
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::close(hold[1]);
    ::close(ready[0]);
    CliWriterGuard guard(svc.data());
    const char ok = guard.ok() ? 'y' : 'n';
    (void)!::write(ready[1], &ok, 1);
    char b;
    (void)!::read(hold[0], &b, 1);  // until the parent closes its end
    ::_exit(0);
  }
  ::close(hold[0]);
  ::close(ready[1]);
  char ok = 0;
  CHECK(::read(ready[0], &ok, 1) == 1 && ok == 'y');
  // A leftover the running import owns: the refused service must not touch it.
  fs::create_directories(svc.lib() + "/.staging/inflight");
  CHECK(!svc.start() && svc.st == CatalogStatus::kBusy);
  CHECK(fs::exists(svc.lib() + "/.staging/inflight"));
  ::close(hold[1]);
  int status = 0;
  ::waitpid(pid, &status, 0);
  CHECK(svc.start());  // guard released: the service starts and its reconcile cleans the leftover
  CHECK(!fs::exists(svc.lib() + "/.staging/inflight"));
  svc.stop();
  ::close(ready[0]);
}

void test_reconcile_io_errors_remove_nothing() {
  TempDir src;
  ifx::write_sfz(fs::path(src.path), "P1");
  ifx::write_sfz(fs::path(src.path), "P2");
  Svc svc;
  CHECK(svc.start());
  {
    Client c;
    CHECK(svc.connect(c));
    const auto job = submit(c, src.path, {{"P1.sfz", kPiano, "", {}}, {"P2.sfz", kOrgan, "", {}}});
    CHECK(!collect(c, job).empty());
  }
  svc.stop();
  std::unique_ptr<Catalog> cat;
  CHECK(Catalog::open(svc.data(), cat) == CatalogStatus::kOk);
  if (!cat) return;
  std::vector<LibraryRecord> before;
  CHECK(cat->list_libraries(before, true) == CatalogStatus::kOk && before.size() == 2);
  // lib/ cannot be examined (EACCES): it is not "no libraries". No row goes.
  {
    ReconcileOptions o;
    o.inject_errno = [](const char* what, const std::string&) { return std::string(what) == "list_lib" ? EACCES : 0; };
    ReconcileReport rep;
    CHECK(reconcile_libraries(*cat, svc.data(), rep, o) == CatalogStatus::kOk);
    CHECK(rep.rows_removed == 0 && !rep.errors.empty() && rep.uncertain);
    std::vector<LibraryRecord> after;
    CHECK(cat->list_libraries(after, true) == CatalogStatus::kOk && after.size() == 2);
  }
  // One library's `current` cannot be resolved (EIO): its row stays, nothing is removed anywhere, and the pass says so.
  {
    const std::string victim = before[0].library_id;
    ReconcileOptions o;
    o.inject_errno = [&](const char* what, const std::string& path) {
      return std::string(what) == "current" && path.find(victim) != std::string::npos ? EIO : 0;
    };
    ReconcileReport rep;
    CHECK(reconcile_libraries(*cat, svc.data(), rep, o) == CatalogStatus::kOk);
    CHECK(rep.rows_removed == 0 && rep.errors.size() == 1 && rep.errors[0].find(victim) != std::string::npos);
    std::vector<LibraryRecord> after;
    CHECK(cat->list_libraries(after, true) == CatalogStatus::kOk && after.size() == 2);
  }
  // ENOENT is a definite absence: with the link really gone the row is removed.
  {
    fs::remove(svc.lib() + "/" + before[1].library_id + "/current");
    ReconcileReport rep;
    CHECK(reconcile_libraries(*cat, svc.data(), rep) == CatalogStatus::kOk && rep.rows_removed == 1 && rep.errors.empty());
  }
}

void test_unreadable_meta_keeps_the_row() {
  TempDir src;
  ifx::write_sfz(fs::path(src.path), "P1");
  Svc svc;
  CHECK(svc.start());
  std::string lib_id;
  {
    Client c;
    CHECK(svc.connect(c));
    CHECK(!collect(c, submit(c, src.path, {{"P1.sfz", kOrgan, "", {}}})).empty());
    auto l = list_libs(c);
    CHECK(l && l->library_list().libraries_size() == 1);
    lib_id = l->library_list().libraries(0).library_id();
  }
  svc.stop();
  const std::string meta = svc.lib() + "/" + lib_id + "/current/" + kCatalogMetaFileName;
  auto reconcile_with = [&](const std::string& content, ReconcileReport& rep) {
    std::unique_ptr<Catalog> cat;
    CHECK(Catalog::open(svc.data(), cat) == CatalogStatus::kOk);
    ifx::put(meta, content);
    CHECK(reconcile_libraries(*cat, svc.data(), rep) == CatalogStatus::kOk);
    LibraryRecord got;
    bool found = false;
    CHECK(cat->get_library(lib_id, got, found) == CatalogStatus::kOk && found);
    CHECK(found && got.instruments.size() == 1 && got.instruments[0].category == kOrgan);  // the row is untouched, not rewritten from a guess
  };
  const std::string good = ltest::slurp(meta);
  ReconcileReport r1, r2, r3;
  reconcile_with("format_version\t99\n", r1);                      // a newer format
  reconcile_with(good.substr(0, good.size() - 3), r2);            // cut off
  reconcile_with(std::string(5u << 20, 'x'), r3);                 // oversized
  for (const auto* r : {&r1, &r2, &r3}) CHECK(r->rows_updated == 0 && r->rows_removed == 0 && r->skipped.size() == 1 && r->uncertain);
  ifx::put(meta, good);
  ReconcileReport fine;
  reconcile_with(good, fine);
  CHECK(fine.skipped.empty() && !fine.changed());
}

}  // namespace

int main() {
  ::signal(SIGPIPE, SIG_IGN);
  test_order_of_the_story();
  test_category_required_writes_nothing();
  test_duplicate_changed_and_metadata_refresh();
  test_progress_skips_and_reasons();
  test_sf2_instrument_categories_and_paging();
  test_cancel_and_queue();
  test_unreadable_and_invalid_sources();
  test_single_writer();
  test_concurrent_clients();
  test_startup_reconcile_and_rebuild_from_bundles();
  test_reconcile_legacy_uncategorised_and_corrupt();
  test_catalog_dir_split();
  test_subscribers_get_progress();
  test_cancel_multi_item_after_first_commit();
  test_stop_while_a_job_runs();
  test_list_paging_and_reply_budget();
  test_item_report_bounds_and_utf8();
  test_unknown_override_key();
  test_cli_import_blocks_service_start();
  test_reconcile_io_errors_remove_nothing();
  test_unreadable_meta_keeps_the_row();
  std::printf(g_fail ? "librarian_import_service_test: %d FAILED\n" : "librarian_import_service_test: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
