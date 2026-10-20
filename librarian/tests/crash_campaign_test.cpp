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
//
// Story 5.8 (second part of main): 50 SIGKILLs of a real child process that imports a 7-timbre SoundFont in checkpoints of 2 (the `librarian_test_cli` binary,
// exec'ed, told by VELORA_LIBRARIAN_KILL=<point>@<n> where to kill itself). The 45 named points that the pipeline passes (7 stage points x 4 publishes, timbre-start and
// timbre-done x 7 timbres, checkpoint-published x 3) are each hit once, in a seeded shuffle, and 5 more drawn from the same seed make 50; each kill runs on a fresh root.
// After every kill: restart (the catalog opens, the reconcile pass runs), the Story 5.6 invariants hold, every bundle directory left under the library verifies, `current`
// names a bundle that holds the first k timbres of the clean import bit for bit, and a completed re-run ends in the clean import's bundle id (the content hash).
// A chain of 10 kills on ONE root and 20 kills of the service process (SIGKILL at timbre, stage and catalog points) follow the same checks.
#include <functional>
#include <cstring>
#include <sqlite3.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <set>

#include "import_fixtures.hpp"
#include "lib_test_util.hpp"
#include "velora/format/bundle.hpp"
#include "velora/format/hash.hpp"
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
  return s == ws::v1::IMPORT_STATE_PUBLISHED || s == ws::v1::IMPORT_STATE_FAILED || s == ws::v1::IMPORT_STATE_CANCELLED ||
         s == ws::v1::IMPORT_STATE_DRIVE_REMOVED;
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


// --- Story 5.8: SIGKILL campaign over checkpointed publishing --------------------------------------------------------------------------------------

constexpr unsigned kMultiTimbres = 7;
struct Tally {
  int none = 0, partial = 0, full = 0;  // what the kills left: no current bundle, a checkpoint, the complete library
};
constexpr const char* kStagePoints[] = {"stage-created", "staged", "verified", "fsynced", "renamed", "link-created", "flipped"};

struct CleanMulti {
  std::string bundle_id;
  std::vector<std::string> keys;
  std::vector<fmt::Digest256> hashes;  // first sample of each instrument
};

CleanMulti clean_multi(const fs::path& sf2, const std::string& root) {
  CleanMulti c;
  ImportOptions o;
  o.sfz_path = sf2.string();
  o.root = root;
  o.category = "piano";
  const ImportResult r = import_file(o);
  if (!r.ok()) {
    ++g_fail;
    std::printf("clean import failed\n");
    return c;
  }
  c.bundle_id = r.bundle_id;
  auto rd = fmt::BundleReader::open_file(r.bundle_dir + "/bundle.vlb");
  if (rd.ok())
    for (const auto& i : rd.value().instruments()) {
      c.keys.push_back(i.key);
      c.hashes.push_back(rd.value().samples()[i.first_sample].pcm_sha256);
    }
  return c;
}

// Runs the test CLI as a real child process; `kill_spec` empty = to completion. Returns the wait status.
int run_cli_child(const std::string& sf2, const std::string& root, const std::string& kill_spec) {
  const pid_t pid = ::fork();
  if (pid == 0) {
    if (!kill_spec.empty()) ::setenv("VELORA_LIBRARIAN_KILL", kill_spec.c_str(), 1);
    else ::unsetenv("VELORA_LIBRARIAN_KILL");
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      ::dup2(devnull, 1);
      ::dup2(devnull, 2);
    }
    const char* argv[] = {"librarian_test_cli", "import", sf2.c_str(), "--root", root.c_str(), "--category", "piano", "--checkpoint-timbres", "2", nullptr};
    ::execv(LIBRARIAN_TEST_EXE, const_cast<char* const*>(argv));
    ::_exit(127);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  return status;
}

bool killed_by_sigkill(int status) { return WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL; }

