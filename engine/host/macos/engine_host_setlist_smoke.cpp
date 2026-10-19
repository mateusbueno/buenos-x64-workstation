// Smoke test of the real engine-host wiring of Story 4.6: `engine-host --data-dir D --library-root R` in a subprocess.
//  * Existing data root: a ws.v1 client creates a setlist, loads it (nothing is loaded into the engine), steps to its Live Set with NextLiveSet (a seamless
//    background switch), reaches the end of the setlist, and a SUBSCRIBED second client receives the State push with the new setlist position after a
//    JumpToLiveSet; then the engine is restarted on the same directory and the setlist, tag and new Live Set are still there. A second engine-host on the
//    same directory is refused (single writer).
//  * Fresh data root (no Live Set at all): the engine starts on the dev configuration, CreateLiveSet makes the first Live Set over ws.v1, JumpToLiveSet loads
//    it, and a restart on the same directory starts with it.
// Note: State pushes also come from the switcher's own tick, and State.setlists is synchronised whenever a State is built, so dropping the controller tick
// from engine-host alone is not detectable here. Removing the controller, the store hand-over to the server thread or --data-dir handling fails this test.
// Runs engine-host --no-audio.
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "ws_v1.pb.h"

namespace fs = std::filesystem;

static bool read_exact(int fd, std::string& out, std::size_t n, int timeout_ms) {
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

static bool call(int fd, const ws::v1::ClientMessage& m, ws::v1::ServerMessage& reply) {
  std::string b;
  (void)m.SerializeToString(&b);
  const std::uint32_t n = static_cast<std::uint32_t>(b.size());
  const char h[4] = {char(n >> 24), char(n >> 16), char(n >> 8), char(n)};
  const std::string s = std::string(h, 4) + b;
  if (::send(fd, s.data(), s.size(), 0) != static_cast<ssize_t>(s.size())) return false;
  for (;;) {
    std::string hd, body;
    if (!read_exact(fd, hd, 4, 5000)) return false;
    const std::uint32_t len = (std::uint8_t(hd[0]) << 24) | (std::uint8_t(hd[1]) << 16) | (std::uint8_t(hd[2]) << 8) | std::uint8_t(hd[3]);
    if (!read_exact(fd, body, len, 5000) || !reply.ParseFromString(body)) return false;
    if (reply.request_id() == m.request_id()) return true;
  }
}

// Reads pushed messages (request_id 0) until `pred` holds for a State; false after `max_wait_ms` without a match.
template <class Pred>
static bool wait_push(int fd, Pred pred, int max_wait_ms = 8000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_wait_ms);
  while (std::chrono::steady_clock::now() < end) {
    std::string hd, body;
    if (!read_exact(fd, hd, 4, 500)) continue;
    const std::uint32_t len = (std::uint8_t(hd[0]) << 24) | (std::uint8_t(hd[1]) << 16) | (std::uint8_t(hd[2]) << 8) | std::uint8_t(hd[3]);
    ws::v1::ServerMessage pm;
    if (!read_exact(fd, body, len, 2000) || !pm.ParseFromString(body)) return false;
    if (pm.request_id() == 0 && pm.has_state() && pred(pm.state())) return true;
  }
  return false;
}

static std::uint64_t g_rid = 0;
static ws::v1::ClientMessage mk() {
  ws::v1::ClientMessage m;
  m.set_request_id(++g_rid);
  return m;
}

