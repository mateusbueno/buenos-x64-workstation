// Story 8.12: the startup reconcile of interrupted imports, proven (FR67, AR28, AD-11). The reconcile RULE is Story 5.6's and the lease-aware GC Story 5.21's and are
// not touched here; this test proves what this story owns:
//
//   1. ordering: the socket answers handshakes during the pass, but every command gets the typed RECONCILING refusal until the pass has finished, then commands work;
//      the pass logs one summary line with the count of every action;
//   2. fault injection: a forked child hosting the real service is killed (`_exit(137)`, the crash campaign's mechanism: a seeded boundary hook) at each of the six named
//      steps (stage, verify, fsync, rename, flip `current`, catalog row), 132 times in a seeded random order, on a fresh library and on a re-import ("Import again": the
//      source changed, the library is installed). After the restart (the service's own startup reconcile) the library set and the catalog equal the state BEFORE that
//      import or the state AFTER it, never anything between, and the previous bundle of a killed re-import still verifies, byte for byte, and is still `current` or
//      replaced by the whole new one;
//   3. boot bound: a synthetic data root of 1 TB apparent size (512 libraries, each with a 2 GiB sparse file; a FIFO named like a sample in every bundle directory, which blocks
//      any open for reading) reconciles in at most 5 s with directory and manifest level work only, and never touches a bundle that `current` references.
// Deterministic: xorshift seed below, no wall clock in any decision (the 5 s bound is the one assertion on time, as the story demands).
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

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