// The restart: open the catalog, run the reconcile pass, then the invariants of the story and of the campaign.
void check_after_kill(const std::string& root, int step, const fs::path& sf2, const CleanMulti& clean, Tally& tally, bool leases_expected = false) {
  std::unique_ptr<Catalog> cat;
  INV(Catalog::open(root, cat) == CatalogStatus::kOk, step, "the catalog does not open after a kill");
  if (!cat) return;
  ReconcileReport rep;
  INV(reconcile_libraries(*cat, root, rep) == CatalogStatus::kOk && rep.errors.empty(), step, "reconcile failed");
  check_invariants(root, step, *cat);
  ReconcileReport rep2;
  INV(reconcile_libraries(*cat, root, rep2) == CatalogStatus::kOk && !rep2.changed(), step, "reconcile is not idempotent");
  // Every bundle directory that is left is complete and verifies (a renamed-but-unflipped checkpoint is a whole bundle, never a partial one).
  const std::string lib_id = fmt::uuid_to_string(fmt::library_id_from_source(fs::weakly_canonical(sf2).generic_string()));
  const fs::path ldir = fs::path(root) / "lib" / lib_id;
  std::error_code ec;
  std::string cur;
  if (fs::is_symlink(ldir / "current", ec)) cur = fs::read_symlink(ldir / "current", ec).string();
  // After the restart ONLY the current bundle is left: superseded checkpoints and unflipped publishes are pruned (no lease exists in this campaign), and what is left
  // verifies and holds the first k timbres of the clean import, bit for bit.
  std::vector<std::string> dirs;
  for (fs::directory_iterator it(ldir, ec), end; !ec && it != end; it.increment(ec))
    if (it->path().filename().string().size() == 64) dirs.push_back(it->path().filename().string());
  // (A persistent root, the chain, also holds the unmarked bundles of earlier runs: those are leases and stay; a marked or unflipped one never does.)
  if (!leases_expected) INV(dirs.size() <= 1 && (dirs.empty() ? cur.empty() : dirs[0] == cur), step, "a bundle directory other than `current` is left after the restart");
  else INV(cur.empty() || std::find(dirs.begin(), dirs.end(), cur) != dirs.end(), step, "current names a missing bundle directory");
  for (const std::string& n : dirs) {
    auto rd = fmt::BundleReader::open_file((ldir / n / "bundle.vlb").string());
    INV(rd.ok() && rd.value().verify_all() == fmt::Error::kNone, step, "a bundle directory left on disk does not verify");
    INV(!fs::exists(ldir / n / kCheckpointMarkerName, ec), step, "a bundle left after the restart still carries the checkpoint marker");
    if (rd.ok()) {
      const auto ins = rd.value().instruments();
      INV(ins.size() >= 1 && ins.size() <= clean.keys.size(), step, "a bundle holds an impossible number of timbres");
      for (std::size_t i = 0; i < ins.size() && i < clean.keys.size(); ++i)
        INV(ins[i].key == clean.keys[i] && rd.value().samples()[ins[i].first_sample].pcm_sha256 == clean.hashes[i], step, "a published timbre differs from the clean import");
    }
  }
  if (cur.empty()) {
    ++tally.none;
  } else {
    auto cur_rd = fmt::BundleReader::open_file((ldir / cur / "bundle.vlb").string());
    if (cur_rd.ok() && cur_rd.value().instruments().size() == clean.keys.size()) ++tally.full;
    else ++tally.partial;
    INV(fs::exists(ldir / cur / "bundle.vlb", ec), step, "current names a missing bundle");
    std::vector<LibraryRecord> rows;
    INV(cat->list_libraries(rows, true) == CatalogStatus::kOk, step, "catalog readable");
    bool found = false;
    for (const auto& r : rows)
      if (r.library_id == lib_id) {
        found = true;
        INV(r.bundle_id == cur, step, "the catalog row does not name the current bundle");
      }
    INV(found, step, "a library with a current bundle is not listed after the restart");
  }
}

