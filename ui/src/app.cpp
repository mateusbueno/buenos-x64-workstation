#include "velora/ui/app.hpp"
#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/adapters/sdl_adapter.hpp"
#include "velora/ui/adapters/drm_adapter.hpp"
#include "velora/ui/virtual_panel.hpp"

#include <cstdio>
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
    auto sdl = std::make_unique<SdlAdapter>();
    sdl->set_scale(options_.scale);
    adapter_ = std::move(sdl);
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

  if (!build_navigation()) {
    return false;
  }

  ws_client_.set_state_callback([this](const ws::v1::State& st) {
    on_state_updated(st);
  });

  ws_client_.start();
  running_.store(true);
  return true;
}

bool UiApp::build_navigation() {
  nav_controller_ = std::make_unique<NavigationController>(&ws_client_);
  if (!nav_controller_->initialize(lv_screen_active())) {
    return false;
  }
  nav_controller_->set_on_language_changed([this]() { rebuild_pending_ = true; });
  if (!options_.headless && dynamic_cast<SdlAdapter*>(adapter_.get())) nav_controller_->enable_led_view();  // Story 3.12: the simulator shows the panel LEDs

  if (!options_.start_surface.empty()) {
    static const std::pair<const char*, SurfaceId> kSurfaces[] = {
        {"performance", SurfaceId::Performance}, {"launcher", SurfaceId::Launcher},
        {"effects", SurfaceId::Effects},         {"setlist", SurfaceId::Setlist},
        {"browser", SurfaceId::SetlistBrowser},  {"scenes", SurfaceId::Scenes},
        {"part", SurfaceId::Part},               {"settings", SurfaceId::Settings},
        {"library", SurfaceId::Library},         {"import", SurfaceId::Import}};
    for (const auto& [name, id] : kSurfaces) {
      if (options_.start_surface == name) {
        nav_controller_->navigate_to(id);
        if (id == SurfaceId::Settings) {
          nav_controller_->settings_view().select_section(
              static_cast<SettingsSection>(options_.start_section));
        } else if (id == SurfaceId::Part) {
          nav_controller_->part_view().set_section(
              static_cast<PartSection>(options_.start_section));
        }
      }
    }
  }
  return true;
}

// Re-creates every view so all labels pick up the newly selected language.
void UiApp::rebuild_navigation() {
  rebuild_pending_ = false;
  const SurfaceId surface = nav_controller_->current_surface();
  const SettingsSection section = nav_controller_->settings_view().selected_section();
  const std::string saved_surface = options_.start_surface;
  options_.start_surface.clear();

  lv_obj_clean(lv_screen_active());
  lv_obj_clean(lv_layer_top());  // only the simulator's LED view lives on the top layer (checked: nothing else uses it)
  nav_controller_.reset();
  if (!build_navigation()) return;

  options_.start_surface = saved_surface;
  nav_controller_->navigate_to(surface);
  if (surface == SurfaceId::Settings) {
    nav_controller_->settings_view().select_section(section);
  }
}

void UiApp::on_state_updated(const ws::v1::State& state) {
  std::lock_guard<std::mutex> lock(pending_mutex_);
  pending_state_ = state;
  has_pending_state_ = true;
}

int UiApp::run() {
  auto start_time = std::chrono::steady_clock::now();
  int frame = 0;

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
        if (nav_controller_) {
          nav_controller_->update_from_state(pending_state_);
        }
        has_pending_state_ = false;
      }
    }

    if (rebuild_pending_) {
      rebuild_navigation();
    }
    if (nav_controller_) {
      nav_controller_->tick(10);
    }

    adapter_->update();
    lv_timer_handler();

    if (!options_.screenshot_path.empty() && ++frame == 60) {
      lv_refr_now(nullptr);
      if (lv_draw_buf_t* snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_ARGB8888)) {
        // Dev aid: raw PPM (P6) dump; convert externally.
        if (FILE* f = std::fopen(options_.screenshot_path.c_str(), "wb")) {
          const uint32_t w = snap->header.w, h = snap->header.h;
          std::fprintf(f, "P6\n%u %u\n255\n", w, h);
          for (uint32_t y = 0; y < h; ++y) {
            const uint8_t* row = static_cast<const uint8_t*>(snap->data) + y * snap->header.stride;
            for (uint32_t x = 0; x < w; ++x) {
              const uint8_t px[3] = {row[x * 4 + 2], row[x * 4 + 1], row[x * 4 + 0]};
              std::fwrite(px, 1, 3, f);
            }
          }
          std::fclose(f);
        }
        lv_draw_buf_destroy(snap);
      }
      break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  return 0;
}

void UiApp::stop() {
  if (!running_.exchange(false)) return;

  VirtualPanel::instance().clear_listeners();
  ws_client_.stop();
  if (adapter_) {
    adapter_->shutdown();
  }
}

}  // namespace velora::ui
