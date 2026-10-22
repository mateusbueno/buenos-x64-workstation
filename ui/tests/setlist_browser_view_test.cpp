#include <cassert>
#include <iostream>
#include <lvgl.h>

#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/screens/setlist_browser_view.hpp"
#include "velora/ui/navigation_controller.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"

using namespace velora::ui;

void test_browser_creation_and_chips() {
  SetlistBrowserView view(nullptr);
  bool ok = view.create(lv_screen_active());
  assert(ok);
  assert(!view.is_empty_state());

  // Default chip is "All Live Sets" (index 3)
  assert(view.selected_chip_index() == 3);
  // Default items include 5 Live Sets
  assert(view.filtered_count() == 5);

  // Switch to Setlists chip (index 2)
  view.select_chip(2);
  assert(view.selected_chip_index() == 2);
  assert(view.filtered_count() == 3);

  // Switch to Favorites chip (index 0)
  view.select_chip(0);
  assert(view.selected_chip_index() == 0);
  // Song 1 and Song 5 and Setlist 2 are favorites
  assert(view.filtered_count() >= 2);

  // Switch to User Tag "Church" (index 5)
  view.select_chip(5);
  assert(view.filtered_count() >= 1);

  std::cout << "[PASS] test_browser_creation_and_chips\n";
}

void test_search_and_empty_states() {
  SetlistBrowserView view(nullptr);
  view.create(lv_screen_active());

  // Search "thou" with All Live Sets
  view.select_chip(3);
  view.set_search_query("thou");
  assert(view.search_query() == "thou");
  // Matches "A Thousand Years", "Thousand Miles", "Thou Art Worthy"
  assert(view.filtered_count() == 3);

  // Search query with no matches
  view.set_search_query("nonexistent_xyz");
  assert(view.is_empty_state());
  assert(view.filtered_count() == 0);
  assert(view.empty_message().find("nonexistent_xyz") != std::string::npos);

  // Empty favorites message
  view.set_search_query("");
  std::vector<BrowserItem> no_fav_items = {
      {"s-1", "Song 1", BrowserItemType::LiveSet, "META", false, false, false, {}}
  };
  view.set_items(no_fav_items);
  view.select_chip(0); // Favorites
  assert(view.is_empty_state());
  assert(view.empty_message() == tr("setlist_browser.empty.no_favorites"));

  // Empty recent message
  view.select_chip(1); // Recent
  assert(view.is_empty_state());
  assert(view.empty_message() == tr("setlist_browser.empty.no_recent"));

  std::cout << "[PASS] test_search_and_empty_states\n";
}

void test_jump_and_load_actions() {
  SetlistBrowserView view(nullptr);
  view.create(lv_screen_active());

  // Select first result (Live Set) and press WRITE
  view.select_chip(3); // All Live Sets
  assert(view.filtered_count() > 0);
  view.select_result(0);

  PanelEvent write_ev;
  write_ev.type = PanelEventType::Button;
  write_ev.id = MCU_BTN_WRITE;
  write_ev.pressed = true;
  view.on_panel_event(write_ev);

  assert(view.last_activated_id() == "song-1");
  assert(view.last_activated_type() == BrowserItemType::LiveSet);

  // Select Setlists chip, select first setlist and press WRITE
  view.select_chip(2); // Setlists
  assert(view.filtered_count() > 0);
  view.select_result(0);
  view.on_panel_event(write_ev);

  assert(view.last_activated_type() == BrowserItemType::Setlist);

  std::cout << "[PASS] test_jump_and_load_actions\n";
}

void test_action_sheet_favorites() {
  SetlistBrowserView view(nullptr);
  view.create(lv_screen_active());

  view.select_chip(3); // All Live Sets
  view.select_result(1); // "Thousand Miles" (is_favorite = false)

  assert(!view.action_sheet().is_open());

  // Open Action Sheet via long press
  view.open_action_sheet_for_selected();
  assert(view.action_sheet().is_open());
  assert(view.action_sheet().action_count() == 2);

  // VALUE highlights row 0 ("★ Add to Favorites")
  assert(view.action_sheet().highlight() == 0);

  // WRITE confirms
  PanelEvent write_ev;
  write_ev.type = PanelEventType::Button;
  write_ev.id = MCU_BTN_WRITE;
  write_ev.pressed = true;
  view.on_panel_event(write_ev);

  assert(!view.action_sheet().is_open());

  // Now item should be favorite
  view.open_action_sheet_for_selected();
  assert(view.action_sheet().is_open());
  // Option should now be "Remove from Favorites"
  // Press EXIT to cancel
  PanelEvent exit_ev;
  exit_ev.type = PanelEventType::Button;
  exit_ev.id = MCU_BTN_EXIT;
  exit_ev.pressed = true;
  view.on_panel_event(exit_ev);
  assert(!view.action_sheet().is_open());

  std::cout << "[PASS] test_action_sheet_favorites\n";
}

void test_navigation_and_top_bar_context() {
  NavigationController nav(nullptr);
  bool ok = nav.initialize(lv_screen_active());
  assert(ok);

  assert(nav.current_surface() == SurfaceId::Performance);

  // 1. Open Setlist Browser via Launcher tile 0
  PanelEvent menu_ev;
  menu_ev.type = PanelEventType::Button;
  menu_ev.id = MCU_BTN_MENU;
  menu_ev.pressed = true;
  nav.on_panel_event(menu_ev);
  assert(nav.current_surface() == SurfaceId::Launcher);

  nav.launcher_view().set_highlight(0); // SetlistBrowser
  assert(nav.launcher_view().is_tile_available(0));
  PanelEvent write_ev;
  write_ev.type = PanelEventType::Button;
  write_ev.id = MCU_BTN_WRITE;
  write_ev.pressed = true;
  nav.on_panel_event(write_ev);
  assert(nav.current_surface() == SurfaceId::SetlistBrowser);

  // EXIT returns to Performance
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_EXIT));
  nav.on_panel_event(PanelEvent::button_release(MCU_BTN_EXIT));
  assert(nav.current_surface() == SurfaceId::Performance);

  // 2. Open Setlist Browser from Top Bar setlist context click (UX-DR12)
  nav.performance_view().set_setlist_context("Test Setlist", 0, 5);
  // Simulate clicking context
  nav.navigate_to(SurfaceId::SetlistBrowser);
  assert(nav.current_surface() == SurfaceId::SetlistBrowser);
  assert(!lv_obj_is_hidden(nav.setlist_browser_view().skeleton().root_container()));

  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_EXIT));
  nav.on_panel_event(PanelEvent::button_release(MCU_BTN_EXIT));
  assert(nav.current_surface() == SurfaceId::Performance);

  std::cout << "[PASS] test_navigation_and_top_bar_context\n";
}

int main() {
  lv_init();
  Locale::instance().initialize("", "en");
  HeadlessAdapter adapter;
  if (!adapter.initialize(1024, 600)) {
    std::cerr << "Failed to init headless adapter\n";
    return 1;
  }

  test_browser_creation_and_chips();
  test_search_and_empty_states();
  test_jump_and_load_actions();
  test_action_sheet_favorites();
  test_navigation_and_top_bar_context();

  std::cout << "All Setlist Browser View tests passed!\n";
  return 0;
}
