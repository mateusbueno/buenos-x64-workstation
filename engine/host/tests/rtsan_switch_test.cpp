// Story 4.5 RT safety: seamless Live Set switches, with reservation handovers, interleaved with dense keyboard traffic. A control thread jumps between
// four Live Sets that reserve the voice pool differently (sample + organ + inserts, split sample parts, organs only, eight sample parts), sometimes
// jumping twice in a row so the first background load is cancelled, while the RT side renders blocks inside a nonblocking function with notes starting
// and ending on 16 channels and the sustain pedal moving. RealtimeSanitizer aborts on any allocation, lock or blocking call on the RT path: adopting a
// switch generation (rebinding every part, committing the prepared reservation, releasing nothing it should not), voices of the old layout released
// by the router, and so on. Without the sanitizer it is the same logic: no leaked generation, no stuck voice or router key, the reservation in force is
// the last Live Set's, and every published switch moved the revision by one.
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

#include "live_set_fixtures.hpp"
#include "velora/core/block_renderer.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/format/bundle.hpp"
#include "velora/host/control_plane.hpp"
#include "velora/host/fs_port.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/host/live_set_loader.hpp"
#include "velora/host/live_set_switcher.hpp"
#include "velora/host/scene_controller.hpp"

namespace fs = std::filesystem;
using namespace velora;
namespace p = core::params;
namespace lsp = host::liveset::lsp;

static std::string make_library(const std::string& root, std::uint32_t index, const char* key, float level) {
  const std::string fp = "velora-rtsan-switch/" + std::to_string(index);
  const format::Uuid lib = format::library_id_from_fingerprint({reinterpret_cast<const std::uint8_t*>(fp.data()), fp.size()});
  format::BundleWriter w;
  w.set_library_id(lib);
  auto ins = w.add_instrument(key, key);
  std::vector<std::int32_t> pcm(4800, format::float_to_bits(level));
  if (!ins.ok() || w.add_sample(ins.value(), {"s", {1, 32, true, 48000}, 4800, pcm}) != format::Error::kNone) std::exit(2);
  format::Region r;
  r.sample = 0;
  r.key_lo = 0;
  r.key_hi = 127;
  r.key_center = 60;
  r.loop_mode = format::LoopMode::kLoopContinuous;
  r.ampeg.release = 0.05f;
  if (w.add_region(ins.value(), r) != format::Error::kNone) std::exit(2);
  auto bytes = w.finalize();
  if (!bytes.ok()) std::exit(2);
  auto probe = format::BundleReader::open_memory(bytes.value());
  if (!probe.ok()) std::exit(2);
  const std::string lib_id = format::uuid_to_string(lib), bundle_id = format::to_hex(probe.value().bundle_id());
  const fs::path dir = fs::path(root) / "lib" / lib_id / bundle_id;
  fs::create_directories(dir);
  std::ofstream f(dir / "bundle.vlb", std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(bytes.value().data()), static_cast<std::streamsize>(bytes.value().size()));
  f.close();
  fs::create_symlink(bundle_id, fs::path(root) / "lib" / lib_id / "current");
  return lib_id;
}

static void rt_block(core::BlockRenderer& br, float* l, float* r, std::uint32_t i) noexcept VELORA_NONBLOCKING {
  const std::uint32_t ch = (i * 7) % 16, key = 30 + (i * 11) % 70;
  // Dense traffic, with a quiet stretch (no new notes) every third 400-block window: a snapshot generation stays pinned in the ledger while any key
  // bound to it still sounds, and notes struck every few blocks on 70 keys never let go of it. The quiet stretches are the rests of a real show.
  const bool quiet = (i / 400) % 3 == 2;
  if (i % 2 == 0 && !quiet) br.submit({core::make_note_on(0, ch, key, 20000 + (i % 40000)), (i * 5) % 64});
  if (i % 2 == 1) br.submit({core::make_note_off(0, ((i - 1) * 7) % 16, 30 + ((i - 1) * 11) % 70), (i * 9) % 64});
  if (i % 97 == 0) br.submit({core::make_control_change(0, 0, 64, 0xFFFFFFFFu), 3});
  if (i % 97 == 50) br.submit({core::make_control_change(0, 0, 64, 0u), 3});
  br.process_block(l, r);
}

