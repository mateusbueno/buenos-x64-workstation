#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/screens/performance_view.hpp"

#include <cassert>
#include <iostream>

using namespace velora::ui;

static void test_pickup_ghost() {
  lv_init();
  Locale::instance().initialize("", "en");

  HeadlessAdapter adapter;
  assert(adapter.initialize(1024, 600));

  PerformanceView view;
  assert(view.create());
  lv_obj_update_layout(view.root_container());

  // Setup 8 parts
  std::vector<PartStripData> test_parts(8);
  for (size_t i = 0; i < 8; ++i) {
    test_parts[i] = {"Sound " + std::to_string(i + 1), "Piano", 0.50f, true, false, false};
  }
  test_parts[2].volume = 0.65f; // Part 3 stored volume = 65%
  test_parts[0].volume = 0.50f; // Part 1 stored volume = 50%
  view.set_parts(test_parts);

  // 1. Set pickup pending on Part 3: physical fader at 20%
  view.set_pickup_pending(2, 0.20f);
  assert(view.is_part_pickup_pending(2));
  assert(view.active_pickup_fader() == 2);

  // Verify ghost and chevron are visible
  assert(view.part_ghost(2) != nullptr);
  assert(!lv_obj_is_hidden(view.part_ghost(2)));
  assert(view.part_chevron(2) != nullptr);
  assert(!lv_obj_is_hidden(view.part_chevron(2)));

  // Verify Status Bar takeover
  assert(view.is_warning_active());
  std::string takeover_text = view.warning_text();
  assert(takeover_text.find("FADER 3") != std::string::npos);
  assert(takeover_text.find("65%") != std::string::npos);
  assert(takeover_text.find("▲") != std::string::npos);

  // 2. Multiple pending faders: set pickup pending on Part 1 at 90% (above stored 50%)
  view.set_pickup_pending(0, 0.90f);
  assert(view.is_part_pickup_pending(0));
  assert(view.active_pickup_fader() == 0); // Most recently moved

  // Takeover now names Part 1
  takeover_text = view.warning_text();
  assert(takeover_text.find("FADER 1") != std::string::npos);
  assert(takeover_text.find("50%") != std::string::npos);
  assert(takeover_text.find("▼") != std::string::npos);

  // 3. Move Part 3 fader slightly (to 30%, still below 65%)
  view.handle_fader_move(2, 0.30f);
  assert(view.is_part_pickup_pending(2));
  assert(view.active_pickup_fader() == 2); // Now Part 3 was most recently moved
  takeover_text = view.warning_text();
  assert(takeover_text.find("FADER 3") != std::string::npos);

  // 4. Cross stored value on Part 3 (from 30% to 70% across 65%)
  view.handle_fader_move(2, 0.70f);
  assert(!view.is_part_pickup_pending(2)); // Part 3 takeover completed!
  assert(view.part_data(2).volume == 0.70f);
  assert(lv_obj_is_hidden(view.part_ghost(2)));
  assert(lv_obj_is_hidden(view.part_chevron(2)));

  // Remaining pending fader (Part 1) takes over the Status Bar
  assert(view.active_pickup_fader() == 0);
  assert(view.is_warning_active());
  takeover_text = view.warning_text();
  assert(takeover_text.find("FADER 1") != std::string::npos);

  // 5. Cross stored value on Part 1 (from 90% down to 40% across 50%)
  view.handle_fader_move(0, 0.40f);
  assert(!view.is_part_pickup_pending(0)); // Part 1 takeover completed!
  assert(view.part_data(0).volume == 0.40f);
  assert(lv_obj_is_hidden(view.part_ghost(0)));
  assert(lv_obj_is_hidden(view.part_chevron(0)));

  // All pending pickups resolved -> Status Bar restored
  assert(!view.is_warning_active());

  // 6. Test fader event via PanelEvent (MCU_CTRL_FADER_2)
  test_parts[1].volume = 0.80f;
  view.set_parts(test_parts);
  view.set_pickup_pending(1, 0.10f);
  assert(view.is_part_pickup_pending(1));

  // Send raw 12-bit fader value = 3500 (3500 / 4095 ≈ 0.854f, crosses 0.80f)
  PanelEvent fader_ev = PanelEvent::fader_move(MCU_CTRL_FADER_2, 3500);
  view.on_panel_event(fader_ev);

  assert(!view.is_part_pickup_pending(1));
  assert(view.part_data(1).volume == 3500.0f / 4095.0f);
  assert(!view.is_warning_active());

  lv_timer_handler();
  adapter.shutdown();

  std::cout << "[PASS] test_pickup_ghost\n";
}

int main() {
  std::cout << "Running pickup_ghost_test...\n";
  test_pickup_ghost();
  std::cout << "All pickup ghost tests passed!\n";
  return 0;
}
