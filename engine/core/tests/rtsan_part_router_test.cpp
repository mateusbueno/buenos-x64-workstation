// Story 4.3: dense keyboard traffic through the Live Set part router, inside a nonblocking function, with the zone model changing
// under it. 10,000 blocks: keys are pressed and released on all 16 keyboard channels, the sustain pedal and parameter events are
// mixed in, and every 3 blocks a new generation is published that moves zones, octave, transpose, the keyboard flags, part volume,
// part state (silent / ready), the engine of a part (sample / organ / FM) and swaps the parts' instruments (sample banks) -- the
// control side (building generations and banks, freeing what RT no longer uses) runs outside the nonblocking function, as on the
// control thread. The voice pool arbiter is reserved with a pool small enough that stealing runs.
//
// What must hold: nothing the router or the engines do allocates or blocks (RTSan); every note that was fanned out is released
// (after every key is up and the pedal is released, no voice remains, the router holds no key); the output is finite and not silent;
// and the run is deterministic (the non-sanitizer build runs it twice and compares a hash of the output). Built twice: with
// RealtimeSanitizer (rtsan_part_router_test, clang >= 20) and without (part_router_traffic_test, everywhere).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/core/snapshot.hpp"

using namespace velora::core;
namespace ports = velora::ports;

namespace {

constexpr std::uint32_t kBlocks = 10000;
constexpr std::uint32_t kGenEvery = 3;
constexpr std::uint32_t kNumBanks = 4;

struct Instrument final : ports::SampleSource {
  std::vector<ports::SampleRegionDesc> regions;
  std::vector<ports::SamplePcm> pcm;
  std::vector<std::vector<float>> data;
  ports::InstrumentView instrument() const noexcept override { return {regions, pcm}; }
};

// Instrument k: a looping tone of its own period, a second looping layer for keys above 72, and a release-trigger region (so the
// stop path starts voices too).
std::shared_ptr<Instrument> make_instrument(std::uint32_t k) {
  auto t = std::make_shared<Instrument>();
  const std::size_t period = 200 + 37 * k;
  std::vector<float> x(period * 8);
  for (std::size_t i = 0; i < x.size(); ++i) x[i] = 0.2f * std::sin(6.2831853f * static_cast<float>(i % period) / static_cast<float>(period));
  t->data.push_back(std::move(x));
  std::vector<float> noise(2000);
  std::uint32_t s = 99 + k;
  for (float& v : noise) {
    s = s * 1664525u + 1013904223u;
    v = (static_cast<float>(s >> 8) / 8388608.0f - 1.0f) * 0.05f;
  }
  t->data.push_back(std::move(noise));
  for (std::size_t i = 0; i < 2; ++i) {
    ports::SamplePcm p;
    p.sample_rate = 48000;
    p.channels = 1;
    p.frames = t->data[i].size();
    p.left = t->data[i].data();
    t->pcm.push_back(p);
  }
  ports::SampleRegionDesc a;
  a.sample_index = 0;
  a.key_lo = 0;
  a.key_hi = 127;
  a.key_center = 60;
  a.loop_mode = ports::SampleLoopMode::kLoopContinuous;
  a.ampeg_release = 0.05f;
  t->regions.push_back(a);
  ports::SampleRegionDesc b = a;
  b.sample_index = 1;
  b.key_lo = 73;
  b.loop_mode = ports::SampleLoopMode::kNoLoop;
  t->regions.push_back(b);
  ports::SampleRegionDesc rel = a;
  rel.sample_index = 1;
  rel.trigger = ports::SampleTrigger::kRelease;
  rel.loop_mode = ports::SampleLoopMode::kNoLoop;
  t->regions.push_back(rel);
  return t;
}

struct Control {
  SnapshotExchange& ex;
  const SampleEngine& sample;
  std::vector<std::shared_ptr<SampleBank>> banks;
  std::vector<std::unique_ptr<Snapshot>> live;
  Snapshot cur = Snapshot::defaults();
  std::uint64_t rev = 0, published = 0, freed = 0, rng = 12345;

