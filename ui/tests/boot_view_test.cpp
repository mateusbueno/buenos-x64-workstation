#include <cassert>
#include <iostream>
#include <cmath>
#include <lvgl.h>

#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/screens/boot_view.hpp"
#include "velora/ui/navigation_controller.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/system_config.hpp"

using namespace velora::ui;

void test_boot_first_frame() {
  BootView view;
  view.set_reduced_motion(false);
  bool ok = view.create(lv_screen_active());
  assert(ok);

  assert(view.root() != nullptr);
  assert(lv_obj_get_width(view.root()) == 1024);
  assert(lv_obj_get_height(view.root()) == 600);

  // Before ticking, bars start at stub height 4
  assert(view.bar_height(0) == 4);
  assert(view.bar_height(1) == 4);
  assert(view.bar_height(2) == 4);
  assert(view.bar_height(3) == 4);
  assert(view.bar_height(4) == 4);

  // Wordmark is initially transparent
  assert(view.wordmark_opacity() == LV_OPA_TRANSP);

  // Status line is initial target resolving text
  assert(view.status_text() == "RESOLVING TARGET");
  assert(view.progress() == 0.0f);
  std::cout << "[PASS] test_boot_first_frame\n";
}

void test_boot_animation_staggered() {
  BootView view;
  view.set_reduced_motion(false);
  bool ok = view.create(lv_screen_active());
  assert(ok);

  // Tick 350 ms: Bar 0 mid-extension, Bar 3 still stub
  view.tick(350);
  assert(view.bar_height(0) > 4 && view.bar_height(0) < 140);
  assert(view.bar_height(1) > 4);
  assert(view.bar_height(3) == 4);
  assert(view.wordmark_opacity() == LV_OPA_TRANSP);

  // Tick another 450 ms (total 800 ms):
  // Bar 0 finished (700 ms). Wordmark starts fading in.
  view.tick(450);
  assert(view.bar_height(0) == 140);
  assert(view.wordmark_opacity() > LV_OPA_TRANSP);

  // Tick another 600 ms (total 1400 ms):
  // All bars complete, wordmark fully opaque.
  view.tick(600);
  assert(view.bar_height(0) == 140);
  assert(view.bar_height(1) == 44);
  assert(view.bar_height(2) == 44);
  assert(view.bar_height(3) == 44);
  assert(view.bar_height(4) == 44);
  assert(view.wordmark_opacity() == LV_OPA_COVER);
  assert(view.animation_finished());
  std::cout << "[PASS] test_boot_animation_staggered\n";
}

void test_boot_reduced_motion() {
  BootView view;
  view.set_reduced_motion(true);
  bool ok = view.create(lv_screen_active());
  assert(ok);

  // Under reduced motion: final static frame immediately without ticking
  assert(view.bar_height(0) == 140);
  assert(view.bar_height(1) == 44);
  assert(view.bar_height(2) == 44);
  assert(view.bar_height(3) == 44);
  assert(view.bar_height(4) == 44);
  assert(view.wordmark_opacity() == LV_OPA_COVER);
  assert(view.animation_finished());
  assert(view.transition_duration_ms() <= 80);
  std::cout << "[PASS] test_boot_reduced_motion\n";
}

void test_boot_status_updates() {
  BootView view;
  assert(view.create(lv_screen_active()));

  view.set_progress(0.65f);
  assert(std::abs(view.progress() - 0.65f) < 0.001f);

  view.set_status("Wedding Ana & Pedro", 2);
  assert(view.status_text() == "RESTORING · Wedding Ana & Pedro · SCENE 2");
  std::cout << "[PASS] test_boot_status_updates\n";
}

void test_boot_ready_no_delay() {
  BootView view;
  view.set_reduced_motion(false);
  assert(view.create(lv_screen_active()));

  bool ready_called = false;
  view.set_on_ready([&]() {
    ready_called = true;
  });

  // Target ready at t = 100 ms (long before animation finishes)
  view.tick(100);
  assert(!view.animation_finished());

  view.set_ready(true);
  assert(ready_called);
  assert(view.is_ready());
  std::cout << "[PASS] test_boot_ready_no_delay\n";
}

void test_boot_navigation() {
  NavigationController nav;
  assert(nav.initialize(lv_screen_active()));

  nav.navigate_to(SurfaceId::Boot);
  assert(nav.current_surface() == SurfaceId::Boot);

  // Engine reports ready
  nav.boot_view().set_ready(true);

  // Automatically switches to Performance
  assert(nav.current_surface() == SurfaceId::Performance);
  std::cout << "[PASS] test_boot_navigation\n";
}

void test_system_config_json() {
  SystemConfig cfg;
  cfg.schema_version = 1;
  cfg.reduced_motion = true;
  cfg.touch_mode = TouchMode::Safe;
  cfg.write_hold_ms = 600;
  cfg.long_press_ms = 800;
  cfg.panel_guard = true;

  std::string json = cfg.to_json();
  assert(json.find("\"reduced_motion\": true") != std::string::npos);
  assert(json.find("\"panel_guard\": true") != std::string::npos);

  SystemConfig loaded;
  assert(loaded.from_json(json));
  assert(loaded.schema_version == 1);
  assert(loaded.reduced_motion == true);
  assert(loaded.touch_mode == TouchMode::Safe);
  assert(loaded.write_hold_ms == 600);
  assert(loaded.long_press_ms == 800);
  assert(loaded.panel_guard == true);
  std::cout << "[PASS] test_system_config_json\n";
}

int main() {
  HeadlessAdapter adapter;
  if (!adapter.initialize(1024, 600)) {
    std::cerr << "Failed to init headless adapter\n";
    return 1;
  }

  Locale::instance().initialize("", "en");
  Locale::instance().set_locale("en");
  Theme::set_theme(ThemeId::Stage);

  test_boot_first_frame();
  test_boot_animation_staggered();
  test_boot_reduced_motion();
  test_boot_status_updates();
  test_boot_ready_no_delay();
  test_boot_navigation();
  test_system_config_json();

  std::cout << "All BootView tests passed!\n";
  return 0;
}
