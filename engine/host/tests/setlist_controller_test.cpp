// Setlist controller tests (Story 4.6): every ws.v1 setlist command with its typed errors, the loaded setlist, LIVE SET stepping scenarios
// (setlist loaded, off-script jump and return, All Live Sets in name order, end / start of the list, empty setlist, a setlist with a missing or
// invalid Live Set), cascades, the published State, and ws.v1 over a socket with concurrent clients. The switcher is a fake SwitchPort here (the
// real one is exercised by engine_host_setlist_smoke on macOS and by the Story 4.5 tests); nothing asserts a duration.
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "setlist_fixtures.hpp"
#include "velora/core/block_renderer.hpp"
#include "velora/host/control_plane.hpp"
#include "velora/host/control_server.hpp"
#include "velora/host/dev_config.hpp"
#include "velora/host/render_driver.hpp"
#include "velora/host/setlist_controller.hpp"

namespace fs = std::filesystem;
using namespace velora;
using namespace velora::host;
namespace sl = velora::host::setlist;
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

using EC = ws::v1::ErrorCode;

std::string make_tmp_dir(const char* tag) {
  std::string t = (fs::temp_directory_path() / (std::string("velora-") + tag + "-XXXXXX")).string();
  if (!::mkdtemp(t.data())) std::abort();
  return t;
}

// A switcher stand-in. A jump to a listed Live Set is accepted and (unless `slow`) is what sounds right away; `slow` keeps the old one sounding
// until finish(), as the real background load does (the target is `effective`).
class FakePort final : public SwitchPort {
 public:
  SwitchOutcome jump_to(const std::string& id) override {
    SwitchOutcome o;
    jumps.push_back(id);
    const CatalogEntry* t = nullptr;
    for (const CatalogEntry& e : cat)
      if (e.id == id) t = &e;
    if (!t) {
      o.result = SwitchOutcome::Result::kUnknownLiveSet;
      o.target_id = id;
      return o;
    }
    o.target_id = t->id;
    o.target_name = t->name;
    if (refuse) {  // accepts but does not switch (a switcher that did not take the request)
      o.result = SwitchOutcome::Result::kAccepted;
      return o;
    }
    if (force) {
      o.result = *force;
      sounding = effective = id;
      return o;
    }
    if (id == sounding && !loading) {
      o.result = SwitchOutcome::Result::kAlreadyLoaded;
      return o;
    }
    o.superseded_previous = loading;
    o.result = SwitchOutcome::Result::kAccepted;
    if (slow) {
      loading = true;
      effective = id;
    } else {
      sounding = effective = id;
    }
    return o;
  }
  void finish() {
    sounding = effective;
    loading = false;
  }
  const std::string& effective_id() const override { return effective; }
  const std::string& sounding_id() const override { return sounding; }
  const std::vector<CatalogEntry>& catalog() const override { return cat; }
  void add_to_catalog(CatalogEntry e) override {
    for (CatalogEntry& c : cat)
      if (c.id == e.id) {
        c = std::move(e);
        return;
      }
    cat.push_back(std::move(e));
  }
  void remove_from_catalog(const std::string& id) override {
    for (std::size_t i = 0; i < cat.size(); ++i)
      if (cat[i].id == id) {
        cat.erase(cat.begin() + static_cast<std::ptrdiff_t>(i));
        return;
      }
  }
  // Test controls.
  void play(const std::string& id) { sounding = effective = id; loading = false; }

  std::vector<CatalogEntry> cat;
  std::string sounding, effective;
  std::vector<std::string> jumps;
  bool slow = false, loading = false, refuse = false;
  std::optional<SwitchOutcome::Result> force;  // answer this result and apply the request
};

std::atomic<std::uint64_t> g_rid{1000};
ws::v1::ClientMessage msg() {
  ws::v1::ClientMessage m;
  m.set_request_id(++g_rid);
  return m;
}

