// Smoke test of the real `librarian-service` binary (Story 4.11), in the style of the engine-host smoke tests: subprocesses on a temp data directory
// and socket, no audio. A Library API client adds favorites and reports Live Sets; the process is stopped with SIGTERM (orderly: unwritten Recent is
// flushed) and with SIGKILL (nothing written while "playing" survives, everything committed does); a second service on the same data is refused;
// a foreign catalog and bad arguments give the documented exit codes.
#include <fcntl.h>

#include "lib_test_util.hpp"

using ltest::Client;
using ltest::TempDir;
using ltest::uid;
using Ids = std::vector<std::string>;

namespace {

struct Proc {
  pid_t pid = -1;
  std::string log;
  bool start(const char* exe, const std::vector<std::string>& args, const std::string& log_path) {
    log = log_path;
    pid = ::fork();
    if (pid == 0) {
      const int lf = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
      ::dup2(lf, 1);
      ::dup2(lf, 2);
      ::dup2(::open("/dev/null", O_RDONLY), 0);
      std::vector<char*> av;
      av.push_back(const_cast<char*>(exe));
      for (const std::string& a : args) av.push_back(const_cast<char*>(a.c_str()));
      av.push_back(nullptr);
      ::execv(exe, av.data());
      _exit(127);
    }
    return pid > 0;
  }
  // Waits for the process to end; returns the exit code, or -signal.
  int wait() {
    int st = 0;
    ::waitpid(pid, &st, 0);
    pid = -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -WTERMSIG(st);
  }
  bool exited_early(int* code) {
    int st = 0;
    if (::waitpid(pid, &st, WNOHANG) != pid) return false;
    pid = -1;
    *code = WIFEXITED(st) ? WEXITSTATUS(st) : -WTERMSIG(st);
    return true;
  }
};

// Starts the service and connects; false if it exited or never listened.
bool launch(const char* exe, const std::string& data, const std::string& sock, const std::string& tag, Proc& p, Client& c) {
  if (!p.start(exe, {"--data", data, "--socket", sock}, data + "/" + tag + ".log")) return false;
  for (int i = 0; i < 400; ++i) {
    int code = 0;
    if (p.exited_early(&code)) return false;
    if (c.connect(sock)) return c.handshake("smoke");
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return false;
}

Ids strs(const std::optional<ws::v1::LibraryServerMessage>& g, bool favorites) {
  if (!g || !g->has_state()) return {"<no state>"};
  return ltest::strs(favorites ? g->state().favorites() : g->state().recent());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: librarian_service_smoke <librarian-service>\n");
    return 2;
  }
  ::signal(SIGPIPE, SIG_IGN);
  const char* exe = argv[1];
  TempDir d;
  const std::string sock = d.sub("lib.sock");

  // Run 1: favorites and Recent over the real socket, then SIGTERM.
  {
    Proc p;
    Client c;
    CHECK(launch(exe, d.path, sock, "run1", p, c));
    CHECK(c.add_fav(uid(1))->ack().changed());
    CHECK(c.add_fav(uid(2))->ack().changed());
    CHECK(c.report(uid(7))->ack().changed());
    CHECK(c.report(uid(8))->ack().changed());
    auto g = c.get();
    CHECK((strs(g, true) == Ids{uid(1), uid(2)}) && (strs(g, false) == Ids{uid(8), uid(7)}));
    // A second service on the same data directory is refused with exit code 1 and a clear message.
    {
      Proc q;
      CHECK(q.start(exe, {"--data", d.path, "--socket", d.sub("other.sock")}, d.sub("second.log")));
      CHECK(q.wait() == 1);
      CHECK(ltest::slurp(d.sub("second.log")).find("already running") != std::string::npos);
    }
    c.close();
    ::kill(p.pid, SIGTERM);
    CHECK(p.wait() == 0);
    CHECK(::access(sock.c_str(), F_OK) != 0);  // the socket is removed
    CHECK(ltest::slurp(d.sub("run1.log")).find("stopped") != std::string::npos);
  }
  // Run 2: persistence; Recent reported while "playing" is RAM only and dies with a SIGKILL, committed data does not.
  {
    Proc p;
    Client c;
    CHECK(launch(exe, d.path, sock, "run2", p, c));
    auto g = c.get();
    CHECK((strs(g, true) == Ids{uid(1), uid(2)}) && (strs(g, false) == Ids{uid(8), uid(7)}));
    CHECK(c.add_fav(uid(3))->ack().changed());  // committed
    CHECK(c.playing(true)->ack().changed());
    CHECK(c.report(uid(9))->ack().changed());   // RAM only
    g = c.get();
    CHECK((strs(g, false) == Ids{uid(9), uid(8), uid(7)}) && g->state().recent_unwritten());
    ::kill(p.pid, SIGKILL);
    CHECK(p.wait() == -SIGKILL);
  }
  // Run 3: after the kill -9. Then the same sequence, ended with SIGTERM: the orderly shutdown writes the pending Recent.
  {
    Proc p;
    Client c;
    CHECK(launch(exe, d.path, sock, "run3", p, c));  // the stale socket file is replaced
    auto g = c.get();
    CHECK((strs(g, true) == Ids{uid(1), uid(2), uid(3)}));
    CHECK((strs(g, false) == Ids{uid(8), uid(7)}));  // uid(9) was never written
    CHECK(c.playing(true)->ack().changed());
    CHECK(c.report(uid(9))->ack().changed());
    c.close();
    ::kill(p.pid, SIGTERM);
    CHECK(p.wait() == 0);
  }
  {
    Proc p;
    Client c;
    CHECK(launch(exe, d.path, sock, "run4", p, c));
    auto g = c.get();
    CHECK((strs(g, false) == Ids{uid(9), uid(8), uid(7)}));
    c.close();
    ::kill(p.pid, SIGTERM);
    CHECK(p.wait() == 0);
  }
  // Story 5.6: --catalog-dir splits the catalog from the library root. catalog.db / catalog.lock go to the catalog directory; lib/ and its .service.lock stay
  // under --data; nothing is created in the wrong place. (This real binary links Homebrew's protobuf next to formatlib's vendored abseil: starting it and
  // talking to it is also the link/ODR smoke of velora_librarian_service.)
  {
    TempDir root;
    const std::string cat = root.sub("catalog");
    ::mkdir(cat.c_str(), 0755);
    const std::string sk = root.sub("s.sock");
    Proc p;
    CHECK(p.start(exe, {"--data", root.path, "--catalog-dir", cat, "--socket", sk}, root.sub("split.log")));
    Client c;
    bool up = false;
    for (int i = 0; i < 400 && !up; ++i) {
      up = c.connect(sk) && c.handshake("smoke");
      if (!up) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    CHECK(up);
    CHECK(c.add_fav(uid(1))->ack().changed());
    CHECK(::access((cat + "/catalog.db").c_str(), F_OK) == 0 && ::access((cat + "/catalog.lock").c_str(), F_OK) == 0);
    CHECK(::access((root.path + "/catalog.db").c_str(), F_OK) != 0 && ::access((root.path + "/catalog.lock").c_str(), F_OK) != 0);
    CHECK(::access((root.path + "/lib/.service.lock").c_str(), F_OK) == 0);
    CHECK(::access((cat + "/lib").c_str(), F_OK) != 0);
    c.close();
    ::kill(p.pid, SIGTERM);
    CHECK(p.wait() == 0);
  }
  // Exit codes: usage 2, a data directory that does not exist 1, a foreign catalog 1 (and the file is left alone).
  {
    Proc p;
    CHECK(p.start(exe, {"--bogus"}, d.sub("usage.log")));
    CHECK(p.wait() == 2);
    CHECK(p.start(exe, {"--data", "/tmp/vl.does-not-exist.xyz", "--socket", d.sub("x.sock")}, d.sub("nodir.log")));
    CHECK(p.wait() == 1);
    TempDir d2;
    { std::ofstream(d2.sub("catalog.db")) << "not a database"; }
    CHECK(p.start(exe, {"--data", d2.path, "--socket", d2.sub("x.sock")}, d2.sub("foreign.log")));
    CHECK(p.wait() == 1);
    CHECK(ltest::slurp(d2.sub("catalog.db")) == "not a database");
  }
  if (g_fail) {
    std::printf("librarian_service_smoke: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("librarian_service_smoke: ok\n");
  return 0;
}
