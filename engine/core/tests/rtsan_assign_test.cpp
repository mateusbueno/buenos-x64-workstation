// Story 4.10: encoder storms in ASSIGN mode beside dense keyboard traffic and generation changes, inside a nonblocking function. 10,000 blocks: organ
// notes on all 16 channels with the sustain pedal and ordinary parameter events, and in the same blocks 1..6 encoder steps (deltas up to +-127, some
// huge, some malformed), encoder mode changes (PART / ALL / ASSIGN), select-part events and control fences. Every 3 blocks the control side publishes a
// generation with another ASSIGN map (assignable parameters of every type, empty slots, a non-assignable, an unknown and a structural id) and, now and
// then, a new program generation -- building and freeing generations happens outside the nonblocking function, as on the control thread.
//
// What must hold: nothing the encoder path does allocates or blocks (RTSan); every live value stays inside its registry range; the output is finite and
// not silent; the counters show every branch ran (applied, empty slot, unrouted, rejected, bad events); and the run is deterministic (two runs agree on a
// hash of the output and of the whole last-value table). Built twice: with RealtimeSanitizer (rtsan_assign_test, clang >= 20) and without
// (assign_traffic_test, everywhere).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "velora/core/assign_events.hpp"
#include "velora/core/block_renderer.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/snapshot.hpp"

using namespace velora::core;

namespace {

constexpr std::uint32_t kBlocks = 10000;
constexpr std::uint32_t kGenEvery = 3;

struct Control {
  SnapshotExchange& ex;
  std::vector<std::unique_ptr<Snapshot>> live;
  std::vector<std::uint16_t> assignable;
  std::uint64_t rev = 0, published = 0, program = 0, rng = 4242;