struct World {
  std::string data = make_tmp_dir("setlist-ctl");
  std::string live_dir, set_dir;
  std::unique_ptr<core::BlockRenderer> br = std::make_unique<core::BlockRenderer>();
  std::unique_ptr<HostEventQueue> midi_q = std::make_unique<HostEventQueue>();
  std::unique_ptr<HostEventQueue> ctl_q = std::make_unique<HostEventQueue>();
  std::atomic<std::uint64_t> clock_ns{1'000'000'000ull};
  ControlPlane plane{br->snapshots(), br->last_values(), *ctl_q, [this] { return clock_ns.load(); }};
  ls::SeededRandom rng{7};
  std::unique_ptr<ls::LiveSetStore> live;
  std::unique_ptr<sl::SetlistStore> store;
  FakePort port;
  std::unique_ptr<SetlistController> ctl;

  explicit World(std::function<void(ls::StoreOptions&, sl::StoreOptions&)> tweak = {}) {
    live_dir = data + "/live_sets";
    set_dir = data + "/setlists";
    fs::create_directories(live_dir);
    fs::create_directories(set_dir);
    ws::v1::State st;
    std::string err;
    CHECK(parse_dev_config("{}", st, err) && plane.load_initial(st, err));
    ls::StoreOptions lo;
    lo.dir = live_dir;
    sl::StoreOptions so;
    so.dir = set_dir;
    so.rng = &rng;
    if (tweak) tweak(lo, so);
    CHECK(ls::LiveSetStore::open(lo, live) == ls::Status::kOk);
    CHECK(sl::SetlistStore::open(so, store) == sl::Status::kOk);
    ctl = std::make_unique<SetlistController>(plane, *store, *live, port);
  }
  ~World() {
    ctl.reset();
    store.reset();
    live.reset();
    std::error_code ec;
    fs::remove_all(data, ec);
  }

  ws::v1::ServerMessage send(const ws::v1::ClientMessage& m, bool* changed = nullptr) {
    bool c = false;
    ws::v1::ServerMessage r = plane.handle(m, c);
    if (changed) *changed = c;
    CHECK(r.request_id() == m.request_id());
    return r;
  }
  static bool is_err(const ws::v1::ServerMessage& r, EC c) { return r.has_error() && r.error().code() == c; }

  // A Live Set through the command (so the catalog and the file are real).
  std::string live_set(const std::string& name) {
    auto m = msg();
    m.mutable_create_live_set()->set_name(name);
    const auto r = send(m);
    CHECK(r.has_created_reply());
    return r.created_reply().id();
  }
  std::string setlist(const std::string& name, const std::vector<std::string>& ids = {}) {
    auto m = msg();
    m.mutable_create_setlist()->set_name(name);
    const auto r = send(m);
    CHECK(r.has_created_reply());
    const std::string id = r.created_reply().id();
    for (const std::string& l : ids) {
      auto a = msg();
      a.mutable_add_live_set_to_setlist()->set_setlist_id(id);
      a.mutable_add_live_set_to_setlist()->set_live_set_id(l);
      CHECK(send(a).has_ack());
    }
    return id;
  }
  ws::v1::ServerMessage load(const std::string& id) {
    auto m = msg();
    m.mutable_load_setlist()->set_id(id);
    return send(m);
  }
  ws::v1::ServerMessage step(bool next) {
    auto m = msg();
    if (next) m.mutable_next_live_set();
    else m.mutable_previous_live_set();
    return send(m);
  }
  ws::v1::SetlistStatus status() {
    auto m = msg();
    m.mutable_get_state();
    const auto r = send(m);
    CHECK(r.has_state() && r.state().has_setlists());
    return r.state().setlists();
  }
  std::vector<std::string> ids_in(const std::string& setlist_id) {
    auto m = msg();
    m.mutable_get_setlists()->set_setlist_id(setlist_id);
    const auto r = send(m);
    std::vector<std::string> v;
    for (const auto& e : r.setlists_reply().entries()) v.push_back(e.live_set_id());
    return v;
  }
};

// ---- commands --------------------------------------------------------------------------------------------------------------------------
void test_commands_and_errors() {
  World w;
  const std::uint64_t rev0 = w.plane.revision();
  bool changed = false;

  auto m = msg();
  m.mutable_create_setlist()->set_name("Friday");
  auto r = w.send(m, &changed);
  CHECK(r.has_created_reply() && ls::is_uuid_v4(r.created_reply().id()) && changed && r.revision() == rev0 + 1);
  const std::string s1 = r.created_reply().id();
  const std::string s2 = w.setlist("Saturday");
  const std::string s3 = w.setlist("Sunday");

  // Errors never move the revision and never push.
  const std::uint64_t rev = w.plane.revision();
  auto bad = [&](ws::v1::ClientMessage x, EC code) {
    bool ch = true;
    const auto e = w.send(x, &ch);
    const bool good = World::is_err(e, code) && !ch && w.plane.revision() == rev && e.revision() == rev;
    if (!good) std::printf("expected error %d, got %s %d\n", static_cast<int>(code), e.has_error() ? "error" : "reply", e.has_error() ? static_cast<int>(e.error().code()) : -1);
    CHECK(good);
  };
  m = msg(); m.mutable_create_setlist()->set_name(""); bad(m, ws::v1::ERROR_CODE_INVALID_NAME);
  m = msg(); m.mutable_create_setlist()->set_name(std::string(65, 'x')); bad(m, ws::v1::ERROR_CODE_INVALID_NAME);
  m = msg(); m.mutable_create_setlist()->set_name("bad\x01name"); bad(m, ws::v1::ERROR_CODE_INVALID_NAME);
  m = msg(); m.mutable_rename_setlist()->set_id("nope"); m.mutable_rename_setlist()->set_name("x"); bad(m, ws::v1::ERROR_CODE_NO_SUCH_SETLIST);
  m = msg(); m.mutable_rename_setlist()->set_id(s1); m.mutable_rename_setlist()->set_name(""); bad(m, ws::v1::ERROR_CODE_INVALID_NAME);
  m = msg(); m.mutable_delete_setlist()->set_id("nope"); bad(m, ws::v1::ERROR_CODE_NO_SUCH_SETLIST);
  m = msg(); m.mutable_reorder_setlists()->add_ids(s1); bad(m, ws::v1::ERROR_CODE_INVALID_ORDER);
  m = msg(); { auto* x = m.mutable_reorder_setlists(); x->add_ids(s1); x->add_ids(s1); x->add_ids(s2); } bad(m, ws::v1::ERROR_CODE_INVALID_ORDER);
  m = msg(); m.mutable_create_tag()->set_name(""); bad(m, ws::v1::ERROR_CODE_INVALID_NAME);
  m = msg(); m.mutable_rename_tag()->set_id("nope"); m.mutable_rename_tag()->set_name("x"); bad(m, ws::v1::ERROR_CODE_NO_SUCH_TAG);
  m = msg(); m.mutable_delete_tag()->set_id("nope"); bad(m, ws::v1::ERROR_CODE_NO_SUCH_TAG);
  m = msg(); m.mutable_set_setlist_tags()->set_setlist_id("nope"); bad(m, ws::v1::ERROR_CODE_NO_SUCH_SETLIST);
  m = msg(); m.mutable_set_setlist_tags()->set_setlist_id(s1); m.mutable_set_setlist_tags()->add_tag_ids(sfixtures::uuid_n(1)); bad(m, ws::v1::ERROR_CODE_NO_SUCH_TAG);
  m = msg(); m.mutable_load_setlist()->set_id("nope"); bad(m, ws::v1::ERROR_CODE_NO_SUCH_SETLIST);
  m = msg(); m.mutable_get_setlists()->set_setlist_id("nope"); bad(m, ws::v1::ERROR_CODE_NO_SUCH_SETLIST);
  m = msg(); m.mutable_delete_live_set()->set_id("nope"); bad(m, ws::v1::ERROR_CODE_NO_SUCH_LIVE_SET);
  m = msg(); m.mutable_delete_live_set()->set_id(sfixtures::uuid_n(1)); bad(m, ws::v1::ERROR_CODE_NO_SUCH_LIVE_SET);
  m = msg(); m.mutable_create_live_set()->set_name(""); bad(m, ws::v1::ERROR_CODE_INVALID_NAME);

  // Rename, reorder, delete.
  m = msg(); m.mutable_rename_setlist()->set_id(s1); m.mutable_rename_setlist()->set_name("Friday night");
  r = w.send(m, &changed);
  CHECK(r.has_ack() && changed && r.revision() == rev + 1);
  m = msg(); { auto* x = m.mutable_reorder_setlists(); x->add_ids(s3); x->add_ids(s1); x->add_ids(s2); }
  CHECK(w.send(m).has_ack());
  m = msg(); m.mutable_get_setlists();
  r = w.send(m);
  CHECK(r.has_setlists_reply() && r.setlists_reply().setlists_size() == 3);
  CHECK(r.setlists_reply().setlists(0).id() == s3 && r.setlists_reply().setlists(1).name() == "Friday night" && r.setlists_reply().setlists(2).id() == s2);
  CHECK(r.setlists_reply().detail_setlist_id().empty() && r.setlists_reply().entries_size() == 0);

  // Live Sets in a setlist.
  const std::string a = w.live_set("Alpha"), b = w.live_set("Bravo"), c = w.live_set("Charlie");
  auto add = [&](const std::string& sid, const std::string& lid, int index = -1) {
    auto x = msg();
    x.mutable_add_live_set_to_setlist()->set_setlist_id(sid);
    x.mutable_add_live_set_to_setlist()->set_live_set_id(lid);
    if (index >= 0) x.mutable_add_live_set_to_setlist()->set_index(static_cast<std::uint32_t>(index));
    return w.send(x);
  };
  CHECK(add(s1, a).has_ack() && add(s1, b).has_ack() && add(s1, c, 0).has_ack());
  CHECK((w.ids_in(s1) == std::vector<std::string>{c, a, b}));
  CHECK(World::is_err(add(s1, a), ws::v1::ERROR_CODE_DUPLICATE_LIVE_SET_IN_SETLIST));
  CHECK(World::is_err(add(s1, sfixtures::uuid_n(77)), ws::v1::ERROR_CODE_NO_SUCH_LIVE_SET));  // not a Live Set the engine knows
  CHECK(World::is_err(add(s1, "not-an-id"), ws::v1::ERROR_CODE_NO_SUCH_LIVE_SET));
  CHECK(World::is_err(add("nope", a), ws::v1::ERROR_CODE_NO_SUCH_SETLIST));
  CHECK(World::is_err(add(s2, a, 5), ws::v1::ERROR_CODE_INVALID_ORDER));
  CHECK(add(s2, a).has_ack());  // the same Live Set in a second setlist
  m = msg(); m.mutable_remove_live_set_from_setlist()->set_setlist_id(s1); m.mutable_remove_live_set_from_setlist()->set_live_set_id(sfixtures::uuid_n(5));
  CHECK(World::is_err(w.send(m), ws::v1::ERROR_CODE_LIVE_SET_NOT_IN_SETLIST));
  m = msg(); { auto* x = m.mutable_reorder_live_sets_in_setlist(); x->set_setlist_id(s1); x->add_live_set_ids(a); x->add_live_set_ids(b); }
  CHECK(World::is_err(w.send(m), ws::v1::ERROR_CODE_INVALID_ORDER));
  m = msg(); { auto* x = m.mutable_reorder_live_sets_in_setlist(); x->set_setlist_id(s1); x->add_live_set_ids(b); x->add_live_set_ids(c); x->add_live_set_ids(a); }
  CHECK(w.send(m).has_ack() && (w.ids_in(s1) == std::vector<std::string>{b, c, a}));
  m = msg(); m.mutable_remove_live_set_from_setlist()->set_setlist_id(s1); m.mutable_remove_live_set_from_setlist()->set_live_set_id(c);
  CHECK(w.send(m).has_ack() && (w.ids_in(s1) == std::vector<std::string>{b, a}));

  // Tags.
  m = msg(); m.mutable_create_tag()->set_name("Weddings");
  r = w.send(m);
  CHECK(r.has_created_reply());
  const std::string t1 = r.created_reply().id();
  m = msg(); m.mutable_create_tag()->set_name("Jazz");
  const std::string t2 = w.send(m).created_reply().id();
  m = msg(); m.mutable_rename_tag()->set_id(t2); m.mutable_rename_tag()->set_name("Jazz club");
  CHECK(w.send(m).has_ack());
  m = msg(); m.mutable_set_setlist_tags()->set_setlist_id(s1); m.mutable_set_setlist_tags()->add_tag_ids(t1); m.mutable_set_setlist_tags()->add_tag_ids(t2);
  CHECK(w.send(m).has_ack());
  m = msg(); m.mutable_set_setlist_tags()->set_setlist_id(s2); m.mutable_set_setlist_tags()->add_tag_ids(t2);
  CHECK(w.send(m).has_ack());
  m = msg(); m.mutable_get_setlists();
  r = w.send(m);
  CHECK(r.setlists_reply().tags_size() == 2 && r.setlists_reply().tags(1).name() == "Jazz club");
  m = msg(); m.mutable_delete_tag()->set_id(t2);  // cascade
  CHECK(w.send(m).has_ack());
  m = msg(); m.mutable_get_setlists();
  r = w.send(m);
  CHECK(r.setlists_reply().tags_size() == 1);
  for (const auto& s : r.setlists_reply().setlists()) {
    if (s.id() == s1) CHECK(s.tag_ids_size() == 1 && s.tag_ids(0) == t1);
    if (s.id() == s2) CHECK(s.tag_ids_size() == 0);
  }

  // Deleting a setlist never deletes a Live Set.
  m = msg(); m.mutable_delete_setlist()->set_id(s1);
  CHECK(w.send(m).has_ack());
  CHECK(w.port.cat.size() == 3 && fs::exists(w.live->path_for(a)) && fs::exists(w.live->path_for(b)) && fs::exists(w.live->path_for(c)));

  // Everything persisted: a new store over the same directory sees it.
  w.ctl.reset();
  w.store.reset();
  sl::StoreOptions so;
  so.dir = w.set_dir;
  CHECK(sl::SetlistStore::open(so, w.store) == sl::Status::kOk);
  CHECK(w.store->library().setlists_size() == 2 && w.store->library().tags_size() == 1);
  CHECK(w.store->find_setlist(s2)->live_set_ids_size() == 1);
}

void test_plane_without_setlists() {
  auto br = std::make_unique<core::BlockRenderer>();
  auto q = std::make_unique<HostEventQueue>();
  ControlPlane plane(br->snapshots(), br->last_values(), *q, [] { return std::uint64_t{1}; });
  ws::v1::State st;
  std::string err;
  CHECK(parse_dev_config("{}", st, err) && plane.load_initial(st, err));
  bool changed = false;
  auto m = msg();
  m.mutable_create_setlist()->set_name("x");
  auto r = plane.handle(m, changed);
  CHECK(World::is_err(r, ws::v1::ERROR_CODE_INVALID_COMMAND) && !changed);
  m = msg();
  m.mutable_next_live_set();
  CHECK(World::is_err(plane.handle(m, changed), ws::v1::ERROR_CODE_INVALID_COMMAND));
  m = msg();
  m.mutable_get_state();
  r = plane.handle(m, changed);
  CHECK(r.has_state() && !r.state().has_setlists());  // no data directory: no setlist status
}

// ---- the new Live Set -------------------------------------------------------------------------------------------------------------------
void test_new_live_set_content_and_default() {
  World w;
  const std::string id = w.live_set("Fresh");
  ls::LoadResult lr;
  CHECK(w.live->read(id, lr) == ls::Status::kOk && lr.live_set.parts_size() == 8 && lr.live_set.scenes_size() == 1);
  for (int k = 0; k < 8; ++k) CHECK(lr.live_set.assign(k) == sl::factory_default_assign()[static_cast<std::size_t>(k)]);
  CHECK(w.port.cat.size() == 1 && w.port.cat[0].id == id && w.port.cat[0].name == "Fresh" && w.port.cat[0].path == w.live->path_for(id));
  const std::array<std::uint32_t, 8> custom = {200, 201, 0, 0, 400, 401, 802, 100};
  CHECK(w.store->set_default_assign(custom) == sl::Status::kOk);
  const std::string id2 = w.live_set("Later");
  ls::LoadResult l1, l2;
  CHECK(w.live->read(id, l1) == ls::Status::kOk && w.live->read(id2, l2) == ls::Status::kOk);
  for (int k = 0; k < 8; ++k) {
    CHECK(l1.live_set.assign(k) == sl::factory_default_assign()[static_cast<std::size_t>(k)]);  // the old one kept its copy
    CHECK(l2.live_set.assign(k) == custom[static_cast<std::size_t>(k)]);
  }
  // The new Live Set can be put into a setlist and stepped to.
  const std::string s = w.setlist("S", {id, id2});
  CHECK(w.ids_in(s).size() == 2);
}

// ---- stepping --------------------------------------------------------------------------------------------------------------------------
void test_name_collation() {
  auto e = [](const char* name, const char* id) { return CatalogEntry{id, name, ""}; };
  CHECK(live_set_name_less(e("alpha", "2"), e("Bravo", "1")));        // case-insensitive
  CHECK(!live_set_name_less(e("Bravo", "1"), e("alpha", "2")));
  CHECK(live_set_name_less(e("Alpha", "2"), e("alpha 2", "1")));      // a prefix sorts first
  CHECK(live_set_name_less(e("ABC", "1"), e("abd", "1")));
  CHECK(live_set_name_less(e("abc", "1"), e("ABC", "2")));            // equal names: by id
  CHECK(!live_set_name_less(e("ABC", "2"), e("abc", "1")));
  CHECK(!live_set_name_less(e("abc", "1"), e("ABC", "1")));           // strict: equal is not less
  CHECK(live_set_name_less(e("10", "1"), e("2", "1")));               // bytewise for digits: "10" < "2"
  CHECK(live_set_name_less(e("Zebra", "1"), e("\xC3\xA9"
                                                "clair", "1")));  // non-ASCII bytes sort after ASCII, no Unicode folding
}

void expect_move_at(int line, const ws::v1::ServerMessage& r, const std::string& id, int position, bool in_setlist) {
  const bool good = r.has_step_reply() && r.step_reply().live_set_id() == id && r.step_reply().position() == position && r.step_reply().in_setlist() == in_setlist &&
                    r.step_reply().jump_result() == ws::v1::JUMP_RESULT_ACCEPTED;
  if (!good)
    std::printf("line %d: step: expected move to %s at %d; got %s (id %s pos %d jump %d)\n", line, id.c_str(), position,
                r.has_error() ? r.error().message().c_str() : "a reply", r.step_reply().live_set_id().c_str(), r.step_reply().position(),
                static_cast<int>(r.step_reply().jump_result()));
  CHECK(good);
}

#define expect_move(...) expect_move_at(__LINE__, __VA_ARGS__)

void test_stepping_in_setlist() {
  World w;
  const std::string a = w.live_set("A one"), b = w.live_set("B two"), c = w.live_set("C three"), d = w.live_set("D four"), x = w.live_set("X outside");
  const std::string s = w.setlist("Show", {a, b, c, d});
  w.port.play(x);  // the engine starts on a Live Set that is not in the setlist
  w.port.jumps.clear();

  // Loading a setlist loads no Live Set: the sound is untouched and nothing is jumped to.
  const std::uint64_t rev = w.plane.revision();
  auto r = w.load(s);
  CHECK(r.has_load_setlist_reply() && r.load_setlist_reply().setlist_id() == s && r.load_setlist_reply().position() == -1 &&
        r.load_setlist_reply().live_set_count() == 4 && r.revision() == rev + 1);
  CHECK(w.port.jumps.empty() && w.port.sounding == x);
  auto st = w.status();
  CHECK(st.available() && st.loaded_setlist_id() == s && st.loaded_setlist_name() == "Show" && st.last_setlist_position() == -1 &&
        st.current_live_set_id() == x && !st.end_of_setlist() && !st.start_of_setlist() && st.setlist_count() == 1 && st.loaded_setlist_size() == 4);

  // Previous on a freshly loaded setlist: before the first, nothing happens.
  r = w.step(false);
  CHECK(World::is_err(r, ws::v1::ERROR_CODE_START_OF_SETLIST) && w.port.jumps.empty() && w.status().start_of_setlist());
  // Next is the first Live Set.
  expect_move(w.step(true), a, 0, true);
  CHECK(w.port.jumps.size() == 1 && w.port.jumps[0] == a);
  st = w.status();
  CHECK(st.last_setlist_position() == 0 && !st.start_of_setlist() && st.current_live_set_id() == a);
  expect_move(w.step(true), b, 1, true);
  expect_move(w.step(true), c, 2, true);
  expect_move(w.step(false), b, 1, true);
  CHECK(w.status().last_setlist_position() == 1);

  // Off-script: a jump to a Live Set outside the setlist leaves the position; stepping returns relative to it.
  w.port.jump_to(x);
  st = w.status();
  CHECK(st.last_setlist_position() == 1 && st.current_live_set_id() == x);
  expect_move(w.step(true), c, 2, true);  // from position 1: the next in the setlist, not "next after X"
  w.port.jump_to(x);
  expect_move(w.step(false), b, 1, true);  // previous from position 2 returns to position 1
  w.port.jump_to(x);
  w.port.jump_to(x);
  CHECK(w.status().last_setlist_position() == 1);

  // A jump to a Live Set that IS in the setlist (the player chose it) moves the position to its index.
  w.port.jump_to(d);
  CHECK(w.status().last_setlist_position() == 3);
  // End of the setlist: "next" does nothing, the state reports it, and no jump is made.
  const std::size_t jumps = w.port.jumps.size();
  const std::uint64_t rev_end = w.plane.revision();
  bool changed = false;
  auto m = msg();
  m.mutable_next_live_set();
  r = w.send(m, &changed);
  CHECK(World::is_err(r, ws::v1::ERROR_CODE_END_OF_SETLIST) && w.port.jumps.size() == jumps && changed /* subscribers learn of the flag */);
  CHECK(w.plane.revision() == rev_end && r.revision() == rev_end);  // a refusal does not move the revision
  st = w.status();
  CHECK(st.end_of_setlist() && !st.start_of_setlist() && st.last_setlist_position() == 3 && st.current_live_set_id() == d);
  CHECK(World::is_err(w.step(true), ws::v1::ERROR_CODE_END_OF_SETLIST) && w.status().end_of_setlist());  // asking again stays
  // Stepping back clears it.
  expect_move(w.step(false), c, 2, true);
  CHECK(!w.status().end_of_setlist());
  // The flag also clears when the current Live Set changes by other means.
  w.port.jump_to(d);
  CHECK(World::is_err(w.step(true), ws::v1::ERROR_CODE_END_OF_SETLIST) && w.status().end_of_setlist());
  w.port.jump_to(a);
  CHECK(!w.status().end_of_setlist() && w.status().last_setlist_position() == 0);
  // Start of the setlist, symmetric.
  CHECK(World::is_err(w.step(false), ws::v1::ERROR_CODE_START_OF_SETLIST) && w.status().start_of_setlist() && !w.status().end_of_setlist());
  expect_move(w.step(true), b, 1, true);
  CHECK(!w.status().start_of_setlist());

  // Another setlist: loading it keeps the sound and the old position is gone; the Live Set that sounds sets the new position when it is in it.
  const std::string s2 = w.setlist("Encore", {x, c});
  const std::string sounding = w.port.sounding;
  const std::size_t jumps2 = w.port.jumps.size();
  r = w.load(s2);
  CHECK(r.has_load_setlist_reply() && w.port.jumps.size() == jumps2 && w.port.sounding == sounding);
  CHECK(w.status().loaded_setlist_id() == s2 && w.status().last_setlist_position() == -1);  // b is not in "Encore": nothing yet
  expect_move(w.step(true), x, 0, true);  // its first Live Set loads only now
  // Loading the loaded setlist again changes nothing.
  CHECK(w.load(s2).has_load_setlist_reply() && w.status().last_setlist_position() == 0);
  // Loading a setlist that holds the sounding Live Set positions on it.
  r = w.load(s);
  CHECK(r.load_setlist_reply().position() == -1);  // x is not in "Show"
  w.port.jump_to(c);
  CHECK(w.status().last_setlist_position() == 2);
  r = w.load(s2);  // c is at index 1 of Encore
  CHECK(r.load_setlist_reply().position() == 1 && w.status().last_setlist_position() == 1);
  // Unloading: back to All Live Sets.
  m = msg();
  m.mutable_unload_setlist();
  CHECK(w.send(m).has_ack() && w.status().loaded_setlist_id().empty() && w.status().last_setlist_position() == -1);
}

void test_stepping_all_live_sets() {
  World w;
  // Names chosen for the collation: case-insensitive, then id for equal names.
  const std::string n1 = w.live_set("banana"), n2 = w.live_set("Apple"), n3 = w.live_set("cherry"), n4 = w.live_set("APPLE"), n5 = w.live_set("Banana 2");
  // Expected order: Apple/APPLE (equal when folded: by id), banana, Banana 2, cherry.
  std::vector<std::string> apples = {n2, n4};
  if (n4 < n2) std::swap(apples[0], apples[1]);
  const std::vector<std::string> order = {apples[0], apples[1], n1, n5, n3};
  w.port.play(order[0]);
  w.port.jumps.clear();
  CHECK(w.status().loaded_setlist_id().empty());
  for (std::size_t i = 1; i < order.size(); ++i) expect_move(w.step(true), order[i], static_cast<int>(i), false);
  CHECK((w.port.jumps == std::vector<std::string>(order.begin() + 1, order.end())));
  // The end of All Live Sets: next does nothing.
  const auto r = w.step(true);
  CHECK(World::is_err(r, ws::v1::ERROR_CODE_END_OF_SETLIST) && r.error().details().at("scope") == "all_live_sets" && w.status().end_of_setlist());
  CHECK(w.port.jumps.size() == order.size() - 1);
  for (std::size_t i = order.size() - 1; i-- > 0;) expect_move(w.step(false), order[i], static_cast<int>(i), false);
  CHECK(World::is_err(w.step(false), ws::v1::ERROR_CODE_START_OF_SETLIST) && w.status().start_of_setlist());
  // The position of a setlist is untouched by walking All Live Sets (none is loaded).
  CHECK(w.status().last_setlist_position() == -1);
  // Starting from nothing: next is the first, previous the last.
  w.port.sounding.clear();
  w.port.effective.clear();
  expect_move(w.step(true), order[0], 0, false);
  w.port.sounding.clear();
  w.port.effective.clear();
  expect_move(w.step(false), order.back(), static_cast<int>(order.size()) - 1, false);
  // A rename shows in the order the next time the catalog is read (the catalog is the source of names).
  for (auto& e : w.port.cat)
    if (e.id == n3) e.name = "aardvark";
  w.port.play(n1);
  expect_move(w.step(false), apples[1], 2, false);
  w.port.play(apples[0]);
  expect_move(w.step(false), n3, 0, false);
}

void test_empty_and_degenerate() {
  World w;
  // No Live Set at all.
  auto r = w.step(true);
  CHECK(World::is_err(r, ws::v1::ERROR_CODE_EMPTY_SETLIST) && w.port.jumps.empty());
  const std::string a = w.live_set("A");
  const std::string empty = w.setlist("Nothing yet");
  w.load(empty);
  r = w.step(true);
  CHECK(World::is_err(r, ws::v1::ERROR_CODE_EMPTY_SETLIST) && r.error().details().at("scope") == "setlist");
  CHECK(World::is_err(w.step(false), ws::v1::ERROR_CODE_EMPTY_SETLIST) && w.port.jumps.empty());
  CHECK(!w.status().end_of_setlist() && !w.status().start_of_setlist());
  // One Live Set: next from the start moves to it, then next is the end and previous the start.
  auto m = msg();
  m.mutable_add_live_set_to_setlist()->set_setlist_id(empty);
  m.mutable_add_live_set_to_setlist()->set_live_set_id(a);
  CHECK(w.send(m).has_ack());
  expect_move(w.step(true), a, 0, true);
  CHECK(World::is_err(w.step(true), ws::v1::ERROR_CODE_END_OF_SETLIST));
  CHECK(World::is_err(w.step(false), ws::v1::ERROR_CODE_START_OF_SETLIST));
}

void test_missing_and_invalid_live_set() {
  World w;
  const std::string a = w.live_set("A"), b = w.live_set("B"), c = w.live_set("C");
  const std::string s = w.setlist("Show", {a, b, c});
  w.load(s);
  expect_move(w.step(true), a, 0, true);
  w.port.jumps.clear();
  // B's file disappears: stepping onto it reports it, does not move the position, does not skip.
  CHECK(::unlink(w.live->path_for(b).c_str()) == 0);
  auto r = w.step(true);
  CHECK(World::is_err(r, ws::v1::ERROR_CODE_LIVE_SET_LOAD_FAILED) && r.error().details().at("reason") == "missing" && r.error().details().at("id") == b);
  // (the catalog still lists it: the file check catches it before any jump)
  CHECK(w.port.jumps.empty() && w.status().last_setlist_position() == 0 && w.port.sounding == a);
  CHECK(World::is_err(w.step(true), ws::v1::ERROR_CODE_LIVE_SET_LOAD_FAILED));  // asking again: the same answer, no skipping
  // The player chooses C; stepping continues from there, and the missing one is flagged in the list.
  w.port.jump_to(c);
  CHECK(w.status().last_setlist_position() == 2);
  auto g = msg();
  g.mutable_get_setlists()->set_setlist_id(s);
  r = w.send(g);
  CHECK(r.setlists_reply().entries_size() == 3 && r.setlists_reply().entries(1).live_set_id() == b);
  // (still in the catalog but its file is gone: the catalog is the engine's list; a delete cleans it)
  // A catalog without B at all: flagged missing, kept, summary counts it.
  w.port.remove_from_catalog(b);
  r = w.send(g);
  CHECK(r.setlists_reply().entries(1).missing() && r.setlists_reply().entries(1).name().empty() && !r.setlists_reply().entries(0).missing());
  CHECK(r.setlists_reply().setlists(0).missing_count() == 1 && r.setlists_reply().setlists(0).live_set_count() == 3);
  w.port.jump_to(a);
  r = w.step(true);
  CHECK(World::is_err(r, ws::v1::ERROR_CODE_LIVE_SET_LOAD_FAILED) && r.error().details().at("reason") == "missing");
  CHECK(w.status().last_setlist_position() == 0);
  // An invalid file (garbage in B's place).
  {
    std::ofstream f(w.live->path_for(b), std::ios::binary);
    f << "this is not a Live Set \xFF\xFF";
  }
  w.port.add_to_catalog(CatalogEntry{b, "B", w.live->path_for(b)});
  r = w.step(true);
  CHECK(World::is_err(r, ws::v1::ERROR_CODE_LIVE_SET_LOAD_FAILED) && r.error().details().at("reason") == "invalid_live_set");
  CHECK(w.port.jumps.back() == a);  // no jump was made to B
  // An entry whose Live Set has no file and is unknown: removing it from the setlist (or deleting the id) cleans the list.
  auto del = msg();
  del.mutable_delete_live_set()->set_id(b);
  CHECK(w.send(del).has_ack());
  CHECK((w.ids_in(s) == std::vector<std::string>{a, c}));
  expect_move(w.step(true), c, 1, true);
}

void test_cascades_and_anchor() {
  World w;
  const std::string a = w.live_set("A"), b = w.live_set("B"), c = w.live_set("C"), d = w.live_set("D");
  const std::string s = w.setlist("S", {a, b, c, d});
  const std::string other = w.setlist("O", {b, d});
  w.load(s);
  w.port.play(c);
  CHECK(w.status().last_setlist_position() == 2);
  // A reorder keeps the position on the same Live Set.
  auto m = msg();
  {
    auto* x = m.mutable_reorder_live_sets_in_setlist();
    x->set_setlist_id(s);
    for (const std::string* id : {&d, &c, &b, &a}) x->add_live_set_ids(*id);
  }
  CHECK(w.send(m).has_ack() && w.status().last_setlist_position() == 1);
  expect_move(w.step(true), b, 2, true);
  // Removing the Live Set at the position moves the anchor to its predecessor: next lands on what took its place.
  w.port.play(c);  // position 1 (d c b a)
  m = msg();
  m.mutable_remove_live_set_from_setlist()->set_setlist_id(s);
  m.mutable_remove_live_set_from_setlist()->set_live_set_id(c);
  CHECK(w.send(m).has_ack() && w.status().last_setlist_position() == 0);
  expect_move(w.step(true), b, 1, true);
  // Deleting a Live Set removes it from every setlist; the one that sounds cannot be deleted.
  m = msg();
  m.mutable_delete_live_set()->set_id(b);
  CHECK(World::is_err(w.send(m), ws::v1::ERROR_CODE_LIVE_SET_IN_USE) && fs::exists(w.live->path_for(b)));
  w.port.slow = true;
  w.port.jump_to(a);  // loading a: a is the effective one, b still sounds
  m = msg();
  m.mutable_delete_live_set()->set_id(a);
  CHECK(World::is_err(w.send(m), ws::v1::ERROR_CODE_LIVE_SET_IN_USE));  // a load is heading there
  w.port.finish();
  w.port.slow = false;
  m = msg();
  m.mutable_delete_live_set()->set_id(d);
  CHECK(w.send(m).has_ack());
  CHECK(!fs::exists(w.live->path_for(d)) && w.port.cat.size() == 3);
  CHECK((w.ids_in(s) == std::vector<std::string>{b, a}) && (w.ids_in(other) == std::vector<std::string>{b}));
  // Deleting the loaded setlist unloads it and leaves the sound alone.
  const std::string sounding = w.port.sounding;
  m = msg();
  m.mutable_delete_setlist()->set_id(s);
  CHECK(w.send(m).has_ack() && w.status().loaded_setlist_id().empty() && w.port.sounding == sounding);
  CHECK(fs::exists(w.live->path_for(a)) && fs::exists(w.live->path_for(b)));
}

void test_slow_load_follows_request() {
  World w;
  const std::string a = w.live_set("A"), b = w.live_set("B"), c = w.live_set("C");
  const std::string s = w.setlist("S", {a, b, c});
  w.load(s);
  w.port.play(a);
  w.port.slow = true;  // the next load takes a while: the old one keeps sounding
  expect_move(w.step(true), b, 1, true);
  CHECK(w.port.sounding == a && w.status().current_live_set_id() == a && w.status().last_setlist_position() == 1);
  auto r = w.step(true);  // a second tap supersedes the first load
  CHECK(r.has_step_reply() && r.step_reply().position() == 2 && r.step_reply().superseded_previous());
  w.port.finish();
  CHECK(w.status().current_live_set_id() == c && w.status().last_setlist_position() == 2);
  // The player goes back before it finishes: the position follows the request.
  expect_move(w.step(false), b, 1, true);
  w.port.jump_to(c);  // cancels, back to what sounds
  CHECK(w.status().last_setlist_position() == 2);
}

void test_status_and_tick() {
  World w;
  const std::string a = w.live_set("A"), b = w.live_set("B");
  const std::string s = w.setlist("S", {a, b});
  w.ctl->tick();
  CHECK(!w.ctl->tick());  // nothing changed since the last tick
  w.load(s);
  CHECK(w.ctl->tick());
  w.port.jump_to(b);  // a jump made by someone else (a plain JumpToLiveSet)
  CHECK(w.ctl->tick() && w.ctl->last_setlist_position() == 1);
  CHECK(!w.ctl->tick());
  // C++ API.
  CHECK(w.ctl->load_setlist("nope") == sl::Status::kNoSuchSetlist);
  CHECK(w.ctl->loaded_setlist_id() == s);
  const StepOutcome o = w.ctl->step(false);
  CHECK(o.result == StepResult::kMoved && o.live_set_id == a && o.position == 0 && o.in_setlist && o.jump.result == SwitchOutcome::Result::kAccepted);
  CHECK(std::string(to_string(o.result)) == "moved" && std::string(to_string(StepResult::kEndOfList)) == "end_of_setlist");
  // GetSetlists: summaries carry the loaded flag and counts, the status is included.
  auto m = msg();
  m.mutable_get_setlists();
  const auto r = w.send(m);
  CHECK(r.setlists_reply().setlists_size() == 1 && r.setlists_reply().setlists(0).loaded() && r.setlists_reply().status().loaded_setlist_id() == s);
  CHECK(r.setlists_reply().status().library_revision() == w.store->library_revision());
}


// ---- review fixes ------------------------------------------------------------------------------------------------------------------------
void test_limit_and_storage_mapping() {
  World w;
  std::string id;
  for (std::size_t i = 0; i < sl::kMaxSetlists; ++i) CHECK(w.store->create_setlist("s" + std::to_string(i), id) == sl::Status::kOk);
  const std::uint64_t rev = w.plane.revision();
  auto m = msg();
  m.mutable_create_setlist()->set_name("101st");
  bool changed = true;
  auto r = w.send(m, &changed);
  CHECK(World::is_err(r, ws::v1::ERROR_CODE_LIMIT_REACHED) && r.error().details().at("limit_name") == "setlists" && r.error().details().at("limit") == "100");
  CHECK(!changed && w.plane.revision() == rev && r.revision() == rev && w.store->library().setlists_size() == 100);
  // 9th tag on a setlist, and the 65th tag.
  const std::string s0 = w.store->library().setlists(0).id();
  std::vector<std::string> tags;
  for (int i = 0; i < 9; ++i) {
    m = msg();
    m.mutable_create_tag()->set_name("t" + std::to_string(i));
    tags.push_back(w.send(m).created_reply().id());
  }
  const std::uint64_t rev2 = w.plane.revision();
  m = msg();
  m.mutable_set_setlist_tags()->set_setlist_id(s0);
  for (const std::string& t : tags) m.mutable_set_setlist_tags()->add_tag_ids(t);
  r = w.send(m);
  CHECK(World::is_err(r, ws::v1::ERROR_CODE_LIMIT_REACHED) && r.error().details().at("limit_name") == "tags_per_setlist" && r.error().details().at("limit") == "8");
  CHECK(w.plane.revision() == rev2 && w.store->find_setlist(s0)->tag_ids_size() == 0);
  while (static_cast<std::size_t>(w.store->library().tags_size()) < sl::kMaxTags) CHECK(w.store->create_tag("x", id) == sl::Status::kOk);
  m = msg();
  m.mutable_create_tag()->set_name("65th");
  r = w.send(m);
  CHECK(World::is_err(r, ws::v1::ERROR_CODE_LIMIT_REACHED) && r.error().details().at("limit_name") == "tags" && r.error().details().at("limit") == "64");
}

void test_injected_store_faults() {
  {  // a fault before the rename: STORAGE_FAILED, nothing changed
    bool arm = false;
    World w([&](ls::StoreOptions&, sl::StoreOptions& so) {
      so.on_stage = [&arm](ls::Stage s) { return arm && s == ls::Stage::kFsynced ? ls::HookAction::kAbort : ls::HookAction::kContinue; };
    });
    const std::string keep = w.setlist("Keep");
    arm = true;
    const std::uint64_t rev = w.plane.revision();
    const auto seq = w.status().status_seq();
    auto m = msg();
    m.mutable_create_setlist()->set_name("Lost");
    bool changed = true;
    auto r = w.send(m, &changed);
    CHECK(World::is_err(r, ws::v1::ERROR_CODE_STORAGE_FAILED) && r.error().details().at("reason") == "fault_injected" && !changed);
    CHECK(w.plane.revision() == rev && r.revision() == rev && w.store->library().setlists_size() == 1 && w.status().setlist_count() == 1);
    CHECK(w.status().status_seq() == seq && w.status().library_revision() == w.store->library_revision());
    m = msg();
    m.mutable_rename_setlist()->set_id(keep);
    m.mutable_rename_setlist()->set_name("Renamed");
    CHECK(World::is_err(w.send(m), ws::v1::ERROR_CODE_STORAGE_FAILED) && w.store->find_setlist(keep)->name() == "Keep");
  }
  {  // the directory sync fails: applied, and the client is told it is not known durable
    bool arm = false;
    World w([&](ls::StoreOptions&, sl::StoreOptions& so) { (void)so; });
    w.ctl.reset();
    w.store.reset();
    sl::StoreOptions so;
    so.dir = w.set_dir;
    so.rng = &w.rng;
    so.inject_dir_sync_failure = true;
    CHECK(sl::SetlistStore::open(so, w.store) == sl::Status::kOk);
    w.ctl = std::make_unique<SetlistController>(w.plane, *w.store, *w.live, w.port);
    (void)arm;
    const std::uint64_t rev = w.plane.revision();
    auto m = msg();
    m.mutable_create_setlist()->set_name("Applied");
    bool changed = false;
    auto r = w.send(m, &changed);
    CHECK(r.has_created_reply() && r.created_reply().not_durable() && changed && r.revision() == rev + 1 && w.store->library().setlists_size() == 1);
    const std::string id = r.created_reply().id();
    CHECK(w.status().not_durable_commits() == 1);
    m = msg();
    m.mutable_rename_setlist()->set_id(id);
    m.mutable_rename_setlist()->set_name("Applied 2");
    r = w.send(m);
    CHECK(r.has_ack() && r.ack().not_durable() && w.store->find_setlist(id)->name() == "Applied 2" && w.status().not_durable_commits() == 2);
    m = msg();
    m.mutable_get_setlists();
    CHECK(w.send(m).has_setlists_reply());  // reads carry no flag
  }
}

void test_reanchor_on_insert() {
  World w;
  const std::string a = w.live_set("A"), b = w.live_set("B"), c = w.live_set("C"), d = w.live_set("D"), e = w.live_set("E"), f = w.live_set("F");
  const std::string s = w.setlist("S", {a, b, c, d});
  w.load(s);
  expect_move(w.step(true), a, 0, true);
  expect_move(w.step(true), b, 1, true);
  expect_move(w.step(true), c, 2, true);
  auto add = [&](const std::string& id, int index) {
    auto m = msg();
    m.mutable_add_live_set_to_setlist()->set_setlist_id(s);
    m.mutable_add_live_set_to_setlist()->set_live_set_id(id);
    m.mutable_add_live_set_to_setlist()->set_index(static_cast<std::uint32_t>(index));
    return w.send(m);
  };
  CHECK(add(e, 0).has_ack());  // before the current one: c moves from 2 to 3
  CHECK(w.status().last_setlist_position() == 3);
  CHECK(add(f, 3).has_ack());  // at the current index (<= p): c moves to 4
  CHECK(w.status().last_setlist_position() == 4);
  expect_move(w.step(true), d, 5, true);  // the Live Set after the CURRENT one
  w.port.play(c);
  CHECK(w.status().last_setlist_position() == 4);
  const std::string g = w.live_set("G");
  CHECK(add(g, 5).has_ack());  // after the current one: unchanged
  CHECK(w.status().last_setlist_position() == 4);
  expect_move(w.step(false), f, 3, true);
}

void test_missing_by_file_and_step_mapping() {
  World w;
  const std::string a = w.live_set("A"), b = w.live_set("B");
  // A Live Set with a valid file that the catalog does not list (made by someone else): not missing.
  std::string ghost;
  CHECK(w.store->create_live_set(*w.live, "Ghost", ghost) == sl::Status::kOk);
  const std::string s = w.setlist("S", {a, b});
  CHECK(w.store->add_live_set(s, ghost, std::nullopt) == sl::Status::kOk);
  CHECK(w.store->add_live_set(s, sfixtures::uuid_n(4242), std::nullopt) == sl::Status::kOk);  // no file anywhere
  auto g = msg();
  g.mutable_get_setlists()->set_setlist_id(s);
  auto r = w.send(g);
  CHECK(r.setlists_reply().entries_size() == 4);
  CHECK(!r.setlists_reply().entries(2).missing() && r.setlists_reply().entries(2).name() == "Ghost" && r.setlists_reply().entries(3).missing());
  CHECK(r.setlists_reply().setlists(0).missing_count() == 1);
  // A catalog entry whose file was deleted behind the engine's back is missing too.
  CHECK(::unlink(w.live->path_for(b).c_str()) == 0);
  r = w.send(g);
  CHECK(r.setlists_reply().entries(1).missing() && r.setlists_reply().setlists(0).missing_count() == 2);

  // StepReply maps every switcher result; a switcher that did not take the request leaves the position alone.
  World v;
  const std::string x = v.live_set("X"), y = v.live_set("Y"), z = v.live_set("Z");
  const std::string t = v.setlist("T", {x, y, z});
  v.load(t);
  v.port.force = SwitchOutcome::Result::kAlreadyLoading;
  auto st = v.step(true);
  CHECK(st.has_step_reply() && st.step_reply().jump_result() == ws::v1::JUMP_RESULT_ALREADY_LOADING && st.step_reply().live_set_id() == x);
  v.port.force = SwitchOutcome::Result::kAlreadyLoaded;
  st = v.step(true);
  CHECK(st.has_step_reply() && st.step_reply().jump_result() == ws::v1::JUMP_RESULT_ALREADY_LOADED && st.step_reply().live_set_id() == y);
  v.port.force.reset();
  v.port.refuse = true;
  const std::size_t jumps = v.port.jumps.size();
  st = v.step(true);
  CHECK(World::is_err(st, ws::v1::ERROR_CODE_LIVE_SET_LOAD_FAILED) && st.error().details().at("reason") == "switch_not_applied" && v.port.jumps.size() == jumps + 1);
  CHECK(v.status().last_setlist_position() == 1 && v.port.effective == y);
  v.port.refuse = false;
  expect_move(v.step(true), z, 2, true);
}

void test_delete_live_set_failures() {
  {  // the file cannot be removed: the references are gone, the Live Set stays, the partial result is reported and published
    bool arm = false;
    std::string live_dir;
    World w([&](ls::StoreOptions& lo, sl::StoreOptions&) { (void)lo; });
    w.ctl.reset();
    w.live.reset();
    ls::StoreOptions lo;
    lo.dir = w.live_dir;
    lo.inject_remove_failure = true;
    CHECK(ls::LiveSetStore::open(lo, w.live) == ls::Status::kOk);
    w.ctl = std::make_unique<SetlistController>(w.plane, *w.store, *w.live, w.port);
    (void)arm;
    const std::string a = w.live_set("A"), b = w.live_set("B");
    const std::string s = w.setlist("S", {a, b});
    w.load(s);
    w.port.play(a);
    const std::uint64_t rev = w.plane.revision();
    auto m = msg();
    m.mutable_delete_live_set()->set_id(b);
    bool changed = false;
    auto r = w.send(m, &changed);
    CHECK(World::is_err(r, ws::v1::ERROR_CODE_STORAGE_FAILED) && r.error().details().at("partial") == "references_removed_file_kept" &&
          r.error().details().at("removed_from_setlists") == "1");
    CHECK(changed && w.plane.revision() == rev + 1 && r.revision() == rev + 1);
    CHECK((w.ids_in(s) == std::vector<std::string>{a}) && fs::exists(w.live->path_for(b)) && w.port.cat.size() == 2);
    CHECK(w.status().loaded_setlist_size() == 1);
    // Nothing referenced it: a plain failure that changes nothing.
    const std::string c = w.live_set("C");
    const std::uint64_t rev2 = w.plane.revision();
    m = msg();
    m.mutable_delete_live_set()->set_id(c);
    r = w.send(m, &changed);
    CHECK(World::is_err(r, ws::v1::ERROR_CODE_STORAGE_FAILED) && r.error().details().count("partial") == 0 && !changed && w.plane.revision() == rev2 && fs::exists(w.live->path_for(c)));
  }
  {  // kNotFound: the file vanished; the command cleans the references and the catalog
    World w;
    const std::string a = w.live_set("A"), b = w.live_set("B");
    const std::string s = w.setlist("S", {a, b});
    CHECK(::unlink(w.live->path_for(b).c_str()) == 0);
    auto m = msg();
    m.mutable_delete_live_set()->set_id(b);
    CHECK(w.send(m).has_ack());
    CHECK((w.ids_in(s) == std::vector<std::string>{a}) && w.port.cat.size() == 1);
    // ... and the same when it is listed in no setlist
    const std::string c = w.live_set("C");
    CHECK(::unlink(w.live->path_for(c).c_str()) == 0);
    m = msg();
    m.mutable_delete_live_set()->set_id(c);
    CHECK(w.send(m).has_ack() && w.port.cat.size() == 1);
    // unknown everywhere
    m = msg();
    m.mutable_delete_live_set()->set_id(c);
    CHECK(World::is_err(w.send(m), ws::v1::ERROR_CODE_NO_SUCH_LIVE_SET));
  }
  {  // a catalog entry whose file is not named <id>.liveset is deleted through the catalog's path; a path outside the directory is refused
    World w;
    const std::string a = w.live_set("A");
    const std::string odd = w.live_set("Odd");
    const std::string odd_path = w.live_dir + "/custom-name.liveset";
    fs::rename(w.live->path_for(odd), odd_path);
    for (auto& e : w.port.cat)
      if (e.id == odd) e.path = odd_path;
    const std::string s = w.setlist("S", {a, odd});
    auto m = msg();
    m.mutable_delete_live_set()->set_id(odd);
    CHECK(w.send(m).has_ack() && !fs::exists(odd_path) && (w.ids_in(s) == std::vector<std::string>{a}) && w.port.cat.size() == 1);
    const std::string evil = w.live_set("Evil");
    const std::string outside = w.data + "/outside.liveset";
    fs::copy_file(w.live->path_for(evil), outside);
    for (auto& e : w.port.cat)
      if (e.id == evil) e.path = outside;
    m = msg();
    m.mutable_delete_live_set()->set_id(evil);
    const auto r = w.send(m);
    CHECK(World::is_err(r, ws::v1::ERROR_CODE_STORAGE_FAILED) && fs::exists(outside) && fs::exists(w.live->path_for(evil)));
  }
}

// ---- ws.v1 over a socket, concurrent clients ---------------------------------------------------------------------------------------------
struct Client {
  int fd = -1;
  bool open(const std::string& path) {
    fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
    return fd >= 0 && ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
  }
  ~Client() {
    if (fd >= 0) ::close(fd);
  }
  bool read_exact(std::string& out, std::size_t n) {
    out.clear();
    while (out.size() < n) {
      pollfd pf{fd, POLLIN, 0};
      if (::poll(&pf, 1, 5000) <= 0) return false;
      char buf[4096];
      const ssize_t k = ::recv(fd, buf, std::min(sizeof buf, n - out.size()), 0);
      if (k <= 0) return false;
      out.append(buf, static_cast<std::size_t>(k));
    }
    return true;
  }
  bool call(const ws::v1::ClientMessage& m, ws::v1::ServerMessage& reply) {
    std::string b;
    (void)m.SerializeToString(&b);
    const std::uint32_t n = static_cast<std::uint32_t>(b.size());
    const char h[4] = {char(n >> 24), char(n >> 16), char(n >> 8), char(n)};
    const std::string s = std::string(h, 4) + b;
    if (::send(fd, s.data(), s.size(), 0) != static_cast<ssize_t>(s.size())) return false;
    for (;;) {
      std::string hd, body;
      if (!read_exact(hd, 4)) return false;
      const std::uint32_t len = (std::uint8_t(hd[0]) << 24) | (std::uint8_t(hd[1]) << 16) | (std::uint8_t(hd[2]) << 8) | std::uint8_t(hd[3]);
      if (!read_exact(body, len) || !reply.ParseFromString(body)) return false;
      if (reply.request_id() == m.request_id()) return true;
      pushed.push_back(reply);
    }
  }
  std::vector<ws::v1::ServerMessage> pushed;
};

void test_socket_concurrent_clients() {
  World w;
  const std::string a = w.live_set("A"), b = w.live_set("B"), c = w.live_set("C");
  w.port.play(a);
  const std::string path = "/tmp/velora-sl-" + std::to_string(::getpid()) + ".sock";
  ControlServer server(w.plane, path);
  std::string err;
  CHECK(server.start(err));
  server.set_tick([&] { return w.ctl->tick(); });
  server.set_thread_start([&] {
    w.live->rebind_control_thread();
    w.store->rebind_control_thread();
  });

  Client sub;
  ws::v1::ServerMessage rep;
  auto hello = [&](Client& c2) {
    auto h = msg();
    h.mutable_hello()->set_version(1);
    return c2.open(path) && c2.call(h, rep) && rep.has_hello_ack();
  };
  CHECK(hello(sub));
  auto sm = msg();
  sm.mutable_subscribe()->set_enable(true);
  CHECK(sub.call(sm, rep) && rep.has_ack());

  constexpr int kClients = 4, kEach = 10;
  std::atomic<int> failures{0};
  std::vector<std::uint64_t> revisions[kClients];
  std::vector<std::thread> threads;
  for (int ci = 0; ci < kClients; ++ci) {
    threads.emplace_back([&, ci] {
      Client cl;
      ws::v1::ServerMessage r;
      auto h = msg();
      h.mutable_hello()->set_version(1);
      if (!(cl.open(path) && cl.call(h, r) && r.has_hello_ack())) {
        ++failures;
        return;
      }
      for (int k = 0; k < kEach; ++k) {
        auto m = msg();
        m.mutable_create_setlist()->set_name("c" + std::to_string(ci) + "-" + std::to_string(k));
        if (!cl.call(m, r) || !r.has_created_reply()) {
          std::printf("client %d call %d: %s\n", ci, k, r.has_error() ? r.error().message().c_str() : "no reply");
          ++failures;
          continue;
        }
        revisions[ci].push_back(r.revision());
      }
    });
  }
  for (auto& t : threads) t.join();
  CHECK(failures.load() == 0);
  // The control thread serialised them: every command got its own revision, strictly increasing per client, all distinct across clients.
  std::set<std::uint64_t> all;
  for (int ci = 0; ci < kClients; ++ci) {
    for (std::size_t i = 0; i < revisions[ci].size(); ++i) {
      all.insert(revisions[ci][i]);
      if (i) CHECK(revisions[ci][i] > revisions[ci][i - 1]);
    }
  }
  CHECK(all.size() == static_cast<std::size_t>(kClients * kEach));
  Client c1;
  CHECK(hello(c1));
  auto g = msg();
  g.mutable_get_setlists();
  CHECK(c1.call(g, rep) && rep.setlists_reply().setlists_size() == kClients * kEach);
  // Stepping over the socket: a typed result, then the State pushed to the subscriber carries the loaded setlist and position.
  const std::string s = rep.setlists_reply().setlists(0).id();
  for (const std::string& l : {a, b, c}) {
    auto ad = msg();
    ad.mutable_add_live_set_to_setlist()->set_setlist_id(s);
    ad.mutable_add_live_set_to_setlist()->set_live_set_id(l);
    CHECK(c1.call(ad, rep) && rep.has_ack());
  }
  auto ld = msg();
  ld.mutable_load_setlist()->set_id(s);
  CHECK(c1.call(ld, rep) && rep.has_load_setlist_reply() && rep.load_setlist_reply().position() == 0);  // a sounds and is index 0
  auto nx = msg();
  nx.mutable_next_live_set();
  CHECK(c1.call(nx, rep) && rep.has_step_reply() && rep.step_reply().live_set_id() == b);
  nx = msg();
  nx.mutable_next_live_set();
  CHECK(c1.call(nx, rep) && rep.step_reply().live_set_id() == c);
  auto end = msg();
  end.mutable_next_live_set();
  CHECK(c1.call(end, rep) && rep.has_error() && rep.error().code() == ws::v1::ERROR_CODE_END_OF_SETLIST);
  // The subscriber saw State pushes; the last one reports the end of the setlist.
  ws::v1::ServerMessage last;
  bool got = false;
  for (int i = 0; i < 400 && !got; ++i) {
    pollfd pf{sub.fd, POLLIN, 0};
    while (::poll(&pf, 1, 20) > 0) {
      std::string hd, body;
      if (!sub.read_exact(hd, 4)) break;
      const std::uint32_t len = (std::uint8_t(hd[0]) << 24) | (std::uint8_t(hd[1]) << 16) | (std::uint8_t(hd[2]) << 8) | std::uint8_t(hd[3]);
      if (!sub.read_exact(body, len)) break;
      ws::v1::ServerMessage pm;
      if (pm.ParseFromString(body) && pm.has_state()) {
        last = pm;
        got = pm.state().setlists().end_of_setlist();
      }
      if (got) break;
    }
  }
  CHECK(got && last.state().setlists().loaded_setlist_id() == s && last.state().setlists().last_setlist_position() == 2);
  server.stop();
}

}  // namespace

int main() {
  test_name_collation();
  test_commands_and_errors();
  test_plane_without_setlists();
  test_new_live_set_content_and_default();
  test_stepping_in_setlist();
  test_stepping_all_live_sets();
  test_empty_and_degenerate();
  test_missing_and_invalid_live_set();
  test_cascades_and_anchor();
  test_slow_load_follows_request();
  test_status_and_tick();
  test_limit_and_storage_mapping();
  test_injected_store_faults();
  test_reanchor_on_insert();
  test_missing_by_file_and_step_mapping();
  test_delete_live_set_failures();
  test_socket_concurrent_clients();
  if (g_fail) {
    std::printf("setlist_controller_tests: %d FAILED\n", g_fail);
    return 1;
  }
  std::printf("setlist_controller_tests: ok\n");
  return 0;
}
