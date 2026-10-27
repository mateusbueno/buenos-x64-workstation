#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <vector>

#include "test_util.hpp"
#include "velora/format/parametric_bundle.hpp"
#include "velora/format/sysex_6op.hpp"
#include "velora/librarian/catalog.hpp"
#include "velora/librarian/library_index.hpp"
#include "velora/librarian/import.hpp"
#include "velora/librarian/preflight.hpp"

namespace fs = std::filesystem;
using namespace velora::librarian;
namespace fmt = velora::format;

namespace {

struct Env {
  fs::path dir, src, root;
  explicit Env(const char* name) {
    dir = fs::temp_directory_path() / (std::string("velora_sysex_") + name + "_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    src = dir / "src";
    root = dir / "data";
    fs::create_directories(src);
    fs::create_directories(root);
  }
  ~Env() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  void put(const std::string& name, const std::vector<uint8_t>& b) const {
    fs::create_directories((src / name).parent_path());
    std::ofstream(src / name, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
  }
  ImportOptions opts(const std::string& name) const {
    ImportOptions o;
    o.sfz_path = (src / name).string();
    o.root = root.string();
    o.write_catalog_meta = true;
    o.category = "Synth Poly";
    return o;
  }
};

static fmt::FmVoiceParameters make_self_made_voice(uint8_t algo, uint8_t fb, const std::string& name) {
  fmt::FmVoiceParameters v{};
  v.algorithm = std::clamp<uint8_t>(algo, 1, 32);
  v.feedback = std::clamp<uint8_t>(fb, 0, 7);
  v.osc_key_sync = true;
  std::memset(v.name, 0, sizeof(v.name));
  std::strncpy(v.name, name.c_str(), 10);
  for (int i = 0; i < 6; ++i) {
    auto& op = v.operators[i];
    op.rates = {99, 90, 70, 40};
    op.levels = {99, 80, 60, 0};
    op.kbd_breakpoint = 60;
    op.total_level = 90;
    op.freq_coarse = i + 1;
  }
  return v;
}

} // namespace

int main() {
  std::cout << "Running sysex_import_test...\n";
  Env env("import");

  // 1. Create a self-made 32-voice bank and write to a .syx file
  std::vector<fmt::FmVoiceParameters> bank;
  for (int i = 1; i <= 32; ++i) {
    bank.push_back(make_self_made_voice(i, i % 8, "VOX " + std::to_string(i)));
  }
  std::vector<uint8_t> bulk_bytes = fmt::encode_sysex_bulk_dump(bank, 0);
  env.put("test_bank.syx", bulk_bytes);

  // 2. Preflight Scan
  {
    PreflightOptions p_opt;
    p_opt.source = env.src.string();
    PreflightResult p_res = preflight_scan(p_opt);
    assert(p_res.ok);
    assert(!p_res.rows.empty());

    bool found = false;
    for (const auto& r : p_res.rows) {
      if (r.rel_path == "test_bank.syx") {
        found = true;
        assert(r.format == "sysex");
        assert(r.suggested == fmt::Category::kSynthPoly);
        assert(r.status == RowStatus::kReady);
        assert(r.pcm_bytes == 0); // Parametric timbres have 0 PCM bytes
      }
    }
    assert(found);
    std::cout << "  ✓ Preflight scan of .syx file passed\n";
  }

  // 3. Import 32-voice Bank
  {
    ImportResult res = import_sysex(env.opts("test_bank.syx"));
    assert(res.ok());
    assert(res.bundle_created);
    assert(res.current_flipped);
    assert(res.timbres_published == 32);
    assert(!res.library_id.empty());
    assert(!res.bundle_id.empty());

    // Verify the published parametric bundle at <root>/lib/<library_id>/current/bundle.vlb
    fs::path cur_bundle = env.root / "lib" / res.library_id / "current" / "bundle.vlb";
    auto b_res = fmt::ParametricBundle::read_file(cur_bundle.string());
    assert(b_res);
    fmt::ParametricBundle published_bundle = b_res.take();
    assert(published_bundle.timbres().size() == 32);
    assert(published_bundle.timbres()[0].key == "1");
    assert(published_bundle.timbres()[31].key == "32");

    // Verify catalog.meta was written
    fs::path cur_meta = env.root / "lib" / res.library_id / "current" / "catalog.meta";
    assert(fs::exists(cur_meta));
    CatalogMeta cm;
    assert(read_catalog_meta(cur_meta.parent_path().string(), cm) == MetaState::kValid);
    assert(cm.source_kind == "sysex");
    assert(cm.instruments.size() == 32);
    assert(cm.instruments[0].key == "1");
    assert(cm.instruments[31].key == "32");

    // Verify Catalog database accepts source_kind = "sysex"
    std::unique_ptr<Catalog> cat;
    CatalogStatus cs = Catalog::open(env.root.string(), cat);
    assert(cs == CatalogStatus::kOk && cat);
    LibraryRecord lr;
    lr.library_id = res.library_id;
    lr.name = cm.library_name;
    lr.source_kind = "sysex";
    lr.source_path = cm.source_path;
    lr.bundle_id = res.bundle_id;
    for (const auto& i : cm.instruments) {
      InstrumentRecord ir;
      ir.key = i.key;
      ir.name = i.name;
      ir.category = static_cast<uint32_t>(i.category);
      lr.instruments.push_back(ir);
    }
    bool changed = false;
    assert(cat->publish_library(lr, changed) == CatalogStatus::kOk);
    assert(changed);

    LibraryRecord fetched;
    bool found = false;
    assert(cat->get_library(res.library_id, fetched, found) == CatalogStatus::kOk);
    assert(found);
    assert(fetched.source_kind == "sysex");
    assert(fetched.instruments.size() == 32);

    std::cout << "  ✓ Import of 32-voice SysEx bank passed\n";
  }

  // 4. Re-import (same content)
  {
    ImportResult res2 = import_sysex(env.opts("test_bank.syx"));
    assert(res2.ok());
    assert(!res2.bundle_created); // unchanged
    std::cout << "  ✓ Re-import of identical bank ends cleanly passed\n";
  }

  // 5. Single voice dump import
  {
    fmt::FmVoiceParameters single_v = make_self_made_voice(12, 5, "SINGLE VOX");
    std::vector<uint8_t> single_bytes = fmt::encode_sysex_single_dump(single_v, 0);
    env.put("single.syx", single_bytes);

    ImportResult res = import_sysex(env.opts("single.syx"));
    assert(res.ok());
    assert(res.timbres_published == 1);

    fs::path cur_meta = env.root / "lib" / res.library_id / "current" / "catalog.meta";
    assert(fs::exists(cur_meta));
    CatalogMeta cm;
    assert(read_catalog_meta(cur_meta.parent_path().string(), cm) == MetaState::kValid);
    assert(cm.source_kind == "sysex");
    assert(cm.instruments.size() == 1);
    assert(cm.instruments[0].key == "1");
    assert(cm.instruments[0].name == "SINGLE VOX");
    std::cout << "  ✓ Import of single voice dump passed\n";
  }

  // 6. Damaged SysEx rejection
  {
    std::vector<uint8_t> damaged_bytes = bulk_bytes;
    damaged_bytes[4102] ^= 0x55; // corrupt checksum
    env.put("damaged.syx", damaged_bytes);

    // Preflight should mark it skipped damaged with copy message
    PreflightOptions p_opt;
    p_opt.source = env.src.string();
    PreflightResult p_res = preflight_scan(p_opt);
    for (const auto& r : p_res.rows) {
      if (r.rel_path == "damaged.syx") {
        assert(r.status == RowStatus::kSkippedDamaged);
        assert(r.detail.find(fmt::kSysexDamagedMessage) != std::string::npos);
      }
    }

    // Import should skip with reason code damaged
    ImportResult res = import_sysex(env.opts("damaged.syx"));
    assert(res.status == Status::kSkipped);
    assert(res.reason_code == ReasonCode::kDamaged);
    assert(res.reasons[0].find(fmt::kSysexDamagedMessage) != std::string::npos);
    assert(!res.bundle_created);

    std::cout << "  ✓ Damaged SysEx rejection with exact copy message passed\n";
  }

  std::cout << "All sysex_import_test checks passed!\n";
  return 0;
}
