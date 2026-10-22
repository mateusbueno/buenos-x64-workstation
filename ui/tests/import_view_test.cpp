#include <cassert>
#include <iostream>
#include <lvgl.h>

#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/screens/import_view.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"

using namespace velora::ui;
using namespace velora::format;

void test_import_view_rows_and_suggestions() {
  ImportView view;
  bool ok = view.create(lv_screen_active());
  assert(ok);

  std::vector<ImportItem> items = {
    {"German_Grand.sfz", 4ULL * 1024 * 1024 * 1024, "SFZ", Category::kPiano, true, ImportItemStatus::Ready, false},
    {"Vintage_Rhodes.sfz", 2ULL * 1024 * 1024 * 1024, "SFZ", Category::kNone, false, ImportItemStatus::NeedsCategory, false},
    {"Damaged_File.sfz", 1ULL * 1024 * 1024 * 1024, "SFZ", Category::kNone, false, ImportItemStatus::SkippedDamaged, false},
    {"Existing_Organ.sfz", 1ULL * 1024 * 1024 * 1024, "SFZ", Category::kOrgan, false, ImportItemStatus::AlreadyImported, false}
  };

  view.set_drive_info("SANDISK 128GB", 8ULL * 1024 * 1024 * 1024, 20ULL * 1024 * 1024 * 1024);
  view.set_items(items);

  assert(view.items().size() == 4);
  assert(view.items()[0].is_suggested);
  assert(view.items()[0].category == Category::kPiano);
  assert(view.items()[1].status == ImportItemStatus::NeedsCategory);

  // Pressing WRITE when row 1 lacks category must fail and show warning
  view.on_panel_event(PanelEvent::button_press(MCU_BTN_WRITE));
  assert(view.view_state() == ImportViewState::Rows);
  assert(!view.warning_message().empty());
  assert(view.warning_message().find("1 timbre(s) need a category") != std::string::npos);

  std::cout << "[PASS] test_import_view_rows_and_suggestions\n";
}

void test_import_view_category_picker_apply_all() {
  ImportView view;
  view.create(lv_screen_active());

  std::vector<ImportItem> items = {
    {"Item1.sfz", 1024, "SFZ", Category::kNone, false, ImportItemStatus::NeedsCategory, false},
    {"Item2.sfz", 1024, "SFZ", Category::kNone, false, ImportItemStatus::NeedsCategory, false},
    {"Skipped.sfz", 1024, "SFZ", Category::kNone, false, ImportItemStatus::SkippedDamaged, false}
  };

  view.set_drive_info("TEST DRIVE", 3072, 100ULL * 1024 * 1024 * 1024);
  view.set_items(items);

  // Focus row 0 and open category picker
  view.on_panel_event(PanelEvent::button_press(MCU_BTN_VALUE_PUSH));
  assert(view.category_picker().is_visible());

  // Set category strings and apply to all
  view.category_picker().set_selected_category(Category::kStrings);
  view.category_picker().set_apply_to_all(true);
  // WRITE confirms picker
  view.category_picker().on_panel_event(PanelEvent::button_press(MCU_BTN_WRITE));

  assert(!view.category_picker().is_visible());
  assert(view.items()[0].category == Category::kStrings);
  assert(view.items()[0].status == ImportItemStatus::Ready);
  assert(view.items()[1].category == Category::kStrings);
  assert(view.items()[1].status == ImportItemStatus::Ready);
  // Skipped damaged item remains skipped
  assert(view.items()[2].status == ImportItemStatus::SkippedDamaged);

  std::cout << "[PASS] test_import_view_category_picker_apply_all\n";
}

