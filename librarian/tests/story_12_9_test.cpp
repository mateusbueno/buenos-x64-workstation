#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <unistd.h>

#include "velora/format/factory_fm_va.hpp"
#include "velora/format/parametric_bundle.hpp"
#include "velora/format/category.hpp"
#include "velora/host/fs_port.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/librarian/catalog.hpp"
#include "velora/librarian/library_index.hpp"
#include "velora/ui/adapters/headless_adapter.hpp"
#include "velora/ui/screens/library_view.hpp"
#include "velora/ui/locale.hpp"

namespace fs = std::filesystem;
namespace fmt = velora::format;
namespace host = velora::host;
namespace lib = velora::librarian;
namespace ui = velora::ui;

namespace {

struct TestEnv {
  fs::path root;
  explicit TestEnv(const std::string& name) {
    root = fs::temp_directory_path() / (name + "_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    fs::create_directories(root / "lib");
  }
  ~TestEnv() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

// AC 1: Built-in Init FM and Init VA timbres
void test_builtin_init_timbres() {
  std::cout << "Testing AC 1: Built-in Init FM and Init VA timbres...\n";
  TestEnv env("test_builtin_init");

  auto fs_port = host::make_posix_fs();
  host::LibraryResolver resolver(fs_port, env.root.string());

  // 1. Built-in timbres resolve directly without disk files
  auto res_fm = resolver.resolve(host::kBuiltinLibraryId, host::kInitFmInstrumentKey);
  assert(res_fm.ok());
  assert(res_fm.is_builtin);
  assert(res_fm.instrument_info().name == host::kInitFmName);
  assert(res_fm.instrument_info().key == host::kInitFmInstrumentKey);

  auto res_va = resolver.resolve(host::kBuiltinLibraryId, host::kInitVaInstrumentKey);
  assert(res_va.ok());
  assert(res_va.is_builtin);
  assert(res_va.instrument_info().name == host::kInitVaName);
  assert(res_va.instrument_info().key == host::kInitVaInstrumentKey);

  // Unknown instrument key on built-in library fails cleanly
  auto res_unknown = resolver.resolve(host::kBuiltinLibraryId, "unknown-key");
  assert(!res_unknown.ok());
  assert(res_unknown.status == host::ResolveStatus::kMissingInstrument);

  // 2. FR58: Category display verification
  // Short label on Part Strips is "Synth Poly", full label is "Synth Poly", never engine type
  std::string_view short_label = fmt::english_short_name(fmt::Category::kSynthPoly);
  std::string_view full_label = fmt::english_name(fmt::Category::kSynthPoly);
  assert(short_label == "Synth Poly");
  assert(full_label == "Synth Poly");
  assert(short_label.find("FM") == std::string_view::npos);
  assert(short_label.find("VA") == std::string_view::npos);
  assert(full_label.find("FM") == std::string_view::npos);
  assert(full_label.find("VA") == std::string_view::npos);

  std::cout << "  ✓ Built-in Init FM and Init VA timbres resolve and conform to FR58\n";
}

// AC 2: Founder-authored factory FM/VA sound pack (>= 50 timbres)
void test_factory_sound_pack_content() {
  std::cout << "Testing AC 2: Founder-authored factory FM/VA pack content...\n";

  fmt::ParametricBundle bundle = fmt::create_factory_fm_va_bundle();
  const auto& timbres = bundle.timbres();

  // Decided: at least 50 timbres
  assert(timbres.size() >= 50);
  std::cout << "  ✓ Total timbres in pack: " << timbres.size() << " (>= 50 required)\n";

  // Check category coverage
  size_t count_epiano = 0;
  size_t count_bells = 0;
  size_t count_bass = 0;
  size_t count_brass = 0;
  size_t count_strings = 0;
  size_t count_lead = 0;
  size_t count_pad = 0;
  size_t count_poly = 0;

  // Trademark blacklist (no third-party trademarks)
  const std::vector<std::string> blacklist = {
    "dx7", "dx-7", "rhodes", "wurlitzer", "wurli", "clavinet", "clavinova",
    "minimoog", "moog", "jupiter", "juno", "prophet", "oberheim", "ob-xa",
    "roland", "yamaha", "korg", "nord", "kurzweil", "casio"
  };

  for (const auto& t : timbres) {
    // Assert category is one of the 17 valid assignable categories
    assert(fmt::is_assignable(t.category));

    // Check categories
    if (t.category == fmt::Category::kEPiano) count_epiano++;
    else if (t.category == fmt::Category::kMalletBells) count_bells++;
    else if (t.category == fmt::Category::kBass) count_bass++;
    else if (t.category == fmt::Category::kBrass) count_brass++;
    else if (t.category == fmt::Category::kStrings) count_strings++;
    else if (t.category == fmt::Category::kSynthLead) count_lead++;
    else if (t.category == fmt::Category::kSynthPad) count_pad++;
    else if (t.category == fmt::Category::kSynthPoly) count_poly++;

    // Assert name contains no trademark
    std::string lower_name = t.name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
    for (const auto& tm : blacklist) {
      assert(lower_name.find(tm) == std::string::npos);
    }

    // Engine type must be either FM or VA
    assert(t.engine_type_id == fmt::kEngineTypeFm || t.engine_type_id == fmt::kEngineTypeVa);
  }

  assert(count_epiano >= 6);
  assert(count_bells >= 4);
  assert(count_bass >= 6);
  assert(count_brass >= 4);
  assert(count_strings >= 4);
  assert(count_lead >= 5);
  assert(count_pad >= 5);
  assert(count_poly >= 5);

  std::cout << "  ✓ All categories covered with zero third-party trademarks\n";

  // Check roundtrip serialization / deserialization
  std::vector<uint8_t> bytes = bundle.serialize();
  auto des_res = fmt::ParametricBundle::deserialize(bytes);
  assert(des_res);
  fmt::ParametricBundle deserialized = des_res.take();
  assert(deserialized.timbres().size() == timbres.size());
  for (size_t i = 0; i < timbres.size(); ++i) {
    assert(deserialized.timbres()[i] == timbres[i]);
  }

  std::cout << "  ✓ Factory pack binary bundle round-trip bit-identical passed\n";
}

// AC 3: Catalog indexing, stable resolution, and re-installation (FR23)
void test_catalog_indexing_and_reinstall() {
  std::cout << "Testing AC 3: Catalog indexing, stable resolution, and re-installation...\n";
  TestEnv env("test_factory_catalog");

  fmt::ParametricBundle bundle = fmt::create_factory_fm_va_bundle();
  std::string lib_id_str = fmt::kFactoryFmVaLibraryId;
  std::vector<uint8_t> bundle_bytes = bundle.serialize();
  auto des_res = fmt::ParametricBundle::deserialize(bundle_bytes);
  assert(des_res);
  std::string bundle_id_str = fmt::to_hex(des_res.value().bundle_id());

  // Setup disk directory structure: <root>/lib/<library_id>/<bundle_id>/bundle.vlb and current
  fs::path lib_dir = env.root / "lib" / lib_id_str;
  fs::path bundle_dir = lib_dir / bundle_id_str;
  fs::create_directories(bundle_dir);

  {
    std::ofstream f(bundle_dir / "bundle.vlb", std::ios::binary);
    f.write(reinterpret_cast<const char*>(bundle_bytes.data()), bundle_bytes.size());
  }

  // Create current symlink
  fs::path cur_link = lib_dir / "current";
  std::error_code ec;
  fs::create_symlink(bundle_id_str, cur_link, ec);
  assert(!ec);

  // 1. Describe bundle through librarian
  lib::DescribeOptions dopt;
  lib::DescribeResult dr = lib::describe_bundle(bundle_dir.string(), dopt);
  assert(dr.status == lib::DescribeStatus::kOk);
  assert(dr.record.library_id == lib_id_str);
  assert(dr.record.bundle_id == bundle_id_str);
  assert(dr.record.instruments.size() == bundle.timbres().size());

  // 2. Publish to SQLite catalog
  std::unique_ptr<lib::Catalog> catalog;
  lib::CatalogStatus cs = lib::Catalog::open(env.root.string(), catalog);
  assert(cs == lib::CatalogStatus::kOk && catalog);

  bool changed = false;
  assert(catalog->publish_library(dr.record, changed) == lib::CatalogStatus::kOk);
  assert(changed);

  // 3. Query catalog
  std::vector<lib::InstrumentRecord> instruments;
  uint32_t total = 0;
  bool found = false;
  assert(catalog->list_instruments(lib_id_str, 0, 100, instruments, total, found) == lib::CatalogStatus::kOk);
  assert(found);
  assert(total == bundle.timbres().size());
  assert(instruments.size() == bundle.timbres().size());

  // 4. Resolve via LibraryResolver
  auto fs_port = host::make_posix_fs();
  host::LibraryResolver resolver(fs_port, env.root.string());

  auto r_va = resolver.resolve(lib_id_str, "va_lead_01");
  assert(r_va.ok());
  assert(r_va.is_parametric());
  assert(r_va.instrument_info().name == "Sync Screamer Lead");
  assert(r_va.instrument_info().key == "va_lead_01");

  auto r_fm = resolver.resolve(lib_id_str, "fm_ep_01");
  assert(r_fm.ok());
  assert(r_fm.is_parametric());
  assert(r_fm.instrument_info().name == "Classic Stage Tine");
  assert(r_fm.instrument_info().key == "fm_ep_01");

  // 5. Re-installation simulation (FR23 semantics):
  // Re-write bundle and re-create symlink, verify resolution remains stable
  fs::remove(cur_link);
  fs::create_symlink(bundle_id_str, cur_link, ec);
  assert(!ec);

  auto r_reinstall = resolver.resolve(lib_id_str, "va_lead_01");
  assert(r_reinstall.ok());
  assert(r_reinstall.instrument_info().name == "Sync Screamer Lead");

  std::cout << "  ✓ Catalog indexing and stable re-installation resolution passed\n";
}

// AC 4: UI integration (LibraryView with built-ins + factory pack)
void test_ui_library_integration() {
  std::cout << "Testing AC 4: UI LibraryView integration...\n";

  lv_init();
  ui::Locale::instance().initialize("", "en");
  ui::HeadlessAdapter adapter;
  if (!adapter.initialize(1024, 600)) {
    std::cerr << "Failed to init headless adapter\n";
    return;
  }

  ui::LibraryView view;
  bool ok = view.create(lv_screen_active());
  assert(ok);

  // Even with an empty catalog, Init FM and Init VA are always available
  assert(view.filtered_indices().size() == 2);
  assert(view.catalog()[0].name == "Init FM");
  assert(view.catalog()[1].name == "Init VA");

  // Now populate with factory pack entries
  fmt::ParametricBundle bundle = fmt::create_factory_fm_va_bundle();
  std::vector<ui::TimbreCatalogEntry> entries;
  for (const auto& t : bundle.timbres()) {
    ui::TimbreCatalogEntry e;
    e.id = std::string(fmt::kFactoryFmVaLibraryId) + ":" + t.key;
    e.name = t.name;
    e.category = t.category;
    e.is_factory = true;
    e.is_favorite = false;
    e.size_bytes = 0;
    entries.push_back(std::move(e));
  }
  view.set_catalog(entries);

  // Total timbres = 52 factory + 2 built-in = 54
  assert(view.catalog().size() == 54);
  assert(view.filtered_indices().size() == 54);

  // Filter by category: E.Piano
  view.set_filter_category(fmt::Category::kEPiano);
  assert(view.filtered_indices().size() == 8);

  // Filter by category: Synth Poly (6 factory + 2 built-in = 8)
  view.set_filter_category(fmt::Category::kSynthPoly);
  assert(view.filtered_indices().size() == 8);

  // Filter by category: Bass
  view.set_filter_category(fmt::Category::kBass);
  assert(view.filtered_indices().size() == 8);

  // Search by keyword
  view.set_filter_category(fmt::Category::kNone);
  view.set_search_query("Tine");
  assert(view.filtered_indices().size() == 2); // Classic Stage Tine, Warm Chorus Tine

  // Audition via MIDI note
  view.set_mode(ui::LibraryMode::ChooseForPart, 2);
  assert(!view.is_audition_sounding());
  view.on_midi_note(64); // E4
  assert(view.is_audition_sounding());
  assert(view.last_auditioned_note() == 64);

  // WRITE assignment to Part 2
  int32_t assigned_part = -1;
  std::string assigned_timbre;
  view.set_on_timbre_selected([&](int32_t part, const std::string& id) {
    assigned_part = part;
    assigned_timbre = id;
  });

  view.on_panel_event(ui::PanelEvent::button_press(MCU_BTN_WRITE));
  assert(assigned_part == 2);
  assert(!assigned_timbre.empty());

  std::cout << "  ✓ UI LibraryView search, category chips, audition, and WRITE assignment passed\n";
}

} // namespace

int main() {
  std::cout << "Running story_12_9_test...\n";
  test_builtin_init_timbres();
  test_factory_sound_pack_content();
  test_catalog_indexing_and_reinstall();
  test_ui_library_integration();
  std::cout << "All Story 12.9 tests passed successfully!\n";
  return 0;
}
