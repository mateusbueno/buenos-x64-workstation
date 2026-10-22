#include <cassert>
#include <iostream>
#include <lvgl.h>

#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/screens/setlist_view.hpp"
#include "velora/ui/navigation_controller.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"

using namespace velora::ui;

void test_setlist_view_basic_and_selection() {
  SetlistView view(nullptr);
  bool ok = view.create(lv_screen_active());
  assert(ok);
  assert(!view.is_empty_state());
  assert(view.setlist_count() == 4);
  assert(view.song_count() == 5);

  // Initial selection
  assert(view.selected_setlist_index() == 0);
  assert(view.focus_column() == SetlistFocusColumn::Setlists);

  // VALUE turn down (CW)
  PanelEvent enc_turn;
  enc_turn.type = PanelEventType::Encoder;
  enc_turn.id = MCU_CTRL_VALUE_ENCODER;
  enc_turn.delta = 1;
  view.on_panel_event(enc_turn);
  assert(view.selected_setlist_index() == 1);

  // VALUE push to toggle column to Songs
  PanelEvent push_ev;
  push_ev.type = PanelEventType::Button;
  push_ev.id = MCU_BTN_VALUE_PUSH;
  push_ev.pressed = true;
  view.on_panel_event(push_ev);
  assert(view.focus_column() == SetlistFocusColumn::Songs);

  // In songs column, VALUE turn down
  view.on_panel_event(enc_turn);
  assert(view.selected_song_index() == 1);

  // WRITE on song jumps to it
  PanelEvent write_ev;
  write_ev.type = PanelEventType::Button;
  write_ev.id = MCU_BTN_WRITE;
  write_ev.pressed = true;
  view.on_panel_event(write_ev);
  assert(view.now_playing_live_set_id() == "song-2");

  // Toggle back to Setlists column and WRITE loads it
  view.toggle_focus_column();
  assert(view.focus_column() == SetlistFocusColumn::Setlists);
  view.on_panel_event(write_ev);
  assert(view.loaded_setlist_id() == "setlist-2");

  std::cout << "[PASS] test_setlist_view_basic_and_selection\n";
}

void test_now_playing_and_pending_edits() {
  SetlistView view(nullptr);
  view.create(lv_screen_active());

  // Setup sample items
  std::vector<SetlistItem> setlists = {
      {"sl-1", "Sunday Service", 3, true}
  };
  std::vector<SongItem> songs = {
      {"s-1", "Song A", 2, false, false},
      {"s-2", "Song B", 3, true, false},
      {"s-3", "Song C", 4, false, true}
  };
  view.set_items(setlists, songs);

  assert(view.song_count() == 3);

  // Update from state with dirty live set
  ws::v1::State state;
  state.set_live_set_id("s-2");
  auto* edits = state.mutable_edits();
  edits->add_dirty_live_set_ids("s-3");
  view.update_from_state(state);

  assert(view.now_playing_live_set_id() == "s-2");

  std::cout << "[PASS] test_now_playing_and_pending_edits\n";
}

void test_reorder_songs() {
  SetlistView view(nullptr);
  view.create(lv_screen_active());

  std::vector<SetlistItem> setlists = {
      {"sl-1", "Test Setlist", 3, true}
  };
  std::vector<SongItem> songs = {
      {"s-1", "Song 1", 1, false, false},
      {"s-2", "Song 2", 1, false, false},
      {"s-3", "Song 3", 1, false, false}
  };
  view.set_items(setlists, songs);
  view.select_song(1); // Select Song 2

  // Reorder up (MCU_BTN_SCENE_PREV)
  PanelEvent up_ev;
  up_ev.type = PanelEventType::Button;
  up_ev.id = MCU_BTN_SCENE_PREV;
  up_ev.pressed = true;
  view.on_panel_event(up_ev);

  assert(view.songs()[0].id == "s-2");
  assert(view.songs()[1].id == "s-1");
  assert(view.selected_song_index() == 0);

  // Reorder down (MCU_BTN_SCENE_NEXT)
  PanelEvent down_ev;
  down_ev.type = PanelEventType::Button;
  down_ev.id = MCU_BTN_SCENE_NEXT;
  down_ev.pressed = true;
  view.on_panel_event(down_ev);

  assert(view.songs()[0].id == "s-1");
  assert(view.songs()[1].id == "s-2");
  assert(view.selected_song_index() == 1);

  std::cout << "[PASS] test_reorder_songs\n";
}

