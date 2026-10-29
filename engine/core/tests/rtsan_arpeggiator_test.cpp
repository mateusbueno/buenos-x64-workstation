// Story 7.2 / 7.3: dense traffic through the per-part EventProcessor slots and Arpeggiator inside a nonblocking function.
// 10,000 blocks with notes pressed and released plus sustain pedal (CC 64) toggles across 8 parts while new Snapshot generations
// cycle Arp On/Off, modes (Up, Down, UpDown, Random, AsPlayed), rates (1/4..1/32 + triplets), octaves (1..4), gates (1..200%),
// swing (50..75%), and latch (On/Off), plus live tempo changes. Verifies zero RT allocation/locks (RTSan), balanced voices
// (zero hanging voices after release), finite non-silent audio, and bit-exact determinism across two runs.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "velora/core/arpeggiator.hpp"
#include "velora/core/block_renderer.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/params_gen.hpp"
#include "velora/core/snapshot.hpp"

using namespace velora::core;

namespace {

constexpr std::uint32_t kTrafficParts = params::kPartCount;
constexpr std::uint32_t kBlocks = 10000;
constexpr std::uint32_t kGenEvery = 5;

struct Control {
  SnapshotExchange& ex;
  std::vector<std::unique_ptr<Snapshot>> live;
  Snapshot cur = Snapshot::defaults();
  std::uint64_t rev = 0, published = 0, rng = 54321;
  std::uint32_t parts = kTrafficParts;

  explicit Control(SnapshotExchange& e, std::uint64_t seed = 54321, std::uint32_t p = kTrafficParts)
      : ex(e), rng(seed), parts(p) {}

  std::uint32_t rnd() {
    rng = rng * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<std::uint32_t>(rng >> 33);
  }

  void reclaim() {
    for (std::size_t i = 0; i < live.size();) {
      if (live[i].get() != ex.current() && ex.safe_to_free(live[i].get())) {
        live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
      } else {
        ++i;
      }
    }
  }

  bool used(std::uint32_t idx) const {
    for (const auto& s : live)
      if (s->ledger_index == idx) return true;
    return false;
  }

  void publish_variant(std::uint32_t k) {
    for (std::uint32_t p = 0; p < parts; ++p) {
      ArpConfig& a = cur.parts[p].arp;
      const std::uint32_t r = rnd();
      a.enabled = ((k + p) % 4) != 0;
      a.mode = static_cast<ArpMode>(r % 5);
      a.rate = static_cast<ArpRate>((r >> 3) % 8);
      a.octaves = static_cast<std::uint8_t>(1 + ((r >> 6) % 4));
      a.gate_percent = static_cast<float>(10 + ((r >> 8) % 160));
      a.swing_percent = static_cast<float>(50 + ((r >> 16) % 26));
      a.latch = ((r >> 22) & 1u) != 0u;
    }
    reclaim();
    std::uint32_t idx = 0;
    while (idx < SnapshotExchange::kLedgerSlots && used(idx)) ++idx;
    if (idx == SnapshotExchange::kLedgerSlots) return;
    auto s = std::make_unique<Snapshot>(cur);
    s->ledger_index = idx;
    s->revision = ++rev;
    ex.publish(s.get());
    live.push_back(std::move(s));
    ++published;
  }
};

struct Player {
  static constexpr std::size_t kMax = 128;
  std::array<std::uint16_t, kMax> keys{};
  std::size_t count = 0;
  std::uint64_t rng = 999;

