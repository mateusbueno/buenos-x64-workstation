// Story 4.4 RT safety: scene recalls (including storms) interleaved with dense keyboard traffic. The control thread loads a Live Set (two
// sample parts, four organ parts, inserts, zones, sends, drawbars in the scenes) and then recalls scenes as fast as the ledger allows, in
// bursts of 1 to 12 back to back, while the RT side renders blocks inside a nonblocking function with notes starting and ending on 16
// channels and the sustain pedal moving. RealtimeSanitizer aborts on any allocation, lock or blocking call on the RT path (adopting a
// scene generation: applying forced performance values, rebinding nothing, cross-fading changed chains).
// Without the sanitizer it is the same logic: no leaked generation, no stuck voice or router key, the final recall works.
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
#include "velora/host/scene_controller.hpp"

namespace fs = std::filesystem;
using namespace velora;
namespace p = core::params;
namespace lsp = host::liveset::lsp;

static std::string make_library(const std::string& root, std::uint32_t index, const char* key, float level) {
  const std::string fp = "velora-rtsan-scene/" + std::to_string(index);
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
  if (i % 2 == 0) br.submit({core::make_note_on(0, ch, key, 20000 + (i % 40000)), (i * 5) % 64});
  if (i % 2 == 1) br.submit({core::make_note_off(0, ((i - 1) * 7) % 16, 30 + ((i - 1) * 11) % 70), (i * 9) % 64});
  if (i % 97 == 0) br.submit({core::make_control_change(0, 0, 64, 0xFFFFFFFFu), 3});
  if (i % 97 == 50) br.submit({core::make_control_change(0, 0, 64, 0u), 3});
  br.process_block(l, r);
}

