#include "velora/ui/locale.hpp"

#include <cassert>
#include <iostream>

using namespace velora::ui;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " " << #c << "\n"; ++g_fail; } } while (0)

int main() {
  Locale& loc = Locale::instance();
  CHECK(loc.initialize("", "en"));

  // 1. English catalog
  CHECK(loc.current_locale() == "en");
  CHECK(loc.tr("shell.top_bar.title") == "Velora 73");
  CHECK(loc.tr("shell.hint.ready") == "Ready");
  CHECK(loc.tr("shell.top_bar.clock_lost") == "CLOCK LOST");

  // 2. Portuguese (pt-BR)
  CHECK(loc.set_locale("pt-BR"));
  CHECK(loc.current_locale() == "pt-BR");
  CHECK(loc.tr("shell.hint.ready") == "Pronto");
  CHECK(loc.tr("shell.top_bar.clock_lost") == "CLOCK PERDIDO");
  CHECK(loc.tr("category.drums_perc.full") == "Bateria & Percussão");

  // 3. Spanish (es)
  CHECK(loc.set_locale("es"));
  CHECK(loc.current_locale() == "es");
  CHECK(loc.tr("shell.hint.ready") == "Listo");
  CHECK(loc.tr("shell.hint.write_to_save") == "Mantenga WRITE para guardar cambios");

  // 4. Fallback to English when key missing in Spanish
  loc.set_entry("en", "test.fallback.key", "English Fallback");
  CHECK(loc.tr("test.fallback.key") == "English Fallback");

  // 5. Fallback to raw key when missing in all catalogs
  CHECK(loc.tr("completely.missing.key") == "completely.missing.key");

  if (g_fail == 0) {
    std::cout << "locale_test passed\n";
    return 0;
  }
  return 1;
}
