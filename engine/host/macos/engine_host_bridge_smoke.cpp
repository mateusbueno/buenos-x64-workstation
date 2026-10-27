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
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
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
  if (argc != 3 && argc != 5) {
    std::fprintf(stderr, "usage: engine_host_bridge_smoke ENGINE_HOST VELORA_SYSD [LIBRARIAN_SERVICE LIBRARIAN_CLI]\n");
    return 2;
  }
  const std::string engine = argv[1], sysd = argv[2];
  // Story 9.11 (optional): a real librarian-service over two tiny SFZ libraries imported with the librarian CLI, reached through the bridge's library allow-list, so Studio's
  // timbre picker and its assignment run against the real librarian, bridge and engine.
  const std::string librarian = argc == 5 ? argv[3] : "", librarian_cli = argc == 5 ? argv[4] : "";
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
  // Story 9.8: the engine has a data directory (so it can create a Live Set and answer GetLiveSetView, the read-only view Velora Studio draws).
  [[maybe_unused]] const int mk1 = ::mkdir((d + "/edata").c_str(), 0700), mk2 = ::mkdir((d + "/lib").c_str(), 0700);
  const std::string lsock = d + "/l.sock";
  if (!librarian_cli.empty()) {
    [[maybe_unused]] const int mk = ::mkdir((d + "/src").c_str(), 0700);
    for (const char* name : {"Smoke Grand", "Smoke Strings"}) {  // one SFZ with one 300-frame mono 16-bit WAV each: one library, one timbre
      std::string pcm;
      std::uint32_t x = static_cast<std::uint32_t>(std::strlen(name)) * 2654435761u + 12345u;
      for (int i = 0; i < 300; ++i) {
        x = x * 1664525u + 1013904223u;
        const std::int16_t v = static_cast<std::int16_t>((x >> 16) & 0x3fff) - 0x2000;
        pcm.push_back(static_cast<char>(v & 0xff));
        pcm.push_back(static_cast<char>((v >> 8) & 0xff));
      }
      auto le32 = [](std::uint32_t v) { return std::string{char(v), char(v >> 8), char(v >> 16), char(v >> 24)}; };
      auto le16 = [](std::uint32_t v) { return std::string{char(v), char(v >> 8)}; };
      const std::string wav = "RIFF" + le32(static_cast<std::uint32_t>(36 + pcm.size())) + "WAVE" + "fmt " + le32(16) + le16(1) + le16(1) + le32(48000) + le32(96000) + le16(2) + le16(16) +
                              "data" + le32(static_cast<std::uint32_t>(pcm.size())) + pcm;
      const std::string base = d + "/src/" + name;
      if (FILE* f = std::fopen((base + "_s.wav").c_str(), "wb")) { std::fwrite(wav.data(), 1, wav.size(), f); std::fclose(f); }
      if (FILE* f = std::fopen((base + ".sfz").c_str(), "w")) { std::fprintf(f, "<region> sample=%s_s.wav key=60 lokey=48 hikey=72\n", name); std::fclose(f); }
      const std::string imp = "'" + librarian_cli + "' import '" + base + ".sfz' --root '" + d + "/lib' --category piano >> '" + d + "/import.log' 2>&1";
      if (std::system(imp.c_str()) != 0) std::printf("FAIL: librarian import of %s\n%s\n", name, slurp(d + "/import.log").c_str()), ++g_fail;
    }
  }
  pid_t epid = spawn({engine, "--no-audio", "--duration", "120", "--socket", esock, "--data-dir", d + "/edata", "--library-root", d + "/lib"}, elog);
  pid_t lpid = -1;
  if (!librarian.empty()) lpid = spawn({librarian, "--data", d + "/lib", "--socket", lsock}, d + "/librarian.log");
  pid_t spid = -1;
  auto start_sysd = [&](int n) {
    const std::string log = d + "/sysd" + std::to_string(n) + ".log";
    std::vector<std::string> args = {sysd, "--data-dir", d + "/data", "--socket", ssock, "--bridge-port", "0", "--engine-socket", esock, "--pairing-code", "123456"};
    if (n >= 2 && lpid > 0) {  // the first bridge runs without a librarian (its library request is a typed UPSTREAM_UNAVAILABLE, checked below)
      args.push_back("--librarian-socket");
      args.push_back(lsock);
    }
    spid = spawn(args, log);
    int port = 0;
    wait_for([&] { return (port = bridge_port(slurp(log))) != 0; });
    return port;
  };
  auto cleanup = [&] {
    if (g_fail) {
      for (const std::string& f : {elog, d + "/sysd1.log", d + "/sysd2.log", d + "/librarian.log"}) std::printf("---- %s ----\n%s\n", f.c_str(), slurp(f).c_str());
    }
    stop(spid);
    stop(lpid);
    stop(epid);
    const std::string cmd = "rm -rf " + d;
    [[maybe_unused]] const int rc = std::system(cmd.c_str());
  };
  std::string token;
  const int port = start_sysd(1);
  CHECK(port > 0);
  CHECK(slurp(d + "/sysd1.log").find("WARNING: --pairing-code") != std::string::npos);  // the loud dev warning
  // The engine takes a moment to listen (longer with a data directory: it makes its first Live Set), so wait for its own socket before anything is sent through the bridge.
  CHECK(wait_for(
      [&] {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        std::strncpy(a.sun_path, esock.c_str(), sizeof a.sun_path - 1);
        const bool up = ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
        ::close(fd);
        return up;
      },
      60000));
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
    std::uint64_t base_rev = r.state().revision();
    {  // Story 9.8: GetLiveSetView through the bridge to the real engine: a Live Set made over the same door, then read back, without moving anything.
      ws::v1::ClientMessage cl;
      cl.set_request_id(80);
      cl.mutable_create_live_set()->set_name("Smoke Set");
      c.send(cl);
      CHECK(c.recv(r) && r.request_id() == 80 && r.has_created_reply());
      const std::string id = r.created_reply().id();
      gs.set_request_id(81);
      c.send(gs);
      CHECK(c.recv(r) && r.request_id() == 81 && r.has_state());
      const std::uint64_t rev_before = r.state().revision();
      ws::v1::ClientMessage gv;
      gv.set_request_id(82);
      gv.mutable_get_live_set_view()->set_live_set_id(id);
      c.send(gv);
      CHECK(c.recv(r) && r.request_id() == 82 && r.has_live_set_view_reply());
      const auto& v = r.live_set_view_reply();
      const bool view_dirty = v.dirty();
      CHECK(v.live_set_id() == id && v.name() == "Smoke Set" && !v.dirty() && !v.sounding() && v.scenes_size() == 8 && v.scene_count() == 1 && v.parts_size() == 8);
      CHECK(!v.scenes(0).empty() && v.scenes(1).empty());
      bool all_empty = true;
      for (const auto& p : v.parts()) all_empty = all_empty && p.empty() && p.on() && !p.missing_library() && p.timbre_name().empty();
      CHECK(all_empty);
      gv.set_request_id(83);
      gv.mutable_get_live_set_view()->set_live_set_id("00000000-0000-4000-8000-000000000000");
      c.send(gv);
      CHECK(c.recv(r) && r.request_id() == 83 && r.has_error() && r.error().code() == ws::v1::ERROR_CODE_NO_SUCH_LIVE_SET);
      gs.set_request_id(84);
      c.send(gs);
      CHECK(c.recv(r) && r.request_id() == 84 && r.has_state() && r.state().revision() == rev_before);  // read-only: the revision did not move
      CHECK(r.state().live_set_id().empty() || r.state().live_set_id() != id);                           // and the unit does not sound it
      base_rev = r.state().revision();
      CHECK(!r.state().edits().current_dirty() && !view_dirty);  // a fresh unit shows no unsaved diamond (item: Studio displays it)
    }
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
                            " VELORA_TEST_PAIRING_CODE=123456 " + std::string(librarian.empty() ? "" : "VELORA_TEST_TIMBRES=1 ") + "cargo test --offline real_bridge_round_trip -- --nocapture > " + d + "/studio.log 2>&1";
    const int rc = std::system(cmd.c_str());
    CHECK(rc == 0);
    CHECK(slurp(d + "/studio.log").find("status: Paired") != std::string::npos);
    CHECK(slurp(d + "/studio.log").find("view of \"Smoke Set\": 8 parts") != std::string::npos);  // Story 9.8: the Rust client read the Live Set created above through GetLiveSetView
    CHECK(slurp(d + "/studio.log").find("real_bridge_setlists_round_trip: ok") != std::string::npos);  // Story 9.13: the Rust client ran the Setlists view's commands and read them back
    if (!librarian.empty()) CHECK(slurp(d + "/studio.log").find("real_timbre_flow: ok") != std::string::npos);  // Story 9.11: the picker's list and the assignment through the real librarian, bridge and engine
    CHECK(slurp(d + "/studio.log").find("real_part_edit_flow: ok") != std::string::npos);  // Story 9.9: the Rust client sent the Part Editor's edits (with base_revision) and the State showed the structural ones
    if (rc != 0 || g_fail) std::printf("---- studio.log ----\n%s\n", slurp(d + "/studio.log").c_str());
  }
  // Story 9.13: the Setlists view's commands through the bridge to the real engine, then an engine restart on the same data directory: everything persisted exactly as the
  // device persists it. (The Rust client above did the same with the page's JSON; this part does not need cargo, so the persistence check never goes vacuous.)
  {
    Tcp c;
    CHECK(c.open(port2));
    CHECK(c.hello("smoke-mac", token).hello_ack().paired());
    auto call = [&](ws::v1::ClientMessage m) {
      const std::uint64_t id = c.send(std::move(m));
      ws::v1::ServerMessage r;
      for (int i = 0; i < 10; ++i) {
        if (!c.recv(r, 5000)) break;
        if (r.request_id() == id) return r;
      }
      return ws::v1::ServerMessage();
    };
    auto create = [&](const char* kind, const std::string& name) {
      ws::v1::ClientMessage m;
      if (std::string(kind) == "setlist") m.mutable_create_setlist()->set_name(name);
      else if (std::string(kind) == "live_set") m.mutable_create_live_set()->set_name(name);
      else m.mutable_create_tag()->set_name(name);
      auto r = call(m);
      CHECK(r.has_created_reply() && !r.created_reply().id().empty());
      return r.created_reply().id();
    };
    auto expect_ack = [&](ws::v1::ClientMessage m) {
      auto r = call(std::move(m));
      CHECK(r.has_ack());
    };
    const std::string la = create("live_set", "Persist A"), lb = create("live_set", "Persist B"), lc = create("live_set", "Persist C");
    const std::string s1 = create("setlist", "Persist draft"), s2 = create("setlist", "Persist second");
    {
      ws::v1::ClientMessage m;
      m.mutable_rename_setlist()->set_id(s1);
      m.mutable_rename_setlist()->set_name("Persist Show");
      expect_ack(m);
    }
    for (const std::string& id : {la, lb, lc}) {
      ws::v1::ClientMessage m;
      m.mutable_add_live_set_to_setlist()->set_setlist_id(s1);
      m.mutable_add_live_set_to_setlist()->set_live_set_id(id);
      expect_ack(m);
    }
    {  // reorder: the full permutation C, A, B (a partial one is refused)
      ws::v1::ClientMessage bad;
      bad.mutable_reorder_live_sets_in_setlist()->set_setlist_id(s1);
      bad.mutable_reorder_live_sets_in_setlist()->add_live_set_ids(lc);
      auto r = call(bad);
      CHECK(r.has_error() && r.error().code() == ws::v1::ERROR_CODE_INVALID_ORDER);
      ws::v1::ClientMessage m;
      m.mutable_reorder_live_sets_in_setlist()->set_setlist_id(s1);
      for (const std::string& id : {lc, la, lb}) m.mutable_reorder_live_sets_in_setlist()->add_live_set_ids(id);
      expect_ack(m);
    }
    {  // the setlists themselves: second first
      ws::v1::ClientMessage m;
      ws::v1::ClientMessage g;
      g.mutable_get_setlists();
      const auto lr = call(g);
      CHECK(lr.has_setlists_reply() && lr.setlists_reply().setlists_size() >= 2);
      m.mutable_reorder_setlists()->add_ids(s2);
      for (const auto& s : lr.setlists_reply().setlists()) if (s.id() != s2) m.mutable_reorder_setlists()->add_ids(s.id());
      expect_ack(m);
    }
    const std::string tag = create("tag", "Persist Tag");
    {
      ws::v1::ClientMessage m;
      m.mutable_set_setlist_tags()->set_setlist_id(s1);
      m.mutable_set_setlist_tags()->add_tag_ids(tag);
      expect_ack(m);
    }
    {  // DeleteTag cascades: a tag set on the second setlist, then deleted, leaves the setlist without it
      const std::string doomed = create("tag", "Persist Doomed");
      ws::v1::ClientMessage m;
      m.mutable_set_setlist_tags()->set_setlist_id(s2);
      m.mutable_set_setlist_tags()->add_tag_ids(doomed);
      expect_ack(m);
      ws::v1::ClientMessage d;
      d.mutable_delete_tag()->set_id(doomed);
      expect_ack(d);
    }
    {  // move B to the second setlist as Studio does (Add, then Remove): the Live Set file stays
      ws::v1::ClientMessage a;
      a.mutable_add_live_set_to_setlist()->set_setlist_id(s2);
      a.mutable_add_live_set_to_setlist()->set_live_set_id(lb);
      a.mutable_add_live_set_to_setlist()->set_index(0);
      expect_ack(a);
      ws::v1::ClientMessage r;
      r.mutable_remove_live_set_from_setlist()->set_setlist_id(s1);
      r.mutable_remove_live_set_from_setlist()->set_live_set_id(lb);
      expect_ack(r);
    }
    // The engine restarts on the same data directory (the bridge reconnects to it).
    stop(epid);
    epid = spawn({engine, "--no-audio", "--duration", "120", "--socket", esock, "--data-dir", d + "/edata", "--library-root", d + "/lib"}, d + "/engine2.log");
    bool back = false;
    ws::v1::ServerMessage lr;
    CHECK(wait_for(
        [&] {
          Tcp t;
          if (!t.open(port2)) return false;
          if (!t.hello("smoke-mac", token).hello_ack().paired()) return false;
          ws::v1::ClientMessage g;
          g.mutable_get_setlists()->set_setlist_id(s1);
          g.mutable_get_setlists()->set_include_all_live_sets(true);
          const std::uint64_t id = t.send(g);
          for (int i = 0; i < 5; ++i) {
            if (!t.recv(lr, 3000)) return false;
            if (lr.request_id() == id) return back = lr.has_setlists_reply();
          }
          return false;
        },
        60000));
    CHECK(back);
    if (back) {
      const auto& rep = lr.setlists_reply();
      CHECK(rep.setlists_size() >= 2);  // vacuity guard: an engine that lost its setlists (or an empty read) fails here
      const ws::v1::SetlistSummary* mine = nullptr;
      for (const auto& s : rep.setlists()) if (s.id() == s1) mine = &s;
      CHECK(mine != nullptr);
      if (mine) {
        CHECK(mine->name() == "Persist Show" && mine->live_set_count() == 2 && mine->missing_count() == 0 && mine->tag_ids_size() == 1 && mine->tag_ids(0) == tag);
      }
      CHECK(rep.setlists_size() >= 2 && rep.setlists(0).id() == s2);  // the setlist order persisted
      CHECK(rep.entries_size() == 2 && rep.entries(0).live_set_id() == lc && rep.entries(1).live_set_id() == la);  // C, A: the reorder and the move persisted
      bool tag_listed = false, b_kept = false;
      for (const auto& t : rep.tags()) tag_listed = tag_listed || (t.id() == tag && t.name() == "Persist Tag");
      for (const auto& l : rep.all_live_sets()) b_kept = b_kept || l.live_set_id() == lb;
      CHECK(tag_listed && b_kept);
      bool doomed_listed = false;
      for (const auto& t : rep.tags()) doomed_listed = doomed_listed || t.name() == "Persist Doomed";
      CHECK(!doomed_listed);
      for (const auto& sl : rep.setlists()) if (sl.id() == s2) CHECK(sl.tag_ids_size() == 0);  // the cascade persisted
      if (std::getenv("VELORA_STUDIO_CARGO_TEST")) {  // the Rust client's setlist (checked only when it ran)
        bool smoke = false;
        for (const auto& s : rep.setlists()) smoke = smoke || (s.name() == "Studio Smoke Setlist" && s.live_set_count() == 2 && s.tag_ids_size() == 1);
        CHECK(smoke);
      }
    }
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
