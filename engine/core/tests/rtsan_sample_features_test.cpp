// Story 5.2: 10,000 blocks of traffic through the block loop with keyswitch changes, controller sweeps and filtered, crossfaded regions,
// inside a nonblocking function (RealtimeSanitizer aborts on any allocation, lock or blocking call on the render path: note dispatch with
// keyswitch gating, the CcState, crossfade smoothing, the per-voice filters, release triggers behind keyswitches, the Live Set fan-out of
// controllers, generation changes with banks being swapped, seamless Live Set switches that reset the keyswitches).
//
// Two scenarios run, each twice (the second run must hash equal, so the render is deterministic):
//   plain     channels 0 and 1 are two parts of one engine with their own banks (no Live Set); notes, keyswitch keys and controllers address a
//             part by its channel;
//   live set  four sample parts under Live Set generations (a new generation every 5 blocks that rebinds the banks, a seamless switch with
//             a new reservation epoch every 95 blocks); the keyboard channel is a source and the controllers are fanned out to the parts.
// Built twice: with RealtimeSanitizer (rtsan_sample_features_test, clang >= 20) and without (sample_features_traffic_test, everywhere).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/core/snapshot.hpp"

using namespace velora::core;
namespace ports = velora::ports;

namespace {

constexpr std::uint32_t kBlocks = 10000;
constexpr std::uint32_t kNumBanks = 3;

struct Instrument final : ports::SampleSource {
  std::vector<ports::SampleRegionDesc> regions;
  std::vector<ports::SamplePcm> pcm;
  std::vector<std::vector<float>> data;
  ports::InstrumentView instrument() const noexcept override { return {regions, pcm}; }
  std::uint32_t add(std::size_t frames, bool stereo, std::uint32_t seed) {
    std::vector<float> buf(frames * (stereo ? 2 : 1));
    std::uint32_t s = seed;
    for (float& v : buf) {
      s = s * 1664525u + 1013904223u;
      v = (static_cast<float>(s >> 8) / 8388608.0f - 1.0f) * 0.4f;
    }
    data.push_back(std::move(buf));
    ports::SamplePcm p;
    p.sample_rate = 48000;
    p.channels = stereo ? 2 : 1;
    p.frames = frames;
    p.left = data.back().data();
    p.right = stereo ? data.back().data() + frames : nullptr;
    pcm.push_back(p);
    return static_cast<std::uint32_t>(pcm.size() - 1);
  }
  ports::SampleRegionDesc& region(std::uint32_t sample, int lo, int hi) {
    ports::SampleRegionDesc d;
    d.sample_index = sample;
    d.key_lo = static_cast<std::uint8_t>(lo);
    d.key_hi = static_cast<std::uint8_t>(hi);
    d.key_center = static_cast<std::uint8_t>((lo + hi) / 2);
    d.loop_mode = ports::SampleLoopMode::kLoopContinuous;
    d.ampeg_release = 0.03f;
    regions.push_back(d);
    return regions.back();
  }
};

// Instrument k: keyswitches (36..38 sw_last with default 36, 35 sw_down/sw_up, sw_previous), CC crossfades on CC1 / CC11 / CC74 (both curves,
// both directions, two controllers on one region), every filter type (mono and stereo, resonant, near Nyquist, beyond the clamp), a
// release-trigger region behind sw_down, plain regions.
std::shared_ptr<Instrument> make_instrument(std::uint32_t k) {
  auto t = std::make_shared<Instrument>();
  const auto mono = t->add(6000, false, 11 + k), stereo = t->add(5000, true, 21 + k), shorts = t->add(1500, false, 31 + k);
  const ports::SampleFilterType types[] = {ports::SampleFilterType::kLpf1p, ports::SampleFilterType::kLpf2p, ports::SampleFilterType::kHpf1p,
                                           ports::SampleFilterType::kHpf2p, ports::SampleFilterType::kBpf2p};
  for (int i = 0; i < 3; ++i) {  // articulations
    auto& r = t->region(i == 1 ? stereo : mono, 40, 96);
    r.sw.last = static_cast<std::int16_t>(36 + i);
    r.sw.lokey = 35;
    r.sw.hikey = 39;
    r.sw.default_key = 36;
    r.filter_type = types[(i + k) % 5];
    r.filter_cutoff_hz = 400.0f + 2500.0f * static_cast<float>(i);
    r.filter_resonance_db = 3.0f * static_cast<float>(i);
  }
  {
    auto& up = t->region(mono, 48, 84); up.sw.up = 35; up.volume_db = -6.0f;
    auto& down = t->region(stereo, 48, 84); down.sw.down = 35; down.pan = 30.0f;
    auto& prev = t->region(mono, 60, 60); prev.sw.previous = 62;
    auto& rel = t->region(shorts, 40, 96); rel.sw.down = 35; rel.trigger = ports::SampleTrigger::kRelease; rel.loop_mode = ports::SampleLoopMode::kNoLoop;
  }
  for (int i = 0; i < 4; ++i) {  // crossfades
    auto& r = t->region(i % 2 ? stereo : mono, 44 + 8 * i, 60 + 8 * i);
    r.xf_curve = i % 2 ? ports::SampleXfCurve::kGain : ports::SampleXfCurve::kPower;
    if (i == 0) { r.xfin_count = 1; r.xfin[0] = {1, 0, 127}; }
    if (i == 1) { r.xfout_count = 1; r.xfout[0] = {11, 20, 110}; }
    if (i == 2) { r.xfin_count = 2; r.xfin[0] = {1, 10, 60}; r.xfin[1] = {74, 0, 100}; r.xfout_count = 1; r.xfout[0] = {11, 90, 127}; }
    if (i == 3) { r.xfin_count = 1; r.xfin[0] = {74, 40, 41}; r.filter_type = types[(i + k) % 5]; r.filter_cutoff_hz = 24000.0f; r.filter_resonance_db = 40.0f; }
  }
  {
    auto& hi = t->region(mono, 90, 127); hi.filter_type = ports::SampleFilterType::kLpf2p; hi.filter_cutoff_hz = 19999.0f; hi.filter_resonance_db = 12.0f;
    auto& lo = t->region(stereo, 0, 39); lo.filter_type = ports::SampleFilterType::kHpf2p; lo.filter_cutoff_hz = 1.0f;
    auto& all = t->region(mono, 0, 127);                // plain (no filter, no crossfade)
    // Story 13.6: choke groups and polyphony limits on the busiest regions, so cuts, limit steals and the three off modes run inside the nonblocking scope too.
    all.group = 1; all.has_off_by = true; all.off_by = 2; all.off_mode = ports::SampleOffMode::kTime; all.off_time = 0.02f;
    all.group_polyphony = 24; all.note_polyphony = 3;
    lo.group = 2; lo.has_off_by = true; lo.off_by = 1; lo.off_mode = ports::SampleOffMode::kNormal; lo.polyphony = 4;
    hi.group = 1; hi.polyphony = 8; hi.group_polyphony = 24; hi.note_polyphony = 2; hi.note_selfmask = false;
  }
  {  // Story 13.7: pitch bend (smoothed, stepped, extreme ranges) and controller-started voices (CC 64 down / up with a polyphony-1 group, CC 20 over a range),
     // some of them also bending, so the wheel ramp and the controller start run inside the nonblocking scope too.
    auto& bend = t->region(mono, 0, 127);
    bend.bend_enabled = true; bend.bend_up = 1200.0f; bend.bend_down = -2400.0f; bend.bend_smooth_ms = 20; bend.volume_db = -12.0f;
    auto& step = t->region(stereo, 40, 80);
    step.bend_enabled = true; step.bend_up = 200.0f; step.bend_down = -200.0f; step.bend_step = 50.0f; step.volume_db = -12.0f;
    auto& down = t->region(t->add(300, false, 51 + k), 1, 1);   // key_center 1..3: a controller voice binds the Live Set generation to its key, so these stay clear of the played keys
    down.no_note = true; down.loop_mode = ports::SampleLoopMode::kNoLoop; down.group = 7; down.polyphony = 1; down.group_polyphony = 1;
    down.off_mode = ports::SampleOffMode::kTime; down.off_time = 0.05f; down.cc_trigger_count = 1; down.cc_triggers[0] = {64, 127.0f, 127.0f};
    auto& up = t->region(t->add(300, false, 61 + k), 2, 2);
    up.no_note = true; up.loop_mode = ports::SampleLoopMode::kNoLoop; up.group = 7; up.polyphony = 1; up.group_polyphony = 1;
    up.off_mode = ports::SampleOffMode::kTime; up.off_time = 0.05f; up.cc_trigger_count = 1; up.cc_triggers[0] = {64, 0.0f, std::nextafter(1.0f, 0.0f)};
    auto& range = t->region(t->add(300, false, 41 + k), 3, 3);
    range.no_note = true; range.loop_mode = ports::SampleLoopMode::kNoLoop; range.bend_enabled = true; range.bend_up = 700.0f; range.bend_smooth_ms = 5;
    range.cc_trigger_count = 2; range.cc_triggers[0] = {20, 30.0f, 100.0f}; range.cc_triggers[1] = {21, 0.0f, 127.0f};
    range.cc_gate_count = 1; range.cc_gates[0] = {74, 0.0f, 100.0f}; range.volume_db = -9.0f;
  }
  {  // Story 13.8: per-voice filters. Key track, velocity track, CC entries on cutoff and resonance (controllers 1, 11, 74, 131 velocity, 135 random, 140 key delta), a
     // second filter with its own tuning, near the clamps, on mono and stereo samples: the coefficients are computed in the nonblocking scope at every voice start.
    auto& a = t->region(mono, 20, 100);
    a.filter_type = ports::SampleFilterType::kLpf2p; a.filter_cutoff_hz = 800.0f; a.filter_resonance_db = 6.0f; a.volume_db = -12.0f;
    a.filter_tune.keytrack = 100.0f; a.filter_tune.veltrack = 2400.0f;
    a.filter_tune.cutoff[0] = {1, 0, 1200.0f}; a.filter_tune.cutoff[1] = {74, 0, -2400.0f}; a.filter_tune.cutoff[2] = {131, 0, 1200.0f}; a.filter_tune.cutoff[3] = {140, 0, 100.0f};
    a.filter_tune.cutoff_count = 4;
    a.filter_tune.res[0] = {11, 0, 24.0f}; a.filter_tune.res[1] = {135, 0, 12.0f}; a.filter_tune.res_count = 2;
    auto& b2 = t->region(stereo, 30, 110);
    b2.filter_type = ports::SampleFilterType::kHpf2p; b2.filter_cutoff_hz = 200.0f; b2.volume_db = -12.0f;
    b2.filter_tune.keytrack = 1200.0f; b2.filter_tune.keycenter = 0; b2.filter_tune.veltrack = 12000.0f;
    b2.filter2_type = ports::SampleFilterType::kLpf2p; b2.filter2_cutoff_hz = 18000.0f; b2.filter2_resonance_db = 40.0f;
    b2.filter2_tune.veltrack = -1200.0f; b2.filter2_tune.cutoff[0] = {1, 0, -12000.0f}; b2.filter2_tune.cutoff_count = 1;
    auto& c2 = t->region(mono, 0, 127);
    c2.filter_type = ports::SampleFilterType::kLpf1p; c2.filter_cutoff_hz = 1.0f; c2.volume_db = -12.0f;
    c2.filter_tune.cutoff[0] = {74, 0, 12000.0f}; c2.filter_tune.cutoff_count = 1;
    c2.filter2_type = ports::SampleFilterType::kBpf2p; c2.filter2_cutoff_hz = 20000.0f; c2.filter2_resonance_db = 40.0f;
  }
  {  // Story 13.9: region EQ and dedicated LFO / EG modulation: all three LFOs and both EGs (extreme depths and rates), a modulated cutoff, three EQ bands (entries on
     // controllers 1, 74 and velocity), on mono and stereo samples: the coefficients are computed in the nonblocking scope at every voice start and every 16 frames.
    auto& a = t->region(mono, 10, 110);
    a.volume_db = -12.0f; a.filter_type = ports::SampleFilterType::kLpf2p; a.filter_cutoff_hz = 900.0f; a.filter_resonance_db = 6.0f;
    a.mod.amp = {true, 1, 10.0f, 100.0f, 0.002f, 0.05f};
    a.mod.pitch = {true, 3, 1200.0f, 60.0f, 0.0f, 0.0f};
    a.mod.fil = {true, 0, 1200.0f, 30.0f, 0.0f, 0.0f};
    a.mod.pitch_eg = {true, 0.001f, 0.01f, 0.0f, 0.1f, 0.05f, 10.0f, 30.0f, 1200.0f, 600.0f};
    a.mod.fil_eg = {true, 0.0f, 0.02f, 0.01f, 0.2f, 0.1f, 0.0f, 40.0f, 12000.0f, -6000.0f};
    a.mod.eq_count = 3;
    a.mod.eq[0].type = 2; a.mod.eq[0].freq = 150.0f; a.mod.eq[0].gain = -12.0f; a.mod.eq[0].gain_count = 1; a.mod.eq[0].gain_mods[0] = {1, 0, 24.0f};
    a.mod.eq[1].freq = 1000.0f; a.mod.eq[1].bw = 0.01f; a.mod.eq[1].gain = 24.0f; a.mod.eq[1].vel2gain = -24.0f; a.mod.eq[1].freq_count = 1; a.mod.eq[1].freq_mods[0] = {74, 0, 4000.0f};
    a.mod.eq[2].type = 3; a.mod.eq[2].freq = 19999.0f; a.mod.eq[2].bw = 4.0f; a.mod.eq[2].gain = 60.0f; a.mod.eq[2].bw_count = 1; a.mod.eq[2].bw_mods[0] = {131, 0, -3.0f};
    auto& b3 = t->region(stereo, 20, 120);
    b3.volume_db = -12.0f;
    b3.mod.amp = {true, 7, -10.0f, 7.0f, 0.0f, 0.3f};
    b3.mod.pitch = {true, 1, -300.0f, 5.5f, 0.01f, 0.1f};
    b3.mod.pitch_eg = {true, 0.0f, 0.0f, 0.0f, 0.0f, 0.2f, 0.0f, 100.0f, -2400.0f, 0.0f};
    b3.mod.eq_count = 1; b3.mod.eq[0].freq = 1.0f; b3.mod.eq[0].gain = -96.0f; b3.mod.eq[0].bw = 0.01f;
    auto& c3 = t->region(mono, 0, 127);
    c3.volume_db = -12.0f; c3.filter_type = ports::SampleFilterType::kHpf1p; c3.filter_cutoff_hz = 20.0f;
    c3.mod.fil = {true, 2, -1200.0f, 100.0f, 0.0f, 0.0f};
    c3.mod.fil_eg = {true, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 12000.0f, 0.0f};  // sustain 0: it releases by itself
  }
  return t;
}

struct Rng {
  std::uint64_t s;
  std::uint32_t operator()() {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<std::uint32_t>(s >> 33);
  }
};

struct Control {
  SnapshotExchange& ex;
  BlockRenderer& br;
  const SampleEngine& sample;
  std::vector<std::shared_ptr<SampleBank>> banks;
  std::vector<std::unique_ptr<Snapshot>> live;
  Snapshot cur = Snapshot::defaults();
  std::uint64_t rev = 0, published = 0, epoch = 0;
  Rng rng{4242};

