// Boot target resolution tests (Story 8.9): every row of the I/O matrix over plain data (no audio, no files), the bounded sysd wait, and the tracker baseline
// (the boot position never overwrites the stored pointer).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <map>
#include <string>
#include <vector>

#include "velora/host/boot_into_store.hpp"
#include "velora/host/boot_target.hpp"

namespace bt = velora::host::boottarget;
namespace bi = velora::host::bootinto;
using velora::host::CatalogEntry;

static int g_fail = 0;
#define CHECK(c)                                               \
  do {                                                         \
    if (!(c)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

namespace {
const std::string A = "a0000000-0000-4000-8000-000000000001", B = "b0000000-0000-4000-8000-000000000002", C = "c0000000-0000-4000-8000-000000000003",
                  D = "d0000000-0000-4000-8000-000000000004", GONE = "e0000000-0000-4000-8000-0000000000ff";

struct Rig {
  bt::Inputs in;
  std::map<std::string, std::vector<std::string>> lists;
  Rig() {
    // Names order: Alpha(A) < Bravo(B) < Charlie(C) < Delta(D); ids are not in name order on purpose below.
    in.live_sets = {{C, "Charlie", "/c"}, {A, "Alpha", "/a"}, {D, "Delta", "/d"}, {B, "Bravo", "/b"}};
    lists["sl-1"] = {C, D};
    lists["sl-other"] = {C};
    lists["sl-empty"] = {};
    lists["sl-gone-first"] = {GONE, D};
    in.setlist_members = [this](const std::string& id) -> const std::vector<std::string>* {
      const auto it = lists.find(id);
      return it == lists.end() ? nullptr : &it->second;
    };
  }
  static bi::Pointer ptr(const std::string& ls, std::uint32_t scene, const std::string& sl) {
    bi::Pointer p;
    p.live_set_id = ls;
    p.scene_index = scene;
    p.setlist_id = sl;
    return p;
  }
};

void test_last_sound() {
  Rig r;
  r.in.mode = ws::v1::BOOT_INTO_MODE_LAST_SOUND;
  r.in.pointer = Rig::ptr(D, 2, "sl-1");
  bt::Target t = bt::resolve(r.in);
  CHECK(t.source == bt::Source::kPointer && t.live_set_id == D && t.scene == 2 && t.setlist_id == "sl-1" && t.notes.empty());
  r.in.pointer = Rig::ptr(B, 0, "");  // no setlist
  t = bt::resolve(r.in);
  CHECK(t.source == bt::Source::kPointer && t.live_set_id == B && t.setlist_id.empty());
  r.in.pointer = Rig::ptr(B, 1, "sl-deleted");  // the setlist is gone: the Live Set and scene stay, NO SETLIST, noted
  t = bt::resolve(r.in);
  CHECK(t.live_set_id == B && t.scene == 1 && t.setlist_id.empty() && t.notes.size() == 1);
  // the pointer's setlist exists but no longer holds the Live Set (off-script): the Live Set still boots, with that setlist loaded (the controller keeps position -1)
  r.in.pointer = Rig::ptr(B, 1, "sl-other");
  t = bt::resolve(r.in);
  CHECK(t.source == bt::Source::kPointer && t.live_set_id == B && t.scene == 1 && t.setlist_id == "sl-other" && t.notes.empty());
  // no pointer: the first Live Set of All Live Sets (by name, not by id order)
  r.in.pointer.reset();
  t = bt::resolve(r.in);
  CHECK(t.source == bt::Source::kFirstOfAll && t.live_set_id == A && t.scene == 0 && t.setlist_id.empty() && !t.notes.empty());
  // the pointer's Live Set was deleted
  r.in.pointer = Rig::ptr(GONE, 3, "sl-1");
  t = bt::resolve(r.in);
  CHECK(t.source == bt::Source::kFirstOfAll && t.live_set_id == A && t.scene == 0 && t.setlist_id.empty());
}

void test_start_of_setlist() {
  Rig r;
  r.in.mode = ws::v1::BOOT_INTO_MODE_START_OF_SETLIST;
  r.in.pointer = Rig::ptr(D, 5, "sl-1");
  bt::Target t = bt::resolve(r.in);
  CHECK(t.source == bt::Source::kStartOfSetlist && t.live_set_id == C && t.scene == 0 && t.setlist_id == "sl-1" && t.notes.empty());
  r.in.pointer = Rig::ptr(D, 5, "sl-empty");  // an empty setlist
  t = bt::resolve(r.in);
  CHECK(t.source == bt::Source::kFirstOfAll && t.live_set_id == A && t.setlist_id.empty());
  r.in.pointer = Rig::ptr(D, 5, "sl-deleted");
  t = bt::resolve(r.in);
  CHECK(t.source == bt::Source::kFirstOfAll && t.live_set_id == A && t.setlist_id.empty());
  r.in.pointer = Rig::ptr(D, 5, "");  // last time no setlist was loaded
  t = bt::resolve(r.in);
  CHECK(t.source == bt::Source::kFirstOfAll && t.setlist_id.empty());
  r.in.pointer.reset();
  t = bt::resolve(r.in);
  CHECK(t.source == bt::Source::kFirstOfAll && t.live_set_id == A);
  r.in.pointer = Rig::ptr(A, 0, "sl-gone-first");  // the first entry was deleted: the first one that exists, setlist loaded
  t = bt::resolve(r.in);
  CHECK(t.source == bt::Source::kStartOfSetlist && t.live_set_id == D && t.setlist_id == "sl-gone-first" && !t.notes.empty());
}

void test_default_live_set() {
  Rig r;
  r.in.mode = ws::v1::BOOT_INTO_MODE_DEFAULT_LIVE_SET;
  r.in.default_live_set_id = C;
  r.in.pointer = Rig::ptr(D, 4, "sl-1");  // the pointer is not used: scene 1, NO SETLIST
  bt::Target t = bt::resolve(r.in);
  CHECK(t.source == bt::Source::kDefaultLiveSet && t.live_set_id == C && t.scene == 0 && t.setlist_id.empty() && t.notes.empty());
  r.in.default_live_set_id = GONE;  // deleted
  t = bt::resolve(r.in);
  CHECK(t.source == bt::Source::kFirstOfAll && t.live_set_id == A && t.scene == 0 && t.setlist_id.empty());
  // an id that is not a Live Set id (legacy): loads as LAST_SOUND
  r.in.default_live_set_id = "live-1";
  t = bt::resolve(r.in);
  CHECK(t.source == bt::Source::kPointer && t.live_set_id == D && t.scene == 4 && !t.notes.empty());
  r.in.default_live_set_id.clear();
  r.in.pointer.reset();
  t = bt::resolve(r.in);
  CHECK(t.source == bt::Source::kFirstOfAll);
}

void test_empty_library_and_unknown_mode() {
  Rig r;
  r.in.live_sets.clear();
  for (auto m : {ws::v1::BOOT_INTO_MODE_LAST_SOUND, ws::v1::BOOT_INTO_MODE_START_OF_SETLIST, ws::v1::BOOT_INTO_MODE_DEFAULT_LIVE_SET}) {
    r.in.mode = m;
    r.in.default_live_set_id = A;
    r.in.pointer = Rig::ptr(A, 0, "sl-1");
    const bt::Target t = bt::resolve(r.in);
    CHECK(t.source == bt::Source::kEmptyLibrary && t.live_set_id.empty() && t.setlist_id.empty());
  }
  Rig u;
  u.in.mode = static_cast<ws::v1::BootIntoMode>(99);
  u.in.pointer = Rig::ptr(B, 1, "");
  const bt::Target t = bt::resolve(u.in);
  CHECK(t.source == bt::Source::kPointer && t.live_set_id == B && t.scene == 1 && !t.notes.empty());
  u.in.mode = ws::v1::BOOT_INTO_MODE_UNSPECIFIED;
  CHECK(bt::resolve(u.in).live_set_id == B);
}

void test_sysd_absent_is_bounded() {
  ws::v1::Config c;
  c.mutable_boot_into()->set_mode(ws::v1::BOOT_INTO_MODE_DEFAULT_LIVE_SET);
  const auto t0 = std::chrono::steady_clock::now();
  const bool got = bt::fetch_sysd_config("/nonexistent/velora-sysd.sock", 150, c);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  CHECK(!got);
  CHECK(c.boot_into().mode() == ws::v1::BOOT_INTO_MODE_LAST_SOUND);  // the default config
  CHECK(ms < 5000);                                                   // bounded (generous: the wait is 150 ms; no timing claim)
  CHECK(!bt::fetch_sysd_config("", 150, c));
  CHECK(bt::kSysdBootWaitMs > 0 && bt::kSysdBootWaitMs <= 5000);
}

void test_tracker_baseline_does_not_overwrite_pointer() {
  const std::string dir = "/tmp/velora-boot-target-XXXXXX";
  std::string d = dir;
  if (!::mkdtemp(d.data())) return;
  {
    bi::StoreOptions so;
    so.dir = d;
    bi::BootIntoStore store(so);
    bi::Pointer stored = Rig::ptr(D, 3, "sl-1");
    CHECK(store.write(stored) == velora::host::liveset::Status::kOk);
    std::uint64_t now = 1'000'000'000ull;
    bi::TrackerProbes pr;
    pr.now_ns = [&now] { return now; };
    bi::BootIntoTracker tr(store, pr);
    // Booted on START_OF_SETLIST: C scene 0 of sl-1; seeded as the baseline.
    tr.seed(Rig::ptr(C, 0, "sl-1"));
    bi::BootIntoTracker::Position pos;
    pos.live_set_id = C;
    pos.scene = 0;
    pos.setlist_id = "sl-1";
    for (int i = 0; i < 40; ++i) {  // 40 s of an idle unit sitting on the boot position
      now += 1'000'000'000ull;
      CHECK(!tr.update(pos));
    }
    CHECK(!tr.dirty() && store.writes() == 1 && store.stored() == stored);
    // A real user change is written (the unit has been idle for long, so at once).
    pos.scene = 1;
    CHECK(tr.update(pos));
    CHECK(store.stored() == Rig::ptr(C, 1, "sl-1"));
    bi::Pointer back;
    CHECK(bi::load(d, back) == bi::LoadStatus::kOk && back == Rig::ptr(C, 1, "sl-1"));
  }
  std::string cmd = "rm -rf " + d;
  [[maybe_unused]] const int rc = std::system(cmd.c_str());
}
}  // namespace

int main() {
  test_last_sound();
  test_start_of_setlist();
  test_default_live_set();
  test_empty_library_and_unknown_mode();
  test_sysd_absent_is_bounded();
  test_tracker_baseline_does_not_overwrite_pointer();
  std::printf(g_fail ? "boot_target_tests: FAILED\n" : "boot_target_tests: ok\n");
  return g_fail ? 1 : 0;
}
