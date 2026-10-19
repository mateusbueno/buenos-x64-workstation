// Smoke test of the real engine-host wiring of Story 4.10: `engine-host --data-dir D --library-root R --no-audio` in a subprocess, a ws.v1 client edits the ASSIGN maps.
//  * SetAssignSlot on the current Live Set is a dirty in-memory edit (pushed to a subscriber, change key assign.slot); the file is untouched until Write.
//  * SetAssignSlot on the global default writes assign_default.pb at once (no Write) and does not touch the Live Set or its dirty flag.
//  * Typed refusals: slot out of range, a non-assignable parameter.
//  * SetEncoderMode reaches the real-time plane through the control lane (State.encoders.rt_mode, seen after the Write's fence rendered blocks).
//  * WriteLiveSet WHOLE_LIVE_SET persists the map; an unwritten map edit is gone after a restart, the written map and the default are there, and a Live Set
//    created after the restart copies the persisted default.
// Removing the commands, their wiring in engine-host (edit session, stores) or the persistence fails this test. Runs engine-host --no-audio.
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
#include <initializer_list>
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


static ws::v1::ClientMessage assign_msg(ws::v1::AssignTarget t, std::uint32_t slot, std::uint32_t param) {
  ws::v1::ClientMessage m = mk();
  m.mutable_set_assign_slot()->set_target(t);
  m.mutable_set_assign_slot()->set_slot(slot);
  m.mutable_set_assign_slot()->set_param_id(param);
  return m;
}

