// Smoke test of the real engine-host wiring of protected libraries (Story 6.5): `engine-host --live-set ... --library-root ... --trust-list ... --dev-device-key ...`
// in a subprocess over a PROTECTED library laid out as the librarian installs it. The Live Set loads (the part is ready), a ws.v1 client reads State.content, then a
// block of the bundle is damaged ON DISK and the I/O edge thread (--scrub-interval-ms) finds it: State.content reports the library damaged. A second engine-host with
// another device's key refuses to start the Live Set with the typed cause. Runs engine-host --no-audio, so it never skips for lack of a device.
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

#include "../tests/protected_fixtures.hpp"
#include "velora/host/live_set_store.hpp"
#include "ws_v1.pb.h"

namespace fs = std::filesystem;
namespace ls = velora::host::liveset;

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

static bool read_message(int fd, ws::v1::ServerMessage& msg, int timeout_ms) {
  std::string hd, body;
  if (!read_exact(fd, hd, 4, timeout_ms)) return false;
  const std::uint32_t len = (std::uint8_t(hd[0]) << 24) | (std::uint8_t(hd[1]) << 16) | (std::uint8_t(hd[2]) << 8) | std::uint8_t(hd[3]);
  return read_exact(fd, body, len, timeout_ms) && msg.ParseFromString(body);
}