// Completes the import (a clean child) and checks that the end state is the clean import's content hash and that the catalog agrees.
void complete_and_compare(const std::string& root, int step, const fs::path& sf2, const CleanMulti& clean) {
  const int done = run_cli_child(sf2.string(), root, "");
  INV(WIFEXITED(done) && WEXITSTATUS(done) == 0, step, "the completing run did not succeed");
  std::unique_ptr<Catalog> cat;
  INV(Catalog::open(root, cat) == CatalogStatus::kOk, step, "the catalog does not open for the completing run");
  if (!cat) return;
  ReconcileReport rep;
  INV(reconcile_libraries(*cat, root, rep) == CatalogStatus::kOk && rep.errors.empty(), step, "reconcile after the completing run failed");
  const std::string lib_id = fmt::uuid_to_string(fmt::library_id_from_source(fs::weakly_canonical(sf2).generic_string()));
  std::error_code ec;
  const fs::path link = fs::path(root) / "lib" / lib_id / "current";
  INV(fs::is_symlink(link, ec) && fs::read_symlink(link, ec).string() == clean.bundle_id, step, "after the completing run `current` is not the clean import's bundle");
  std::vector<LibraryRecord> rows;
  INV(cat->list_libraries(rows, true) == CatalogStatus::kOk && rows.size() == 1 && rows[0].bundle_id == clean.bundle_id &&
          rows[0].instrument_count() == kMultiTimbres,
      step, "the catalog does not list the complete library");
  check_invariants(root, step, *cat);
}

