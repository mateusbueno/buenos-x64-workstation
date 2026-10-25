// Journaled Boot-into pointer tests (Story 8.8): format, load fallback, atomic write with kill injection at every stage (1,000 simulated + real
// process kills), identical-skip, the idle-safe policy table with a fake clock, no write while a note is held or an import verifies, and a long
// run. No wall-clock assertions.
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include "velora/core/snapshot.hpp"
#include "velora/host/boot_into_store.hpp"

namespace fs = std::filesystem;
namespace bi = velora::host::bootinto;
namespace ls = velora::host::liveset;

static int g_fail = 0;
#define CHECK(c)                                               \
  do {                                                         \
    if (!(c)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

namespace {

std::string slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
void spit(const std::string& path, const std::string& b) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(b.data(), static_cast<std::streamsize>(b.size()));
}
struct TmpDir {
  std::string path;
  TmpDir() {
    std::string t = (fs::temp_directory_path() / "velora-bootinto-XXXXXX").string();
    if (!::mkdtemp(t.data())) std::abort();
    path = t;
  }
  ~TmpDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

bi::Pointer ptr(const std::string& live, std::uint32_t scene, const std::string& setlist = "sl-1") {
  bi::Pointer p;
  p.live_set_id = live;
  p.scene_index = scene;
  p.setlist_id = setlist;
  return p;
}

constexpr std::uint64_t kSec = 1'000'000'000ull;

void test_format() {
  const bi::Pointer p = ptr("0b9c1a52-4c1e-4a39-9d50-6a0d8f0c1e11", 3, "5f1c1a52-4c1e-4a39-9d50-6a0d8f0c1e22");
  const std::string b = bi::serialize(p);
  CHECK(b.size() <= bi::kMaxFileBytes && b.size() < 512);
  bi::Pointer q;
  CHECK(bi::parse(b, q) && q == p && q.schema_version == bi::kSchemaVersion);
  // no setlist is valid
  const bi::Pointer none = ptr("a", 0, "");
  CHECK(bi::parse(bi::serialize(none), q) && q == none);
  // an invalid pointer (no Live Set, bad id) is not valid
  CHECK(!bi::valid_pointer(ptr("", 0)));
  CHECK(!bi::valid_pointer(ptr("bad id\n", 0)));
  CHECK(!bi::valid_pointer(ptr(std::string(65, 'a'), 0)));
}

void test_load_fallback() {
  TmpDir d;
  bi::Pointer out = ptr("keep", 9);
  CHECK(bi::load(d.path, out) == bi::LoadStatus::kNoPointer);  // missing
  const std::string file = d.path + "/" + bi::kFileName;
  const std::string good = bi::serialize(ptr("ls-a", 2));
  spit(file, good);
  CHECK(bi::load(d.path, out) == bi::LoadStatus::kOk && out == ptr("ls-a", 2));
  // truncated at every length
  for (std::size_t n = 0; n < good.size(); ++n) {
    spit(file, good.substr(0, n));
    bi::Pointer o;
    CHECK(bi::load(d.path, o) == bi::LoadStatus::kNoPointer);
  }
  // every single-byte flip is caught (CRC or field check)
  for (std::size_t i = 0; i < good.size(); ++i) {
    std::string m = good;
    m[i] = static_cast<char>(m[i] ^ 0x01);
    spit(file, m);
    bi::Pointer o;
    const bool ok = bi::load(d.path, o) == bi::LoadStatus::kOk;
    CHECK(!ok || o == ptr("ls-a", 2));  // never a different pointer
    CHECK(!ok);
  }
  // unknown schema (with a self-consistent CRC)
  bi::Pointer future = ptr("ls-a", 2);
  future.schema_version = 2;
  spit(file, bi::serialize(future));
  CHECK(bi::load(d.path, out) == bi::LoadStatus::kNoPointer);
  // trailing bytes, empty, garbage, oversized, a directory in place of the file
  spit(file, good + "x");
  CHECK(bi::load(d.path, out) == bi::LoadStatus::kNoPointer);
  spit(file, "");
  CHECK(bi::load(d.path, out) == bi::LoadStatus::kNoPointer);
  spit(file, std::string(100, '\xff'));
  CHECK(bi::load(d.path, out) == bi::LoadStatus::kNoPointer);
  spit(file, std::string(bi::kMaxFileBytes + 1, 'a'));
  CHECK(bi::load(d.path, out) == bi::LoadStatus::kNoPointer);
  fs::remove(file);
  fs::create_directory(file);
  CHECK(bi::load(d.path, out) == bi::LoadStatus::kNoPointer);
}

void test_identical_skip() {
  TmpDir d;
  bi::BootIntoStore s({d.path});
  CHECK(!s.has_stored());
  bool wrote = false;
  CHECK(s.write(ptr("a", 1), &wrote) == ls::Status::kOk && wrote && s.writes() == 1);
  CHECK(s.write(ptr("a", 1), &wrote) == ls::Status::kOk && !wrote && s.writes() == 1);
  CHECK(s.write(ptr("a", 2), &wrote) == ls::Status::kOk && wrote && s.writes() == 2);
  bi::BootIntoStore again({d.path});  // a fresh store reads it back
  CHECK(again.has_stored() && again.stored() == ptr("a", 2));
  CHECK(again.write(ptr("a", 2), &wrote) == ls::Status::kOk && !wrote && again.writes() == 0);
  CHECK(s.write(ptr("", 0), &wrote) == ls::Status::kInvalid && !wrote);
  CHECK(!fs::exists(d.path + "/" + bi::kTmpFileName));
}

void test_write_errors() {
  TmpDir d;
  bi::StoreOptions o{d.path};
  o.inject_write_errno = ENOSPC;
  bi::BootIntoStore s(o);
  CHECK(s.write(ptr("a", 1)) == ls::Status::kNoSpace);
  CHECK(!s.has_stored() && !fs::exists(d.path + "/" + bi::kFileName) && !fs::exists(d.path + "/" + bi::kTmpFileName));
  bi::StoreOptions o2{d.path};
  o2.inject_dir_sync_failure = true;
  bi::BootIntoStore s2(o2);
  CHECK(s2.write(ptr("a", 1)) == ls::Status::kCommittedNotDurable);
  CHECK(!s2.has_stored());  // not durable: the caller retries
}

// Kill at each stage, 1,000 iterations: the file is then the previous or the new pointer, never corrupt.
void test_kill_injection() {
  TmpDir d;
  const ls::Stage stages[] = {ls::Stage::kTempWritten, ls::Stage::kFsynced, ls::Stage::kRenamed, ls::Stage::kDirSynced};
  bi::Pointer prev = ptr("seed", 0);
  {
    bi::BootIntoStore s({d.path});
    CHECK(s.write(prev) == ls::Status::kOk);
  }
  int seen_prev = 0, seen_next = 0;
  for (int i = 0; i < 1000; ++i) {
    const int mode = i % 6;  // 0..3: abort at that stage, 4: no kill, 5: killed before the first stage hook (nothing written at all)
    const bi::Pointer next = ptr("live-" + std::to_string(i), static_cast<std::uint32_t>(i % 8));
    bi::StoreOptions o{d.path};
    if (mode < 4) {
      const ls::Stage at = stages[mode];
      o.on_stage = [at](ls::Stage st) { return st == at ? ls::HookAction::kAbort : ls::HookAction::kContinue; };
    }
    bi::BootIntoStore s(o);
    CHECK(s.has_stored() && s.stored() == prev);
    if (mode == 5) {
      // died before touching the disk
    } else {
      const ls::Status st = s.write(next);
      CHECK(mode == 4 ? st == ls::Status::kOk : st == ls::Status::kFaultInjected);
    }
    bi::Pointer got;
    const bool ok = bi::load(d.path, got) == bi::LoadStatus::kOk;
    CHECK(ok);  // a previous pointer existed, so there is always one
    CHECK(got == prev || got == next);
    if (got == next) {
      ++seen_next;
      prev = next;
    } else {
      ++seen_prev;
    }
  }
  CHECK(seen_prev > 0 && seen_next > 0);
}

// Real process kills (the hook _exit()s) at every stage.
void test_real_kill() {
  TmpDir d;
  bi::Pointer prev = ptr("seed", 0);
  {
    bi::BootIntoStore s({d.path});
    CHECK(s.write(prev) == ls::Status::kOk);
  }
  const ls::Stage stages[] = {ls::Stage::kTempWritten, ls::Stage::kFsynced, ls::Stage::kRenamed, ls::Stage::kDirSynced};
  for (int i = 0; i < 40; ++i) {
    const ls::Stage at = stages[i % 4];
    const bi::Pointer next = ptr("kill-" + std::to_string(i), 1);
    const pid_t pid = ::fork();
    if (pid == 0) {
      bi::StoreOptions o{d.path};
      o.on_stage = [at](ls::Stage st) -> ls::HookAction {
        if (st == at) ::_exit(0);
        return ls::HookAction::kContinue;
      };
      bi::BootIntoStore s(o);
      s.write(next);
      ::_exit(1);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    bi::Pointer got;
    CHECK(bi::load(d.path, got) == bi::LoadStatus::kOk);
    CHECK(got == prev || got == next);
    if (got == next) prev = next;
  }
}

struct Rig {
  TmpDir dir;
  std::uint64_t now = 100 * kSec;
  std::uint32_t notes = 0;
  bool pedal = false, verifying = false;
  bi::BootIntoStore store;
  bi::BootIntoTracker tracker;
  Rig()
      : store(bi::StoreOptions{dir.path}),
        tracker(store, bi::TrackerProbes{[this] { return now; }, [this] { return notes; }, [this] { return pedal; }, [this] { return verifying; }}) {}
  void advance(std::uint64_t ns, bool tick_each_20ms = true) {
    const std::uint64_t end = now + ns;
    while (now < end) {
      now += 20'000'000ull;
      if (tick_each_20ms) tracker.tick();
    }
  }
};

void test_policy() {
  {  // Change: dirty in memory, no file write
    Rig r;
    r.advance(10 * kSec);
    r.tracker.mark_dirty(ptr("a", 1));
    CHECK(r.tracker.dirty() && r.store.writes() == 0 && !fs::exists(r.dir.path + "/" + bi::kFileName));
  }
  {  // Idle 2 s: one atomic write, and a second identical state writes nothing
    Rig r;
    r.advance(5 * kSec);
    r.notes = 1;
    r.advance(100'000'000ull);
    r.notes = 0;
    r.tracker.mark_dirty(ptr("a", 1));
    r.advance(1900'000'000ull);
    CHECK(r.store.writes() == 0);
    r.advance(300'000'000ull);
    CHECK(r.store.writes() == 1 && !r.tracker.dirty());
    r.tracker.mark_dirty(ptr("a", 1));  // identical to what is stored
    r.advance(5 * kSec);
    CHECK(r.store.writes() == 1 && !r.tracker.dirty());
    bi::Pointer got;
    CHECK(bi::load(r.dir.path, got) == bi::LoadStatus::kOk && got == ptr("a", 1));
  }
  {  // Many changes while idle collapse into one write of the last one
    Rig r;
    r.advance(5 * kSec);
    for (int i = 0; i < 50; ++i) r.tracker.mark_dirty(ptr("a", static_cast<std::uint32_t>(i)));
    r.advance(3 * kSec);
    CHECK(r.store.writes() == 1);
    bi::Pointer got;
    CHECK(bi::load(r.dir.path, got) == bi::LoadStatus::kOk && got == ptr("a", 49));
  }
  {  // 30 s rule: pedal held down (no note) for > 30 s after the change
    Rig r;
    r.advance(5 * kSec);
    r.pedal = true;
    r.tracker.mark_dirty(ptr("a", 1));
    r.advance(29 * kSec);
    CHECK(r.store.writes() == 0);
    r.advance(1100'000'000ull);
    CHECK(r.store.writes() == 1);
  }
  {  // Note held: no write for any duration, then eligible (2 s idle or 30 s) once released
    Rig r;
    r.advance(5 * kSec);
    r.notes = 2;
    r.tracker.mark_dirty(ptr("a", 1));
    r.advance(120 * kSec);
    CHECK(r.store.writes() == 0 && r.tracker.dirty());
    r.notes = 0;  // 30 s long gone: eligible at once
    r.advance(40'000'000ull);
    CHECK(r.store.writes() == 1);
    // a short change then a short hold: waits for 2 s of quiet after release
    r.notes = 1;
    r.tracker.mark_dirty(ptr("a", 2));
    r.advance(3 * kSec);
    r.notes = 0;
    r.advance(1500'000'000ull);
    CHECK(r.store.writes() == 1);
    r.advance(600'000'000ull);
    CHECK(r.store.writes() == 2);
  }
  {  // Import verify: no write until verify ends
    Rig r;
    r.advance(5 * kSec);
    r.verifying = true;
    r.tracker.mark_dirty(ptr("a", 1));
    r.advance(100 * kSec);
    CHECK(r.store.writes() == 0 && r.tracker.dirty());
    r.verifying = false;
    r.advance(40'000'000ull);
    CHECK(r.store.writes() == 1);
  }
  {  // Identical to the stored pointer: dirty cleared, no write
    Rig r;
    CHECK(r.store.write(ptr("a", 1)) == ls::Status::kOk);
    const std::uint64_t w = r.store.writes();
    r.tracker.mark_dirty(ptr("a", 1));
    r.advance(5 * kSec);
    CHECK(r.store.writes() == w && !r.tracker.dirty());
  }
  {  // observe(): polls the position, dirty only on change; an invalid position (no Live Set) is ignored
    Rig r;
    r.tracker.observe(ptr("", 0));
    CHECK(!r.tracker.dirty());
    r.tracker.observe(ptr("a", 1));
    CHECK(r.tracker.dirty());
    r.advance(3 * kSec);
    CHECK(r.store.writes() == 1);
    r.tracker.observe(ptr("a", 1));
    CHECK(!r.tracker.dirty());
    r.tracker.observe(ptr("a", 2));
    CHECK(r.tracker.dirty());
  }
  {  // Write failure keeps the pointer dirty and retries on the next tick
    TmpDir d;
    int fail_left = 3;
    bi::StoreOptions o{d.path};
    o.on_stage = [&](ls::Stage st) {
      if (st == ls::Stage::kFsynced && fail_left > 0) {
        --fail_left;
        return ls::HookAction::kAbort;
      }
      return ls::HookAction::kContinue;
    };
    std::uint64_t now = 100 * kSec;
    bi::BootIntoStore s(o);
    bi::BootIntoTracker t(s, bi::TrackerProbes{[&] { return now; }, nullptr, nullptr, nullptr});
    t.mark_dirty(ptr("a", 1));
    now += 3 * kSec;
    CHECK(!t.tick() && t.dirty() && s.writes() == 1);  // failure 1
    for (int i = 0; i < 40; ++i) {                     // 800 ms: backing off, no attempt
      now += 20'000'000ull;
      CHECK(!t.tick());
    }
    CHECK(s.writes() == 1);
    now += 300'000'000ull;
    CHECK(!t.tick() && s.writes() == 2);  // failure 2 after the pause
    now += 1 * kSec + 20'000'000ull;
    CHECK(!t.tick() && s.writes() == 3);  // failure 3
    now += 1 * kSec + 20'000'000ull;
    CHECK(t.tick() && !t.dirty() && s.writes() == 4);
    bi::Pointer got;
    CHECK(bi::load(d.path, got) == bi::LoadStatus::kOk && got == ptr("a", 1));
  }
}

// A failed write that may have committed must not make the next identical write look redundant.
void test_stale_stored() {
  TmpDir d;
  bi::StoreOptions o{d.path};
  o.inject_dir_sync_failure = true;
  bi::BootIntoStore s(o);
  CHECK(s.write(ptr("a", 1)) == ls::Status::kCommittedNotDurable);
  bi::StoreOptions o2{d.path};
  bi::BootIntoStore s2(o2);
  CHECK(s2.write(ptr("a", 1)) == ls::Status::kOk);
  // same store: the stored copy is invalidated by the failure, so the identical pointer is written again
  bi::StoreOptions o3{d.path};
  int n = 0;
  o3.on_stage = [&](ls::Stage st) { return (st == ls::Stage::kRenamed && n++ == 0) ? ls::HookAction::kAbort : ls::HookAction::kContinue; };
  bi::BootIntoStore s3(o3);
  CHECK(s3.write(ptr("b", 2)) == ls::Status::kFaultInjected);  // renamed, then "killed"
  bool wrote = false;
  CHECK(s3.write(ptr("b", 2), &wrote) == ls::Status::kOk && wrote);
}

// The 30 s clock runs from the first dirty time: a stream of changes under a held pedal cannot starve the write.
void test_no_starvation() {
  Rig r;
  r.advance(5 * kSec);
  r.pedal = true;
  std::uint32_t i = 0;
  for (int sec = 0; sec < 29; ++sec) {
    r.tracker.mark_dirty(ptr("a", i++ % 8));
    r.advance(kSec);
  }
  CHECK(r.store.writes() == 0);
  r.tracker.mark_dirty(ptr("a", 100));
  r.advance(1500'000'000ull);
  CHECK(r.store.writes() == 1);
}

// A clock that steps back counts as no time elapsed (no unsigned wrap that would write at once).
void test_non_monotonic_clock() {
  Rig r;
  r.advance(5 * kSec);
  r.pedal = true;
  r.tracker.mark_dirty(ptr("a", 1));
  r.now -= 50 * kSec;
  r.tracker.tick();
  r.pedal = false;
  r.tracker.tick();
  CHECK(r.store.writes() == 0 && r.tracker.dirty());
  r.advance(3 * kSec);
  CHECK(r.store.writes() == 1);
}

// The control-tick body with the plane's accessors (a LastValueTable as RT fills it), run on another thread than the one that made the store.
void test_update_wiring() {
  TmpDir d;
  velora::core::LastValueTable live;
  std::uint64_t now = 100 * kSec;
  bi::BootIntoStore store(bi::StoreOptions{d.path});  // made on this thread
  bi::BootIntoTracker t(store, bi::TrackerProbes{[&] { return now; }, [&] { return live.load_held_notes(); }, [&] { return live.load_pedal_down(); }, nullptr});
  bi::BootIntoTracker::Position pos;
  pos.live_set_id = "ls-1";
  pos.scene = 2;
  pos.setlist_id = "sl-1";
  bool wrote_after_hold = true, wrote_final = false, loading_ok = true;
  std::thread control([&] {
    live.store_input_activity(1, false);  // a key is held
    CHECK(!t.update(pos) && t.dirty());
    for (int i = 0; i < 500; ++i) {  // 10 s, key still held: no write
      now += 20'000'000ull;
      if (t.update(pos)) wrote_after_hold = false;
    }
    live.store_input_activity(0, false);
    pos.scene = 3;
    for (int i = 0; i < 20; ++i) {  // a load in progress: the position is not taken
      pos.loading = true;
      pos.live_set_id = "ls-2";
      now += 20'000'000ull;
      t.update(pos);
    }
    if (t.pending().live_set_id != "ls-1") loading_ok = false;
    pos.loading = false;
    pos.live_set_id = "ls-1";
    for (int i = 0; i < 200; ++i) {
      now += 20'000'000ull;
      if (t.update(pos)) wrote_final = true;
    }
  });
  control.join();
  CHECK(wrote_after_hold && loading_ok && wrote_final);
  bi::Pointer got;
  CHECK(bi::load(d.path, got) == bi::LoadStatus::kOk && got == ptr("ls-1", 3, "sl-1"));
  // an unsettled position (no Live Set, no scene) is ignored
  bi::BootIntoTracker::Position none;
  bi::BootIntoStore s2(bi::StoreOptions{d.path});
  bi::BootIntoTracker t2(s2, bi::TrackerProbes{[&] { return now; }, nullptr, nullptr, nullptr});
  t2.update(none);
  CHECK(!t2.dirty());
}

// A long run: many changes with notes coming and going; no file write ever happens while a note is held or while an import verifies.
void test_long_run() {
  TmpDir d;
  std::uint64_t now = 100 * kSec;
  std::uint32_t notes = 0;
  bool pedal = false, verifying = false;
  std::uint64_t violations = 0;
  bool hold_at_write = false, verify_at_write = false;
  bi::StoreOptions o{d.path};
  o.on_stage = [&](ls::Stage st) {
    if (st == ls::Stage::kTempWritten) {
      if (notes > 0) hold_at_write = true;
      if (verifying) verify_at_write = true;
    }
    return ls::HookAction::kContinue;
  };
  bi::BootIntoStore s(o);
  bi::BootIntoTracker t(s, bi::TrackerProbes{[&] { return now; }, [&] { return notes; }, [&] { return pedal; }, [&] { return verifying; }});
  std::uint32_t rng = 12345;
  const auto next = [&] {
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
  };
  for (int step = 0; step < 200000; ++step) {  // 20 ms each: about 66 minutes
    now += 20'000'000ull;
    const std::uint32_t r = next() % 1000;
    if (r < 20) notes = (notes > 0) ? 0 : 1 + next() % 4;
    if (r >= 20 && r < 30) pedal = !pedal;
    if (r >= 30 && r < 33) verifying = !verifying;
    if (r >= 33 && r < 45) t.observe(ptr("live-" + std::to_string(next() % 6), next() % 8));
    t.tick();
  }
  if (hold_at_write || verify_at_write) ++violations;
  CHECK(violations == 0);
  CHECK(s.writes() > 0);
  // far fewer writes than changes: never one per change
  CHECK(s.writes() < 100000);
}

}  // namespace

int main() {
  test_format();
  test_load_fallback();
  test_identical_skip();
  test_write_errors();
  test_kill_injection();
  test_real_kill();
  test_policy();
  test_stale_stored();
  test_no_starvation();
  test_non_monotonic_clock();
  test_update_wiring();
  test_long_run();
  if (g_fail) {
    std::printf("boot_into_tests: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("boot_into_tests: all passed\n");
  return 0;
}
