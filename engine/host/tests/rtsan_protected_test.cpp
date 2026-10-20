// Story 6.5 RT safety over a PROTECTED instrument. A real protected library (format 3, AES-256-CTR blocks, signed manifest) is installed in a temp library root,
// resolved, unwrapped with the dev key provider and made resident by the ProtectedHeadProvider -- hash checks and decryption on this (non-real-time) thread. Then the
// block renderer plays it inside a nonblocking function with dense note traffic while a real I/O "edge" thread re-reads, hash-checks and decrypts the blocks of the same
// instrument (EdgeBlockReader) and, later, finds some of them damaged. RealtimeSanitizer aborts on any allocation, lock or blocking call on the render path: the audio
// thread only reads decoded float PCM and the damage flags the edge thread sets. Without the sanitizer it is the same logic (macOS runs it as protected_rt_traffic_test).
//
// The negative control is rtsan_decrypt_unsafe_test.cpp: decrypting a block inside a nonblocking function IS reported by RTSan, so the claim "the render callback does
// not decrypt" is something the sanitizer would catch, not an accident of this test.
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

#include "protected_fixtures.hpp"
#include "velora/core/block_renderer.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/host/dev_key_provider.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/host/protected_head_provider.hpp"

namespace fs = std::filesystem;
using namespace velora;

static void run_block(core::BlockRenderer& br, float* l, float* r, std::uint32_t i) noexcept [[clang::nonblocking]] {
  br.submit({core::make_note_on(0, 0, 48 + (i * 7) % 36, 20000 + (i * 977) % 40000), (i * 5) % 60});
  br.submit({core::make_note_off(0, 0, 48 + ((i + 23) * 7) % 36, 0), (i * 11) % 60});
  if (i % 50 == 0)
    for (std::uint32_t k = 0; k < 30; ++k) br.submit({core::make_note_on(0, 0, 48 + k, 30000 + 500 * k), k});  // pool-full bursts
  br.process_block(l, r);
}

int main() {
  pfx::TmpDir tmp;
  const pfx::Keys keys = pfx::make_keys(5);
  const auto lib = pfx::install_library(tmp.path, 1, {{"multi.sfz", pfx::Spec::kMulti, 0.1f, 40000}}, keys);  // ten blocks per sample
  std::string err;
  host::DevKeyProvider::Options ko;
  auto provider = host::DevKeyProvider::from_bytes(keys.device_priv, ko, err);
  if (!provider) {
    std::printf("rtsan_protected_test: %s\n", err.c_str());
    return 2;
  }
  auto fsp = host::make_posix_fs();
  host::LibraryResolver resolver(fsp, tmp.path);
  resolver.set_protected_trust(std::make_shared<const format::TrustedKeys>(keys.trusted));
  host::DamageRegistry damage;
  host::ProtectedHeadProvider heads(fsp, *provider, &damage);
  auto resolved = resolver.resolve(lib.library_id, "multi.sfz");
  std::shared_ptr<const host::BundleInstrument> inst;
  if (!resolved.ok() || heads.load(resolved, inst) != host::LoadStatus::kOk || !inst) {
    std::printf("rtsan_protected_test: the protected instrument did not load\n");
    return 2;
  }

  core::SampleEngine::Config cfg;
  cfg.max_voices = 48;  // small enough that bursts exhaust the pool
  auto se = std::make_unique<core::SampleEngine>(cfg);
  se->set_instrument(*inst);
  auto br = std::make_unique<core::BlockRenderer>();
  br->bind_engine(*se);
  for (std::uint32_t ch = 0; ch < 16; ++ch) br->set_channel_engine(ch, static_cast<std::uint32_t>(core::EngineTypeId::kSample));

  // The I/O edge thread: re-reads, hash-checks and decrypts every block again and again while the audio thread plays; after a while it "finds" damage
  // in a few blocks (it marks them, exactly as a failed hash check would) and keeps scrubbing.
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> scrubbed{0}, damaged{0}, rounds{0};
  std::thread edge([&] {
    host::EdgeBlockReader reader(inst);
    std::uint32_t round = 0;
    while (!stop.load(std::memory_order_acquire)) {
      const host::ScrubResult r = reader.scrub(&stop);
      scrubbed.fetch_add(r.blocks_checked, std::memory_order_relaxed);
      damaged.fetch_add(r.damaged, std::memory_order_relaxed);
      if (round >= 2) {  // some blocks go bad (a real hash failure marks them the same way)
        inst->mark_block_damaged(round % 3, (round * 3) % 10);
        damage.report({lib.library_id, "multi.sfz", round % 3, (round * 3) % 10, "content_integrity"});
      }
      ++round;
      rounds.store(round, std::memory_order_release);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });

  float l[core::kBlockFrames], r[core::kBlockFrames];
  double energy = 0.0;
  std::uint32_t peak_voices = 0;
  // No wall-clock assumption: the audio loop runs until the edge thread has completed several rounds (each a full scrub; later rounds mark blocks damaged) AND voices have
  // met a damaged block, with at least 8000 blocks of traffic and a generous cap; the assertions below are about what happened, not about how fast.
  std::uint32_t blocks_run = 0;
  for (std::uint32_t i = 0; i < 2000000; ++i) {
    if (i >= 8000 && rounds.load(std::memory_order_acquire) >= 5 && se->content_integrity_events() > 0 && scrubbed.load() >= 90) break;
    ++blocks_run;
    run_block(*br, l, r, i);
    for (std::uint32_t k = 0; k < core::kBlockFrames; ++k) energy += static_cast<double>(l[k]) * l[k] + static_cast<double>(r[k]) * r[k];
    peak_voices = std::max(peak_voices, se->active_voices());
  }
  stop.store(true, std::memory_order_release);
  edge.join();
  const std::uint64_t events = se->content_integrity_events();
  std::printf("rtsan_protected_test: scrubbed=%llu damaged_found=%llu integrity_events=%llu peak_voices=%u energy=%.3g reports=%llu\n",
              static_cast<unsigned long long>(scrubbed.load()), static_cast<unsigned long long>(damaged.load()), static_cast<unsigned long long>(events), peak_voices,
              energy, static_cast<unsigned long long>(damage.reports()));
  int rc = 0;
  auto need = [&](bool cond, const char* what) {
    if (!cond) {
      std::printf("rtsan_protected_test: FAILED condition: %s\n", what);
      rc = 1;
    }
  };
  need(blocks_run < 2000000, "the handshake completed before the cap");
  need(scrubbed.load() >= 90, "the edge thread decrypted and checked the blocks while audio ran");
  need(damaged.load() == 0, "an intact library stays intact under the scrub");
  need(energy > 0.0, "audio was produced");
  need(peak_voices > 4, "several voices played");
  need(events > 0, "voices met the blocks the edge thread marked damaged");
  need(inst->damaged_blocks() > 0 && damage.library_damaged(lib.library_id), "the damage was recorded and reported");
  return rc;
}
