// Migration runner (Story 8.5, FR69, AD-12): read-only before the slot is good, atomic migration to N after, never N+1, rollback leaves bytes identical,
// kill mid-migration never corrupts, the migrated data opens on this release (N) and on the next (as its N-1). The synthetic migration moves a Live
// Set file from format_version 1 to 2 (N = 2 in this test) through the real Live Set reader / writer; no real irreversible migration exists yet.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "live_set_fixtures.hpp"
#include "velora/host/migration_runner.hpp"

namespace fs = std::filesystem;
namespace lsn = velora::host::liveset;
namespace mig = velora::host::migration;

namespace {
int g_fail = 0;
#define CHECK(c)                                                       \
  do {                                                                 \
    if (!(c)) {                                                        \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);         \
      ++g_fail;                                                        \
    }                                                                  \
  } while (0)

std::string slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
void spit(const std::string& p, const std::string& b) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(b.data(), static_cast<std::streamsize>(b.size()));
}
std::size_t entries(const std::string& dir) {
  std::size_t n = 0;
  for (const auto& e : fs::directory_iterator(dir)) { (void)e; ++n; }
  return n;
}

struct TmpDir {
  std::string path;
  TmpDir() {
    std::string t = (fs::temp_directory_path() / "velora_migration_XXXXXX").string();
    std::vector<char> b(t.begin(), t.end());
    b.push_back('\0');
    path = mkdtemp(b.data());
  }
  ~TmpDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

// The synthetic unit: Live Set file `a.liveset`, N-1 = 1 -> N = 2 (read through the reader at N = 2, written back as N with unknown fields kept).
mig::Unit live_set_unit(std::uint32_t to = 2) {
  mig::Unit u;
  u.name = "liveset-1-to-2";
  u.file = "a.liveset";
  u.from_version = to - 1;
  u.to_version = to;
  u.version_of = [](std::string_view b, std::uint32_t& v) {  // the format's version word, read without any policy
    velora::liveset::v1::LiveSet m;
    if (!m.ParseFromArray(b.data(), static_cast<int>(b.size())) || m.format_version() == 0) return false;
    v = m.format_version();
    return true;
  };
  u.migrate = [to](std::string_view in, std::string& out) {
    lsn::ReadOptions ro;
    ro.part_count = 0;
    ro.current_version = to;
    lsn::LoadResult lr;
    if (lsn::parse(in, ro, lr) != lsn::Status::kOk) return false;
    return lsn::serialize(lr.live_set, out) == lsn::Status::kOk;
  };
  return u;
}

std::string v1_file() {
  velora::liveset::v1::LiveSet l = fixtures::minimal();
  l.set_format_version(1);
  fixtures::unknown_varint(&l, 500, 42);
  std::string b;
  CHECK(lsn::serialize(l, b) == lsn::Status::kOk);
  return b;
}

mig::Options opts(const std::string& dir, bool good) {
  mig::Options o;
  o.dir = dir;
  o.current_version = 2;
  o.slot_good = [good] { return good; };
  return o;
}

bool opens(const std::string& bytes, std::uint32_t current, bool* migrated = nullptr) {
  lsn::ReadOptions ro;
  ro.part_count = 0;
  ro.current_version = current;
  lsn::LoadResult lr;
  const bool ok = lsn::parse(bytes, ro, lr) == lsn::Status::kOk && lr.live_set.GetReflection()->GetUnknownFields(lr.live_set).field_count() == 1;
  if (migrated) *migrated = lr.migrated;
  return ok;
}

void test_before_and_after_mark_good() {
  TmpDir d;
  const std::string path = d.path + "/a.liveset";
  const std::string old = v1_file();
  spit(path, old);
  const std::string before = slurp(path);

  // The new release boots on N-1 data, the slot is not good: the N-1 reader serves it, nothing is written (not even a temp file).
  int probes = 0;
  mig::Options o = opts(d.path, false);
  o.slot_good = [&] { ++probes; return false; };
  mig::Report r = mig::run(o, {live_set_unit()});
  CHECK(r.units.size() == 1 && r.units[0].outcome == mig::Outcome::kDeferred && r.any_pending() && probes == 1);
  CHECK(r.units[0].version_before == 1 && r.units[0].version_after == 1);
  CHECK(slurp(path) == before && entries(d.path) == 1);
  CHECK(opens(slurp(path), 2));  // the release at N still reads it (as N-1)
  // Rollback: the slot is never marked good, the previous firmware (N-1 = 1) reads the data byte-identical.
  r = mig::run(opts(d.path, false), {live_set_unit()});  // another boot, still not good
  CHECK(r.units[0].outcome == mig::Outcome::kDeferred && slurp(path) == before);
  bool migrated = true;
  CHECK(opens(slurp(path), 1, &migrated) && !migrated);
  // An empty probe fails closed.
  mig::Options none = opts(d.path, true);
  none.slot_good = nullptr;
  CHECK(mig::run(none, {live_set_unit()}).units[0].outcome == mig::Outcome::kDeferred && slurp(path) == before);

  // After mark-good the unit runs: atomic write of N, nothing else left in the directory.
  r = mig::run(opts(d.path, true), {live_set_unit()});
  CHECK(r.units[0].outcome == mig::Outcome::kMigrated && r.units[0].version_after == 2 && r.slot_good && !r.any_pending());
  const std::string after = slurp(path);
  CHECK(after != before && entries(d.path) == 1);
  CHECK(opens(after, 2, &migrated) && !migrated);   // this release reads it as N
  CHECK(opens(after, 3, &migrated) && migrated);    // the NEXT release reads it as its N-1 (the "old release on migrated data" case)
  lsn::LoadResult lr;
  lsn::ReadOptions n2;
  n2.part_count = 0;
  n2.current_version = 2;
  CHECK(lsn::parse(after, n2, lr) == lsn::Status::kOk && lr.file_version == 2 && lr.live_set.format_version() == 2);
  // Idempotent: a second run finds N and does nothing; the probe is not even asked.
  int probes2 = 0;
  mig::Options o2 = opts(d.path, true);
  o2.slot_good = [&] { ++probes2; return true; };
  r = mig::run(o2, {live_set_unit()});
  CHECK(r.units[0].outcome == mig::Outcome::kUpToDate && probes2 == 0 && slurp(path) == after);
}

void test_never_writes_other_than_n() {
  TmpDir d;
  spit(d.path + "/a.liveset", v1_file());
  const std::string before = slurp(d.path + "/a.liveset");
  // A unit that targets N+1 (or anything but the running N) is refused untouched, the slot being good or not.
  mig::Report r = mig::run(opts(d.path, true), {live_set_unit(3)});
  CHECK(r.units[0].outcome == mig::Outcome::kBadUnit && slurp(d.path + "/a.liveset") == before && !r.slot_probed);
  // A migration that produces the wrong version is refused, the old file intact.
  mig::Unit bad = live_set_unit();
  bad.migrate = [](std::string_view in, std::string& out) { out.assign(in); return true; };  // still version 1
  r = mig::run(opts(d.path, true), {bad});
  CHECK(r.units[0].outcome == mig::Outcome::kFailed && slurp(d.path + "/a.liveset") == before && entries(d.path) == 1);
  mig::Unit refuse = live_set_unit();
  refuse.migrate = [](std::string_view, std::string&) { return false; };
  CHECK(mig::run(opts(d.path, true), {refuse}).units[0].outcome == mig::Outcome::kFailed && slurp(d.path + "/a.liveset") == before);
  // Malformed file names.
  mig::Unit nm = live_set_unit();
  nm.file = "../a.liveset";
  CHECK(mig::run(opts(d.path, true), {nm}).units[0].outcome == mig::Outcome::kBadUnit);
  // Too old / too new / unreadable data is left alone with a typed outcome.
  for (std::uint32_t v : {0u, 3u}) {
    velora::liveset::v1::LiveSet l = fixtures::minimal();
    l.set_format_version(v);
    std::string b;
    lsn::serialize(l, b);
    spit(d.path + "/a.liveset", b);
    mig::Report rr = mig::run(opts(d.path, true), {live_set_unit()});
    CHECK(rr.units[0].outcome == mig::Outcome::kNotMigratable && slurp(d.path + "/a.liveset") == b);
  }
  spit(d.path + "/a.liveset", "garbage");
  CHECK(mig::run(opts(d.path, true), {live_set_unit()}).units[0].outcome == mig::Outcome::kNotMigratable && slurp(d.path + "/a.liveset") == "garbage");
  fs::remove(d.path + "/a.liveset");
  CHECK(mig::run(opts(d.path, true), {live_set_unit()}).units[0].outcome == mig::Outcome::kAbsent);
}

void test_kill_mid_migration() {
  using Stage = lsn::Stage;
  for (Stage kill : {Stage::kTempWritten, Stage::kFsynced, Stage::kRenamed, Stage::kDirSynced}) {
    TmpDir d;
    const std::string path = d.path + "/a.liveset";
    const std::string old = v1_file();
    spit(path, old);
    mig::Options o = opts(d.path, true);
    o.on_stage = [kill](Stage s) { return s == kill ? lsn::HookAction::kAbort : lsn::HookAction::kContinue; };
    mig::Report r = mig::run(o, {live_set_unit()});
    const bool after_rename = kill == Stage::kRenamed || kill == Stage::kDirSynced;
    CHECK(r.units[0].outcome == (after_rename ? mig::Outcome::kMigratedNotDurable : mig::Outcome::kFailed));  // the process "died" here
    const std::string now = slurp(path);
    const bool is_old = now == old;
    bool migrated = false;
    CHECK(opens(now, 2, &migrated));                       // old or new, never corrupt
    CHECK(is_old == migrated);                             // old file = N-1 (migrated in memory), new file = N
    CHECK(kill == Stage::kTempWritten || kill == Stage::kFsynced ? is_old : !is_old);  // before the rename the old file stands; after it the new one
    // The next boot (slot good) finishes the job whichever side of the rename the kill fell on.
    CHECK(fs::exists(path + ".migrate.tmp") == !after_rename);  // a kill before the rename leaves a stale temp; the next run removes it
    r = mig::run(opts(d.path, true), {live_set_unit()});
    CHECK(r.units[0].outcome == (is_old ? mig::Outcome::kMigrated : mig::Outcome::kUpToDate));
    CHECK(opens(slurp(path), 2, &migrated) && !migrated && !fs::exists(path + ".migrate.tmp"));
  }
  // Disk full and a failed directory fsync.
  TmpDir d;
  const std::string path = d.path + "/a.liveset";
  const std::string old = v1_file();
  spit(path, old);
  mig::Options full = opts(d.path, true);
  full.inject_write_errno = ENOSPC;
  mig::Report r = mig::run(full, {live_set_unit()});
  CHECK(r.units[0].outcome == mig::Outcome::kFailed && slurp(path) == old && entries(d.path) == 1);
  mig::Options nd = opts(d.path, true);
  nd.inject_dir_sync_failure = true;
  r = mig::run(nd, {live_set_unit()});
  CHECK(r.units[0].outcome == mig::Outcome::kMigratedNotDurable && r.units[0].version_after == 2 && opens(slurp(path), 2));
}

void test_robustness() {
  // Several units in one run: the probe is asked once, one failure does not stop the next, the probe may be true on a later run.
  {
    TmpDir d;
    spit(d.path + "/a.liveset", v1_file());
    spit(d.path + "/b.liveset", v1_file());
    spit(d.path + "/c.liveset", v1_file());
    mig::Unit a = live_set_unit(), b = live_set_unit(), c = live_set_unit();
    a.name = "a"; b.name = "b"; b.file = "b.liveset"; c.name = "c"; c.file = "c.liveset";
    b.migrate = [](std::string_view, std::string&) { return false; };
    int probes = 0;
    mig::Options o = opts(d.path, true);
    o.slot_good = [&] { ++probes; return true; };
    mig::Report r = mig::run(o, {a, b, c});
    CHECK(probes == 1 && r.units[0].outcome == mig::Outcome::kMigrated && r.units[1].outcome == mig::Outcome::kFailed && r.units[2].outcome == mig::Outcome::kMigrated);
    bool good = false;
    spit(d.path + "/a.liveset", v1_file());
    mig::Options later = opts(d.path, false);
    later.slot_good = [&] { return good; };
    CHECK(mig::run(later, {a}).units[0].outcome == mig::Outcome::kDeferred);
    good = true;
    CHECK(mig::run(later, {a}).units[0].outcome == mig::Outcome::kMigrated);
  }
  // max_bytes on input and output; symlink and directory sources.
  {
    TmpDir d;
    const std::string old = v1_file();
    spit(d.path + "/a.liveset", old);
    mig::Unit small = live_set_unit();
    small.max_bytes = 8;
    CHECK(mig::run(opts(d.path, true), {small}).units[0].outcome == mig::Outcome::kNotMigratable && slurp(d.path + "/a.liveset") == old);
    mig::Unit tight = live_set_unit();
    tight.max_bytes = old.size();  // input fits, the output (version word and all) must too: make it larger on purpose
    tight.migrate = [&old](std::string_view in, std::string& out) { out.assign(in); out.append(old.size(), 'x'); return true; };
    CHECK(mig::run(opts(d.path, true), {tight}).units[0].outcome == mig::Outcome::kFailed && slurp(d.path + "/a.liveset") == old);
    fs::create_symlink(d.path + "/a.liveset", d.path + "/link.liveset");
    mig::Unit ln = live_set_unit();
    ln.file = "link.liveset";
    CHECK(mig::run(opts(d.path, true), {ln}).units[0].outcome == mig::Outcome::kNotMigratable && slurp(d.path + "/a.liveset") == old);
    fs::create_directory(d.path + "/dir.liveset");
    mig::Unit dr = live_set_unit();
    dr.file = "dir.liveset";
    CHECK(mig::run(opts(d.path, true), {dr}).units[0].outcome == mig::Outcome::kNotMigratable);
  }
  // Throwing callbacks and the validate hook; overflow-safe unit shape.
  {
    TmpDir d;
    const std::string old = v1_file();
    spit(d.path + "/a.liveset", old);
    mig::Unit t = live_set_unit();
    t.migrate = [](std::string_view, std::string&) -> bool { throw 1; };
    CHECK(mig::run(opts(d.path, true), {t}).units[0].outcome == mig::Outcome::kFailed);
    mig::Unit tv = live_set_unit();
    tv.version_of = [](std::string_view, std::uint32_t&) -> bool { throw std::runtime_error("x"); };
    CHECK(mig::run(opts(d.path, true), {tv}).units[0].outcome == mig::Outcome::kFailed);
    mig::Unit val = live_set_unit();
    val.validate = [](std::string_view) { return false; };
    CHECK(mig::run(opts(d.path, true), {val}).units[0].outcome == mig::Outcome::kFailed && slurp(d.path + "/a.liveset") == old && entries(d.path) == 1);
    val.validate = [](std::string_view o) { return !o.empty(); };
    CHECK(mig::run(opts(d.path, true), {val}).units[0].outcome == mig::Outcome::kMigrated);
    spit(d.path + "/a.liveset", old);
    mig::Unit zero = live_set_unit();
    zero.from_version = 0;
    zero.to_version = 1;
    mig::Options o1 = opts(d.path, true);
    o1.current_version = 1;
    CHECK(mig::run(o1, {zero}).units[0].outcome == mig::Outcome::kBadUnit);
    mig::Unit wrap = live_set_unit();
    wrap.from_version = 0xFFFFFFFFu;
    wrap.to_version = 0;
    mig::Options o0 = opts(d.path, true);
    o0.current_version = 0;
    CHECK(mig::run(o0, {wrap}).units[0].outcome == mig::Outcome::kBadUnit && slurp(d.path + "/a.liveset") == old);
  }
  // Concurrent change between read and rename: refused, the other writer's file stands, no temp left.
  {
    TmpDir d;
    const std::string path = d.path + "/a.liveset";
    spit(path, v1_file());
    velora::liveset::v1::LiveSet l = fixtures::minimal();
    l.set_format_version(1);
    l.set_name("Edited meanwhile");
    fixtures::unknown_varint(&l, 500, 42);
    std::string other;
    CHECK(lsn::serialize(l, other) == lsn::Status::kOk);
    mig::Options o = opts(d.path, true);
    o.on_stage = [&](lsn::Stage st) {
      if (st == lsn::Stage::kTempWritten) spit(path, other);  // a concurrent writer replaces the source after the migration read it
      return lsn::HookAction::kContinue;
    };
    mig::Report r = mig::run(o, {live_set_unit()});
    CHECK(r.units[0].outcome == mig::Outcome::kFailed && slurp(path) == other && entries(d.path) == 1);
  }
}

}  // namespace

int main() {
  test_before_and_after_mark_good();
  test_never_writes_other_than_n();
  test_kill_mid_migration();
  test_robustness();
  std::printf(g_fail ? "migration_runner_test: %d FAILED\n" : "migration_runner_test: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