  Control(SnapshotExchange& e, const SampleEngine& s) : ex(e), sample(s) {
    for (std::uint32_t k = 0; k < kNumBanks; ++k) {
      auto ins = make_instrument(k);
      banks.push_back(sample.prepare(*ins, ins));
    }
    cur.live_set = true;
  }
  std::uint32_t rnd() {
    rng = rng * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<std::uint32_t>(rng >> 33);
  }
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
  // Part p of generation k: a new zone / octave / transpose / keyboard flag / volume / state / engine / instrument.
  void publish_variant(std::uint32_t k) {
    for (std::uint32_t p = 0; p < params::kPartCount; ++p) {
      PartCfg& c = cur.parts[p];
      const std::uint32_t r = rnd();
      switch ((k + p) % 5) {
        case 0: {  // zones: layers, splits, single keys, inverted-looking edges
          const std::uint32_t lo = r % 100, hi = lo + (rnd() % (128 - lo));
          c.low_key = static_cast<std::uint8_t>(lo);
          c.high_key = static_cast<std::uint8_t>(std::min<std::uint32_t>(hi, 127));
          break;
        }
        case 1:
          c.octave = static_cast<std::int8_t>(static_cast<int>(r % 9) - 4);
          c.transpose = static_cast<std::int8_t>(static_cast<int>(rnd() % 25) - 12);
          break;
        case 2: c.keyboard_on = (r % 3 == 0) ? 0 : 1; break;
        case 3: c.volume = static_cast<float>(r % 11) / 10.0f; break;
        case 4: {
          const std::uint32_t kind = r % 8;
          if (kind < 4) {  // a sample part on one of the banks
            c.engine_type = static_cast<std::uint32_t>(EngineTypeId::kSample);
            c.state = PartState::kReady;
            c.sample_bank = banks[rnd() % kNumBanks];
          } else if (kind == 4) {  // the organ
            c.engine_type = static_cast<std::uint32_t>(EngineTypeId::kOrgan);
            c.state = PartState::kReady;
            c.sample_bank = nullptr;
          } else if (kind == 5) {  // FM: no engine compiled in
            c.engine_type = static_cast<std::uint32_t>(EngineTypeId::kFm);
            c.state = PartState::kEngineNotAvailable;
            c.sample_bank = nullptr;
          } else if (kind == 6) {  // unresolved timbre
            c.engine_type = static_cast<std::uint32_t>(EngineTypeId::kSample);
            c.state = PartState::kMissingLibrary;
            c.sample_bank = nullptr;
          } else {  // asked to sound an engine the registry lacks
            c.engine_type = static_cast<std::uint32_t>(EngineTypeId::kVa);
            c.state = PartState::kReady;
            c.sample_bank = nullptr;
          }
          break;
        }
      }
      c.output = static_cast<std::uint8_t>((k / 7 + p) % 2);
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

// The keyboard player: presses and releases keys on all 16 channels, remembering what is down.
struct Player {
  static constexpr std::size_t kMax = 256;
  std::array<std::uint16_t, kMax> keys{};  // (channel << 7) | key of the keys that are down; fixed storage: the run is nonblocking
  std::size_t count = 0;
  std::uint64_t rng = 777;
  std::uint32_t rnd() {
    rng = rng * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<std::uint32_t>(rng >> 33);
  }
};

void run_block(BlockRenderer& br, Player& pl, OutputBuses ob, std::uint32_t i) noexcept VELORA_NONBLOCKING {
  // Pressing outpaces releasing until about 200 keys are down, so the pool is full and steals; then both even out.
  const std::uint32_t presses = pl.count >= 200 ? 0 : 1 + pl.rnd() % 3;
  std::uint16_t fresh[3];
  for (std::uint32_t n = 0; n < presses; ++n) {
    const std::uint32_t ch = pl.rnd() % 16, key = pl.rnd() % 128;
    br.submit({make_note_on(0, ch, key, 2000 + pl.rnd() % 60000), pl.rnd() % 64});  // re-strikes of held keys happen too
    fresh[n] = static_cast<std::uint16_t>((ch << 7) | key);  // released from the next block on (its note off must not precede its note on)
  }
  const std::uint32_t releases = pl.rnd() % 3;
  for (std::uint32_t n = 0; n < releases && pl.count > 0; ++n) {
    const std::size_t at = pl.rnd() % pl.count;
    const std::uint16_t d = pl.keys[at];
    pl.keys[at] = pl.keys[--pl.count];
    br.submit({make_note_off(0, d >> 7, d & 127, 0), pl.rnd() % 64});
  }
  for (std::uint32_t n = 0; n < presses; ++n) pl.keys[pl.count++] = fresh[n];
  if (pl.rnd() % 4 == 0) br.submit({make_note_off(0, pl.rnd() % 16, pl.rnd() % 128, 0), pl.rnd() % 64});  // a note off nobody asked for
  if (i % 37 == 0) br.submit({make_control_change(0, pl.rnd() % 16, 64, (i / 37) % 2 ? 0xFFFFFFFFu : 0u), 20});  // the pedal
  if (i % 5 == 0)  // parameter events keep addressing parts by channel
    br.submit({make_param_event(*params::find(params::kDrawbar8), i % params::kPartCount, static_cast<double>((i / 5) % 9)), 30});
  if (i % 7 == 0) br.submit({make_param_event(*params::find(params::kSendReverbLevel), (i / 7) % params::kPartCount, static_cast<double>(i % 11) / 10.0), 40});
  br.process_block(ob);
}

bool run_once(std::uint64_t& hash_out, std::uint64_t& gens_out) {
  auto br = std::make_unique<BlockRenderer>(8);
  SampleEngine::Config sc;
  sc.max_voices = 340;  // 256 floor + 64 fade headroom + slack
  SampleEngine sample(sc);
  OrganEngine organ;
  if (!br->bind_engine(sample) || !br->bind_engine(organ)) {
    std::printf("FAIL: cannot bind the engines\n");
    return false;
  }
  VoiceArbiter::LiveSetConfig rc;
  rc.budget = 1800;  // the floors and the organ leave little shared: stealing runs
  rc.organ_rotary_cost = OrganEngine::reservation_cost_units(1);
  rc.floors[0] = {static_cast<std::uint32_t>(EngineTypeId::kSample), 256};
  rc.floor_count = 1;
  if (br->arbiter().reserve(rc) != VoiceArbiter::ReserveStatus::kOk) {
    std::printf("FAIL: cannot reserve the voice pool\n");
    return false;
  }
  Control ctl(br->snapshots(), sample);
  ctl.publish_variant(0);
  Player pl;
  std::uint64_t hash = 1469598103934665603ull;
  bool finite = true;
  float peak = 0.0f;
  float l[kBlockFrames], r[kBlockFrames], al[kBlockFrames], ar[kBlockFrames];
  for (std::uint32_t i = 0; i < kBlocks; ++i) {
    if (i % kGenEvery == 0) ctl.publish_variant(1 + i / kGenEvery);
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
  // Let every key go and the pedal up; the same parts must now release everything they were given.
  for (std::size_t k = 0; k < pl.count; ++k) br->submit({make_note_off(0, pl.keys[k] >> 7, pl.keys[k] & 127, 0), 0});
  br->submit({make_control_change(0, 0, 64, 0u), 1});
  for (int i = 0; i < 3000; ++i) {
    br->process_block(OutputBuses{l, r, al, ar});
    // Organ voices release in 2 ms; samples in <= 50 ms + a 50 ms fade-out; the release-trigger voices are one-shots (<= 2000 frames).
  }
  hash_out = hash;
  bool ok = true;
  if (!finite) { std::printf("FAIL: non-finite output\n"); ok = false; }
  if (!(peak > 0.01f)) { std::printf("FAIL: output is (nearly) silent, peak %g\n", static_cast<double>(peak)); ok = false; }
  if (sample.active_voices() != 0) { std::printf("FAIL: %u sample voices hang\n", sample.active_voices()); ok = false; }
  if (organ.active_voices() != 0) { std::printf("FAIL: %u organ voices hang\n", organ.active_voices()); ok = false; }
  if (br->router().held_keys() != 0) { std::printf("FAIL: the router still holds %u keys\n", br->router().held_keys()); ok = false; }
  if (br->router().fanned_notes() < 10000) { std::printf("FAIL: only %llu notes fanned\n", (unsigned long long)br->router().fanned_notes()); ok = false; }
  if (br->router().dropped_out_of_range() == 0) { std::printf("FAIL: no out-of-range transposition was ever exercised\n"); ok = false; }
  if (br->arbiter().steals() == 0) { std::printf("FAIL: the pool never stole (traffic too light)\n"); ok = false; }
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
  std::printf("part router traffic: %u blocks, %llu generations, deterministic (%016llx)\n", kBlocks, static_cast<unsigned long long>(ga),
              static_cast<unsigned long long>(a));
  return 0;
}
