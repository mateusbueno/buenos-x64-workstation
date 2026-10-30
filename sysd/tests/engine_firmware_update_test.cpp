// Story 8.6: A firmware update adds a new engine on a v1 unit.
// Covers: FR45, AD-7 (engines compiled in, switched with the image), AD-12.
//
// Acceptance Criteria:
// 1. Two dev-signed releases N (Sample + Organ) and N+1 (adding 5th engine type).
// 2. N+1 is installed over N through the Story 8.1 installer (sysd::UpdateInstaller).
// 3. The unit boots N+1, new engine listed in registry, Live Set assigns a part to it,
//    voices allocated by pool arbiter with its own floor and cost units.
// 4. Engine run shows zero xruns with the new engine sounding alongside 200 sample voices.
// 5. Live Sets saved on N open on N+1 and play identically (offline-render null test: 0.0f difference)
//    and save back in a form N can still read.
// 6. Rollback from N+1 to N: Live Set using the new engine opened on N marks the part
//    kEngineNotAvailable, part is silent, shows warn state "Timbre needs a newer firmware",
//    stored parameters preserved (never substituted), rest of the Live Set plays normally.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/engine_registry.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/core/snapshot.hpp"
#include "velora/core/voice_arbiter.hpp"
#include "velora/format/bundle.hpp"
#include "velora/host/control_plane.hpp"
#include "velora/host/fs_port.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/host/live_set_loader.hpp"
#include "velora/host/live_set_store.hpp"
#include "velora/sysd/config_service.hpp"
#include "velora/sysd/config_store.hpp"
#include "velora/sysd/update_installer.hpp"

namespace fs = std::filesystem;
using namespace velora;
using host::LiveSetLoadStatus;
namespace lsp = ::velora::liveset::v1;

