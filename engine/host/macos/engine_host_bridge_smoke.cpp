// End-to-end smoke of the sysd TCP bridge (Foundation: Velora Studio device connection) with the real binaries: `engine-host --no-audio` and `velora-sysd
// --bridge-port 0 --engine-socket ... --pairing-code ...`, and a TCP client speaking ws.v1 on 127.0.0.1: Hello, NOT_PAIRED before pairing, a wrong then the right
// code, GetState / Subscribe through to the real engine (request_id preserved, State relayed), a change made by a second client pushed to the first, config
// through the bridge, persistence across a bridge restart (paired computers remembered), and the bridge reconnecting clients after the engine restarts is the
// client's job (the unit drop is a typed error + close). The Studio Rust client runs against the same setup by hand (desktop/README.md).
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "ws_v1.pb.h"

static int g_fail = 0;
#define CHECK(c)                                               \
  do {                                                         \
    if (!(c)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

using Clock = std::chrono::steady_clock;

static std::string slurp(const std::string& p) {
  std::string s;
  if (FILE* f = std::fopen(p.c_str(), "r")) {
    char b[4096];
    std::size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    std::fclose(f);
  }
  return s;
}

static pid_t spawn(const std::vector<std::string>& args, const std::string& out_path) {
  const pid_t pid = ::fork();
  if (pid == 0) {
    const int o = ::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ::dup2(o, 1);
    ::dup2(o, 2);
    std::vector<char*> av;
    for (const std::string& a : args) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    ::execv(av[0], av.data());
    _exit(127);
  }
  return pid;
}

static void stop(pid_t& pid) {
  if (pid <= 0) return;
  ::kill(pid, SIGTERM);
  int st = 0;
  ::waitpid(pid, &st, 0);
  pid = -1;
}

static bool wait_for(const std::function<bool()>& pred, int timeout_ms = 20000) {
  const auto end = Clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!pred()) {
    if (Clock::now() > end) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return true;
}

struct Tcp {
  int fd = -1;
  std::string rx;
  std::uint64_t next = 1;
  bool open(int port) {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<std::uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    return ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
  }
  ~Tcp() {
    if (fd >= 0) ::close(fd);
  }
  std::uint64_t send(ws::v1::ClientMessage m) {
    if (!m.request_id()) m.set_request_id(next++);
    const std::string b = m.SerializeAsString();
    const std::uint32_t n = static_cast<std::uint32_t>(b.size());
    const char h[4] = {char(n >> 24), char(n >> 16), char(n >> 8), char(n)};
    std::string f(h, 4);
    f += b;
    std::size_t off = 0;
    while (off < f.size()) {
      const ssize_t w = ::send(fd, f.data() + off, f.size() - off, 0);
      if (w <= 0) break;
      off += static_cast<std::size_t>(w);
    }
    return m.request_id();
  }
  bool recv(ws::v1::ServerMessage& out, int timeout_ms = 5000) {
    const auto end = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
      if (rx.size() >= 4) {
        const std::uint32_t len = (std::uint8_t(rx[0]) << 24) | (std::uint8_t(rx[1]) << 16) | (std::uint8_t(rx[2]) << 8) | std::uint8_t(rx[3]);
        if (rx.size() >= 4 + len) {
          const bool ok = out.ParseFromArray(rx.data() + 4, static_cast<int>(len));
          rx.erase(0, 4 + len);
          return ok;
        }
      }
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count();
      if (left <= 0) return false;
      pollfd p{fd, POLLIN, 0};
      if (::poll(&p, 1, static_cast<int>(left)) <= 0) continue;
      char buf[65536];
      const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
      if (n <= 0) return false;
      rx.append(buf, static_cast<std::size_t>(n));
    }
  }
  ws::v1::ServerMessage hello(const std::string& id, const std::string& token = "") {
    ws::v1::ClientMessage m;
    m.mutable_hello()->set_version(1);
    m.mutable_hello()->set_client_id(id);
    m.mutable_hello()->set_pairing_token(token);
    m.mutable_hello()->set_client_name("bridge-smoke");
    for (const char* f : {"params.v1", "snapshots.v1", "subscribe.v1", "liveset.v1", "config.v1", "library.v1"}) m.mutable_hello()->add_features(f);
    send(m);
    ws::v1::ServerMessage r;
    recv(r);
    return r;
  }
};

static int bridge_port(const std::string& log) {
  const std::string key = "bridge ws.v1 on 127.0.0.1:";
  const std::size_t p = log.find(key);
  return p == std::string::npos ? 0 : std::atoi(log.c_str() + p + key.size());
}

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: engine_host_bridge_smoke ENGINE_HOST VELORA_SYSD\n");
    return 2;
  }
  const std::string engine = argv[1], sysd = argv[2];
  if (const char* root = std::getenv("VELORA_STUDIO_CARGO_TEST")) {
    (void)root;
    if (std::system("command -v cargo >/dev/null 2>&1") != 0) {
      std::printf("SKIPPED: cargo is not installed, so the Velora Studio Rust client cannot run against the bridge\n");
      return 77;  // ctest SKIP_RETURN_CODE
    }
  }
  char dir[] = "/tmp/vbs-XXXXXX";
  if (!mkdtemp(dir)) return 2;
  const std::string d = dir, esock = d + "/e.sock", ssock = d + "/s.sock", elog = d + "/engine.log", slog = d + "/sysd.log";
  pid_t epid = spawn({engine, "--no-audio", "--duration", "120", "--socket", esock}, elog);
  pid_t spid = -1;
  auto start_sysd = [&](int n) {
    const std::string log = d + "/sysd" + std::to_string(n) + ".log";
    spid = spawn({sysd, "--data-dir", d + "/data", "--socket", ssock, "--bridge-port", "0", "--engine-socket", esock, "--pairing-code", "123456"}, log);
    int port = 0;
    wait_for([&] { return (port = bridge_port(slurp(log))) != 0; });
    return port;
  };
  auto cleanup = [&] {
    if (g_fail) {
      for (const std::string& f : {elog, d + "/sysd1.log", d + "/sysd2.log"}) std::printf("---- %s ----\n%s\n", f.c_str(), slurp(f).c_str());
    }
    stop(spid);
    stop(epid);
    const std::string cmd = "rm -rf " + d;
    [[maybe_unused]] const int rc = std::system(cmd.c_str());
  };
  std::string token;
  const int port = start_sysd(1);
  CHECK(port > 0);
  CHECK(slurp(d + "/sysd1.log").find("WARNING: --pairing-code") != std::string::npos);  // the loud dev warning
  // The engine takes a moment to listen.
  CHECK(wait_for([&] {
    Tcp t;
    if (!t.open(port)) return false;
    auto a = t.hello("probe");
    return a.has_hello_ack();
  }));
  {
    Tcp c;
    CHECK(c.open(port));
    auto ack = c.hello("smoke-mac");
    CHECK(ack.has_hello_ack() && !ack.hello_ack().paired() && !ack.hello_ack().device_id().empty());
    ws::v1::ClientMessage gs;
    gs.mutable_get_state();
    c.send(gs);
    ws::v1::ServerMessage r;
    CHECK(c.recv(r) && r.error().code() == ws::v1::ERROR_CODE_NOT_PAIRED);
    ws::v1::ClientMessage pm;
    pm.mutable_pair_request()->set_code("000000");
    c.send(pm);
    CHECK(c.recv(r) && r.error().code() == ws::v1::ERROR_CODE_PAIRING_CODE_INVALID);
    pm.mutable_pair_request()->set_code("123456");
    pm.mutable_pair_request()->set_computer_name("Smoke Mac");
    c.send(pm);
    CHECK(c.recv(r) && r.has_pair_reply() && r.pair_reply().paired());
    token = r.pair_reply().pairing_token();
    CHECK(token.size() == 64);
    // GetState reaches the real engine; the reply carries our request_id.
    gs.set_request_id(77);
    c.send(gs);
    CHECK(c.recv(r) && r.request_id() == 77 && r.has_state() && r.state().parts_size() >= 8);
    const std::uint64_t base_rev = r.state().revision();
    // Subscribe: Ack with our id, then the current State pushed (id 0).
    ws::v1::ClientMessage sub;
    sub.set_request_id(78);
    sub.mutable_subscribe()->set_enable(true);
    c.send(sub);
    CHECK(c.recv(r) && r.request_id() == 78 && r.has_ack());
    CHECK(c.recv(r) && r.request_id() == 0 && r.has_state());
    // Another paired computer changes something; the subscriber gets the new State pushed.
    Tcp o;
    CHECK(o.open(port));
    CHECK(o.hello("smoke-mac", token).hello_ack().paired());  // the same computer with its pairing token: no code
    {
      Tcp thief;  // the same id without the token is NOT paired
      CHECK(thief.open(port) && !thief.hello("smoke-mac").hello_ack().paired());
    }
    ws::v1::ClientMessage chg;
    chg.mutable_set_send_levels()->set_part(1);
    chg.mutable_set_send_levels()->set_reverb(0.31f);
    chg.mutable_set_send_levels()->set_delay(0.12f);
    o.send(chg);
    ws::v1::ServerMessage orr;
    CHECK(o.recv(orr) && (orr.has_ack() || orr.has_state()));
    bool pushed = false;
    for (int i = 0; i < 20 && !pushed; ++i) {
      if (!c.recv(r, 3000)) break;
      pushed = r.request_id() == 0 && r.has_state() && r.state().revision() > base_rev;
    }
    CHECK(pushed);
    // Config is served in-process by sysd through the same door.
    ws::v1::ClientMessage gc;
    gc.set_request_id(90);
    gc.mutable_get_config();
    c.send(gc);
    // (a push may be queued before the reply)
    bool got_cfg = false;
    for (int i = 0; i < 5 && !got_cfg; ++i) {
      if (!c.recv(r, 3000)) break;
      got_cfg = r.request_id() == 90 && r.has_config_reply();
    }
    CHECK(got_cfg);
    ws::v1::ClientMessage lib;  // no librarian configured: typed
    lib.set_request_id(91);
    lib.mutable_library()->set_payload(std::string("\x32\x00", 2));  // LibraryClientMessage{get_favorites_and_recent}, field 6, empty
    c.send(lib);
    got_cfg = false;
    for (int i = 0; i < 5 && !got_cfg; ++i) {
      if (!c.recv(r, 3000)) break;
      got_cfg = r.request_id() == 91 && r.has_error() && r.error().code() == ws::v1::ERROR_CODE_UPSTREAM_UNAVAILABLE;
    }
    CHECK(got_cfg);
  }
  // Restart the bridge: the paired computer is remembered (no code), the window is open again with a fresh count.
  stop(spid);
  const int port2 = start_sysd(2);
  CHECK(port2 > 0);
  {
    Tcp c;
    CHECK(c.open(port2));
    CHECK(c.hello("smoke-mac", token).hello_ack().paired());
    ws::v1::ClientMessage gs;
    gs.set_request_id(5);
    gs.mutable_get_state();
    c.send(gs);
    ws::v1::ServerMessage r;
    CHECK(c.recv(r) && r.request_id() == 5 && r.has_state());
  }
  // With VELORA_STUDIO_CARGO_TEST=<repo root> (the ctest entry engine_host_bridge_studio_smoke sets it): the Velora Studio Rust client against this same real bridge and engine (needs cargo and its crates). It pairs with the code: the restarted bridge has a fresh window.
  if (const char* root = std::getenv("VELORA_STUDIO_CARGO_TEST")) {
    const std::string cmd = std::string("cd '") + root + "/desktop/src-tauri' && VELORA_TEST_BRIDGE=127.0.0.1:" + std::to_string(port2) +
                            " VELORA_TEST_PAIRING_CODE=123456 cargo test --offline real_bridge_round_trip -- --nocapture > " + d + "/studio.log 2>&1";
    const int rc = std::system(cmd.c_str());
    CHECK(rc == 0);
    CHECK(slurp(d + "/studio.log").find("status: Paired") != std::string::npos);
    if (rc != 0) std::printf("---- studio.log ----\n%s\n", slurp(d + "/studio.log").c_str());
  }
  // The engine goes away: a connected client is told (typed error, request_id 0) and closed; the bridge itself stays up.
  {
    Tcp c;
    CHECK(c.open(port2));
    CHECK(c.hello("smoke-mac", token).hello_ack().paired());
    ws::v1::ClientMessage gs;
    gs.set_request_id(6);
    gs.mutable_get_state();
    c.send(gs);
    ws::v1::ServerMessage r;
    CHECK(c.recv(r) && r.has_state());
    stop(epid);
    bool told = false;
    for (int i = 0; i < 5 && !told; ++i) {
      if (!c.recv(r, 5000)) break;
      told = r.has_error() && r.error().code() == ws::v1::ERROR_CODE_UPSTREAM_UNAVAILABLE;
    }
    CHECK(told);
    Tcp c2;
    CHECK(c2.open(port2));
    CHECK(c2.hello("smoke-mac", token).hello_ack().paired());
    c2.send(gs);
    CHECK(c2.recv(r) && r.error().code() == ws::v1::ERROR_CODE_UPSTREAM_UNAVAILABLE);
  }
  cleanup();
  std::printf(g_fail ? "engine_host_bridge_smoke: FAILED\n" : "engine_host_bridge_smoke: ok\n");
  return g_fail ? 1 : 0;
}
