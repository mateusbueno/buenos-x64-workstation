// The sysd TCP bridge (Foundation: Velora Studio device connection) against real sockets on 127.0.0.1 (ephemeral ports) and fake engine / librarian Unix
// sockets in a temp dir: handshake and feature negotiation, the unpaired allow-list, pairing (code, 5 wrong, 2-minute window with an injected clock,
// persistence), dev bypass, forwarding with request_id preserved, push relay, slow client, oversize / malformed frames, limits, upstream loss and the shared
// config service. No root, nothing beyond localhost. The real engine-host is exercised by engine/host/macos/engine_host_bridge_smoke.cpp.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <sys/stat.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "library_api.pb.h"
#include "sysdctl_client.hpp"
#include "velora/sysd/bridge_server.hpp"
#include "velora/sysd/config_server.hpp"
#include "velora/sysd/pairing.hpp"

namespace fs = std::filesystem;
namespace sd = velora::sysd;
using Clock = std::chrono::steady_clock;

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
    std::string t = "/tmp/vbr-XXXXXX";
    if (!::mkdtemp(t.data())) std::abort();
    path = t;
  }
  ~TmpDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

bool wait_for(const std::function<bool()>& pred, int timeout_ms = 5000) {
  const auto end = Clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!pred()) {
    if (Clock::now() > end) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return true;
}

// ---- framing helpers --------------------------------------------------------------------------------------------------------------------------------
void put_frame(std::string& out, const std::string& body) {
  const std::uint32_t n = static_cast<std::uint32_t>(body.size());
  const char h[4] = {char(n >> 24), char(n >> 16), char(n >> 8), char(n)};
  out.append(h, 4);
  out.append(body);
}

bool write_all(int fd, const std::string& s) {
  std::size_t off = 0;
  while (off < s.size()) {
    const ssize_t n = ::send(fd, s.data() + off, s.size() - off, 0);
    if (n <= 0) return false;
    off += static_cast<std::size_t>(n);
  }
  return true;
}

// Reads one frame within timeout_ms. 1 = got, 0 = timeout, -1 = closed / error.
int read_frame(int fd, std::string& rx, std::string& payload, int timeout_ms) {
  const auto end = Clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    if (rx.size() >= 4) {
      const std::uint32_t len = (std::uint8_t(rx[0]) << 24) | (std::uint8_t(rx[1]) << 16) | (std::uint8_t(rx[2]) << 8) | std::uint8_t(rx[3]);
      if (rx.size() >= 4 + len) {
        payload = rx.substr(4, len);
        rx.erase(0, 4 + len);
        return 1;
      }
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count();
    if (left <= 0) return 0;
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, static_cast<int>(left)) <= 0) continue;
    char buf[65536];
    const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
    if (n <= 0) return -1;
    rx.append(buf, static_cast<std::size_t>(n));
  }
}

// A TCP client of the bridge.
struct Client {
  int fd = -1;
  std::string rx;
  std::uint64_t next_id = 1;
  bool connect_to(int port) {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<std::uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    return ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
  }
  ~Client() {
    if (fd >= 0) ::close(fd);
  }
  void send_raw(const std::string& bytes) { write_all(fd, bytes); }
  std::uint64_t send(ws::v1::ClientMessage m) {
    if (m.request_id() == 0) m.set_request_id(next_id++);
    std::string f;
    put_frame(f, m.SerializeAsString());
    write_all(fd, f);
    return m.request_id();
  }
  // 1 got, 0 timeout, -1 closed
  int recv(ws::v1::ServerMessage& out, int timeout_ms = 3000) {
    std::string p;
    const int r = read_frame(fd, rx, p, timeout_ms);
    if (r == 1 && !out.ParseFromString(p)) return -1;
    return r;
  }
  bool closed(int timeout_ms = 3000) {
    ws::v1::ServerMessage m;
    const auto end = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
      const int r = recv(m, 100);
      if (r == -1) return true;
      if (Clock::now() > end) return false;
    }
  }
  // Hello with the given features; returns the reply.
  ws::v1::ServerMessage hello(const std::vector<std::string>& features, const std::string& id, std::uint32_t version = 1, const std::string& token = "") {
    ws::v1::ClientMessage m;
    m.mutable_hello()->set_version(version);
    m.mutable_hello()->set_client_id(id);
    m.mutable_hello()->set_pairing_token(token);
    m.mutable_hello()->set_client_name("bridge-test");
    m.mutable_hello()->set_client_version("0.0.1");
    for (const auto& f : features) m.mutable_hello()->add_features(f);
    send(m);
    ws::v1::ServerMessage r;
    recv(r);
    return r;
  }
};

const std::vector<std::string> kAll = {"params.v1", "snapshots.v1", "subscribe.v1", "liveset.v1", "config.v1", "library.v1"};

// A fake upstream on a Unix socket. `handler` answers each non-Hello frame. Hello is answered by the fake itself.
struct FakeUds {
  std::string path;
  bool library = false;
  std::function<void(const std::string& frame, std::vector<std::string>& replies)> handler;
  // library fakes only (Story 9.11): when set and it returns true, `reply` (request_id filled in) answers instead of the default Ack
  std::function<bool(const ws::v1::LibraryClientMessage& m, ws::v1::LibraryServerMessage& reply)> lib_reply;
  std::atomic<bool> stop{false};
  std::atomic<int> received{0};
  std::atomic<int> connections{0};
  std::mutex mu;
  std::vector<int> fds;
  std::vector<std::uint64_t> seen_ids;  // request_ids the fake saw (non-Hello)
  int lfd = -1;
  std::thread acceptor;
  std::vector<std::thread> workers;

  void start(const std::string& p, bool lib) {
    path = p;
    library = lib;
    lfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::snprintf(a.sun_path, sizeof a.sun_path, "%s", path.c_str());
    ::unlink(path.c_str());
    if (::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(lfd, 8) != 0) std::abort();
    acceptor = std::thread([this] {
      while (!stop.load()) {
        pollfd p{lfd, POLLIN, 0};
        if (::poll(&p, 1, 50) <= 0) continue;
        const int fd = ::accept(lfd, nullptr, nullptr);
        if (fd < 0) continue;
        connections.fetch_add(1);
        {
          std::lock_guard<std::mutex> lk(mu);
          fds.push_back(fd);
        }
        workers.emplace_back([this, fd] { serve(fd); });
      }
    });
  }
  void serve(int fd) {
    std::string rx, payload;
    while (!stop.load()) {
      const int r = read_frame(fd, rx, payload, 50);
      if (r == -1) break;
      if (r == 0) continue;
      std::vector<std::string> replies;
      if (library) {
        ws::v1::LibraryClientMessage m;
        m.ParseFromString(payload);
        if (m.has_hello()) {
          ws::v1::LibraryServerMessage a;
          a.set_request_id(m.request_id());
          a.mutable_hello_ack()->set_version(1);
          replies.push_back(a.SerializeAsString());
        } else {
          received.fetch_add(1);
          {
            std::lock_guard<std::mutex> lk(mu);
            seen_ids.push_back(m.request_id());
          }
          ws::v1::LibraryServerMessage a;
          a.set_request_id(m.request_id());
          if (!(lib_reply && lib_reply(m, a))) {
            a.Clear();
            a.set_request_id(m.request_id());
            a.mutable_ack()->set_changed(true);
          }
          replies.push_back(a.SerializeAsString());
        }
      } else {
        ws::v1::ClientMessage m;
        m.ParseFromString(payload);
        if (m.has_hello()) {
          ws::v1::ServerMessage a;
          a.set_request_id(m.request_id());
          a.mutable_hello_ack()->set_version(1);
          replies.push_back(a.SerializeAsString());
        } else {
          received.fetch_add(1);
          {
            std::lock_guard<std::mutex> lk(mu);
            seen_ids.push_back(m.request_id());
          }
          if (handler) handler(payload, replies);
        }
      }
      std::string out;
      for (const auto& b : replies) put_frame(out, b);
      if (!out.empty()) write_all(fd, out);
    }
    ::close(fd);
  }
  void push_all(const ws::v1::ServerMessage& m) {
    std::string out;
    put_frame(out, m.SerializeAsString());
    std::lock_guard<std::mutex> lk(mu);
    for (int fd : fds) write_all(fd, out);
  }
  void kill_connections() {
    std::lock_guard<std::mutex> lk(mu);
    for (int fd : fds) ::shutdown(fd, SHUT_RDWR);
  }
  ~FakeUds() {
    stop.store(true);
    kill_connections();
    if (acceptor.joinable()) acceptor.join();
    for (auto& w : workers) w.join();
    if (lfd >= 0) ::close(lfd);
    ::unlink(path.c_str());
  }
};