static void save(const std::string& dir, const lsp::LiveSet& s) {
  std::string bytes;
  if (host::liveset::serialize(s, bytes) != host::liveset::Status::kOk) std::exit(2);
  std::ofstream f(dir + "/" + s.id() + ".liveset", std::ios::binary | std::ios::trunc);
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

static lsp::LiveSet organ_parts(lsp::LiveSet s, int from, int to) {
  for (int i = from; i < to; ++i) {
    lsp::Part* o = s.mutable_parts(i);
    o->clear_timbre();
    o->mutable_engine()->set_engine_type_id(2);
    fixtures::entry(o->mutable_engine()->mutable_params(), p::kDrawbar8, 8);
    o->set_volume(0.5f);
  }
  return s;
}

int main() {
  char tmpl[] = "/tmp/velora-rtswitch-XXXXXX";
  if (!mkdtemp(tmpl)) return 2;
  const std::string root = tmpl, dir = root + "/sets";
  fs::create_directories(dir);
  const std::string lib_a = make_library(root, 1, "a.sfz", 0.05f), lib_b = make_library(root, 2, "b.sfz", 0.03f);

  // S1: two sample parts, two organ parts, an insert. S2: split sample parts. S3: organs only (no sample floor). S4: eight sample parts.
  lsp::LiveSet s1 = organ_parts(host::liveset::make_default_live_set(fixtures::uuid_n(61), "S1", 8), 2, 4);
  fixtures::timbre(s1.mutable_parts(0), lib_a, "a.sfz");
  fixtures::timbre(s1.mutable_parts(1), lib_b, "b.sfz");
  s1.mutable_parts(3)->add_inserts()->set_type(ws::v1::INSERT_TYPE_EQ);
  lsp::LiveSet s2 = host::liveset::make_default_live_set(fixtures::uuid_n(62), "S2", 8);
  fixtures::timbre(s2.mutable_parts(0), lib_a, "a.sfz");
  s2.mutable_parts(0)->set_high_key(59);
  s2.mutable_parts(0)->set_octave(1);
  fixtures::timbre(s2.mutable_parts(1), lib_b, "b.sfz");
  s2.mutable_parts(1)->set_low_key(60);
  s2.mutable_parts(1)->set_transpose(-3);
  lsp::LiveSet s3 = organ_parts(host::liveset::make_default_live_set(fixtures::uuid_n(63), "S3", 8), 2, 6);
  lsp::LiveSet s4 = host::liveset::make_default_live_set(fixtures::uuid_n(64), "S4", 8);
  for (int i = 0; i < 8; ++i) fixtures::timbre(s4.mutable_parts(i), i % 2 ? lib_b : lib_a, i % 2 ? "b.sfz" : "a.sfz");
  const lsp::LiveSet sets[4] = {s1, s2, s3, s4};
  for (const lsp::LiveSet& s : sets) save(dir, s);

  auto br = std::make_unique<core::BlockRenderer>(8);
  core::SampleEngine::Config sc;
  sc.max_voices = 340;
  core::SampleEngine sample(sc);
  core::OrganEngine organ;
  if (!br->bind_engine(sample) || !br->bind_engine(organ)) return 2;
  host::ControlEventQueue q;
  host::ControlPlane plane(br->snapshots(), br->last_values(), q, [] { return std::uint64_t{0}; });
  host::SceneController scenes(plane);
  host::LibraryResolver resolver(host::make_posix_fs(), root);
  host::RamHeadProvider heads;
  host::LiveSetLoaderOptions lo;
  lo.scenes = &scenes;
  host::LiveSetLoader loader(*br, plane, resolver, heads, &sample, lo);
  if (!loader.load(s1).ok()) {
    std::printf("initial load failed\n");
    return 1;
  }
  host::LiveSetSwitcher sw(loader, plane, host::SwitcherOptions{});
  sw.set_catalog(host::scan_live_set_dir(dir, 8));
  if (sw.catalog().size() != 4) return 2;

  constexpr int kJumps = 160;
  std::atomic<bool> control_done{false};
  std::atomic<std::uint32_t> rt_blocks{0};
  std::atomic<int> bad{0};
  std::string last_id = s1.id();
  std::thread control([&] {
    for (int i = 0; i < kJumps; ++i) {
      const std::string& target = sets[(static_cast<std::size_t>(i) * 7 + 3) % 4].id();
      sw.jump_to(target);
      if (i % 3 == 0) sw.jump_to(sets[(static_cast<std::size_t>(i) * 5 + 1) % 4].id());  // the first load is cancelled (or already done)
      std::size_t spins = 0;
      while (sw.loading() && spins++ < 200000) {
        sw.tick();
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
      if (sw.loading()) ++bad;
      if (sw.status().state == host::SwitchState::kFailed) ++bad;  // nothing here may fail
      plane.reclaim();
      // Let RT render a few hundred blocks between switches, and wait until the ledger has room: the pace is in rendered blocks, not time, so it does
      // not depend on scheduling. (A commit that finds the 32-slot ledger full is retried for a few seconds and then reported as busy.)
      const std::uint32_t target_blocks = rt_blocks.load() + 200;
      while (rt_blocks.load() < target_blocks) std::this_thread::sleep_for(std::chrono::microseconds(50));
      for (int w = 0; w < 200000 && plane.live_generations() > 20; ++w) {
        plane.reclaim();
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    }
    control_done.store(true);
  });

  float l[core::kBlockFrames], r[core::kBlockFrames];
  std::uint32_t i = 0;
  while (!control_done.load() || i < 300) {
    rt_block(*br, l, r, i++);
    rt_blocks.store(i);
    if (i % 4 == 0) std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
  control.join();
  for (std::uint32_t ch = 0; ch < 16; ++ch)
    for (std::uint32_t k = 0; k < 128; ++k) br->submit({core::make_note_off(0, ch, k), 0});  // may overflow the queue: repeated below
  br->submit({core::make_control_change(0, 0, 64, 0u), 0});
  for (int k = 0; k < 1200; ++k) {
    if (k % 8 == 0)
      for (std::uint32_t ch = 0; ch < 16; ++ch)
        for (std::uint32_t key = 30; key < 100; ++key) br->submit({core::make_note_off(0, ch, key), 0});
    rt_block(*br, l, r, 1u + 2u * static_cast<std::uint32_t>(k));  // odd i: note offs only
  }
  plane.reclaim();

  const std::string loaded = loader.loaded_id();
  const bool is_organ_only = loaded == s3.id();
  const bool has_organ = loaded == s1.id() || loaded == s3.id();
  const auto res = br->arbiter().reservation();
  std::printf("rtsan_switch_test: published=%llu cancelled=%u revision=%llu live_generations=%zu blocks=%u loaded=%s organ_rotary=%llu floors=%llu\n",
              (unsigned long long)sw.switches_published(), sw.status().cancelled_loads, (unsigned long long)plane.revision(), plane.live_generations(), i,
              loader.loaded_name().c_str(), (unsigned long long)res.organ_rotary, (unsigned long long)res.floors);
  std::error_code ec;
  fs::remove_all(root, ec);
  const bool ok = bad.load() == 0 && sw.switches_published() > static_cast<std::uint64_t>(kJumps) / 2 &&
                  plane.revision() == 1 + sw.switches_published() && plane.live_generations() == 1 &&
                  br->router().held_keys() == 0 && sample.active_voices() == 0 && organ.active_voices() == 0 &&
                  br->held_snapshot() == br->snapshots().current() && (sw.status().state == host::SwitchState::kReady || sw.status().state == host::SwitchState::kIdle) &&
                  (has_organ ? res.organ_rotary > 0 : res.organ_rotary == 0) && (is_organ_only ? res.floors == 0 : res.floors > 0) &&
                  br->snapshots().current()->reservation_epoch != 0;
  return ok ? 0 : 1;
}
