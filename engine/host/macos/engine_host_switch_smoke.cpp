// Smoke test of the real engine-host wiring (Story 4.5): start `engine-host --live-set-dir DIR --library-root ... --socket ...` in a subprocess and switch to the other Live Set over ws.v1: JumpToLiveSet answers at once, the State reports `loading` and then the new
// Live Set as `ready`. Removing the switcher or its control tick from engine-host makes the jump answer NO_LIVE_SET or never finish and fails this test.
// It runs engine-host with --no-audio (the wiring under test is the control plane, the switcher and the server tick), so it never skips.
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "ws_v1.pb.h"

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
    if (!read_exact(fd, hd, 4, 3000)) return false;
    const std::uint32_t len = (std::uint8_t(hd[0]) << 24) | (std::uint8_t(hd[1]) << 16) | (std::uint8_t(hd[2]) << 8) | std::uint8_t(hd[3]);
    if (!read_exact(fd, body, len, 3000) || !reply.ParseFromString(body)) return false;
    if (reply.request_id() == m.request_id()) return true;
  }
}

int main(int argc, char** argv) {
  if (argc != 5) {
    std::fprintf(stderr, "usage: engine_host_switch_smoke ENGINE_HOST LIVE_SET_DIR LIBRARY_ROOT TARGET_LIVE_SET_ID\n");
    return 2;
  }
  char dir[] = "/tmp/velora-ehs-XXXXXX";
  if (!mkdtemp(dir)) return 2;
  const std::string sock = std::string(dir) + "/e.sock", log = std::string(dir) + "/log.txt";
  const pid_t pid = ::fork();
  if (pid == 0) {
    const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ::dup2(fd, 1);
    ::dup2(fd, 2);
    const int devnull = ::open("/dev/null", O_RDONLY);
    ::dup2(devnull, 0);
    ::execl(argv[1], argv[1], "--no-audio", "--duration", "30", "--live-set-dir", argv[2], "--library-root", argv[3], "--socket", sock.c_str(), (char*)nullptr);
    _exit(127);
  }
  auto slurp = [&] {
    std::ifstream f(log);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
  };
  auto cleanup = [&] {
    ::kill(pid, SIGTERM);
    int st = 0;
    ::waitpid(pid, &st, 0);
    ::unlink(sock.c_str());
    ::unlink(log.c_str());
    ::rmdir(dir);
  };
  // Argument pairing: --live-set-dir without --library-root is refused (exit 2) with a clear message.
  {
    const pid_t bad = ::fork();
    if (bad == 0) {
      const int fd2 = ::open((log + ".pair").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
      ::dup2(fd2, 2);
      ::dup2(fd2, 1);
      ::execl(argv[1], argv[1], "--no-audio", "--live-set-dir", argv[2], "--socket", (sock + ".x").c_str(), (char*)nullptr);
      _exit(127);
    }
    int st2 = 0;
    ::waitpid(bad, &st2, 0);
    std::ifstream pf(log + ".pair");
    std::stringstream pss;
    pss << pf.rdbuf();
    ::unlink((log + ".pair").c_str());
    if (!(WIFEXITED(st2) && WEXITSTATUS(st2) == 2) || pss.str().find("go together") == std::string::npos) {
      std::printf("pairing error not refused (status %d)\n", st2);
      ::kill(pid, SIGTERM);
      ::waitpid(pid, &st2, 0);
      return 1;
    }
  }
  int fd = -1;
  for (int i = 0; i < 200 && fd < 0; ++i) {
    int st = 0;
    if (::waitpid(pid, &st, WNOHANG) == pid) {  // engine-host ended before the socket came up
      const std::string out = slurp();
      std::printf("engine-host exited early:\n%s\n", out.c_str());
      ::unlink(sock.c_str());
      ::unlink(log.c_str());
      ::rmdir(dir);
      const bool no_device = out.find("engine-host: audio:") != std::string::npos || out.find("engine-host: midi:") != std::string::npos;
      return no_device ? 77 : 1;
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
  if (fd < 0) {
    std::printf("no control socket:\n%s\n", slurp().c_str());
    cleanup();
    return 1;
  }
  bool ok = true;
  ws::v1::ClientMessage m;
  ws::v1::ServerMessage rep;
  m.set_request_id(1);
  m.mutable_hello()->set_version(1);
  ok = ok && call(fd, m, rep) && rep.has_hello_ack();
  m.Clear();
  m.set_request_id(2);
  m.mutable_get_state();
  ok = ok && call(fd, m, rep) && rep.has_state() && rep.state().live_set_name() == "Switch A" && !rep.state().loading().loading();
  // An unknown id is a typed error.
  m.Clear();
  m.set_request_id(3);
  m.mutable_jump_to_live_set()->set_id("00000000-0000-4000-8000-000000000000");
  ok = ok && call(fd, m, rep) && rep.has_error() && rep.error().code() == ws::v1::ERROR_CODE_NO_SUCH_LIVE_SET;
  // The jump: accepted at once, then loading, then the new Live Set is the one that sounds.
  m.Clear();
  m.set_request_id(4);
  m.mutable_jump_to_live_set()->set_id(argv[4]);
  const bool got = call(fd, m, rep);
  if (!(got && rep.has_jump_reply() && rep.jump_reply().result() == ws::v1::JUMP_RESULT_ACCEPTED && rep.jump_reply().target_name() == "Switch B")) {
    std::printf("JumpToLiveSet failed: got=%d error=%d code=%d\n", got, rep.has_error(), rep.has_error() ? int(rep.error().code()) : -1);
    ok = false;
  }
  bool ready = false;
  for (int i = 0; i < 200 && ok && !ready; ++i) {
    m.Clear();
    m.set_request_id(10 + i);
    m.mutable_get_state();
    if (!call(fd, m, rep) || !rep.has_state()) {
      ok = false;
      break;
    }
    ready = rep.state().loading().state() == ws::v1::LOADING_STATE_READY && rep.state().live_set_name() == "Switch B";
    if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (!ready) std::printf("the switch never became ready\n%s\n", slurp().c_str());
  ok = ok && ready && rep.state().revision() == 2;
  {
    const std::string out = slurp();
    if (out.find("2 Live Set(s)") == std::string::npos) {
      std::printf("missing log lines:\n%s\n", out.c_str());
      ok = false;
    }
  }
  ::close(fd);
  cleanup();
  std::printf(ok ? "engine_host_switch_smoke: ok\n" : "engine_host_switch_smoke: FAILED\n");
  return ok ? 0 : 1;
}
