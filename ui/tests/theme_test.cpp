#include "velora/ui/theme.hpp"
#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/screens/shell_view.hpp"
#include "velora/ui/locale.hpp"

#include <cassert>
#include <iostream>

using namespace velora::ui;

static void test_palette_values() {
  const auto& stage = Theme::stage();
  assert(stage.bg.blue == 0x0C);
  assert(stage.bg.green == 0x0D);
  assert(stage.bg.red == 0x0B);

  const auto& studio = Theme::studio();
  assert(studio.bg.blue == 0xF2);
  assert(studio.bg.green == 0xF4);
  assert(studio.bg.red == 0xF2);

  const auto& inv = Theme::invariant();
  assert(inv.drawbar_brown.red == 0x7F);
  assert(inv.drawbar_brown.green == 0x5E);
  assert(inv.drawbar_brown.blue == 0x47);

  const auto& rad = Theme::radius();
  assert(rad.sm == 5);
  assert(rad.md == 8);
  assert(rad.lg == 12);

  const auto& sp = Theme::spacing();
  assert(sp.device_topbar == 56);
  assert(sp.device_hint == 56);
  assert(sp.touch_target_min == 56);
  assert(sp.selection_border == 3);

  std::cout << "[PASS] test_palette_values\n";
}

static void test_theme_switching() {
  lv_init();
  Locale::instance().initialize("", "en");

  HeadlessAdapter adapter;
  bool ok = adapter.initialize(1024, 600);
  assert(ok);

  Theme::set_theme(ThemeId::Stage);
  assert(Theme::current_id() == ThemeId::Stage);
  assert(Theme::current().bg.red == Theme::stage().bg.red);

  {
    ShellView shell;
    ok = shell.create();
    assert(ok);
    assert(shell.root_container() != nullptr);

    lv_timer_handler();

    // Switch to Studio at runtime
    Theme::set_theme(ThemeId::Studio);
    assert(Theme::current_id() == ThemeId::Studio);
    assert(Theme::current().bg.red == Theme::studio().bg.red);

    lv_timer_handler();

    // Switch back to Stage
    Theme::set_theme(ThemeId::Stage);
    assert(Theme::current_id() == ThemeId::Stage);
    assert(Theme::current().bg.red == Theme::stage().bg.red);

    lv_timer_handler();
  }

  adapter.shutdown();

  std::cout << "[PASS] test_theme_switching\n";
}

int main() {
  std::cout << "Running theme_test...\n";
  test_palette_values();
  test_theme_switching();
  std::cout << "All theme tests passed!\n";
  return 0;
}
