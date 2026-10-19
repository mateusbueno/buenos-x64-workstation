// Story 2.7: 10,000 blocks of note traffic through the routing topology with insert-chain changes every few blocks (new generations
// carrying freshly built effect instances), send / line-in level changes and part output switches, inside a nonblocking function.
// The control side (building snapshots and effect instances, freeing generations) runs outside it, as on the control thread. Built
// twice: with RealtimeSanitizer (rtsan_routing_test, clang >= 20) and without (routing_traffic_test, everywhere) -- the second also
// proves determinism: two runs give bit-identical output.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/mixer.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/params_gen.hpp"
#include "velora/core/snapshot.hpp"

using namespace velora::core;

namespace {

constexpr std::uint32_t kBlocks = 10000;
constexpr std::uint32_t kChainEvery = 3;  // a chain change every 3 blocks: faster than the 3.75-block cross-fade

class Noise final : public velora::ports::LineInSource {
 public:
  void read_block(float* l, float* r, std::uint32_t frames) noexcept VELORA_NONBLOCKING override {
    for (std::uint32_t i = 0; i < frames; ++i) {
      s_ = s_ * 1664525u + 1013904223u;
      l[i] = (static_cast<float>(s_ >> 8) / 8388608.0f - 1.0f) * 0.1f;
      r[i] = -l[i];
    }
  }

 private:
  std::uint32_t s_ = 12345;
};

// The control thread's part: builds a generation (new chains for the parts it changes) and frees what RT no longer uses.
struct Control {
  SnapshotExchange& ex;
  std::vector<std::unique_ptr<Snapshot>> live;
  Snapshot cur = Snapshot::defaults();
  std::uint64_t rev = 0;
  std::uint64_t published = 0, freed = 0;

  explicit Control(SnapshotExchange& e) : ex(e) {}

  void reclaim() {
    for (std::size_t i = 0; i < live.size();) {
      if (live[i].get() != ex.current() && ex.safe_to_free(live[i].get())) {
        live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
        ++freed;
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
    const std::uint32_t part = k % 4;
    PartCfg& p = cur.parts[part];
    p.insert_count = static_cast<std::uint8_t>(k % 6);  // 0..5 slots: add, remove
    for (std::uint32_t i = 0; i < p.insert_count; ++i) {
      p.inserts[i].type = static_cast<std::uint8_t>(1 + (k / 6 + i) % 5);  // reorder
      p.inserts[i].bypass = ((k + i) % 4 == 0) ? 1 : 0;                    // bypass
    }
    p.chain = build_chain(p);
    p.output = static_cast<std::uint8_t>((k / 7) % 2);                     // Main / Aux switches
    cur.master[static_cast<std::size_t>(params::find(params::kMasterLevel)->master_index)] = -3.0f * static_cast<float>(k % 3);
    reclaim();
    std::uint32_t idx = 0;
    while (idx < SnapshotExchange::kLedgerSlots && used(idx)) ++idx;
    if (idx == SnapshotExchange::kLedgerSlots) return;  // all ledger slots busy: skip this change (the commit would be BUSY)
    auto s = std::make_unique<Snapshot>(cur);
    s->ledger_index = idx;
    s->revision = ++rev;
    ex.publish(s.get());
    live.push_back(std::move(s));
    ++published;
  }
};

void run_block(BlockRenderer& br, OutputBuses ob, std::uint32_t i) noexcept VELORA_NONBLOCKING {
  const std::uint32_t part = i % 4;
  br.submit({make_note_on(0, part, 36 + (i * 7) % 60, 3000 + (i * 977) % 60000), (i * 5) % 64});
  br.submit({make_note_off(0, part, 36 + ((i + 20) * 7) % 60), (i * 11) % 64});
  br.submit({make_param_event(*params::find(params::kSendReverbLevel), part, static_cast<double>(i % 11) / 10.0), (i * 3) % 64});
  if (i % 2 == 0)
    br.submit({make_param_event(*params::find(params::kSendDelayLevel), (part + 1) % 4, static_cast<double>((i / 2) % 7) / 6.0), 17});
  if (i % 5 == 0) br.submit({make_param_event(*params::find(params::kLineInLevel), 0, static_cast<double>((i / 5) % 4) / 3.0), 40});
  br.process_block(ob);
}

// False when the run is wrong: the output was not finite or never sounded, or the chains did not settle.
bool run_once(std::uint64_t& hash_out, std::uint64_t& chains_published) {
  auto br = std::make_unique<BlockRenderer>(32);
  Noise noise;
  br->set_line_in(&noise);
  Control ctl(br->snapshots());
  ctl.publish_variant(0);
  std::uint64_t hash = 1469598103934665603ull;
  bool finite = true;
  float peak = 0.0f;
  float l[kBlockFrames], r[kBlockFrames], al[kBlockFrames], ar[kBlockFrames];
  for (std::uint32_t i = 0; i < kBlocks; ++i) {
    if (i % kChainEvery == 0) ctl.publish_variant(1 + i / kChainEvery);
    if (i % 17 == 0) ctl.reclaim();
    run_block(*br, OutputBuses{l, r, al, ar}, i);
    for (std::uint32_t f = 0; f < kBlockFrames; ++f) {
      for (float v : {l[f], r[f], al[f], ar[f]}) {
        finite = finite && std::isfinite(v);
        peak = std::max(peak, std::fabs(v));
      }
      std::uint32_t bits[4];
      std::memcpy(&bits[0], &l[f], 4);
      std::memcpy(&bits[1], &r[f], 4);
      std::memcpy(&bits[2], &al[f], 4);
      std::memcpy(&bits[3], &ar[f], 4);
      for (std::uint32_t b : bits) hash = (hash ^ b) * 1099511628211ull;
    }
  }
  chains_published = ctl.published;
  // settle: everything fades out, RT holds no reference but the live notes' and the current chains'
  for (int i = 0; i < 40; ++i) br->process_block(OutputBuses{l, r, al, ar});
  hash_out = hash;
  bool ok = true;
  if (!br->mixer().chains_settled()) { std::printf("FAIL: chains did not settle\n"); ok = false; }
  if (!finite) { std::printf("FAIL: non-finite output\n"); ok = false; }
  if (!(peak > 0.01f)) { std::printf("FAIL: output is (nearly) silent, peak %g\n", static_cast<double>(peak)); ok = false; }
  return ok;
}

}  // namespace

int main() {
  std::uint64_t pub1 = 0, pub2 = 0;
  std::uint64_t a = 0, b = 0;
  if (!run_once(a, pub1) || !run_once(b, pub2)) return 1;
  if (pub1 < 1000) {
    std::printf("FAIL: only %llu generations published\n", static_cast<unsigned long long>(pub1));
    return 1;
  }
  if (a != b || pub1 != pub2) {
    std::printf("FAIL: two runs differ (%016llx vs %016llx)\n", static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
    return 1;
  }
  std::printf("routing traffic: %u blocks, %llu generations, deterministic (%016llx)\n", kBlocks, static_cast<unsigned long long>(pub1),
              static_cast<unsigned long long>(a));
  return 0;
}