  std::uint32_t rnd() noexcept VELORA_NONBLOCKING {
    rng = rng * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<std::uint32_t>(rng >> 33);
  }
};

void run_block(BlockRenderer& br, Player& pl, OutputBuses ob, std::uint32_t i,
               std::uint32_t traffic_parts) noexcept VELORA_NONBLOCKING {
  const std::uint32_t presses = pl.count >= 64 ? 0 : 1 + pl.rnd() % 2;
  std::uint16_t fresh[2]{};
  for (std::uint32_t n = 0; n < presses; ++n) {
    const std::uint32_t ch = pl.rnd() % traffic_parts;
    const std::uint32_t key = 36 + (pl.rnd() % 60);
    br.submit({make_note_on(0, ch, key, 10000 + pl.rnd() % 50000), pl.rnd() % kBlockFrames});
    fresh[n] = static_cast<std::uint16_t>((ch << 7) | key);
  }
  const std::uint32_t releases = pl.rnd() % 3;
  for (std::uint32_t n = 0; n < releases && pl.count > 0; ++n) {
    const std::size_t at = pl.rnd() % pl.count;
    const std::uint16_t d = pl.keys[at];
    pl.keys[at] = pl.keys[--pl.count];
    br.submit({make_note_off(0, d >> 7, d & 127, 0), pl.rnd() % kBlockFrames});
  }
  for (std::uint32_t n = 0; n < presses; ++n) pl.keys[pl.count++] = fresh[n];
  if (i % 7 == 0) {
    const std::uint32_t ch = pl.rnd() % traffic_parts;
    const std::uint32_t val32 = (pl.rnd() & 1u) ? 0xFFFFFFFFu : 0u;
    br.submit({make_control_change(0, ch, Ump::kControllerSustain, val32), pl.rnd() % kBlockFrames});
  }
  if (i % 25 == 0) {
    const double bpm = 60.0 + static_cast<double>((i * 13) % 180);
    br.submit({make_param_event(*params::find(params::kTempoBpm), 0, bpm), pl.rnd() % kBlockFrames});
  }
  br.process_block(ob);
}

bool run_once(std::uint64_t& hash_out, std::uint64_t& gens_out, std::uint64_t ctl_seed = 54321,
              std::uint32_t traffic_parts = kTrafficParts) {
  auto br = std::make_unique<BlockRenderer>(64);
  Control ctl(br->snapshots(), ctl_seed, traffic_parts);
  ctl.publish_variant(0);
  Player pl;
  std::uint64_t hash = 1469598103934665603ull;
  bool finite = true;
  float peak = 0.0f;
  float l[kBlockFrames], r[kBlockFrames], al[kBlockFrames], ar[kBlockFrames];
  for (std::uint32_t i = 0; i < kBlocks; ++i) {
    if (i % kGenEvery == 0) ctl.publish_variant(1 + i / kGenEvery);
    if (i % 19 == 0) ctl.reclaim();
    run_block(*br, pl, OutputBuses{l, r, al, ar}, i, traffic_parts);
    for (std::uint32_t f = 0; f < kBlockFrames; ++f) {
      finite = finite && std::isfinite(l[f]) && std::isfinite(r[f]);
      peak = std::max(peak, std::max(std::fabs(l[f]), std::fabs(r[f])));
      std::uint32_t bits[2];
      std::memcpy(&bits[0], &l[f], 4);
      std::memcpy(&bits[1], &r[f], 4);
      for (std::uint32_t b : bits) hash = (hash ^ b) * 1099511628211ull;
    }
  }
  gens_out = ctl.published;
  for (std::uint32_t p = 0; p < traffic_parts; ++p) {
    ArpConfig off_cfg = br->arp(p).config();
    off_cfg.latch = false;
    off_cfg.enabled = false;
    br->set_part_arp(p, off_cfg);
    br->submit({make_control_change(0, p, Ump::kControllerSustain, 0u), 0});
  }
  for (std::size_t k = 0; k < pl.count; ++k) {
    br->submit({make_note_off(0, pl.keys[k] >> 7, pl.keys[k] & 127, 0), 0});
  }
  for (int i = 0; i < 64; ++i) {
    br->process_block(OutputBuses{l, r, al, ar});
  }
  hash_out = hash;
  if (!finite) {
    std::printf("FAIL: non-finite output\n");
    return false;
  }
  if (!(peak > 0.01f)) {
    std::printf("FAIL: output silent (peak %g)\n", static_cast<double>(peak));
    return false;
  }
  if (br->test_tone().active_voices() != 0) {
    std::printf("FAIL: %u test-tone voices still active after release\n", br->test_tone().active_voices());
    return false;
  }
  return true;
}

}  // namespace

int main() {
  for (std::uint32_t parts : {8u, params::kPartCount}) {
    for (std::uint64_t seed : {54321ull, 11111ull, 424242ull}) {
      std::uint64_t h1 = 0, h2 = 0, g1 = 0, g2 = 0;
      if (!run_once(h1, g1, seed, parts) || !run_once(h2, g2, seed, parts)) return 1;
      if (h1 != h2 || g1 != g2) {
        std::printf("FAIL: two runs differ for parts=%u seed=%llu (%016llx vs %016llx)\n",
                    parts, static_cast<unsigned long long>(seed),
                    static_cast<unsigned long long>(h1), static_cast<unsigned long long>(h2));
        return 1;
      }
      std::printf("arpeggiator traffic (parts=%u, seed=%llu): %u blocks, %llu generations, deterministic (%016llx)\n",
                  parts, static_cast<unsigned long long>(seed), kBlocks,
                  static_cast<unsigned long long>(g1), static_cast<unsigned long long>(h1));
    }
  }
  return 0;
}