int main() {
  char tmpl[] = "/tmp/velora-rtscene-XXXXXX";
  if (!mkdtemp(tmpl)) return 2;
  const std::string root = tmpl;
  const std::string lib_a = make_library(root, 1, "a.sfz", 0.05f), lib_b = make_library(root, 2, "b.sfz", 0.03f);

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

  lsp::LiveSet s = host::liveset::make_default_live_set(fixtures::uuid_n(60), "RT scenes", 8);
  fixtures::timbre(s.mutable_parts(0), lib_a, "a.sfz");
  fixtures::timbre(s.mutable_parts(1), lib_b, "b.sfz");
  s.mutable_parts(1)->set_low_key(60);
  for (int i = 2; i < 6; ++i) {
    lsp::Part* o = s.mutable_parts(i);
    o->clear_timbre();
    o->mutable_engine()->set_engine_type_id(2);
    fixtures::entry(o->mutable_engine()->mutable_params(), p::kDrawbar8, 8);
    o->set_volume(0.5f);
  }
  s.mutable_parts(3)->add_inserts()->set_type(ws::v1::INSERT_TYPE_EQ);
  // Eight scenes that move everything a scene may move.
  for (int k = 0; k < 8; ++k) {
    lsp::Scene* sc_ = s.add_scenes();
    sc_->set_name("scene " + std::to_string(k));
    for (int pi = 0; pi < 6; ++pi) {
      lsp::ScenePart* sp = sc_->add_parts();
      sp->set_volume(static_cast<float>(1 + (k + pi) % 4) / 4.0f);
      sp->set_keyboard_on((k + pi) % 3 != 0);
      sp->set_low_key(static_cast<std::uint32_t>((k * 7 + pi) % 40));
      sp->set_high_key(static_cast<std::uint32_t>(70 + (k * 5 + pi) % 57));
      sp->set_octave((k + pi) % 3 - 1);
      sp->set_transpose((k * 3 + pi) % 7 - 3);
      sp->set_output((k + pi) % 4 == 0 ? ws::v1::PART_OUTPUT_AUX : ws::v1::PART_OUTPUT_MAIN);
      sp->set_send_reverb(static_cast<float>((k + pi) % 5) / 8.0f);
      sp->set_send_delay(static_cast<float>((k * 2 + pi) % 5) / 8.0f);
      if (pi >= 2) {
        fixtures::entry(sp->mutable_engine_params(), p::kDrawbar8, static_cast<float>((k + pi) % 9));
        fixtures::entry(sp->mutable_engine_params(), p::kDrawbar16, static_cast<float>(k % 9));
      }
      if (pi == 3 && k % 2 == 0) {  // the chain changes with the scene
        lsp::InsertChain* c = sp->mutable_inserts();
        c->add_slots()->set_type(k % 4 == 0 ? ws::v1::INSERT_TYPE_COMPRESSOR : ws::v1::INSERT_TYPE_CHORUS_PHASER);
      }
    }
    fixtures::entry(sc_->mutable_master(), p::kReverbLevel, static_cast<float>(k % 4) / 4.0f);  // reverb_level
  }
  const host::LiveSetLoadResult res = loader.load(s);
  if (!res.ok()) {
    std::printf("load failed: %s %s\n", host::to_string(res.status), res.detail.c_str());
    return 1;
  }

  constexpr int kRecalls = 3000;
  std::atomic<bool> control_done{false};
  std::atomic<int> ok_count{0}, busy_count{0}, bad{0};
  std::atomic<std::size_t> max_live{0};
  std::atomic<std::uint32_t> rt_blocks{0};
  std::thread control([&] {
    int sent = 0;
    while (sent < kRecalls) {
      const int burst = 1 + (sent * 7) % 12;  // storms of back to back recalls
      for (int b = 0; b < burst && sent < kRecalls; ++b, ++sent) {
        const host::SceneOutcome o = (sent % 5 == 4) ? scenes.next_scene() : scenes.recall_scene(static_cast<std::uint32_t>((sent * 5) % 8));
        if (o.result == host::SceneResult::kOk || o.result == host::SceneResult::kAtLast) ++ok_count;
        else if (o.result == host::SceneResult::kBusy) ++busy_count;
        else ++bad;
        std::size_t m = max_live.load();
        while (plane.live_generations() > m && !max_live.compare_exchange_weak(m, plane.live_generations())) {}
      }
      const std::uint32_t target = rt_blocks.load() + 6;  // let RT render a few blocks between storms
      while (rt_blocks.load() < target) std::this_thread::sleep_for(std::chrono::microseconds(50));
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
    for (std::uint32_t k = 0; k < 128; ++k) br->submit({core::make_note_off(0, ch, k), 0});  // may overflow the queue: repeat below
  br->submit({core::make_control_change(0, 0, 64, 0u), 0});
  for (int k = 0; k < 600; ++k) {
    if (k % 8 == 0)
      for (std::uint32_t ch = 0; ch < 16; ++ch)
        for (std::uint32_t key = 30; key < 100; ++key) br->submit({core::make_note_off(0, ch, key), 0});
    rt_block(*br, l, r, 1u + 2u * static_cast<std::uint32_t>(k));  // odd i: note offs only
  }
  plane.reclaim();

  const host::SceneOutcome last = scenes.recall_scene(2);
  for (int k = 0; k < 10; ++k) rt_block(*br, l, r, 1u + 2u * static_cast<std::uint32_t>(k));
  plane.reclaim();
  std::printf("rtsan_scene_test: recalls ok=%d busy=%d bad=%d max_live=%zu blocks=%u live_generations=%zu revision=%llu\n", ok_count.load(),
              busy_count.load(), bad.load(), max_live.load(), i, plane.live_generations(), (unsigned long long)plane.revision());
  std::error_code ec;
  fs::remove_all(root, ec);
  const bool ok = bad.load() == 0 && ok_count.load() > kRecalls / 2 && last.result == host::SceneResult::kOk &&
                  max_live.load() <= core::SnapshotExchange::kLedgerSlots && plane.live_generations() <= 2 &&
                  br->router().held_keys() == 0 && sample.active_voices() == 0 && organ.active_voices() == 0 &&
                  br->held_snapshot() == br->snapshots().current();
  return ok ? 0 : 1;
}