void test_on_screen_keyboard_actions() {
  SetlistView view(nullptr);
  view.create(lv_screen_active());

  assert(!view.keyboard().is_open());

  // Prompt new setlist opens OSK
  view.prompt_new_setlist();
  assert(view.keyboard().is_open());

  // Type name on OSK: "Gospel"
  view.keyboard().set_text("Gospel");
  PanelEvent write_ev;
  write_ev.type = PanelEventType::Button;
  write_ev.id = MCU_BTN_WRITE;
  write_ev.pressed = true;
  view.keyboard().on_panel_event(write_ev);

  assert(!view.keyboard().is_open());
  assert(view.setlist_count() == 5);
  assert(view.setlists().back().name == "Gospel");

  // Prompt new live set
  view.prompt_new_live_set();
  assert(view.keyboard().is_open());
  view.keyboard().set_text("Amazing Grace");
  view.keyboard().on_panel_event(write_ev);

  assert(!view.keyboard().is_open());
  assert(view.songs().back().name == "Amazing Grace");
  assert(view.now_playing_live_set_id() == view.songs().back().id);

  std::cout << "[PASS] test_on_screen_keyboard_actions\n";
}

void test_empty_state() {
  SetlistView view(nullptr);
  view.create(lv_screen_active());

  view.clear_setlists();
  assert(view.is_empty_state());
  assert(view.empty_container() != nullptr);
  assert(!lv_obj_is_hidden(view.empty_container()));

  // Pressing WRITE in empty state prompts new setlist
  PanelEvent write_ev;
  write_ev.type = PanelEventType::Button;
  write_ev.id = MCU_BTN_WRITE;
  write_ev.pressed = true;
  view.on_panel_event(write_ev);

  assert(view.keyboard().is_open());
  view.keyboard().close();

  std::cout << "[PASS] test_empty_state\n";
}

void test_navigation_integration() {
  NavigationController nav(nullptr);
  bool ok = nav.initialize(lv_screen_active());
  assert(ok);

  assert(nav.current_surface() == SurfaceId::Performance);

  // Press MENU to open Launcher
  PanelEvent menu_ev;
  menu_ev.type = PanelEventType::Button;
  menu_ev.id = MCU_BTN_MENU;
  menu_ev.pressed = true;
  nav.on_panel_event(menu_ev);
  assert(nav.current_surface() == SurfaceId::Launcher);

  // Navigate to Setlist surface
  nav.navigate_to(SurfaceId::Setlist);
  assert(nav.current_surface() == SurfaceId::Setlist);

  // EXIT on Setlist returns to Performance
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_EXIT));
  nav.on_panel_event(PanelEvent::button_release(MCU_BTN_EXIT));
  assert(nav.current_surface() == SurfaceId::Performance);

  std::cout << "[PASS] test_navigation_integration\n";
}

int main() {
  lv_init();
  Locale::instance().initialize("", "en");
  HeadlessAdapter adapter;
  if (!adapter.initialize(1024, 600)) {
    std::cerr << "Failed to init headless adapter\n";
    return 1;
  }

  test_setlist_view_basic_and_selection();
  test_now_playing_and_pending_edits();
  test_reorder_songs();
  test_on_screen_keyboard_actions();
  test_empty_state();
  test_navigation_integration();

  std::cout << "All Setlist View tests passed!\n";
  return 0;
}