static std::string slurp(const std::string& p) {
  std::ifstream f(p);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

struct Engine {
  pid_t pid = -1;
  std::string log;
};

static Engine start(const char* exe, const std::string& dir, const std::string& name, const std::vector<std::string>& args) {
  Engine e;
  e.log = dir + "/" + name + ".log";
  e.pid = ::fork();
  if (e.pid == 0) {
    const int fd = ::open(e.log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ::dup2(fd, 1);
    ::dup2(fd, 2);
    const int devnull = ::open("/dev/null", O_RDONLY);
    ::dup2(devnull, 0);
    std::vector<char*> av;
    av.push_back(const_cast<char*>(exe));
    for (const std::string& a : args) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    ::execv(exe, av.data());
    _exit(127);
  }
  return e;
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: engine_host_protected_smoke ENGINE_HOST\n");
    return 2;
  }
  char dirb[] = "/tmp/velora-ehp-XXXXXX";
  if (!mkdtemp(dirb)) return 2;
  const std::string dir = dirb;
  const std::string root = dir + "/root";
  int rc = 1;
  auto finish = [&](bool ok, const char* why) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    std::printf(ok ? "engine_host_protected_smoke: ok\n" : "engine_host_protected_smoke: FAILED (%s)\n", why);
    rc = ok ? 0 : 1;
    return rc;
  };

  const pfx::Keys keys = pfx::make_keys(5);
  const auto lib = pfx::install_library(root, 1, {{"piano.sfz", pfx::Spec::kConstFloat, 0.1f}}, keys);
  pfx::write_file(dir + "/device.x25519.priv", pfx::Bytes(keys.device_priv.begin(), keys.device_priv.end()));
  const pfx::Keys other = pfx::make_keys(9);
  pfx::write_file(dir + "/other.priv", pfx::Bytes(other.device_priv.begin(), other.device_priv.end()));
  {
    std::ofstream t(dir + "/dev.pub-list");
    t << "dev content-manifest " << velora::format::to_hex(keys.sign_id) << " " << velora::format::to_hex(keys.sign_pub) << "\n";
  }
  ls::lsp::LiveSet s = ls::make_default_live_set("2e6b1f40-7c3a-4d58-9b21-6a0c5e8d4f17", "Protected", 8);
  s.mutable_parts(0)->mutable_timbre()->set_library_id(lib.library_id);
  s.mutable_parts(0)->mutable_timbre()->set_instrument_key("piano.sfz");
  std::string bytes;
  if (ls::serialize(s, bytes) != ls::Status::kOk) return finish(false, "serialize");
  { std::ofstream f(dir + "/p.liveset", std::ios::binary | std::ios::trunc); f.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); }

  // 1. The wrong device key: the Live Set does not load, the cause is typed, the process exits 1.
  {
    Engine e = start(argv[1], dir, "wrong", {"--no-audio", "--duration", "5", "--live-set", dir + "/p.liveset", "--library-root", root, "--socket", dir + "/w.sock", "--trust-list",
                                              dir + "/dev.pub-list", "--dev-device-key", dir + "/other.priv"});
    int st = 0;
    for (int i = 0; i < 200; ++i) {
      if (::waitpid(e.pid, &st, WNOHANG) == e.pid) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      if (i == 199) {
        ::kill(e.pid, SIGKILL);
        ::waitpid(e.pid, &st, 0);
        return finish(false, "wrong key: engine-host did not exit");
      }
    }
    const std::string out = slurp(e.log);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 1 || out.find("key_unavailable") == std::string::npos) {
      std::printf("%s\n", out.c_str());
      return finish(false, "wrong key was not refused with key_unavailable");
    }
  }

  // 2. The right key: the part is ready, the control plane shows a clean State.content, then the damage found on disk is reported.
  Engine e = start(argv[1], dir, "good", {"--no-audio", "--duration", "30", "--live-set", dir + "/p.liveset", "--library-root", root, "--socket", dir + "/e.sock", "--trust-list",
                                           dir + "/dev.pub-list", "--dev-device-key", dir + "/device.x25519.priv", "--scrub-interval-ms", "100"});
  auto cleanup = [&] {
    ::kill(e.pid, SIGTERM);
    int st = 0;
    ::waitpid(e.pid, &st, 0);
  };
  int fd = -1;
  for (int i = 0; i < 200 && fd < 0; ++i) {
    int st = 0;
    if (::waitpid(e.pid, &st, WNOHANG) == e.pid) {
      std::printf("engine-host exited early:\n%s\n", slurp(e.log).c_str());
      return finish(false, "engine-host exited early");
    }
    const int c = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, (dir + "/e.sock").c_str(), sizeof a.sun_path - 1);
    if (::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) fd = c;
    else {
      ::close(c);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  if (fd < 0) {
    std::printf("no control socket:\n%s\n", slurp(e.log).c_str());
    cleanup();
    return finish(false, "no control socket");
  }
  bool ok = true;
  const std::string log0 = slurp(e.log);
  ok = ok && log0.find("core_limit=0/0") != std::string::npos && log0.find("protected libraries: on") != std::string::npos && log0.find("live set part 0: ready") != std::string::npos;
  ws::v1::ClientMessage m;
  ws::v1::ServerMessage rep;
  m.set_request_id(1);
  m.mutable_hello()->set_version(1);
  ok = ok && call(fd, m, rep) && rep.has_hello_ack();
  auto get_state = [&](int id) {
    m.Clear();
    m.set_request_id(static_cast<std::uint32_t>(id));
    m.mutable_get_state();
    return call(fd, m, rep) && rep.has_state();
  };
  ok = ok && get_state(2) && rep.state().has_content() && rep.state().content().damaged_library_count() == 0 && rep.state().content().content_integrity() == 0;
  if (!ok) std::printf("before damage: log=\n%s\n", log0.c_str());

  // Damage one block of the installed bundle in place; the edge thread (every 100 ms) finds it.
  const std::uint64_t blocks_bytes = 4096ull * 4 * 2 + (12000 - 8192) * 4;
  {
    std::fstream f(lib.dir / "bundle.vlrp", std::ios::in | std::ios::out | std::ios::binary);
    f.seekg(static_cast<std::streamoff>(lib.bundle.size() - blocks_bytes + 16384 + 50));
    char c = 0;
    f.get(c);
    f.seekp(static_cast<std::streamoff>(lib.bundle.size() - blocks_bytes + 16384 + 50));
    f.put(static_cast<char>(c ^ 0x20));
  }
  // The report reaches a SUBSCRIBER as a pushed State (no command is sent): the control tick pushes State.content when an edge thread reports damage.
  m.Clear();
  m.set_request_id(5);
  m.mutable_subscribe()->set_enable(true);
  ok = ok && call(fd, m, rep);
  ws::v1::ServerMessage push;
  bool reported = false;
  const std::uint64_t rev_before = rep.revision();
  for (int i = 0; i < 300 && ok && !reported; ++i) {  // pushes already queued (the initial State) are skipped; the damaged one arrives within the scrub interval
    if (!read_message(fd, push, 100)) continue;
    reported = push.request_id() == 0 && push.has_state() && push.state().content().damaged_library_count() == 1 && push.state().content().damaged_library_ids_size() == 1 &&
               push.state().content().damaged_library_ids(0) == lib.library_id && push.state().content().damage_reports() >= 1;
  }
  if (!reported) std::printf("the damaged library was never pushed (ok=%d)\n%s\n", ok, slurp(e.log).c_str());
  ok = ok && reported && push.revision() > rev_before;  // a new revision
  ok = ok && get_state(900) && rep.state().content().damaged_library_count() == 1;  // and GetState agrees
  ::close(fd);
  cleanup();
  const std::string log1 = slurp(e.log);
  ok = ok && log1.find("damaged block(s) found") != std::string::npos;
  return finish(ok, "damage");
}