// One service process (forked) imports the SoundFont with checkpoints of 2 and kills itself with SIGKILL at the n-th arrival at `point` (a timbre, stage or catalog point).
int run_service_child(const std::string& root, const std::string& sock, const fs::path& sf2, const std::string& point, int n) {
  const pid_t pid = ::fork();
  if (pid == 0) {
    auto count = std::make_shared<int>(0);
    auto arrive = [=](const char* p) {
      if (point == p && ++*count == n) ::kill(::getpid(), SIGKILL);
    };
    LibraryServiceOptions o;
    o.data_dir = root;
    o.socket_path = sock;
    o.checkpoint_timbres = 2;
    o.import_point_hook = [arrive](const char* p, std::uint64_t, std::size_t) { arrive(p); };
    o.import_timbre_hook = [arrive](const char* p, std::uint64_t, std::size_t) { arrive(p); };
    LibraryService svc(o);
    std::string err;
    if (!svc.start(err)) ::_exit(21);
    Client c;
    if (!c.connect(sock) || !c.handshake("kill")) ::_exit(22);
    ws::v1::LibraryClientMessage m;
    auto* r = m.mutable_import_request();
    r->set_source(sf2.string());
    r->add_items()->set_category(1);
    auto a = c.call(m, 30000);
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

void multi_timbre_campaign() {
  ltest::TempDir work;
  fs::create_directories(work.sub("src"));
  const fs::path sf2 = ifx::write_many_sf2(fs::path(work.sub("src")) / "multi.sf2", kMultiTimbres);
  fs::create_directories(work.sub("clean"));
  const CleanMulti clean = clean_multi(sf2, work.sub("clean"));
  CHECK(clean.keys.size() == kMultiTimbres && !clean.bundle_id.empty());

  struct Pt {
    std::string name;
    int n;
  };
  std::vector<Pt> pts;
  for (const char* sp : kStagePoints)
    for (int n = 1; n <= 4; ++n) pts.push_back({sp, n});  // three checkpoints and the final bundle
  for (int n = 1; n <= static_cast<int>(kMultiTimbres); ++n) {
    pts.push_back({"timbre-start", n});
    pts.push_back({"timbre-done", n});
  }
  for (int n = 1; n <= 3; ++n) pts.push_back({"checkpoint-published", n});
  CHECK(pts.size() == 45);
  Rng r{kSeed ^ 0x58ULL};
  for (std::size_t i = pts.size() - 1; i > 0; --i) std::swap(pts[i], pts[r.below(static_cast<unsigned>(i + 1))]);
  std::vector<Pt> plan = pts;
  for (int extra = 0; extra < 5; ++extra) plan.push_back(pts[r.below(static_cast<unsigned>(pts.size()))]);
  CHECK(plan.size() == 50);

  std::map<std::string, int> hit;
  int kills = 0, chain_kills = 0;
  Tally cli_tally, chain_tally, service_tally;
  for (std::size_t i = 0; i < plan.size(); ++i) {
    const int step = 1000 + static_cast<int>(i);
    const std::string root = work.sub("k" + std::to_string(i));
    fs::create_directories(root);
    const int st = run_cli_child(sf2.string(), root, plan[i].name + "@" + std::to_string(plan[i].n));
    INV(killed_by_sigkill(st), step, "the child was not killed by SIGKILL at its point (the kill point was not reached)");
    if (killed_by_sigkill(st)) {
      ++kills;
      ++hit[plan[i].name];
    }
    check_after_kill(root, step, sf2, clean, cli_tally);
    complete_and_compare(root, step, sf2, clean);
  }
  CHECK(kills == 50);
  for (const char* sp : kStagePoints) CHECK(hit[sp] >= 3);
  CHECK(hit["timbre-start"] >= 3 && hit["timbre-done"] >= 3 && hit["checkpoint-published"] >= 2);

  // A chain: ten kills on ONE root (a library that already has a partial bundle is not checkpointed again, so later kills see other pipelines), then a completed run.
  {
    const std::string root = work.sub("chain");
    fs::create_directories(root);
    for (int i = 0; i < 10; ++i) {
      const Pt& p = pts[r.below(static_cast<unsigned>(pts.size()))];
      const int st = run_cli_child(sf2.string(), root, p.name + "@" + std::to_string(p.n));
      INV(WIFEXITED(st) || killed_by_sigkill(st), 2000 + i, "the chained child neither exited nor was killed");
      if (killed_by_sigkill(st)) ++chain_kills;
      check_after_kill(root, 2000 + i, sf2, clean, chain_tally, true);
    }
    complete_and_compare(root, 2010, sf2, clean);
  }

  // The service process: SIGKILL at timbre, stage and catalog points (the catalog rows of a checkpoint are written between its publish and the next timbre).
  struct SPt {
    const char* name;
    int n;
  };
  const SPt spts[] = {{"timbre-done", 3},         {"timbre-start", 5},        {"checkpoint-published", 1}, {"checkpoint-published", 3}, {"catalog-row-written", 1},
                      {"catalog-row-written", 2}, {"catalog-row-written", 4}, {"catalog-committed", 1},     {"catalog-committed", 3},    {"catalog-committed", 4},
                      {"stage-created", 2},       {"samples-written", 3},     {"verified", 2},             {"fsynced", 4},              {"renamed", 1},
                      {"renamed", 3},             {"link-created", 2},        {"link-flipped", 1},         {"link-flipped", 3},         {"job-finished", 1}};
  int service_kills = 0;
  for (std::size_t i = 0; i < sizeof spts / sizeof spts[0]; ++i) {
    const int step = 3000 + static_cast<int>(i);
    const std::string root = work.sub("s" + std::to_string(i));
    fs::create_directories(root);
    const int st = run_service_child(root, work.sub("sock" + std::to_string(i)), sf2, spts[i].name, spts[i].n);
    INV(killed_by_sigkill(st), step, "the service child was not killed at its point");
    if (killed_by_sigkill(st)) ++service_kills;
    // The restart of the service itself (it reconciles on start) and then the same checks.
    check_after_kill(root, step, sf2, clean, service_tally);
    complete_and_compare(root, step, sf2, clean);
  }
  CHECK(service_kills == static_cast<int>(sizeof spts / sizeof spts[0]));
  // The kills really left partial libraries in every phase (a vacuous campaign would pass otherwise).
  CHECK(cli_tally.none >= 3 && cli_tally.partial >= 20 && cli_tally.full >= 1);
  CHECK(chain_kills >= 3 && chain_tally.none + chain_tally.partial + chain_tally.full == 10);
  CHECK(service_tally.none >= 1 && service_tally.partial >= 6 && service_tally.full >= 2);
  std::printf("multi-timbre campaign: states left by the CLI kills: %d without a current bundle, %d with a checkpoint, %d complete; chain: %d of 10 steps killed (%d/%d/%d); service: %d/%d/%d\n",
              cli_tally.none, cli_tally.partial, cli_tally.full, chain_kills, chain_tally.none, chain_tally.partial, chain_tally.full, service_tally.none, service_tally.partial,
              service_tally.full);
  std::printf("multi-timbre campaign: %d SIGKILLs of the CLI at %zu distinct points (+5 repeats), 10 chained, %d of the service; every restart reconciled, every re-run equals the clean import\n",
              kills, pts.size(), service_kills);
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

  multi_timbre_campaign();

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
