#pragma once

#include <memory>
#include <string>
#include <atomic>
#include <mutex>
#include <chrono>

#include "velora/ui/display_adapter.hpp"
#include "velora/ui/ws_client.hpp"
#include "velora/ui/screens/shell_view.hpp"
#include "velora/ui/navigation_controller.hpp"
#include "velora/ui/locale.hpp"

namespace velora::ui {

struct AppOptions {
  std::string socket_path{"/tmp/velora-engine.sock"};
  std::string locale{"en"};
  bool headless{false};
  int duration_sec{0};  // 0 = run until closed
  float scale{1.0f};    // Window scaling factor for desktop (1.0 = 1024x600)
  std::string start_surface;  // dev: performance|launcher|effects|setlist|browser|scenes|part|settings|library|import
  std::string screenshot_path;  // dev: save a BMP after ~1s and exit
  std::string sysd_socket;    // Story 7.7: the sysd config service socket; empty = the clock source stays a local setting
  int start_section{0};       // dev: initial section for settings/part surfaces
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
  NavigationController* nav_controller() noexcept { return nav_controller_.get(); }
  IDisplayAdapter* display_adapter() noexcept { return adapter_.get(); }

 private:
  void on_state_updated(const ws::v1::State& state);
  bool build_navigation();
  void rebuild_navigation();

  AppOptions options_;
  std::unique_ptr<IDisplayAdapter> adapter_;
  WsClient ws_client_;
  ShellView shell_view_;
  std::unique_ptr<SysdConfigSender> sysd_sender_;  // Story 7.7 (before nav_controller_: destroyed after it)
  std::unique_ptr<NavigationController> nav_controller_;

  bool rebuild_pending_{false};
  std::atomic<bool> running_{false};
  std::mutex pending_mutex_;
  bool has_pending_state_{false};
  ws::v1::State pending_state_;
  bool has_pending_setlists_{false};  // Story 8.9: the latest SetlistsReply (All Live Sets for the Boot into picker), applied on the UI thread
  ws::v1::SetlistsReply pending_setlists_;
};

}  // namespace velora::ui
