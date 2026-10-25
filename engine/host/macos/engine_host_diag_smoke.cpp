// Smoke test of velora-diag against the real engine-host (Story 1.21): start `engine-host --no-audio --socket ...` and run the velora-diag CLI against it;
// it must exit 0 and print the counters, both histograms and the uptime. A dead socket must exit 1 with a message on stderr.
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

static int run(const std::string& exe, const std::string& sock, const std::string& out_path, const std::string& err_path) {
  const pid_t pid = ::fork();
  if (pid == 0) {
    const int o = ::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    const int e = ::open(err_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ::dup2(o, 1);
    ::dup2(e, 2);
    ::execl(exe.c_str(), exe.c_str(), "--socket", sock.c_str(), (char*)nullptr);
    _exit(127);
  }
  int st = 0;
  ::waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static std::string slurp(const std::string& p) {
  std::string s;
  if (FILE* f = std::fopen(p.c_str(), "r")) {
    char b[1024];
    std::size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    std::fclose(f);
  }
  return s;
}

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: engine_host_diag_smoke ENGINE_HOST VELORA_DIAG\n");
    return 2;
  }
  char dir[] = "/tmp/velora-ehd-XXXXXX";
  if (!mkdtemp(dir)) return 2;
  const std::string d = dir, sock = d + "/e.sock", log = d + "/log.txt", out = d + "/out.txt", err = d + "/err.txt";
  const pid_t pid = ::fork();
  if (pid == 0) {
    const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ::dup2(fd, 1);
    ::dup2(fd, 2);
    ::execl(argv[1], argv[1], "--no-audio", "--duration", "20", "--socket", sock.c_str(), (char*)nullptr);
    _exit(127);
  }
  auto cleanup = [&] {
    ::kill(pid, SIGTERM);
    int st = 0;
    ::waitpid(pid, &st, 0);
    for (const auto& f : {sock, log, out, err}) ::unlink(f.c_str());
    ::rmdir(dir);
  };
  bool up = false;
  for (int i = 0; i < 200 && !up; ++i) {
    const int c = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, sock.c_str(), sizeof a.sun_path - 1);
    up = ::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
    ::close(c);
    if (!up) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (!up) {
    std::printf("no control socket:\n%s\n", slurp(log).c_str());
    cleanup();
    return 1;
  }
  bool ok = true;
  const int rc = run(argv[2], sock, out, err);
  const std::string o = slurp(out);
  for (const char* k : {"uptime_ms:", "xrun:", "voice_steal:", "voice_exhausted:", "stream_underrun:", "render_time", "event_to_onset_latency"}) {
    if (o.find(k) == std::string::npos) {
      std::printf("missing '%s' in output:\n%s\n", k, o.c_str());
      ok = false;
    }
  }
  if (rc != 0) {
    std::printf("velora-diag exit %d, stderr: %s\n", rc, slurp(err).c_str());
    ok = false;
  }
  const int rc_bad = run(argv[2], d + "/missing.sock", out, err);
  if (rc_bad != 1 || slurp(err).empty()) {
    std::printf("unreachable socket: exit %d, stderr '%s'\n", rc_bad, slurp(err).c_str());
    ok = false;
  }
  cleanup();
  std::printf(ok ? "engine_host_diag_smoke: ok\n" : "engine_host_diag_smoke: FAILED\n");
  return ok ? 0 : 1;
}
