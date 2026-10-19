// Story 2.7 / 2.8: 10,000 blocks of note traffic through the routing topology with insert-chain changes every few blocks (new generations
// carrying freshly built effect instances), send / line-in level changes and part output switches, inside a nonblocking function.
// Story 2.8 adds EQ and compressor traffic: insert parameters, master EQ / compressor values and both master bypass flags cycle with
// every generation, the line-in level moves the compressor input level, and the published gain reductions are checked.
// Story 2.9 adds the chorus/phaser, EP amp and rotary inserts: all three types sit in the cycled chains, their parameters cycle through the whole 0..1
// range, and every few generations sit exactly on the documented thresholds (chorus/phaser mode at 0.5 and its next float, rotary speed at 0.25 / 0.75,
// drive / mix / cabinet at the ends), so the new types and their mode and speed changes run under the sanitizer; the run counts what it covered.
// Story 2.10 adds the reverb: type, time (0.2 s .. 10 s) and return level cycle with every generation (so cross-fades run all the time, and
// the level reaches 0 where the reverb clears itself), the part sends cycle with the events of every block, and the REV master send level moves
// through 0..1 with live events; the run checks that both reverb instances worked (their tails are not required to fade out within the run: at 10 s they outlast it).
// Story 2.11 adds the delay: time (1 ms .. 2 s), tempo sync on/off, every note value, feedback (including values beyond the cap, which the engine clamps),
// return level, damping and ping-pong cycle with every generation (so the time glides, the sync mode switches and the smoothed parameters move all the time), the
// per-part delay sends keep cycling with the events of every block, and the tempo (20 .. 300 bpm) is changed by live events every few blocks; the run checks that
// both delay instances worked and that every note value and both sync modes were seen.
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
  // Story 2.9 coverage: generations whose chains held each of the new types, chorus/phaser in each mode, rotary at each speed.
  std::uint64_t type_seen[6] = {0, 0, 0, 0, 0, 0};
  std::uint64_t reverb_type_seen[3] = {0, 0, 0};
  std::uint64_t delay_note_seen[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0}, delay_sync_seen[2] = {0, 0};
  std::uint64_t chorus_mode_seen[2] = {0, 0}, rotary_speed_seen[3] = {0, 0, 0};

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
      // Story 2.8: EQ / compressor settings cycle through their whole range (threshold, ratio, attack, release, makeup; LOW, MID, MID FREQ, HIGH)
      for (std::uint32_t q = 0; q < p.inserts[i].params.size(); ++q)
        p.inserts[i].params[q] = static_cast<float>((k * 7 + i * 3 + q * 5) % 11) / 10.0f;
      if (k % 4 == 0) {  // thresholds and ends of the Story 2.9 mappings
        auto& pr = p.inserts[i].params;
        const float edge[6] = {0.0f, 0.25f, 0.5f, std::nextafter(0.5f, 1.0f), 0.75f, 1.0f};
        const float e = edge[(k / 4 + i) % 6];
        if (p.inserts[i].type == params::kInsertTypeChorusPhaser) pr = {e, e, 1.0f, e, 1.0f};
        else if (p.inserts[i].type == params::kInsertTypeRotary) pr = {e, e, 0.5f, 0.5f, 0.0f};
        else if (p.inserts[i].type == params::kInsertTypeEpAmp) pr = {e, 1.0f - e, e, 1.0f - e, e};
      }
      ++type_seen[p.inserts[i].type];
      if (p.inserts[i].type == params::kInsertTypeChorusPhaser) ++chorus_mode_seen[p.inserts[i].params[0] > 0.5f ? 1 : 0];
      if (p.inserts[i].type == params::kInsertTypeRotary) ++rotary_speed_seen[p.inserts[i].params[0] < 0.25f ? 0 : (p.inserts[i].params[0] < 0.75f ? 1 : 2)];
    }
    p.chain = build_chain(p);
    p.output = static_cast<std::uint8_t>((k / 7) % 2);                     // Main / Aux switches
    cur.master[static_cast<std::size_t>(params::find(params::kMasterLevel)->master_index)] = -3.0f * static_cast<float>(k % 3);
    // The master section: EQ gains and frequency, compressor threshold / ratio / times / makeup, and both bypass flags change with every generation.
    auto setm = [&](std::uint16_t id, float v) { cur.master[static_cast<std::size_t>(params::find(id)->master_index)] = v; };
    const float u = static_cast<float>(k % 13) / 12.0f;
    setm(params::kMasterEqLowGain, -15.0f + 30.0f * u);
    setm(params::kMasterEqMidGain, 15.0f - 30.0f * u);
    setm(params::kMasterEqMidFreq, 200.0f + 7800.0f * static_cast<float>((k * 5) % 9) / 8.0f);
    setm(params::kMasterEqHighGain, k % 4 == 0 ? 0.0f : -15.0f + 30.0f * static_cast<float>((k * 3) % 7) / 6.0f);
    setm(params::kMasterEqBypass, k % 5 == 1 ? 1.0f : 0.0f);
    setm(params::kMasterCompThreshold, -60.0f + 60.0f * u);
    setm(params::kMasterCompRatio, k % 6 == 0 ? 1.0f : 1.0f + 19.0f * static_cast<float>((k * 7) % 10) / 9.0f);
    setm(params::kMasterCompAttack, 0.1f + 99.9f * static_cast<float>((k * 3) % 8) / 7.0f);
    setm(params::kMasterCompRelease, 10.0f + 990.0f * static_cast<float>((k * 5) % 6) / 5.0f);
    setm(params::kMasterCompMakeup, k % 3 == 0 ? 0.0f : 24.0f * static_cast<float>(k % 5) / 4.0f);
    setm(params::kMasterCompBypass, k % 7 == 2 ? 1.0f : 0.0f);
    {  // the reverb (Story 2.10): type every generation, time over its whole range, level including 0
      static constexpr float kTimes[5] = {0.2f, 0.7f, 2.0f, 5.5f, 10.0f}, kLevels[4] = {1.0f, 0.0f, 0.25f, 0.6f};
      setm(params::kReverbType, static_cast<float>(k % 3));
      setm(params::kReverbTime, kTimes[(k * 3) % 5]);
      setm(params::kReverbLevel, kLevels[(k / 5) % 4]);
      ++reverb_type_seen[k % 3];
    }
    {  // the delay (Story 2.11)
      static constexpr float kTimes[5] = {1.0f, 20.0f, 350.0f, 1000.0f, 2000.0f}, kFeedback[5] = {0.0f, 0.5f, 0.95f, 1.5f, 0.8f}, kLevels[4] = {1.0f, 0.0f, 0.5f, 0.8f},
                             kDamping[4] = {0.0f, 0.25f, 1.0f, 0.6f}, kPingPong[3] = {0.0f, 1.0f, 0.5f};
      const bool sync = k % 3 == 1;
      setm(params::kDelayTime, kTimes[(k * 3) % 5]);
      setm(params::kDelaySync, sync ? 1.0f : 0.0f);
      setm(params::kDelayNote, static_cast<float>(k % 9));
      setm(params::kDelayFeedback, kFeedback[(k * 2) % 5]);
      setm(params::kDelayLevel, kLevels[(k / 4) % 4]);
      setm(params::kDelayDamping, kDamping[(k * 3) % 4]);
      setm(params::kDelayPingpong, kPingPong[(k / 2) % 3]);
      ++delay_note_seen[k % 9];
      ++delay_sync_seen[sync ? 1 : 0];
    }
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
  if (i % 4 == 0) br.submit({make_param_event(*params::find(params::kSendReverbMaster), 0, static_cast<double>((i / 4) % 5) / 4.0), 29});
  if (i % 3 == 0)  // the RT plane's tempo: 20 .. 300 bpm in steps, the extremes included
    br.submit({make_param_event(*params::find(params::kTempoBpm), 0, 20.0 + 28.0 * static_cast<double>(((i / 3) * 7) % 11)), 33});
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
  bool finite = true, gr_ok = true;
  float peak = 0.0f, max_gr = 0.0f;
  float l[kBlockFrames], r[kBlockFrames], al[kBlockFrames], ar[kBlockFrames];
  for (std::uint32_t i = 0; i < kBlocks; ++i) {
    if (i % kChainEvery == 0) ctl.publish_variant(1 + i / kChainEvery);
    if (i % 17 == 0) ctl.reclaim();
    run_block(*br, OutputBuses{l, r, al, ar}, i);
    for (std::size_t g = 0; g < LastValueTable::kGainReductionSlots; ++g) {  // published compressor gain reduction: finite, >= 0, sometimes working
      const float gr = br->last_values().load_gain_reduction(g);
      gr_ok = gr_ok && std::isfinite(gr) && gr >= 0.0f && gr < 120.0f;
      max_gr = std::max(max_gr, gr);
    }
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
  bool covered = true;
  for (std::size_t t : {1u, 2u, 3u}) covered = covered && ctl.type_seen[t] > 300;  // rotary, chorus/phaser, EP amp
  for (int m = 0; m < 2; ++m) covered = covered && ctl.chorus_mode_seen[m] > 100;
  for (int sp = 0; sp < 3; ++sp) covered = covered && ctl.rotary_speed_seen[sp] > 100;
  for (int ty = 0; ty < 3; ++ty) covered = covered && ctl.reverb_type_seen[ty] > 300;
  for (int n = 0; n < 9; ++n) covered = covered && ctl.delay_note_seen[n] > 300;
  covered = covered && ctl.delay_sync_seen[0] > 300 && ctl.delay_sync_seen[1] > 300;
  const bool delay_worked = br->mixer().delay(0).engine_blocks() > 1000 && br->mixer().delay(1).engine_blocks() > 1000;
  const bool reverb_worked = br->mixer().reverb(0).engine_blocks() > 1000 && br->mixer().reverb(1).engine_blocks() > 1000;
  // settle: everything fades out, RT holds no reference but the live notes' and the current chains'
  for (int i = 0; i < 40; ++i) br->process_block(OutputBuses{l, r, al, ar});
  hash_out = hash;
  bool ok = true;
  if (!br->mixer().chains_settled()) { std::printf("FAIL: chains did not settle\n"); ok = false; }
  if (!reverb_worked) { std::printf("FAIL: the reverb instances did not run\n"); ok = false; }
  if (!delay_worked) { std::printf("FAIL: the delay instances did not run\n"); ok = false; }
  if (!finite) { std::printf("FAIL: non-finite output\n"); ok = false; }
  if (!covered) { std::printf("FAIL: the Story 2.9 insert types / modes / speeds were not all exercised\n"); ok = false; }
  if (!gr_ok || !(max_gr > 1.0f)) { std::printf("FAIL: gain reduction invalid or never published (max %g dB)\n", static_cast<double>(max_gr)); ok = false; }
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