void test_import_view_storage_check_and_progress() {
  ImportView view;
  view.create(lv_screen_active());

  std::vector<ImportItem> items = {
    {"Item1.sfz", 10ULL * 1024 * 1024 * 1024, "SFZ", Category::kPiano, false, ImportItemStatus::Ready, false}
  };

  // Internal free space (5 GB) is smaller than required (10 GB)
  view.set_drive_info("DRIVE", 10ULL * 1024 * 1024 * 1024, 5ULL * 1024 * 1024 * 1024);
  view.set_items(items);

  view.on_panel_event(PanelEvent::button_press(MCU_BTN_WRITE));
  assert(view.view_state() == ImportViewState::InsufficientStorage);

  // Now provide sufficient storage
  view.set_drive_info("DRIVE", 10ULL * 1024 * 1024 * 1024, 50ULL * 1024 * 1024 * 1024);
  view.set_items(items);

  bool finished_fired = false;
  uint32_t imported_cnt = 0;
  uint32_t skipped_cnt = 0;
  view.set_on_import_finished([&](uint32_t imp, uint32_t skp) {
    finished_fired = true;
    imported_cnt = imp;
    skipped_cnt = skp;
  });

  view.on_panel_event(PanelEvent::button_press(MCU_BTN_WRITE));
  assert(view.view_state() == ImportViewState::Progress);
  assert(view.is_in_progress());

  // Tick until complete
  view.tick(100);
  view.tick(100);

  assert(finished_fired);
  assert(imported_cnt == 1);
  assert(skipped_cnt == 0);
  assert(view.view_state() == ImportViewState::Completed);

  std::cout << "[PASS] test_import_view_storage_check_and_progress\n";
}

void test_import_view_drive_removed() {
  ImportView view;
  view.create(lv_screen_active());

  std::vector<ImportItem> items = {
    {"Item1.sfz", 1024, "SFZ", Category::kPiano, false, ImportItemStatus::Ready, false},
    {"Item2.sfz", 1024, "SFZ", Category::kPiano, false, ImportItemStatus::Ready, false}
  };
  view.set_drive_info("DRIVE", 2048, 100ULL * 1024 * 1024 * 1024);
  view.set_items(items);

  view.start_import();
  assert(view.is_in_progress());

  view.simulate_drive_removed();
  assert(view.view_state() == ImportViewState::DriveRemoved);

  std::cout << "[PASS] test_import_view_drive_removed\n";
}

void test_import_view_sysex_rows() {
  ImportView view;
  view.create(lv_screen_active());

  std::vector<ImportItem> items = {
    {"FM_EP1.syx", 163, "Import 6-OP SysEx", Category::kSynthPoly, true, ImportItemStatus::Ready, false},
    {"FM_BANK.syx", 4104, "Import 6-OP SysEx", Category::kSynthPoly, true, ImportItemStatus::Ready, false}
  };

  view.set_drive_info("USB DRIVE", 4267, 100ULL * 1024 * 1024 * 1024);
  view.set_items(items);

  assert(view.items().size() == 2);
  assert(view.items()[0].format_str == "Import 6-OP SysEx");
  assert(view.items()[0].category == Category::kSynthPoly);
  assert(view.items()[0].status == ImportItemStatus::Ready);
  assert(view.items()[1].format_str == "Import 6-OP SysEx");
  assert(view.items()[1].category == Category::kSynthPoly);
  assert(view.items()[1].status == ImportItemStatus::Ready);

  // Apply to all can change the category of the entire SysEx bank
  view.on_panel_event(PanelEvent::button_press(MCU_BTN_VALUE_PUSH));
  assert(view.category_picker().is_visible());
  view.category_picker().set_selected_category(Category::kSynthLead);
  view.category_picker().set_apply_to_all(true);
  view.category_picker().on_panel_event(PanelEvent::button_press(MCU_BTN_WRITE));

  assert(!view.category_picker().is_visible());
  assert(view.items()[0].category == Category::kSynthLead);
  assert(view.items()[1].category == Category::kSynthLead);

  std::cout << "[PASS] test_import_view_sysex_rows\n";
}

int main() {
  lv_init();
  Locale::instance().initialize("", "en");
  HeadlessAdapter adapter;
  if (!adapter.initialize(1024, 600)) {
    std::cerr << "Failed to init headless adapter\n";
    return 1;
  }

  test_import_view_rows_and_suggestions();
  test_import_view_category_picker_apply_all();
  test_import_view_storage_check_and_progress();
  test_import_view_drive_removed();
  test_import_view_sysex_rows();

  std::cout << "All ImportView tests passed!\n";
  return 0;
}