  Control(SnapshotExchange& e, BlockRenderer& b, const SampleEngine& s) : ex(e), br(b), sample(s) {
    for (std::uint32_t k = 0; k < kNumBanks; ++k) {
      auto ins = make_instrument(k);
      banks.push_back(sample.prepare(*ins, ins));
    }
    cur.live_set = true;
    for (std::uint32_t p = 0; p < 4; ++p) {
      cur.parts[p].engine_type = static_cast<std::uint32_t>(EngineTypeId::kSample);
      cur.parts[p].state = PartState::kReady;
      cur.parts[p].sample_bank = banks[p % kNumBanks];
    }
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
  void publish(std::uint32_t k, bool seamless) {
    for (std::uint32_t p = 0; p < 4; ++p) {
      PartCfg& c = cur.parts[p];
      const std::uint32_t r = rng();
      if ((k + p) % 3 == 0) c.sample_bank = banks[r % kNumBanks];      // another instrument (or the same one again)
      if ((k + p) % 5 == 0) c.keyboard_on = (r % 4 == 0) ? 0 : 1;
      if ((k + p) % 7 == 0) { c.low_key = static_cast<std::uint8_t>(r % 30); c.high_key = static_cast<std::uint8_t>(80 + r % 48); }
    }
    reclaim();
    std::uint32_t idx = 0;
    while (idx < SnapshotExchange::kLedgerSlots && used(idx)) ++idx;
    if (idx == SnapshotExchange::kLedgerSlots) return;
    auto s = std::make_unique<Snapshot>(cur);
    s->ledger_index = idx;
    s->revision = ++rev;
    if (seamless) {
      VoiceArbiter::LiveSetConfig c;
      c.budget = 1u << 20;
      c.waive_organ_reservation = true;
      c.waive_sample_floor = true;
      if (br.arbiter().prepare_reservation(c, s->reservation) == VoiceArbiter::ReserveStatus::kOk) s->reservation_epoch = ++epoch;
    }
    ex.publish(s.get());
    live.push_back(std::move(s));
    ++published;
  }
};

struct Player {
  static constexpr std::size_t kMax = 64;
  std::array<std::uint16_t, kMax> keys{};  // (channel << 7) | key: fixed storage, the run is nonblocking
  std::size_t count = 0;
  Rng rng{99};
  std::uint64_t switches = 0, ccs = 0;
};

// One block of traffic. `live` = keyboard channels are sources (any of 16); otherwise the channel is the part (0 or 1).
void run_block(BlockRenderer& br, Player& pl, float* l, float* r, std::uint32_t i, bool live) noexcept VELORA_NONBLOCKING {
  auto channel = [&]() { return live ? pl.rng() % 16 : pl.rng() % 2; };
  // Notes: pressing outpaces releasing up to ~40 keys down.
  const std::uint32_t presses = pl.count >= 40 ? 0 : 1 + pl.rng() % 2;
  std::uint16_t fresh[2];
  for (std::uint32_t n = 0; n < presses; ++n) {
    const std::uint32_t ch = channel(), key = pl.rng() % 5 == 0 ? 35 + pl.rng() % 5 : 24 + pl.rng() % 90;  // some are keyswitch keys
    if (key >= 35 && key <= 39) ++pl.switches;
    br.submit({make_note_on(0, ch, key, 2000 + pl.rng() % 60000), pl.rng() % 64});
    fresh[n] = static_cast<std::uint16_t>((ch << 7) | key);
  }
  const std::uint32_t releases = pl.rng() % 3;
  for (std::uint32_t n = 0; n < releases && pl.count > 0; ++n) {
    const std::size_t at = pl.rng() % pl.count;
    const std::uint16_t d = pl.keys[at];
    pl.keys[at] = pl.keys[--pl.count];
    br.submit({make_note_off(0, d >> 7, d & 127, 0), pl.rng() % 64});
  }
  for (std::uint32_t n = 0; n < presses; ++n) pl.keys[pl.count++] = fresh[n];
  // Controller sweeps (CC1 as a ramp, CC11 and CC74 at random), a few other controllers, whole steps and fractional MIDI 2.0 values.
  br.submit({make_control_change(0, channel(), 1, static_cast<std::uint32_t>(((i * 7u) % 128u) * 33554432u + (i % 3u) * 1000u)), pl.rng() % 64});
  ++pl.ccs;
  if (i % 3 == 0) { br.submit({make_control_change(0, channel(), 11, pl.rng() * 2u + (pl.rng() & 1u)), pl.rng() % 64}); ++pl.ccs; }
  if (i % 4 == 1) { br.submit({make_control_change(0, channel(), 74, pl.rng() * 2u), pl.rng() % 64}); ++pl.ccs; }
  if (i % 11 == 0) br.submit({make_control_change(0, channel(), pl.rng() % 128, pl.rng() * 2u), 10});
  // Story 13.7: the wheel every block (extremes and centre among random positions) and the controllers that start voices.
  {
    static constexpr std::uint32_t kWheel[] = {0u, 0x80000000u, 0xFFFFFFFFu};
    br.submit({make_pitch_bend(0, channel(), i % 4 == 0 ? kWheel[(i / 4) % 3] : pl.rng() * 2u + (pl.rng() & 1u)), pl.rng() % 64});
    if (i % 23 == 2) br.submit({make_control_change(0, channel(), 20, pl.rng() * 2u), pl.rng() % 64});
    if (i % 37 == 4) br.submit({make_control_change(0, channel(), 21, pl.rng() * 2u), pl.rng() % 64});
  }
  if (i % 29 == 0) br.submit({make_control_change(0, channel(), 64, (i / 29) % 2 ? 0xFFFFFFFFu : 0u), 20});  // the pedal
  if (i % 5 == 0)  // a registry-mapped controller (a parameter event) keeps addressing the part by its channel
    br.submit({make_param_event(*params::find(params::kDrawbar8), i % 2, static_cast<double>((i / 5) % 9)), 30});
  br.process_block(l, r);
}

bool run_once(bool live, std::uint64_t& hash_out) {
  auto br = std::make_unique<BlockRenderer>(8);
  SampleEngine::Config sc;
  sc.max_voices = 96;
  sc.extended_voices = true;
  sc.second_filter_voices = true;
  sc.modulated_voices = true;
  SampleEngine sample(sc);
  if (!br->bind_engine(sample)) {
    std::printf("FAIL: cannot bind the sample engine\n");
    return false;
  }
  std::unique_ptr<Control> ctl;
  std::vector<std::shared_ptr<SampleBank>> plain_banks;
  if (live) {
    VoiceArbiter::LiveSetConfig rc;
    rc.budget = 1u << 20;
    rc.waive_organ_reservation = true;
    rc.waive_sample_floor = true;
    if (br->arbiter().reserve(rc) != VoiceArbiter::ReserveStatus::kOk) {
      std::printf("FAIL: cannot reserve the voice pool\n");
      return false;
    }
    ctl = std::make_unique<Control>(br->snapshots(), *br, sample);
    ctl->publish(0, false);
  } else {
    for (std::uint32_t p = 0; p < 2; ++p) {
      auto ins = make_instrument(p);
      plain_banks.push_back(sample.prepare(*ins, ins));
      sample.set_part_sample_bank(p, plain_banks.back().get());
      br->set_channel_engine(p, static_cast<std::uint32_t>(EngineTypeId::kSample));
    }
  }
  Player pl;
  std::uint64_t hash = 1469598103934665603ull;
  bool finite = true;
  float peak = 0.0f;
  float l[kBlockFrames], r[kBlockFrames];
  std::uint32_t distinct = 0;
  std::array<bool, 64> seen_switch{};
  for (std::uint32_t i = 0; i < kBlocks; ++i) {
    if (ctl) {
      if (i % 5 == 0) ctl->publish(1 + i / 5, i % 95 == 0 && i > 0);
      if (i % 17 == 0) ctl->reclaim();
    }
    run_block(*br, pl, l, r, i, live);
    for (std::uint32_t f = 0; f < kBlockFrames; ++f) {
      for (float v : {l[f], r[f]}) {
        finite = finite && std::isfinite(v);
        peak = std::max(peak, std::fabs(v));
      }
      std::uint32_t bits[2];
      std::memcpy(&bits[0], &l[f], 4);
      std::memcpy(&bits[1], &r[f], 4);
      for (std::uint32_t b : bits) hash = (hash ^ b) * 1099511628211ull;
    }
    const int ks = sample.part_last_keyswitch(0);
    if (ks >= 0 && !seen_switch[static_cast<std::size_t>(ks) & 63]) {
      seen_switch[static_cast<std::size_t>(ks) & 63] = true;
      ++distinct;
    }
  }
  // Let every key go and the pedal up: nothing may hang.
  for (std::size_t k = 0; k < pl.count; ++k) br->submit({make_note_off(0, pl.keys[k] >> 7, pl.keys[k] & 127, 0), 0});
  for (std::uint32_t ch = 0; ch < 2; ++ch) br->submit({make_control_change(0, ch, 64, 0u), 1});  // plain: a pedal per part; Live Set: either lets go of all
  for (int i = 0; i < 3000; ++i) br->process_block(l, r);
  hash_out = hash;
  bool ok = true;
  auto need = [&](bool cond, const char* what) {
    if (!cond) {
      std::printf("FAIL (%s): %s\n", live ? "live set" : "plain", what);
      ok = false;
    }
  };
  need(finite, "non-finite output");
  need(peak > 0.01f, "output is (nearly) silent");
  need(sample.active_voices() == 0, "voices hang");
  need(pl.switches > 500 && pl.ccs > 10000, "traffic too light");
  need(distinct >= 3, "the keyswitch of part 0 did not move through at least 3 keys");
  need(sample.undeclared_extended_voices() == 0, "extended voices on an engine that declared them");
  need(sample.undeclared_second_filter_voices() == 0, "second-filter voices on an engine that declared them");
  need(sample.undeclared_modulated_voices() == 0, "modulated voices on an engine that declared them");
  need(br->ignored_events() < 500, "controllers or the wheel are being ignored");  // only the ones no engine reads (a part with another engine)
  if (live) need(ctl->published > 1000 && ctl->epoch >= 50, "too few generations / seamless switches");
  if (live) std::printf("  generations %llu, seamless switches %llu\n", (unsigned long long)ctl->published, (unsigned long long)ctl->epoch);
  return ok;
}

}  // namespace

int main() {
  for (bool live : {false, true}) {
    std::uint64_t a = 0, b = 0;
    if (!run_once(live, a) || !run_once(live, b)) return 1;
    if (a != b) {
      std::printf("FAIL: two runs differ (%016llx vs %016llx)\n", static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
      return 1;
    }
    std::printf("sample features traffic (%s): %u blocks, deterministic (%016llx)\n", live ? "live set" : "plain", kBlocks,
                static_cast<unsigned long long>(a));
  }
  return 0;
}
