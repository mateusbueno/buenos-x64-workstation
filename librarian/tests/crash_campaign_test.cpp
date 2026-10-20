// Story 5.6 crash campaign: "the librarian is killed at random points during 200 scripted imports; on restart the reconcile pass leaves the catalog and
// the library tree agreeing and no partial library is listed".
//
// A seeded script of 200 imports (SFZ and SF2, single- and multi-instrument, re-imports of unchanged and of changed sources, categories and names that
// change, files that are skipped as damaged or unsupported). Every import runs in a forked child that hosts the real LibraryService on a real socket and
// is killed with _exit(137) by the service's boundary hook at a point chosen from the seed: stage-created, samples-written, verified, fsynced, renamed,
// link-created, link-flipped, catalog-row-written (inside the catalog transaction), catalog-committed, job-finished, or inside the reconcile pass of its
// own startup. After each child the parent restarts (opens the catalog, runs the reconcile pass) and checks:
//   (a) every catalog library has a `current` bundle that opens and verifies (verify_all) and instrument rows equal to what the bundle says;
//   (b) every library directory with a usable `current` is in the catalog (no published library is missing, no partial one is listed);
//   (c) no staging leftovers and no orphan `.current.tmp.*` links;
//   (d) re-running the whole script without faults on the crashed tree and on a fresh tree ends in the same published state.
// Deterministic (xorshift seeded below; no wall clock in any decision), no timing assertion. Runtime is printed: about 20 s on a laptop (tiny sources; most of
// it is process start and service poll intervals, not work), under the 60 s above which a campaign would have to be marked slow.
#include <functional>
#include <cstring>
#include <sqlite3.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <map>
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

