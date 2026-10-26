// velora-sysd: the host process that owns the system configuration (Foundation: sysd config service). It serves ws.v1 config messages on a Unix domain
// socket and persists the config under <data-dir>/config/sysd_config.pb. It becomes a systemd unit on the board (sysd/README.md has an example).
//   velora-sysd --socket PATH --data-dir DIR [--duration SECONDS] [--slot-good]
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "velora/sysd/config_server.hpp"

namespace {
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }
}  // namespace

int main(int argc, char** argv) {
  std::string socket_path = "/tmp/velora-sysd.sock", data_dir;
  long duration_s = 0;
  bool slot_good = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--socket") && i + 1 < argc) {
      socket_path = argv[++i];
    } else if (!std::strcmp(argv[i], "--data-dir") && i + 1 < argc) {
      data_dir = argv[++i];
    } else if (!std::strcmp(argv[i], "--duration") && i + 1 < argc) {
      duration_s = std::strtol(argv[++i], nullptr, 10);
    } else if (!std::strcmp(argv[i], "--slot-good")) {
      slot_good = true;  // dev/test: the running slot counts as marked good, so an N-1 config file is migrated to N at start (the board wires RAUC here)
    } else {
      std::fprintf(stderr, "usage: velora-sysd --data-dir DIR [--socket PATH] [--duration SECONDS] [--slot-good]\n");
      return std::strcmp(argv[i], "--help") ? 2 : 0;
    }
  }
  if (data_dir.empty()) {
    std::fprintf(stderr, "velora-sysd: --data-dir is required\n");
    return 2;
  }
  const std::string cfg_dir = data_dir + "/config";
  for (const std::string& d : {data_dir, cfg_dir}) {
    if (::mkdir(d.c_str(), 0755) != 0 && errno != EEXIST) {
      std::fprintf(stderr, "velora-sysd: cannot create %s: %s\n", d.c_str(), std::strerror(errno));
      return 1;
    }
  }
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);

  velora::sysd::StoreOptions so;
  so.dir = cfg_dir;
  so.log = [](const std::string& s) { std::fprintf(stderr, "%s\n", s.c_str()); };
  if (slot_good) so.slot_good = [] { return true; };
  velora::sysd::ConfigStore store(std::move(so));
  if (!store.writer_locked()) {
    std::fprintf(stderr, "velora-sysd: another sysd owns %s\n", cfg_dir.c_str());
    return 1;
  }
  (void)store.migrate();  // deferred (nothing written) until the slot is good
  velora::sysd::ConfigService service(store);
  velora::sysd::ConfigServer server(service, socket_path);
  std::string err;
  if (!server.start(err)) {
    std::fprintf(stderr, "velora-sysd: %s\n", err.c_str());
    return 1;
  }
  std::printf("sysd: config %s (%s), revision %llu, ws.v1 on %s\n", (cfg_dir + "/" + velora::sysd::kFileName).c_str(),
              velora::sysd::to_string(store.load_reason()), (unsigned long long)store.revision(), socket_path.c_str());
  std::fflush(stdout);
  const auto start = std::chrono::steady_clock::now();
  while (!g_stop.load()) {
    if (duration_s > 0 && std::chrono::steady_clock::now() - start >= std::chrono::seconds(duration_s)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  server.stop();
  return 0;
}
