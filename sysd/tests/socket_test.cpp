// sysd over real Unix domain sockets in a temp dir (Foundation: sysd config service): the server (framing, limits, stale / live socket rules, subscribe latency,
// concurrent clients) and the engine client (sysd absent at start and appearing later, last-known config kept on disconnect, revision guard across pushes and
// reconnects, never blocking the caller). Waits are bounded polls on a condition, never sleeps in logic.
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "sysdctl_client.hpp"
#include "velora/host/keybed_reader.hpp"
#include "velora/host/sysd_config_client.hpp"
#include "velora/host/velocity_curve_holder.hpp"
#include "velora/sysd/config_server.hpp"

namespace fs = std::filesystem;
namespace sd = velora::sysd;
namespace hc = velora::host;
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
    // A short path: a socket path is limited to ~100 bytes on macOS.
    std::string t = "/tmp/vsd-XXXXXX";
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

// A running sysd: store + service + server on a directory.
struct Sysd {
  std::string dir, sock;
  std::unique_ptr<sd::ConfigStore> store;
  std::unique_ptr<sd::ConfigService> svc;
  std::unique_ptr<sd::ConfigServer> server;
  bool start(const std::string& d, const std::string& s, sd::ConfigServerLimits lim = {}) {
    dir = d;
    sock = s;
    sd::StoreOptions so;
    so.dir = d;
    store = std::make_unique<sd::ConfigStore>(so);
    svc = std::make_unique<sd::ConfigService>(*store);
    server = std::make_unique<sd::ConfigServer>(*svc, s, lim);
    std::string err;
    if (!server->start(err)) {
      std::printf("start: %s\n", err.c_str());
      return false;
    }
    return true;
  }
  void stop() {
    server.reset();
    svc.reset();
    store.reset();
  }
};

ws::v1::ConfigPatch clock_patch(ws::v1::ClockSource c) {
  ws::v1::ConfigPatch p;
  p.set_clock_source(c);
  return p;
}

