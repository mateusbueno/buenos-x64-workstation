#pragma once

#include <memory>
#include <string>
#include <atomic>
#include <mutex>
#include <chrono>

#include "velora/ui/display_adapter.hpp"
#include "velora/ui/ws_client.hpp"
#include "velora/ui/screens/shell_view.hpp"
#include "velora/ui/locale.hpp"

namespace velora::ui {

struct AppOptions {
  std::string socket_path{"/tmp/velora-engine.sock"};
  std::string locale{"en"};
  bool headless{false};
  int duration_sec{0};  // 0 = run until closed
};

class UiApp {
 public:
  UiApp(AppOptions options = {});
  ~UiApp();

  bool init();
  int run();
  void stop();

  // Accessors
  WsClient& ws_client() noexcept { return ws_client_; }
  ShellView& shell_view() noexcept { return shell_view_; }
  IDisplayAdapter* display_adapter() noexcept { return adapter_.get(); }

 private:
  void on_state_updated(const ws::v1::State& state);

  AppOptions options_;
  std::unique_ptr<IDisplayAdapter> adapter_;
  WsClient ws_client_;
  ShellView shell_view_;

  std::atomic<bool> running_{false};
  std::mutex pending_mutex_;
  bool has_pending_state_{false};
  ws::v1::State pending_state_;
};

}  // namespace velora::ui
