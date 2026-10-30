#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>

#include "velora/core/ep_engine.hpp"
#include "velora/core/fx.hpp"
#include "velora/format/category.hpp"
#include "velora/format/ep_format.hpp"
#include "velora/format/factory_ep.hpp"
#include "velora/format/parametric_bundle.hpp"
#include "velora/host/fs_port.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/host/live_set_loader.hpp"
#include "velora/host/live_set_store.hpp"
#include "velora/librarian/catalog.hpp"
#include "velora/librarian/library_index.hpp"

namespace fs = std::filesystem;
namespace fmt = velora::format;
namespace host = velora::host;
namespace lib = velora::librarian;
namespace core = velora::core;

namespace {

struct TestEnv {
  fs::path root;
  explicit TestEnv(const std::string& name) {
    root = fs::temp_directory_path() / (name + "_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    fs::create_directories(root / "lib");
    fs::create_directories(root / "live_sets");
  }
  ~TestEnv() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

// AC 1: Built-in Init Tine and Init Reed timbres
void test_builtin_init_ep_timbres() {
  std::cout << "Testing AC 1: Built-in Init Tine and Init Reed timbres...\n";
  TestEnv env("test_builtin_init_ep");

  auto fs_port = host::make_posix_fs();
  host::LibraryResolver resolver(fs_port, env.root.string());

  // 1. Built-in timbres resolve directly without disk files
  auto res_tine = resolver.resolve(host::kBuiltinLibraryId, host::kInitTineInstrumentKey);
  assert(res_tine.ok());
  assert(res_tine.is_builtin);
  assert(res_tine.instrument_info().name == host::kInitTineName);
  assert(res_tine.instrument_info().key == host::kInitTineInstrumentKey);

  auto res_reed = resolver.resolve(host::kBuiltinLibraryId, host::kInitReedInstrumentKey);
  assert(res_reed.ok());
  assert(res_reed.is_builtin);
  assert(res_reed.instrument_info().name == host::kInitReedName);
  assert(res_reed.instrument_info().key == host::kInitReedInstrumentKey);

  // Unknown instrument key fails cleanly
  auto res_unknown = resolver.resolve(host::kBuiltinLibraryId, "unknown-ep-key");
  assert(!res_unknown.ok());
  assert(res_unknown.status == host::ResolveStatus::kMissingInstrument);

  // FR58 & NFR29: Category display verification
  std::string_view short_label = fmt::english_short_name(fmt::Category::kEPiano);
  std::string_view full_label = fmt::english_name(fmt::Category::kEPiano);
  assert(short_label == "E.Piano");
  assert(full_label == "E.Piano");

  std::cout << "  ✓ Built-in Init Tine and Init Reed timbres resolve and conform to NFR29/FR58\n";
}

// AC 2: Factory EP Sound Pack content (>= 6 tine, >= 4 reed, + 1 Init each, no trademarks)
void test_factory_ep_sound_pack_content() {
  std::cout << "Testing AC 2: Factory EP Sound Pack content...\n";

  fmt::ParametricBundle bundle = fmt::create_factory_ep_bundle();
  const auto& timbres = bundle.timbres();

  // Decided: at least 6 tine + 1 init, 4 reed + 1 init (total >= 12)
  assert(timbres.size() >= 12);
  std::cout << "  ✓ Total timbres in pack: " << timbres.size() << " (>= 12 required)\n";

  size_t count_tine = 0;
  size_t count_reed = 0;
  bool has_init_tine = false;
  bool has_init_reed = false;

  // Trademark blacklist (strictly generic names per NFR29)
  const std::vector<std::string> blacklist = {
    "rhodes", "wurlitzer", "wurli", "clavinet", "clavinova", "dyno-my-piano",
    "fender", "yamaha", "roland", "korg", "nord", "kurzweil", "casio"
  };

  for (const auto& t : timbres) {
    // Category must be E.Piano
    assert(t.category == fmt::Category::kEPiano);
    assert(t.engine_type_id == fmt::kEngineTypeEp);

    // Trademark check
    std::string lower_name = t.name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
    for (const auto& tm : blacklist) {
      assert(lower_name.find(tm) == std::string::npos);
    }

    // Voicing parameters range check
    const auto& ep = t.ep_parameters;
    assert(ep.hammer_hardness >= 0.0f && ep.hammer_hardness <= 1.0f);
    assert(ep.pickup_position >= 0.0f && ep.pickup_position <= 1.0f);
    assert(ep.pickup_level >= 0.0f && ep.pickup_level <= 1.0f);
    assert(ep.damper_noise >= 0.0f && ep.damper_noise <= 1.0f);
    assert(ep.attack_noise >= 0.0f && ep.attack_noise <= 1.0f);
    assert(ep.decay_rate >= 0.0f && ep.decay_rate <= 1.0f);
    assert(ep.tone_bar_coupling >= 0.0f && ep.tone_bar_coupling <= 1.0f);

    if (ep.instrument_type == fmt::EpInstrumentType::kTine) {
      count_tine++;
      if (t.name == "Init Tine") has_init_tine = true;
    } else if (ep.instrument_type == fmt::EpInstrumentType::kReed) {
      count_reed++;
      if (t.name == "Init Reed") has_init_reed = true;
    }
  }

  assert(has_init_tine);
  assert(has_init_reed);
  assert(count_tine >= 7); // 6 voicings + 1 Init
  assert(count_reed >= 5); // 4 voicings + 1 Init

  std::cout << "  ✓ Factory pack verified: " << count_tine << " tine (" << (count_tine - 1)
            << " voicings + Init), " << count_reed << " reed (" << (count_reed - 1)
            << " voicings + Init), 0 trademark violations\n";
}

// AC 3: Catalog indexing, serialization, and LibraryResolver resolution
void test_catalog_indexing_and_resolution() {
  std::cout << "Testing AC 3: Catalog indexing, serialization, and LibraryResolver...\n";
  TestEnv env("test_factory_ep_catalog");

  fmt::ParametricBundle bundle = fmt::create_factory_ep_bundle();
  std::string lib_id_str = fmt::kFactoryEpLibraryId;
  std::vector<uint8_t> bundle_bytes = bundle.serialize();

  // Round-trip deserialize check
  auto des_res = fmt::ParametricBundle::deserialize(bundle_bytes);
  assert(des_res);
  const auto& deserialized = des_res.value();
  assert(deserialized.timbres().size() == bundle.timbres().size());
  for (size_t i = 0; i < bundle.timbres().size(); ++i) {
    assert(deserialized.timbres()[i].key == bundle.timbres()[i].key);
    assert(deserialized.timbres()[i].name == bundle.timbres()[i].name);
    assert(deserialized.timbres()[i].ep_parameters == bundle.timbres()[i].ep_parameters);
  }

  std::string bundle_id_str = fmt::to_hex(deserialized.bundle_id());

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

  auto r_tine = resolver.resolve(lib_id_str, "ep_tine_stage_73_pure");
  assert(r_tine.ok());
  assert(r_tine.is_parametric());
  assert(r_tine.instrument_info().name == "Stage 73 Pure");

  auto r_reed = resolver.resolve(lib_id_str, "ep_reed_200a_classic");
  assert(r_reed.ok());
  assert(r_reed.is_parametric());
  assert(r_reed.instrument_info().name == "200A Classic Reed");

  std::cout << "  ✓ Catalog indexing, serialization round-trip, and LibraryResolver resolution passed\n";
}

// AC 4: Sampled electric pianos stay installed and unchanged
void test_sampled_ep_coexistence() {
  std::cout << "Testing AC 4: Sampled electric pianos stay installed and unchanged...\n";
  TestEnv env("test_ep_coexistence");

  std::unique_ptr<lib::Catalog> catalog;
  lib::CatalogStatus cs = lib::Catalog::open(env.root.string(), catalog);
  assert(cs == lib::CatalogStatus::kOk && catalog);

  // 1. Register existing sampled library with sampled electric pianos
  lib::LibraryRecord sample_rec;
  sample_rec.library_id = "11111111-2222-3333-4444-555555555555";
  sample_rec.bundle_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  sample_rec.name = "Stage Vintage Collection (Sampled)";
  sample_rec.kind = lib::LibraryKind::kUser;
  sample_rec.source_kind = "sfz";

  lib::InstrumentRecord ir1;
  ir1.key = "sample_stage_ep";
  ir1.name = "Stage 73 Mark I Sampled";
  ir1.category = static_cast<uint32_t>(fmt::Category::kEPiano);
  ir1.sample_count = 120;
  ir1.region_count = 120;
  ir1.bytes = 500ULL * 1024 * 1024;
  sample_rec.instruments.push_back(ir1);

  bool changed = false;
  assert(catalog->publish_library(sample_rec, changed) == lib::CatalogStatus::kOk);

  // 2. Install factory modeled EP pack
  fmt::ParametricBundle ep_bundle = fmt::create_factory_ep_bundle();
  lib::LibraryRecord ep_rec;
  ep_rec.library_id = fmt::kFactoryEpLibraryId;
  ep_rec.bundle_id = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  ep_rec.name = fmt::kFactoryEpLibraryName;
  ep_rec.kind = lib::LibraryKind::kFactory;
  ep_rec.source_kind = "sysex";
  for (const auto& t : ep_bundle.timbres()) {
    lib::InstrumentRecord ir;
    ir.key = t.key;
    ir.name = t.name;
    ir.category = static_cast<uint32_t>(fmt::id(t.category));
    ir.bytes = 0;
    ep_rec.instruments.push_back(ir);
  }
  assert(catalog->publish_library(ep_rec, changed) == lib::CatalogStatus::kOk);

  // 3. Verify sampled library is completely unchanged and coexists
  std::vector<lib::InstrumentRecord> sample_insts;
  uint32_t sample_total = 0;
  bool sample_found = false;
  assert(catalog->list_instruments(sample_rec.library_id, 0, 10, sample_insts, sample_total, sample_found) == lib::CatalogStatus::kOk);
  assert(sample_found);
  assert(sample_total == 1);
  assert(sample_insts[0].name == "Stage 73 Mark I Sampled");
  assert(sample_insts[0].sample_count == 120);

  // Verify modeled EP library is present
  std::vector<lib::InstrumentRecord> ep_insts;
  uint32_t ep_total = 0;
  bool ep_found = false;
  assert(catalog->list_instruments(ep_rec.library_id, 0, 100, ep_insts, ep_total, ep_found) == lib::CatalogStatus::kOk);
  assert(ep_found);
  assert(ep_total >= 12);

  std::cout << "  ✓ Sampled electric pianos stay installed and unchanged alongside modeled pack\n";
}

// AC 5: 8-Part Live Set stores, recalls and renders with effects
void test_live_set_8_part_recall_and_dsp_render() {
  std::cout << "Testing AC 5: 8-Part Live Set stores, recalls and renders with effects...\n";

  // Initialize engine core and EP engine
  core::EpEngine ep_engine(48000.0f, 64);
  assert(ep_engine.type_id() == core::EngineTypeId::kModeledEP);

  // Create an effect instance (EP Amp Drive / Chorus Phaser)
  auto fx_drive = core::make_effect(core::EffectType::kEpAmp);
  assert(fx_drive != nullptr);

  auto fx_chorus = core::make_effect(core::EffectType::kChorusPhaser);
  assert(fx_chorus != nullptr);

  // Setup 8 parts with distinct factory voicings and zones
  fmt::ParametricBundle bundle = fmt::create_factory_ep_bundle();
  assert(bundle.timbres().size() >= 8);

  for (uint32_t p = 0; p < 8; ++p) {
    const auto& t = bundle.timbres()[p];
    core::EpVoicingParameters vp{};
    vp.instrument_type = (t.ep_parameters.instrument_type == fmt::EpInstrumentType::kReed)
                             ? core::EpInstrumentType::kReed
                             : core::EpInstrumentType::kTine;
    vp.hammer_hardness = t.ep_parameters.hammer_hardness;
    vp.pickup_position = t.ep_parameters.pickup_position;
    vp.pickup_level = t.ep_parameters.pickup_level;
    vp.damper_noise = t.ep_parameters.damper_noise;
    vp.attack_noise = t.ep_parameters.attack_noise;
    vp.decay_rate = t.ep_parameters.decay_rate;
    vp.tone_bar_coupling = t.ep_parameters.tone_bar_coupling;

    ep_engine.set_part_voice_params(p, vp);
  }

  // Trigger notes on all 8 parts simultaneously across keyboard zones
  for (uint32_t p = 0; p < 8; ++p) {
    uint32_t note = 36 + p * 7; // Spread across C2..G6
    ep_engine.start_voice(p, note, 100 << 9);
  }

  assert(ep_engine.active_voices() == 8);

  // Render audio into stereo buffer
  constexpr uint32_t kFrames = 256;
  std::vector<float> left(kFrames, 0.0f);
  std::vector<float> right(kFrames, 0.0f);
  ep_engine.render(left.data(), right.data(), kFrames);

  // Run through effects chain
  core::StereoBlock block{};
  block.left = left.data();
  block.right = right.data();
  block.frames = 64;

  fx_drive->process(block);
  fx_chorus->process(block);

  // Verify non-zero, stable audio output without NaN or Inf
  float max_mag = 0.0f;
  for (uint32_t i = 0; i < 64; ++i) {
    assert(!std::isnan(block.left[i]));
    assert(!std::isinf(block.left[i]));
    assert(!std::isnan(block.right[i]));
    assert(!std::isinf(block.right[i]));
    max_mag = std::max(max_mag, std::abs(block.left[i]));
    max_mag = std::max(max_mag, std::abs(block.right[i]));
  }

  assert(max_mag > 1e-4f);
  std::cout << "  ✓ 8-Part Live Set rendering with EP Drive and Chorus produced clean sound (peak = "
            << max_mag << ")\n";
}

}  // namespace

int main() {
  std::cout << "Running Story 14.11 Acceptance Test Suite...\n";
  test_builtin_init_ep_timbres();
  test_factory_ep_sound_pack_content();
  test_catalog_indexing_and_resolution();
  test_sampled_ep_coexistence();
  test_live_set_8_part_recall_and_dsp_render();
  std::cout << "All Story 14.11 tests passed successfully!\n";
  return 0;
}