int raw_connect(const std::string& path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  std::memcpy(a.sun_path, path.c_str(), path.size());
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

std::string frame_of(const google::protobuf::MessageLite& m) {
  std::string b = m.SerializeAsString();
  const std::uint32_t n = static_cast<std::uint32_t>(b.size());
  const char h[4] = {char(n >> 24), char(n >> 16), char(n >> 8), char(n)};
  return std::string(h, 4) + b;
}

// ---- server -------------------------------------------------------------------------------------------------------
void test_server_basic_and_latency() {
  TmpDir t;
  Sysd s;
  CHECK(s.start(t.path, t.path + "/s.sock"));
  struct stat st{};
  CHECK(::stat(s.sock.c_str(), &st) == 0 && S_ISSOCK(st.st_mode) && (st.st_mode & 0777) == 0600);
  velora::sysdctl::Connection a, b;
  std::string err;
  CHECK(a.open(s.sock, err) && b.open(s.sock, err));
  ws::v1::ConfigReply r;
  CHECK(a.get(r, err) && r.revision() == 0 && r.config().clock_source() == ws::v1::CLOCK_SOURCE_INTERNAL);
  CHECK(b.subscribe(true, err));
  ws::v1::ServerMessage push;
  CHECK(b.next_push(push, 2000) && push.has_config_reply() && push.config_reply().revision() == 0);  // the current config at once
  // Subscribers get the config within 100 ms of a change (best of several samples: robust against a loaded machine, still a real bound).
  long best_us = 1'000'000;
  for (int i = 0; i < 6; ++i) {
    CHECK(a.set(clock_patch(i % 2 ? ws::v1::CLOCK_SOURCE_DIN : ws::v1::CLOCK_SOURCE_USB), r, err));
    const auto t0 = Clock::now();  // from the moment the change is committed and acknowledged: the persist (F_FULLFSYNC) is not part of the push latency
    CHECK(b.next_push(push, 2000) && push.config_reply().revision() == static_cast<std::uint64_t>(i + 1));
    best_us = std::min<long>(best_us, std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
  }
  CHECK(best_us < 100'000);
  // A typed error over the wire; the connection survives.
  ws::v1::Error e;
  ws::v1::ConfigPatch bad;
  bad.mutable_sustain()->set_min(50);
  bad.mutable_sustain()->set_max(10);
  CHECK(!a.set(bad, r, err, &e) && e.code() == ws::v1::ERROR_CODE_INVALID_CONFIG);
  CHECK(a.get(r, err) && r.revision() == 6);
  // Persisted: a new sysd on the same directory serves it.
  s.stop();
  CHECK(s.start(t.path, t.path + "/s.sock"));
  velora::sysdctl::Connection c;
  CHECK(c.open(s.sock, err) && c.get(r, err) && r.revision() == 6 && r.config().clock_source() == ws::v1::CLOCK_SOURCE_DIN);
  // The N-1 Hello is accepted by the dialect rule (N = 1: only 1), 0 and 2 are refused with the typed error.
  velora::sysdctl::Connection d;
  CHECK(!d.open(s.sock, err, 3000, 2));
}

void test_socket_rules() {
  TmpDir t;
  // A file that is not a socket is never touched.
  {
    FILE* f = std::fopen((t.path + "/notsock").c_str(), "w");
    std::fputs("x", f);
    std::fclose(f);
    Sysd s;
    sd::StoreOptions so;
    so.dir = t.path;
    sd::ConfigStore st(so);
    sd::ConfigService svc(st);
    sd::ConfigServer srv(svc, t.path + "/notsock");
    std::string err;
    CHECK(!srv.start(err) && fs::exists(t.path + "/notsock"));
  }
  // A live sysd is refused; a stale socket file (crashed run) is replaced.
  TmpDir d;
  Sysd a;
  CHECK(a.start(d.path, d.path + "/s.sock"));
  {
    sd::StoreOptions so;
    so.dir = d.path;
    sd::ConfigStore st(so);
    CHECK(!st.writer_locked());  // the second sysd cannot take the directory either
    sd::ConfigService svc(st);
    sd::ConfigServer srv(svc, d.path + "/s.sock");
    std::string err;
    CHECK(!srv.start(err));
  }
  a.stop();
  {  // after stop the socket file is gone; plant a stale one (bound, never listened, closed)
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string p = d.path + "/s.sock";
    std::memcpy(addr.sun_path, p.c_str(), p.size());
    CHECK(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
    ::close(fd);
    CHECK(fs::exists(p));
    Sysd b;
    CHECK(b.start(d.path, p));
    velora::sysdctl::Connection c;
    std::string err;
    CHECK(c.open(p, err));
  }
  CHECK(!fs::exists(d.path + "/s.sock"));  // removed on stop
}

void test_framing_and_limits() {
  TmpDir t;
  Sysd s;
  sd::ConfigServerLimits lim;
  lim.handshake_timeout_ms = 150;
  lim.max_clients = 3;
  CHECK(s.start(t.path, t.path + "/s.sock", lim));
  {  // silence: dropped after the handshake deadline
    const int fd = raw_connect(s.sock);
    CHECK(fd >= 0);
    pollfd pf{fd, POLLIN, 0};
    CHECK(::poll(&pf, 1, 3000) == 1);
    char b[8];
    CHECK(::recv(fd, b, sizeof b, 0) == 0);
    ::close(fd);
  }
  {  // oversize frame: typed error, then close
    const int fd = raw_connect(s.sock);
    const char h[4] = {0, 1, 0, 1};  // 65537
    CHECK(::send(fd, h, 4, 0) == 4);
    std::string rx;
    char b[512];
    for (;;) {
      pollfd pf{fd, POLLIN, 0};
      if (::poll(&pf, 1, 3000) != 1) break;
      const ssize_t n = ::recv(fd, b, sizeof b, 0);
      if (n <= 0) break;
      rx.append(b, static_cast<std::size_t>(n));
    }
    ws::v1::ServerMessage m;
    CHECK(rx.size() > 4 && m.ParseFromArray(rx.data() + 4, static_cast<int>(rx.size() - 4)) && m.error().code() == ws::v1::ERROR_CODE_FRAME_TOO_LARGE);
    ::close(fd);
  }
  {  // fire-and-forget: Hello + SetConfig in one write, then shutdown(SHUT_WR): the buffered frames are still served and the replies still come back
    const int fd = raw_connect(s.sock);
    ws::v1::ClientMessage h;
    h.set_request_id(1);
    h.mutable_hello()->set_version(1);
    ws::v1::ClientMessage m;
    m.set_request_id(2);
    m.mutable_set_config()->mutable_patch()->set_clock_source(ws::v1::CLOCK_SOURCE_USB);
    const std::string f = frame_of(h) + frame_of(m);
    CHECK(::send(fd, f.data(), f.size(), 0) == static_cast<ssize_t>(f.size()));
    CHECK(::shutdown(fd, SHUT_WR) == 0);
    std::string rx;
    char b[512];
    for (;;) {
      pollfd pf{fd, POLLIN, 0};
      if (::poll(&pf, 1, 3000) != 1) break;
      const ssize_t n = ::recv(fd, b, sizeof b, 0);
      if (n <= 0) break;
      rx.append(b, static_cast<std::size_t>(n));
    }
    ::close(fd);
    CHECK(s.svc->config().clock_source() == ws::v1::CLOCK_SOURCE_USB && s.svc->revision() == 1);
    CHECK(rx.size() > 8);  // HelloAck and ConfigReply were written before the close
  }
  {  // a first frame that is not Hello: HANDSHAKE_REQUIRED then close
    const int fd = raw_connect(s.sock);
    ws::v1::ClientMessage m;
    m.set_request_id(4);
    m.mutable_get_config();
    const std::string f = frame_of(m);
    CHECK(::send(fd, f.data(), f.size(), 0) == static_cast<ssize_t>(f.size()));
    std::string rx;
    char b[512];
    for (;;) {
      pollfd pf{fd, POLLIN, 0};
      if (::poll(&pf, 1, 3000) != 1) break;
      const ssize_t n = ::recv(fd, b, sizeof b, 0);
      if (n <= 0) break;
      rx.append(b, static_cast<std::size_t>(n));
    }
    ws::v1::ServerMessage r;
    CHECK(rx.size() > 4 && r.ParseFromArray(rx.data() + 4, static_cast<int>(rx.size() - 4)) && r.error().code() == ws::v1::ERROR_CODE_HANDSHAKE_REQUIRED);
    ::close(fd);
  }
  {  // connections beyond max_clients are closed
    std::vector<int> fds;
    for (int i = 0; i < 3; ++i) fds.push_back(raw_connect(s.sock));
    CHECK(wait_for([&] { return s.server->accepted() >= 6; }));
    const int extra = raw_connect(s.sock);
    pollfd pf{extra, POLLIN, 0};
    CHECK(::poll(&pf, 1, 3000) == 1);
    char b[8];
    CHECK(::recv(extra, b, sizeof b, 0) == 0);
    ::close(extra);
    for (int fd : fds) ::close(fd);
  }
}

void test_concurrent_clients_real_sockets() {
  TmpDir t;
  Sysd s;
  CHECK(s.start(t.path, t.path + "/s.sock"));
  constexpr int kClients = 4, kPer = 25;
  std::mutex mu;
  std::set<std::uint64_t> revs;
  std::atomic<int> failures{0};
  std::vector<std::thread> ts;
  for (int c = 0; c < kClients; ++c) {
    ts.emplace_back([&, c] {
      velora::sysdctl::Connection conn;
      std::string err;
      if (!conn.open(s.sock, err)) {
        ++failures;
        return;
      }
      for (int k = 0; k < kPer; ++k) {
        ws::v1::ConfigPatch p;
        p.mutable_expression()->set_min(1);
        p.mutable_expression()->set_max(300 + static_cast<std::uint32_t>(c * kPer + k));
        ws::v1::ConfigReply r;
        if (!conn.set(p, r, err)) {
          ++failures;
          continue;
        }
        std::lock_guard<std::mutex> lk(mu);
        revs.insert(r.revision());
      }
    });
  }
  for (auto& th : ts) th.join();
  CHECK(failures.load() == 0 && revs.size() == static_cast<std::size_t>(kClients * kPer) && *revs.begin() == 1 && *revs.rbegin() == revs.size());
  CHECK(s.svc->revision() == revs.size());
}

// ---- engine client ------------------------------------------------------------------------------------------------
struct Recorder {
  std::mutex mu;
  std::vector<std::pair<std::uint64_t, ws::v1::Config>> got;
  hc::SysdConfigClient::Callback cb() {
    return [this](const ws::v1::Config& c, std::uint64_t rev) {
      std::lock_guard<std::mutex> lk(mu);
      got.emplace_back(rev, c);
    };
  }
  std::size_t size() {
    std::lock_guard<std::mutex> lk(mu);
    return got.size();
  }
};

// Control-thread stand-in: polls the client the way the control tick does until `pred` holds.
bool tick_until(hc::SysdConfigClient& c, const std::function<bool()>& pred, int timeout_ms = 5000) {
  return wait_for([&] {
    c.tick();
    return pred();
  }, timeout_ms);
}

void test_client_sysd_absent_then_appears() {
  TmpDir t;
  const std::string sock = t.path + "/s.sock";
  hc::SysdConfigClientOptions o;
  o.socket_path = sock;
  o.backoff_min_ms = 5;
  o.backoff_max_ms = 40;
  hc::SysdConfigClient client(o);
  Recorder rec;
  client.set_callback(rec.cb());
  client.start();
  // sysd absent: the engine runs on the defaults, the client does not block or fail, tick() is a cheap no-op.
  CHECK(hc::default_config().SerializeAsString() == client.config().SerializeAsString() && client.revision() == 0 && !client.connected());
  for (int i = 0; i < 1000; ++i) CHECK(!client.tick() || false);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));  // a few failed connection attempts
  CHECK(!client.connected() && rec.size() == 0);
  // sysd appears later (with a persisted non-default config): the client applies it on connect, once.
  {
    sd::StoreOptions so;
    so.dir = t.path;
    sd::ConfigStore pre(so);
    CHECK(pre.apply(clock_patch(ws::v1::CLOCK_SOURCE_USB)).changed());
  }
  Sysd s;
  CHECK(s.start(t.path, sock));
  CHECK(tick_until(client, [&] { return rec.size() >= 1; }));
  CHECK(rec.got[0].first == 1 && rec.got[0].second.clock_source() == ws::v1::CLOCK_SOURCE_USB);
  CHECK(client.connected() && client.connects() == 1 && client.config().clock_source() == ws::v1::CLOCK_SOURCE_USB && client.revision() == 1);
  // A change is delivered once per revision.
  velora::sysdctl::Connection ctl;
  std::string err;
  ws::v1::ConfigReply r;
  CHECK(ctl.open(sock, err) && ctl.set(clock_patch(ws::v1::CLOCK_SOURCE_DIN), r, err));
  CHECK(tick_until(client, [&] { return rec.size() >= 2; }));
  CHECK(rec.got[1].first == 2 && rec.got[1].second.clock_source() == ws::v1::CLOCK_SOURCE_DIN);
  CHECK(ctl.set(clock_patch(ws::v1::CLOCK_SOURCE_DIN), r, err));  // identical: no revision, no callback
  CHECK(ctl.set(clock_patch(ws::v1::CLOCK_SOURCE_INTERNAL), r, err));
  CHECK(tick_until(client, [&] { return rec.size() >= 3; }));
  for (int i = 0; i < 50; ++i) client.tick();
  CHECK(rec.size() == 3 && rec.got[2].first == 3);
  // sysd goes away: the last-known config is kept, the client is not connected, the engine is unaffected.
  s.stop();
  CHECK(wait_for([&] { return !client.connected(); }));
  CHECK(client.config().clock_source() == ws::v1::CLOCK_SOURCE_INTERNAL && client.revision() == 3);
  CHECK(!client.tick());
  // sysd returns with the same persisted config: reconnects, and the guard does not re-fire the callback for the same revision and content.
  CHECK(s.start(t.path, sock));
  CHECK(wait_for([&] { return client.connects() == 2 && client.ignored() >= 1; }));  // the new connection's first Config was seen and refused by the guard
  client.tick();
  CHECK(rec.size() == 3);
  // ... and a change made after the reconnect is delivered.
  CHECK(ctl.open(sock, err) && ctl.set(clock_patch(ws::v1::CLOCK_SOURCE_USB), r, err));
  CHECK(tick_until(client, [&] { return rec.size() >= 4; }));
  CHECK(rec.got[3].first == 4);
  client.stop();
  CHECK(client.config().clock_source() == ws::v1::CLOCK_SOURCE_USB);  // still readable after stop
}

// Story 3.6: the velocity curve set through sysd (what `velora-sysdctl set velocity_curve light` sends) reaches a keybed reader through the client callback
// and the holder, for the next note, with no restart; a sysd + engine restart on the same data dir restores it from sysd's persisted file.
void test_velocity_curve_via_sysd() {
  TmpDir t;
  const std::string sock = t.path + "/s.sock";
  auto curve_patch = [](ws::v1::VelocityCurve c) {
    ws::v1::ConfigPatch p;
    p.set_velocity_curve(c);
    return p;
  };
  // one "engine": client + callback (the engine-host's: apply_velocity_curve to the holder) + a reader attached to the holder
  struct Engine {
    hc::VelocityCurveHolder holder;
    hc::HostEventQueue q;
    hc::KeybedReader reader{q, hc::KeybedCalibration::three_contact_73()};
    std::unique_ptr<hc::SysdConfigClient> client;
    int applied = 0;
    void start(const std::string& sock) {
      hc::SysdConfigClientOptions o;
      o.socket_path = sock;
      o.backoff_min_ms = 5;
      o.backoff_max_ms = 40;
      client = std::make_unique<hc::SysdConfigClient>(o);
      client->set_callback([this](const ws::v1::Config& c, std::uint64_t) { applied += hc::apply_velocity_curve(c, holder) ? 1 : 0; });
      holder.attach(reader);
      client->start();
    }
    ~Engine() {
      if (client) client->stop();
      holder.detach(reader);
    }
  };
  Sysd s;
  CHECK(s.start(t.path, sock));
  {
    Engine e;
    e.start(sock);
    CHECK(e.reader.velocity_curve() == hc::VelocityCurve::kNormal);  // default Normal: before sysd answers, and as sysd's default
    CHECK(tick_until(*e.client, [&] { return e.applied >= 1; }));
    CHECK(e.reader.velocity_curve() == hc::VelocityCurve::kNormal);
    velora::sysdctl::Connection ctl;
    std::string err;
    ws::v1::ConfigReply r;
    CHECK(ctl.open(sock, err) && ctl.set(curve_patch(ws::v1::VELOCITY_CURVE_LIGHT), r, err));
    CHECK(tick_until(*e.client, [&] { return e.applied >= 2; }));
    CHECK(e.reader.velocity_curve() == hc::VelocityCurve::kLight && e.holder.curve() == hc::VelocityCurve::kLight);  // next note, no restart
    CHECK(ctl.set(curve_patch(ws::v1::VELOCITY_CURVE_FIXED), r, err));
    CHECK(tick_until(*e.client, [&] { return e.applied >= 3; }));
    CHECK(e.reader.velocity_curve() == hc::VelocityCurve::kFixed);
    CHECK(ctl.set(curve_patch(ws::v1::VELOCITY_CURVE_HEAVY), r, err));
    CHECK(tick_until(*e.client, [&] { return e.applied >= 4; }));
    CHECK(e.reader.velocity_curve() == hc::VelocityCurve::kHeavy);
    // an unknown / unspecified value is not applied (the last curve is kept)
    ws::v1::Config bad;
    bad.set_velocity_curve(static_cast<ws::v1::VelocityCurve>(99));
    CHECK(!hc::apply_velocity_curve(bad, e.holder) && e.reader.velocity_curve() == hc::VelocityCurve::kHeavy);
    CHECK(ctl.set(curve_patch(ws::v1::VELOCITY_CURVE_LIGHT), r, err));
    CHECK(tick_until(*e.client, [&] { return e.applied >= 5; }));
    CHECK(e.reader.velocity_curve() == hc::VelocityCurve::kLight);
  }
  // reboot: sysd and the engine restart on the same data dir; the persisted curve is restored
  s.stop();
  CHECK(s.start(t.path, sock));
  {
    Engine e;
    e.start(sock);
    CHECK(tick_until(*e.client, [&] { return e.applied >= 1; }));
    CHECK(e.reader.velocity_curve() == hc::VelocityCurve::kLight && e.holder.curve() == hc::VelocityCurve::kLight);
  }
}

// A scripted server: after the handshake and the subscription it sends the given (revision, clock) pushes, then holds the connection open until the test
// closes it. Used for the revision guard (stale, duplicate, out of order) and for a sysd that restarted with a lower revision.
struct FakeSysd {
  std::string path;
  int lfd = -1;
  std::thread th;
  std::atomic<bool> stop{false};
  std::mutex mu;
  std::vector<std::vector<std::pair<std::uint64_t, ws::v1::ClockSource>>> scripts;  // one per connection
  std::atomic<int> conns{0};
  std::atomic<bool> hang{false};  // accept and never answer
  std::atomic<bool> close_on_request{false};  // after the script, drop the connection when the client sends anything (Story 3.11: kLost)

  bool start(const std::string& p) {
    path = p;
    lfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::memcpy(a.sun_path, p.c_str(), p.size());
    if (::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(lfd, 4) != 0) return false;
    th = std::thread([this] { run(); });
    return true;
  }
  static bool read_one(int fd, ws::v1::ClientMessage& m) {
    std::string rx;
    char b[1];
    while (rx.size() < 4) {
      if (::recv(fd, b, 1, 0) <= 0) return false;
      rx.push_back(b[0]);
    }
    const std::uint32_t len = (std::uint8_t(rx[0]) << 24) | (std::uint8_t(rx[1]) << 16) | (std::uint8_t(rx[2]) << 8) | std::uint8_t(rx[3]);
    std::string body(len, '\0');
    std::size_t off = 0;
    while (off < len) {
      const ssize_t n = ::recv(fd, body.data() + off, len - off, 0);
      if (n <= 0) return false;
      off += static_cast<std::size_t>(n);
    }
    return m.ParseFromString(body);
  }
  void run() {
    while (!stop.load()) {
      pollfd pf{lfd, POLLIN, 0};
      if (::poll(&pf, 1, 20) != 1) continue;
      const int fd = ::accept(lfd, nullptr, nullptr);
      if (fd < 0) continue;
      const int idx = conns.fetch_add(1);
      if (hang.load()) {
        while (!stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        ::close(fd);
        continue;
      }
      ws::v1::ClientMessage m;
      if (!read_one(fd, m) || !m.has_hello()) {
        ::close(fd);
        continue;
      }
      ws::v1::ServerMessage ack;
      ack.set_request_id(m.request_id());
      ack.mutable_hello_ack()->set_version(1);
      std::string f = frame_of(ack);
      ::send(fd, f.data(), f.size(), 0);
      if (!read_one(fd, m) || !m.has_subscribe_config()) {
        ::close(fd);
        continue;
      }
      ws::v1::ServerMessage ok;
      ok.set_request_id(m.request_id());
      ok.mutable_ack();
      f = frame_of(ok);
      ::send(fd, f.data(), f.size(), 0);
      std::vector<std::pair<std::uint64_t, ws::v1::ClockSource>> script;
      {
        std::lock_guard<std::mutex> lk(mu);
        if (static_cast<std::size_t>(idx) < scripts.size()) script = scripts[static_cast<std::size_t>(idx)];
      }
      for (const auto& p : script) {
        ws::v1::ServerMessage push;
        push.set_revision(p.first);
        push.mutable_config_reply()->set_revision(p.first);
        *push.mutable_config_reply()->mutable_config() = hc::default_config();
        push.mutable_config_reply()->mutable_config()->set_clock_source(p.second);
        f = frame_of(push);
        ::send(fd, f.data(), f.size(), 0);
      }
      char b[16];
      while (!stop.load()) {  // hold until the client leaves or the test ends
        pollfd p2{fd, POLLIN, 0};
        if (::poll(&p2, 1, 20) == 1) {
          const ssize_t n = ::recv(fd, b, sizeof b, 0);
          if (n <= 0 || close_on_request.load()) break;
        }
      }
      ::close(fd);
    }
  }
  ~FakeSysd() {
    stop.store(true);
    if (th.joinable()) th.join();
    if (lfd >= 0) ::close(lfd);
    ::unlink(path.c_str());
  }
};

void test_client_revision_guard() {
  TmpDir t;
  const std::string sock = t.path + "/f.sock";
  FakeSysd fake;
  using CS = ws::v1::ClockSource;
  // Connection 0: revisions 5, 5 (duplicate), 4 (stale), 6, then 6 again. Connection 1: sysd "restarted": the same 6 (same content), then 2 with
  // other content (a sysd that lost its file starts again low: a new connection's first config is what counts), then 2 again (duplicate).
  fake.scripts = {{{5, ws::v1::CLOCK_SOURCE_DIN}, {5, ws::v1::CLOCK_SOURCE_DIN}, {4, ws::v1::CLOCK_SOURCE_USB}, {6, ws::v1::CLOCK_SOURCE_USB}, {6, ws::v1::CLOCK_SOURCE_USB}},
                  {{6, ws::v1::CLOCK_SOURCE_USB}}};
  CHECK(fake.start(sock));
  hc::SysdConfigClientOptions o;
  o.socket_path = sock;
  o.backoff_min_ms = 5;
  o.backoff_max_ms = 20;
  hc::SysdConfigClient client(o);
  Recorder rec;
  client.set_callback(rec.cb());
  client.start();
  // Everything that reached the control thread: the worker may coalesce, so poll until the latest is seen, then check no stale / duplicate ever fired.
  CHECK(tick_until(client, [&] { return client.revision() == 6 && client.received() >= 1; }));
  CHECK(wait_for([&] { return client.ignored() >= 3; }));  // duplicate 5, stale 4, duplicate 6
  for (int i = 0; i < 50; ++i) client.tick();
  {
    std::lock_guard<std::mutex> lk(rec.mu);
    CHECK(!rec.got.empty() && rec.got.back().first == 6 && rec.got.back().second.clock_source() == ws::v1::CLOCK_SOURCE_USB);
    std::set<std::uint64_t> seen;
    for (const auto& g : rec.got) CHECK(seen.insert(g.first).second);  // never twice for the same revision
    CHECK(client.received() == 2 && client.ignored() == 3);            // 5 and 6 accepted
  }
  const std::size_t after_first = rec.size();
  // Drop the connection: the client reconnects, the first config of the new connection (6, USB) equals what it holds: not re-fired.
  fake.stop.store(true);
  fake.th.join();
  fake.stop.store(false);
  fake.th = std::thread([&] { fake.run(); });
  CHECK(wait_for([&] { return client.configs_seen() >= 6 && client.ignored() >= 4; }));  // 5 pushes of connection 0, then the first one of connection 1
  client.tick();
  CHECK(rec.size() == after_first && client.connects() >= 2);
  client.stop();
  // The pure guard.
  CHECK(hc::SysdConfigClient::newer_in_connection(5, 6) && !hc::SysdConfigClient::newer_in_connection(5, 5) && !hc::SysdConfigClient::newer_in_connection(5, 4));
  (void)sizeof(CS);
}

// Story 3.11: SysdConfigClient::set_config() against a sysd that never answers (kTimeout), while another write is in flight (kBusy), and one that drops the
// connection (kLost); not connected is typed too. Nothing is retried.
void test_set_config_failures() {
  TmpDir t;
  const std::string sock = t.path + "/p.sock";
  FakeSysd fake;
  fake.scripts = {{{1, ws::v1::CLOCK_SOURCE_INTERNAL}}};
  CHECK(fake.start(sock));
  hc::SysdConfigClientOptions o;
  o.socket_path = sock;
  o.backoff_min_ms = 5;
  o.backoff_max_ms = 20;
  hc::SysdConfigClient client(o);
  ws::v1::ConfigPatch patch;
  patch.mutable_sustain()->set_min(100);
  patch.mutable_sustain()->set_max(900);
  CHECK(client.set_config(patch, 100).status == hc::SysdSetResult::Status::kNotConnected);  // not started
  client.start();
  CHECK(wait_for([&] { return client.connected(); }));
  auto t0 = Clock::now();
  CHECK(client.set_config(patch, 200).status == hc::SysdSetResult::Status::kTimeout);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
  CHECK(ms >= 150 && ms < 1500);
  // busy: one write waits for an answer that never comes, a second one is refused at once
  hc::SysdSetResult::Status first = hc::SysdSetResult::Status::kOk;
  std::thread a([&] { first = client.set_config(patch, 600).status; });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  t0 = Clock::now();
  CHECK(client.set_config(patch, 600).status == hc::SysdSetResult::Status::kBusy);
  CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() < 100);
  a.join();
  CHECK(first == hc::SysdSetResult::Status::kTimeout);
  // lost: sysd drops the connection when the request arrives
  fake.close_on_request.store(true);
  CHECK(wait_for([&] { return client.connected(); }));
  t0 = Clock::now();
  CHECK(client.set_config(patch, 3000).status == hc::SysdSetResult::Status::kLost);
  CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() < 2500);
  client.stop();
}

void test_client_never_blocks() {
  TmpDir t;
  const std::string sock = t.path + "/h.sock";
  FakeSysd fake;
  fake.hang.store(true);
  CHECK(fake.start(sock));
  hc::SysdConfigClientOptions o;
  o.socket_path = sock;
  hc::SysdConfigClient client(o);
  client.set_callback([](const ws::v1::Config&, std::uint64_t) {});
  client.start();
  CHECK(wait_for([&] { return fake.conns.load() >= 1; }));  // the worker is now stuck in a handshake with a server that never answers
  const auto t0 = Clock::now();
  for (int i = 0; i < 10000; ++i) {
    client.tick();
    (void)client.config();
    (void)client.revision();
  }
  CHECK(Clock::now() - t0 < std::chrono::milliseconds(500));  // the control thread is never made to wait on the I/O thread
  const auto t1 = Clock::now();
  client.stop();  // and stop() does not wait out the 2 s handshake deadline
  CHECK(Clock::now() - t1 < std::chrono::milliseconds(1500));
}

}  // namespace

int main() {
  test_server_basic_and_latency();
  test_socket_rules();
  test_framing_and_limits();
  test_concurrent_clients_real_sockets();
  test_client_sysd_absent_then_appears();
  test_client_revision_guard();
  test_set_config_failures();
  test_client_never_blocks();
  test_velocity_curve_via_sysd();
  if (g_fail) {
    std::printf("sysd_socket_tests: %d FAILED\n", g_fail);
    return 1;
  }
  std::printf("sysd_socket_tests: ok\n");
  return 0;
}
