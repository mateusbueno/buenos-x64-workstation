// ConfigService tests (Foundation: sysd config service): the ws.v1 protocol as a state machine, driven by a fake loop (no sockets, no sleeps, no clock):
// handshake rules, GetConfig / SetConfig / SubscribeConfig, push on every change to every subscriber in the same call that made the change (the 100 ms bound is
// "before handle() returns"), typed errors that change and push nothing, engine commands refused, serialised concurrent SetConfig with strictly increasing
// revisions, not_durable propagation.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "velora/sysd/config_service.hpp"

namespace fs = std::filesystem;
namespace sd = velora::sysd;

static int g_fail = 0;
#define CHECK(c)                                               \
  do {                                                         \
    if (!(c)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

namespace {

struct TmpDir {
  std::string path;
  TmpDir() {
    std::string t = (fs::temp_directory_path() / "velora-sysd-svc-XXXXXX").string();
    if (!::mkdtemp(t.data())) std::abort();
    path = t;
  }
  ~TmpDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

struct Rig {
  TmpDir dir;
  sd::StoreOptions so;
  std::unique_ptr<sd::ConfigStore> store;
  std::unique_ptr<sd::ConfigService> svc;
  explicit Rig(bool inject_dir_sync_failure = false) {
    so.dir = dir.path;
    so.inject_dir_sync_failure = inject_dir_sync_failure;
    store = std::make_unique<sd::ConfigStore>(so);
    svc = std::make_unique<sd::ConfigService>(*store);
  }
  std::vector<sd::Outbound> send(sd::ConnId c, const ws::v1::ClientMessage& m) {
    std::vector<sd::Outbound> out;
    svc->handle(c, m.SerializeAsString(), out);
    return out;
  }
  // A connection that completed Hello.
  sd::ConnId connect(sd::ConnId id, std::uint32_t version = 1) {
    svc->open(id);
    ws::v1::ClientMessage h;
    h.set_request_id(1);
    h.mutable_hello()->set_version(version);
    const auto out = send(id, h);
    CHECK(out.size() == 1 && out[0].msg.has_hello_ack() && out[0].msg.hello_ack().version() == version && !out[0].close_after);
    return id;
  }
};

ws::v1::ClientMessage get(std::uint64_t rid) {
  ws::v1::ClientMessage m;
  m.set_request_id(rid);
  m.mutable_get_config();
  return m;
}
ws::v1::ClientMessage subscribe(std::uint64_t rid, bool on = true) {
  ws::v1::ClientMessage m;
  m.set_request_id(rid);
  m.mutable_subscribe_config()->set_enable(on);
  return m;
}
ws::v1::ClientMessage set_clock(std::uint64_t rid, ws::v1::ClockSource c) {
  ws::v1::ClientMessage m;
  m.set_request_id(rid);
  m.mutable_set_config()->mutable_patch()->set_clock_source(c);
  return m;
}
ws::v1::ClientMessage set_sustain_max(std::uint64_t rid, unsigned mx) {
  ws::v1::ClientMessage m;
  m.set_request_id(rid);
  ws::v1::PedalPatch* p = m.mutable_set_config()->mutable_patch()->mutable_sustain();
  p->set_min(1);
  p->set_max(mx);
  return m;
}
bool is_error(const sd::Outbound& o, ws::v1::ErrorCode c) { return o.msg.has_error() && o.msg.error().code() == c; }

void test_handshake() {
  Rig r;
  r.svc->open(1);
  {  // not Hello first
    const auto out = r.send(1, get(7));
    CHECK(out.size() == 1 && is_error(out[0], ws::v1::ERROR_CODE_HANDSHAKE_REQUIRED) && out[0].close_after && out[0].msg.request_id() == 7);
  }
  r.svc->open(2);
  {  // malformed first frame closes
    std::vector<sd::Outbound> out;
    r.svc->handle(2, std::string("\xff\xff\xff\xff\x07garbage", 12), out);
    CHECK(out.size() == 1 && is_error(out[0], ws::v1::ERROR_CODE_MALFORMED_MESSAGE) && out[0].close_after);
  }
  for (const std::uint32_t v : {0u, 2u, 99u, 0xFFFFFFFFu}) {  // N = 1: 0 too old, above too new
    r.svc->open(3);
    ws::v1::ClientMessage h;
    h.mutable_hello()->set_version(v);
    const auto out = r.send(3, h);
    CHECK(out.size() == 1 && is_error(out[0], ws::v1::ERROR_CODE_UNSUPPORTED_VERSION) && out[0].close_after);
    CHECK(out[0].msg.error().details().at("reason") == (v == 0 ? "too_old" : "too_new") && out[0].msg.error().details().at("supported") == "1");
  }
  {  // a good Hello answers the config.v1 feature and the current revision
    r.svc->open(4);
    ws::v1::ClientMessage h;
    h.set_request_id(5);
    h.mutable_hello()->set_version(1);
    h.mutable_hello()->add_features("future.v9");  // unknown features are ignored
    const auto out = r.send(4, h);
    CHECK(out.size() == 1 && out[0].msg.hello_ack().features_size() == 1 && out[0].msg.hello_ack().features(0) == "config.v1");
    // Hello again: not a command.
    const auto again = r.send(4, h);
    CHECK(again.size() == 1 && is_error(again[0], ws::v1::ERROR_CODE_INVALID_COMMAND) && !again[0].close_after);
  }
  // A frame after a failed handshake on an unknown connection is ignored.
  CHECK(r.send(42, get(1)).empty());
}

void test_engine_commands_refused() {
  Rig r;
  r.connect(1);
  ws::v1::ClientMessage m;
  m.set_request_id(9);
  m.mutable_get_state();
  auto out = r.send(1, m);
  CHECK(out.size() == 1 && is_error(out[0], ws::v1::ERROR_CODE_INVALID_COMMAND) && !out[0].close_after);
  m.Clear();
  m.mutable_set_led()->set_led_id(1);
  out = r.send(1, m);
  CHECK(out.size() == 1 && is_error(out[0], ws::v1::ERROR_CODE_INVALID_COMMAND));
  m.Clear();
  m.mutable_subscribe()->set_enable(true);  // the engine's State subscription is not sysd's
  out = r.send(1, m);
  CHECK(out.size() == 1 && is_error(out[0], ws::v1::ERROR_CODE_INVALID_COMMAND) && r.svc->subscribers() == 0);
  out = r.send(1, ws::v1::ClientMessage{});  // no command
  CHECK(out.size() == 1 && is_error(out[0], ws::v1::ERROR_CODE_INVALID_COMMAND));
  CHECK(r.svc->revision() == 0);
}

void test_get_set_subscribe() {
  Rig r;
  r.connect(1);
  r.connect(2);
  r.connect(3);
  auto out = r.send(1, get(10));
  CHECK(out.size() == 1 && out[0].msg.request_id() == 10 && out[0].msg.has_config_reply());
  CHECK(out[0].msg.config_reply().revision() == 0 && out[0].msg.config_reply().config().clock_source() == ws::v1::CLOCK_SOURCE_INTERNAL);
  // Subscribe: ack, then the full config at once (request_id 0).
  out = r.send(2, subscribe(11));
  CHECK(out.size() == 2 && out[0].msg.has_ack() && out[0].msg.request_id() == 11);
  CHECK(out[1].to == 2 && out[1].msg.request_id() == 0 && out[1].msg.has_config_reply() && out[1].msg.config_reply().revision() == 0);
  out = r.send(3, subscribe(12));
  CHECK(out.size() == 2);
  CHECK(r.svc->subscribers() == 2);
  // Connection 1 (not subscribed) changes the config: its reply first, then one push to each subscriber, in the same call.
  out = r.send(1, set_clock(13, ws::v1::CLOCK_SOURCE_DIN));
  CHECK(out.size() == 3);
  CHECK(out[0].to == 1 && out[0].msg.request_id() == 13 && out[0].msg.config_reply().revision() == 1 && !out[0].msg.config_reply().not_durable());
  CHECK(out[0].msg.config_reply().config().clock_source() == ws::v1::CLOCK_SOURCE_DIN);
  std::set<sd::ConnId> pushed;
  for (std::size_t i = 1; i < out.size(); ++i) {
    CHECK(out[i].msg.request_id() == 0 && out[i].msg.has_config_reply() && out[i].msg.config_reply().revision() == 1);
    CHECK(out[i].msg.config_reply().config().clock_source() == ws::v1::CLOCK_SOURCE_DIN);
    pushed.insert(out[i].to);
  }
  CHECK((pushed == std::set<sd::ConnId>{2, 3}));
  // A subscriber that changes it gets its reply and also the push.
  out = r.send(2, set_clock(14, ws::v1::CLOCK_SOURCE_USB));
  CHECK(out.size() == 3 && out[0].to == 2 && out[0].msg.request_id() == 14 && out[1].msg.config_reply().revision() == 2);
  // Unsubscribe: acked, no more pushes, and no immediate push.
  out = r.send(3, subscribe(15, false));
  CHECK(out.size() == 1 && out[0].msg.has_ack() && r.svc->subscribers() == 1);
  out = r.send(1, set_clock(16, ws::v1::CLOCK_SOURCE_INTERNAL));
  CHECK(out.size() == 2 && out[1].to == 2);
  // A closed connection stops receiving.
  r.svc->close(2);
  out = r.send(1, set_clock(17, ws::v1::CLOCK_SOURCE_DIN));
  CHECK(out.size() == 1);
  CHECK(r.svc->revision() == 4);
}

void test_rejected_and_unchanged_push_nothing() {
  Rig r;
  r.connect(1);
  r.connect(2);
  (void)r.send(2, subscribe(1));
  (void)r.send(1, set_clock(2, ws::v1::CLOCK_SOURCE_DIN));
  const std::uint64_t rev = r.svc->revision();
  // Typed errors: nothing changes, nothing is pushed, the error carries the request id and the unchanged revision.
  auto out = r.send(1, set_clock(3, ws::v1::CLOCK_SOURCE_UNSPECIFIED));
  CHECK(out.size() == 1 && is_error(out[0], ws::v1::ERROR_CODE_UNKNOWN_ENUM_VALUE) && out[0].msg.request_id() == 3 && out[0].msg.revision() == rev);
  out = r.send(1, set_sustain_max(4, 0));  // min 1 >= max 0
  CHECK(out.size() == 1 && is_error(out[0], ws::v1::ERROR_CODE_INVALID_CONFIG) && out[0].msg.error().details().at("field") == "sustain");
  out = r.send(1, set_sustain_max(5, 5000));
  CHECK(out.size() == 1 && is_error(out[0], ws::v1::ERROR_CODE_OUT_OF_RANGE));
  ws::v1::ClientMessage empty;
  empty.set_request_id(6);
  empty.mutable_set_config();
  out = r.send(1, empty);
  CHECK(out.size() == 1 && is_error(out[0], ws::v1::ERROR_CODE_INVALID_COMMAND));
  CHECK(r.svc->revision() == rev && r.svc->config().clock_source() == ws::v1::CLOCK_SOURCE_DIN);
  // A valid but identical SetConfig: replied, same revision, nothing pushed.
  out = r.send(1, set_clock(7, ws::v1::CLOCK_SOURCE_DIN));
  CHECK(out.size() == 1 && out[0].msg.has_config_reply() && out[0].msg.config_reply().revision() == rev);
}

void test_not_durable_flag() {
  Rig r(/*inject_dir_sync_failure=*/true);
  r.connect(1);
  r.connect(2);
  (void)r.send(2, subscribe(1));
  const auto out = r.send(1, set_clock(2, ws::v1::CLOCK_SOURCE_USB));
  CHECK(out.size() == 2 && out[0].msg.config_reply().not_durable() && out[0].msg.config_reply().revision() == 1);
  CHECK(out[1].msg.config_reply().config().clock_source() == ws::v1::CLOCK_SOURCE_USB);
}

void test_persist_failure_pushes_nothing() {
  Rig r;
  r.so.inject_write_errno = ENOSPC;
  r.svc.reset();
  r.store.reset();
  r.store = std::make_unique<sd::ConfigStore>(r.so);
  r.svc = std::make_unique<sd::ConfigService>(*r.store);
  r.connect(1);
  r.connect(2);
  (void)r.send(2, subscribe(1));
  const auto out = r.send(1, set_clock(2, ws::v1::CLOCK_SOURCE_USB));
  CHECK(out.size() == 1 && is_error(out[0], ws::v1::ERROR_CODE_STORAGE_FULL));
  CHECK(r.svc->revision() == 0 && r.svc->config().clock_source() == ws::v1::CLOCK_SOURCE_INTERNAL);
}

// Two (here eight) concurrent writers: serialised, every change gets its own revision, strictly increasing, no gaps; the subscriber sees every revision once.
void test_concurrent_set_config() {
  Rig r;
  r.connect(100);
  std::vector<sd::Outbound> sub_pushes;
  {
    const auto out = r.send(100, subscribe(1));
    CHECK(out.size() == 2);
  }
  constexpr int kThreads = 8, kPer = 40;
  std::vector<std::vector<std::uint64_t>> replied(kThreads);
  std::vector<std::vector<std::uint64_t>> pushed(kThreads);
  for (int t = 0; t < kThreads; ++t) r.connect(static_cast<sd::ConnId>(t + 1));
  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&, t] {
      for (int k = 0; k < kPer; ++k) {
        const unsigned mx = 300 + static_cast<unsigned>(t * kPer + k);  // distinct: every call changes the config
        std::vector<sd::Outbound> out;
        r.svc->handle(static_cast<sd::ConnId>(t + 1), set_sustain_max(1000 + k, mx).SerializeAsString(), out);
        for (const sd::Outbound& o : out) {
          if (o.to == static_cast<sd::ConnId>(t + 1) && o.msg.has_config_reply() && o.msg.request_id() != 0) replied[t].push_back(o.msg.config_reply().revision());
          if (o.to == 100) pushed[t].push_back(o.msg.config_reply().revision());
        }
      }
    });
  }
  for (auto& th : ts) th.join();
  std::set<std::uint64_t> seen, seen_push;
  std::size_t n = 0, np = 0;
  for (int t = 0; t < kThreads; ++t) {
    CHECK(std::is_sorted(replied[t].begin(), replied[t].end()));  // one thread's own revisions increase
    for (std::uint64_t v : replied[t]) seen.insert(v), ++n;
    for (std::uint64_t v : pushed[t]) seen_push.insert(v), ++np;
  }
  CHECK(n == static_cast<std::size_t>(kThreads * kPer) && seen.size() == n);  // unique
  CHECK(*seen.begin() == 1 && *seen.rbegin() == n);                          // no gaps
  CHECK(np == n && seen_push == seen);                                        // the subscriber got every revision exactly once
  CHECK(r.svc->revision() == n);
}

// The persisted file and the served config agree after a series of changes, across a restart of the store (the service is rebuilt on the same directory).
void test_restart_serves_persisted() {
  Rig r;
  r.connect(1);
  (void)r.send(1, set_clock(2, ws::v1::CLOCK_SOURCE_USB));
  (void)r.send(1, set_sustain_max(3, 3000));
  r.svc.reset();
  r.store.reset();
  r.store = std::make_unique<sd::ConfigStore>(r.so);
  r.svc = std::make_unique<sd::ConfigService>(*r.store);
  r.connect(1);
  const auto out = r.send(1, get(4));
  CHECK(out.size() == 1 && out[0].msg.config_reply().revision() == 2 && out[0].msg.config_reply().config().clock_source() == ws::v1::CLOCK_SOURCE_USB);
  CHECK(out[0].msg.config_reply().config().sustain().max() == 3000);
}

}  // namespace

int main() {
  test_handshake();
  test_engine_commands_refused();
  test_get_set_subscribe();
  test_rejected_and_unchanged_push_nothing();
  test_not_durable_flag();
  test_persist_failure_pushes_nothing();
  test_concurrent_set_config();
  test_restart_serves_persisted();
  if (g_fail) {
    std::printf("sysd_service_tests: %d FAILED\n", g_fail);
    return 1;
  }
  std::printf("sysd_service_tests: ok\n");
  return 0;
}
