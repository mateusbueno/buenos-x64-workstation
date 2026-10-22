#include <cassert>
#include <iostream>
#include <lvgl.h>

#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/screens/library_view.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"

using namespace velora::ui;
using namespace velora::format;

void test_library_view_search_and_filters() {
  LibraryView view;
  bool ok = view.create(lv_screen_active());
  assert(ok);

  std::vector<TimbreCatalogEntry> entries = {
    {"t1", "German Grand D-274", Category::kPiano, true, true, 4ULL * 1024 * 1024 * 1024, 100},
    {"t2", "Rhodes Mk1", Category::kEPiano, true, false, 2ULL * 1024 * 1024 * 1024, 200},
    {"t3", "Chamber Strings", Category::kStrings, true, false, 3ULL * 1024 * 1024 * 1024, 0},
    {"t4", "Custom Brass", Category::kBrass, false, true, 1ULL * 1024 * 1024 * 1024, 0}
  };

  view.set_catalog(entries);
  assert(view.filtered_indices().size() == 4);

  // Search filter
  view.set_search_query("strings");
  assert(view.filtered_indices().size() == 1);
  assert(view.catalog()[view.filtered_indices()[0]].name == "Chamber Strings");

  // Clear search
  view.set_search_query("");
  assert(view.filtered_indices().size() == 4);

  // Category filter
  view.set_filter_category(Category::kPiano);
  assert(view.filtered_indices().size() == 1);
  assert(view.catalog()[view.filtered_indices()[0]].name == "German Grand D-274");

  // Favorites only
  view.set_filter_favorites_only(true);
  assert(view.filtered_indices().size() == 2); // t1 and t4

  std::cout << "[PASS] test_library_view_search_and_filters\n";
}

void test_library_view_choose_mode_and_audition() {
  LibraryView view;
  view.create(lv_screen_active());

  std::vector<TimbreCatalogEntry> entries = {
    {"t1", "German Grand D-274", Category::kPiano, true, false, 1024, 0},
    {"t2", "Rhodes Mk1", Category::kEPiano, true, false, 1024, 0}
  };
  view.set_catalog(entries);

  // Set mode to Choose for Part 2 (index 1)
  view.set_mode(LibraryMode::ChooseForPart, 1);
  assert(view.mode() == LibraryMode::ChooseForPart);
  assert(view.target_part_idx() == 1);

  // Keybed audition
  assert(!view.is_audition_sounding());
  view.on_midi_note(60); // Middle C
  assert(view.is_audition_sounding());
  assert(view.last_auditioned_note() == 60);

  // WRITE assigns focused timbre
  bool selected_fired = false;
  int32_t chosen_part = -1;
  std::string chosen_id;
  view.set_on_timbre_selected([&](int32_t part, const std::string& id) {
    selected_fired = true;
    chosen_part = part;
    chosen_id = id;
  });

  bool close_fired = false;
  view.set_on_close([&]() {
    close_fired = true;
  });

  view.on_panel_event(PanelEvent::button_press(MCU_BTN_WRITE));
  assert(selected_fired);
  assert(chosen_part == 1);
  assert(chosen_id == "t1");
  assert(close_fired);

  std::cout << "[PASS] test_library_view_choose_mode_and_audition\n";
}

void test_library_view_favorite_toggle() {
  LibraryView view;
  view.create(lv_screen_active());

  std::vector<TimbreCatalogEntry> entries = {
    {"t1", "German Grand D-274", Category::kPiano, true, false, 1024, 0}
  };
  view.set_catalog(entries);

  assert(!view.catalog()[0].is_favorite);

  bool fav_fired = false;
  std::string fav_id;
  bool fav_status = false;
  view.set_on_favorite_toggled([&](const std::string& id, bool fav) {
    fav_fired = true;
    fav_id = id;
    fav_status = fav;
  });

  view.toggle_favorite_at_focused();
  assert(fav_fired);
  assert(fav_id == "t1");
  assert(fav_status == true);
  assert(view.catalog()[0].is_favorite);

  std::cout << "[PASS] test_library_view_favorite_toggle\n";
}

int main() {
  lv_init();
  Locale::instance().initialize("", "en");
  HeadlessAdapter adapter;
  if (!adapter.initialize(1024, 600)) {
    std::cerr << "Failed to init headless adapter\n";
    return 1;
  }

  test_library_view_search_and_filters();
  test_library_view_choose_mode_and_audition();
  test_library_view_favorite_toggle();

  std::cout << "All LibraryView tests passed!\n";
  return 0;
}