namespace {

constexpr std::uint64_t kSeed = 0x8c12ea51ULL;

struct Rng {
  std::uint64_t s;
  std::uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  unsigned below(unsigned n) { return static_cast<unsigned>(next() % n); }
};

bool terminal(ws::v1::ImportState s) {
  return s == ws::v1::IMPORT_STATE_PUBLISHED || s == ws::v1::IMPORT_STATE_FAILED || s == ws::v1::IMPORT_STATE_CANCELLED || s == ws::v1::IMPORT_STATE_DRIVE_REMOVED;
}

ws::v1::LibraryClientMessage import_request(const std::string& source) {
  ws::v1::LibraryClientMessage m;
  auto* r = m.mutable_import_request();
  r->set_source(source);
  r->add_items()->set_category(1);
  return m;
}

// --- states ---------------------------------------------------------------------------------------------------------------------------------

struct State {
  std::map<std::string, std::string> current;  // library_id -> bundle named by `current`
  std::map<std::string, LibraryRecord> rows;   // the catalog
};

State state_of(const std::string& root) {
  State s;
  std::error_code ec;
  for (fs::directory_iterator it(fs::path(root) / "lib", ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.size() != 36) continue;
    if (fs::is_symlink(it->path() / "current", ec)) s.current[name] = fs::read_symlink(it->path() / "current", ec).string();
  }
  std::unique_ptr<Catalog> cat;
  if (Catalog::open(root, cat) != CatalogStatus::kOk) {
    ++g_fail;
    std::printf("FAIL cannot open the catalog of %s\n", root.c_str());
    return s;
  }
  std::vector<LibraryRecord> rows;
  cat->list_libraries(rows, true);
  for (auto& r : rows) s.rows[r.library_id] = std::move(r);
  return s;
}

bool same_state(const State& a, const State& b) {
  if (a.current != b.current || a.rows.size() != b.rows.size()) return false;
  for (const auto& kv : a.rows) {
    auto it = b.rows.find(kv.first);
    if (it == b.rows.end() || !same_content(kv.second, it->second)) return false;
  }
  return true;
}

// Runs one import through a service hosted in a forked child. The child dies with `_exit(137)` when the service reaches `point` (nullptr = never). Returns the wait status.
int import_in_child(const std::string& root, const std::string& sock, const std::string& source, const char* point) {
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::alarm(120);  // a hung child dies instead of hanging the test
    LibraryServiceOptions o;
    o.data_dir = root;
    o.socket_path = sock;
    if (point) {
      const std::string p = point;
      o.import_point_hook = [p](const char* at, std::uint64_t, std::size_t) {
        if (p == at) ::_exit(137);
      };
    }
    LibraryService svc(o);
    std::string err;
    if (!svc.start(err)) ::_exit(21);
    Client c;
    if (!c.connect(sock) || !c.handshake("kill")) ::_exit(22);
    auto a = c.call(import_request(source), 30000);
    if (!a || !a->has_import_accepted()) ::_exit(23);
    for (;;) {
      auto e = c.recv(60000);
      if (!e) ::_exit(24);
      if (e->has_import_progress() && terminal(e->import_progress().state())) break;
    }
    svc.stop();
    ::_exit(0);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  return status;
}

// The restart: the service starts on `root` (its own startup reconcile runs before it answers a command), takes a command, stops.
// Returns the summary lines it logged.
std::vector<std::string> restart(const std::string& root, const std::string& sock, ReconcileReport* rep = nullptr) {
  std::vector<std::string> lines;
  LibraryServiceOptions o;
  o.data_dir = root;
  o.socket_path = sock;
  o.log = [&](const std::string& l) { lines.push_back(l); };
  LibraryService svc(o);
  std::string err;
  if (!svc.start(err)) {
    ++g_fail;
    std::printf("FAIL restart: %s\n", err.c_str());
    return lines;
  }
  Client c;
  CHECK(c.connect(sock) && c.handshake("restart"));
  auto r = c.get();
  CHECK(r && r->has_state());  // commands are accepted once start() returned
  if (rep) *rep = svc.startup_reconcile();
  svc.stop();
  return lines;
}

bool no_leftovers(const std::string& root) {
  std::error_code ec;
  const fs::path lib = fs::path(root) / "lib";
  for (fs::directory_iterator it(lib / ".staging", ec), end; !ec && it != end; it.increment(ec))
    if (it->path().filename().string() != ".lock") return false;
  for (fs::directory_iterator it(lib, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().filename().string().size() != 36) continue;
    for (fs::directory_iterator sub(it->path(), ec), send; !ec && sub != send; sub.increment(ec))
      if (sub->path().filename().string().rfind(".current.tmp", 0) == 0) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// 1. ordering and summary

std::string make_installed_root(const ltest::TempDir& work, const std::string& name, const fs::path& src, std::string* library_id = nullptr) {
  const std::string root = work.sub(name);
  fs::create_directories(root);
  const int st = import_in_child(root, work.sub(name + ".sock"), src.string(), nullptr);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  if (library_id) {
    const State s = state_of(root);
    CHECK(s.current.size() == 1);
    if (!s.current.empty()) *library_id = s.current.begin()->first;
  }
  return root;
}

void ordering_and_summary_test(const ltest::TempDir& work, const fs::path& src) {
  // The root: one installed library whose row is missing (recreated), a staging leftover (deleted), an orphan temp link (deleted),
  // a marked non-current bundle (GC) and a second library without `current` (row removed).
  std::string lib_a;
  const std::string root = make_installed_root(work, "ord", src, &lib_a);
  const fs::path libdir = fs::path(root) / "lib";
  {
    std::unique_ptr<Catalog> cat;
    CHECK(Catalog::open(root, cat) == CatalogStatus::kOk);
    bool removed = false;
    CHECK(cat->remove_library(lib_a, removed) == CatalogStatus::kOk && removed);  // the row to recreate
    LibraryRecord ghost;                                                          // a row whose library has no `current`
    ghost.library_id = "00000000-0000-4000-8000-000000000042";
    ghost.name = "Ghost";
    ghost.source_kind = "sfz";
    ghost.bundle_id = std::string(64, 'a');
    InstrumentRecord ins;
    ins.key = "g.sfz";
    ins.name = "g";
    ins.category = 1;
    ghost.instruments.push_back(ins);
    bool changed = false;
    CHECK(cat->publish_library(ghost, changed) == CatalogStatus::kOk);
  }
  fs::create_directories(libdir / ".staging" / "leftover");
  ifx::put(libdir / ".staging" / "leftover" / "x", "partial");
  ifx::put(libdir / lib_a / ".current.tmp.dead", "x");
  const std::string marked(64, 'b');
  fs::create_directories(libdir / lib_a / marked);
  ifx::put(libdir / lib_a / marked / kCheckpointMarkerName, "");

  // Hold the pass at its first hook. The socket is up and answers the handshake; any command is refused RECONCILING.
  std::mutex mu;
  std::condition_variable cv;
  bool in_pass = false, release = false;
  std::vector<std::string> lines;
  std::mutex lines_mu;
  LibraryServiceOptions o;
  o.data_dir = root;
  o.socket_path = work.sub("ord2.sock");
  o.playing_lease_ms = 50;  // the lease timer must not touch the catalog during the pass
  o.log = [&](const std::string& l) {
    std::lock_guard<std::mutex> lk(lines_mu);
    lines.push_back(l);
  };
  o.import_point_hook = [&](const char* at, std::uint64_t, std::size_t) {
    if (std::strcmp(at, "reconcile.cleaned") != 0) return;
    std::unique_lock<std::mutex> lk(mu);
    in_pass = true;
    cv.notify_all();
    if (!cv.wait_for(lk, std::chrono::seconds(60), [&] { return release; })) {
      std::printf("FAIL the held pass was never released\n");
      ++g_fail;
    }
  };
  LibraryService svc(o);
  std::string err;
  std::atomic<bool> started{false};
  std::thread starter([&] { started = svc.start(err); });
  {
    std::unique_lock<std::mutex> lk(mu);
    if (!cv.wait_for(lk, std::chrono::seconds(30), [&] { return in_pass; })) {
      ++g_fail;
      std::printf("FAIL the pass never reached its first hook\n");
      release = true;
      cv.notify_all();
      lk.unlock();
      starter.join();
      svc.stop();
      return;
    }
  }
  {
    Client c;
    CHECK(c.connect(o.socket_path) && c.handshake("early"));
    auto fav = c.add_fav("ls1");
    CHECK(fav && fav->has_error() && fav->error().code() == ws::v1::LIBRARY_ERROR_CODE_RECONCILING);
    auto imp = c.call(import_request(src.string()));
    CHECK(imp && imp->has_error() && imp->error().code() == ws::v1::LIBRARY_ERROR_CODE_RECONCILING);
    CHECK(imp && imp->error().details().count("retry_after_ms") && imp->error().details().at("retry_after_ms") == "250");
    {
      ws::v1::LibraryClientMessage m;
      m.mutable_preflight_request()->set_source(src.string());
      auto r = c.call(m);
      CHECK(r && r->has_error() && r->error().code() == ws::v1::LIBRARY_ERROR_CODE_RECONCILING);
      ws::v1::LibraryClientMessage m2;
      m2.mutable_install_protected()->set_source_dir(work.sub("nopack"));
      r = c.call(m2);
      CHECK(r && r->has_error() && r->error().code() == ws::v1::LIBRARY_ERROR_CODE_RECONCILING);
      ws::v1::LibraryClientMessage m3;
      m3.mutable_subscribe()->set_enable(true);
      r = c.call(m3);
      CHECK(r && r->has_error() && r->error().code() == ws::v1::LIBRARY_ERROR_CODE_RECONCILING);
    }
    // Clients come and go while the pass is held, and the 50 ms playing lease would expire several times over: the service thread must stay alive and answer.
    for (int k = 0; k < 5; ++k) {
      Client x;
      CHECK(x.connect(o.socket_path) && x.handshake("churn"));
      auto g = x.get();
      CHECK(g && g->has_error() && g->error().code() == ws::v1::LIBRARY_ERROR_CODE_RECONCILING);
    }  // each closes here
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    auto still = c.get();
    CHECK(still && still->has_error() && still->error().code() == ws::v1::LIBRARY_ERROR_CODE_RECONCILING);
    auto get = c.get();
    CHECK(get && get->has_error() && get->error().code() == ws::v1::LIBRARY_ERROR_CODE_RECONCILING);
    {
      std::lock_guard<std::mutex> lk(lines_mu);
      CHECK(lines.empty());  // no summary before the pass is over
    }
    {
      std::lock_guard<std::mutex> lk(mu);
      release = true;
    }
    cv.notify_all();
    starter.join();
    CHECK(started.load());
    // Same connection, after the pass: commands work.
    auto ok = c.add_fav("ls1");
    CHECK(ok && ok->has_ack() && ok->ack().changed());
    auto st = c.get();
    CHECK(st && st->has_state());
  }
  svc.stop();
  std::lock_guard<std::mutex> lines_lk(lines_mu);
  CHECK(lines.size() == 1);
  if (!lines.empty()) {
    const std::string& l = lines[0];
    std::printf("%s\n", l.c_str());
    for (const char* want : {"staging_deleted=1 ", "tmp_links_deleted=1 ", "catalog_rows_removed=1 ", "catalog_rows_recreated=1 ", "gc_bundles_deleted=1 ", "libraries=1 "})
      CHECK(l.find(want) != std::string::npos);
  }
  CHECK(no_leftovers(root));
  CHECK(!fs::exists(libdir / lib_a / marked));
  // A second start finds nothing to do.
  const auto again = restart(root, work.sub("ord3.sock"));
  CHECK(again.size() == 1 && again[0].find("staging_deleted=0 ") != std::string::npos && again[0].find("catalog_rows_recreated=0 ") != std::string::npos &&
        again[0].find("catalog_rows_removed=0 ") != std::string::npos && again[0].find("gc_bundles_deleted=0 ") != std::string::npos);
}

// A startup pass that returns non-OK: start() fails with `reconcile: ...`, logs a line, leaves no socket, and releases every lock, so a second service starts.
void failed_pass_test(const ltest::TempDir& work, const fs::path& src) {
  std::string lib_a;
  const std::string root = make_installed_root(work, "fail", src, &lib_a);
  {
    std::unique_ptr<Catalog> cat;
    CHECK(Catalog::open(root, cat) == CatalogStatus::kOk);
    bool removed = false;
    CHECK(cat->remove_library(lib_a, removed) == CatalogStatus::kOk && removed);  // the pass has a row to write
  }
  std::vector<std::string> lines;
  LibraryServiceOptions o;
  o.data_dir = root;
  o.socket_path = work.sub("fail1.sock");
  o.log = [&](const std::string& l) { lines.push_back(l); };
  {
    LibraryService svc(o);
    svc.set_catalog_fault_hook([](const char* p) { return std::strcmp(p, "library.before_commit") == 0; });
    std::string err;
    CHECK(!svc.start(err));
    CHECK(err.rfind("reconcile:", 0) == 0);
    CHECK(!fs::exists(o.socket_path));
    CHECK(lines.size() == 1 && lines[0].rfind("reconcile:", 0) == 0);
  }
  // A hook that throws during the pass fails the start cleanly too (no joinable thread is destroyed).
  {
    LibraryServiceOptions t = o;
    t.log = nullptr;
    t.import_point_hook = [](const char* at, std::uint64_t, std::size_t) {
      if (std::strcmp(at, "reconcile.cleaned") == 0) throw std::runtime_error("boom");
    };
    LibraryService svc(t);
    std::string err;
    CHECK(!svc.start(err) && err.rfind("reconcile:", 0) == 0);
    CHECK(!fs::exists(t.socket_path));
  }
  // Flock, catalog lock, socket and thread are all released: a second service on the same root starts and reconciles.
  std::vector<std::string> lines2;
  o.log = [&](const std::string& l) { lines2.push_back(l); };
  LibraryService second(o);
  std::string err;
  CHECK(second.start(err));
  CHECK(lines2.size() == 1 && lines2[0].find("catalog_rows_recreated=1 ") != std::string::npos);
  Client c;
  CHECK(c.connect(o.socket_path) && c.handshake("second"));
  auto g = c.get();
  CHECK(g && g->has_state());
  second.stop();
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// 2. kills at the six named steps

struct StepSpec {
  const char* name;
  std::vector<const char*> points;  // the boundaries of the pipeline that belong to the step
};

void kill_campaign(const ltest::TempDir& work, const fs::path& srcdir, const fs::path& sfz) {
  const std::vector<StepSpec> steps = {{"stage", {"stage-created", "samples-written"}},
                                       {"verify", {"verified"}},
                                       {"fsync", {"fsynced"}},
                                       {"rename", {"renamed"}},
                                       {"flip-current", {"link-created", "link-flipped"}},
                                       {"catalog-row", {"catalog-row-written"}}};
  // Reference states. The source is at variant 1 for every kill run; the installed library of the re-import scenario was imported from variant 0.
  ifx::write_sfz(srcdir, "Piano", 0);
  const std::string proto_r = make_installed_root(work, "proto_r", sfz);
  const State before_r = state_of(proto_r);
  CHECK(before_r.current.size() == 1);
  const std::string lib_id = before_r.current.empty() ? "" : before_r.current.begin()->first;
  const std::string old_bundle = before_r.current.empty() ? "" : before_r.current.begin()->second;
  const std::string old_bundle_bytes = ltest::slurp(proto_r + "/lib/" + lib_id + "/" + old_bundle + "/bundle.vlb");
  CHECK(!old_bundle_bytes.empty());
  ifx::write_sfz(srcdir, "Piano", 1);

  const std::string proto_f = work.sub("proto_f");
  fs::create_directories(proto_f);
  const State before_f = state_of(proto_f);
  CHECK(before_f.current.empty() && before_f.rows.empty());

  auto build_after = [&](const std::string& proto, const std::string& name) {
    const std::string root = work.sub(name);
    fs::copy(proto, root, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
    const int st = import_in_child(root, work.sub(name + ".sock"), sfz.string(), nullptr);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    return state_of(root);
  };
  const State after_f = build_after(proto_f, "after_f");
  const State after_r = build_after(proto_r, "after_r");
  CHECK(after_f.current.size() == 1 && after_r.current.size() == 1);
  const std::string new_bundle = after_r.current.empty() ? "" : after_r.current.begin()->second;
  CHECK(!new_bundle.empty() && new_bundle != old_bundle);  // a changed source really makes a new bundle

  // The plan: 11 kills per (step, scenario) = 132, in a seeded shuffle.
  struct Run {
    unsigned step;
    bool reimport;
  };
  std::vector<Run> plan;
  for (unsigned s = 0; s < steps.size(); ++s)
    for (int k = 0; k < 11; ++k) {
      plan.push_back({s, false});
      plan.push_back({s, true});
    }
  Rng rng{kSeed};
  for (std::size_t i = plan.size() - 1; i > 0; --i) std::swap(plan[i], plan[rng.below(static_cast<unsigned>(i + 1))]);
  CHECK(plan.size() >= 100);

  std::map<std::string, int> hits, ended_before, ended_after, point_hits;
  int kills = 0;
  for (std::size_t i = 0; i < plan.size(); ++i) {
    const StepSpec& step = steps[plan[i].step];
    const char* point = step.points[rng.below(static_cast<unsigned>(step.points.size()))];
    const bool re = plan[i].reimport;
    char tag[160];
    std::snprintf(tag, sizeof tag, "seed=0x%llx run=%zu step=%s point=%s reimport=%d", static_cast<unsigned long long>(kSeed), i, step.name, point, re ? 1 : 0);
    const std::string root = work.sub("k" + std::to_string(i));
    fs::copy(re ? proto_r : proto_f, root, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
    const int st = import_in_child(root, work.sub("k.sock"), sfz.string(), point);
    if (!(WIFEXITED(st) && WEXITSTATUS(st) == 137)) {
      ++g_fail;
      std::printf("FAIL %s: the child was not killed at its point (status %d)\n", tag, st);
      continue;
    }
    ++kills;
    ++hits[step.name];
    ++point_hits[point];
    // Restart: the service's startup reconcile, then the states.
    ReconcileReport rep;
    restart(root, work.sub("k2.sock"), &rep);
    if (!rep.errors.empty()) {
      ++g_fail;
      std::printf("FAIL %s: the restart reconcile reported errors\n", tag);
    }
    const State now = state_of(root);
    const bool is_before = same_state(now, re ? before_r : before_f);
    const bool is_after = same_state(now, re ? after_r : after_f);
    if (!is_before && !is_after) {
      ++g_fail;
      std::printf("FAIL %s: library set / catalog is neither the state before nor the state after the import\n", tag);
    }
    ++(is_after ? ended_after : ended_before)[step.name];
    if (!no_leftovers(root)) {
      ++g_fail;
      std::printf("FAIL %s: staging leftovers or temp links after the restart\n", tag);
    }
    // The bundle that `current` names is whole and verifies.
    for (const auto& kv : now.current) {
      auto rd = fmt::BundleReader::open_file(root + "/lib/" + kv.first + "/" + kv.second + "/bundle.vlb");
      if (!(rd.ok() && rd.value().verify_all() == fmt::Error::kNone)) {
        ++g_fail;
        std::printf("FAIL %s: `current` names a bundle that does not verify\n", tag);
      }
    }
    if (re) {
      // The killed re-import leaves the previous bundle playing: current is the previous or the whole new one, and the previous bundle is untouched either way.
      const std::string cur = now.current.count(lib_id) ? now.current.at(lib_id) : "";
      if (cur != old_bundle && cur != new_bundle) {
        ++g_fail;
        std::printf("FAIL %s: `current` of the re-imported library is neither the previous nor the new bundle\n", tag);
      }
      if (ltest::slurp(root + "/lib/" + lib_id + "/" + old_bundle + "/bundle.vlb") != old_bundle_bytes) {
        ++g_fail;
        std::printf("FAIL %s: the previous bundle was modified or deleted\n", tag);
      }
    }
  }
  CHECK(kills == static_cast<int>(plan.size()));
  CHECK(kills >= 100);
  for (const StepSpec& s : steps)
    for (const char* p : s.points) {
      if (point_hits[p] < 1) {
        ++g_fail;
        std::printf("FAIL point %s was never hit\n", p);
      }
    }
  int before_total = 0, after_total = 0;
  for (const StepSpec& s : steps) {
    CHECK(hits[s.name] >= 11);
    before_total += ended_before[s.name];
    after_total += ended_after[s.name];
  }
  // Not vacuous: kills before the flip end in the before-state, kills after it in the after-state.
  CHECK(before_total >= 20 && after_total >= 20);
  CHECK(ended_before["stage"] > 0 && ended_after["catalog-row"] > 0 && ended_after["flip-current"] > 0);
  std::printf("kill campaign: %d kills over %zu steps, ended in the state before: %d, after: %d\n", kills, steps.size(), before_total, after_total);
  for (const StepSpec& s : steps) std::printf("  %-13s kills %2d  before %2d  after %2d\n", s.name, hits[s.name], ended_before[s.name], ended_after[s.name]);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// 3. boot bound on a 1 TB-apparent data root

struct FileSig {
  std::uint64_t size;
  std::int64_t mtime_ns;
  unsigned mode;
  bool operator==(const FileSig& o) const { return size == o.size && mtime_ns == o.mtime_ns && mode == o.mode; }
};

// Name, size, mtime and mode of everything under every bundle directory that a `current` link references.
std::map<std::string, FileSig> referenced_bundles(const std::string& root) {
  std::map<std::string, FileSig> m;
  std::error_code ec;
  const fs::path lib = fs::path(root) / "lib";
  for (fs::directory_iterator it(lib, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().filename().string().size() != 36) continue;
    std::error_code e2;
    if (!fs::is_symlink(it->path() / "current", e2)) continue;
    const fs::path bundle = it->path() / fs::read_symlink(it->path() / "current", e2);
    for (fs::recursive_directory_iterator f(bundle, e2), fend; !e2 && f != fend; f.increment(e2)) {
      struct stat st;
      if (::lstat(f->path().c_str(), &st) != 0) continue;
#ifdef __APPLE__
      const std::int64_t ns = static_cast<std::int64_t>(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
      const std::int64_t ns = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
#endif
      m[f->path().string()] = {static_cast<std::uint64_t>(st.st_size), ns, static_cast<unsigned>(st.st_mode)};
    }
  }
  return m;
}

void boot_bound_test(const ltest::TempDir& work) {
  constexpr unsigned kLibs = 512;
  constexpr std::uint64_t kPad = 2ULL << 30;  // 2 GiB apparent per library: 512 x 2 GiB = 1.1e12 bytes
  const std::string root = work.sub("big");
  const fs::path src = work.sub("bigsrc");
  fs::create_directories(root);
  std::vector<std::string> ids;
  for (unsigned i = 0; i < kLibs; ++i) {
    const std::string name = "L" + std::to_string(i);
    const fs::path sfz = ifx::write_sfz(src, name, 0, 64);
    ImportOptions io;
    io.sfz_path = sfz.string();
    io.root = root;
    io.category = "piano";
    const ImportResult r = import_file(io);
    if (!r.ok()) {
      ++g_fail;
      std::printf("FAIL setup import %u\n", i);
      return;
    }
    ids.push_back(r.library_id);
    // The "content": a sparse 2 GiB file, a FIFO named like a sample (opening it for reading blocks forever) and an unreadable file. Reconcile has no business with them.
    const fs::path b = r.bundle_dir;
    const int fd = ::open((b / "samples.pad").c_str(), O_WRONLY | O_CREAT, 0600);
    if (fd < 0 || ::ftruncate(fd, static_cast<off_t>(kPad)) != 0) {
      ++g_fail;
      std::printf("FAIL cannot create a sparse file: %s\n", std::strerror(errno));
      if (fd >= 0) ::close(fd);
      return;
    }
    ::close(fd);
    struct stat pst;
    if (::stat((b / "samples.pad").c_str(), &pst) != 0 || static_cast<std::uint64_t>(pst.st_blocks) * 512 > (1u << 20)) {
      ++g_fail;
      std::printf("FAIL the sparse file allocated real space (st_blocks=%lld): file system without sparse files\n", static_cast<long long>(pst.st_blocks));
      return;
    }
    if (::geteuid() != 0) ::chmod((b / "samples.pad").c_str(), 0);  // root ignores modes; the FIFO is the trap then
    CHECK(::mkfifo((b / "trap.wav").c_str(), 0600) == 0);
  }
  // The catalog is brought up to date once, untimed (this is the boot after the import). Then the work a boot after a crash has to do.
  {
    std::unique_ptr<Catalog> cat;
    CHECK(Catalog::open(root, cat) == CatalogStatus::kOk);
    ReconcileReport first;
    CHECK(reconcile_libraries(*cat, root, first) == CatalogStatus::kOk && first.rows_added == kLibs);
  }
  const fs::path lib = fs::path(root) / "lib";
  // Leftovers of an interrupted import (a big one included), orphan temp links, 8 libraries that lost `current`, 8 marked non-current bundles.
  fs::create_directories(lib / ".staging" / "u1" / "deep");
  {
    const int fd = ::open((lib / ".staging" / "u1" / "deep" / "pad").c_str(), O_WRONLY | O_CREAT, 0600);
    CHECK(fd >= 0 && ::ftruncate(fd, static_cast<off_t>(kPad)) == 0);
    if (fd >= 0) ::close(fd);
  }
  for (unsigned i = 0; i < 5; ++i) ifx::put(lib / ids[i] / ".current.tmp.dead", "x");
  for (unsigned i = 0; i < 8; ++i) fs::remove(lib / ids[100 + i] / "current");
  const std::string marked(64, 'c');
  for (unsigned i = 0; i < 8; ++i) {
    const fs::path d = lib / ids[200 + i] / marked;
    fs::create_directories(d);
    ifx::put(d / kCheckpointMarkerName, "");
    ifx::put(d / "samples.wav", "not read");
  }
  std::uint64_t apparent = 0;
  {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(lib, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code e2;
      if (it->is_regular_file(e2)) apparent += it->file_size(e2);
    }
  }
  CHECK(apparent >= 1000000000000ULL);  // 1 TB apparent
  const auto sig_before = referenced_bundles(root);
  std::printf("referenced bundle files: %zu\n", sig_before.size());
  CHECK(sig_before.size() >= kLibs * 3);

  std::unique_ptr<Catalog> cat;
  CHECK(Catalog::open(root, cat) == CatalogStatus::kOk);
  // The pass runs on its own thread so that a blocked read (the FIFOs) fails the test instead of hanging it.
  auto done = std::make_shared<std::promise<void>>();
  auto fut = done->get_future();
  ReconcileReport rep;
  CatalogStatus rs = CatalogStatus::kIo;
  const auto t0 = std::chrono::steady_clock::now();
  std::thread t([&, done] {
    rs = reconcile_libraries(*cat, root, rep);
    done->set_value();
  });
  if (fut.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
    std::printf("FAIL boot bound: the pass did not finish in 30 s (blocked on a content file?)\n");
    std::fflush(stdout);
    std::error_code rm;
    fs::remove_all(work.path, rm);  // no 1 TB-apparent tree left behind
    ::_exit(1);
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  t.join();
  std::printf("boot bound: %.2f TB apparent, %u libraries: %s in %.3f s\n", static_cast<double>(apparent) / 1e12, kLibs, reconcile_summary(rep).c_str(), secs);
  CHECK(rs == CatalogStatus::kOk && rep.errors.empty());
  CHECK(secs <= 5.0 && rep.elapsed_us <= 5000000ULL);
  CHECK(rep.staging_removed == 1 && rep.tmp_links_removed == 5 && rep.rows_removed == 8 && rep.bundles_pruned == 8);
  CHECK(rep.rows_added == 0 && rep.rows_updated == 0 && rep.rows_unchanged == kLibs - 8);
  CHECK(rep.libraries_on_disk == kLibs - 8);
  // Never touched a bundle that `current` references (size, mtime, mode of every file in it, the traps included).
  const auto sig_after = referenced_bundles(root);
  std::size_t kept = 0;
  for (const auto& kv : sig_before) {
    auto it = sig_after.find(kv.first);
    if (it == sig_after.end()) {  // only the bundles of the 8 libraries that lost their link are no longer referenced
      continue;
    }
    ++kept;
    if (!(it->second == kv.second)) {
      ++g_fail;
      std::printf("FAIL a referenced bundle file changed: %s\n", kv.first.c_str());
    }
  }
  CHECK(kept == sig_after.size() && kept + 8 * 4 >= sig_before.size() && kept >= (kLibs - 8) * 3);
  for (unsigned i = 0; i < 8; ++i) CHECK(!fs::exists(lib / ids[200 + i] / marked));
}

}  // namespace

int main() {
  ltest::TempDir work;
  if (work.path.empty()) return 2;
  const fs::path srcdir = work.sub("src");
  fs::create_directories(srcdir);
  const fs::path sfz = ifx::write_sfz(srcdir, "Piano", 0);
  const auto t0 = std::chrono::steady_clock::now();
  ordering_and_summary_test(work, sfz);
  failed_pass_test(work, sfz);
  kill_campaign(work, srcdir, sfz);
  boot_bound_test(work);
  std::printf("startup reconcile test: %s (%.1f s)\n", g_fail ? "FAILED" : "ok", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  return g_fail ? 1 : 0;
}
