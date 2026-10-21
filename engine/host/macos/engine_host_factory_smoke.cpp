// Smoke test of the real engine-host first-boot import of the signed factory Live Sets (Story 6.9): `engine-host --data-dir D --library-root R --trust-list T --dev-device-key K
// --factory-live-sets F` in a subprocess over a PROTECTED library laid out as the librarian installs it. First boot: the Live Sets are imported through the store and the engine
// loads the first one (its protected part is ready). Second boot: nothing is imported again. A file naming an instrument that is not installed is refused with the typed cause and
// the missing (library_id, instrument_key), writes nothing, and the engine still starts with what it has. A file signed by a key that is not trusted is refused. --no-audio.
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

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

#include "../tests/protected_fixtures.hpp"
#include "velora/format/protected_writer.hpp"
#include "velora/host/factory_live_set.hpp"
#include "velora/host/live_set_store.hpp"

namespace fs = std::filesystem;
namespace ls = velora::host::liveset;
namespace hf = velora::host::factory;
namespace fmt = velora::format;

static std::string slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Runs engine-host to completion (--duration 2) with its output in `log`; returns the exit status or -1.
static int run_engine(const char* exe, const std::string& log, const std::vector<std::string>& args) {
  const pid_t pid = ::fork();
  if (pid == 0) {
    const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
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
  int st = 0;
  for (int i = 0; i < 400; ++i) {
    if (::waitpid(pid, &st, WNOHANG) == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ::kill(pid, SIGKILL);
  ::waitpid(pid, &st, 0);
  return -1;
}

static std::string sign_file(const fmt::SigningSeed& seed, const velora::liveset::v1::FactoryLiveSets& sets) {
  const fmt::KeyId id = fmt::key_id_of_public(fmt::ed25519_public_from_seed(seed));
  std::string payload;
  if (!hf::serialize_payload(sets, payload)) std::abort();
  auto sig = fmt::ed25519_sign(seed, hf::signing_statement(id, payload));
  if (!sig) std::abort();
  return hf::assemble_container(id, sig.value(), payload);
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: engine_host_factory_smoke ENGINE_HOST\n");
    return 2;
  }
  char dirb[] = "/tmp/velora-ehf-XXXXXX";
  if (!mkdtemp(dirb)) return 2;
  const std::string dir = dirb;
  auto finish = [&](bool ok, const char* why) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    std::printf(ok ? "engine_host_factory_smoke: ok\n" : "engine_host_factory_smoke: FAILED (%s)\n", why);
    return ok ? 0 : 1;
  };

  const std::string root = dir + "/root", data = dir + "/data";
  const pfx::Keys keys = pfx::make_keys(5);
  const auto lib = pfx::install_library(root, 1, {{"piano.sfz", pfx::Spec::kConstFloat, 0.1f}}, keys);
  pfx::write_file(dir + "/device.x25519.priv", pfx::Bytes(keys.device_priv.begin(), keys.device_priv.end()));
  {
    std::ofstream t(dir + "/dev.pub-list");
    t << "dev content-manifest " << fmt::to_hex(keys.sign_id) << " " << fmt::to_hex(keys.sign_pub) << "\n";
  }
  auto make_ls = [&](std::uint32_t program, const std::string& name, const std::string& lib_id, const std::string& key) {
    velora::liveset::v1::LiveSet l = ls::make_default_live_set(hf::derive_live_set_id(program), name, 8);
    l.mutable_parts(0)->mutable_timbre()->set_library_id(lib_id);
    l.mutable_parts(0)->mutable_timbre()->set_instrument_key(key);
    l.mutable_factory()->set_program_id(program);
    l.mutable_factory()->set_release_version("1.0.0");
    return l;
  };
  auto sets_of = [&](std::vector<velora::liveset::v1::LiveSet> v) {
    velora::liveset::v1::FactoryLiveSets s;
    s.set_format_version(1);
    s.set_release_name("Smoke release");
    s.set_release_version("1.0.0");
    s.set_root("dev");
    for (auto& l : v) {
      auto* e = s.add_entries();
      e->set_program_id(l.factory().program_id());
      *e->mutable_live_set() = l;
    }
    return s;
  };
  auto write = [&](const std::string& path, const std::string& bytes) { std::ofstream(path, std::ios::binary | std::ios::trunc).write(bytes.data(), static_cast<std::streamsize>(bytes.size())); };
  write(dir + "/good.vfls", sign_file(keys.seed, sets_of({make_ls(1, "Factory Piano", lib.library_id, "piano.sfz")})));
  write(dir + "/missing.vfls", sign_file(keys.seed, sets_of({make_ls(1, "Factory Piano", lib.library_id, "piano.sfz"), make_ls(2, "Strings", "00000000-0000-4000-8000-0000000000ee", "gone.sfz")})));
  const pfx::Keys other = pfx::make_keys(9);
  write(dir + "/untrusted.vfls", sign_file(other.seed, sets_of({make_ls(1, "Factory Piano", lib.library_id, "piano.sfz")})));

  const std::vector<std::string> base = {"--no-audio", "--duration", "1", "--data-dir", data, "--library-root", root, "--socket", dir + "/e.sock", "--trust-list", dir + "/dev.pub-list",
                                         "--dev-device-key", dir + "/device.x25519.priv"};
  auto args = [&](const std::string& vfls) {
    std::vector<std::string> a = base;
    a.push_back("--factory-live-sets");
    a.push_back(vfls);
    return a;
  };
  const std::string id1 = hf::derive_live_set_id(1), id2 = hf::derive_live_set_id(2);

  // 1. A file naming an uninstalled instrument: refused with the cause and the missing pair; nothing written; the engine still runs.
  int rc = run_engine(argv[1], dir + "/r1.log", args(dir + "/missing.vfls"));
  std::string log = slurp(dir + "/r1.log");
  if (rc != 0 || log.find("unresolved_references") == std::string::npos || log.find("gone.sfz") == std::string::npos || log.find("00000000-0000-4000-8000-0000000000ee") == std::string::npos) {
    std::printf("%s\n", log.c_str());
    return finish(false, "missing library was not refused with the typed cause");
  }
  if (fs::exists(data + "/live_sets/" + id1 + ".liveset") || fs::exists(data + "/live_sets/" + hf::kStateFileName)) return finish(false, "a refused file wrote something");
  // 1b. The same with --factory-live-sets-required: fatal.
  {
    std::vector<std::string> a = args(dir + "/missing.vfls");
    a.push_back("--factory-live-sets-required");
    rc = run_engine(argv[1], dir + "/r1b.log", a);
    log = slurp(dir + "/r1b.log");
    if (rc != 1 || log.find("unresolved_references") == std::string::npos) {
      std::printf("%s\n", log.c_str());
      return finish(false, "--factory-live-sets-required did not make the refusal fatal");
    }
  }
  // 2. A signer that is not trusted.
  rc = run_engine(argv[1], dir + "/r2.log", args(dir + "/untrusted.vfls"));
  log = slurp(dir + "/r2.log");
  if (rc != 0 || log.find("untrusted_key") == std::string::npos || fs::exists(data + "/live_sets/" + id1 + ".liveset")) {
    std::printf("%s\n", log.c_str());
    return finish(false, "an untrusted signer was not refused");
  }
  // 3. First boot: imported, and the engine loads the factory Live Set (its protected part is ready).
  rc = run_engine(argv[1], dir + "/r3.log", args(dir + "/good.vfls"));
  log = slurp(dir + "/r3.log");
  if (rc != 0 || log.find("factory live sets: Smoke release 1.0.0: 1 imported, 0 updated, 0 unchanged") == std::string::npos || log.find("live set part 0: ready") == std::string::npos ||
      !fs::exists(data + "/live_sets/" + id1 + ".liveset") || !fs::exists(data + "/live_sets/" + hf::kStateFileName)) {
    std::printf("%s\n", log.c_str());
    return finish(false, "first boot did not import and load");
  }
  const std::string bytes1 = slurp(data + "/live_sets/" + id1 + ".liveset");
  // 4. Second boot: nothing imported again, the file untouched.
  rc = run_engine(argv[1], dir + "/r4.log", args(dir + "/good.vfls"));
  log = slurp(dir + "/r4.log");
  if (rc != 0 || log.find("0 imported, 0 updated, 1 unchanged") == std::string::npos || slurp(data + "/live_sets/" + id1 + ".liveset") != bytes1) {
    std::printf("%s\n", log.c_str());
    return finish(false, "second boot was not idempotent");
  }
  // 5. The user deleted it: a third boot does not bring it back.
  fs::remove(data + "/live_sets/" + id1 + ".liveset");
  rc = run_engine(argv[1], dir + "/r5.log", args(dir + "/good.vfls"));
  log = slurp(dir + "/r5.log");
  if (log.find("1 deleted by the user") == std::string::npos || fs::exists(data + "/live_sets/" + id1 + ".liveset")) {
    std::printf("%s\n", log.c_str());
    return finish(false, "a deleted factory Live Set came back");
  }
  (void)id2;
  return finish(true, "");
}
