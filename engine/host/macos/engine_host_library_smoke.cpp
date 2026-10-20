// Smoke test of the optional librarian wiring of Story 4.11: `engine-host --library-socket PATH` in a subprocess reports the Live Set that sounds to the
// real `librarian-service` process over the Library API, even when the engine was started BEFORE the service (the client reconnects with backoff and
// delivers what it queued). It also checks the performing state: a note injected by the hidden --test-inject-note-after-ms flag makes the
// librarian's state().playing true, and false again after the (short) --library-playing-idle-ms window. The engine-host itself has no librarian logic; the catalog is written only by the service. Runs engine-host --no-audio.
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "library_api.pb.h"
#include "ws_v1.pb.h"

namespace fs = std::filesystem;

namespace {

bool read_exact(int fd, std::string& out, std::size_t n, int timeout_ms) {
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

int connect_to(const std::string& path) {
  const int c = ::socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  std::strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
  if (::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) return c;
  ::close(c);
  return -1;
}

// One request/reply exchange (pushes are skipped).
template <class Msg, class Reply>
bool call(int fd, Msg& m, Reply& reply, std::uint64_t& rid) {
  m.set_request_id(++rid);
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
    if (reply.request_id() == rid) return true;
  }
}

struct Proc {
  pid_t pid = -1;
  void start(const char* exe, const std::vector<std::string>& args, const std::string& log) {
    pid = ::fork();
    if (pid == 0) {
      const int lf = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
      ::dup2(lf, 1);
      ::dup2(lf, 2);
      ::dup2(::open("/dev/null", O_RDONLY), 0);
      std::vector<char*> av{const_cast<char*>(exe)};
      for (const std::string& a : args) av.push_back(const_cast<char*>(a.c_str()));
      av.push_back(nullptr);
      ::execv(exe, av.data());
      _exit(127);
    }
  }
  int stop() {
    if (pid <= 0) return 0;
    ::kill(pid, SIGTERM);
    int st = 0;
    ::waitpid(pid, &st, 0);
    pid = -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  }
};

bool wait_connect(const std::string& path, int& fd) {
  for (int i = 0; i < 400 && fd < 0; ++i) {
    fd = connect_to(path);
    if (fd < 0) std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return fd >= 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 6) {
    std::fprintf(stderr, "usage: engine_host_library_smoke ENGINE_HOST LIBRARIAN_SERVICE LIVE_SET_FIXTURE_DIR LIBRARY_ROOT TARGET_LIVE_SET_ID\n");
    return 2;
  }
  ::signal(SIGPIPE, SIG_IGN);
  char tmpl[] = "/tmp/vlsm.XXXXXX";
  if (!mkdtemp(tmpl)) return 2;
  const std::string root = tmpl;
  const std::string data = root + "/data", cat = root + "/cat";
  fs::create_directories(data + "/live_sets");
  fs::create_directories(cat);
  for (const auto& e : fs::directory_iterator(argv[3]))
    if (e.path().extension() == ".liveset") fs::copy_file(e.path(), data + "/live_sets/" + e.path().filename().string());
  const std::string esock = root + "/e.sock", lsock = root + "/l.sock";
  bool ok = true;
  auto fail = [&](const char* what) {
    std::printf("FAILED: %s\n", what);
    ok = false;
  };
  Proc engine, lib;
  // The engine starts first: there is no librarian yet.
  engine.start(argv[1], {"--no-audio", "--duration", "60", "--data-dir", data, "--library-root", argv[4], "--socket", esock, "--library-socket", lsock,
                         "--library-playing-idle-ms", "2500", "--test-inject-note-after-ms", "5000"},
               root + "/engine.log");
  int efd = -1;
  std::string first_id;
  std::uint64_t rid = 0;
  if (!wait_connect(esock, efd)) {
    fail("engine-host did not start");
  } else {
    ws::v1::ClientMessage m;
    ws::v1::ServerMessage r;
    m.mutable_hello()->set_version(1);
    if (!call(efd, m, r, rid) || !r.has_hello_ack()) fail("hello");
    m.Clear();
    m.mutable_get_state();
    if (ok && (!call(efd, m, r, rid) || !r.has_state() || r.state().live_set_id().empty())) fail("initial state");
    else if (ok) first_id = r.state().live_set_id();
    m.Clear();
    m.mutable_jump_to_live_set()->set_id(argv[5]);
    if (ok && (!call(efd, m, r, rid) || !r.has_jump_reply())) fail("JumpToLiveSet");
  }
  // Now the librarian comes up; the engine's client reconnects and delivers what it queued.
  if (ok) {
    lib.start(argv[2], {"--data", cat, "--socket", lsock}, root + "/lib.log");
    int lfd = -1;
    if (!wait_connect(lsock, lfd)) {
      fail("librarian-service did not start");
    } else {
      ::close(lfd);
      // Polls the librarian's state (bounded); the order in which the engine's tick reports the two Live Sets is not part of the contract.
      auto fetch = [&](std::vector<std::string>& recent, bool& playing) {
        const int fd = connect_to(lsock);
        if (fd < 0) return false;
        ws::v1::LibraryClientMessage m;
        ws::v1::LibraryServerMessage r;
        std::uint64_t lrid = 0;
        m.mutable_hello()->set_version(1);
        bool got = false;
        if (call(fd, m, r, lrid)) {
          m.Clear();
          m.mutable_get_favorites_and_recent();
          if (call(fd, m, r, lrid) && r.has_state()) {
            recent.assign(r.state().recent().begin(), r.state().recent().end());
            playing = r.state().playing();
            got = true;
          }
        }
        ::close(fd);
        return got;
      };
      if (first_id == argv[5]) fail("the target Live Set must differ from the first one");
      std::vector<std::string> recent;
      bool playing = false;
      bool both = false;
      for (int i = 0; i < 300 && ok && !both; ++i) {  // the engine's client backs off up to 2 s before it reaches the late librarian
        if (fetch(recent, playing)) both = recent.size() == 2 && std::find(recent.begin(), recent.end(), first_id) != recent.end() &&
                                           std::find(recent.begin(), recent.end(), std::string(argv[5])) != recent.end();
        if (!both) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (ok && !both) fail("Recent did not receive the engine's two Live Sets");
      // The performing state: the injected note makes it true, and it falls back after the idle window.
      bool saw_true = false, saw_false_after = false;
      for (int i = 0; i < 400 && ok && !saw_false_after; ++i) {
        if (fetch(recent, playing)) {
          if (playing) saw_true = true;
          else if (saw_true) saw_false_after = true;
        }
        if (!saw_false_after) std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (ok && !saw_true) fail("the librarian never saw playing=true after the injected note");
      else if (ok && !saw_false_after) fail("the librarian never saw playing=false after the idle window");
    }
  }
  if (efd >= 0) ::close(efd);
  engine.stop();
  if (lib.stop() != 0) fail("librarian-service did not stop cleanly");
  std::error_code ec;
  fs::remove_all(root, ec);
  std::printf(ok ? "engine_host_library_smoke: ok\n" : "engine_host_library_smoke: FAILED\n");
  return ok ? 0 : 1;
}
