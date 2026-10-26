// End-to-end smoke of the sysd config service (Foundation: sysd config service) with the real binaries: `engine-host --no-audio --sysd-socket` starts BEFORE
// velora-sysd (it runs on the defaults and answers its own ws.v1 meanwhile), applies the config when sysd appears, applies a change made with velora-sysdctl once,
// keeps running when sysd goes away, does not re-apply the same revision when sysd comes back, and a refused sysdctl command changes nothing.
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "sysdctl_client.hpp"

static int g_fail = 0;
#define CHECK(c)                                               \
  do {                                                         \
    if (!(c)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

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

static pid_t spawn(const std::vector<std::string>& args, const std::string& out_path, const std::string& err_path) {
  const pid_t pid = ::fork();
  if (pid == 0) {
    const int o = ::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    const int e = err_path.empty() ? o : ::open(err_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ::dup2(o, 1);
    ::dup2(e, 2);
    std::vector<char*> av;
    for (const std::string& a : args) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    ::execv(av[0], av.data());
    _exit(127);
  }
  return pid;
}

static int run(const std::vector<std::string>& args, const std::string& out_path, const std::string& err_path) {
  const auto t0 = std::chrono::steady_clock::now();
  const pid_t pid = spawn(args, out_path, err_path);
  int st = 0;
  ::waitpid(pid, &st, 0);
  const int rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  std::printf("  [%s %s] exit %d in %lld ms\n", args[0].substr(args[0].rfind('/') + 1).c_str(), args.size() > 3 ? args[3].c_str() : "",
              rc, (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
  return rc;
}

static bool wait_for(const std::function<bool()>& pred, int timeout_ms = 20000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!pred()) {
    if (std::chrono::steady_clock::now() > end) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return true;
}

static std::size_t count(const std::string& hay, const std::string& needle) {
  std::size_t n = 0;
  for (std::size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) ++n;
  return n;
}

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: engine_host_sysd_smoke ENGINE_HOST VELORA_SYSD VELORA_SYSDCTL\n");
    return 2;
  }
  const std::string engine = argv[1], sysd = argv[2], ctl = argv[3];
  char dir[] = "/tmp/vsm-XXXXXX";
  if (!mkdtemp(dir)) return 2;
  const std::string d = dir, esock = d + "/e.sock", ssock = d + "/s.sock", elog = d + "/engine.log", slog = d + "/sysd.log", out = d + "/out.txt", err = d + "/err.txt";
  const pid_t epid = spawn({engine, "--no-audio", "--duration", "120", "--socket", esock, "--sysd-socket", ssock}, elog, "");
  pid_t spid = -1;
  auto cleanup = [&] {
    if (g_fail) {  // everything a failure needs, before the directory goes
      for (const std::string& f : {elog, slog, err, out}) std::printf("---- %s ----\n%s\n", f.c_str(), slurp(f).c_str());
    }
    if (spid > 0) {
      ::kill(spid, SIGTERM);
      int st = 0;
      ::waitpid(spid, &st, 0);
    }
    ::kill(epid, SIGTERM);
    int st = 0;
    ::waitpid(epid, &st, 0);
    const std::string cmd = "rm -rf " + d;
    [[maybe_unused]] const int rc = std::system(cmd.c_str());
  };
  auto engine_alive = [&] {
    velora::sysdctl::Connection c;
    std::string e;
    if (!c.open(esock, e, 10000)) return false;
    ws::v1::ClientMessage m;
    m.mutable_get_diagnostics();
    ws::v1::ServerMessage r;
    return c.call(m, r, e) && r.has_diagnostics_reply();
  };
  // 1. sysd absent: the engine starts and serves with the defaults.
  CHECK(wait_for([&] { return engine_alive(); }));
  CHECK(slurp(elog).find("sysd: config client on") != std::string::npos);
  CHECK(slurp(elog).find("sysd config: revision") == std::string::npos);
  // The engine refuses the config messages (it does not serve them): the answer is the unknown-command error, from the real binary.
  {
    velora::sysdctl::Connection c;
    std::string e;
    ws::v1::ConfigReply cr;
    CHECK(c.open(esock, e, 10000) && !c.get(cr, e) && e.find("ERROR_CODE_INVALID_COMMAND") != std::string::npos);
  }
  // 2. sysd appears: the engine applies its config once.
  spid = spawn({sysd, "--data-dir", d + "/data", "--socket", ssock}, slog, "");
  CHECK(wait_for([&] { return slurp(elog).find("sysd config: revision 0 clock_source=CLOCK_SOURCE_INTERNAL velocity_curve=VELOCITY_CURVE_NORMAL boot_into=BOOT_INTO_MODE_LAST_SOUND") != std::string::npos; }));
  // 3. A change through the CLI reaches the engine, once.
  CHECK(run({ctl, "--socket", ssock, "set", "clock_source", "din", "velocity_curve", "heavy"}, out, err) == 0);
  CHECK(slurp(out).find("clock_source: CLOCK_SOURCE_DIN") != std::string::npos && slurp(out).find("revision: 1") != std::string::npos);
  CHECK(wait_for([&] { return slurp(elog).find("sysd config: revision 1 clock_source=CLOCK_SOURCE_DIN velocity_curve=VELOCITY_CURVE_HEAVY") != std::string::npos; }));
  CHECK(run({ctl, "--socket", ssock, "get"}, out, err) == 0 && slurp(out).find("velocity_curve: VELOCITY_CURVE_HEAVY") != std::string::npos);
  // Story 7.7: the engine applied the clock source of both configs (INTERNAL at first contact, then DIN).
  CHECK(slurp(elog).find("clock source: CLOCK_SOURCE_INTERNAL applied") != std::string::npos);
  CHECK(slurp(elog).find("clock source: CLOCK_SOURCE_DIN applied") != std::string::npos);
  // 4. A refused command (min >= max): typed error, exit 1, nothing changes, the engine is not told.
  CHECK(run({ctl, "--socket", ssock, "set", "sustain", "900,100"}, out, err) == 1);
  CHECK(slurp(err).find("ERROR_CODE_INVALID_CONFIG") != std::string::npos);
  CHECK(run({ctl, "--socket", ssock, "get"}, out, err) == 0 && slurp(out).find("revision: 1") != std::string::npos);
  CHECK(run({ctl, "--socket", ssock, "set", "boot_into", "default_live_set"}, out, err) == 1);  // needs an id
  CHECK(run({ctl, "--socket", ssock, "set", "boot_into", "default_live_set", "default_live_set_id", "b8e2f6a3-5d40-4f9b-8c71-2a3b4c5d6e71"}, out, err) == 0);
  CHECK(wait_for([&] { return slurp(elog).find("sysd config: revision 2") != std::string::npos; }));
  // 5. sysd goes away: the engine keeps running. It comes back with the same persisted config: no second application of revision 2.
  ::kill(spid, SIGTERM);
  int st = 0;
  ::waitpid(spid, &st, 0);
  spid = -1;
  CHECK(engine_alive());
  const std::size_t applied = count(slurp(elog), "sysd config: revision");
  spid = spawn({sysd, "--data-dir", d + "/data", "--socket", ssock}, slog, "");
  CHECK(wait_for([&] { return run({ctl, "--socket", ssock, "get"}, out, err) == 0; }));
  CHECK(slurp(out).find("revision: 2") != std::string::npos && slurp(out).find("clock_source: CLOCK_SOURCE_DIN") != std::string::npos);  // persisted
  CHECK(run({ctl, "--socket", ssock, "set", "clock_source", "usb"}, out, err) == 0);
  CHECK(wait_for([&] { return slurp(elog).find("sysd config: revision 3 clock_source=CLOCK_SOURCE_USB") != std::string::npos; }));
  CHECK(wait_for([&] { return slurp(elog).find("clock source: CLOCK_SOURCE_USB applied") != std::string::npos; }));
  CHECK(count(slurp(elog), "clock source: CLOCK_SOURCE_DIN applied") == 1);  // the persisted DIN was not applied a second time
  CHECK(count(slurp(elog), "sysd config: revision") == applied + 1);  // only the new revision: 2 was not applied again
  CHECK(engine_alive());
  // sysdctl watch prints the config now and exits after COUNT pushes.
  CHECK(run({ctl, "--socket", ssock, "watch", "1"}, out, err) == 0 && slurp(out).find("clock_source: CLOCK_SOURCE_USB") != std::string::npos);
  CHECK(run({ctl, "--socket", d + "/none.sock", "get"}, out, err) == 1);
  // Story 3.11: the calibration command reaches the engine's control plane; this engine has no panel reader (no --virtual-panel), so it is a typed refusal, nothing
  // changes (the measurement itself is tested with the reader in pedal_test and sysd_pedal_calibration_test). A bad pedal name is a usage error.
  CHECK(run({ctl, "--socket", ssock, "--engine-socket", esock, "calibrate-pedal", "sustain", "--seconds", "1"}, out, err) == 1 &&
        slurp(err).find("no panel reader") != std::string::npos);
  CHECK(run({ctl, "--engine-socket", esock, "calibrate-pedal", "volume", "--seconds", "1"}, out, err) == 2);
  cleanup();
  std::printf(g_fail ? "engine_host_sysd_smoke: FAILED\n" : "engine_host_sysd_smoke: ok\n");
  return g_fail ? 1 : 0;
}
