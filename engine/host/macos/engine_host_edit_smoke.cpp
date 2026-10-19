// Smoke test of the real engine-host wiring of Story 4.9: `engine-host --data-dir D --library-root R --no-audio` in a subprocess, a ws.v1 client edits and writes.
//  * A structural edit (SetPartVolume) and a performance edit (SetPerformanceParam) show up in State.edits as a dirty Live Set with a change summary; a
//    subscriber receives the State push.
//  * WriteLiveSet WHOLE_LIVE_SET raises the fence (engine-host --no-audio renders the blocks the wait needs), writes the file atomically and clears the flag;
//    the file holds both edits.
//  * Edits are in memory only: an edit that was not written is gone after a restart (no autosave).
//  * WriteLiveSet SAVE_AS_NEW creates a new Live Set file with a new id and the given name; an invalid name is refused.
// Removing the edit session, its wiring in engine-host (stores, tick, fence idle hook) or the control->RT fence fails this test. Runs engine-host --no-audio.
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

#include "velora/host/live_set_store.hpp"
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


namespace ls = velora::host::liveset;

static bool read_set(const std::string& path, velora::liveset::v1::LiveSet& out) {
  ls::ReadOptions ro;
  ls::LoadResult lr;
  if (ls::read_file(path, ro, lr) != ls::Status::kOk) return false;
  out = lr.live_set;
  return true;
}

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: engine_host_edit_smoke ENGINE_HOST LIVE_SET_FIXTURE_DIR LIBRARY_ROOT\n");
    return 2;
  }
  char tmpl[] = "/tmp/velora-eheditsmoke-XXXXXX";
  if (!mkdtemp(tmpl)) return 2;
  const std::string data = tmpl;
  fs::create_directories(data + "/live_sets");
  for (const auto& e : fs::directory_iterator(argv[2]))
    if (e.path().extension() == ".liveset") fs::copy_file(e.path(), data + "/live_sets/" + e.path().filename().string());
  bool ok = true;
  Engine eng;
  auto fail = [&](const char* what) {
    std::printf("FAILED: %s\n%s\n", what, eng.slurp().c_str());
    ok = false;
  };
  std::string a_id, a_path;
  velora::liveset::v1::LiveSet before;
  if (!eng.start(argv[1], data, argv[3], "e1")) {
    fail("engine-host did not start");
  } else if (!hello(eng.fd)) {
    fail("hello");
  } else {
    ws::v1::ServerMessage r;
    if (!state(eng.fd, r) || !r.state().edits().available() || r.state().edits().current_dirty() || r.state().edits().dirty_count() != 0 || r.state().live_set_name() != "Switch A")
      fail("initial state: no edit session, or already dirty");
    else a_id = r.state().live_set_id();
    a_path = data + "/live_sets/switch_a.liveset";  // the fixture is not named <id>.liveset: the first Write moves it to its canonical name
    if (ok && !read_set(a_path, before)) fail("cannot read the Live Set file");
    // A subscriber sees the dirty flag arrive.
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
    ws::v1::ClientMessage m = mk();
    if (ok) {
      ws::v1::ServerMessage sr;
      m.mutable_subscribe()->set_enable(true);
      if (sub < 0 || !hello(sub) || !call(sub, m, sr) || !sr.has_ack()) fail("subscriber connect");
    }
    if (ok) {  // structural edit
      m = mk();
      m.mutable_set_part_volume()->set_part(0);
      m.mutable_set_part_volume()->set_volume(0.5f);
      if (!call(eng.fd, m, r) || !r.has_ack()) fail("SetPartVolume");
      else if (!wait_push(sub, [&](const ws::v1::State& st) { return st.edits().current_dirty() && st.edits().dirty_live_set_ids_size() == 1; }))
        fail("no State push with the dirty flag");
    }
    if (ok) {
      m = mk();
      m.mutable_set_performance_param()->set_param_id(ws::v1::PARAM_DRAWBAR_8);
      m.mutable_set_performance_param()->set_part(3);
      m.mutable_set_performance_param()->set_value(2);
      if (!call(eng.fd, m, r) || !r.has_ack()) fail("SetPerformanceParam");
    }
    if (ok && (!state(eng.fd, r) || !r.state().edits().current_dirty() || r.state().edits().change_summary_text().find("Part 1 volume") == std::string::npos ||
               r.state().edits().dirty_live_set_ids(0) != a_id))
      fail("State.edits after the structural edit");
    if (ok) {  // the file is untouched so far
      velora::liveset::v1::LiveSet now;
      if (!read_set(a_path, now) || now.SerializeAsString() != before.SerializeAsString()) fail("a file was written before any Write");
    }
    if (ok) {  // a refused scope changes nothing
      m = mk();
      m.mutable_write_live_set()->set_scope(ws::v1::WRITE_SCOPE_SCENE);
      if (!call(eng.fd, m, r) || !r.has_error() || r.error().code() != ws::v1::ERROR_CODE_WRITE_SCOPE_UNAVAILABLE) fail("Scene scope with no scene current must be refused");
    }
    if (ok) {  // Whole Live Set: the fence is acknowledged by the blocks engine-host renders while it waits
      m = mk();
      m.mutable_write_live_set()->set_scope(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET);
      if (!call(eng.fd, m, r) || !r.has_write_reply() || r.write_reply().written_live_set_id() != a_id || r.write_reply().current_dirty() || r.write_reply().written_count() < 2)
        fail("WriteLiveSet WHOLE_LIVE_SET");
      else {
        a_path = data + "/live_sets/" + a_id + ".liveset";
        if (fs::exists(data + "/live_sets/switch_a.liveset")) fail("the old file of the Live Set is still there beside the canonical one");
        velora::liveset::v1::LiveSet now;
        float drawbar = -1;
        if (read_set(a_path, now))
          for (const auto& e : now.parts(3).engine().params())
            if (e.param_id() == 102) drawbar = e.value();
        if (!read_set(a_path, now) || std::abs(now.parts(0).volume() - 0.5f) > 1e-6f || drawbar != 2.0f) fail("the written file does not hold the edits (volume, drawbar)");
      }
    }
    if (ok && (!state(eng.fd, r) || r.state().edits().current_dirty() || r.state().edits().dirty_count() != 0 || r.state().edits().writes_done() != 1)) fail("flags after the Write");
    if (ok) {  // an edit that is not written...
      m = mk();
      m.mutable_set_part_volume()->set_part(0);
      m.mutable_set_part_volume()->set_volume(0.9f);
      if (!call(eng.fd, m, r) || !r.has_ack() || !state(eng.fd, r) || !r.state().edits().current_dirty()) fail("second edit");
    }
    if (ok) {  // Save as new: refused name, then a good one
      m = mk();
      m.mutable_write_live_set()->set_scope(ws::v1::WRITE_SCOPE_SAVE_AS_NEW);
      m.mutable_write_live_set()->set_new_name(std::string(70, 'n'));
      if (!call(eng.fd, m, r) || !r.has_error() || r.error().code() != ws::v1::ERROR_CODE_INVALID_NAME) fail("a 70-byte name must be refused");
      m = mk();
      m.mutable_write_live_set()->set_scope(ws::v1::WRITE_SCOPE_SAVE_AS_NEW);
      m.mutable_write_live_set()->set_new_name("Smoke copy");
      if (!call(eng.fd, m, r) || !r.has_write_reply() || r.write_reply().written_live_set_id() == a_id || r.write_reply().written_name() != "Smoke copy" ||
          !r.write_reply().current_dirty())
        fail("SAVE_AS_NEW");
      else {
        velora::liveset::v1::LiveSet copy;
        if (!read_set(data + "/live_sets/" + r.write_reply().written_live_set_id() + ".liveset", copy) || copy.name() != "Smoke copy" || std::abs(copy.parts(0).volume() - 0.9f) > 1e-6f)
          fail("the new Live Set file");
      }
    }
    if (sub >= 0) ::close(sub);
  }
  eng.stop();

  // Restart: the unwritten edit (volume 0.9 of the original) is gone; what was written is there.
  if (ok) {
    if (!eng.start(argv[1], data, argv[3], "e2")) {
      fail("restart");
    } else if (!hello(eng.fd)) {
      fail("hello after restart");
    } else {
      velora::liveset::v1::LiveSet now;
      ws::v1::ServerMessage r;
      if (!read_set(a_path, now) || std::abs(now.parts(0).volume() - 0.5f) > 1e-6f) fail("the unwritten edit reached the file");
      if (ok && (!state(eng.fd, r) || r.state().edits().current_dirty() || r.state().edits().dirty_count() != 0)) fail("edits survived a restart");
    }
    eng.stop();
  }
  std::error_code ec;
  fs::remove_all(data, ec);
  std::printf(ok ? "engine_host_edit_smoke: ok\n" : "engine_host_edit_smoke: FAILED\n");
  return ok ? 0 : 1;
}
