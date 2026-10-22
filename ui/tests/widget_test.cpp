#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/virtual_panel.hpp"
#include "velora/ui/widgets/hint_bar.hpp"
#include "velora/ui/widgets/list_row.hpp"
#include "velora/ui/widgets/segmented_control.hpp"
#include "velora/ui/screens/surface_skeleton.hpp"

#include <cassert>
#include <iostream>
#include <vector>

using namespace velora::ui;

static void test_virtual_panel() {
  auto& vp = VirtualPanel::instance();
  vp.clear_listeners();

  std::vector<PanelEvent> events;
  vp.add_listener([&](const PanelEvent& ev) {
    events.push_back(ev);
  });

  // 1. EXIT
  assert(vp.handle_key(27, true));   // Esc
  assert(events.size() == 1 && events.back().id == MCU_BTN_EXIT && events.back().pressed);
  assert(vp.handle_key('x', false));
  assert(events.size() == 2 && events.back().id == MCU_BTN_EXIT && !events.back().pressed);

  // 2. MENU
  assert(vp.handle_key(9, true));    // Tab
  assert(events.back().id == MCU_BTN_MENU && events.back().pressed);
  assert(vp.handle_key('m', false));
  assert(events.back().id == MCU_BTN_MENU && !events.back().pressed);

  // 3. WRITE
  assert(vp.handle_key(13, true));   // Return
  assert(events.back().id == MCU_BTN_WRITE && events.back().pressed);
  assert(vp.handle_key('w', false));
  assert(events.back().id == MCU_BTN_WRITE && !events.back().pressed);

  // 4. VALUE PUSH
  assert(vp.handle_key(32, true));   // Space
  assert(events.back().id == MCU_BTN_VALUE_PUSH && events.back().pressed);
  assert(vp.handle_key('v', false));
  assert(events.back().id == MCU_BTN_VALUE_PUSH && !events.back().pressed);

  // 5. VALUE TURN
  assert(vp.handle_key(']', true));  // +1
  assert(events.back().id == MCU_CTRL_VALUE_ENCODER && events.back().delta == 1);
  assert(vp.handle_key('[', true));  // -1
  assert(events.back().id == MCU_CTRL_VALUE_ENCODER && events.back().delta == -1);

  // 6. LIVE SET PREV / NEXT
  assert(vp.handle_key(',', true));
  assert(events.back().id == MCU_BTN_LIVE_SET_PREV && events.back().pressed);
  assert(vp.handle_key('.', true));
  assert(events.back().id == MCU_BTN_LIVE_SET_NEXT && events.back().pressed);

  // 7. SCENE PREV / NEXT
  assert(vp.handle_key('1', true));
  assert(events.back().id == MCU_BTN_SCENE_PREV && events.back().pressed);
  assert(vp.handle_key('2', true));
  assert(events.back().id == MCU_BTN_SCENE_NEXT && events.back().pressed);

  std::cout << "[PASS] test_virtual_panel\n";
}

static void test_widgets_and_skeleton() {
  lv_init();
  Locale::instance().initialize("", "en");

  HeadlessAdapter adapter;
  assert(adapter.initialize(1024, 600));

  SurfaceSkeleton skeleton;
  assert(skeleton.create());
  lv_obj_update_layout(skeleton.root_container());

  // Check skeleton dimensions
  assert(lv_obj_get_width(skeleton.root_container()) == 1024);
  assert(lv_obj_get_height(skeleton.root_container()) == 600);
  assert(lv_obj_get_width(skeleton.top_bar()) == 1024);
  assert(lv_obj_get_height(skeleton.top_bar()) == 56);
  assert(lv_obj_get_width(skeleton.content_area()) == 1024);
  assert(lv_obj_get_height(skeleton.content_area()) == 488);
  assert(lv_obj_get_height(skeleton.hint_bar().container()) == 56);

  // Test Title
  skeleton.set_title("Effects · Part 1");
  assert(std::string(lv_label_get_text(skeleton.title_label())) == "Effects · Part 1");

  // Test Hint Bar
  std::vector<HintItem> hints = {
    {"VALUE", tr("hint.verb.scroll")},
    {"WRITE", tr("hint.verb.select")},
    {"EXIT", tr("hint.verb.back")}
  };
  skeleton.set_hints(hints, tr("hint.reassurance.nothing_to_write"));
  assert(skeleton.hint_bar().hints().size() == 3);
  assert(skeleton.hint_bar().hints()[0].control == "VALUE");
  assert(skeleton.hint_bar().hints()[0].verb == "scroll");
  assert(skeleton.hint_bar().right_text() == "Nothing to write");

  // Test ListRow inside content area
  ListRow row1;
  assert(row1.create(skeleton.content_area(), 980, 56));
  lv_obj_update_layout(row1.container());

  // Verify touch target >= 56px
  assert(lv_obj_get_height(row1.container()) >= 56);

  row1.set_title("Arena Rock Lead");
  row1.set_meta("3 scenes");
  assert(row1.title() == "Arena Rock Lead");
  assert(row1.meta() == "3 scenes");

  row1.set_now_playing(true);
  assert(row1.now_playing());

  row1.set_has_pending_edits(true);
  assert(row1.has_pending_edits());

  bool confirmed = false;
  row1.set_on_confirm([&](ListRow*) { confirmed = true; });

  row1.set_selected(false);
  row1.on_write();
  assert(!confirmed);  // Not selected, WRITE does not confirm

  row1.set_selected(true);
  assert(row1.is_selected());
  row1.on_write();
  assert(confirmed);   // Selected, confirmed

  // Test SegmentedControl
  SegmentedControl seg;
  std::vector<std::string> seg_labels = {"MAIN", "AUX"};
  assert(seg.create(skeleton.content_area(), seg_labels, 120));
  lv_obj_update_layout(seg.container());

  assert(seg.segment_count() == 2);
  assert(seg.selected_index() == 0);

  // Verify each segment touch target >= 56px
  assert(lv_obj_get_height(seg.segment_obj(0)) >= 56);
  assert(lv_obj_get_width(seg.segment_obj(0)) >= 56);
  assert(lv_obj_get_height(seg.segment_obj(1)) >= 56);
  assert(lv_obj_get_width(seg.segment_obj(1)) >= 56);

  int32_t seg_changed = -1;
  seg.set_on_change([&](int32_t idx) { seg_changed = idx; });

  int32_t seg_confirmed = -1;
  seg.set_on_confirm([&](int32_t idx) { seg_confirmed = idx; });

  // Value turn to next
  seg.on_value_turn(1);
  assert(seg.selected_index() == 1);
  assert(seg_changed == 1);

  // Value turn clamp
  seg.on_value_turn(1);
  assert(seg.selected_index() == 1);

  // Value turn back
  seg.on_value_turn(-1);
  assert(seg.selected_index() == 0);
  assert(seg_changed == 0);

  // Confirm via WRITE
  seg.on_write();
  assert(seg_confirmed == 0);

  lv_timer_handler();
  adapter.shutdown();

  std::cout << "[PASS] test_widgets_and_skeleton\n";
}

int main() {
  std::cout << "Running widget_test...\n";
  test_virtual_panel();
  test_widgets_and_skeleton();
  std::cout << "All widget tests passed!\n";
  return 0;
}
