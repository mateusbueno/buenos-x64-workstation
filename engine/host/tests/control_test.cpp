// Story 2.1 control plane: dev config, ws.v1 over a real Unix domain socket for every matrix row,
// held note across a swap, concurrent clients, generation exhaustion and the stale/live socket rules.
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/fx.hpp"
#include "velora/host/control_plane.hpp"
#include "velora/host/control_server.hpp"
#include "velora/host/dev_config.hpp"
#include "velora/host/midi_ingest.hpp"
#include "velora/host/render_driver.hpp"

using namespace velora;
using namespace velora::host;
namespace p = core::params;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
#define CHECK_MSG(c, m) do { if (!(c)) { std::printf("FAIL %s:%d %s (%s)\n", __FILE__, __LINE__, #c, std::string(m).c_str()); ++g_fail; } } while (0)

static std::uint64_t steady_ns() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// --- a complete engine side (no audio device): renderer + driver + control plane -----------------------
struct Rig {
  std::unique_ptr<core::BlockRenderer> br = std::make_unique<core::BlockRenderer>();
  std::unique_ptr<HostEventQueue> midi_q = std::make_unique<HostEventQueue>();
  std::unique_ptr<HostEventQueue> ctl_q = std::make_unique<HostEventQueue>();
  RenderDriver drv{*br, *midi_q, {}, ctl_q.get()};
  MidiIngest ingest{*midi_q};
  // One synthetic host clock for MIDI stamps, control-thread stamps and block starts (ns).
  std::atomic<std::uint64_t> clock_ns{1'000'000'000ull};
  ControlPlane plane{br->snapshots(), br->last_values(), *ctl_q, [this] { return now(); }};
  float l[64], r[64];
  std::uint64_t now() const { return clock_ns.load(std::memory_order_relaxed); }

  bool load(const std::string& json) {
    ws::v1::State st;
    std::string err;
    if (!parse_dev_config(json, st, err)) { std::printf("parse: %s\n", err.c_str()); return false; }
    if (!plane.load_initial(st, err)) { std::printf("load: %s\n", err.c_str()); return false; }
    return true;
  }
  void step(int blocks = 1) {
    for (int i = 0; i < blocks; ++i) {
      const std::uint64_t t = clock_ns.fetch_add(1'333'333) + 1'333'333;
      drv.render_block(l, r, t);
    }
  }
  float live(std::uint16_t id, std::uint32_t part) {
    return br->last_values().load(p::perf_slot(*p::find(id), part));
  }
};

// --- UDS client ----------------------------------------------------------------------------------------
struct Client {
  int fd = -1;
  bool open(const std::string& path) {
    fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
    return fd >= 0 && ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
  }
  ~Client() { if (fd >= 0) ::close(fd); }
  bool write_all(const std::string& s) {
    std::size_t off = 0;
    while (off < s.size()) {
      const ssize_t n = ::send(fd, s.data() + off, s.size() - off, 0);
      if (n <= 0) return false;
      off += static_cast<std::size_t>(n);
    }
    return true;
  }
  bool read_exact(std::string& out, std::size_t n, int timeout_ms) {
    out.clear();
    while (out.size() < n) {
      pollfd pf{fd, POLLIN, 0};
      if (::poll(&pf, 1, timeout_ms) <= 0) return false;
      char buf[4096];
      const ssize_t k = ::recv(fd, buf, std::min(sizeof buf, n - out.size()), 0);
      if (k <= 0) return false;
      out.append(buf, static_cast<std::size_t>(k));
    }
    return true;
  }
  static std::string frame(const std::string& body) {
    const std::uint32_t n = static_cast<std::uint32_t>(body.size());
    const char h[4] = {char(n >> 24), char(n >> 16), char(n >> 8), char(n)};
    return std::string(h, 4) + body;
  }
  bool send(const ws::v1::ClientMessage& m) {
    std::string b;
    m.SerializeToString(&b);
    return write_all(frame(b));
  }
  bool recv(ws::v1::ServerMessage& m, int timeout_ms = 3000) {
    std::string h, b;
    if (!read_exact(h, 4, timeout_ms)) return false;
    const std::uint32_t n = (std::uint8_t(h[0]) << 24) | (std::uint8_t(h[1]) << 16) | (std::uint8_t(h[2]) << 8) | std::uint8_t(h[3]);
    return read_exact(b, n, timeout_ms) && m.ParseFromString(b);
  }
  bool call(const ws::v1::ClientMessage& m, ws::v1::ServerMessage& reply) {
    if (!send(m)) return false;
    for (;;) {  // skip pushed State messages (request_id 0)
      if (!recv(reply)) return false;
      if (reply.request_id() == m.request_id() || reply.request_id() != 0) return true;
      pushed.push_back(reply);
    }
  }
  bool closed_by_peer(int timeout_ms = 2000) {
    pollfd pf{fd, POLLIN, 0};
    if (::poll(&pf, 1, timeout_ms) <= 0) return false;
    char c;
    return ::recv(fd, &c, 1, 0) == 0;
  }
  std::vector<ws::v1::ServerMessage> pushed;
};

static std::uint64_t g_rid = 100;
static ws::v1::ClientMessage hello(std::uint32_t version = 1) {
  ws::v1::ClientMessage m;
  m.set_request_id(++g_rid);
  m.mutable_hello()->set_version(version);
  m.mutable_hello()->set_client_name("control_test");
  m.mutable_hello()->add_features("params.v1");
  m.mutable_hello()->add_features("from-the-future");
  return m;
}
static ws::v1::ClientMessage perf(ws::v1::ParamId id, std::uint32_t part, float v) {
  ws::v1::ClientMessage m;
  m.set_request_id(++g_rid);
  auto* c = m.mutable_set_performance_param();
  c->set_param_id(id); c->set_part(part); c->set_value(v);
  return m;
}
static ws::v1::ClientMessage chain(std::uint32_t part, std::initializer_list<ws::v1::InsertType> types) {
  ws::v1::ClientMessage m;
  m.set_request_id(++g_rid);
  auto* c = m.mutable_set_insert_chain();
  c->set_part(part);
  for (auto t : types) { auto* s = c->add_slots(); s->set_type(t); s->add_params(0.25f); }
  return m;
}
static ws::v1::ClientMessage get_state() {
  ws::v1::ClientMessage m;
  m.set_request_id(++g_rid);
  m.mutable_get_state();
  return m;
}
static ws::v1::ClientMessage get_live() {
  ws::v1::ClientMessage m;
  m.set_request_id(++g_rid);
  m.mutable_get_performance_values();
  return m;
}
static bool is_err(const ws::v1::ServerMessage& r, ws::v1::ErrorCode c) {
  return r.has_error() && r.error().code() == c;
}

struct Live {
  Rig rig;
  std::unique_ptr<ControlServer> server;
  std::string path;
  explicit Live(const char* tag) {
    path = "/tmp/velora-ctl-" + std::string(tag) + "-" + std::to_string(::getpid()) + ".sock";
    std::string err;
    CHECK(rig.load("{}"));
    server = std::make_unique<ControlServer>(rig.plane, path);
    CHECK_MSG(server->start(err), err);
  }
  bool connect(Client& c) {
    ws::v1::ServerMessage ack;
    return c.open(path) && c.call(hello(), ack) && ack.has_hello_ack();
  }
};

template <typename F>
static bool wait_until(F&& cond, int timeout_ms = 3000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!cond()) {
    if (std::chrono::steady_clock::now() > end) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return true;
}

static std::string slurp(const char* path) {
  std::string s;
  if (FILE* f = std::fopen(path, "rb")) {
    char b[4096];
    std::size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    std::fclose(f);
  }
  return s;
}

// ------------------------------------------------------------------------------------------------------
static void test_dev_config() {
  Rig rig;
  ws::v1::State st;
  std::string err;
  CHECK_MSG(load_dev_config(VELORA_DEV_CONFIG, st, err), err);
  CHECK_MSG(rig.plane.load_initial(st, err), err);
  CHECK(rig.plane.revision() == 1);
  CHECK(rig.br->snapshots().current() != nullptr && rig.br->snapshots().current()->revision == 1);
  ws::v1::State out;
  rig.plane.fill_state(&out);
  CHECK(out.parts_size() == 8 && out.parts(0).inserts_size() == 2 && out.parts(0).inserts(0).type() == ws::v1::INSERT_TYPE_ROTARY);
  CHECK(out.parts(1).inserts(1).bypass() && out.parts(2).output() == ws::v1::PART_OUTPUT_AUX);
  CHECK(std::fabs(out.parts(0).send_reverb() - 0.25f) < 1e-6f);
  // published before audio runs: the first block adopts it and applies the initial performance values
  rig.step();
  CHECK(rig.live(p::kDrawbar16, 0) == 8.0f && rig.live(p::kDrawbar4, 0) == 6.0f && rig.live(p::kRotarySpeed, 0) == 1.0f);
  CHECK(rig.live(p::kDrawbar8, 3) == 8.0f);  // registry default elsewhere
  CHECK(rig.br->held_snapshot() == rig.br->snapshots().current());
  // a second initial load is refused
  CHECK(!rig.plane.load_initial(st, err));

  // invalid dev files: the engine must refuse to start with a clear error
  auto bad = [&](const char* json, const char* needle) {
    Rig r2;
    ws::v1::State s2;
    std::string e;
    const bool ok = parse_dev_config(json, s2, e) && r2.plane.load_initial(s2, e);
    CHECK_MSG(!ok, json);
    CHECK_MSG(e.find(needle) != std::string::npos, e + " / wanted " + needle);
    CHECK(r2.br->snapshots().current() == nullptr);  // nothing published
  };
  bad(R"({"parts":[{"part":9}]})", "part index");
  bad(R"({"parts":[{"part":0},{"part":0}]})", "twice");
  bad(R"({"parts":[{"part":0,"sendReverb":3.0}]})", "range");
  bad(R"({"parts":[{"part":0,"output":"PART_OUTPUT_MAIN","inserts":[{},{},{},{},{},{}]}]})", "insert slots");
  bad(R"({"master":[{"paramId":"PARAM_DRAWBAR_8","value":1}]})", "performance");
  bad(R"({"performanceInit":[{"paramId":"PARAM_INSERT_TYPE","value":1}]})", "structural");
  bad(R"({"performanceInit":[{"paramId":"PARAM_DRAWBAR_8","part":0,"value":12}]})", "range");
  bad(R"({"performanceInit":[{"paramId":"PARAM_DRAWBAR_8","part":1,"value":3},{"paramId":"PARAM_DRAWBAR_8","part":1,"value":4}]})", "twice");
  bad(R"({"performanceInit":[{"paramId":"PARAM_DRAWBAR_8","value":3}]})", "explicit part");
  bad(R"({"master":[{"paramId":"PARAM_MASTER_EQ_LOW_GAIN","value":1},{"paramId":"PARAM_MASTER_EQ_LOW_GAIN","value":2}]})", "twice");
  bad(R"({"nonsense":1})", "nonsense");
  bad(R"({not json)", "");
  ws::v1::State s3;
  std::string e3;
  CHECK(!load_dev_config("/nonexistent/engine-dev.json", s3, e3) && e3.find("/nonexistent") != std::string::npos);
}

static void test_handshake() {
  Live L("hs");
  {  // unsupported version
    Client c;
    CHECK(c.open(L.path));
    ws::v1::ServerMessage r;
    CHECK(c.call(hello(99), r));
    CHECK(is_err(r, ws::v1::ERROR_CODE_UNSUPPORTED_VERSION) && r.error().details().at("supported") == "1");
    CHECK(c.closed_by_peer());
  }
  {  // a command before Hello
    Client c;
    CHECK(c.open(L.path));
    ws::v1::ServerMessage r;
    CHECK(c.call(get_state(), r));
    CHECK(is_err(r, ws::v1::ERROR_CODE_HANDSHAKE_REQUIRED));
    CHECK(c.closed_by_peer());
  }
  {  // good handshake: version + features, unknown client features ignored; a second Hello is refused
    Client c;
    CHECK(c.open(L.path));
    ws::v1::ServerMessage r;
    CHECK(c.call(hello(), r) && r.has_hello_ack() && r.hello_ack().version() == 1 && r.hello_ack().revision() == 1);
    bool has_params = false;
    for (const auto& f : r.hello_ack().features()) has_params |= f == "params.v1";
    CHECK(has_params);
    CHECK(c.call(hello(), r) && is_err(r, ws::v1::ERROR_CODE_INVALID_COMMAND));
    CHECK(c.call(get_state(), r) && r.has_state());  // connection survived
  }
}

static void test_structural_commands() {
  Live L("st");
  Client c, sub;
  CHECK(L.connect(c) && L.connect(sub));
  ws::v1::ServerMessage r;
  ws::v1::ClientMessage subscribe;
  subscribe.set_request_id(++g_rid);
  subscribe.mutable_subscribe()->set_enable(true);
  CHECK(sub.call(subscribe, r) && r.has_ack());
  ws::v1::ServerMessage push;
  CHECK(sub.recv(push) && push.request_id() == 0 && push.state().revision() == 1);  // current state right away

  const core::Snapshot* before = L.rig.br->snapshots().current();
  const std::uint64_t before_rev = before->revision;
  auto cmd = chain(2, {ws::v1::INSERT_TYPE_ROTARY, ws::v1::INSERT_TYPE_EQ});
  CHECK(c.call(cmd, r));
  CHECK(r.request_id() == cmd.request_id() && r.has_ack() && r.revision() == 2);
  const core::Snapshot* after = L.rig.br->snapshots().current();
  CHECK(after != before && before_rev == 1 && after->revision == 2 && after->parts[2].insert_count == 2);
  CHECK(sub.recv(push) && push.request_id() == 0 && push.has_state() && push.state().revision() == 2);
  CHECK(push.state().parts(2).inserts_size() == 2);

  // send levels
  ws::v1::ClientMessage m;
  m.set_request_id(++g_rid);
  m.mutable_set_send_levels()->set_part(1);
  m.mutable_set_send_levels()->set_reverb(0.6f);
  m.mutable_set_send_levels()->set_delay(0.3f);
  CHECK(c.call(m, r) && r.has_ack() && r.revision() == 3);
  // part output, timbre ref, master params
  m.Clear(); m.set_request_id(++g_rid);
  m.mutable_set_part_output()->set_part(3);
  m.mutable_set_part_output()->set_output(ws::v1::PART_OUTPUT_AUX);
  CHECK(c.call(m, r) && r.has_ack() && r.revision() == 4);
  m.Clear(); m.set_request_id(++g_rid);
  m.mutable_set_timbre_ref()->set_part(4);
  m.mutable_set_timbre_ref()->set_timbre_ref(4000000000u);
  CHECK(c.call(m, r) && r.has_ack() && r.revision() == 5);
  m.Clear(); m.set_request_id(++g_rid);
  auto* mp = m.mutable_set_master_params();
  auto* v = mp->add_values(); v->set_param_id(ws::v1::PARAM_MASTER_EQ_HIGH_GAIN); v->set_value(-3.5f);
  v = mp->add_values(); v->set_param_id(ws::v1::PARAM_MASTER_COMP_RATIO); v->set_value(4);
  CHECK(c.call(m, r) && r.has_ack() && r.revision() == 6);

  CHECK(c.call(get_state(), r) && r.has_state() && r.revision() == 6);
  const auto& st = r.state();
  CHECK(std::fabs(st.parts(1).send_reverb() - 0.6f) < 1e-6f && std::fabs(st.parts(1).send_delay() - 0.3f) < 1e-6f);
  CHECK(st.parts(3).output() == ws::v1::PART_OUTPUT_AUX && st.parts(4).timbre_ref() == 4000000000u);
  bool found = false;
  for (const auto& mv : st.master())
    if (mv.param_id() == ws::v1::PARAM_MASTER_EQ_HIGH_GAIN) found = std::fabs(mv.value() + 3.5f) < 1e-6f;
  CHECK(found);

  // RT adopts the latest generation at the next block; initial send levels become the live values
  L.rig.step();
  CHECK(L.rig.br->held_snapshot()->revision == 6);
  CHECK(std::fabs(L.rig.live(p::kSendReverbLevel, 1) - 0.6f) < 1e-6f);
  // every pushed state is in order
  std::uint64_t last = 2;
  int pushes = 1;
  while (pushes < 5) {
    CHECK(sub.recv(push) && push.state().revision() == last + 1);
    last = push.state().revision();
    ++pushes;
  }
}

static void test_invalid_commands() {
  Live L("bad");
  Client c;
  CHECK(L.connect(c));
  ws::v1::ServerMessage r;
  const std::uint64_t rev0 = L.rig.plane.revision();
  auto expect = [&](const ws::v1::ClientMessage& m, ws::v1::ErrorCode code, const char* what) {
    CHECK_MSG(c.call(m, r), what);
    CHECK_MSG(is_err(r, code), std::string(what) + " got " + std::to_string(r.has_error() ? r.error().code() : -1));
    CHECK_MSG(r.request_id() == m.request_id() && r.revision() == rev0, what);
    CHECK(!r.error().message().empty());
  };
  // unknown param id
  expect(perf(static_cast<ws::v1::ParamId>(9999), 0, 1), ws::v1::ERROR_CODE_UNKNOWN_PARAM, "unknown id");
  expect(perf(ws::v1::PARAM_ID_UNSPECIFIED, 0, 1), ws::v1::ERROR_CODE_UNKNOWN_PARAM, "unspecified id");
  expect(perf(static_cast<ws::v1::ParamId>(99), 0, 1), ws::v1::ERROR_CODE_UNKNOWN_PARAM, "reserved id");
  // out of range
  expect(perf(ws::v1::PARAM_DRAWBAR_8, 0, 9), ws::v1::ERROR_CODE_OUT_OF_RANGE, "drawbar 9");
  expect(perf(ws::v1::PARAM_DRAWBAR_8, 0, -1), ws::v1::ERROR_CODE_OUT_OF_RANGE, "drawbar -1");
  expect(perf(ws::v1::PARAM_DRAWBAR_8, 0, 2.5f), ws::v1::ERROR_CODE_OUT_OF_RANGE, "fractional step");
  expect(perf(ws::v1::PARAM_KEY_CLICK, 0, std::nanf("")), ws::v1::ERROR_CODE_OUT_OF_RANGE, "nan");
  expect(perf(ws::v1::PARAM_KEY_CLICK, 0, INFINITY), ws::v1::ERROR_CODE_OUT_OF_RANGE, "inf");
  // part index above 7
  expect(perf(ws::v1::PARAM_DRAWBAR_8, 8, 1), ws::v1::ERROR_CODE_INVALID_PART, "perf part 8");
  expect(chain(8, {ws::v1::INSERT_TYPE_EQ}), ws::v1::ERROR_CODE_INVALID_PART, "chain part 8");
  {
    ws::v1::ClientMessage m; m.set_request_id(++g_rid);
    m.mutable_set_send_levels()->set_part(200);
    expect(m, ws::v1::ERROR_CODE_INVALID_PART, "send part 200");
    m.Clear(); m.set_request_id(++g_rid);
    m.mutable_set_part_output()->set_part(8);
    expect(m, ws::v1::ERROR_CODE_INVALID_PART, "output part 8");
    m.Clear(); m.set_request_id(++g_rid);
    m.mutable_set_timbre_ref()->set_part(9);
    expect(m, ws::v1::ERROR_CODE_INVALID_PART, "timbre part 9");
    // send level out of range
    m.Clear(); m.set_request_id(++g_rid);
    m.mutable_set_send_levels()->set_part(0);
    m.mutable_set_send_levels()->set_reverb(1.5f);
    expect(m, ws::v1::ERROR_CODE_OUT_OF_RANGE, "send 1.5");
    // bad enum
    m.Clear(); m.set_request_id(++g_rid);
    m.mutable_set_part_output()->set_part(0);
    m.mutable_set_part_output()->set_output(static_cast<ws::v1::PartOutput>(7));
    expect(m, ws::v1::ERROR_CODE_UNKNOWN_ENUM_VALUE, "output 7");
    // wrong class: a performance param sent as structural, and the reverse
    m.Clear(); m.set_request_id(++g_rid);
    auto* v = m.mutable_set_master_params()->add_values();
    v->set_param_id(ws::v1::PARAM_DRAWBAR_8); v->set_value(3);
    expect(m, ws::v1::ERROR_CODE_WRONG_CLASS, "performance param as structural");
    expect(perf(ws::v1::PARAM_MASTER_EQ_LOW_GAIN, 0, 1), ws::v1::ERROR_CODE_WRONG_CLASS, "structural as performance");
    expect(perf(ws::v1::PARAM_INSERT_TYPE, 0, 1), ws::v1::ERROR_CODE_WRONG_CLASS, "insert type as performance");
    // wrong scope
    m.Clear(); m.set_request_id(++g_rid);
    v = m.mutable_set_master_params()->add_values();
    v->set_param_id(ws::v1::PARAM_PART_OUTPUT); v->set_value(1);
    expect(m, ws::v1::ERROR_CODE_WRONG_SCOPE, "part param in master params");
    // master params: all or nothing; empty is invalid; out of range
    m.Clear(); m.set_request_id(++g_rid);
    auto* mp = m.mutable_set_master_params();
    v = mp->add_values(); v->set_param_id(ws::v1::PARAM_MASTER_EQ_LOW_GAIN); v->set_value(5);
    v = mp->add_values(); v->set_param_id(ws::v1::PARAM_MASTER_COMP_RATIO); v->set_value(99);
    expect(m, ws::v1::ERROR_CODE_OUT_OF_RANGE, "second master value out of range");
    m.Clear(); m.set_request_id(++g_rid);
    m.mutable_set_master_params();
    expect(m, ws::v1::ERROR_CODE_INVALID_COMMAND, "empty master params");
    // no command at all
    m.Clear(); m.set_request_id(++g_rid);
    expect(m, ws::v1::ERROR_CODE_INVALID_COMMAND, "empty command");
  }
  {  // too many slots, bad slot type, too many insert params, slot param out of range
    expect(chain(0, {ws::v1::INSERT_TYPE_EQ, ws::v1::INSERT_TYPE_EQ, ws::v1::INSERT_TYPE_EQ, ws::v1::INSERT_TYPE_EQ,
                     ws::v1::INSERT_TYPE_EQ, ws::v1::INSERT_TYPE_EQ}),
           ws::v1::ERROR_CODE_INVALID_SLOT, "6 slots");
    auto m = chain(0, {ws::v1::INSERT_TYPE_EQ});
    m.mutable_set_insert_chain()->mutable_slots(0)->set_type(static_cast<ws::v1::InsertType>(42));
    expect(m, ws::v1::ERROR_CODE_UNKNOWN_ENUM_VALUE, "insert type 42");
    m = chain(0, {ws::v1::INSERT_TYPE_EQ});
    for (int i = 0; i < 4; ++i) m.mutable_set_insert_chain()->mutable_slots(0)->add_params(0.5f);
    expect(m, ws::v1::ERROR_CODE_INVALID_SLOT, "5 insert params");
    m = chain(0, {ws::v1::INSERT_TYPE_EQ});
    m.mutable_set_insert_chain()->mutable_slots(0)->set_params(0, 2.0f);
    expect(m, ws::v1::ERROR_CODE_OUT_OF_RANGE, "insert param 2.0");
  }
  // a Hello is not a command, once connected
  {
    auto m = hello();
    expect(m, ws::v1::ERROR_CODE_INVALID_COMMAND, "hello after hello");
  }
  // malformed protobuf: error, connection survives
  {
    CHECK(c.write_all(Client::frame(std::string("\xff\xff\xff\xff\x07garbage", 12))));
    CHECK(c.recv(r) && is_err(r, ws::v1::ERROR_CODE_MALFORMED_MESSAGE) && r.revision() == rev0);
    CHECK(c.call(get_state(), r) && r.has_state() && r.state().revision() == rev0);
  }
  // an empty frame is an empty message: invalid command, connection survives
  {
    CHECK(c.write_all(Client::frame("")));
    CHECK(c.recv(r) && is_err(r, ws::v1::ERROR_CODE_INVALID_COMMAND));
  }
  CHECK(L.rig.plane.revision() == rev0);
  CHECK(L.rig.br->snapshots().current()->revision == rev0);
  CHECK_MSG(L.rig.plane.commands_rejected() >= 25, std::to_string(L.rig.plane.commands_rejected()));
  CHECK(L.rig.ctl_q->pop(*std::make_unique<StampedUmp>()) == false);  // nothing leaked to the RT queue
  // oversized frame: framing violation, error then close
  {
    Client big;
    CHECK(L.connect(big));
    const std::uint32_t n = 1u << 20;
    const char h[4] = {char(n >> 24), char(n >> 16), char(n >> 8), char(n)};
    CHECK(big.write_all(std::string(h, 4)));
    ws::v1::ServerMessage e;
    CHECK(big.recv(e) && is_err(e, ws::v1::ERROR_CODE_FRAME_TOO_LARGE));
    CHECK(big.closed_by_peer());
  }
  CHECK(L.rig.plane.revision() == rev0);
  // other clients are unaffected
  CHECK(c.call(chain(0, {ws::v1::INSERT_TYPE_EQ}), r) && r.has_ack() && r.revision() == rev0 + 1);
}

static void test_performance_path() {
  Live L("perf");
  Client c;
  CHECK(L.connect(c));
  ws::v1::ServerMessage r;
  const auto rev0 = L.rig.plane.revision();
  // drawbar 3 (8 ft) of part 4 (index 3) via ws.v1
  CHECK(c.call(perf(ws::v1::PARAM_DRAWBAR_8, 3, 5), r) && r.has_ack() && r.revision() == rev0);
  CHECK(L.rig.plane.revision() == rev0 && L.rig.br->snapshots().current()->revision == rev0);  // no new generation
  CHECK(L.rig.live(p::kDrawbar8, 3) == 8.0f);  // RT has not run a block yet: still the default
  // it is one timestamped UMP on the control->RT queue, applied by RT at its frame
  L.rig.step(3);
  CHECK(L.rig.live(p::kDrawbar8, 3) == 5.0f && L.rig.live(p::kDrawbar8, 2) == 8.0f);
  CHECK(L.rig.br->param_events_applied() == 1);
  // the control thread reads it back from the last-value table (no locks, no round trip through RT)
  CHECK(c.call(get_live(), r) && r.has_performance_values());
  bool seen = false;
  for (const auto& pv : r.performance_values().values())
    if (pv.param_id() == ws::v1::PARAM_DRAWBAR_8 && pv.part() == 3) seen = pv.value() == 5.0f;
  CHECK(seen);
  // enum parameter (rotary speed) and a float
  CHECK(c.call(perf(ws::v1::PARAM_ROTARY_SPEED, 0, 2), r) && r.has_ack());
  CHECK(c.call(perf(ws::v1::PARAM_SEND_DELAY_LEVEL, 7, 0.5f), r) && r.has_ack());
  L.rig.step(3);
  CHECK(L.rig.live(p::kRotarySpeed, 0) == 2.0f && std::fabs(L.rig.live(p::kSendDelayLevel, 7) - 0.5f) < 1e-6f);

  // the same parameter via MIDI CC (CC 72 = drawbar 8', channel 3 = part 4), as MIDI 1.0 and MIDI 2.0 UMP
  const std::uint32_t cc1[] = {0x2000'0000u | (0xBu << 20) | (3u << 16) | (72u << 8) | 127u};
  L.rig.ingest.on_ump(cc1, 1, L.rig.now());
  L.rig.step(2);
  CHECK(L.rig.live(p::kDrawbar8, 3) == 8.0f);
  const std::uint32_t cc2[] = {0x4000'0000u | (0xBu << 20) | (3u << 16) | (72u << 8), 0x2000'0000u};  // 1/8 of range
  L.rig.ingest.on_ump(cc2, 2, L.rig.now());
  L.rig.step(2);
  CHECK(L.rig.live(p::kDrawbar8, 3) == 1.0f);
  const std::uint32_t unmapped[] = {0x2000'0000u | (0xBu << 20) | (3u << 16) | (20u << 8) | 127u};
  const auto ign = L.rig.ingest.counters().ignored.load();
  L.rig.ingest.on_ump(unmapped, 1, L.rig.now());
  CHECK(L.rig.ingest.counters().ignored.load() == ign + 1);
  CHECK(L.rig.ingest.counters().params.load() == 2);
  CHECK(L.rig.plane.revision() == rev0);
  // CoreMIDI-style CC 64 (sustain) is not a registry parameter: it still reaches the arbiter's pedal state
  const std::uint32_t pedal_down[] = {0x2000'0000u | (0xBu << 20) | (0u << 16) | (64u << 8) | 127u};
  const std::uint32_t pedal_up[] = {0x2000'0000u | (0xBu << 20) | (0u << 16) | (64u << 8)};
  L.rig.ingest.on_ump(pedal_down, 1, L.rig.now());
  L.rig.step(3);
  CHECK(L.rig.br->arbiter().sustain_down(0));
  L.rig.ingest.on_ump(pedal_up, 1, L.rig.now());
  L.rig.step(3);
  CHECK(!L.rig.br->arbiter().sustain_down(0));
}

static void test_held_note_across_swap() {
  Live L("held");
  Client c;
  CHECK(L.connect(c));
  ws::v1::ServerMessage r;
  L.rig.step();
  // a note sounds on generation 1
  const std::uint32_t on[] = {0x2000'0000u | (0x9u << 20) | (0u << 16) | (60u << 8) | 100u};
  L.rig.ingest.on_ump(on, 1, L.rig.now());
  L.rig.step(3);
  CHECK(L.rig.br->snapshots().refs(0) == 1);
  const core::Snapshot* gen1 = L.rig.br->snapshots().current();
  // swap while it is held
  CHECK(c.call(chain(0, {ws::v1::INSERT_TYPE_CHORUS_PHASER}), r) && r.has_ack() && r.revision() == 2);
  L.rig.step(3);
  CHECK(L.rig.br->held_snapshot()->revision == 2);
  // the control thread's reclaim pass runs every poll tick: give it time, then check generation 1 is still there
  CHECK(!wait_until([&] { return L.rig.plane.freed_generations() != 0; }, 150));
  CHECK(L.rig.plane.live_generations() == 2 && L.rig.plane.freed_generations() == 0);
  CHECK(gen1->magic == core::Snapshot::kMagic && gen1->revision == 1);  // still readable: not freed
  // more swaps with the note still held: generation 1 stays, the in-between ones go
  for (int i = 0; i < 3; ++i) {
    CHECK(c.call(chain(0, {ws::v1::INSERT_TYPE_EQ}), r) && r.has_ack());
    L.rig.step(2);
  }
  CHECK(wait_until([&] { return L.rig.plane.freed_generations() == 3; }));
  CHECK(L.rig.plane.live_generations() == 2);  // generation 1 (held note) + current
  // release the note: the voice leaves, RT reports no references, generation 1 is freed by the control thread
  const std::uint32_t off[] = {0x2000'0000u | (0x8u << 20) | (0u << 16) | (60u << 8)};
  L.rig.ingest.on_ump(off, 1, L.rig.now());
  L.rig.step(4);
  CHECK(L.rig.br->snapshots().refs(0) == 0);
  CHECK(wait_until([&] { return L.rig.plane.live_generations() == 1; }));
  CHECK(L.rig.plane.live_generations() == 1 && L.rig.plane.freed_generations() == 4);
}

// --- Story 2.7: effect instances travel with the generation, the FX budget refuses, lifetimes ------------------------------------
static ws::v1::ClientMessage chain_slots(std::uint32_t part, std::initializer_list<std::pair<ws::v1::InsertType, bool>> slots) {
  ws::v1::ClientMessage m;
  m.set_request_id(++g_rid);
  auto* c = m.mutable_set_insert_chain();
  c->set_part(part);
  for (const auto& sl : slots) {
    auto* s = c->add_slots();
    s->set_type(sl.first);
    s->set_bypass(sl.second);
  }
  return m;
}

static void test_fx_chain_and_budget() {
  Rig rig;
  CHECK(rig.load("{}"));
  const std::int64_t base = core::Effect::live_instances();
  CHECK(rig.plane.fx_cost() == core::kFixedFxCost && rig.plane.fx_budget() == core::kMaxFxCost);
  bool changed = false;
  ws::v1::ServerMessage r;

  // A chain command builds its effect instances on the control thread and publishes them with the generation.
  r = rig.plane.handle(chain_slots(1, {{ws::v1::INSERT_TYPE_EQ, false}, {ws::v1::INSERT_TYPE_COMPRESSOR, true}, {ws::v1::INSERT_TYPE_NONE, false}}), changed);
  CHECK(changed && r.has_ack() && r.revision() == 2);
  const core::Snapshot* s2 = rig.br->snapshots().current();
  CHECK(s2->parts[1].chain && s2->parts[1].chain->slot_count() == 3);
  CHECK(s2->parts[1].chain->slot(0).effect->type() == core::EffectType::kEq && !s2->parts[1].chain->slot(0).bypass);
  CHECK(s2->parts[1].chain->slot(1).effect->type() == core::EffectType::kCompressor && s2->parts[1].chain->slot(1).bypass);
  CHECK(s2->parts[1].chain->slot(2).effect == nullptr);  // type none: an empty slot
  CHECK(core::Effect::live_instances() == base + 2);
  CHECK(rig.plane.fx_cost() == core::kFixedFxCost + core::effect_cost_units(core::EffectType::kEq) + core::effect_cost_units(core::EffectType::kCompressor));

  // Commands that do not touch the chain, or repeat it, keep the very same instances (RT would not cross-fade).
  const core::FxChain* c1 = s2->parts[1].chain.get();
  r = rig.plane.handle(chain_slots(1, {{ws::v1::INSERT_TYPE_EQ, false}, {ws::v1::INSERT_TYPE_COMPRESSOR, true}, {ws::v1::INSERT_TYPE_NONE, false}}), changed);
  CHECK(changed && rig.br->snapshots().current()->parts[1].chain.get() == c1);
  ws::v1::ClientMessage out;
  out.set_request_id(++g_rid);
  out.mutable_set_part_output()->set_part(1);
  out.mutable_set_part_output()->set_output(ws::v1::PART_OUTPUT_AUX);
  r = rig.plane.handle(out, changed);
  CHECK(changed && r.has_ack() && rig.br->snapshots().current()->parts[1].chain.get() == c1 &&
        rig.br->snapshots().current()->parts[1].output == p::kPartOutputAux);
  // A changed chain gets new instances (a bypass toggle included).
  r = rig.plane.handle(chain_slots(1, {{ws::v1::INSERT_TYPE_EQ, false}, {ws::v1::INSERT_TYPE_COMPRESSOR, false}}), changed);
  CHECK(changed && rig.br->snapshots().current()->parts[1].chain.get() != c1);
  // Other parts' chains are untouched by it.
  CHECK(rig.br->snapshots().current()->parts[0].chain == nullptr);

  // The FX budget: exactly the current cost still fits; anything above is refused with the typed error and nothing changes.
  rig.plane.set_fx_budget(rig.plane.fx_cost());
  const std::uint64_t rev = rig.plane.revision();
  const core::Snapshot* before = rig.br->snapshots().current();
  const std::int64_t live_before = core::Effect::live_instances();
  ws::v1::State st_before;
  rig.plane.fill_state(&st_before);
  r = rig.plane.handle(chain_slots(2, {{ws::v1::INSERT_TYPE_ROTARY, false}}), changed);
  CHECK(!changed && is_err(r, ws::v1::ERROR_CODE_FX_BUDGET_EXCEEDED) && r.revision() == rev);
  CHECK(r.error().details().at("fx_budget") == std::to_string(rig.plane.fx_budget()));
  CHECK(rig.plane.revision() == rev && rig.br->snapshots().current() == before);
  CHECK(core::Effect::live_instances() == live_before);  // the instances built for the refused command are gone
  ws::v1::State st_after;
  rig.plane.fill_state(&st_after);
  CHECK(st_before.SerializeAsString() == st_after.SerializeAsString());
  // a replacement of equal or lower cost fits (two EQ instances cost less than EQ + compressor)
  r = rig.plane.handle(chain_slots(1, {{ws::v1::INSERT_TYPE_EQ, false}, {ws::v1::INSERT_TYPE_EQ, false}}), changed);
  CHECK(changed && r.has_ack());
  // removing effects always fits; then the heavier one does too
  r = rig.plane.handle(chain_slots(1, {}), changed);
  CHECK(changed && r.has_ack() && rig.plane.fx_cost() == core::kFixedFxCost);
  rig.plane.set_fx_budget(core::kFixedFxCost + core::effect_cost_units(core::EffectType::kRotary));
  r = rig.plane.handle(chain_slots(2, {{ws::v1::INSERT_TYPE_ROTARY, false}}), changed);
  CHECK(changed && r.has_ack());
  r = rig.plane.handle(chain_slots(3, {{ws::v1::INSERT_TYPE_CHORUS_PHASER, false}}), changed);
  CHECK(!changed && is_err(r, ws::v1::ERROR_CODE_FX_BUDGET_EXCEEDED));
  // refusals are counted like every other rejected command
  CHECK(rig.plane.commands_rejected() >= 2);

  // The budget also applies to the initial configuration: the engine refuses to start on a dev file that does not fit.
  Rig r2;
  r2.plane.set_fx_budget(core::kFixedFxCost + 1);
  ws::v1::State st;
  std::string err;
  CHECK_MSG(load_dev_config(VELORA_DEV_CONFIG, st, err), err);
  CHECK(!r2.plane.load_initial(st, err) && err.find("FX budget") != std::string::npos);
  CHECK(r2.plane.revision() == 0 && r2.br->snapshots().current() == nullptr);
}

// The effect instances live exactly as long as the generations that hold them: a held note keeps the old generation (and its
// instances); the cross-fade keeps the generation RT still processes through.
static void test_effect_lifetime_across_generations() {
  Rig rig;
  CHECK(rig.load("{}"));
  const std::int64_t base = core::Effect::live_instances();
  bool changed = false;
  CHECK(rig.plane.handle(chain_slots(0, {{ws::v1::INSERT_TYPE_EQ, false}}), changed).has_ack());  // generation 2
  rig.step(2);
  const std::uint32_t gen2_ledger = rig.br->held_snapshot()->ledger_index;
  const std::uint32_t on[] = {0x2000'0000u | (0x9u << 20) | (0u << 16) | (60u << 8) | 100u};
  rig.ingest.on_ump(on, 1, rig.now());
  rig.step(3);  // the note sounds on generation 2
  CHECK(rig.br->snapshots().refs(gen2_ledger) == 2);  // the note and the chain entry
  CHECK(core::Effect::live_instances() == base + 1);

  CHECK(rig.plane.handle(chain_slots(0, {{ws::v1::INSERT_TYPE_COMPRESSOR, false}}), changed).has_ack());  // generation 3
  rig.step(1);
  rig.plane.reclaim();
  CHECK(core::Effect::live_instances() == base + 2);  // the cross-fade is running: the old chain is still in use
  rig.step(8);
  rig.plane.reclaim();
  CHECK(core::Effect::live_instances() == base + 2);  // fade over, but the held note still references generation 2
  CHECK(rig.br->snapshots().refs(gen2_ledger) == 1);

  CHECK(rig.plane.handle(chain_slots(0, {{ws::v1::INSERT_TYPE_EQ, false}, {ws::v1::INSERT_TYPE_EQ, false}}), changed).has_ack());  // generation 4
  rig.step(10);
  rig.plane.reclaim();
  CHECK(core::Effect::live_instances() == base + 3);  // generation 2 (held note) + generation 4's two; generation 3 is gone
  const std::uint32_t off[] = {0x2000'0000u | (0x8u << 20) | (0u << 16) | (60u << 8)};
  rig.ingest.on_ump(off, 1, rig.now());
  rig.step(12);
  rig.plane.reclaim();
  CHECK(core::Effect::live_instances() == base + 2);  // generation 2 freed with its instance, generation 3's chain was skipped/freed
  CHECK(rig.plane.live_generations() == 1);
}

// --- Story 2.7 follow-ups: params compare, budget lowering, master level and line-in through the control plane, bus driver ------
static ws::v1::ClientMessage chain_one(std::uint32_t part, ws::v1::InsertType t, float p1) {
  ws::v1::ClientMessage m;
  m.set_request_id(++g_rid);
  auto* c = m.mutable_set_insert_chain();
  c->set_part(part);
  auto* sl = c->add_slots();
  sl->set_type(t);
  sl->add_params(p1);
  return m;
}

static void test_chain_identity_and_budget_lowering() {
  Rig rig;
  CHECK(rig.load("{}"));
  bool changed = false;
  CHECK(rig.plane.handle(chain_one(0, ws::v1::INSERT_TYPE_EQ, 0.25f), changed).has_ack());
  const core::FxChain* c1 = rig.br->snapshots().current()->parts[0].chain.get();
  CHECK(rig.plane.handle(chain_one(0, ws::v1::INSERT_TYPE_EQ, 0.25f), changed).has_ack() &&
        rig.br->snapshots().current()->parts[0].chain.get() == c1);  // identical: same instances
  CHECK(rig.plane.handle(chain_one(0, ws::v1::INSERT_TYPE_EQ, 0.75f), changed).has_ack());  // only a parameter value differs
  const core::FxChain* c2 = rig.br->snapshots().current()->parts[0].chain.get();
  CHECK(c2 != c1 && c2 != nullptr);

  // Lowering the budget below the current cost never blocks a command that does not raise the cost.
  CHECK(rig.plane.handle(chain_slots(1, {{ws::v1::INSERT_TYPE_EQ, false}, {ws::v1::INSERT_TYPE_COMPRESSOR, false}}), changed).has_ack());
  const std::uint64_t cost = rig.plane.fx_cost();
  rig.plane.set_fx_budget(core::kFixedFxCost);  // far below the current cost
  ws::v1::ServerMessage r = rig.plane.handle(chain_slots(2, {{ws::v1::INSERT_TYPE_EQ, false}}), changed);
  CHECK(!changed && is_err(r, ws::v1::ERROR_CODE_FX_BUDGET_EXCEEDED));  // raising it is refused
  r = rig.plane.handle(chain_slots(1, {{ws::v1::INSERT_TYPE_EQ, false}}), changed);  // removes the compressor
  CHECK(changed && r.has_ack() && rig.plane.fx_cost() < cost);
  ws::v1::ClientMessage out;
  out.set_request_id(++g_rid);
  out.mutable_set_part_output()->set_part(4);
  out.mutable_set_part_output()->set_output(ws::v1::PART_OUTPUT_AUX);
  CHECK(rig.plane.handle(out, changed).has_ack() && changed);  // other commands go through
  r = rig.plane.handle(chain_slots(1, {}), changed);
  CHECK(changed && r.has_ack());
  r = rig.plane.handle(chain_slots(0, {}), changed);
  CHECK(changed && r.has_ack() && rig.plane.fx_cost() == core::kFixedFxCost);
}

static float window_peak(Rig& rig, int blocks) {
  float pk = 0;
  for (int b = 0; b < blocks; ++b) {
    rig.step();
    for (int i = 0; i < 64; ++i) pk = std::max(pk, std::max(std::fabs(rig.l[i]), std::fabs(rig.r[i])));
  }
  return pk;
}

class ConstLineIn final : public ports::LineInSource {
 public:
  void read_block(float* l, float* r, std::uint32_t frames) noexcept override {
    for (std::uint32_t i = 0; i < frames; ++i) l[i] = r[i] = 0.2f;
  }
};

static void test_master_level_and_line_in_through_control() {
  Rig rig;
  CHECK(rig.load("{}"));
  const std::uint32_t on[] = {0x2000'0000u | (0x9u << 20) | (0u << 16) | (60u << 8) | 100u};
  rig.ingest.on_ump(on, 1, rig.now());
  rig.step(40);
  const float before = window_peak(rig, 12);
  CHECK(before > 0.01f);
  ws::v1::ClientMessage m;
  m.set_request_id(++g_rid);
  auto* v = m.mutable_set_master_params()->add_values();
  v->set_param_id(ws::v1::PARAM_MASTER_LEVEL);
  v->set_value(-6.0206f);
  bool changed = false;
  CHECK(rig.plane.handle(m, changed).has_ack() && changed);
  rig.step(6);
  const float after = window_peak(rig, 12);
  CHECK(std::fabs(after / before - 0.5f) < 0.03f);

  // line_in_level is a performance parameter with master scope: a UMP event reaches the mixer (the part field is ignored)
  ConstLineIn src;
  Rig r2;
  CHECK(r2.load("{}"));
  r2.br->set_line_in(&src);
  r2.step(4);
  CHECK(window_peak(r2, 4) == 0.0f);  // default level 0
  ws::v1::ClientMessage lm;
  lm.set_request_id(++g_rid);
  auto* pm = lm.mutable_set_performance_param();
  pm->set_param_id(ws::v1::PARAM_LINE_IN_LEVEL);
  pm->set_value(0.5f);
  CHECK(r2.plane.handle(lm, changed).has_ack() && !changed);
  r2.step(6);
  CHECK(std::fabs(r2.live(p::kLineInLevel, 0) - 0.5f) < 1e-6f);
  const float lp = window_peak(r2, 4);
  CHECK(std::fabs(lp - 0.1f) < 0.002f);
}

static void test_render_block_buses_equivalence() {
  Rig a, b;
  CHECK(a.load("{}") && b.load("{}"));
  bool changed = false;
  for (Rig* r : {&a, &b}) {
    ws::v1::ClientMessage out;
    out.set_request_id(++g_rid);
    out.mutable_set_part_output()->set_part(0);
    out.mutable_set_part_output()->set_output(ws::v1::PART_OUTPUT_AUX);
    CHECK(r->plane.handle(out, changed).has_ack());
    ws::v1::ClientMessage sm;
    sm.set_request_id(++g_rid);
    sm.mutable_set_send_levels()->set_part(0);
    sm.mutable_set_send_levels()->set_reverb(0.5f);
    CHECK(r->plane.handle(sm, changed).has_ack());
  }
  const std::uint32_t on[] = {0x2000'0000u | (0x9u << 20) | (0u << 16) | (60u << 8) | 100u};
  for (Rig* r : {&a, &b}) r->ingest.on_ump(on, 1, r->now());
  ws::v1::ClientMessage pm;  // a control-lane event as well
  pm.set_request_id(++g_rid);
  auto* sp = pm.mutable_set_performance_param();
  sp->set_param_id(ws::v1::PARAM_SEND_DELAY_LEVEL);
  sp->set_part(0);
  sp->set_value(0.25f);
  for (Rig* r : {&a, &b}) CHECK(r->plane.handle(pm, changed).has_ack());
  float al[64], ar[64];
  float worst = 0, peak_aux = 0;
  for (int i = 0; i < 40; ++i) {
    const std::uint64_t t = a.clock_ns.fetch_add(1'333'333) + 1'333'333;
    b.clock_ns.store(a.now());
    a.drv.render_block(a.l, a.r, t);
    core::OutputBuses ob{b.l, b.r, al, ar};
    b.drv.render_block_buses(ob, t);
    for (int k = 0; k < 64; ++k) {
      worst = std::max(worst, std::fabs(a.l[k] - (b.l[k] + al[k])));  // render_block folds Aux into Main
      worst = std::max(worst, std::fabs(a.r[k] - (b.r[k] + ar[k])));
      peak_aux = std::max(peak_aux, std::fabs(al[k]));
    }
  }
  CHECK(worst < 1e-7f && peak_aux > 0.01f);
  CHECK(a.drv.counters().blocks.load() == b.drv.counters().blocks.load() && a.drv.counters().blocks.load() == 40);
  CHECK(a.drv.counters().events.load() == b.drv.counters().events.load() && a.drv.counters().events.load() >= 2);
  CHECK(a.br->aux_folded_blocks() > 0 && b.br->aux_folded_blocks() == 0);
}

static void test_concurrent_clients() {
  Live L("conc");
  constexpr int kClients = 8, kPerClient = 25;
  std::atomic<bool> stop{false};
  std::atomic<int> rt_bad{0};
  std::thread rt([&] {  // a real "RT" thread keeps rendering while the control thread swaps
    while (!stop.load()) {
      L.rig.step();
      const core::Snapshot* h = L.rig.br->held_snapshot();
      if (h && h->magic != core::Snapshot::kMagic) rt_bad.fetch_add(1);
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  });
  Client sub;
  CHECK(L.connect(sub));
  ws::v1::ServerMessage sr;
  ws::v1::ClientMessage subscribe;
  subscribe.set_request_id(++g_rid);
  subscribe.mutable_subscribe()->set_enable(true);
  CHECK(sub.call(subscribe, sr));
  CHECK(sub.recv(sr) && sr.state().revision() == 1);

  std::vector<std::vector<std::uint64_t>> revs(kClients);
  std::vector<float> last(kClients, -1);
  std::vector<std::thread> th;
  std::atomic<int> failures{0};
  for (int i = 0; i < kClients; ++i) {
    th.emplace_back([&, i] {
      Client c;
      ws::v1::ServerMessage rep;
      if (!c.open(L.path) || !c.call(hello(), rep)) { failures++; return; }
      for (int k = 0; k < kPerClient; ++k) {
        ws::v1::ClientMessage m;
        m.set_request_id(1000 * (i + 1) + k);
        m.mutable_set_send_levels()->set_part(static_cast<std::uint32_t>(i));
        const float v = (k + 1) / 100.0f;
        m.mutable_set_send_levels()->set_reverb(v);
        if (!c.call(m, rep) || !rep.has_ack() || rep.request_id() != m.request_id()) { failures++; return; }
        revs[static_cast<std::size_t>(i)].push_back(rep.revision());
        last[static_cast<std::size_t>(i)] = v;
      }
    });
  }
  for (auto& t : th) t.join();
  CHECK(failures.load() == 0);
  std::set<std::uint64_t> all;
  for (auto& v : revs) {
    for (std::size_t k = 1; k < v.size(); ++k) CHECK(v[k] > v[k - 1]);  // per client: strictly increasing
    all.insert(v.begin(), v.end());
  }
  CHECK(all.size() == static_cast<std::size_t>(kClients * kPerClient));  // no revision handed out twice
  CHECK(*all.begin() == 2 && *all.rbegin() == 1 + kClients * kPerClient);  // contiguous: no lost update
  // no lost update: every part ends with its client's last value
  Client c;
  ws::v1::ServerMessage rep;
  CHECK(L.connect(c) && c.call(get_state(), rep) && rep.state().revision() == 1 + kClients * kPerClient);
  for (int i = 0; i < kClients; ++i) CHECK(std::fabs(rep.state().parts(i).send_reverb() - last[static_cast<std::size_t>(i)]) < 1e-6f);
  // the subscriber saw strictly increasing revisions
  std::uint64_t prev = 1;
  int got = 0;
  ws::v1::ServerMessage push;
  while (got < kClients * kPerClient && sub.recv(push, 1000)) {
    CHECK(push.state().revision() > prev);
    prev = push.state().revision();
    ++got;
  }
  CHECK(got == kClients * kPerClient);
  stop.store(true);
  rt.join();
  CHECK(rt_bad.load() == 0);
}

static void test_generation_exhaustion() {
  Rig rig;
  CHECK(rig.load("{}"));
  ws::v1::ServerMessage r;
  bool changed = false;
  // every swap is followed by a new held note, so every generation keeps a sounding reference
  for (int i = 0; i < 31; ++i) {
    rig.br->submit({core::make_note_on(0, 0, static_cast<std::uint32_t>(40 + i), 30000), 0});
    rig.step(2);
    r = rig.plane.handle(chain(0, {ws::v1::INSERT_TYPE_EQ}), changed);
    CHECK(r.has_ack() && changed);
    rig.step(2);
  }
  rig.br->submit({core::make_note_on(0, 0, 100, 30000), 0});
  rig.step(2);
  CHECK(rig.plane.live_generations() == 32);
  const auto rev = rig.plane.revision();
  r = rig.plane.handle(chain(0, {ws::v1::INSERT_TYPE_EQ}), changed);
  CHECK(is_err(r, ws::v1::ERROR_CODE_BUSY) && !changed && rig.plane.revision() == rev);
  for (int i = 0; i < 31; ++i) rig.br->submit({core::make_note_off(0, 0, static_cast<std::uint32_t>(40 + i)), 0});
  rig.br->submit({core::make_note_off(0, 0, 100), 0});
  rig.step(6);
  r = rig.plane.handle(chain(0, {ws::v1::INSERT_TYPE_EQ}), changed);
  CHECK(r.has_ack() && changed);
  CHECK(rig.plane.live_generations() <= 3);
}

static void test_socket_lifecycle() {
  Rig rig;
  CHECK(rig.load("{}"));
  const std::string path = "/tmp/velora-ctl-life-" + std::to_string(::getpid()) + ".sock";
  std::string err;
  // a stale socket file (no listener) is replaced
  {
    int s = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
    CHECK(::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
    ::close(s);
  }
  ControlServer a(rig.plane, path);
  CHECK_MSG(a.start(err), err);
  ControlServer b(rig.plane, path);  // a live listener is never stolen
  CHECK(!b.start(err) && err.find("already listening") != std::string::npos);
  ControlServer tooLong(rig.plane, std::string(200, 'x'));
  CHECK(!tooLong.start(err));
  Client c;
  ws::v1::ServerMessage r;
  CHECK(c.open(path) && c.call(hello(), r) && r.has_hello_ack());
  a.stop();
  CHECK(::access(path.c_str(), F_OK) != 0);  // socket file removed on stop
}


static void test_reclaim_respects_the_hazard() {
  Rig rig;
  CHECK(rig.load("{}"));
  rig.step();  // RT adopts generation 1; no voice references it
  const core::Snapshot* g1 = rig.br->snapshots().rt_held();
  CHECK(g1 && g1->revision == 1 && rig.br->snapshots().refs(g1->ledger_index) == 0);
  bool changed = false;
  CHECK(rig.plane.handle(chain(0, {ws::v1::INSERT_TYPE_EQ}), changed).has_ack() && changed);  // gen 2; RT has not run
  CHECK(rig.plane.reclaim() == 0);
  CHECK(rig.plane.live_generations() == 2 && rig.plane.freed_generations() == 0);
  CHECK(g1->magic == core::Snapshot::kMagic && g1->revision == 1);  // still allocated and intact
  rig.step();  // RT moves on to generation 2
  CHECK(rig.plane.reclaim() == 1 && rig.plane.live_generations() == 1 && rig.plane.freed_generations() == 1);
}

static void test_lane_tie_break_is_midi_first() {
  for (int variant = 0; variant < 2; ++variant) {
    Rig rig;
    CHECK(rig.load("{}"));
    rig.step();
    const p::ParamDef& d = *p::find(p::kDrawbar8);
    const std::uint64_t t = rig.now() + 1'000'000;
    const float midi_v = variant == 0 ? 8.0f : 2.0f, ctl_v = variant == 0 ? 2.0f : 8.0f;
    rig.midi_q->push(StampedUmp{core::make_param_event(d, 0, midi_v), t});
    rig.ctl_q->push(StampedUmp{core::make_param_event(d, 0, ctl_v), t});  // same host time, same frame
    rig.step(4);
    CHECK(rig.live(p::kDrawbar8, 0) == ctl_v);  // MIDI lane applies first, the control lane lands last
    CHECK(rig.br->param_events_applied() == 2);
  }
}

// --- server robustness ---------------------------------------------------------------------------------
struct Custom {
  Rig rig;
  std::unique_ptr<ControlServer> server;
  std::string path;
  Custom(const char* tag, ControlServerLimits lim = {}) {
    path = "/tmp/velora-ctl-" + std::string(tag) + "-" + std::to_string(::getpid()) + ".sock";
    std::string err;
    CHECK(rig.load("{}"));
    server = std::make_unique<ControlServer>(rig.plane, path, lim);
    CHECK_MSG(server->start(err), err);
  }
  bool connect(Client& c) {
    ws::v1::ServerMessage ack;
    return c.open(path) && c.call(hello(), ack) && ack.has_hello_ack();
  }
};

static ws::v1::ClientMessage send_levels(std::uint32_t part, float reverb) {
  ws::v1::ClientMessage m;
  m.set_request_id(++g_rid);
  m.mutable_set_send_levels()->set_part(part);
  m.mutable_set_send_levels()->set_reverb(reverb);
  return m;
}

static std::string wire(const ws::v1::ClientMessage& m) {
  std::string b;
  m.SerializeToString(&b);
  return Client::frame(b);
}

static void test_subscribe_off_and_fragmentation() {
  Custom L("frag");
  Client sub, c;
  CHECK(L.connect(sub) && L.connect(c));
  ws::v1::ServerMessage r, push;
  ws::v1::ClientMessage m;
  m.set_request_id(++g_rid);
  m.mutable_subscribe()->set_enable(true);
  CHECK(sub.call(m, r) && sub.recv(push) && push.state().revision() == 1);
  m.set_request_id(++g_rid);
  m.mutable_subscribe()->set_enable(false);
  CHECK(sub.call(m, r) && r.has_ack());
  CHECK(c.call(send_levels(0, 0.5f), r) && r.revision() == 2);
  CHECK(!sub.recv(push, 150));  // unsubscribed: no push for the structural change
  CHECK(sub.call(get_state(), r) && r.state().revision() == 2);  // and the connection is fine

  // one frame split into single bytes across many sends
  const std::string one = wire(send_levels(1, 0.25f));
  for (char ch : one) {
    CHECK(c.write_all(std::string(1, ch)));
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  CHECK(c.recv(r) && r.has_ack() && r.revision() == 3);
  // several frames in one write
  std::string many;
  for (int i = 0; i < 5; ++i) many += wire(send_levels(2, 0.1f * (i + 1)));
  CHECK(c.write_all(many));
  for (int i = 0; i < 5; ++i) CHECK(c.recv(r) && r.has_ack() && r.revision() == static_cast<std::uint64_t>(4 + i));
  // header split from body, and a frame followed by half of the next
  const std::string a = wire(send_levels(3, 0.3f)), b = wire(send_levels(3, 0.4f));
  CHECK(c.write_all(a + b.substr(0, 3)));
  CHECK(c.recv(r) && r.revision() == 9);
  CHECK(c.write_all(b.substr(3)));
  CHECK(c.recv(r) && r.revision() == 10);
}

static void test_disconnect_mid_frame_and_file_modes() {
  Custom L("mid");
  {
    Client c;
    CHECK(L.connect(c));
    const std::string f = wire(send_levels(0, 0.5f));
    CHECK(c.write_all(f.substr(0, f.size() - 2)));  // dies mid-frame
  }
  Client d;
  ws::v1::ServerMessage r;
  CHECK(L.connect(d) && d.call(get_state(), r) && r.state().revision() == 1);  // nothing was applied, server fine
  struct stat st{};
  CHECK(::stat(L.path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode) && (st.st_mode & 0777) == 0600);

  // a path that is not a socket is never unlinked or replaced
  const std::string reg = "/tmp/velora-ctl-notsock-" + std::to_string(::getpid());
  if (FILE* f = std::fopen(reg.c_str(), "w")) { std::fputs("keep", f); std::fclose(f); }
  ControlServer bad(L.rig.plane, reg);
  std::string err;
  CHECK(!bad.start(err) && err.find("not a socket") != std::string::npos);
  CHECK(slurp(reg.c_str()) == "keep");
  ::unlink(reg.c_str());
}

static void test_handshake_deadline_and_client_cap() {
  ControlServerLimits lim;
  lim.max_clients = 3;
  lim.handshake_timeout_ms = 300;
  Custom L("lim", lim);
  Client idle;
  CHECK(idle.open(L.path));  // never says Hello
  CHECK(idle.closed_by_peer(2000));
  Client a, b, c, over;
  CHECK(L.connect(a) && L.connect(b) && L.connect(c));
  CHECK(over.open(L.path));
  CHECK(over.closed_by_peer(1500));  // beyond max_clients
  ws::v1::ServerMessage r;
  CHECK(a.call(get_state(), r) && r.has_state());  // the others are unaffected
}

static void test_slow_subscriber_is_dropped() {
  Custom L("slow");
  Client slow, c;
  CHECK(L.connect(slow) && L.connect(c));
  int rcv = 4096;
  ::setsockopt(slow.fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
  ws::v1::ServerMessage r;
  ws::v1::ClientMessage m;
  m.set_request_id(++g_rid);
  m.mutable_subscribe()->set_enable(true);
  CHECK(slow.send(m));  // and never reads again
  for (int i = 0; i < 4000; ++i) {
    CHECK(c.call(send_levels(static_cast<std::uint32_t>(i % 8), (i % 100) / 100.0f), r) && r.has_ack());
    if (i % 50 == 0 && wait_until([&] { return L.server->dropped_slow() > 0; }, 1)) break;
  }
  CHECK(wait_until([&] { return L.server->dropped_slow() == 1; }));
  CHECK(c.call(get_state(), r) && r.has_state());  // the others keep working
  // the slow client sees its buffered data and then EOF
  char buf[65536];
  bool eof = false;
  for (int i = 0; i < 100000 && !eof; ++i) {
    pollfd pf{slow.fd, POLLIN, 0};
    if (::poll(&pf, 1, 1000) <= 0) break;
    const ssize_t n = ::recv(slow.fd, buf, sizeof buf, 0);
    eof = n <= 0;
  }
  CHECK(eof);
}

int main() {
  test_dev_config();
  test_handshake();
  test_structural_commands();
  test_invalid_commands();
  test_performance_path();
  test_held_note_across_swap();
  test_fx_chain_and_budget();
  test_effect_lifetime_across_generations();
  test_chain_identity_and_budget_lowering();
  test_master_level_and_line_in_through_control();
  test_render_block_buses_equivalence();
  test_concurrent_clients();
  test_generation_exhaustion();
  test_socket_lifecycle();
  test_reclaim_respects_the_hazard();
  test_lane_tie_break_is_midi_first();
  test_subscribe_off_and_fragmentation();
  test_disconnect_mid_frame_and_file_modes();
  test_handshake_deadline_and_client_cap();
  test_slow_subscriber_is_dropped();
  if (g_fail) std::printf("control_tests: %d failure(s)\n", g_fail);
  else std::printf("control_tests: ok\n");
  return g_fail ? 1 : 0;
}
