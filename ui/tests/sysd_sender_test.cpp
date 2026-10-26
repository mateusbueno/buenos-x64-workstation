// Story 7.7: the real SysdConfigSender against a real velora-sysd process in a temp dir: a request reaches sysd and comes back as the reported value, the value
// persists across a sysd restart (and is what a new sender is told), a request while sysd is absent fails with an event (no hang, no crash), and a sender made
// before sysd starts is told the config once sysd appears.
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "velora/ui/sysd_config_sender.hpp"

using namespace velora::ui;

static int g_fail = 0;
#define CHECK(c)                                               \
  do {                                                         \
    if (!(c)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

static pid_t spawn(const std::vector<std::string>& args, const std::string& log) {
  const pid_t pid = ::fork();
  if (pid == 0) {
    const int o = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
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
  if (pid > 0) {
    ::kill(pid, SIGTERM);
    int st = 0;
    ::waitpid(pid, &st, 0);
  }
  pid = -1;
}
// Next event of the wanted kind within the timeout (other events are skipped).
static bool wait_event(SysdConfigSender& s, SysdConfigSender::Event::Kind kind, SysdConfigSender::Event& out, int timeout_ms = 15000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < end) {
    SysdConfigSender::Event e;
    while (s.poll(e))
      if (e.kind == kind) {
        out = e;
        return true;
      }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}
static bool wait_config(SysdConfigSender& s, ClockSourceMode want, int timeout_ms = 15000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < end) {
    SysdConfigSender::Event e;
    if (!wait_event(s, SysdConfigSender::Event::Kind::Config, e, 200)) continue;
    if (e.clock_source == want) return true;
  }
  return false;
}

static bool wait_boot(SysdConfigSender& s, uint8_t mode, const std::string& id, int timeout_ms = 15000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < end) {
    SysdConfigSender::Event e;
    if (!wait_event(s, SysdConfigSender::Event::Kind::Config, e, 200)) continue;
    if (e.has_boot_into && e.boot_into == mode && e.default_live_set_id == id) return true;
  }
  return false;
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: sysd_sender_test VELORA_SYSD\n");
    return 2;
  }
  const std::string sysd = argv[1];
  char dir[] = "/tmp/vss-XXXXXX";
  if (!mkdtemp(dir)) return 2;
  const std::string d = dir, sock = d + "/s.sock", log = d + "/sysd.log";
  pid_t spid = -1;

  {  // 1. sysd absent: the sender exists, a request fails with an event.
    auto s = make_socket_sysd_sender(sock);
    s->request_clock_source(ClockSourceMode::DIN);
    SysdConfigSender::Event e;
    CHECK(wait_event(*s, SysdConfigSender::Event::Kind::Failed, e) && !e.error.empty() && e.unreachable);
    // 2. sysd appears: this same sender (it was waiting) is told the config, then a request is applied and reported back.
    spid = spawn({sysd, "--data-dir", d + "/data", "--socket", sock}, log);
    CHECK(wait_config(*s, ClockSourceMode::Internal));
    s->request_clock_source(ClockSourceMode::USB);
    CHECK(wait_config(*s, ClockSourceMode::USB));
    s->request_clock_source(ClockSourceMode::DIN);
    CHECK(wait_config(*s, ClockSourceMode::DIN));
  }
  // 3. Persistence: sysd restarts on the same data dir; a new sender is told DIN.
  stop(spid);
  spid = spawn({sysd, "--data-dir", d + "/data", "--socket", sock}, log);
  {
    auto s = make_socket_sysd_sender(sock);
    CHECK(wait_config(*s, ClockSourceMode::DIN));
    // 4. sysd goes away under a connected sender, then returns: the sender reconnects and its next request works.
    stop(spid);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    spid = spawn({sysd, "--data-dir", d + "/data", "--socket", sock}, log);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    s->request_clock_source(ClockSourceMode::USB);
    SysdConfigSender::Event e;
    bool ok = false;
    for (int i = 0; i < 10 && !ok; ++i) {  // a request raced with the reconnect may fail once; the retry must land
      ok = wait_config(*s, ClockSourceMode::USB, 3000);
      if (!ok) s->request_clock_source(ClockSourceMode::USB);
    }
    CHECK(ok);
  }
  // 5. Story 8.9: Boot into. Mode only, then the default Live Set (mode + id), persisted across a restart; a refused id is a Failed event for Boot into and leaves the value.
  spid = spawn({sysd, "--data-dir", d + "/data", "--socket", sock}, log);
  {
    const std::string uuid = "b8e2f6a3-5d40-4f9b-8c71-2a3b4c5d6e71";
    auto s = make_socket_sysd_sender(sock);
    CHECK(wait_boot(*s, 0, ""));
    s->request_boot_into(1, "");
    CHECK(wait_boot(*s, 1, ""));
    s->request_boot_into(2, uuid);
    CHECK(wait_boot(*s, 2, uuid));
    s->request_boot_into(0, "live-1");  // not a Live Set id: refused
    SysdConfigSender::Event e;
    CHECK(wait_event(*s, SysdConfigSender::Event::Kind::Failed, e) && e.setting == SysdConfigSender::Event::Setting::BootInto && !e.unreachable);
    stop(spid);  // restart: the value persists and a new sender is told
    spid = spawn({sysd, "--data-dir", d + "/data", "--socket", sock}, log);
    auto s2 = make_socket_sysd_sender(sock);
    CHECK(wait_boot(*s2, 2, uuid));
  }
  stop(spid);
  const std::string cmd = "rm -rf " + d;
  [[maybe_unused]] const int rc = std::system(cmd.c_str());
  std::printf(g_fail ? "sysd_sender_test: FAILED\n" : "sysd_sender_test: ok\n");
  return g_fail ? 1 : 0;
}
