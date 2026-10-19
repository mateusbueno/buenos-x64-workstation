// Smoke test of the real engine-host wiring (Story 4.4): start `engine-host --live-set ... --library-root ... --socket ...` in a subprocess and
// recall a scene over ws.v1. Removing the SceneController from engine-host's Live Set path makes the recall answer NO_LIVE_SET and fails this test.
// It runs engine-host with --no-audio, so it never skips for lack of a device.
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
  m.SerializeToString(&b);
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
  if (argc != 4) {
    std::fprintf(stderr, "usage: engine_host_scene_smoke ENGINE_HOST LIVE_SET LIBRARY_ROOT\n");
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
    ::execl(argv[1], argv[1], "--no-audio", "--duration", "20", "--live-set", argv[2], "--library-root", argv[3], "--socket", sock.c_str(), (char*)nullptr);
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
  m.mutable_recall_scene()->set_index(1);
  const bool got = call(fd, m, rep);
  if (!(got && rep.has_scene_reply() && rep.scene_reply().result() == ws::v1::SCENE_STEP_RESULT_OK && rep.scene_reply().current_scene() == 1)) {
    std::printf("RecallScene failed: got=%d error=%d code=%d\n", got, rep.has_error(), rep.has_error() ? int(rep.error().code()) : -1);
    ok = false;
  }
  m.Clear();
  m.set_request_id(3);
  m.mutable_get_state();
  ok = ok && call(fd, m, rep) && rep.has_state() && rep.state().current_scene() == 1 && rep.state().scene_count() == 3;
  ::close(fd);
  cleanup();
  std::printf(ok ? "engine_host_scene_smoke: ok\n" : "engine_host_scene_smoke: FAILED\n");
  return ok ? 0 : 1;
}