namespace {

constexpr int kSteps = 200;
constexpr std::uint64_t kSeed = 0x5eed5eed2026ULL;

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

struct Source {
  const char* file;
  bool sf2;
  bool valid;     // imports successfully
  const char* kind;
};
const Source kSources[] = {
    {"A.sfz", false, true, "sfz"},      {"B.sfz", false, true, "sfz"},   {"C.sfz", false, true, "sfz"},   {"gm.sf2", true, true, "sf2"},
    {"gm2.sf2", true, true, "sf2"},     {"Broken.sfz", false, false, "damaged"}, {"Fancy.sfz", false, false, "unsupported"},
};
constexpr int kSourceCount = 7;

const char* const kPoints[] = {"stage-created", "samples-written", "verified",          "fsynced",           "renamed",
                               "link-created",  "link-flipped",    "catalog-row-written", "catalog-committed", "job-finished"};
constexpr int kPointCount = 10;

struct Step {
  int source = 0;
  unsigned variant = 0;  // 0..2: the content of the source at this step
  unsigned category = 1;
  std::string name;
  unsigned override_cat = 0;  // SF2: category of preset 128:0
  int point = -1;             // index into kPoints; -1 = no kill
  std::string reconcile_point;  // kill inside the startup reconcile instead (only fires if there is something to reconcile)
};

std::vector<Step> make_script() {
  Rng r{kSeed};
  std::vector<Step> v;
  for (int i = 0; i < kSteps; ++i) {
    Step s;
    // 2/3 of the steps hit a valid source, 1/6 a source that is skipped; sources repeat a lot, so re-imports are the norm.
    const unsigned roll = r.below(12);
    s.source = roll < 3 ? 0 : roll < 5 ? 1 : roll < 6 ? 2 : roll < 8 ? 3 : roll < 10 ? 4 : roll < 11 ? 5 : 6;
    s.variant = r.below(10) < 3 ? 1 + r.below(2) : 0;  // mostly the same bytes again (unchanged), sometimes a changed source
    s.category = 1 + r.below(17);
    if (r.below(4) == 0) s.name = "Name " + std::to_string(r.below(5));
    if (kSources[s.source].sf2 && r.below(2) == 0) s.override_cat = 1 + r.below(17);
    const unsigned k = r.below(20);
    if (k < 14) s.point = static_cast<int>(r.below(kPointCount));
    else if (k < 16) s.reconcile_point = "reconcile.cleaned";
    else if (k < 17) s.reconcile_point = "reconcile.add";
    else if (k < 18) s.reconcile_point = "reconcile.update";
    // k >= 18: a clean run
    v.push_back(std::move(s));
  }
  return v;
}

void materialize(const fs::path& dir, const Step& s, std::map<int, unsigned>& state) {
  if (state.count(s.source) && state[s.source] == s.variant) return;
  const Source& src = kSources[s.source];
  if (std::string(src.kind) == "damaged") ifx::write_damaged_sfz(dir, "Broken");
  else if (std::string(src.kind) == "unsupported") ifx::write_unsupported_sfz(dir, "Fancy");
  else if (src.sf2) ifx::write_sf2(dir / src.file, s.variant);
  else ifx::write_sfz(dir, std::string(src.file, std::strlen(src.file) - 4), s.variant);
  state[s.source] = s.variant;
}

ws::v1::LibraryClientMessage request_for(const fs::path& dir, const Step& s) {
  ws::v1::LibraryClientMessage m;
  auto* r = m.mutable_import_request();
  r->set_source((dir / kSources[s.source].file).string());
  auto* it = r->add_items();
  it->set_category(s.category);
  it->set_name(s.name);
  if (kSources[s.source].sf2 && s.override_cat) {
    auto* o = it->add_instruments();
    o->set_key("128:0");
    o->set_category(s.override_cat);
    o->set_name("Kit " + std::to_string(s.override_cat));
  }
  return m;
}

bool terminal(ws::v1::ImportState s) {
  return s == ws::v1::IMPORT_STATE_PUBLISHED || s == ws::v1::IMPORT_STATE_FAILED || s == ws::v1::IMPORT_STATE_CANCELLED;
}

// Runs one import on a service over `data`; returns the terminal state (0 if it could not).
int run_import(const std::string& data, const std::string& sock, const fs::path& dir, const Step& s, const std::function<void(const char*, std::uint64_t, std::size_t)>& hook) {
  LibraryServiceOptions o;
  o.data_dir = data;
  o.socket_path = sock;
  o.import_point_hook = hook;
  LibraryService svc(o);
  std::string err;
  if (!svc.start(err)) return -1;
  Client c;
  if (!c.connect(sock) || !c.handshake("campaign")) return -2;
  auto r = c.call(request_for(dir, s), 30000);
  if (!r || !r->has_import_accepted()) return -3;
  const auto job = r->import_accepted().job_id();
  int state = 0;
  auto note = [&](const ws::v1::LibraryServerMessage& m) {
    if (m.has_import_progress() && m.import_progress().job_id() == job && terminal(m.import_progress().state())) state = m.import_progress().state();
  };
  for (const auto& m : c.pushes) note(m);
  while (!state) {
    auto m = c.recv(60000);
    if (!m) return -4;
    note(*m);
  }
  svc.stop();
  return state;
}

// --- the invariants --------------------------------------------------------------------------------------------------------------------------

int g_inv_fail = 0;
#define INV(cond, step, what)                                                                       \
  do {                                                                                              \
    if (!(cond)) {                                                                                  \
      std::printf("INVARIANT step %d: %s (%s:%d)\n", (step), (what), __FILE__, __LINE__);          \
      ++g_inv_fail;                                                                                 \
      ++g_fail;                                                                                     \
    }                                                                                               \
  } while (0)

std::string current_target(const fs::path& libdir) {
  std::error_code ec;
  const fs::path link = libdir / "current";
  if (!fs::is_symlink(link, ec)) return {};
  return fs::read_symlink(link, ec).string();
}

void check_invariants(const std::string& data, int step, Catalog& cat) {
  const fs::path lib = fs::path(data) / "lib";
  std::error_code ec;
  // (c) no leftovers.
  std::set<std::string> staging;
  for (fs::directory_iterator it(lib / ".staging", ec), end; !ec && it != end; it.increment(ec)) staging.insert(it->path().filename().string());
  INV(staging.empty() || staging == std::set<std::string>{".lock"}, step, "staging leftovers");
  std::map<std::string, std::string> on_disk;  // library_id -> bundle_id of a usable current
  for (fs::directory_iterator it(lib, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.size() != 36) continue;
    for (fs::directory_iterator sub(it->path(), ec), send; !ec && sub != send; sub.increment(ec))
      INV(sub->path().filename().string().rfind(".current.tmp", 0) != 0, step, "orphan .current.tmp link");
    const std::string target = current_target(it->path());
    if (target.size() == 64 && fs::exists(it->path() / target / "bundle.vlb", ec)) on_disk[name] = target;
  }
  std::vector<LibraryRecord> rows;
  INV(cat.list_libraries(rows, true) == CatalogStatus::kOk, step, "catalog readable");
  std::set<std::string> listed;
  for (const LibraryRecord& r : rows) {
    listed.insert(r.library_id);
    // (a) a valid current bundle that verifies, and rows equal to it.
    INV(on_disk.count(r.library_id) == 1 && on_disk[r.library_id] == r.bundle_id, step, "catalog library without its current bundle");
    if (!on_disk.count(r.library_id)) continue;
    const std::string bdir = (lib / r.library_id / r.bundle_id).string();
    auto rd = fmt::BundleReader::open_file(bdir + "/bundle.vlb");
    INV(rd.ok() && rd.value().verify_all() == fmt::Error::kNone, step, "listed bundle does not verify");
    const DescribeResult dr = describe_bundle(bdir);
    INV(dr.status == DescribeStatus::kOk && same_content(dr.record, r), step, "instrument rows differ from the bundle");
    INV(!r.instruments.empty(), step, "a listed library without instruments");
    for (const auto& i : r.instruments) INV(i.category >= 1 && i.category <= 17, step, "category out of range");
  }
  // (b) every current bundle is cataloged.
  for (const auto& kv : on_disk) INV(listed.count(kv.first) == 1, step, "published library missing from the catalog");
}

struct Published {
  std::map<std::string, LibraryRecord> libs;
  std::map<std::string, std::string> currents;
};
Published snapshot(const std::string& data) {
  Published p;
  std::unique_ptr<Catalog> cat;
  if (Catalog::open(data, cat) != CatalogStatus::kOk) {
    ++g_fail;
    std::printf("snapshot: cannot open catalog\n");
    return p;
  }
  std::vector<LibraryRecord> rows;
  cat->list_libraries(rows, true);
  for (auto& r : rows) {
    p.currents[r.library_id] = r.bundle_id;
    r.imported_seq = 0;  // the order of imports differs between a crashed and a clean history
    p.libs[r.library_id] = std::move(r);
  }
  return p;
}

}  // namespace

