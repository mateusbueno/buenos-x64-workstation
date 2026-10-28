#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/screens/shell_view.hpp"
#include "velora/ui/locale.hpp"

#include <iostream>
#include <cstring>

using namespace velora::ui;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " " << #c << "\n"; ++g_fail; } } while (0)

int main() {
  lv_init();
  Locale::instance().initialize("", "en");

  HeadlessAdapter adapter;
  CHECK(adapter.initialize(1024, 600));

  ShellView shell;
  CHECK(shell.create());
  lv_obj_update_layout(shell.root_container());

  // Check dimensions
  CHECK(lv_obj_get_width(shell.root_container()) == 1024);
  CHECK(lv_obj_get_height(shell.root_container()) == 600);

  // Check initial strings
  CHECK(std::string(lv_label_get_text(shell.title_label())) == "Velora 73");
  CHECK(std::string(lv_label_get_text(shell.status_label())) == "Ready");

  // Update tempo
  shell.set_tempo(142.5f);
  CHECK(std::string(lv_label_get_text(shell.tempo_label())).find("142.5") != std::string::npos);

  // Update live set
  shell.set_live_set_name("Arena Rock Lead");
  CHECK(std::string(lv_label_get_text(shell.live_set_label())) == "Arena Rock Lead");

  // Update clock lost
  shell.set_clock_lost(true);
  // (removed: lv_label_get_text() on the root container, which is not a label, behind `|| true`: it asserted nothing and crashed on Linux where the text pointer is null)

  // Test update_from_state
  ws::v1::State st;
  auto* pv = st.add_performance_init();
  pv->set_param_id(ws::v1::PARAM_TEMPO_BPM);
  pv->set_value(98.4f);
  st.set_live_set_name("Sunday Morning Piano");
  shell.update_from_state(st);

  CHECK(std::string(lv_label_get_text(shell.tempo_label())).find("98.4") != std::string::npos);
  CHECK(std::string(lv_label_get_text(shell.live_set_label())) == "Sunday Morning Piano");

  // Step LVGL timers
  lv_timer_handler();

  if (g_fail == 0) {
    std::cout << "ui_shell_test passed\n";
    return 0;
  }
  return 1;
}
