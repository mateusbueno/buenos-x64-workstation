#include "velora/ui/app.hpp"
#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/adapters/sdl_adapter.hpp"
#include "velora/ui/adapters/drm_adapter.hpp"

#include <iostream>
#include <thread>
#include <chrono>

namespace velora::ui {

UiApp::UiApp(AppOptions options)
    : options_(std::move(options)),
      ws_client_(options_.socket_path) {}

UiApp::~UiApp() {
  stop();
}

bool UiApp::init() {
  lv_init();

  Locale::instance().initialize("", options_.locale);
  Locale::instance().set_locale(options_.locale);

  // Select adapter
  if (options_.headless) {
    adapter_ = std::make_unique<HeadlessAdapter>();
  } else {
#if defined(__APPLE__) || defined(VELORA_USE_SDL)
    adapter_ = std::make_unique<SdlAdapter>();
#elif defined(__linux__) && LV_USE_LINUX_DRM
    adapter_ = std::make_unique<DrmAdapter>();
#else
    adapter_ = std::make_unique<HeadlessAdapter>();
#endif
  }

  if (!adapter_->initialize(1024, 600)) {
    // Fallback to headless if display server fails to open
    adapter_ = std::make_unique<HeadlessAdapter>();
    if (!adapter_->initialize(1024, 600)) {
      return false;
    }
  }

  if (!shell_view_.create()) {
    return false;
  }

  ws_client_.set_state_callback([this](const ws::v1::State& st) {
    on_state_updated(st);
  });

  ws_client_.start();
  running_.store(true);
  return true;
}

void UiApp::on_state_updated(const ws::v1::State& state) {
  std::lock_guard<std::mutex> lock(pending_mutex_);
  pending_state_ = state;
  has_pending_state_ = true;
}

int UiApp::run() {
  auto start_time = std::chrono::steady_clock::now();

  while (running_.load()) {
    if (adapter_->should_close()) {
      break;
    }

    if (options_.duration_sec > 0) {
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - start_time).count();
      if (elapsed >= options_.duration_sec) {
        break;
      }
    }

    // Process pending state updates from engine
    {
      std::lock_guard<std::mutex> lock(pending_mutex_);
      if (has_pending_state_) {
        shell_view_.update_from_state(pending_state_);
        has_pending_state_ = false;
      }
    }

    adapter_->update();
    lv_timer_handler();

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  return 0;
}

void UiApp::stop() {
  if (!running_.exchange(false)) return;

  ws_client_.stop();
  if (adapter_) {
    adapter_->shutdown();
  }
}

}  // namespace velora::ui