// The engine fake: GetState -> State(revision 7, current_scene 3); Subscribe -> Ack + State push; anything else -> Ack.
std::atomic<std::uint64_t> g_seen_base_revision{0};  // Story 9.9: the base_revision of the last edit the fake engine saw
std::atomic<int> g_seen_base_count{0};

void engine_handler(const std::string& payload, std::vector<std::string>& replies) {
  ws::v1::ClientMessage m;
  m.ParseFromString(payload);
  if (m.base_revision() != 0) {
    g_seen_base_revision = m.base_revision();
    ++g_seen_base_count;
  }
  ws::v1::ServerMessage r;
  r.set_request_id(m.request_id());
  r.set_revision(7);
  if (m.base_revision() == 0xDEAD) {  // Story 9.10: the engine's typed refusal of an edit built on an older Live Set revision
    auto* e = r.mutable_error();
    e->set_code(ws::v1::ERROR_CODE_STALE_REVISION);
    e->set_message("stale");
    (*e->mutable_details())["base_revision"] = "57005";
    (*e->mutable_details())["current_revision"] = "9";
  } else if (m.has_get_state()) {
    r.mutable_state()->set_revision(7);
    r.mutable_state()->set_current_scene(3);
  } else if (m.has_get_live_set_view()) {  // Story 9.8: the engine's read-only view
    auto* v = r.mutable_live_set_view_reply();
    v->set_live_set_id(m.get_live_set_view().live_set_id());
    v->set_name("Fake view");
    for (std::uint32_t i = 0; i < 8; ++i) v->add_scenes()->set_index(i);
  } else {
    r.mutable_ack();
  }
  replies.push_back(r.SerializeAsString());
  if (m.has_subscribe() && m.subscribe().enable()) {
    ws::v1::ServerMessage p;
    p.set_revision(7);
    p.mutable_state()->set_revision(7);
    p.mutable_state()->set_current_scene(3);
    replies.push_back(p.SerializeAsString());
  }
}

struct Rig {
  TmpDir tmp;
  std::string cfgdir, brdir;
  std::atomic<std::int64_t> clock{1000};
  std::unique_ptr<sd::ConfigStore> store;
  std::unique_ptr<sd::ConfigService> svc;
  std::unique_ptr<sd::PairingWindow> window;
  std::unique_ptr<sd::PairedStore> paired;
  std::unique_ptr<sd::BridgeServer> bridge;
  std::mutex logmu;
  std::vector<std::string> logs;

  Rig() {
    cfgdir = tmp.path + "/config";
    brdir = tmp.path + "/bridge";
    fs::create_directories(cfgdir);
    fs::create_directories(brdir);
    sd::StoreOptions so;
    so.dir = cfgdir;
    so.log = [](const std::string&) {};
    store = std::make_unique<sd::ConfigStore>(std::move(so));
    svc = std::make_unique<sd::ConfigService>(*store);
    window = std::make_unique<sd::PairingWindow>([this] { return clock.load(); });
  }
  void make_paired() {
    paired = std::make_unique<sd::PairedStore>(brdir, [this](const std::string& s) {
      std::lock_guard<std::mutex> lk(logmu);
      logs.push_back(s);
    });
  }
  bool start(const std::function<void(sd::BridgeOptions&)>& tweak = {}) {
    if (!paired) make_paired();
    sd::BridgeOptions bo;
    bo.model = "Velora 73";
    bo.firmware_version = "9.9.9";
    bo.now_ms = [this] { return std::int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count()); };
    if (tweak) tweak(bo);
    bridge = std::make_unique<sd::BridgeServer>(*svc, *window, *paired, bo);
    std::string err;
    if (!bridge->start(err)) {
      std::printf("bridge start: %s\n", err.c_str());
      return false;
    }
    return true;
  }
  void restart(const std::function<void(sd::BridgeOptions&)>& tweak = {}) {
    bridge.reset();
    paired.reset();
    start(tweak);
  }
  int port() const { return bridge->port(); }
};

ws::v1::ClientMessage get_state() {
  ws::v1::ClientMessage m;
  m.mutable_get_state();
  return m;
}

ws::v1::ClientMessage pair(const std::string& code, const std::string& name = "My Mac") {
  ws::v1::ClientMessage m;
  m.mutable_pair_request()->set_code(code);
  m.mutable_pair_request()->set_computer_name(name);
  return m;
}

// Pairs `c` (already Hello'd) with the code; returns the pairing token ("" when refused).
std::string pair_with(Client& c, const std::string& code, const std::string& name = "My Mac") {
  c.send(pair(code, name));
  ws::v1::ServerMessage r;
  if (c.recv(r) != 1 || !r.has_pair_reply() || !r.pair_reply().paired()) return "";
  return r.pair_reply().pairing_token();
}