struct Engine {
  pid_t pid = -1;
  int fd = -1;
  std::string log;
  std::string sock;
  std::string slurp() const {
    std::ifstream f(log);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
  }
  // Starts engine-host; returns false when it ended early or the socket never came up.
  bool start(const char* exe, const std::string& data, const std::string& lib, const std::string& tag, int* exit_code = nullptr) {
    sock = data + "/" + tag + ".sock";
    log = data + "/" + tag + ".log";
    pid = ::fork();
    if (pid == 0) {
      const int lf = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
      ::dup2(lf, 1);
      ::dup2(lf, 2);
      ::dup2(::open("/dev/null", O_RDONLY), 0);
      ::execl(exe, exe, "--no-audio", "--duration", "60", "--data-dir", data.c_str(), "--library-root", lib.c_str(), "--socket", sock.c_str(), (char*)nullptr);
      _exit(127);
    }
    for (int i = 0; i < 200 && fd < 0; ++i) {
      int st = 0;
      if (::waitpid(pid, &st, WNOHANG) == pid) {
        if (exit_code) *exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        pid = -1;
        return false;
      }
      const int c = ::socket(AF_UNIX, SOCK_STREAM, 0);
      sockaddr_un a{};
      a.sun_family = AF_UNIX;
      std::strncpy(a.sun_path, sock.c_str(), sizeof a.sun_path - 1);
      if (::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) fd = c;
      else {
        ::close(c);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }
    return fd >= 0;
  }
  void stop() {
    if (fd >= 0) ::close(fd);
    fd = -1;
    if (pid > 0) {
      ::kill(pid, SIGTERM);
      int st = 0;
      ::waitpid(pid, &st, 0);
    }
    pid = -1;
    ::unlink(sock.c_str());
  }
};

static bool hello(int fd) {
  ws::v1::ClientMessage m = mk();
  ws::v1::ServerMessage r;
  m.mutable_hello()->set_version(1);
  return call(fd, m, r) && r.has_hello_ack();
}

static bool state(int fd, ws::v1::ServerMessage& r) {
  ws::v1::ClientMessage m = mk();
  m.mutable_get_state();
  return call(fd, m, r) && r.has_state();
}

int main(int argc, char** argv) {
  if (argc != 5) {
    std::fprintf(stderr, "usage: engine_host_setlist_smoke ENGINE_HOST LIVE_SET_FIXTURE_DIR LIBRARY_ROOT TARGET_LIVE_SET_ID\n");
    return 2;
  }
  char tmpl[] = "/tmp/velora-ehsl-XXXXXX";
  if (!mkdtemp(tmpl)) return 2;
  const std::string data = tmpl;
  fs::create_directories(data + "/live_sets");
  for (const auto& e : fs::directory_iterator(argv[2]))
    if (e.path().extension() == ".liveset") fs::copy_file(e.path(), data + "/live_sets/" + e.path().filename().string());
  bool ok = true;
  Engine eng;
  std::string setlist_id, new_id, a_id;
  auto fail = [&](const char* what) {
    std::printf("FAILED: %s\n%s\n", what, eng.slurp().c_str());
    ok = false;
  };
  if (!eng.start(argv[1], data, argv[3], "e1")) {
    fail("engine-host did not start");
  } else if (!hello(eng.fd)) {
    fail("hello");
  } else {
    ws::v1::ServerMessage r;
    if (!state(eng.fd, r) || !r.state().setlists().available() || r.state().live_set_name() != "Switch A" || !r.state().setlists().loaded_setlist_id().empty())
      fail("initial state");
    else a_id = r.state().live_set_id();
    // A second engine-host on the same data directory is refused: the single writer lock.
    {
      Engine second;
      int code = -1;
      const bool up = second.start(argv[1], data, argv[3], "e2", &code);
      if (up || code != 1) {
        second.stop();
        fail("a second engine on the same data directory was not refused");
      } else if (second.slurp().find("cannot open") == std::string::npos) {
        fail("the refusal does not say why");
      }
    }
    ws::v1::ClientMessage m = mk();
    m.mutable_create_setlist()->set_name("Smoke show");
    if (ok && (!call(eng.fd, m, r) || !r.has_created_reply())) fail("CreateSetlist");
    else if (ok) setlist_id = r.created_reply().id();
    if (ok) {
      m = mk();
      m.mutable_add_live_set_to_setlist()->set_setlist_id(setlist_id);
      m.mutable_add_live_set_to_setlist()->set_live_set_id(argv[4]);
      if (!call(eng.fd, m, r) || !r.has_ack()) fail("AddLiveSetToSetlist");
    }
    if (ok) {
      m = mk();
      m.mutable_add_live_set_to_setlist()->set_setlist_id(setlist_id);
      m.mutable_add_live_set_to_setlist()->set_live_set_id(argv[4]);
      if (!call(eng.fd, m, r) || !r.has_error() || r.error().code() != ws::v1::ERROR_CODE_DUPLICATE_LIVE_SET_IN_SETLIST) fail("duplicate refused");
    }
    if (ok) {
      m = mk();
      m.mutable_load_setlist()->set_id(setlist_id);
      if (!call(eng.fd, m, r) || !r.has_load_setlist_reply() || r.load_setlist_reply().position() != -1) fail("LoadSetlist");
      // Loading the setlist loaded nothing: Switch A still sounds and no load runs.
      if (ok && (!state(eng.fd, r) || r.state().live_set_name() != "Switch A" || r.state().loading().loading() || r.state().setlists().loaded_setlist_id() != setlist_id))
        fail("the setlist load must not touch the sound");
    }
    if (ok) {
      m = mk();
      m.mutable_next_live_set();
      if (!call(eng.fd, m, r) || !r.has_step_reply() || r.step_reply().live_set_name() != "Switch B" || r.step_reply().position() != 0 ||
          r.step_reply().jump_result() != ws::v1::JUMP_RESULT_ACCEPTED)
        fail("NextLiveSet");
    }
    bool ready = false;
    for (int i = 0; i < 200 && ok && !ready; ++i) {
      if (!state(eng.fd, r)) {
        fail("get_state");
        break;
      }
      ready = r.state().loading().state() == ws::v1::LOADING_STATE_READY && r.state().live_set_name() == "Switch B";
      if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (ok && (!ready || r.state().setlists().last_setlist_position() != 0 || r.state().setlists().current_live_set_id() != argv[4])) fail("the step never became ready");
    if (ok) {
      m = mk();
      m.mutable_next_live_set();
      if (!call(eng.fd, m, r) || !r.has_error() || r.error().code() != ws::v1::ERROR_CODE_END_OF_SETLIST) fail("end of setlist");
      else if (!state(eng.fd, r) || !r.state().setlists().end_of_setlist()) fail("end_of_setlist flag");
    }
    if (ok) {  // a subscriber sees the State push (with the new setlist position) after a plain JumpToLiveSet to a Live Set of the loaded setlist
      m = mk();
      m.mutable_add_live_set_to_setlist()->set_setlist_id(setlist_id);
      m.mutable_add_live_set_to_setlist()->set_live_set_id(a_id);
      if (!call(eng.fd, m, r) || !r.has_ack()) fail("AddLiveSetToSetlist (second)");
    }
    if (ok) {
      const int sub = [&] {
        const int c = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        std::strncpy(a.sun_path, eng.sock.c_str(), sizeof a.sun_path - 1);
        if (::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
          ::close(c);
          return -1;
        }
        return c;
      }();
      ws::v1::ServerMessage sr;
      m = mk();
      m.mutable_subscribe()->set_enable(true);
      if (sub < 0 || !hello(sub) || !call(sub, m, sr) || !sr.has_ack()) {
        fail("subscriber connect");
      } else {
        m = mk();
        m.mutable_jump_to_live_set()->set_id(a_id);
        if (!call(eng.fd, m, r) || !r.has_jump_reply()) fail("JumpToLiveSet");
        else if (!wait_push(sub, [&](const ws::v1::State& st) { return st.setlists().last_setlist_position() == 1 && st.setlists().loaded_setlist_id() == setlist_id; }))
          fail("no State push with the new setlist position");
        else if (!wait_push(sub, [&](const ws::v1::State& st) { return st.loading().state() == ws::v1::LOADING_STATE_READY && st.live_set_id() == a_id; }))
          fail("no State push when the jump finished");
      }
      if (sub >= 0) ::close(sub);
    }
    if (ok) {
      m = mk();
      m.mutable_create_live_set()->set_name("Brand new");
      if (!call(eng.fd, m, r) || !r.has_created_reply()) fail("CreateLiveSet");
      else new_id = r.created_reply().id();
    }
    if (ok) {
      m = mk();
      m.mutable_create_tag()->set_name("Smoke");
      std::string tag;
      if (!call(eng.fd, m, r) || !r.has_created_reply()) fail("CreateTag");
      else {
        tag = r.created_reply().id();
        m = mk();
        m.mutable_set_setlist_tags()->set_setlist_id(setlist_id);
        m.mutable_set_setlist_tags()->add_tag_ids(tag);
        if (!call(eng.fd, m, r) || !r.has_ack()) fail("SetSetlistTags");
      }
    }
  }
  eng.stop();

  // Restart on the same directory: the setlist, its tag, the new Live Set are all there.
  if (ok) {
    if (!eng.start(argv[1], data, argv[3], "e3")) {
      fail("restart");
    } else if (!hello(eng.fd)) {
      fail("hello after restart");
    } else {
      ws::v1::ServerMessage r;
      ws::v1::ClientMessage m = mk();
      m.mutable_get_setlists()->set_setlist_id(setlist_id);
      if (!call(eng.fd, m, r) || !r.has_setlists_reply() || r.setlists_reply().setlists_size() != 1 || r.setlists_reply().setlists(0).name() != "Smoke show" ||
          r.setlists_reply().setlists(0).tag_ids_size() != 1 || r.setlists_reply().tags_size() != 1 || r.setlists_reply().entries_size() != 2 ||
          r.setlists_reply().entries(0).live_set_id() != argv[4] || r.setlists_reply().entries(0).missing() || !r.setlists_reply().status().loaded_setlist_id().empty())
        fail("the setlist did not survive the restart");
      m = mk();
      m.mutable_jump_to_live_set()->set_id(new_id);
      if (!call(eng.fd, m, r) || !r.has_jump_reply()) fail("the created Live Set is unknown after the restart");
    }
    if (ok && eng.slurp().find("3 Live Set(s)") == std::string::npos) fail("the restart did not list 3 Live Sets");
    eng.stop();
  }
  // A fresh data root: no Live Set to start with. CreateLiveSet bootstraps it.
  if (ok) {
    char tmpl2[] = "/tmp/velora-ehsf-XXXXXX";
    if (!mkdtemp(tmpl2)) return 2;
    const std::string fresh = tmpl2;
    Engine e2;
    std::string first;
    auto fail2 = [&](const char* what) {
      std::printf("FAILED (fresh data dir): %s\n%s\n", what, e2.slurp().c_str());
      ok = false;
    };
    if (!e2.start(argv[1], fresh, argv[3], "f1")) {
      fail2("engine-host did not start on an empty data directory");
    } else if (!hello(e2.fd)) {
      fail2("hello");
    } else {
      ws::v1::ServerMessage r;
      ws::v1::ClientMessage m = mk();
      m.mutable_create_live_set()->set_name("First");
      if (!call(e2.fd, m, r) || !r.has_created_reply()) fail2("CreateLiveSet");
      else first = r.created_reply().id();
      if (ok) {
        m = mk();
        m.mutable_jump_to_live_set()->set_id(first);
        if (!call(e2.fd, m, r) || !r.has_jump_reply() || r.jump_reply().result() != ws::v1::JUMP_RESULT_ACCEPTED) fail2("JumpToLiveSet");
      }
      bool ready = false;
      for (int i = 0; i < 200 && ok && !ready; ++i) {
        if (!state(e2.fd, r)) {
          fail2("get_state");
          break;
        }
        ready = r.state().loading().state() == ws::v1::LOADING_STATE_READY && r.state().live_set_name() == "First";
        if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (ok && !ready) fail2("the first Live Set never loaded");
      if (ok && e2.slurp().find("holds no Live Set yet") == std::string::npos) fail2("the bootstrap line is missing");
    }
    e2.stop();
    if (ok) {
      if (!e2.start(argv[1], fresh, argv[3], "f2")) {
        fail2("restart on the bootstrapped directory");
      } else {
        ws::v1::ServerMessage r;
        if (!hello(e2.fd) || !state(e2.fd, r) || r.state().live_set_name() != "First" || r.state().live_set_id() != first) fail2("the restart did not start on the created Live Set");
      }
      e2.stop();
    }
    std::error_code ec2;
    fs::remove_all(fresh, ec2);
  }
  std::error_code ec;
  fs::remove_all(data, ec);
  std::printf(ok ? "engine_host_setlist_smoke: ok\n" : "engine_host_setlist_smoke: FAILED\n");
  return ok ? 0 : 1;
}