  explicit Control(SnapshotExchange& e) : ex(e) {
    for (const params::ParamDef& d : params::kParams)
      if (d.cls == params::Class::kPerformance && d.assignable) assignable.push_back(d.id);
  }
  std::uint32_t rnd() {
    rng = rng * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<std::uint32_t>(rng >> 33);
  }
  void reclaim() {
    for (std::size_t i = 0; i < live.size();) {
      if (live[i].get() != ex.current() && ex.safe_to_free(live[i].get())) live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
      else ++i;
    }
  }
  bool used(std::uint32_t idx) const {
    for (const auto& s : live)
      if (s->ledger_index == idx) return true;
    return false;
  }
  void publish_variant() {
    reclaim();
    std::uint32_t idx = 0;
    while (idx < SnapshotExchange::kLedgerSlots && used(idx)) ++idx;
    if (idx == SnapshotExchange::kLedgerSlots) return;
    auto s = std::make_unique<Snapshot>(Snapshot::defaults());
    for (std::uint16_t& slot : s->assign) {
      const std::uint32_t r = rnd() % 16;
      if (r < 2) slot = 0;                                  // empty
      else if (r == 2) slot = params::kLineInLevel;         // registered, not assignable
      else if (r == 3) slot = 9999;                         // not in the registry
      else if (r == 4) slot = params::kInsertType;          // structural
      else slot = assignable[rnd() % assignable.size()];
    }
    if (rnd() % 5 == 0) s->program_generation = ++program;  // a scene-recall-like generation
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
  std::uint64_t rng = 31337;
  std::uint32_t rnd() {
    rng = rng * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<std::uint32_t>(rng >> 33);
  }
};

void run_block(BlockRenderer& br, Player& pl, OutputBuses ob, std::uint32_t i) noexcept VELORA_NONBLOCKING {
  if (pl.count < 24 && pl.rnd() % 2 == 0) {
    const std::uint32_t ch = pl.rnd() % 16, key = 24 + pl.rnd() % 72;
    br.submit({make_note_on(0, ch, key, 2000 + pl.rnd() % 60000), pl.rnd() % 64});
    pl.keys[pl.count++] = static_cast<std::uint16_t>((ch << 7) | key);
  }
  if (pl.count > 0 && pl.rnd() % 3 == 0) {
    const std::size_t at = pl.rnd() % pl.count;
    const std::uint16_t d = pl.keys[at];
    pl.keys[at] = pl.keys[--pl.count];
    br.submit({make_note_off(0, d >> 7, d & 127, 0), 64 + pl.rnd() % 64});  // after any note on of this block
  }
  if (i % 41 == 0) br.submit({make_control_change(0, pl.rnd() % 16, 64, (i / 41) % 2 ? 0xFFFFFFFFu : 0u), 20});
  if (i % 5 == 0) br.submit({make_param_event(*params::find(params::kDrawbar8), i % params::kPartCount, static_cast<double>((i / 5) % 9)), 30});
  // The storm.
  const std::uint32_t steps = 1 + pl.rnd() % 6;
  for (std::uint32_t n = 0; n < steps; ++n) {
    const std::uint32_t r = pl.rnd();
    std::int32_t delta = static_cast<std::int32_t>(r % 255) - 127;
    if (r % 97 == 0) delta = (r & 0x100) ? 30000 : -30000;  // saturating
    br.submit({make_encoder_step_event((r >> 9) % 8, delta), (r >> 12) % 64});
  }
  if (i % 11 == 0) br.submit({Ump{make_encoder_step_event(0, 1).word0, 0x7FFFFFFFu}, 3});  // malformed: outside int16
  if (i % 13 == 0) br.submit({make_encoder_mode_event(static_cast<EncoderMode>((i / 13) % 3 == 0 ? 0 : 2)), pl.rnd() % 64});  // mostly ASSIGN
  if (i % 17 == 0) br.submit({make_encoder_mode_event(EncoderMode::kAll), 40});
  if (i % 19 == 0) br.submit({make_select_part_event((i / 19) % params::kPartCount), pl.rnd() % 64});
  if (i % 23 == 0) br.submit({make_select_part_event(params::kPartCount + i % 5), 10});  // malformed: no such part
  if (i % 29 == 0) br.submit({make_fence_event(i), 50});
  br.process_block(ob);
}

bool run_once(std::uint64_t& hash_out, std::uint64_t& gens_out) {
  auto br = std::make_unique<BlockRenderer>(8);
  OrganEngine organ;
  if (!br->bind_engine(organ)) {
    std::printf("FAIL: cannot bind the organ\n");
    return false;
  }
  for (std::uint32_t p = 0; p < params::kPartCount; ++p) br->set_channel_engine(p, static_cast<std::uint32_t>(EngineTypeId::kOrgan));
  Control ctl(br->snapshots());
  ctl.publish_variant();
  Player pl;
  std::uint64_t hash = 1469598103934665603ull;
  bool finite = true;
  float peak = 0.0f;
  float l[kBlockFrames], r[kBlockFrames], al[kBlockFrames], ar[kBlockFrames];
  for (std::uint32_t i = 0; i < kBlocks; ++i) {
    if (i % kGenEvery == 0) ctl.publish_variant();
    if (i % 17 == 0) ctl.reclaim();
    run_block(*br, pl, OutputBuses{l, r, al, ar}, i);
    for (std::uint32_t f = 0; f < kBlockFrames; ++f) {
      for (float v : {l[f], r[f], al[f], ar[f]}) {
        finite = finite && std::isfinite(v);
        peak = std::max(peak, std::fabs(v));
      }
      std::uint32_t bits[2];
      std::memcpy(&bits[0], &l[f], 4);
      std::memcpy(&bits[1], &r[f], 4);
      for (std::uint32_t b : bits) hash = (hash ^ b) * 1099511628211ull;
    }
  }
  gens_out = ctl.published;
  bool ok = true;
  bool in_range = true;
  for (const params::ParamDef& d : params::kParams) {
    if (d.cls != params::Class::kPerformance) continue;
    for (std::uint32_t p = 0; p < (d.scope == params::Scope::kPart ? params::kPartCount : 1u); ++p) {
      const float v = br->last_values().load(params::perf_slot(d, p));
      in_range = in_range && v >= static_cast<float>(d.min) && v <= static_cast<float>(d.max);
      std::uint32_t bits;
      std::memcpy(&bits, &v, 4);
      hash = (hash ^ bits) * 1099511628211ull;
    }
  }
  hash_out = hash;
  const EncoderState::Counters c = br->encoder_state().counters();
  if (!finite) { std::printf("FAIL: non-finite output\n"); ok = false; }
  if (!(peak > 0.01f)) { std::printf("FAIL: output is (nearly) silent, peak %g\n", static_cast<double>(peak)); ok = false; }
  if (!in_range) { std::printf("FAIL: a live value left its range\n"); ok = false; }
  if (c.steps_applied < 5000 || c.steps_empty_slot == 0 || c.steps_rejected == 0 || c.events_rejected == 0) {
    std::printf("FAIL: a branch never ran (applied %llu, empty %llu, unrouted %llu, rejected %llu, bad events %llu)\n", (unsigned long long)c.steps_applied,
                (unsigned long long)c.steps_empty_slot, (unsigned long long)c.steps_unrouted, (unsigned long long)c.steps_rejected, (unsigned long long)c.events_rejected);
    ok = false;
  }
  if (br->last_values().load_fence_ack() == 0) { std::printf("FAIL: no fence was acknowledged\n"); ok = false; }
  return ok;
}

}  // namespace

int main() {
  std::uint64_t a = 0, b = 0, ga = 0, gb = 0;
  if (!run_once(a, ga) || !run_once(b, gb)) return 1;
  if (ga < 500) {
    std::printf("FAIL: only %llu generations published\n", static_cast<unsigned long long>(ga));
    return 1;
  }
  if (a != b || ga != gb) {
    std::printf("FAIL: two runs differ (%016llx vs %016llx)\n", static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
    return 1;
  }
  std::printf("assign traffic: %u blocks, %llu generations, deterministic (%016llx)\n", kBlocks, static_cast<unsigned long long>(ga), static_cast<unsigned long long>(a));
  return 0;
}
