#include "velora/ui/app.hpp"

#include <iostream>
#include <string>
#include <csignal>

static velora::ui::UiApp* g_app = nullptr;

static void signal_handler(int sig) {
  (void)sig;
  if (g_app) {
    g_app->stop();
  }
}

int main(int argc, char** argv) {
  velora::ui::AppOptions opts;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--socket" && i + 1 < argc) {
      opts.socket_path = argv[++i];
    } else if (arg == "--locale" && i + 1 < argc) {
      opts.locale = argv[++i];
    } else if (arg == "--headless") {
      opts.headless = true;
    } else if (arg == "--duration" && i + 1 < argc) {
      opts.duration_sec = std::stoi(argv[++i]);
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: ui [options]\n"
                << "  --socket <path>     Unix domain socket path (default: /tmp/velora-engine.sock)\n"
                << "  --locale <lang>     UI locale code (en, pt-BR, es; default: en)\n"
                << "  --headless          Run without creating an OS window (for testing)\n"
                << "  --duration <sec>    Exit after N seconds\n";
      return 0;
    }
  }

  velora::ui::UiApp app(opts);
  g_app = &app;
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  if (!app.init()) {
    std::cerr << "Failed to initialize UI application\n";
    return 1;
  }

  int ret = app.run();
  g_app = nullptr;
  return ret;
}
