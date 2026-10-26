// velora-sysd: the host process that owns the system configuration (Foundation: sysd config service). It serves ws.v1 config messages on a Unix domain
// socket and persists the config under <data-dir>/config/sysd_config.pb. It becomes a systemd unit on the board (sysd/README.md has an example).
//   velora-sysd --socket PATH --data-dir DIR [--duration SECONDS] [--slot-good]
//     [--bridge-port PORT [--bind ADDR] [--engine-socket PATH] [--librarian-socket PATH] [--pairing-code CODE] [--dev-bypass-pairing]]
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <thread>

#include "velora/sysd/bridge_server.hpp"
#include "velora/sysd/config_server.hpp"
#include "velora/sysd/pairing.hpp"

namespace {
// 127.0.0.0/8 and ::1 only.
bool is_loopback(const std::string& host) {
  in_addr a4{};
  if (inet_pton(AF_INET, host.c_str(), &a4) == 1) return (ntohl(a4.s_addr) >> 24) == 127;
  in6_addr a6{};
  return inet_pton(AF_INET6, host.c_str(), &a6) == 1 && IN6_IS_ADDR_LOOPBACK(&a6);
}
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }
}  // namespace

int main(int argc, char** argv) {
  std::string socket_path = "/tmp/velora-sysd.sock", data_dir;
  long duration_s = 0;
  bool slot_good = false;
  velora::sysd::BridgeOptions bo;
  bool bridge = false, dev_bypass = false, allow_dev_nonloopback = false;
  std::string pairing_code;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--socket") && i + 1 < argc) {
      socket_path = argv[++i];
    } else if (!std::strcmp(argv[i], "--data-dir") && i + 1 < argc) {
      data_dir = argv[++i];
    } else if (!std::strcmp(argv[i], "--duration") && i + 1 < argc) {
      duration_s = std::strtol(argv[++i], nullptr, 10);
    } else if (!std::strcmp(argv[i], "--slot-good")) {
      slot_good = true;  // dev/test: the running slot counts as marked good, so an N-1 config file is migrated to N at start (the board wires RAUC here)
    } else if (!std::strcmp(argv[i], "--bridge-port") && i + 1 < argc) {
      const char* v = argv[++i];
      char* end = nullptr;
      const long port = std::strtol(v, &end, 10);
      if (!*v || *end || port < 0 || port > 65535) {
        std::fprintf(stderr, "velora-sysd: --bridge-port must be a number from 0 to 65535 (0 = ephemeral), got '%s'\n", v);
        return 2;
      }
      bo.port = static_cast<int>(port);
      bridge = true;
    } else if (!std::strcmp(argv[i], "--bind") && i + 1 < argc) {
      bo.bind_host = argv[++i];
    } else if (!std::strcmp(argv[i], "--engine-socket") && i + 1 < argc) {
      bo.engine_socket = argv[++i];
    } else if (!std::strcmp(argv[i], "--librarian-socket") && i + 1 < argc) {
      bo.librarian_socket = argv[++i];
    } else if (!std::strcmp(argv[i], "--pairing-code") && i + 1 < argc) {
      pairing_code = argv[++i];  // DEV / TEST ONLY: a fixed code
    } else if (!std::strcmp(argv[i], "--i-know-dev-flags-on-a-network")) {
      allow_dev_nonloopback = true;  // explicit override: the dev flags on a non-loopback bind
    } else if (!std::strcmp(argv[i], "--dev-bypass-pairing")) {
      dev_bypass = true;  // DEV ONLY: every client is paired
    } else {
      std::fprintf(stderr,
                   "usage: velora-sysd --data-dir DIR [--socket PATH] [--duration SECONDS] [--slot-good]\n"
                   "         [--bridge-port PORT [--bind ADDR] [--engine-socket PATH] [--librarian-socket PATH] [--pairing-code CODE] [--dev-bypass-pairing] [--i-know-dev-flags-on-a-network]]\n");
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
  const auto steady_ms = [] {
    return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  };
  const auto log_line = [](const std::string& s) { std::fprintf(stderr, "%s\n", s.c_str()); };
  std::unique_ptr<velora::sysd::PairedStore> paired;
  std::unique_ptr<velora::sysd::PairingWindow> window;
  std::unique_ptr<velora::sysd::BridgeServer> bridge_server;
  if (bridge) {
    if ((dev_bypass || !pairing_code.empty()) && !allow_dev_nonloopback && !is_loopback(bo.bind_host)) {
      std::fprintf(stderr,
                   "velora-sysd: --dev-bypass-pairing and --pairing-code are dev flags and are refused on the non-loopback address '%s' "
                   "(add --i-know-dev-flags-on-a-network to override)\n",
                   bo.bind_host.c_str());
      return 2;
    }
    if (!pairing_code.empty() && !velora::sysd::PairingWindow::valid_code(pairing_code)) {
      std::fprintf(stderr, "velora-sysd: --pairing-code must be at least %zu digits\n", velora::sysd::PairingWindow::kMinCodeLength);
      return 2;
    }
    const std::string bridge_dir = data_dir + "/bridge";
    if (::mkdir(bridge_dir.c_str(), 0700) != 0 && errno != EEXIST) {
      std::fprintf(stderr, "velora-sysd: cannot create %s: %s\n", bridge_dir.c_str(), std::strerror(errno));
      return 1;
    }
    ::chmod(bridge_dir.c_str(), 0700);  // it holds the pairing token hashes
    paired = std::make_unique<velora::sysd::PairedStore>(bridge_dir, log_line);
    if (!paired->ok()) {
      std::fprintf(stderr, "velora-sysd: the bridge needs the OS entropy source (getentropy) and it is unavailable; not starting\n");
      return 1;
    }
    window = std::make_unique<velora::sysd::PairingWindow>(steady_ms);
    bo.dev_bypass_pairing = dev_bypass;
    bo.now_ms = steady_ms;
    bo.log = log_line;
    if (dev_bypass) {
      std::fprintf(stderr, "velora-sysd: WARNING: --dev-bypass-pairing is ON: every client is paired. DEV HOST ONLY, never on a unit.\n");
    } else {
      if (!pairing_code.empty())
        std::fprintf(stderr, "velora-sysd: WARNING: --pairing-code sets a FIXED pairing code. DEV / TEST ONLY, never on a unit.\n");
      else if (!velora::sysd::PairingWindow::random_code(pairing_code)) {
        std::fprintf(stderr, "velora-sysd: no entropy for the pairing code; not starting the bridge\n");
        return 1;
      }
      window->open(pairing_code);
      // The code is printed on stdout (journald on a unit): a dev convenience until the unit's own pairing UI (Story 9.6) shows it on the display only.
      std::printf("sysd: pairing code %s (open for 2 minutes, closes after 5 wrong codes, closes at the first pairing)\n", pairing_code.c_str());
    }
    bridge_server = std::make_unique<velora::sysd::BridgeServer>(service, *window, *paired, bo);
    bridge_server->set_foreign_sink([&server](std::vector<velora::sysd::Outbound> o) { server.post(std::move(o)); });
    server.set_foreign_sink([&bridge_server](std::vector<velora::sysd::Outbound> o) { bridge_server->post(std::move(o)); });
  }
  if (!server.start(err)) {
    std::fprintf(stderr, "velora-sysd: %s\n", err.c_str());
    return 1;
  }
  if (bridge_server) {
    if (!bridge_server->start(err)) {
      std::fprintf(stderr, "velora-sysd: bridge: %s\n", err.c_str());
      server.stop();
      return 1;
    }
    std::printf("sysd: bridge ws.v1 on %s:%d (engine %s, librarian %s)\n", bo.bind_host.c_str(), bridge_server->port(),
                bo.engine_socket.empty() ? "none" : bo.engine_socket.c_str(), bo.librarian_socket.empty() ? "none" : bo.librarian_socket.c_str());
  }
  std::printf("sysd: config %s (%s), revision %llu, ws.v1 on %s\n", (cfg_dir + "/" + velora::sysd::kFileName).c_str(),
              velora::sysd::to_string(store.load_reason()), (unsigned long long)store.revision(), socket_path.c_str());
  std::fflush(stdout);
  const auto start = std::chrono::steady_clock::now();
  while (!g_stop.load()) {
    if (duration_s > 0 && std::chrono::steady_clock::now() - start >= std::chrono::seconds(duration_s)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (bridge_server) bridge_server->stop();
  server.stop();
  return 0;
}
