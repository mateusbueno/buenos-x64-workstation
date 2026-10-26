// Smoke test of Story 8.9 with the real binaries: `engine-host --data-dir D --library-root R [--sysd-socket S]` boots into the target the sysd `boot_into` setting and
// the Boot-into pointer (Story 8.8) name: LAST_SOUND (pointer's Live Set, scene and setlist), START_OF_SETLIST (first Live Set of the pointer's setlist),
// DEFAULT_LIVE_SET (the configured Live Set at scene 1, NO SETLIST), the fallbacks (deleted target, corrupt / missing pointer, deleted setlist, sysd absent: bounded
// wait, LAST_SOUND), an empty library (an empty new Live Set), and the stored pointer is not overwritten by the boot position, only by a real user change.
// Runs engine-host --no-audio, velora-sysd and velora-sysdctl.
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
#include <vector>
#include <memory>
#include <optional>

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
  std::vector<std::string> extra;  // more engine-host arguments (--sysd-socket ...)
  bool start(const char* exe, const std::string& data, const std::string& lib, const std::string& tag, int* exit_code = nullptr) {
    sock = data + "/" + tag + ".sock";
    log = data + "/" + tag + ".log";
    pid = ::fork();
    if (pid == 0) {
      const int lf = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
      ::dup2(lf, 1);
      ::dup2(lf, 2);
      ::dup2(::open("/dev/null", O_RDONLY), 0);
      std::vector<std::string> a = {exe, "--no-audio", "--duration", "60", "--data-dir", data, "--library-root", lib, "--socket", sock};
      for (const std::string& x : extra) a.push_back(x);
      std::vector<char*> av;
      for (std::string& x : a) av.push_back(x.data());
      av.push_back(nullptr);
      ::execv(exe, av.data());
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


#include "velora/host/boot_into_store.hpp"
#include "velora/host/live_set_store.hpp"
#include "velora/host/live_set_switcher.hpp"
#include "velora/host/setlist_store.hpp"

namespace bi = velora::host::bootinto;
namespace hs = velora::host;

static int g_fail = 0;
#define CHECK(c)                                               \
  do {                                                         \
    if (!(c)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

static std::string slurp_file(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static pid_t spawn_quiet(const std::vector<std::string>& args, const std::string& log) {
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

static int run_ctl(const std::vector<std::string>& args, const std::string& log) {
  const pid_t pid = spawn_quiet(args, log);
  int st = 0;
  ::waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

struct Sysd {
  pid_t pid = -1;
  std::string sock;
  bool start(const std::string& exe, const std::string& base) {
    const std::string dir = base + "/sysd";
    sock = base + "/sysd.sock";
    fs::create_directories(dir);
    pid = spawn_quiet({exe, "--data-dir", dir, "--socket", sock}, base + "/sysd.log");
    for (int i = 0; i < 100; ++i) {
      if (fs::exists(sock)) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
  }
  void stop() {
    if (pid > 0) {
      ::kill(pid, SIGTERM);
      int st = 0;
      ::waitpid(pid, &st, 0);
    }
    pid = -1;
  }
};

static std::string g_exe, g_sysd, g_ctl, g_lib, g_fixtures, g_scenes;

struct Root {
  std::string dir;
  std::vector<hs::CatalogEntry> sorted;  // the fixture Live Sets by name
  std::string setlist;                   // a setlist [second, first] (when made)
  std::string scenes_id;                 // the 3-scene "Scenes show" fixture (with_scenes); NOT part of `sorted`, which is the two switch fixtures
  explicit Root(bool with_live_sets = true, bool with_setlist = true, bool with_scenes = false) {
    char tmpl[] = "/tmp/velora-ehbt-XXXXXX";
    if (!mkdtemp(tmpl)) std::abort();
    dir = tmpl;
    fs::create_directories(dir + "/live_sets");
    fs::create_directories(dir + "/setlists");
    if (with_live_sets)
      for (const auto& e : fs::directory_iterator(g_fixtures))
        if (e.path().extension() == ".liveset") fs::copy_file(e.path(), dir + "/live_sets/" + e.path().filename().string());
    sorted = hs::scan_live_set_dir(dir + "/live_sets", hs::liveset::kV1PartCount);
    if (with_scenes) {
      fs::copy_file(g_scenes, dir + "/live_sets/" + fs::path(g_scenes).filename().string());
      for (const auto& e : hs::scan_live_set_dir(dir + "/live_sets", hs::liveset::kV1PartCount))
        if (e.name == "Scenes show") scenes_id = e.id;
      sorted.erase(std::remove_if(sorted.begin(), sorted.end(), [&](const hs::CatalogEntry& e) { return e.id == scenes_id; }), sorted.end());
    }
    if (with_live_sets && with_setlist && sorted.size() >= 2) {
      hs::setlist::StoreOptions so;
      so.dir = dir + "/setlists";
      std::unique_ptr<hs::setlist::SetlistStore> st;
      if (hs::setlist::SetlistStore::open(so, st) != hs::setlist::Status::kOk) std::abort();
      if (st->create_setlist("Gig", setlist) != hs::setlist::Status::kOk) std::abort();
      if (st->add_live_set(setlist, sorted[1].id, std::nullopt) != hs::setlist::Status::kOk) std::abort();
      if (st->add_live_set(setlist, sorted[0].id, std::nullopt) != hs::setlist::Status::kOk) std::abort();
    }
  }
  ~Root() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  void write_pointer(const std::string& live, std::uint32_t scene, const std::string& sl) const {
    bi::StoreOptions o;
    o.dir = dir;
    bi::BootIntoStore s(o);
    bi::Pointer p;
    p.live_set_id = live;
    p.scene_index = scene;
    p.setlist_id = sl;
    if (s.write(p) != hs::liveset::Status::kOk) std::abort();
  }
  std::string pointer_bytes() const { return slurp_file(dir + "/" + bi::kFileName); }
};

struct Booted {
  std::string live_id, live_name, setlist_id, log;
  int scene = -2;
  std::uint32_t scene_count = 0;
  bool ok = false;
};

// Boots an engine on `root` (optionally with sysd), reads the State once, stops it.
static Booted boot(const Root& root, const std::string& sysd_sock, const char* tag) {
  Booted b;
  Engine e;
  if (!sysd_sock.empty()) e.extra = {"--sysd-socket", sysd_sock};
  if (!e.start(g_exe.c_str(), root.dir, g_lib, tag) || !hello(e.fd)) {
    b.log = e.slurp();
    e.stop();
    return b;
  }
  ws::v1::ServerMessage r;
  // the Live Set that boots is loaded before the control server answers
  if (state(e.fd, r)) {
    b.live_id = r.state().live_set_id();
    b.live_name = r.state().live_set_name();
    b.setlist_id = r.state().setlists().loaded_setlist_id();
    b.scene = r.state().current_scene();
    b.scene_count = r.state().scene_count();
    b.ok = true;
  }
  b.log = e.slurp();
  e.stop();
  return b;
}

static bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

int main(int argc, char** argv) {
  if (argc != 7) {
    std::fprintf(stderr, "usage: engine_host_boot_smoke ENGINE_HOST VELORA_SYSD VELORA_SYSDCTL LIVE_SET_FIXTURE_DIR LIBRARY_ROOT SCENES_LIVESET\n");
    return 2;
  }
  g_exe = argv[1];
  g_sysd = argv[2];
  g_ctl = argv[3];
  g_fixtures = argv[4];
  g_lib = argv[5];
  g_scenes = argv[6];
  const std::string GONE = "e0000000-0000-4000-8000-0000000000ff";

  {  // No sysd, no pointer: LAST_SOUND with nothing remembered = the first Live Set of All Live Sets.
    Root r;
    CHECK(r.sorted.size() >= 2);
    const Booted b = boot(r, "", "n1");
    CHECK(b.ok && b.live_id == r.sorted[0].id && b.setlist_id.empty());
    CHECK(has(b.log, "boot target: first_of_all"));
    CHECK(r.pointer_bytes().empty());  // booting alone writes no pointer
  }
  {  // LAST_SOUND with a pointer: its Live Set, scene and setlist.
    Root r;
    r.write_pointer(r.sorted[1].id, 0, r.setlist);
    const Booted b = boot(r, "", "n2");
    CHECK(b.ok && b.live_id == r.sorted[1].id && b.setlist_id == r.setlist);
    CHECK(has(b.log, "boot target: pointer"));
  }
  {  // Scenes (the 3-scene fixture): LAST_SOUND restores a non-zero scene; a scene the Live Set does not have is scene 1; DEFAULT / START start at scene 1.
    Root r(true, true, true);
    CHECK(!r.scenes_id.empty());
    r.write_pointer(r.scenes_id, 2, "");
    Booted b = boot(r, "", "sc1");
    CHECK(b.ok && b.live_id == r.scenes_id && b.scene_count == 3 && b.scene == 2);
    r.write_pointer(r.scenes_id, 99, "");
    b = boot(r, "", "sc2");
    CHECK(b.ok && b.live_id == r.scenes_id && b.scene_count == 3 && b.scene == 0 && has(b.log, "does not exist"));
    Sysd sd;
    CHECK(sd.start(g_sysd, r.dir));
    CHECK(run_ctl({g_ctl, "--socket", sd.sock, "set", "default_live_set_id", r.scenes_id}, r.dir + "/ctl.log") == 0);
    CHECK(run_ctl({g_ctl, "--socket", sd.sock, "set", "boot_into", "default_live_set"}, r.dir + "/ctl.log") == 0);
    r.write_pointer(r.scenes_id, 2, "");  // the pointer is on scene 3, the default starts at scene 1
    b = boot(r, sd.sock, "sc3");
    CHECK(b.ok && b.live_id == r.scenes_id && b.scene == 0);
    sd.stop();
  }
  {  // The pointer's setlist exists but no longer holds the Live Set (an off-script position): it still boots on that Live Set, with the setlist loaded.
    Root r;
    r.write_pointer(r.sorted[1].id, 0, r.setlist);  // the setlist is [sorted[1], sorted[0]]; remove sorted[1] from it
    {
      hs::setlist::StoreOptions so;
      so.dir = r.dir + "/setlists";
      std::unique_ptr<hs::setlist::SetlistStore> st;
      CHECK(hs::setlist::SetlistStore::open(so, st) == hs::setlist::Status::kOk);
      CHECK(st->remove_live_set(r.setlist, r.sorted[1].id) == hs::setlist::Status::kOk);
    }
    const Booted b = boot(r, "", "n2c");
    CHECK(b.ok && b.live_id == r.sorted[1].id && b.setlist_id == r.setlist);
  }
  {  // Deleted target / deleted setlist / corrupt pointer: fallbacks, boot ends up with a sounding Live Set.
    Root r;
    r.write_pointer(GONE, 3, r.setlist);
    Booted b = boot(r, "", "n3");
    CHECK(b.ok && b.live_id == r.sorted[0].id && b.setlist_id.empty() && has(b.log, "no longer exists"));
    r.write_pointer(r.sorted[1].id, 0, "00000000-0000-4000-8000-00000000dead");
    b = boot(r, "", "n3b");
    CHECK(b.ok && b.live_id == r.sorted[1].id && b.setlist_id.empty() && has(b.log, "no longer exists: NO SETLIST"));
    { std::ofstream f(r.dir + "/" + bi::kFileName, std::ios::binary | std::ios::trunc); f << "not a pointer"; }
    b = boot(r, "", "n3c");
    CHECK(b.ok && b.live_id == r.sorted[0].id && has(b.log, "no Boot-into pointer"));
  }
  {  // Missing library: the target loads; every part that needs the absent library is "missing library" (silent, no substitution); the boot still ends on that Live Set.
    Root r;
    r.write_pointer(r.sorted[1].id, 0, "");
    const std::string saved = g_lib;
    g_lib = r.dir + "/no-library-here";
    fs::create_directories(g_lib);
    const Booted b = boot(r, "", "ml");
    g_lib = saved;
    CHECK(b.ok && b.live_id == r.sorted[1].id && has(b.log, "live set part 0: missing library") && b.log.find("substitut") == std::string::npos);
  }
  {  // Empty library: an empty new Live Set, the unit still sounds one.
    Root r(false, false);
    const Booted b = boot(r, "", "n4");
    CHECK(b.ok && !b.live_id.empty() && b.live_name == "Live Set" && has(b.log, "made an empty Live Set"));
    const Booted b2 = boot(r, "", "n4b");  // the next boot starts on it (it exists now)
    CHECK(b2.ok && b2.live_id == b.live_id);
  }
  {  // sysd configured but absent: bounded wait, then LAST_SOUND.
    Root r;
    r.write_pointer(r.sorted[1].id, 0, "");
    const auto t0 = std::chrono::steady_clock::now();
    const Booted b = boot(r, r.dir + "/none.sock", "n5");
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(b.ok && b.live_id == r.sorted[1].id && has(b.log, "sysd did not answer within"));
    CHECK(ms < 15000);  // no stall (generous; no timing claim)
  }
  {  // With sysd: each mode.
    Root r;
    Sysd sd;
    CHECK(sd.start(g_sysd, r.dir));
    r.write_pointer(r.sorted[0].id, 0, r.setlist);  // the last sound was in the setlist (which starts with the second Live Set by name)
    Booted b = boot(r, sd.sock, "s1");  // sysd default: LAST_SOUND
    CHECK(b.ok && b.live_id == r.sorted[0].id && b.setlist_id == r.setlist);
    CHECK(run_ctl({g_ctl, "--socket", sd.sock, "set", "boot_into", "start_of_setlist"}, r.dir + "/ctl.log") == 0);
    b = boot(r, sd.sock, "s2");
    CHECK(b.ok && b.live_id == r.sorted[1].id && b.setlist_id == r.setlist && has(b.log, "boot target: start_of_setlist"));
    CHECK(run_ctl({g_ctl, "--socket", sd.sock, "set", "default_live_set_id", r.sorted[1].id}, r.dir + "/ctl.log") == 0);
    CHECK(run_ctl({g_ctl, "--socket", sd.sock, "set", "boot_into", "default_live_set"}, r.dir + "/ctl.log") == 0);
    const std::string before = r.pointer_bytes();
    b = boot(r, sd.sock, "s3");
    CHECK(b.ok && b.live_id == r.sorted[1].id && b.setlist_id.empty() && has(b.log, "boot target: default_live_set"));
    CHECK(r.pointer_bytes() == before);
    // the default Live Set was deleted: fallback to the first of All Live Sets
    fs::remove(r.dir + "/live_sets/" + fs::path(r.sorted[1].path).filename().string());
    b = boot(r, sd.sock, "s4");
    CHECK(b.ok && b.live_id == r.sorted[0].id && has(b.log, "does not exist"));
    // an invalid default id is refused by sysd
    CHECK(run_ctl({g_ctl, "--socket", sd.sock, "set", "default_live_set_id", "live-1"}, r.dir + "/ctl.log") == 1);
    sd.stop();
  }
  {  // The pointer is not overwritten by the boot position, only by a real user change.
    Root r;
    Sysd sd;
    CHECK(sd.start(g_sysd, r.dir));
    CHECK(run_ctl({g_ctl, "--socket", sd.sock, "set", "default_live_set_id", r.sorted[0].id}, r.dir + "/ctl.log") == 0);
    CHECK(run_ctl({g_ctl, "--socket", sd.sock, "set", "boot_into", "default_live_set"}, r.dir + "/ctl.log") == 0);
    r.write_pointer("e0000000-0000-4000-8000-0000000000ff", 0, "");  // a Live Set that is not there: no jump below can be "identical" to it
    const std::string stored = r.pointer_bytes();
    Engine e;
    e.extra = {"--sysd-socket", sd.sock};
    CHECK(e.start(g_exe.c_str(), r.dir, g_lib, "p1") && hello(e.fd));
    ws::v1::ServerMessage rep;
    CHECK(state(e.fd, rep) && rep.state().live_set_id() == r.sorted[0].id);
    std::this_thread::sleep_for(std::chrono::milliseconds(3500));  // past the 2 s idle rule
    CHECK(r.pointer_bytes() == stored);
    // a real user jump (to the other Live Set): the pointer follows once the unit is idle
    ws::v1::ClientMessage m = mk();
    m.mutable_jump_to_live_set()->set_id(r.sorted[1].id);
    CHECK(call(e.fd, m, rep) && rep.has_jump_reply());
    bool followed = false;
    for (int i = 0; i < 80 && !followed; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      bi::Pointer p;
      followed = bi::load(r.dir, p) == bi::LoadStatus::kOk && p.live_set_id == r.sorted[1].id;
    }
    CHECK(followed);
    e.stop();
    sd.stop();
  }
  std::printf(g_fail ? "engine_host_boot_smoke: FAILED\n" : "engine_host_boot_smoke: ok\n");
  return g_fail ? 1 : 0;
}