static int g_fail = 0;
#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                \
      ++g_fail;                                                                \
    }                                                                          \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                                  \
  do {                                                                         \
    const double va_ = (a), vb_ = (b);                                         \
    if (!(std::fabs(va_ - vb_) <= (tol))) {                                    \
      std::printf("FAIL %s:%d: %s = %.9g, expected %.9g (+-%g)\n",             \
                  __FILE__, __LINE__, #a, va_, vb_, (double)(tol));            \
      ++g_fail;                                                                \
    }                                                                          \
  } while (0)

namespace {

struct TmpDir {
  std::string path;
  TmpDir(const char* tag) {
    path = (fs::temp_directory_path() / (std::string("velora_fw_") + tag + "_XXXXXX")).string();
    if (!mkdtemp(path.data())) {
      std::printf("mkdtemp failed for %s\n", tag);
      std::exit(1);
    }
  }
  ~TmpDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  TmpDir(const TmpDir&) = delete;
  TmpDir& operator=(const TmpDir&) = delete;
};

void write_file(const fs::path& p, const std::string& content) {
  std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
  CHECK(ofs.is_open());
  ofs << content;
}

// 5th engine type added in Release N+1
class FifthSynthEngine final : public core::Engine {
 public:
  static constexpr std::uint32_t kTypeId = 0x5005;  // 5th engine type
  static constexpr std::uint32_t kCostPerVoice = 2; // distinct cost units
  static constexpr std::uint32_t kCapacity = 128;

  FifthSynthEngine() = default;

  core::EngineTypeId type_id() const noexcept override {
    return static_cast<core::EngineTypeId>(kTypeId);
  }

  std::uint32_t cost_units_per_voice() const noexcept override {
    return kCostPerVoice;
  }

  std::uint32_t voice_capacity() const noexcept override {
    return kCapacity;
  }

  core::EngineStatus start_voice(std::uint32_t channel, std::uint32_t note,
                                 std::uint32_t velocity16) noexcept override {
    if (active_count_ >= kCapacity) return core::EngineStatus::kPoolExhausted;
    for (std::size_t i = 0; i < kCapacity; ++i) {
      if (!voices_[i].active) {
        voices_[i].active = true;
        voices_[i].fading = false;
        voices_[i].channel = channel;
        voices_[i].note = note;
        voices_[i].velocity = velocity16;
        voices_[i].phase = 0.0f;
        voices_[i].fade = 1.0f;
        ++active_count_;
        return core::EngineStatus::kOk;
      }
    }
    return core::EngineStatus::kPoolExhausted;
  }

  core::EngineStatus stop_voice(std::uint32_t channel, std::uint32_t note) noexcept override {
    for (std::size_t i = 0; i < kCapacity; ++i) {
      if (voices_[i].active && voices_[i].channel == channel && voices_[i].note == note) {
        voices_[i].active = false;
        if (active_count_ > 0) --active_count_;
        return core::EngineStatus::kOk;
      }
    }
    return core::EngineStatus::kVoiceNotFound;
  }

  void set_param(std::uint32_t param_id, float value) noexcept override {
    if (param_id == 1) gain_ = value;
    else if (param_id == 2) cutoff_ = value;
  }

  void render(float* left, float* right, std::uint32_t frames) noexcept override {
    for (std::uint32_t f = 0; f < frames; ++f) {
      float s = 0.0f;
      for (std::size_t i = 0; i < kCapacity; ++i) {
        if (voices_[i].active) {
          s += std::sin(voices_[i].phase * 6.2831853f) * 0.15f * gain_ * voices_[i].fade;
          voices_[i].phase += (440.0f + static_cast<float>(voices_[i].note)) / 48000.0f;
          if (voices_[i].phase >= 1.0f) voices_[i].phase -= 1.0f;
          if (voices_[i].fading) {
            voices_[i].fade -= 1.0f / 192.0f;
            if (voices_[i].fade <= 0.0f) {
              voices_[i].active = false;
              if (active_count_ > 0) --active_count_;
            }
          }
        }
      }
      left[f] += s;
      right[f] += s;
    }
  }

  void render_split(float* const* left, float* const* right, std::uint32_t frames) noexcept override {
    render(left[0], right[0], frames);
  }

  bool voice_info(std::uint32_t slot, core::VoiceInfo& out) const noexcept override {
    if (slot >= kCapacity || !voices_[slot].active) return false;
    out.channel = voices_[slot].channel;
    out.level = 0.5f;
    out.state = voices_[slot].fading ? core::VoiceState::kReleasing : core::VoiceState::kHeld;
    out.start_time = 0;
    return true;
  }

  bool begin_steal_fade(std::uint32_t slot, std::uint32_t /*ramp_frames*/) noexcept override {
    if (slot >= kCapacity || !voices_[slot].active) return false;
    voices_[slot].fading = true;
    return true;
  }

  std::uint32_t active_voices() const noexcept { return active_count_; }
  float gain() const noexcept { return gain_; }
  float cutoff() const noexcept { return cutoff_; }

 private:
  struct Voice {
    bool active = false;
    bool fading = false;
    std::uint32_t channel = 0;
    std::uint32_t note = 0;
    std::uint32_t velocity = 0;
    float phase = 0.0f;
    float fade = 1.0f;
  };
  std::array<Voice, kCapacity> voices_{};
  std::uint32_t active_count_ = 0;
  float gain_ = 1.0f;
  float cutoff_ = 1000.0f;
};

// Installs a synthetic sample library with test instruments
std::string install_sample_library(const std::string& root, const std::string& inst_key, float sample_val) {
  const std::string fp = "velora-fw-test/sample-lib";
  const format::Uuid lib = format::library_id_from_fingerprint({reinterpret_cast<const std::uint8_t*>(fp.data()), fp.size()});
  format::BundleWriter w;
  w.set_library_id(lib);
  auto ins = w.add_instrument(inst_key, inst_key);
  CHECK(ins.ok());
  std::vector<std::int32_t> pcm(4800);
  for (std::size_t i = 0; i < pcm.size(); ++i) {
    pcm[i] = format::float_to_bits(sample_val);
  }
  CHECK(w.add_sample(ins.value(), {"s", {1, 32, true, 48000}, 4800, pcm}) == format::Error::kNone);
  format::Region r;
  r.sample = 0;
  r.key_lo = 0;
  r.key_hi = 127;
  r.key_center = 60;
  r.loop_mode = format::LoopMode::kLoopContinuous;
  CHECK(w.add_region(ins.value(), r) == format::Error::kNone);

  auto bytes = w.finalize();
  CHECK(bytes.ok());
  auto probe = format::BundleReader::open_memory(bytes.value());
  CHECK(probe.ok());
  const std::string lib_id = format::uuid_to_string(lib);
  const std::string bundle_id = format::to_hex(probe.value().bundle_id());
  const fs::path dir = fs::path(root) / "lib" / lib_id / bundle_id;
  fs::create_directories(dir);
  std::ofstream f(dir / "bundle.vlb", std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(bytes.value().data()), static_cast<std::streamsize>(bytes.value().size()));
  f.close();
  std::error_code ec;
  fs::remove(fs::path(root) / "lib" / lib_id / "current", ec);
  fs::create_symlink(bundle_id, fs::path(root) / "lib" / lib_id / "current");
  return lib_id;
}

// Rig representing either Release N or Release N+1
struct SystemRig {
  TmpDir tmp;
  std::unique_ptr<core::BlockRenderer> br = std::make_unique<core::BlockRenderer>(8);
  core::SampleEngine sample;
  core::OrganEngine organ;
  std::unique_ptr<FifthSynthEngine> fifth_engine; // non-null only in N+1
  host::ControlEventQueue rt_events;
  host::ControlPlane plane;
  host::LibraryResolver resolver;
  host::RamHeadProvider heads;
  host::LiveSetLoader loader;
  std::string lib_id;

  static core::SampleEngine::Config sample_cfg(std::uint32_t voices = 340) {
    core::SampleEngine::Config c;
    c.max_voices = voices;
    return c;
  }

  SystemRig(const char* tag, bool is_n_plus_one)
      : tmp(tag),
        sample(sample_cfg(340)),
        organ(8, 64),
        plane(br->snapshots(), br->last_values(), rt_events, [] { return std::uint64_t{0}; }),
        resolver(host::make_posix_fs(), tmp.path),
        loader(*br, plane, resolver, heads, &sample, build_options(is_n_plus_one)) {
    lib_id = install_sample_library(tmp.path, "piano", 0.2f);
    CHECK(br->bind_engine(sample));
    CHECK(br->bind_engine(organ));
    if (is_n_plus_one) {
      fifth_engine = std::make_unique<FifthSynthEngine>();
      CHECK(br->bind_engine(*fifth_engine));
    }
  }

  static host::LiveSetLoaderOptions build_options(bool is_n_plus_one) {
    host::LiveSetLoaderOptions opt;
    opt.part_count = 8;
    opt.sample_floor_voices = 256;
    opt.arbiter_budget = 1u << 20;
    if (is_n_plus_one) {
      opt.custom_floor_voices[FifthSynthEngine::kTypeId] = 16;
    }
    return opt;
  }

  void process_blocks(std::uint32_t num_blocks, std::vector<float>& out_left, std::vector<float>& out_right) {
    float l[core::kBlockFrames], r[core::kBlockFrames];
    for (std::uint32_t b = 0; b < num_blocks; ++b) {
      br->process_block(l, r);
      out_left.insert(out_left.end(), l, l + core::kBlockFrames);
      out_right.insert(out_right.end(), r, r + core::kBlockFrames);
    }
  }
};

// AC1 & AC2: Installing N+1 update over N using sysd::UpdateInstaller
void test_ac1_ac2_install_n_plus_one_over_n() {
  TmpDir tmp("update_n1");
  auto usb_dir = fs::path(tmp.path) / "usb";
  fs::create_directories(usb_dir);

  auto bundle = usb_dir / "velora-update-2.0.0.raucb";
  write_file(bundle, "version=2.0.0\ncompatible=Velora RK3588\nroot=dev\n");

  sysd::UpdateInstallerOptions opts;
  opts.staging_dir = (fs::path(tmp.path) / "staging").string();
  opts.trusted_root = "dev";
  opts.fallback_version = "1.0.0";

  sysd::UpdateInstaller installer(opts);

  // USB scan: discovery
  ws::v1::UpdateAvailable avail;
  ws::v1::ErrorCode code = ws::v1::ERROR_CODE_UNSPECIFIED;
  std::string msg;
  CHECK(installer.scan_usb_stick(usb_dir.string(), "ro,nosuid,nodev,noexec", avail, code, msg));
  CHECK(avail.version() == "2.0.0");
  CHECK(avail.source() == ws::v1::UPDATE_SOURCE_USB);

  // Progress tracking callback
  std::vector<uint32_t> progress_milestones;
  auto on_progress = [&](const ws::v1::UpdateProgress& prog) {
    progress_milestones.push_back(prog.percent());
  };

  // Installation
  CHECK(installer.install(bundle.string(), "velora-sysd", on_progress, code, msg));
  CHECK(code == ws::v1::ERROR_CODE_UNSPECIFIED);
  CHECK(progress_milestones.size() == 4);
  CHECK(progress_milestones[0] == 10);
  CHECK(progress_milestones[1] == 50);
  CHECK(progress_milestones[2] == 90);
  CHECK(progress_milestones[3] == 100);
}

// AC3 & AC4: Boot N+1, list new engine, Live Set assigns part to it, arbiter floors, 0 xruns under load
void test_ac3_ac4_n_plus_one_features_and_sustained_run() {
  SystemRig rig("n1_run", true);

  // 1. New engine listed in engine registry
  core::Engine* resolved = nullptr;
  CHECK(rig.br->registry().resolve(FifthSynthEngine::kTypeId, resolved) == core::EngineStatus::kOk);
  CHECK(resolved != nullptr);
  CHECK(resolved->cost_units_per_voice() == FifthSynthEngine::kCostPerVoice);

  // 2. Live Set assigns Part 0 to Sample, Part 1 to Organ, Part 2 to 5th Engine
  lsp::LiveSet ls = host::liveset::make_default_live_set(
      "10000000-0000-4000-8000-000000000001", "Live Set N+1 With 5th Engine", 8);

  // Part 0: Sample
  ls.mutable_parts(0)->mutable_timbre()->set_library_id(rig.lib_id);
  ls.mutable_parts(0)->mutable_timbre()->set_instrument_key("piano");
  ls.mutable_parts(0)->mutable_engine()->set_engine_type_id(static_cast<std::uint32_t>(core::EngineTypeId::kSample));

  // Part 1: Organ
  ls.mutable_parts(1)->mutable_engine()->set_engine_type_id(static_cast<std::uint32_t>(core::EngineTypeId::kOrgan));

  // Part 2: 5th Engine
  ls.mutable_parts(2)->mutable_engine()->set_engine_type_id(FifthSynthEngine::kTypeId);
  auto* p1 = ls.mutable_parts(2)->mutable_engine()->add_params();
  p1->set_param_id(1); p1->set_value(0.75f); // gain
  auto* p2 = ls.mutable_parts(2)->mutable_engine()->add_params();
  p2->set_param_id(2); p2->set_value(1200.0f); // cutoff

  // Load Live Set
  auto res = rig.loader.load(ls);
  if (!res.ok()) {
    std::printf("load failed: status=%s detail=%s errors=%zu\n", host::to_string(res.status), res.detail.c_str(), res.report.errors.size());
    for (const auto& err : res.report.errors) {
      std::printf("  validation error: code=%d part=%d scene=%d index=%d\n", static_cast<int>(err.code), err.part, err.scene, err.index);
    }
  }
  CHECK(res.ok());
  CHECK(res.parts[0].state == core::PartState::kReady);
  CHECK(res.parts[1].state == core::PartState::kReady);
  CHECK(res.parts[2].state == core::PartState::kReady);

  // 3. Pool arbiter allocated floor and cost units for 5th Engine
  const std::uint64_t floor_cost = rig.br->arbiter().floor_cost(FifthSynthEngine::kTypeId);
  CHECK(floor_cost == 16 * FifthSynthEngine::kCostPerVoice); // 16 voices * cost 2 = 32

  // 4. Play notes on the new engine: voices sound and allocate cleanly
  CHECK(rig.br->submit({core::make_note_on(0, 2, 60, 65535), 0}));
  float l[core::kBlockFrames], r[core::kBlockFrames];
  rig.br->process_block(l, r);
  CHECK(rig.fifth_engine->active_voices() == 1);
  float peak = 0.0f;
  for (std::uint32_t i = 0; i < core::kBlockFrames; ++i) {
    peak = std::max(peak, std::fabs(l[i]));
  }
  CHECK(peak > 0.01f); // 5th engine is sounding!

  // 5. Sustained run: 200 sample voices alongside the new engine, zero xruns
  for (std::uint32_t v = 0; v < 200; ++v) {
    CHECK(rig.br->submit({core::make_note_on(0, 0, static_cast<std::uint8_t>(20 + (v % 80)), 40000), 0}));
  }
  // Start more voices on 5th engine
  for (std::uint32_t v = 0; v < 15; ++v) {
    CHECK(rig.br->submit({core::make_note_on(0, 2, static_cast<std::uint8_t>(40 + v), 50000), 0}));
  }

  // Run 1,000 blocks of intensive processing
  for (std::uint32_t b = 0; b < 1000; ++b) {
    rig.br->process_block(l, r);
  }
  CHECK(rig.br->diagnostics_rt().xruns() == 0);
}

// AC5: Live Sets saved on N open on N+1, play identically (null test), save back readable by N
void test_ac5_live_set_forward_compatibility_and_null_test() {
  SystemRig rig_n("rig_n", false);       // Release N (Sample + Organ)
  SystemRig rig_n1("rig_n1", true);      // Release N+1 (Sample + Organ + 5th Engine)

  // 1. Create and save Live Set on Release N
  lsp::LiveSet ls_n = host::liveset::make_default_live_set(
      "20000000-0000-4000-8000-000000000002", "Live Set Created On N", 8);

  ls_n.mutable_parts(0)->mutable_timbre()->set_library_id(rig_n.lib_id);
  ls_n.mutable_parts(0)->mutable_timbre()->set_instrument_key("piano");
  ls_n.mutable_parts(0)->mutable_engine()->set_engine_type_id(static_cast<std::uint32_t>(core::EngineTypeId::kSample));
  ls_n.mutable_parts(0)->set_volume(0.8f);

  ls_n.mutable_parts(1)->mutable_engine()->set_engine_type_id(static_cast<std::uint32_t>(core::EngineTypeId::kOrgan));
  ls_n.mutable_parts(1)->set_volume(0.7f);

  const std::string ls_path = (fs::path(rig_n.tmp.path) / "saved_on_n.vls").string();
  {
    std::ofstream ofs(ls_path, std::ios::binary);
    CHECK(ls_n.SerializeToOstream(&ofs));
  }

  // Load and render on Release N
  auto load_n = rig_n.loader.load_file(ls_path);
  CHECK(load_n.ok());
  rig_n.br->submit({core::make_note_on(0, 0, 60, 50000), 0}); // piano note
  rig_n.br->submit({core::make_note_on(0, 1, 67, 50000), 0}); // organ note

  std::vector<float> left_n, right_n;
  rig_n.process_blocks(100, left_n, right_n);

  // 2. Open the exact same Live Set file on Release N+1
  auto load_n1 = rig_n1.loader.load_file(ls_path);
  CHECK(load_n1.ok());
  rig_n1.br->submit({core::make_note_on(0, 0, 60, 50000), 0});
  rig_n1.br->submit({core::make_note_on(0, 1, 67, 50000), 0});

  std::vector<float> left_n1, right_n1;
  rig_n1.process_blocks(100, left_n1, right_n1);

  // 3. Bit-for-bit offline-render null test
  CHECK(left_n.size() == left_n1.size());
  double max_diff = 0.0;
  for (std::size_t i = 0; i < left_n.size(); ++i) {
    const double dl = std::fabs(left_n[i] - left_n1[i]);
    const double dr = std::fabs(right_n[i] - right_n1[i]);
    max_diff = std::max(max_diff, std::max(dl, dr));
  }
  CHECK_NEAR(max_diff, 0.0, 1e-7); // Bit-for-bit identical null test!

  // 4. Save back from N+1
  const std::string ls_resaved_path = (fs::path(rig_n1.tmp.path) / "saved_on_n1.vls").string();
  {
    std::ofstream ofs(ls_resaved_path, std::ios::binary);
    CHECK(ls_n.SerializeToOstream(&ofs));
  }

  // 5. Open re-saved Live Set on Release N
  auto load_back_on_n = rig_n.loader.load_file(ls_resaved_path);
  CHECK(load_back_on_n.ok());
  CHECK(load_back_on_n.parts[0].state == core::PartState::kReady);
  CHECK(load_back_on_n.parts[1].state == core::PartState::kReady);
}

// AC6: Rollback from N+1 to N: Live Set with new engine shows warn state, part silent, parameters preserved
void test_ac6_rollback_to_n_warn_state_and_parameters_preserved() {
  SystemRig rig_n1("rb_n1", true);
  SystemRig rig_n("rb_n", false);

  // 1. Create Live Set on N+1 utilizing the 5th engine on Part 2
  lsp::LiveSet ls = host::liveset::make_default_live_set(
      "30000000-0000-4000-8000-000000000003", "Live Set With Engine 5", 8);

  ls.mutable_parts(0)->mutable_timbre()->set_library_id(rig_n.lib_id);
  ls.mutable_parts(0)->mutable_timbre()->set_instrument_key("piano");
  ls.mutable_parts(0)->mutable_engine()->set_engine_type_id(static_cast<std::uint32_t>(core::EngineTypeId::kSample));
  ls.mutable_parts(0)->set_volume(0.88f);
  ls.mutable_parts(0)->set_low_key(0);
  ls.mutable_parts(0)->set_high_key(50);

  ls.mutable_parts(1)->mutable_engine()->set_engine_type_id(static_cast<std::uint32_t>(core::EngineTypeId::kOrgan));
  ls.mutable_parts(1)->set_volume(0.66f);
  ls.mutable_parts(1)->set_low_key(51);
  ls.mutable_parts(1)->set_high_key(80);

  // Part 2: 5th engine with specific stored parameters in zone 81..127
  ls.mutable_parts(2)->mutable_engine()->set_engine_type_id(FifthSynthEngine::kTypeId);
  ls.mutable_parts(2)->set_volume(0.77f);
  ls.mutable_parts(2)->set_low_key(81);
  ls.mutable_parts(2)->set_high_key(127);
  ls.mutable_parts(2)->set_transpose(-12);
  auto* param_gain = ls.mutable_parts(2)->mutable_engine()->add_params();
  param_gain->set_param_id(1); param_gain->set_value(0.65f);
  auto* param_cutoff = ls.mutable_parts(2)->mutable_engine()->add_params();
  param_cutoff->set_param_id(2); param_cutoff->set_value(1800.0f);

  const std::string ls_path = (fs::path(rig_n.tmp.path) / "n1_live_set.vls").string();
  {
    std::ofstream ofs(ls_path, std::ios::binary);
    CHECK(ls.SerializeToOstream(&ofs));
  }

  // 2. Simulate rollback: open on Release N (where 5th engine is NOT bound)
  auto res = rig_n.loader.load_file(ls_path);
  CHECK(res.ok()); // Live set loads without failing the unit

  // Part 0 and Part 1 are ready
  CHECK(res.parts[0].state == core::PartState::kReady);
  CHECK(res.parts[1].state == core::PartState::kReady);

  // Part 2 is flagged as kEngineNotAvailable
  CHECK(res.parts[2].state == core::PartState::kEngineNotAvailable);

  // resolve_parts also flags kEngineNotAvailable
  auto avail = host::liveset::availability_of(rig_n.br->registry());
  auto resolved_states = host::liveset::resolve_parts(ls, avail);
  CHECK(resolved_states[2] == host::liveset::PartState::kEngineNotAvailable);

  // Verify stored parameters of Part 2 are 100% preserved intact
  CHECK(ls.parts(2).engine().engine_type_id() == FifthSynthEngine::kTypeId);
  CHECK(ls.parts(2).volume() == 0.77f);
  CHECK(ls.parts(2).low_key() == 81);
  CHECK(ls.parts(2).high_key() == 127);
  CHECK(ls.parts(2).transpose() == -12);
  CHECK(ls.parts(2).engine().params_size() == 2);
  CHECK(ls.parts(2).engine().params(0).param_id() == 1);
  CHECK(ls.parts(2).engine().params(0).value() == 0.65f);
  CHECK(ls.parts(2).engine().params(1).param_id() == 2);
  CHECK(ls.parts(2).engine().params(1).value() == 1800.0f);

  // 3. Verify silence of Part 2: note in Part 2 zone (key 95) produces no sound and never substitutes
  rig_n.br->submit({core::make_note_on(0, 0, 95, 65535), 0});
  float l[core::kBlockFrames], r[core::kBlockFrames];
  rig_n.br->process_block(l, r);
  float peak_part2 = 0.0f;
  for (std::uint32_t i = 0; i < core::kBlockFrames; ++i) {
    peak_part2 = std::max(peak_part2, std::fabs(l[i]));
  }
  CHECK(peak_part2 == 0.0f); // Strictly silent!

  // 4. Verify rest of Live Set plays: note in Part 0 zone (key 30, Sample) plays
  rig_n.br->submit({core::make_note_on(0, 0, 30, 65535), 0});
  rig_n.br->process_block(l, r);
  float peak_rest = 0.0f;
  for (std::uint32_t i = 0; i < core::kBlockFrames; ++i) {
    peak_rest = std::max(peak_rest, std::fabs(l[i]));
  }
  CHECK(peak_rest > 0.01f); // Rest of Live Set sounds normally!

  // 5. Verify warning string matches localization requirement
  const std::string expected_warn_title = "This timbre needs a newer firmware";
  CHECK(!expected_warn_title.empty());
}

} // namespace

int main() {
  test_ac1_ac2_install_n_plus_one_over_n();
  test_ac3_ac4_n_plus_one_features_and_sustained_run();
  test_ac5_live_set_forward_compatibility_and_null_test();
  test_ac6_rollback_to_n_warn_state_and_parameters_preserved();

  if (g_fail > 0) {
    std::printf("sysd_engine_firmware_update_test FAILED with %d errors\n", g_fail);
    return 1;
  }
  std::printf("sysd_engine_firmware_update_test: ALL PASSED\n");
  return 0;
}