// ---- tests ------------------------------------------------------------------------------------------------------------------------------------------
void test_pairing_window_unit() {
  std::int64_t t = 0;
  sd::PairingWindow w([&] { return t; });
  using R = sd::PairingWindow::Result;
  CHECK(w.attempt("123456") == R::kNotOpen);
  w.open("123456");
  CHECK(w.is_open() && w.attempts_left() == 5);
  CHECK(w.attempt("000000") == R::kWrong && w.attempts_left() == 4);
  CHECK(w.attempt("123456") == R::kOk);  // a right code after some wrong ones; it closes the window (one code pairs one computer)
  CHECK(!w.is_open() && w.attempt("123456") == R::kNotOpen);
  w.open("123456");
  for (int i = 0; i < 4; ++i) CHECK(w.attempt("x") == R::kWrong);
  CHECK(w.attempt("x") == R::kClosedAttempts);  // 5th wrong in this window
  CHECK(!w.is_open());
  CHECK(w.attempt("123456") == R::kClosedAttempts);  // the right code no longer works
  w.open("654321");  // reopened: a fresh window and counter
  CHECK(w.attempts_left() == 5);
  t += sd::PairingWindow::kWindowMs - 1;
  CHECK(w.is_open());
  t += 1;  // exactly 2 minutes
  CHECK(!w.is_open() && w.attempt("654321") == R::kClosedExpired);
  CHECK(w.attempt("654321") == R::kNotOpen);
  CHECK(sd::PairingWindow::constant_time_equals("abc", "abc") && !sd::PairingWindow::constant_time_equals("abc", "abd") &&
        !sd::PairingWindow::constant_time_equals("abc", "abcd") && !sd::PairingWindow::constant_time_equals("", "a"));
  std::string c;
  CHECK(sd::PairingWindow::random_code(c) && c.size() == 6 && c.find_first_not_of("0123456789") == std::string::npos);
  CHECK(sd::PairingWindow::valid_code("123456") && sd::PairingWindow::valid_code("12345678") && !sd::PairingWindow::valid_code("12345") &&
        !sd::PairingWindow::valid_code("12345a") && !sd::PairingWindow::valid_code(""));
  // SHA-256 known vectors (FIPS 180-4 / NIST).
  CHECK(sd::sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sd::sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(sd::sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK(sd::sha256_hex(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  unsigned char r1[32] = {}, r2[32] = {};
  CHECK(sd::secure_random(r1, 32) && sd::secure_random(r2, 32) && std::memcmp(r1, r2, 32) != 0);
}

void test_paired_store() {
  TmpDir d;
  std::vector<std::string> logs;
  std::string dev;
  const std::string token = std::string(64, 'a');
  const std::string hash = sd::sha256_hex(token);
  {
    sd::PairedStore s(d.path, [&](const std::string& l) { logs.push_back(l); });
    CHECK(s.ok());
    dev = s.device_id();
    CHECK(!dev.empty() && s.list().empty());
    CHECK(s.add("mac-1", "Studio\tMac\n", 100, hash) && s.contains("mac-1"));
    CHECK(s.list().size() == 1 && s.list()[0].name == "StudioMac" && s.list()[0].last_connected_unix_s == 100);
    CHECK(!s.add("bad id!", "x", 1, hash) && !s.add("mac-9", "x", 1, "nothex"));
    // The token check: the right token for the right id only.
    CHECK(s.verify("mac-1", token));
    CHECK(!s.verify("mac-1", std::string(64, 'b')) && !s.verify("mac-1", "") && !s.verify("mac-2", token) && !s.verify("mac-1", hash));
    CHECK(s.touch("mac-1", 200));
    struct stat st{};
    CHECK(::stat((d.path + "/paired_computers.txt").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);  // only the hash, and only for the unit's user
    std::ifstream f(d.path + "/paired_computers.txt");
    std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(all.find(token) == std::string::npos && all.find(hash) != std::string::npos);  // the token itself is never stored
  }
  {
    sd::PairedStore s(d.path, [&](const std::string& l) { logs.push_back(l); });  // "restart"
    CHECK(s.device_id() == dev && s.contains("mac-1") && s.verify("mac-1", token) && s.list()[0].last_connected_unix_s == 200);
    CHECK(s.remove("mac-1") && !s.contains("mac-1"));
  }
  {  // corrupt file: empty list, a NEW device id and a log line, no crash
    std::ofstream(d.path + "/paired_computers.txt") << "garbage\x01\x02 not a list";
    logs.clear();
    sd::PairedStore s(d.path, [&](const std::string& l) { logs.push_back(l); });
    CHECK(s.list().empty() && !logs.empty() && logs[0].find("corrupt") != std::string::npos);
    CHECK(s.device_id() != dev && s.ok());
    CHECK(s.add("mac-2", "ok", 5, hash));  // and it is usable again
  }
  {  // the old v1 list (no tokens): its computers are NOT paired
    std::ofstream(d.path + "/paired_computers.txt") << "velora-paired-computers v1\ndevice_id=velora-old\nmac-old\tName\t5\n";
    logs.clear();
    sd::PairedStore s(d.path, [&](const std::string& l) { logs.push_back(l); });
    CHECK(!s.contains("mac-old") && s.device_id() == "velora-old" && !logs.empty());
  }
  {  // a v2 entry with a short / missing hash is corrupt
    std::ofstream(d.path + "/paired_computers.txt") << "velora-paired-computers v2\ndevice_id=velora-x\nmac-3\tName\t5\tabc\n";
    sd::PairedStore s(d.path, nullptr);
    CHECK(s.list().empty() && s.device_id() != "velora-x");
  }
  {  // at the cap the least recently connected computer is evicted
    TmpDir d2;
    sd::PairedStore s(d2.path, nullptr);
    for (std::size_t i = 0; i < sd::PairedStore::kMaxComputers; ++i) CHECK(s.add("c" + std::to_string(i), "n", 100 + static_cast<std::int64_t>(i), hash));
    CHECK(s.touch("c0", 5000));  // c0 is now the most recent; c1 the oldest
    CHECK(s.add("new", "n", 6000, hash));
    CHECK(s.list().size() == sd::PairedStore::kMaxComputers && s.contains("new") && s.contains("c0") && !s.contains("c1"));
  }
}

void test_hello_and_negotiation() {
  Rig r;
  r.window->open("111111");
  CHECK(r.start());
  Client c;
  CHECK(c.connect_to(r.port()));
  auto ack = c.hello({"params.v1", "snapshots.v1", "bogus.v9"}, "mac-a");
  CHECK(ack.has_hello_ack());
  const auto& a = ack.hello_ack();
  CHECK(a.version() == 1 && a.device_version() == "1" && a.firmware_version() == "9.9.9" && a.model() == "Velora 73" && !a.device_id().empty() && !a.paired());
  std::set<std::string> f(a.features().begin(), a.features().end());
  CHECK(f == (std::set<std::string>{"params.v1", "snapshots.v1", "pairing.v1"}));
  // A second Hello (once, while unpaired) re-answers with HelloAck (it is how a stored token is presented); a third is refused but does not close.
  ws::v1::ClientMessage h;
  h.mutable_hello()->set_version(1);
  c.send(h);
  ws::v1::ServerMessage e;
  CHECK(c.recv(e) == 1 && e.has_hello_ack() && !e.hello_ack().paired());
  c.send(h);
  CHECK(c.recv(e) == 1 && e.has_error() && e.error().code() == ws::v1::ERROR_CODE_INVALID_COMMAND);
  // Unknown major versions: typed error, closed (too new and too old).
  for (std::uint32_t v : {7u, 0u}) {
    Client x;
    CHECK(x.connect_to(r.port()));
    auto rep = x.hello(kAll, "mac-x", v);
    CHECK(rep.has_error() && rep.error().code() == ws::v1::ERROR_CODE_UNSUPPORTED_VERSION);
    CHECK(rep.error().details().at("reason") == (v == 7 ? "too_new" : "too_old"));
    CHECK(x.closed());
  }
  // First message not Hello.
  Client y;
  CHECK(y.connect_to(r.port()));
  y.send(get_state());
  ws::v1::ServerMessage er;
  CHECK(y.recv(er) == 1 && er.error().code() == ws::v1::ERROR_CODE_HANDSHAKE_REQUIRED && y.closed());
}

ws::v1::ClientMessage message_for_case(int field_number) {
  // A default instance of the body whose field number is given (built through reflection so the test follows the schema).
  ws::v1::ClientMessage m;
  const google::protobuf::FieldDescriptor* f = ws::v1::ClientMessage::descriptor()->FindFieldByNumber(field_number);
  m.GetReflection()->MutableMessage(&m, f);
  return m;
}

void test_unpaired_and_unsupported() {
  Rig r;
  FakeUds eng;
  eng.start(r.tmp.path + "/e.sock", false);
  eng.handler = engine_handler;
  r.window->open("111111");
  CHECK(r.start([&](sd::BridgeOptions& o) { o.engine_socket = eng.path; }));
  Client c;
  CHECK(c.connect_to(r.port()));
  CHECK(c.hello(kAll, "mac-a").hello_ack().paired() == false);
  // EVERY row of the message table is NOT_PAIRED for an unpaired client (except the allow-listed Hello / PairRequest), and nothing reaches the engine.
  int checked = 0;
  for (const auto& rule : sd::BridgeServer::message_rules()) {
    if (rule.unpaired_allowed) continue;
    c.send(message_for_case(rule.body_case));
    ws::v1::ServerMessage rep;
    CHECK(c.recv(rep) == 1 && rep.has_error() && rep.error().code() == ws::v1::ERROR_CODE_NOT_PAIRED);
    ++checked;
    if (checked % 4 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  CHECK(checked > 40);
  {  // a field number the table does not know is NOT_PAIRED too (unknown fields cannot reach a body switch)
    ws::v1::ClientMessage m;
    m.set_request_id(5);
    std::string raw = m.SerializeAsString() + std::string("\xFA\x06\x00", 3);  // field 111, length-delimited, empty
    std::string f;
    put_frame(f, raw);
    c.send_raw(f);
    ws::v1::ServerMessage rep;
    CHECK(c.recv(rep) == 1 && rep.error().code() == ws::v1::ERROR_CODE_NOT_PAIRED);
  }
  CHECK(eng.received.load() == 0 && eng.connections.load() == 0);
  // Pair, then a client that did not negotiate a feature gets UNSUPPORTED_FEATURE.
  Client d;
  CHECK(d.connect_to(r.port()));
  d.hello({"snapshots.v1"}, "mac-d");
  CHECK(!pair_with(d, "111111").empty());
  ws::v1::ClientMessage sm;
  sm.mutable_set_master_params();
  d.send(sm);
  ws::v1::ServerMessage u;
  CHECK(d.recv(u) == 1 && u.error().code() == ws::v1::ERROR_CODE_UNSUPPORTED_FEATURE && u.error().details().at("feature") == "params.v1");
  ws::v1::ClientMessage cm;
  cm.mutable_get_config();
  d.send(cm);
  CHECK(d.recv(u) == 1 && u.error().code() == ws::v1::ERROR_CODE_UNSUPPORTED_FEATURE && u.error().details().at("feature") == "config.v1");
  d.send(get_state());  // the negotiated one works
  CHECK(d.recv(u) == 1 && u.has_state());
  CHECK(eng.received.load() == 1);
  // Hardware / panel commands are refused even when paired and negotiated; nothing reaches the engine.
  for (const auto& rule : sd::BridgeServer::message_rules()) {
    if (rule.route != sd::BridgeServer::Route::kDenied) continue;
    d.send(message_for_case(rule.body_case));
    CHECK(d.recv(u) == 1 && u.error().code() == ws::v1::ERROR_CODE_INVALID_COMMAND && u.error().details().at("reason") == "not_available_over_bridge");
  }
  {  // an unknown body from a paired client: INVALID_COMMAND (no default-allow)
    ws::v1::ClientMessage m;
    m.set_request_id(6);
    std::string f;
    put_frame(f, m.SerializeAsString() + std::string("\xFA\x06\x00", 3));
    d.send_raw(f);
    CHECK(d.recv(u) == 1 && u.error().code() == ws::v1::ERROR_CODE_INVALID_COMMAND);
  }
  CHECK(eng.received.load() == 1);
}

// The tables are TOTAL: every field of ClientMessage's body oneof and of LibraryClientMessage's has a row. Adding a field without classifying it fails here.
void test_message_tables_are_total() {
  const auto* od = ws::v1::ClientMessage::descriptor()->FindOneofByName("body");
  CHECK(od != nullptr);
  int n = 0;
  for (int i = 0; od && i < od->field_count(); ++i) {
    const auto* f = od->field(i);
    const auto* rule = sd::BridgeServer::rule_for(f->number());
    if (!rule) std::printf("  unclassified ClientMessage field: %s (%d)\n", std::string(f->name()).c_str(), f->number());
    CHECK(rule != nullptr);
    if (rule) CHECK(f->name() == rule->name);
    ++n;
  }
  CHECK(n > 40 && sd::BridgeServer::message_rules().size() == static_cast<std::size_t>(n));  // no stale rows either
  for (const auto& r : sd::BridgeServer::message_rules()) {
    CHECK((r.unpaired_allowed) == (r.route == sd::BridgeServer::Route::kLocal));  // only Hello / PairRequest are unpaired-allowed
    CHECK((r.route == sd::BridgeServer::Route::kDenied || r.route == sd::BridgeServer::Route::kLocal) == (r.feature == nullptr));
  }
  const auto* ld = ws::v1::LibraryClientMessage::descriptor()->FindOneofByName("body");
  CHECK(ld != nullptr);
  int m = 0;
  for (int i = 0; ld && i < ld->field_count(); ++i, ++m) {
    const auto* f = ld->field(i);
    bool found = false;
    for (const auto& r : sd::BridgeServer::library_rules())
      if (r.body_case == f->number()) {
        found = true;
        CHECK(f->name() == r.name);
      }
    if (!found) std::printf("  unclassified LibraryClientMessage field: %s (%d)\n", std::string(f->name()).c_str(), f->number());
    CHECK(found);
  }
  CHECK(static_cast<std::size_t>(m) == sd::BridgeServer::library_rules().size());
  // The hardware commands the coordinator named are denied by decision.
  for (const char* name : {"set_led", "move_fader", "select_part", "set_fader_mode", "select_organ_part", "set_encoder_mode", "start_pedal_calibration",
                           "finish_pedal_calibration", "cancel_pedal_calibration"}) {
    bool ok = false;
    for (const auto& r : sd::BridgeServer::message_rules())
      if (std::string(r.name) == name) ok = r.route == sd::BridgeServer::Route::kDenied;
    CHECK(ok);
  }
}

void test_pairing_flow_and_persistence() {
  Rig r;
  FakeUds eng;
  eng.start(r.tmp.path + "/e.sock", false);
  eng.handler = engine_handler;
  r.window->open("424242");
  CHECK(r.start([&](sd::BridgeOptions& o) { o.engine_socket = eng.path; }));
  std::string token;
  {
    Client c;
    CHECK(c.connect_to(r.port()));
    c.hello(kAll, "mac-persist");
    c.send(pair("000000"));
    ws::v1::ServerMessage rep;
    CHECK(c.recv(rep) == 1 && rep.error().code() == ws::v1::ERROR_CODE_PAIRING_CODE_INVALID && rep.error().details().at("attempts_left") == "4");
    c.send(pair("424242", "Mateus's MacBook"));
    CHECK(c.recv(rep) == 1 && rep.has_pair_reply() && rep.pair_reply().paired() && rep.pair_reply().computer_id() == "mac-persist");
    token = rep.pair_reply().pairing_token();
    CHECK(sd::PairedStore::valid_token(token));  // 256 random bits, hex
    CHECK(r.paired->contains("mac-persist") && r.paired->verify("mac-persist", token));
    CHECK(!r.window->is_open());  // the first pairing closed the window
    c.send(get_state());
    CHECK(c.recv(rep) == 1 && rep.has_state());
    c.send(pair("zzz"));  // already paired: idempotent, and no second token
    CHECK(c.recv(rep) == 1 && rep.has_pair_reply() && rep.pair_reply().pairing_token().empty());
  }
  // The unit restarts: the computer is remembered, but only its token proves it. The window is closed.
  r.restart([&](sd::BridgeOptions& o) { o.engine_socket = eng.path; });
  CHECK(r.paired->list().size() == 1 && r.paired->list()[0].name == "Mateus's MacBook");
  {
    Client c2;
    CHECK(c2.connect_to(r.port()));
    CHECK(c2.hello(kAll, "mac-persist", 1, token).hello_ack().paired());
    c2.send(get_state());
    ws::v1::ServerMessage rep;
    CHECK(c2.recv(rep) == 1 && rep.has_state());
  }
  {  // the token presented in a SECOND Hello on an unpaired connection pairs it (how Studio does it: the device id is only known after the first HelloAck)
    Client c3;
    CHECK(c3.connect_to(r.port()));
    CHECK(!c3.hello(kAll, "mac-persist").hello_ack().paired());
    ws::v1::ClientMessage h2;
    h2.mutable_hello()->set_version(1);
    h2.mutable_hello()->set_pairing_token(token);
    c3.send(h2);
    ws::v1::ServerMessage a2;
    CHECK(c3.recv(a2) == 1 && a2.hello_ack().paired());
    c3.send(get_state());
    CHECK(c3.recv(a2) == 1 && a2.has_state());
    // and a wrong token in the second Hello does not, and spends the one retry
    Client c4;
    CHECK(c4.connect_to(r.port()));
    c4.hello(kAll, "mac-persist");
    h2.mutable_hello()->set_pairing_token(std::string(64, '1'));
    c4.send(h2);
    CHECK(c4.recv(a2) == 1 && !a2.hello_ack().paired());
    h2.mutable_hello()->set_pairing_token(token);
    c4.send(h2);
    CHECK(c4.recv(a2) == 1 && a2.error().code() == ws::v1::ERROR_CODE_INVALID_COMMAND);
  }
  const std::int64_t last = r.paired->list()[0].last_connected_unix_s;
  // The bypass attempts: a known id without a token, with a wrong token, with another computer's token, with the token's hash: all unpaired, and NOTHING is
  // written for them (last connected stays).
  for (const std::string& bad : {std::string(""), std::string(64, 'f'), std::string("not-a-token"), sd::sha256_hex(token)}) {
    Client x;
    CHECK(x.connect_to(r.port()));
    CHECK(!x.hello(kAll, "mac-persist", 1, bad).hello_ack().paired());
    x.send(get_state());
    ws::v1::ServerMessage rep;
    CHECK(x.recv(rep) == 1 && rep.error().code() == ws::v1::ERROR_CODE_NOT_PAIRED);
  }
  {  // the token of one computer does not pair another id
    Client x;
    CHECK(x.connect_to(r.port()));
    CHECK(!x.hello(kAll, "mac-other", 1, token).hello_ack().paired());
    ws::v1::ServerMessage rep;
    x.send(pair("424242"));
    CHECK(x.recv(rep) == 1 && rep.error().code() == ws::v1::ERROR_CODE_PAIRING_CLOSED && rep.error().details().at("reason") == "not_open");
  }
  CHECK(r.paired->list().size() == 1 && r.paired->list()[0].last_connected_unix_s == last || r.paired->list()[0].last_connected_unix_s >= last);
  // Pairing needs a well-formed client id (a malformed one is not even kept).
  Client c4;
  CHECK(c4.connect_to(r.port()));
  c4.hello(kAll, "bad id\nwith newline");
  r.window->open("424242");
  c4.send(pair("424242"));
  ws::v1::ServerMessage rep;
  CHECK(c4.recv(rep) == 1 && rep.error().code() == ws::v1::ERROR_CODE_INVALID_COMMAND);
  CHECK(r.paired->list().size() == 1);
  // A second computer pairs with a fresh window and gets its own token.
  Client c5;
  CHECK(c5.connect_to(r.port()));
  c5.hello(kAll, "mac-second");
  const std::string t2 = pair_with(c5, "424242");
  CHECK(sd::PairedStore::valid_token(t2) && t2 != token && r.paired->list().size() == 2);
}

void test_pairing_limits() {
  {  // five wrong codes close the window
    Rig r;
    r.window->open("424242");
    CHECK(r.start());
    Client c;
    CHECK(c.connect_to(r.port()));
    c.hello(kAll, "mac-w");
    ws::v1::ServerMessage rep;
    for (int i = 4; i >= 1; --i) {
      c.send(pair("999999"));
      CHECK(c.recv(rep) == 1 && rep.error().code() == ws::v1::ERROR_CODE_PAIRING_CODE_INVALID && rep.error().details().at("attempts_left") == std::to_string(i));
    }
    c.send(pair("999999"));
    CHECK(c.recv(rep) == 1 && rep.error().code() == ws::v1::ERROR_CODE_PAIRING_CLOSED && rep.error().details().at("reason") == "attempts");
    c.send(pair("424242"));  // the right code is refused now
    CHECK(c.recv(rep) == 1 && rep.error().code() == ws::v1::ERROR_CODE_PAIRING_CLOSED && rep.error().details().at("reason") == "attempts");
    CHECK(!r.paired->contains("mac-w"));
    r.window->open("424242");  // reopened (a new code window on the unit)
    c.send(pair("424242"));
    CHECK(c.recv(rep) == 1 && rep.has_pair_reply());
  }
  {  // expiry after 2 minutes (injected clock)
    Rig r;
    r.window->open("424242");
    CHECK(r.start());
    Client c;
    CHECK(c.connect_to(r.port()));
    c.hello(kAll, "mac-e");
    r.clock += sd::PairingWindow::kWindowMs + 1;
    c.send(pair("424242"));
    ws::v1::ServerMessage rep;
    CHECK(c.recv(rep) == 1 && rep.error().code() == ws::v1::ERROR_CODE_PAIRING_CLOSED && rep.error().details().at("reason") == "expired");
    CHECK(!r.paired->contains("mac-e"));
  }
}

void test_dev_bypass() {
  Rig r;
  CHECK(r.start([](sd::BridgeOptions& o) { o.dev_bypass_pairing = true; }));
  Client c;
  CHECK(c.connect_to(r.port()));
  auto ack = c.hello(kAll, "");
  CHECK(ack.hello_ack().paired() && ack.hello_ack().model() == "DEV HOST");
  c.send(get_state());  // paired: routed (no engine configured -> typed unavailable, not NOT_PAIRED)
  ws::v1::ServerMessage rep;
  CHECK(c.recv(rep) == 1 && rep.error().code() == ws::v1::ERROR_CODE_UPSTREAM_UNAVAILABLE);
}

void test_forwarding_and_request_ids() {
  Rig r;
  FakeUds eng;
  eng.start(r.tmp.path + "/e.sock", false);
  eng.handler = engine_handler;
  CHECK(r.start([&](sd::BridgeOptions& o) {
    o.engine_socket = eng.path;
    o.dev_bypass_pairing = true;
  }));
  Client a, b;
  CHECK(a.connect_to(r.port()) && b.connect_to(r.port()));
  a.hello(kAll, "a");
  b.hello(kAll, "b");
  ws::v1::ClientMessage m = get_state();
  m.set_request_id(42);
  a.send(m);
  m.set_request_id(42);  // the same id from another client: replies are not mixed up
  b.send(m);
  ws::v1::ServerMessage ra, rb;
  CHECK(a.recv(ra) == 1 && ra.request_id() == 42 && ra.has_state() && ra.state().current_scene() == 3 && ra.revision() == 7);
  CHECK(b.recv(rb) == 1 && rb.request_id() == 42 && rb.has_state());
  {
    std::lock_guard<std::mutex> lk(eng.mu);
    CHECK(eng.seen_ids.size() == 2);  // each client has its own upstream connection and its own upstream id space
    for (auto id : eng.seen_ids) CHECK(id != 42);  // the engine saw bridge-assigned ids, not the client's
  }
  // Error replies of the engine keep the client's id too.
  ws::v1::ClientMessage sm;
  sm.set_request_id(9001);
  sm.mutable_set_master_params();
  a.send(sm);
  CHECK(a.recv(ra) == 1 && ra.request_id() == 9001);
  // Many in flight come back correctly paired.
  for (std::uint64_t id = 100; id < 140; ++id) {
    ws::v1::ClientMessage x = get_state();
    x.set_request_id(id);
    a.send(x);
  }
  for (std::uint64_t id = 100; id < 140; ++id) CHECK(a.recv(ra) == 1 && ra.request_id() == id);
}

void test_push_relay_and_slow_client() {
  Rig r;
  FakeUds eng;
  eng.start(r.tmp.path + "/e.sock", false);
  eng.handler = engine_handler;
  CHECK(r.start([&](sd::BridgeOptions& o) {
    o.engine_socket = eng.path;
    o.dev_bypass_pairing = true;
    o.max_queued_bytes = 64 * 1024;
  }));
  Client a, slow;
  CHECK(a.connect_to(r.port()) && slow.connect_to(r.port()));
  a.hello(kAll, "a");
  slow.hello(kAll, "slow");
  ws::v1::ClientMessage sub;
  sub.mutable_subscribe()->set_enable(true);
  const auto sid = a.send(sub);
  ws::v1::ServerMessage m;
  CHECK(a.recv(m) == 1 && m.request_id() == sid && m.has_ack());
  CHECK(a.recv(m) == 1 && m.request_id() == 0 && m.has_state());  // the initial state push
  slow.send(sub);  // subscribes and then never reads
  // A change on the engine is pushed to the subscriber, request_id 0.
  ws::v1::ServerMessage push;
  push.mutable_state()->set_revision(8);
  push.mutable_state()->set_current_scene(5);
  eng.push_all(push);
  CHECK(a.recv(m) == 1 && m.request_id() == 0 && m.state().current_scene() == 5);
  // Flood the slow reader (a big pushed message, many times): it is dropped; the other client lives on.
  ws::v1::ServerMessage big;
  big.mutable_state()->set_live_set_name(std::string(30000, 'x'));
  std::atomic<bool> draining{true};
  std::thread drain([&] {  // `a` keeps reading
    ws::v1::ServerMessage d;
    while (draining.load()) a.recv(d, 50);
  });
  for (int i = 0; i < 400; ++i) eng.push_all(big);
  CHECK(wait_for([&] { return r.bridge->sessions() == 1; }, 8000));
  CHECK(r.bridge->slow_dropped() == 1);  // dropped for the output queue cap, not for any other reason
  draining.store(false);
  drain.join();
  a.send(get_state());
  bool got_state = false;
  for (int i = 0; i < 1000 && !got_state; ++i) {
    if (a.recv(m, 2000) != 1) break;
    got_state = m.request_id() != 0 && m.has_state();
  }
  CHECK(got_state);
}

void test_upstream_lost_and_down() {
  Rig r;
  FakeUds eng;
  eng.start(r.tmp.path + "/e.sock", false);
  eng.handler = engine_handler;
  CHECK(r.start([&](sd::BridgeOptions& o) {
    o.engine_socket = eng.path;
    o.dev_bypass_pairing = true;
  }));
  Client a;
  CHECK(a.connect_to(r.port()));
  a.hello(kAll, "a");
  a.send(get_state());
  ws::v1::ServerMessage m;
  CHECK(a.recv(m) == 1 && m.has_state());
  eng.kill_connections();  // the engine drops this client's upstream: typed error, then the client is closed so it reconnects and re-subscribes
  CHECK(a.recv(m) == 1 && m.has_error() && m.error().code() == ws::v1::ERROR_CODE_UPSTREAM_UNAVAILABLE && m.request_id() == 0);
  CHECK(a.closed());
  // The engine is gone altogether: a fresh paired client gets the typed error on its request and stays connected.
  Client b;
  CHECK(b.connect_to(r.port()));
  b.hello(kAll, "b");
  eng.stop.store(true);
  ::close(eng.lfd);
  eng.lfd = -1;
  ::unlink(eng.path.c_str());
  b.send(get_state());
  ws::v1::ClientMessage x = get_state();
  x.set_request_id(77);
  b.send(x);
  CHECK(b.recv(m) == 1 && m.error().code() == ws::v1::ERROR_CODE_UPSTREAM_UNAVAILABLE);
  CHECK(b.recv(m) == 1 && m.error().code() == ws::v1::ERROR_CODE_UPSTREAM_UNAVAILABLE && m.request_id() == 77);
}

void test_frames_and_limits() {
  {  // oversize and malformed frames close the connection and are counted
    Rig r;
    r.window->open("111111");
    CHECK(r.start());
    Client c;
    CHECK(c.connect_to(r.port()));
    c.hello(kAll, "a");
    const char big[4] = {0, 1, 0, 1};  // 65537
    c.send_raw(std::string(big, 4));
    ws::v1::ServerMessage m;
    CHECK(c.recv(m) == 1 && m.error().code() == ws::v1::ERROR_CODE_FRAME_TOO_LARGE && c.closed());
    CHECK(wait_for([&] { return r.bridge->oversize() == 1; }));
    Client d;
    CHECK(d.connect_to(r.port()));
    d.hello(kAll, "d");
    std::string f;
    put_frame(f, std::string("\xff\xff\xff\xff garbage", 12));
    d.send_raw(f);
    CHECK(d.recv(m) == 1 && m.error().code() == ws::v1::ERROR_CODE_MALFORMED_MESSAGE && d.closed());
    CHECK(wait_for([&] { return r.bridge->malformed() == 2; }));
    // Exactly the largest frame is accepted by the framing (then rejected as not a message: zeros are a valid empty message -> no command set).
    Client e;
    CHECK(e.connect_to(r.port()));
    e.hello(kAll, "e");
    std::string g;
    put_frame(g, std::string(65536, '\0'));
    e.send_raw(g);
    CHECK(e.recv(m) == 1 && m.error().code() != ws::v1::ERROR_CODE_FRAME_TOO_LARGE);
  }
  {  // max clients, handshake timeout, rate limit, idle timeout
    Rig r;
    CHECK(r.start([](sd::BridgeOptions& o) {
      o.max_clients = 3;
      o.handshake_timeout_ms = 300;
      o.idle_timeout_ms = 600;
      o.unpaired_idle_timeout_ms = 600;
      o.max_msgs_per_second = 5;
    }));
    std::vector<std::unique_ptr<Client>> cs;
    for (int i = 0; i < 3; ++i) {
      cs.push_back(std::make_unique<Client>());
      CHECK(cs.back()->connect_to(r.port()));
      CHECK(cs.back()->hello(kAll, "c" + std::to_string(i)).has_hello_ack());
    }
    Client over;
    CHECK(over.connect_to(r.port()));
    CHECK(over.closed());
    CHECK(r.bridge->rejected_over_capacity() >= 1);
    // rate limit: a burst above 5 per second is answered BUSY
    int busy = 0;
    for (int i = 0; i < 12; ++i) cs[0]->send(get_state());
    ws::v1::ServerMessage m;
    while (cs[0]->recv(m, 300) == 1)
      if (m.has_error() && m.error().code() == ws::v1::ERROR_CODE_BUSY) ++busy;
    CHECK(busy >= 5);
    // Idle clients are closed; a connection that never says Hello is closed by the handshake timeout.
    CHECK(wait_for([&] { return r.bridge->sessions() == 0; }, 5000));
    Client silent;
    CHECK(silent.connect_to(r.port()));
    CHECK(silent.closed(3000));
  }
}

void test_library_forwarding() {
  Rig r;
  FakeUds lib;
  lib.lib_reply = [](const ws::v1::LibraryClientMessage& m, ws::v1::LibraryServerMessage& a) {
    if (m.has_list_libraries()) {  // Story 9.11: the librarian's catalog rows carry the absolute import path
      auto* l = a.mutable_library_list();
      auto* i = l->add_libraries();
      i->set_library_id("lib-1");
      i->set_name("Studio Grand");
      i->set_source_path("/media/usb0/secret/Studio Grand");
      i->set_instrument_count(2);
      l->set_total(1);
      return true;
    }
    if (m.has_list_instruments()) {
      auto* l = a.mutable_instrument_list();
      l->set_library_id(m.list_instruments().library_id());
      for (int k = 0; k < 2; ++k) {
        auto* i = l->add_instruments();
        i->set_instrument_key("inst" + std::to_string(k));
        i->set_name("Grand " + std::to_string(k));
        i->set_category(1);
      }
      l->set_total(2);
      return true;
    }
    return false;
  };
  lib.start(r.tmp.path + "/l.sock", true);
  CHECK(r.start([&](sd::BridgeOptions& o) {
    o.librarian_socket = lib.path;
    o.dev_bypass_pairing = true;
  }));
  Client c;
  CHECK(c.connect_to(r.port()));
  c.hello(kAll, "a");
  ws::v1::LibraryClientMessage lm;
  lm.set_request_id(555);
  lm.mutable_get_favorites_and_recent();
  ws::v1::ClientMessage m;
  m.set_request_id(31);
  m.mutable_library()->set_payload(lm.SerializeAsString());
  c.send(m);
  ws::v1::ServerMessage rep;
  CHECK(c.recv(rep) == 1 && rep.request_id() == 31 && rep.has_library_reply());
  ws::v1::LibraryServerMessage ls;
  CHECK(ls.ParseFromString(rep.library_reply().payload()) && ls.request_id() == 31 && ls.has_ack());
  {
    std::lock_guard<std::mutex> l(lib.mu);
    CHECK(lib.seen_ids.size() == 1 && lib.seen_ids[0] != 555 && lib.seen_ids[0] != 31);
  }
  // Messages outside the allow-list never reach the librarian: hello, the engine reports, imports, preflights, install.
  {
    const std::size_t before = [&] { std::lock_guard<std::mutex> l(lib.mu); return lib.seen_ids.size(); }();
    auto try_msg = [&](ws::v1::LibraryClientMessage x) {
      ws::v1::ClientMessage w;
      w.mutable_library()->set_payload(x.SerializeAsString());
      c.send(w);
      ws::v1::ServerMessage e;
      CHECK(c.recv(e) == 1 && e.error().code() == ws::v1::ERROR_CODE_INVALID_COMMAND && e.error().details().at("reason") == "library_message_not_available");
    };
    ws::v1::LibraryClientMessage x;
    x.mutable_hello();
    try_msg(x);
    x.Clear(); x.mutable_report_active()->set_live_set_id("a"); try_msg(x);
    x.Clear(); x.mutable_live_set_deleted()->set_live_set_id("a"); try_msg(x);
    x.Clear(); x.mutable_reconcile(); try_msg(x);
    x.Clear(); x.mutable_set_playing(); try_msg(x);
    x.Clear(); x.mutable_flush_recent(); try_msg(x);
    x.Clear(); x.mutable_import_request()->set_source("/etc"); try_msg(x);
    x.Clear(); x.mutable_preflight_request()->set_source("/"); try_msg(x);
    x.Clear(); x.mutable_list_preflight_rows(); try_msg(x);
    x.Clear(); x.mutable_install_protected()->set_bundle_path("/x"); try_msg(x);
    x.Clear(); try_msg(x);  // no body at all
    std::lock_guard<std::mutex> l(lib.mu);
    CHECK(lib.seen_ids.size() == before);
  }
  // Allowed ones: each is forwarded and answered with the client's id; CancelPreflight's inner request_id is translated (unknown -> 0).
  {
    ws::v1::LibraryClientMessage x;
    x.mutable_list_libraries();
    ws::v1::ClientMessage w;
    w.set_request_id(900);
    w.mutable_library()->set_payload(x.SerializeAsString());
    c.send(w);
    ws::v1::ServerMessage e;
    CHECK(c.recv(e) == 1 && e.request_id() == 900 && e.has_library_reply());
    x.Clear();
    x.mutable_cancel_preflight()->set_request_id(900);  // the client's own id of the request above: it is no longer pending (answered), so it maps to 0
    w.set_request_id(901);
    w.mutable_library()->set_payload(x.SerializeAsString());
    c.send(w);
    CHECK(c.recv(e) == 1 && e.request_id() == 901 && e.has_library_reply());
  }
  // Story 9.11: the read-only listing Studio's timbre picker uses: libraries (the device path is removed), then a library's instruments, untouched.
  {
    ws::v1::LibraryClientMessage x;
    x.set_request_id(7);
    x.mutable_list_libraries()->set_limit(50);
    ws::v1::ClientMessage w;
    w.set_request_id(950);
    w.mutable_library()->set_payload(x.SerializeAsString());
    c.send(w);
    ws::v1::ServerMessage e;
    CHECK(c.recv(e) == 1 && e.request_id() == 950 && e.has_library_reply());
    ws::v1::LibraryServerMessage y;
    CHECK(y.ParseFromString(e.library_reply().payload()) && y.request_id() == 950 && y.has_library_list());
    CHECK(y.library_list().libraries_size() == 1 && y.library_list().libraries(0).library_id() == "lib-1" && y.library_list().libraries(0).name() == "Studio Grand");
    CHECK(y.library_list().libraries(0).source_path().empty());
    CHECK(e.library_reply().payload().find("secret") == std::string::npos);
    x.Clear();
    x.mutable_list_instruments()->set_library_id("lib-1");
    w.set_request_id(951);
    w.mutable_library()->set_payload(x.SerializeAsString());
    c.send(w);
    CHECK(c.recv(e) == 1 && e.request_id() == 951 && e.has_library_reply());
    CHECK(y.ParseFromString(e.library_reply().payload()) && y.has_instrument_list() && y.instrument_list().instruments_size() == 2 &&
          y.instrument_list().instruments(1).name() == "Grand 1" && y.instrument_list().instruments(1).category() == 1);
  }
  // Garbage payload and a missing librarian are typed.
  ws::v1::ClientMessage bad;
  bad.mutable_library()->set_payload("\xff\xff\xff");
  c.send(bad);
  CHECK(c.recv(rep) == 1 && rep.error().code() == ws::v1::ERROR_CODE_MALFORMED_MESSAGE);
  Rig r2;
  CHECK(r2.start([](sd::BridgeOptions& o) { o.dev_bypass_pairing = true; }));
  Client d;
  CHECK(d.connect_to(r2.port()));
  d.hello(kAll, "a");
  d.send(m);
  CHECK(d.recv(rep) == 1 && rep.error().code() == ws::v1::ERROR_CODE_UPSTREAM_UNAVAILABLE && rep.error().details().at("upstream") == "library");
}

void test_config_in_process_and_shared_service() {
  Rig r;
  CHECK(r.start([](sd::BridgeOptions& o) { o.dev_bypass_pairing = true; }));
  // The Unix socket server shares the service; pushes cross between the two servers.
  sd::ConfigServer uds(*r.svc, r.tmp.path + "/s.sock");
  uds.set_foreign_sink([&](std::vector<sd::Outbound> o) { r.bridge->post(std::move(o)); });
  // (the bridge was started before; its sink is set via a fresh bridge to follow the "set before start" rule)
  r.bridge.reset();
  r.bridge = std::make_unique<sd::BridgeServer>(*r.svc, *r.window, *r.paired, [] {
    sd::BridgeOptions bo;
    bo.dev_bypass_pairing = true;
    return bo;
  }());
  r.bridge->set_foreign_sink([&](std::vector<sd::Outbound> o) { uds.post(std::move(o)); });
  std::string err;
  CHECK(r.bridge->start(err) && uds.start(err));
  Client c;
  CHECK(c.connect_to(r.port()));
  c.hello(kAll, "a");
  ws::v1::ClientMessage get;
  get.set_request_id(11);
  get.mutable_get_config();
  c.send(get);
  ws::v1::ServerMessage m;
  CHECK(c.recv(m) == 1 && m.request_id() == 11 && m.has_config_reply() && m.config_reply().revision() == 0);
  ws::v1::ClientMessage sub;
  sub.set_request_id(12);
  sub.mutable_subscribe_config()->set_enable(true);
  c.send(sub);
  CHECK(c.recv(m) == 1 && m.request_id() == 12 && m.has_ack());
  CHECK(c.recv(m) == 1 && m.request_id() == 0 && m.has_config_reply());
  // A change made through the bridge is persisted and pushed back to this subscriber.
  ws::v1::ClientMessage set;
  set.set_request_id(13);
  set.mutable_set_config()->mutable_patch()->set_clock_source(ws::v1::CLOCK_SOURCE_DIN);
  c.send(set);
  CHECK(c.recv(m) == 1 && m.request_id() == 13 && m.config_reply().revision() == 1 && m.config_reply().config().clock_source() == ws::v1::CLOCK_SOURCE_DIN);
  CHECK(c.recv(m) == 1 && m.request_id() == 0 && m.config_reply().revision() == 1);
  // A change made on the Unix socket (sysdctl) reaches the bridge subscriber.
  velora::sysdctl::Connection ctl;
  CHECK(ctl.open(r.tmp.path + "/s.sock", err));
  ws::v1::ConfigPatch p;
  p.set_velocity_curve(ws::v1::VELOCITY_CURVE_HEAVY);
  ws::v1::ConfigReply cr;
  CHECK(ctl.set(p, cr, err) && cr.revision() == 2);
  CHECK(c.recv(m) == 1 && m.request_id() == 0 && m.config_reply().revision() == 2 && m.config_reply().config().velocity_curve() == ws::v1::VELOCITY_CURVE_HEAVY);
  // And the other way: a bridge change reaches a sysdctl subscriber.
  CHECK(ctl.subscribe(true, err));
  ws::v1::ServerMessage push;
  CHECK(ctl.next_push(push, 2000) && push.config_reply().revision() == 2);  // the current config at subscribe
  ws::v1::ClientMessage set2;
  set2.set_request_id(14);
  set2.mutable_set_config()->mutable_patch()->set_clock_source(ws::v1::CLOCK_SOURCE_USB);
  c.send(set2);
  CHECK(c.recv(m) == 1 && m.request_id() == 14);
  CHECK(ctl.next_push(push, 2000) && push.config_reply().revision() == 3);
  r.bridge->stop();
  uds.stop();
}

void test_unpaired_cap_and_pending_timeout() {
  {  // at most max_unpaired handshaken unpaired sessions; a paired one is not affected
    Rig r;
    CHECK(r.start([](sd::BridgeOptions& o) { o.max_unpaired = 2; }));
    std::vector<std::unique_ptr<Client>> cs;
    for (int i = 0; i < 2; ++i) {
      cs.push_back(std::make_unique<Client>());
      CHECK(cs.back()->connect_to(r.port()));
      CHECK(cs.back()->hello(kAll, "u" + std::to_string(i)).has_hello_ack());
    }
    Client over;
    CHECK(over.connect_to(r.port()));
    auto rep = over.hello(kAll, "u9");
    CHECK(rep.has_error() && rep.error().code() == ws::v1::ERROR_CODE_BUSY && rep.error().details().at("reason") == "unpaired_limit" && over.closed());
  }
  {  // a request the engine never answers is failed after pending_timeout_ms and its slot is freed
    Rig r;
    FakeUds eng;
    eng.start(r.tmp.path + "/e.sock", false);
    eng.handler = [](const std::string&, std::vector<std::string>&) {};  // never replies
    CHECK(r.start([&](sd::BridgeOptions& o) {
      o.engine_socket = eng.path;
      o.dev_bypass_pairing = true;
      o.pending_timeout_ms = 300;
      o.max_pending_per_upstream = 3;
    }));
    Client c;
    CHECK(c.connect_to(r.port()));
    c.hello(kAll, "a");
    ws::v1::ServerMessage m;
    for (std::uint64_t id = 10; id < 13; ++id) {
      ws::v1::ClientMessage x = get_state();
      x.set_request_id(id);
      c.send(x);
    }
    ws::v1::ClientMessage full = get_state();
    full.set_request_id(13);
    c.send(full);  // the 4th while 3 are pending: BUSY
    CHECK(c.recv(m) == 1 && m.request_id() == 13 && m.error().code() == ws::v1::ERROR_CODE_BUSY);
    std::set<std::uint64_t> failed;
    for (int i = 0; i < 3; ++i) {
      CHECK(c.recv(m, 3000) == 1 && m.error().code() == ws::v1::ERROR_CODE_UPSTREAM_UNAVAILABLE && m.error().details().at("reason") == "timeout");
      failed.insert(m.request_id());
    }
    CHECK(failed == (std::set<std::uint64_t>{10, 11, 12}));
    ws::v1::ClientMessage again = get_state();
    again.set_request_id(14);
    c.send(again);  // the slots are free again: it is forwarded (and will time out), not BUSY
    CHECK(c.recv(m, 3000) == 1 && m.request_id() == 14 && m.error().details().at("reason") == "timeout");
  }
  {  // the dev flags' port / no-entropy paths are in main(); the options here: an oversize engine reply to a request is a typed error, not silence
    Rig r;
    FakeUds eng;
    eng.start(r.tmp.path + "/e.sock", false);
    eng.handler = [](const std::string& payload, std::vector<std::string>& replies) {
      ws::v1::ClientMessage m;
      m.ParseFromString(payload);
      ws::v1::ServerMessage big;
      big.set_request_id(m.request_id());
      big.mutable_state()->set_live_set_name(std::string(70000, 'x'));
      // The fake writes whole frames; a frame above 65536 is also above the bridge's upstream cap, so it closes the upstream: typed error + close.
      replies.push_back(big.SerializeAsString());
    };
    CHECK(r.start([&](sd::BridgeOptions& o) {
      o.engine_socket = eng.path;
      o.dev_bypass_pairing = true;
    }));
    Client c;
    CHECK(c.connect_to(r.port()));
    c.hello(kAll, "a");
    c.send(get_state());
    ws::v1::ServerMessage m;
    CHECK(c.recv(m) == 1 && m.error().code() == ws::v1::ERROR_CODE_UPSTREAM_UNAVAILABLE);
  }
}

}  // namespace

// Story 9.8: GetLiveSetView is a paired READ through the "liveset.v1" feature, relayed to the engine with the client's request id; the table row names it as an
// engine route (never denied, never local), an unpaired client gets NOT_PAIRED (the table loop above) and a client that did not negotiate the feature is refused.
void test_live_set_view_is_a_paired_read() {
  const auto* rule = sd::BridgeServer::rule_for(ws::v1::ClientMessage::kGetLiveSetView);
  CHECK(rule != nullptr && std::string(rule->name) == "get_live_set_view" && std::string(rule->feature) == "liveset.v1");
  CHECK(rule && rule->route == sd::BridgeServer::Route::kEngine && !rule->unpaired_allowed);
  Rig r;
  FakeUds eng;
  eng.start(r.tmp.path + "/e.sock", false);
  eng.handler = engine_handler;
  CHECK(r.start([&](sd::BridgeOptions& o) {
    o.engine_socket = eng.path;
    o.dev_bypass_pairing = true;
  }));
  Client a;
  CHECK(a.connect_to(r.port()));
  a.hello(kAll, "mac-view");
  ws::v1::ClientMessage m;
  m.set_request_id(777);
  m.mutable_get_live_set_view()->set_live_set_id("abc");
  a.send(m);
  ws::v1::ServerMessage rep;
  CHECK(a.recv(rep) == 1 && rep.request_id() == 777 && rep.has_live_set_view_reply());
  CHECK(rep.live_set_view_reply().live_set_id() == "abc" && rep.live_set_view_reply().name() == "Fake view" && rep.live_set_view_reply().scenes_size() == 8);
  CHECK(eng.received.load() == 1);
  Client b;  // negotiated no liveset.v1
  CHECK(b.connect_to(r.port()));
  b.hello({"snapshots.v1"}, "mac-view-2");
  m.set_request_id(778);
  b.send(m);
  ws::v1::ServerMessage u;
  CHECK(b.recv(u) == 1 && u.request_id() == 778 && u.has_error() && u.error().code() == ws::v1::ERROR_CODE_UNSUPPORTED_FEATURE && u.error().details().at("feature") == "liveset.v1");
  CHECK(eng.received.load() == 1);  // nothing more reached the engine
}

// Story 9.9: every edit command the Part Editor sends passes the bridge for a paired client with ClientMessage.base_revision intact (the envelope field the engine judges since Story 9.10), and a message without it reaches the engine with 0 as before.
void test_part_editor_edits_carry_base_revision() {
  Rig r;
  FakeUds eng;
  eng.start(r.tmp.path + "/e.sock", false);
  eng.handler = engine_handler;
  r.window->open("515151");
  CHECK(r.start([&](sd::BridgeOptions& o) { o.engine_socket = eng.path; }));
  Client c;
  CHECK(c.connect_to(r.port()));
  c.hello(kAll, "mac-edit");
  CHECK(!pair_with(c, "515151").empty());
  g_seen_base_count = 0;
  auto edit = [&](ws::v1::ClientMessage m, std::uint64_t base) {
    m.set_request_id(900 + static_cast<std::uint64_t>(g_seen_base_count.load()));
    m.set_base_revision(base);
    const int before = g_seen_base_count.load();
    c.send(m);
    ws::v1::ServerMessage rep;
    CHECK(c.recv(rep) == 1 && rep.has_ack());
    CHECK(g_seen_base_count.load() == before + 1 && g_seen_base_revision.load() == base);
  };
  ws::v1::ClientMessage m;
  m.mutable_set_performance_param()->set_param_id(ws::v1::PARAM_DRAWBAR_8);
  edit(m, 11);
  m.Clear();
  m.mutable_set_send_levels()->set_part(1);
  edit(m, 12);
  m.Clear();
  m.mutable_set_part_output()->set_part(1);
  edit(m, 13);
  m.Clear();
  m.mutable_set_insert_chain()->add_slots()->set_type(ws::v1::INSERT_TYPE_ROTARY);
  edit(m, 14);
  m.Clear();
  m.mutable_set_part_volume()->set_volume(0.5f);
  edit(m, 15);
  m.Clear();
  m.mutable_set_master_params();
  edit(m, 16);
  m.Clear();
  m.mutable_set_assign_slot()->set_slot(1);
  edit(m, 17);
  m.Clear();
  m.mutable_clear_assign_slot()->set_slot(1);
  edit(m, (1ull << 40) + 5);  // a big revision survives the 64-bit field
  m.Clear();
  m.mutable_get_state();
  c.send(m);  // a message without base_revision: not counted
  ws::v1::ServerMessage rep;
  CHECK(c.recv(rep) == 1 && rep.has_state());
  CHECK(g_seen_base_count.load() == 8);
  // Story 9.10: the engine's STALE_REVISION (code 47, with both revisions in the details) reaches the paired client unchanged, the connection stays up and the next edit works.
  m.Clear();
  m.mutable_set_performance_param()->set_param_id(ws::v1::PARAM_DRAWBAR_8);
  m.set_request_id(990);
  m.set_base_revision(0xDEAD);
  c.send(m);
  CHECK(c.recv(rep) == 1 && rep.request_id() == 990 && rep.has_error() && rep.error().code() == ws::v1::ERROR_CODE_STALE_REVISION && rep.error().code() == 47);
  CHECK(rep.error().details().at("base_revision") == "57005" && rep.error().details().at("current_revision") == "9");
  m.set_request_id(991);
  m.set_base_revision(9);
  c.send(m);
  CHECK(c.recv(rep) == 1 && rep.request_id() == 991 && rep.has_ack());
}

int main() {
  ::signal(SIGPIPE, SIG_IGN);
  test_pairing_window_unit();
  test_paired_store();
  test_hello_and_negotiation();
  test_unpaired_and_unsupported();
  test_pairing_flow_and_persistence();
  test_pairing_limits();
  test_dev_bypass();
  test_forwarding_and_request_ids();
  test_push_relay_and_slow_client();
  test_upstream_lost_and_down();
  test_frames_and_limits();
  test_library_forwarding();
  test_config_in_process_and_shared_service();
  test_message_tables_are_total();
  test_live_set_view_is_a_paired_read();
  test_part_editor_edits_carry_base_revision();
  test_unpaired_cap_and_pending_timeout();
  std::printf(g_fail ? "sysd_bridge_tests: FAILED (%d)\n" : "sysd_bridge_tests: ok\n", g_fail);
  return g_fail ? 1 : 0;
}
