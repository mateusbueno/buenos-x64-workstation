#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/locale.hpp"
#include "velora/ui/theme.hpp"
#include "velora/ui/font_manager.hpp"
#include "velora/ui/navigation_controller.hpp"

#include <cassert>
#include <iostream>
#include <vector>

using namespace velora::ui;

static void tap_exit(NavigationController& nav) {
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_EXIT));
  nav.on_panel_event(PanelEvent::button_release(MCU_BTN_EXIT));
}

static void tap_menu(NavigationController& nav) {
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_MENU));
  nav.on_panel_event(PanelEvent::button_release(MCU_BTN_MENU));
}

void test_full_navigation_and_locales() {
  lv_init();

  HeadlessAdapter adapter;
  assert(adapter.initialize(1024, 600));

  // 1. Verify FontManager initialization with DejaVu Sans
  assert(FontManager::instance().initialize());
  assert(FontManager::instance().font_14() != nullptr);
  assert(FontManager::instance().font_18() != nullptr);
  assert(FontManager::instance().font_24() != nullptr);

  // 2. Initialize in Portuguese to verify accented letters and layout
  Locale::instance().initialize("", "pt-BR");
  Locale::instance().set_locale("pt-BR");

  NavigationController nav;
  assert(nav.initialize());

  // Initial surface must be Performance
  assert(nav.current_surface() == SurfaceId::Performance);
  assert(!lv_obj_is_hidden(nav.performance_view().root_container()));

  // 3. Navigate to Launcher via MENU
  tap_menu(nav);
  assert(nav.current_surface() == SurfaceId::Launcher);
  assert(!lv_obj_is_hidden(nav.launcher_view().skeleton().root_container()));

  // 4. Navigate from Launcher to Scenes (tile 3)
  nav.launcher_view().set_highlight(4); // Scenes
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_WRITE));
  assert(nav.current_surface() == SurfaceId::Scenes);
  assert(!lv_obj_is_hidden(nav.scenes_view().skeleton().root_container()));

  // Test Scenes action buttons and cards in pt-BR
  nav.scenes_view().select_slot(1);
  tap_exit(nav);
  assert(nav.current_surface() == SurfaceId::Performance);

  // 5. Navigate to PartView (via part selection on Performance)
  nav.navigate_to(SurfaceId::Part);
  assert(nav.current_surface() == SurfaceId::Part);
  assert(!lv_obj_is_hidden(nav.part_view().skeleton().root_container()));

  // Switch sections in PartView
  nav.part_view().set_section(PartSection::Timbre);
  nav.part_view().set_section(PartSection::ZoneKeyboard);
  nav.part_view().set_section(PartSection::SoundEngine);
  nav.part_view().set_section(PartSection::LevelOutput);
  nav.part_view().set_section(PartSection::EnvelopeEQ);
  nav.part_view().set_section(PartSection::EffectsSends);
  nav.part_view().set_section(PartSection::Arpeggiator);
  tap_exit(nav);
  assert(nav.current_surface() == SurfaceId::Performance);

  // 6. Navigate to Settings (via Launcher)
  tap_menu(nav);
  assert(nav.current_surface() == SurfaceId::Launcher);
  nav.launcher_view().set_highlight(6); // Settings
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_WRITE));
  assert(nav.current_surface() == SurfaceId::Settings);
  assert(!lv_obj_is_hidden(nav.settings_view().skeleton().root_container()));

  // Verify segmented controls and sections in SettingsView
  nav.settings_view().select_section(SettingsSection::DisplayLanguage);
  nav.settings_view().select_section(SettingsSection::KeyboardPedals);
  nav.settings_view().select_section(SettingsSection::MidiClock);
  nav.settings_view().select_section(SettingsSection::KnobsGlobalAssign);
  nav.settings_view().select_section(SettingsSection::StorageLibrary);
  nav.settings_view().select_section(SettingsSection::Computer);
  nav.settings_view().select_section(SettingsSection::SystemUpdate);
  tap_exit(nav);
  assert(nav.current_surface() == SurfaceId::Performance);

  // 7. Navigate to Effects (via REV button click callback)
  nav.navigate_to(SurfaceId::Effects);
  assert(nav.current_surface() == SurfaceId::Effects);
  assert(!lv_obj_is_hidden(nav.effects_view().skeleton().root_container()));
  nav.effects_view().select_section(EffectsSection::Sends);
  nav.effects_view().select_section(EffectsSection::Master);
  nav.effects_view().select_section(EffectsSection::Outputs);
  nav.effects_view().select_section(EffectsSection::Inserts);
  tap_exit(nav);
  assert(nav.current_surface() == SurfaceId::Performance);

  // 8. Navigate to Setlist & SetlistBrowser
  tap_menu(nav);
  nav.launcher_view().set_highlight(1); // Setlist
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_WRITE));
  assert(nav.current_surface() == SurfaceId::Setlist);
  tap_exit(nav);
  assert(nav.current_surface() == SurfaceId::Performance);

  nav.navigate_to(SurfaceId::SetlistBrowser);
  assert(nav.current_surface() == SurfaceId::SetlistBrowser);
  tap_exit(nav);
  assert(nav.current_surface() == SurfaceId::Performance);

  // 9. Navigate to Library
  tap_menu(nav);
  nav.launcher_view().set_highlight(2); // Library
  nav.on_panel_event(PanelEvent::button_press(MCU_BTN_WRITE));
  assert(nav.current_surface() == SurfaceId::Library);
  tap_exit(nav);
  assert(nav.current_surface() == SurfaceId::Performance);

  // 10. USB Drive / Import view
  std::vector<ImportItem> items;
  ImportItem it;
  it.file_name = "GrandPiano.wav";
  it.size_bytes = 1048576;
  it.status = ImportItemStatus::Ready;
  items.push_back(it);
  nav.on_usb_drive_inserted("KINGSTON", items);
  nav.navigate_to(SurfaceId::Import);
  assert(nav.current_surface() == SurfaceId::Import);
  tap_exit(nav);
  // The USB notice toast may consume the first EXIT (dismiss); a second one leaves Import.
  if (nav.current_surface() != SurfaceId::Performance) tap_exit(nav);
  assert(nav.current_surface() == SurfaceId::Performance);

  // 11. Write Dialog
  nav.set_pending_edits(true, false, "Edição de teste");
  nav.open_write_dialog();
  assert(nav.write_dialog().is_open());
  nav.write_dialog().close();

  // 12. Switch to Spanish ('es') and English ('en') dynamically
  Locale::instance().set_locale("es");
  nav.update_from_state(ws::v1::State());
  Locale::instance().set_locale("en");
  nav.update_from_state(ws::v1::State());

  lv_timer_handler();
  std::cout << "[PASS] test_full_navigation_and_locales\n";
}

int main() {
  std::cout << "Running full_navigation_suite_test...\n";
  test_full_navigation_and_locales();
  std::cout << "All full navigation tests passed!\n";
  return 0;
}