static bool map_is(const ws::v1::State& st, std::initializer_list<std::uint32_t> want) {
  if (st.encoders().assign_size() != 8) return false;
  int k = 0;
  for (std::uint32_t v : want) {
    if (st.encoders().assign(k) != v) return false;
    ++k;
  }
  for (; k < 8; ++k)
    if (st.encoders().assign(k) != 0) return false;
  return true;
}

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: engine_host_assign_smoke ENGINE_HOST LIVE_SET_FIXTURE_DIR LIBRARY_ROOT\n");
    return 2;
  }
  char tmpl[] = "/tmp/velora-ehassignsmoke-XXXXXX";
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
  const std::uint32_t kDrawbar8 = 102, kSendReverb = 400, kDrawbar2 = 105, kRotaryOn = 201, kLineIn = 800;
  const std::uint32_t factory[8] = {401, 400, 113, 112, 116, 117, 200, 115};
  std::string a_id, a_path;
  std::string default_path = data + "/setlists/assign_default.pb";
  if (!eng.start(argv[1], data, argv[3], "e1")) {
    fail("engine-host did not start");
  } else if (!hello(eng.fd)) {
    fail("hello");
  } else {
    ws::v1::ServerMessage r;
    if (!state(eng.fd, r) || r.state().live_set_name() != "Switch A" || !map_is(r.state(), {}) || r.state().encoders().mode() != ws::v1::ENCODER_MODE_PART ||
        !r.state().encoders().assign_default_available() || r.state().encoders().assign_default_size() != 8)
      fail("initial State.encoders");
    else {
      for (int k = 0; k < 8; ++k)
        if (r.state().encoders().assign_default(k) != factory[k]) fail("the default is not the factory default");
      a_id = r.state().live_set_id();
    }
    a_path = data + "/live_sets/switch_a.liveset";
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
    if (ok) {  // the current Live Set's map: a dirty in-memory edit
      if (!call(eng.fd, assign_msg(ws::v1::ASSIGN_TARGET_CURRENT_LIVE_SET, 1, kDrawbar8), r) || !r.has_ack() ||
          !call(eng.fd, assign_msg(ws::v1::ASSIGN_TARGET_CURRENT_LIVE_SET, 2, kSendReverb), r) || !r.has_ack())
        fail("SetAssignSlot on the current Live Set");
      else if (!wait_push(sub, [&](const ws::v1::State& st) { return st.edits().current_dirty() && map_is(st, {kDrawbar8, kSendReverb}); }))
        fail("no State push with the new map and the dirty flag");
    }
    if (ok && (!state(eng.fd, r) || !r.state().edits().current_dirty() || r.state().edits().change_summary_text().find("ASSIGN K1") == std::string::npos))
      fail("the change summary does not name ASSIGN K1");
    if (ok) {  // the file is untouched
      velora::liveset::v1::LiveSet now;
      if (!read_set(a_path, now) || now.assign(0) != 0 || now.assign(1) != 0) fail("the map reached the file before any Write");
    }
    if (ok) {  // the global default: written at once, no Write; the current Live Set is not touched
      if (fs::exists(default_path)) fail("assign_default.pb exists before any edit");
      if (!call(eng.fd, assign_msg(ws::v1::ASSIGN_TARGET_GLOBAL_DEFAULT, 8, kRotaryOn), r) || !r.has_ack()) fail("SetAssignSlot on the global default");
      else if (!fs::exists(default_path)) fail("the global default was not written at once");
      else if (!state(eng.fd, r) || r.state().encoders().assign_default(7) != kRotaryOn || !map_is(r.state(), {kDrawbar8, kSendReverb}) || r.state().edits().dirty_count() != 1)
        fail("State after the default edit");
    }
    if (ok) {  // typed refusals change nothing
      if (!call(eng.fd, assign_msg(ws::v1::ASSIGN_TARGET_CURRENT_LIVE_SET, 9, kDrawbar8), r) || !r.has_error() || r.error().code() != ws::v1::ERROR_CODE_INVALID_SLOT)
        fail("slot 9 must be INVALID_SLOT");
      if (!call(eng.fd, assign_msg(ws::v1::ASSIGN_TARGET_GLOBAL_DEFAULT, 1, kLineIn), r) || !r.has_error() || r.error().code() != ws::v1::ERROR_CODE_NOT_ASSIGNABLE)
        fail("line_in_level must be NOT_ASSIGNABLE");
      if (!call(eng.fd, assign_msg(ws::v1::ASSIGN_TARGET_UNSPECIFIED, 1, kDrawbar8), r) || !r.has_error() || r.error().code() != ws::v1::ERROR_CODE_INVALID_COMMAND)
        fail("no target must be INVALID_COMMAND");
    }
    if (ok) {  // the encoder mode reaches RT through the control lane; --no-audio renders blocks only while a Write waits for its fence
      m = mk();
      m.mutable_set_encoder_mode()->set_mode(ws::v1::ENCODER_MODE_ASSIGN);
      if (!call(eng.fd, m, r) || !r.has_ack() || !state(eng.fd, r) || r.state().encoders().mode() != ws::v1::ENCODER_MODE_ASSIGN) fail("SetEncoderMode");
      m = mk();
      m.mutable_select_part()->set_part(3);
      if (!call(eng.fd, m, r) || !r.has_ack()) fail("SelectPart");
    }
    if (ok) {  // Write persists the map; its fence applied the mode and the part events
      m = mk();
      m.mutable_write_live_set()->set_scope(ws::v1::WRITE_SCOPE_WHOLE_LIVE_SET);
      if (!call(eng.fd, m, r) || !r.has_write_reply() || r.write_reply().current_dirty() || r.write_reply().written_count() != 2) fail("WriteLiveSet WHOLE_LIVE_SET");
      else {
        a_path = data + "/live_sets/" + a_id + ".liveset";
        velora::liveset::v1::LiveSet now;
        if (!read_set(a_path, now) || now.assign(0) != kDrawbar8 || now.assign(1) != kSendReverb || now.assign(2) != 0) fail("the written file does not hold the map");
      }
      if (ok && (!state(eng.fd, r) || r.state().edits().current_dirty() || !map_is(r.state(), {kDrawbar8, kSendReverb}) || r.state().encoders().rt_mode() != ws::v1::ENCODER_MODE_ASSIGN ||
                 r.state().encoders().rt_selected_part() != 3))
        fail("State after the Write (clean, map kept, the real-time plane applied the mode and the part)");
    }
    if (ok) {  // an edit that is not written
      if (!call(eng.fd, assign_msg(ws::v1::ASSIGN_TARGET_CURRENT_LIVE_SET, 3, kDrawbar2), r) || !r.has_ack() || !state(eng.fd, r) || !r.state().edits().current_dirty())
        fail("an unwritten map edit");
    }
    if (sub >= 0) ::close(sub);
  }
  eng.stop();

  // Restart: the unwritten slot is gone; the written map and the default are there; a new Live Set copies the persisted default.
  if (ok) {
    if (!eng.start(argv[1], data, argv[3], "e2")) {
      fail("restart");
    } else if (!hello(eng.fd)) {
      fail("hello after restart");
    } else {
      ws::v1::ServerMessage r;
      velora::liveset::v1::LiveSet now;
      if (!read_set(a_path, now) || now.assign(2) != 0) fail("the unwritten edit reached the file");
      if (ok && (!state(eng.fd, r) || r.state().edits().current_dirty() || !map_is(r.state(), {kDrawbar8, kSendReverb}) || r.state().encoders().assign_default(7) != kRotaryOn ||
                 r.state().encoders().mode() != ws::v1::ENCODER_MODE_PART))
        fail("State after the restart: map, default, mode (the encoder mode is engine state, not persisted)");
      if (ok) {
        ws::v1::ClientMessage m = mk();
        m.mutable_create_live_set()->set_name("Smoke new");
        if (!call(eng.fd, m, r) || !r.has_created_reply()) fail("CreateLiveSet");
        else {
          velora::liveset::v1::LiveSet created;
          if (!read_set(data + "/live_sets/" + r.created_reply().id() + ".liveset", created) || created.assign(7) != kRotaryOn || created.assign(0) != factory[0])
            fail("the new Live Set does not copy the persisted default");
        }
      }
    }
    eng.stop();
  }
  std::error_code ec;
  fs::remove_all(data, ec);
  std::printf(ok ? "engine_host_assign_smoke: ok\n" : "engine_host_assign_smoke: FAILED\n");
  return ok ? 0 : 1;
}