int main() {
  ::signal(SIGPIPE, SIG_IGN);
  const auto t0 = std::chrono::steady_clock::now();
  const std::vector<Step> script = make_script();
  ltest::TempDir work;
  const fs::path src = fs::path(work.path) / "src";
  const std::string data = work.sub("data"), gold = work.sub("gold");
  fs::create_directories(src);
  fs::create_directories(data);
  fs::create_directories(gold);
  const std::string sock = work.sub("s");
  std::map<int, unsigned> state;
  std::map<std::string, int> kills;
  int killed = 0, clean = 0, expected_hits = 0, reconcile_kills = 0;

  for (int i = 0; i < kSteps; ++i) {
    const Step& s = script[static_cast<std::size_t>(i)];
    materialize(src, s, state);
    const char* point = s.point >= 0 ? kPoints[s.point] : nullptr;
    const pid_t pid = ::fork();
    if (pid == 0) {
      const std::string rp = s.reconcile_point;
      auto hook = [point, rp](const char* p, std::uint64_t, std::size_t) {
        if ((point && std::string(p) == point) || (!rp.empty() && std::string(p) == rp)) ::_exit(137);
      };
      const int st = run_import(data, sock, src, s, hook);
      ::_exit(st > 0 ? 0 : 20 + (-st));
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    const bool exited = WIFEXITED(status);
    const int code = exited ? WEXITSTATUS(status) : -1;
    INV(exited && (code == 0 || code == 137), i, "the child neither finished nor was killed by the hook");
    if (code == 137) {
      ++killed;
      if (point) ++kills[point];
      else ++reconcile_kills;
    } else {
      ++clean;
    }
    // A valid import must reach every boundary but the catalog-row one (an unchanged library writes no row) and link-created (no flip needed).
    const std::string pn = point ? point : "";
    const bool must_fire = kSources[s.source].valid && point && pn != "catalog-row-written" && pn != "link-created";
    if (must_fire) {
      ++expected_hits;
      INV(code == 137, i, "a boundary of a valid import was not reached (the hook is vacuous)");
    }

    // The restart: open the catalog, run the reconcile pass.
    std::unique_ptr<Catalog> cat;
    INV(Catalog::open(data, cat) == CatalogStatus::kOk, i, "the catalog does not open after a kill");
    if (!cat) break;
    ReconcileReport rep;
    INV(reconcile_libraries(*cat, data, rep) == CatalogStatus::kOk && rep.errors.empty(), i, "reconcile failed");
    INV(rep.rows_removed == 0, i, "reconcile removed a row (no library is ever removed in this script)");
    if (code == 137 && point) {
      if (pn == "stage-created" || pn == "samples-written" || pn == "verified" || pn == "fsynced")
        INV(rep.staging_removed == 1 && rep.rows_added + rep.rows_updated == 0, i, "a kill before the rename leaves exactly one staging directory and no catalog change");
      // An identical re-import refreshes catalog.meta / category inside the existing bundle BEFORE the flip (Story 5.5), so a kill after that may leave a
      // refreshed bundle under an unchanged `current`: reconcile then updates that one row to what the bundle says. A new library is never listed.
      if (pn == "renamed") INV(rep.rows_added == 0 && rep.rows_updated <= 1, i, "a kill before the flip lists no new library");
      if (pn == "link-created") INV(rep.tmp_links_removed == 1 && rep.rows_added == 0 && rep.rows_updated <= 1, i, "a kill with the temp link in place");
      if (pn == "catalog-committed" || pn == "job-finished") INV(!rep.changed(), i, "everything was committed: reconcile has nothing to repair");
      if (pn == "catalog-row-written") INV(rep.rows_added + rep.rows_updated <= 1, i, "an uncommitted catalog transaction is repaired by at most one row");
    }
    check_invariants(data, i, *cat);
    // Idempotence on every step.
    ReconcileReport rep2;
    INV(reconcile_libraries(*cat, data, rep2) == CatalogStatus::kOk && !rep2.changed(), i, "reconcile is not idempotent");
  }

  // (d) Re-running the whole script without faults converges: on the crashed tree and on a fresh one the published state is the same. Both replays read
  // the sources from the same `src` (a library_id is the UUIDv5 of the canonical source path, so equal paths give equal ids).
  auto replay = [&](const std::string& d, const std::string& sock_path) {
    std::map<int, unsigned> st;
    LibraryServiceOptions o;
    o.data_dir = d;
    o.socket_path = sock_path;
    LibraryService svc(o);
    std::string err;
    if (!svc.start(err)) {
      ++g_fail;
      std::printf("replay: cannot start: %s\n", err.c_str());
      return;
    }
    Client c;
    CHECK(c.connect(sock_path) && c.handshake("replay"));
    for (const Step& s : script) {
      materialize(src, s, st);
      auto r = c.call(request_for(src, s), 30000);
      CHECK(r && r->has_import_accepted());
      if (!r || !r->has_import_accepted()) return;
      const auto job = r->import_accepted().job_id();
      bool done = false;
      auto note = [&](const ws::v1::LibraryServerMessage& m) {
        if (m.has_import_progress() && m.import_progress().job_id() == job && terminal(m.import_progress().state())) done = true;
      };
      for (const auto& m : c.pushes) note(m);
      c.pushes.clear();
      while (!done) {
        auto m = c.recv(60000);
        if (!m) {
          ++g_fail;
          std::printf("replay: no terminal event\n");
          return;
        }
        note(*m);
      }
    }
    svc.stop();
  };
  replay(data, work.sub("r1"));
  replay(gold, work.sub("r2"));
  const Published a = snapshot(data), b = snapshot(gold);
  CHECK(!a.libs.empty() && a.libs.size() == b.libs.size());
  for (const auto& kv : b.libs) {
    CHECK(a.libs.count(kv.first) == 1);
    if (a.libs.count(kv.first)) CHECK(same_content(a.libs.at(kv.first), kv.second));
  }
  {
    std::unique_ptr<Catalog> cat;
    CHECK(Catalog::open(data, cat) == CatalogStatus::kOk);
    if (cat) check_invariants(data, kSteps, *cat);
  }

  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("crash campaign: %d imports, %d killed (%d inside the startup reconcile), %d ran clean, %d boundaries required to fire; kills per point:", kSteps, killed,
              reconcile_kills, clean, expected_hits);
  for (int p = 0; p < kPointCount; ++p) std::printf(" %s=%d", kPoints[p], kills[kPoints[p]]);
  std::printf("; %zu libraries published; %.1f s\n", a.libs.size(), secs);
  // Every boundary was exercised by the script (a vacuous campaign would pass otherwise).
  for (int p = 0; p < kPointCount; ++p) {
    const std::string pn = kPoints[p];
    if (pn == "link-created" || pn == "catalog-row-written") continue;  // reached only when a flip / a catalog change is needed; counted above when they fired
    CHECK(kills[pn] >= 3);
  }
  CHECK(kills["link-created"] >= 1 && kills["catalog-row-written"] >= 1);
  CHECK(reconcile_kills >= 1);
  std::printf(g_fail ? "librarian_crash_campaign: %d FAILED\n" : "librarian_crash_campaign: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
