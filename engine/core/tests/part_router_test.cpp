// Live Set part routing (Story 4.3): zones, layers, splits, octave / transpose, keyboard on/off, note-off consistency, sustain,
// silent parts (missing library, FM), sample + organ in one Live Set. Everything renders offline through the real BlockRenderer
// with hand-built sample banks (no formatlib, no files, no host): the Live Set loader's own tests are in engine/host.
//
// How a part is told apart in the output: its instrument is a constant (DC) loop at its own level, so the level of a rendered note is the
// sum of the levels of the parts that sound it (0.01 / 0.1 / 1 digits: "which parts played" is read off the level). Pitch is read
// off a sine instrument by counting zero crossings.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/part_router.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/core/snapshot.hpp"

using namespace velora::core;
namespace ports = velora::ports;

static int g_fail = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    if (!(c)) {                                                          \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
      ++g_fail;                                                          \
    }                                                                    \
  } while (0)
#define CHECK_NEAR(a, b, tol)                                                                                       \
  do {                                                                                                              \
    const double va_ = (a), vb_ = (b);                                                                              \
    if (!(std::fabs(va_ - vb_) <= (tol))) {                                                                         \
      std::printf("FAIL %s:%d: %s = %.9g, expected %.9g (+-%g)\n", __FILE__, __LINE__, #a, va_, vb_, (double)(tol)); \
      ++g_fail;                                                                                                     \
    }                                                                                                               \
  } while (0)

namespace {

struct TestInstrument final : ports::SampleSource {
  std::vector<ports::SampleRegionDesc> regions;
  std::vector<ports::SamplePcm> pcm;
  std::vector<std::vector<float>> data;
  ports::InstrumentView instrument() const noexcept override { return {regions, pcm}; }
  std::uint32_t add_sample(std::vector<float> left) {
    data.push_back(std::move(left));
    ports::SamplePcm p;
    p.sample_rate = 48000;
    p.channels = 1;
    p.frames = data.back().size();
    p.left = data.back().data();
    pcm.push_back(p);
    return static_cast<std::uint32_t>(pcm.size() - 1);
  }
  // A looping constant over the whole keyboard.
  static std::shared_ptr<TestInstrument> dc(float v) {
    auto t = std::make_shared<TestInstrument>();
    ports::SampleRegionDesc d;
    d.sample_index = t->add_sample(std::vector<float>(4800, v));
    d.key_lo = 0;
    d.key_hi = 127;
    d.key_center = 60;
    d.loop_mode = ports::SampleLoopMode::kLoopContinuous;
    t->regions.push_back(d);
    return t;
  }
  // An attack region (a looping constant) plus a release-trigger region (a one-shot constant of 4800 frames, ~100 ms).
  static std::shared_ptr<TestInstrument> with_release(float attack_level, float release_level) {
    auto t = dc(attack_level);
    ports::SampleRegionDesc r;
    r.sample_index = t->add_sample(std::vector<float>(4800, release_level));
    r.key_lo = 0;
    r.key_hi = 127;
    r.key_center = 60;
    r.loop_mode = ports::SampleLoopMode::kNoLoop;
    r.trigger = ports::SampleTrigger::kRelease;
    t->regions.push_back(r);
    return t;
  }
  // Two attack regions on every key: one note starts two voices.
  static std::shared_ptr<TestInstrument> two_layers(float level) {
    auto t = dc(level);
    t->regions.push_back(t->regions[0]);
    return t;
  }
  // A looping sine of 100 Hz at key 60 (480 frames a period).
  static std::shared_ptr<TestInstrument> sine() {
    auto t = std::make_shared<TestInstrument>();
    std::vector<float> x(4800);
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * static_cast<double>(i) / 480.0));
    ports::SampleRegionDesc d;
    d.sample_index = t->add_sample(std::move(x));
    d.key_lo = 0;
    d.key_hi = 127;
    d.key_center = 60;
    d.loop_mode = ports::SampleLoopMode::kLoopContinuous;
    t->regions.push_back(d);
    return t;
  }
};

constexpr std::uint32_t kVel = 65535;
constexpr std::uint32_t kTail = 80;  // blocks: the 6 ms release plus the 50 ms fade-out of a finished voice, with margin
// Mono centred sample at full velocity: pan law then sfizz's compensation (engine/core/README.md).
double unity_gain() { return std::cos(2047.0 * 1.57079632679489661923 / 4094.0) * 1.4125375446227544; }

struct Rig {
  std::unique_ptr<BlockRenderer> br = std::make_unique<BlockRenderer>(8);
  SampleEngine sample;
  std::unique_ptr<OrganEngine> organ;
  std::vector<std::unique_ptr<Snapshot>> gens;
  Snapshot cur = Snapshot::defaults();
  std::vector<std::vector<float>> blocks_l;  // every rendered block's left channel
  float l[kBlockFrames], r[kBlockFrames];

  static SampleEngine::Config cfg(std::uint32_t voices) {
    SampleEngine::Config c;
    c.max_voices = voices;
    return c;
  }
  explicit Rig(bool with_organ = false, std::uint32_t voices = 64) : sample(cfg(voices)) {
    CHECK(br->bind_engine(sample));
    if (with_organ) {
      organ = std::make_unique<OrganEngine>();
      CHECK(br->bind_engine(*organ));
    }
    cur.live_set = true;
  }
  // part p plays `ins` (sample engine) in zone [lo, hi].
  void sample_part(std::uint32_t p, const std::shared_ptr<TestInstrument>& ins, int lo = 0, int hi = 127, int octave = 0, int transpose = 0,
                   bool keyboard = true) {
    PartCfg& c = cur.parts[p];
    c.engine_type = static_cast<std::uint32_t>(EngineTypeId::kSample);
    c.state = PartState::kReady;
    c.low_key = static_cast<std::uint8_t>(lo);
    c.high_key = static_cast<std::uint8_t>(hi);
    c.octave = static_cast<std::int8_t>(octave);
    c.transpose = static_cast<std::int8_t>(transpose);
    c.keyboard_on = keyboard ? 1 : 0;
    c.sample_bank = sample.prepare(*ins, ins);
  }
  void publish() {
    auto s = std::make_unique<Snapshot>(cur);
    s->ledger_index = static_cast<std::uint32_t>(gens.size() % SnapshotExchange::kLedgerSlots);
    s->revision = gens.size() + 1;
    br->snapshots().publish(s.get());
    gens.push_back(std::move(s));  // never freed in a test: the ledger is not under test here
  }
  // Generation lifetime as the control thread manages it (Control::reclaim): generations are freed only when nobody can read them any more.
  std::vector<std::unique_ptr<Snapshot>> live;
  std::uint64_t rev = 0;
  void publish_live() {
    reclaim();
    std::uint32_t idx = 0;
    for (;; ++idx) {
      bool used = false;
      for (const auto& g : live) used = used || g->ledger_index == idx;
      if (!used) break;
    }
    auto g = std::make_unique<Snapshot>(cur);
    g->ledger_index = idx;
    g->revision = ++rev;
    br->snapshots().publish(g.get());
    live.push_back(std::move(g));
  }
  void reclaim() {
    for (std::size_t i = 0; i < live.size();) {
      if (live[i].get() != br->snapshots().current() && br->snapshots().safe_to_free(live[i].get())) live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
      else ++i;
    }
  }
  void run(std::uint32_t blocks) {
    for (std::uint32_t b = 0; b < blocks; ++b) {
      br->process_block(l, r);
      blocks_l.emplace_back(l, l + kBlockFrames);
    }
  }
  void note_on(std::uint32_t key, std::uint32_t ch = 0, std::uint32_t frame = 0) { CHECK(br->submit({make_note_on(0, ch, key, kVel), frame})); }
  void note_off(std::uint32_t key, std::uint32_t ch = 0, std::uint32_t frame = 0) { CHECK(br->submit({make_note_off(0, ch, key, 0), frame})); }
  void cc64(bool down, std::uint32_t ch = 0) { CHECK(br->submit({make_control_change(0, ch, 64, down ? 0xFFFFFFFFu : 0u), 0})); }
  // The mean of the last 32 frames of the last block, in instrument units.
  double level() const {
    double s = 0;
    const auto& b = blocks_l.back();
    for (std::uint32_t i = kBlockFrames - 32; i < kBlockFrames; ++i) s += b[i];
    return s / 32.0 / unity_gain();
  }
  // Plays `key` for 6 blocks, reads the level, releases it and lets the tail die.
  double play(std::uint32_t key) {
    note_on(key);
    run(6);
    const double v = level();
    note_off(key);
    run(kTail);
    return v;
  }
  // Zero crossings per second of the last `blocks` blocks (rising edges).
  double frequency(std::uint32_t blocks) const {
    std::vector<float> x;
    for (std::size_t i = blocks_l.size() - blocks; i < blocks_l.size(); ++i) x.insert(x.end(), blocks_l[i].begin(), blocks_l[i].end());
    std::uint32_t n = 0;
    for (std::size_t i = 1; i < x.size(); ++i)
      if (x[i - 1] < 0.0f && x[i] >= 0.0f) ++n;
    return static_cast<double>(n) * 48000.0 / static_cast<double>(x.size());
  }
};

// --- the router alone ------------------------------------------------------------------------------------------

void test_router_unit() {
  PartRouter router;
  Snapshot s = Snapshot::defaults();
  s.live_set = true;
  for (std::uint32_t p = 0; p < 3; ++p) {
    s.parts[p].engine_type = 1;
    s.parts[p].state = PartState::kReady;
  }
  s.parts[0].high_key = 47;                // bass
  s.parts[1].low_key = 48;                 // piano, an octave up and a whole tone down
  s.parts[1].octave = 1;
  s.parts[1].transpose = -2;
  s.parts[2].keyboard_on = 0;              // a part with the keyboard off
  router.configure(s);
  CHECK(router.active());
  PartRouter::Fanout f;
  router.note_on(0, 47, f);
  CHECK(f.count == 1 && f.t[0].part == 0 && f.t[0].pitch == 47);
  router.note_on(0, 48, f);
  CHECK(f.count == 1 && f.t[0].part == 1 && f.t[0].pitch == 48 + 12 - 2);  // octave / transpose move the pitch, not the zone
  router.note_off(0, 47, f);
  CHECK(f.count == 1 && f.t[0].part == 0 && f.t[0].pitch == 47);
  router.note_off(0, 47, f);  // already released: nothing to do
  CHECK(f.count == 0 && router.orphan_note_offs() == 1);
  // The zone changes while key 48 is held: the note off still goes to part 1 at pitch 58, not to the new zone.
  s.parts[1].low_key = 100;
  s.parts[1].transpose = 5;
  s.parts[0].high_key = 127;
  router.configure(s);
  router.note_off(0, 48, f);
  CHECK(f.count == 1 && f.t[0].part == 1 && f.t[0].pitch == 58);
  CHECK(router.held_keys() == 0);
  // Out of range drops the part's note and counts it.
  s.parts[1].low_key = 0;
  s.parts[1].octave = 4;
  s.parts[1].transpose = 12;
  router.configure(s);
  router.note_on(0, 100, f);
  CHECK(f.count == 1 && f.t[0].part == 0);
  CHECK(router.dropped_out_of_range() == 1);
  router.note_off(0, 100, f);
  // Different source channels are different keys.
  router.note_on(3, 60, f);
  router.note_on(4, 60, f);
  CHECK(router.held_keys() == 2);
  router.note_off(3, 60, f);
  CHECK(router.held_keys() == 1);
  router.note_off(4, 60, f);
  // Re-striking a held key after a transposition change releases the old pitch first.
  s.parts[1].octave = 0;
  s.parts[1].transpose = 0;
  router.configure(s);
  router.note_on(0, 60, f);
  s.parts[1].transpose = 2;
  router.configure(s);
  router.note_on(0, 60, f);
  bool saw = false;
  for (std::uint32_t i = 0; i < f.count; ++i)
    if (f.t[i].part == 1) saw = f.t[i].release_first && f.t[i].old_pitch == 60 && f.t[i].pitch == 62;
  CHECK(saw);
  router.note_off(0, 60, f);
  CHECK(router.held_keys() == 0);
  // Not a Live Set generation: idle.
  s.live_set = false;
  router.configure(s);
  CHECK(!router.active());
}

// --- layers, splits, zones -------------------------------------------------------------------------------------

void test_layer() {
  Rig rig;
  rig.sample_part(0, TestInstrument::dc(0.01f));  // "pad"
  rig.sample_part(1, TestInstrument::dc(0.1f));   // "piano"
  rig.publish();
  rig.run(2);
  CHECK_NEAR(rig.play(60), 0.11, 1e-4);   // overlapping full-range zones: both sound
  CHECK_NEAR(rig.play(21), 0.11, 1e-4);
  CHECK_NEAR(rig.play(108), 0.11, 1e-4);
  CHECK(rig.sample.active_voices() == 0);
  CHECK(rig.br->router().held_keys() == 0);
}

void test_split_b2_c3() {
  Rig rig;
  rig.sample_part(0, TestInstrument::dc(0.1f), 0, 47);     // bass up to B2 (MIDI 47)
  rig.sample_part(1, TestInstrument::dc(0.01f), 48, 127);  // piano from C3 (MIDI 48)
  rig.publish();
  rig.run(2);
  CHECK_NEAR(rig.play(47), 0.1, 1e-4);   // exactly B2: bass only
  CHECK_NEAR(rig.play(48), 0.01, 1e-4);  // exactly C3: piano only
  CHECK_NEAR(rig.play(46), 0.1, 1e-4);
  CHECK_NEAR(rig.play(49), 0.01, 1e-4);
  CHECK_NEAR(rig.play(0), 0.1, 1e-4);
  CHECK_NEAR(rig.play(127), 0.01, 1e-4);
  // Both at once: a chord across the split sounds both parts.
  rig.note_on(47);
  rig.note_on(48);
  rig.run(6);
  CHECK_NEAR(rig.level(), 0.11, 1e-4);
  rig.note_off(47);
  rig.run(kTail);
  CHECK_NEAR(rig.level(), 0.01, 1e-4);  // only the piano note is still down
  rig.note_off(48);
  rig.run(kTail);
  CHECK(rig.sample.active_voices() == 0);
}

void test_zone_uses_played_key() {
  Rig rig;
  auto sine = TestInstrument::sine();
  // Zone C3..B6, an octave and two semitones up: the zone test uses the key as played, the pitch is 14 semitones higher.
  rig.sample_part(0, sine, 48, 95, 1, 2);
  rig.publish();
  rig.run(2);
  rig.note_on(60);
  rig.run(220);
  CHECK_NEAR(rig.frequency(200), 100.0 * std::pow(2.0, 14.0 / 12.0), 5.0);  // pitch 74
  rig.note_off(60);
  rig.run(kTail);
  // Key 47 would sound at pitch 61 (inside the zone if the zone were tested on the pitch): it is outside, silent.
  rig.note_on(47);
  rig.run(10);
  CHECK(rig.sample.active_voices() == 0);
  rig.note_off(47);
  // Key 95 (the last in the zone) plays 14 semitones up even though that is beyond the zone's top.
  rig.note_on(95);
  rig.run(220);
  CHECK_NEAR(rig.frequency(200), 100.0 * std::pow(2.0, (95.0 + 14.0 - 60.0) / 12.0), 25.0);
  rig.note_off(95);
  rig.run(kTail);
  rig.note_on(96);  // one above the zone
  rig.run(10);
  CHECK(rig.sample.active_voices() == 0);  // 96 started nothing
  rig.note_off(96);
  rig.run(kTail);
  CHECK(rig.sample.active_voices() == 0);
}

void test_octave_transpose_pitch() {
  struct Case { int octave, transpose, key; double semis; };
  const Case cases[] = {{0, 0, 60, 0}, {-1, 0, 60, -12}, {2, 0, 60, 24}, {0, 7, 60, 7}, {1, -12, 72, 0}, {-2, 5, 65, -19}};
  for (const Case& c : cases) {
    Rig rig;
    rig.sample_part(0, TestInstrument::sine(), 0, 127, c.octave, c.transpose);
    rig.publish();
    rig.run(2);
    rig.note_on(static_cast<std::uint32_t>(c.key));
    rig.run(220);
    const double expect = 100.0 * std::pow(2.0, (c.key - 60 + c.semis) / 12.0);
    CHECK_NEAR(rig.frequency(200), expect, std::max(5.0, expect * 0.02));
  }
}

void test_out_of_range_dropped() {
  Rig rig;
  rig.sample_part(0, TestInstrument::dc(0.1f), 0, 127, 4, 12);   // +60 semitones
  rig.sample_part(1, TestInstrument::dc(0.01f), 0, 127, -4, 0);  // -48 semitones
  rig.publish();
  rig.run(2);
  // key 100: part 0 would be 160 (dropped), part 1 is 52 (plays)
  CHECK_NEAR(rig.play(100), 0.01, 1e-4);
  CHECK(rig.br->router().dropped_out_of_range() == 1);
  // key 20: part 0 is 80 (plays), part 1 is -28 (dropped)
  CHECK_NEAR(rig.play(20), 0.1, 1e-4);
  CHECK(rig.br->router().dropped_out_of_range() == 2);
  // key 60: part 0 is 120 (plays), part 1 is 12 (plays)
  CHECK_NEAR(rig.play(60), 0.11, 1e-4);
  CHECK(rig.br->router().dropped_out_of_range() == 2);
}

// --- keyboard on / off ------------------------------------------------------------------------------------------

void test_keyboard_off() {
  Rig rig;
  rig.sample_part(0, TestInstrument::dc(0.1f));
  rig.sample_part(1, TestInstrument::dc(0.01f), 0, 127, 0, 0, /*keyboard=*/false);
  rig.publish();
  rig.run(2);
  CHECK_NEAR(rig.play(60), 0.1, 1e-4);  // the part with the keyboard off receives no notes
  CHECK(rig.sample.active_voices() == 0);

  // A key is held on both parts; then part 1 goes off (a new generation) while the key is down.
  rig.cur.parts[1].keyboard_on = 1;
  rig.publish();
  rig.run(2);
  rig.note_on(64);
  rig.run(6);
  CHECK_NEAR(rig.level(), 0.11, 1e-4);
  rig.cur.parts[1].keyboard_on = 0;
  rig.publish();
  rig.run(4);
  CHECK_NEAR(rig.level(), 0.11, 1e-4);   // what it sounds keeps sounding...
  rig.note_on(65);                        // ...but new keys do not reach it
  rig.run(6);
  CHECK_NEAR(rig.level(), 0.21, 1e-4);   // 0.11 (key 64) + 0.1 (key 65, part 0 only)
  rig.note_off(64);
  rig.run(kTail);
  CHECK_NEAR(rig.level(), 0.1, 1e-4);    // ...and the held note of the off part is released by its own note off
  rig.note_off(65);
  rig.run(kTail);
  CHECK(rig.sample.active_voices() == 0);
  CHECK(rig.br->router().held_keys() == 0);
}

// --- the note off goes where the note on went -------------------------------------------------------------------

void test_note_off_follows_note_on() {
  Rig rig;
  rig.sample_part(0, TestInstrument::dc(0.1f), 0, 127, 0, 0);
  rig.sample_part(1, TestInstrument::dc(0.01f), 0, 127, 0, 0);
  rig.publish();
  rig.run(2);
  rig.note_on(60);
  rig.note_on(72);
  rig.run(6);
  // While both keys are down: transpose part 0, move part 1's zone away, silence nothing else.
  rig.cur.parts[0].transpose = 5;
  rig.cur.parts[0].octave = 1;
  rig.cur.parts[1].low_key = 100;
  rig.cur.parts[1].high_key = 110;
  rig.publish();
  rig.run(2);
  rig.note_off(60);
  rig.note_off(72);
  rig.run(kTail);
  CHECK(rig.sample.active_voices() == 0);   // nothing hangs
  CHECK(rig.br->router().held_keys() == 0);
  CHECK(std::fabs(rig.level()) < 1e-6);
  // A part switched to a different instrument while a key is down: the voice finishes on the old one.
  Rig r2;
  r2.sample_part(0, TestInstrument::dc(0.1f));
  r2.publish();
  r2.run(2);
  r2.note_on(60);
  r2.run(6);
  CHECK_NEAR(r2.level(), 0.1, 1e-4);
  r2.sample_part(0, TestInstrument::dc(0.5f));
  r2.publish();
  r2.run(4);
  CHECK_NEAR(r2.level(), 0.1, 1e-4);        // the sounding note stays on its instrument
  r2.note_off(60);
  r2.run(kTail);
  CHECK(r2.sample.active_voices() == 0);
  CHECK_NEAR(r2.play(61), 0.5, 1e-4);       // new notes use the new one
}

void test_retrigger_does_not_orphan() {
  Rig rig;
  rig.sample_part(0, TestInstrument::dc(0.1f), 0, 127, 0, 0);
  rig.publish();
  rig.run(2);
  rig.note_on(60);
  rig.run(2);
  rig.cur.parts[0].transpose = 3;  // the same key struck again after a transposition change
  rig.publish();
  rig.run(2);
  rig.note_on(60);
  rig.run(8);
  CHECK_NEAR(rig.level(), 0.1, 1e-4);   // the first note was let go, the second sounds
  rig.note_off(60);
  rig.run(kTail);
  CHECK(rig.sample.active_voices() == 0);
  CHECK(rig.br->router().held_keys() == 0);
}

// --- silent parts ----------------------------------------------------------------------------------------------

void test_silent_parts() {
  Rig rig;
  rig.sample_part(0, TestInstrument::dc(0.1f));
  // Part 1: the timbre did not resolve: silent, with its state; part 2: an FM part (reserved engine, none compiled in).
  rig.cur.parts[1].engine_type = static_cast<std::uint32_t>(EngineTypeId::kSample);
  rig.cur.parts[1].state = PartState::kMissingLibrary;
  rig.cur.parts[2].engine_type = static_cast<std::uint32_t>(EngineTypeId::kFm);
  rig.cur.parts[2].state = PartState::kEngineNotAvailable;
  rig.cur.parts[3].engine_type = static_cast<std::uint32_t>(EngineTypeId::kFm);  // would be asked to sound: the registry refuses, silently
  rig.cur.parts[3].state = PartState::kReady;
  rig.publish();
  rig.run(2);
  CHECK_NEAR(rig.play(60), 0.1, 1e-4);  // the others play, nothing is substituted
  CHECK(rig.br->status_count(EngineStatus::kEngineNotAvailable) >= 1);
  CHECK(rig.br->status_count(EngineStatus::kPoolExhausted) == 0);
  // With every part silent a key is simply not taken by anyone.
  Rig none;
  none.cur.parts[0].engine_type = static_cast<std::uint32_t>(EngineTypeId::kSample);
  none.cur.parts[0].state = PartState::kMissingLibrary;
  none.publish();
  none.run(2);
  none.note_on(60);
  none.run(10);
  CHECK(std::fabs(none.level()) < 1e-9);
  CHECK(none.br->router().unzoned_keys() == 1);
  none.note_off(60);
  none.run(2);
  CHECK(none.br->router().orphan_note_offs() == 1);
}

// --- sample and organ in one Live Set ---------------------------------------------------------------------------

void test_sample_and_organ() {
  Rig rig(/*with_organ=*/true);
  rig.sample_part(0, TestInstrument::dc(0.1f), 0, 127);
  rig.cur.parts[1].engine_type = static_cast<std::uint32_t>(EngineTypeId::kOrgan);  // an organ part: no timbre
  rig.cur.parts[1].state = PartState::kReady;
  rig.cur.parts[1].low_key = 36;
  rig.cur.parts[1].high_key = 96;
  rig.publish();
  rig.run(2);
  rig.note_on(60);
  rig.run(20);
  CHECK(rig.sample.active_voices() >= 1);
  CHECK(rig.organ->active_voices() >= 1);   // the organ part played the same key
  rig.note_on(30);                          // below the organ's zone
  rig.run(2);
  CHECK(rig.organ->active_voices() == 1);
  rig.note_off(60);
  rig.note_off(30);
  rig.run(200);
  CHECK(rig.sample.active_voices() == 0);
  CHECK(rig.organ->active_voices() == 0);
  CHECK(rig.br->router().held_keys() == 0);
}

// --- sustain pedal ---------------------------------------------------------------------------------------------

void test_sustain_fanout() {
  Rig rig;
  rig.sample_part(0, TestInstrument::dc(0.1f));
  rig.sample_part(1, TestInstrument::dc(0.01f), 0, 127, 0, 0, /*keyboard=*/false);
  rig.publish();
  rig.run(2);
  rig.cc64(true);
  rig.note_on(60);
  rig.run(4);
  rig.note_off(60);   // the pedal holds it
  rig.run(kTail);
  CHECK_NEAR(rig.level(), 0.1, 1e-4);
  rig.cc64(false);    // pedal up: released
  rig.run(kTail);
  CHECK(rig.sample.active_voices() == 0);
  CHECK(std::fabs(rig.level()) < 1e-6);
  // Pedal down on a different keyboard channel is the same pedal for the Live Set.
  rig.cc64(true, 5);
  rig.note_on(62, 5);
  rig.run(4);
  rig.note_off(62, 5);
  rig.run(kTail);
  CHECK_NEAR(rig.level(), 0.1, 1e-4);
  rig.cc64(false, 5);
  rig.run(kTail);
  CHECK(rig.sample.active_voices() == 0);
}

// A part that changes engine while the pedal still holds one of its notes does not leave that voice behind.
void test_pedal_across_engine_change() {
  Rig rig(/*with_organ=*/true);
  rig.sample_part(0, TestInstrument::dc(0.1f));
  rig.publish();
  rig.run(2);
  rig.cc64(true);
  rig.note_on(60);
  rig.run(4);
  rig.note_off(60);  // held by the pedal on the sample engine
  rig.run(4);
  CHECK(rig.sample.active_voices() == 1);
  rig.cur.parts[0].engine_type = static_cast<std::uint32_t>(EngineTypeId::kOrgan);  // the part becomes an organ
  rig.cur.parts[0].sample_bank = nullptr;
  rig.publish();
  rig.run(2);
  rig.note_on(60);   // the same (part, pitch) again, now on the organ
  rig.run(kTail);
  CHECK(rig.sample.active_voices() == 0);   // the pedal-held sample voice was let go, not orphaned
  CHECK(rig.organ->active_voices() >= 1);
  rig.note_off(60);
  rig.cc64(false);
  rig.run(kTail);
  CHECK(rig.organ->active_voices() == 0);
  CHECK(rig.br->router().held_keys() == 0);
}

// --- part volume ------------------------------------------------------------------------------------------------

void test_part_volume() {
  Rig rig;
  rig.sample_part(0, TestInstrument::dc(0.2f));
  rig.sample_part(1, TestInstrument::dc(0.2f));
  rig.cur.parts[0].volume = 0.5f;
  rig.cur.parts[1].volume = 0.0f;
  rig.publish();
  rig.run(2);
  CHECK_NEAR(rig.play(60), 0.1, 1e-4);   // 0.2 x 0.5 + 0.2 x 0
  rig.cur.parts[1].volume = 1.0f;
  rig.publish();
  rig.run(8);
  CHECK_NEAR(rig.play(60), 0.3, 1e-4);
}

// --- channels are keyboards, not parts, in a Live Set; plain addressing outside it ----------------------------------

void test_channel_conventions() {
  Rig rig;
  rig.sample_part(0, TestInstrument::dc(0.1f));
  rig.sample_part(1, TestInstrument::dc(0.01f));
  rig.publish();
  rig.run(2);
  // A note on channel 9 (a keyboard source, not a part) plays every part of the Live Set.
  rig.note_on(60, 9);
  rig.run(6);
  CHECK_NEAR(rig.level(), 0.11, 1e-4);
  rig.note_off(60, 9);
  rig.run(kTail);
  // Outside a Live Set generation the same renderer addresses parts by channel as before.
  Rig plain;
  plain.cur.live_set = false;
  plain.publish();
  plain.run(2);
  plain.br->set_channel_engine(3, static_cast<std::uint32_t>(EngineTypeId::kSample));
  CHECK(plain.br->router().active() == false);
}

// --- generation lifetime, leaving a Live Set, shared pitch, release triggers --------------------------------------------

// A part that switched to another instrument while its note is down: the sounding voice, and the release-trigger voice, keep their bank alive
// through the generation ledger; the instrument is freed only when the note and its release are finished.
void test_bank_lifetime() {
  Rig rig;
  std::weak_ptr<TestInstrument> watch;
  {
    auto ins = TestInstrument::dc(0.1f);
    watch = ins;
    rig.sample_part(0, ins);
  }
  rig.publish_live();
  rig.run(2);
  rig.note_on(60);
  rig.run(6);
  rig.sample_part(0, TestInstrument::dc(0.5f));  // the replacement: the old bank is now held only by the first generation
  rig.publish_live();
  rig.run(4);
  rig.reclaim();
  CHECK(!watch.expired());                    // the sounding voice still reads it
  CHECK_NEAR(rig.level(), 0.1, 1e-4);          // and keeps rendering
  rig.note_off(60);
  rig.run(kTail);
  rig.reclaim();
  CHECK(watch.expired());                     // after the note and its release: freed
  CHECK(rig.sample.active_voices() == 0);

  // The release-trigger voice started at note off: the bank outlives it.
  Rig rr;
  std::weak_ptr<TestInstrument> w2;
  {
    auto ins = TestInstrument::with_release(0.1f, 0.4f);
    w2 = ins;
    rr.sample_part(0, ins);
  }
  rr.publish_live();
  rr.run(2);
  rr.note_on(60);
  rr.run(6);
  rr.publish_live();  // a later generation that still carries the bank
  rr.run(2);
  rr.note_off(60);
  rr.run(2);
  CHECK(rr.sample.active_voices() == 2);       // the attack voice releasing and the release-trigger voice
  rr.sample_part(0, TestInstrument::dc(0.5f));
  rr.publish_live();
  rr.run(2);
  rr.reclaim();
  CHECK(!w2.expired());
  CHECK(rr.sample.active_voices() >= 1);
  rr.run(240);  // the one-shot release sample is 100 ms long, then its own fade-out
  rr.reclaim();
  CHECK(rr.sample.active_voices() == 0);
  CHECK(w2.expired());
}

// A note whose second region found no free voice: the voice that did start still holds the generation.
void test_partial_start_binds_generation() {
  Rig rig(false, /*voices=*/3);
  std::weak_ptr<TestInstrument> watch;
  {
    auto ins = TestInstrument::two_layers(0.1f);
    watch = ins;
    rig.sample_part(0, ins);
  }
  rig.publish_live();
  rig.run(2);
  rig.note_on(60);  // two voices
  rig.run(2);
  rig.note_on(64);  // one voice: the second region finds the pool exhausted
  rig.run(2);
  CHECK(rig.br->status_count(EngineStatus::kPoolExhausted) >= 1);
  CHECK(rig.sample.active_voices() == 3);
  rig.sample_part(0, TestInstrument::dc(0.5f));
  rig.publish_live();
  rig.run(2);
  rig.note_off(60);
  rig.run(kTail);
  rig.reclaim();
  CHECK(rig.sample.active_voices() == 1);      // key 64's lone voice
  CHECK(!watch.expired());                     // still referenced by key 64's generation
  rig.note_off(64);
  rig.run(kTail);
  rig.reclaim();
  CHECK(rig.sample.active_voices() == 0);
  CHECK(watch.expired());
}

// Leaving a Live Set generation for a plain one: held notes are released, the per-part banks are forgotten (they belong to generations that get
// freed) and the channels map to the engines they had before.
void test_leave_live_set() {
  Rig rig;
  auto fallback = TestInstrument::dc(0.3f);
  CHECK(rig.sample.set_instrument(*fallback));
  rig.br->set_channel_engine(0, static_cast<std::uint32_t>(EngineTypeId::kSample));  // plain addressing: channel 0 plays the sample engine
  std::weak_ptr<TestInstrument> watch;
  {
    auto ins = TestInstrument::dc(0.1f);
    watch = ins;
    rig.sample_part(0, ins);
  }
  rig.publish_live();
  rig.run(2);
  CHECK_NEAR(rig.play(60), 0.1, 1e-4);
  rig.note_on(62);   // still down when the Live Set generation goes away
  rig.run(6);
  CHECK(rig.br->router().held_keys() == 1);
  rig.cur = Snapshot::defaults();  // a plain generation: no banks, no Live Set
  rig.publish_live();
  rig.run(kTail);
  rig.reclaim();
  CHECK(rig.br->router().held_keys() == 0);
  CHECK(rig.sample.active_voices() == 0);      // the held note was released
  CHECK(watch.expired());                      // nothing points at the old bank any more
  CHECK_NEAR(rig.play(60), 0.3, 1e-4);         // channel 0 -> the sample engine's default instrument, not a stale bank
}

// A part rebound while the key is down does not start a release-trigger voice from the new bank.
void test_release_trigger_bank_mismatch() {
  auto scenario = [](bool rebind) {
    Rig rig;
    rig.sample_part(0, TestInstrument::with_release(0.1f, 0.4f));
    rig.publish_live();
    rig.run(2);
    rig.note_on(60);
    rig.run(6);
    if (rebind) {
      rig.sample_part(0, TestInstrument::with_release(0.2f, 0.7f));
      rig.publish_live();
      rig.run(2);
    }
    rig.note_off(60);
    rig.run(2);
    return rig.sample.active_voices();
  };
  CHECK(scenario(false) == 2);  // the attack voice releasing and its release-trigger voice
  CHECK(scenario(true) == 1);   // the new bank is not consulted for a note it did not start
}

// The engine id is kept whole (kTestTone = 0xF000 does not fit a byte).
void test_full_engine_id() {
  PartRouter router;
  Snapshot s = Snapshot::defaults();
  s.live_set = true;
  s.parts[0].engine_type = static_cast<std::uint32_t>(EngineTypeId::kTestTone);
  s.parts[0].state = PartState::kReady;
  router.configure(s);
  PartRouter::Fanout f;
  router.note_on(0, 60, f);
  CHECK(f.count == 1 && f.t[0].engine == 0xF000u);
  router.note_off(0, 60, f);
  CHECK(f.count == 1 && f.t[0].engine == 0xF000u);

  Rig rig;
  rig.cur.parts[0].engine_type = static_cast<std::uint32_t>(EngineTypeId::kTestTone);
  rig.cur.parts[0].state = PartState::kReady;
  rig.publish();
  rig.run(2);
  rig.note_on(60);
  rig.run(6);
  CHECK(rig.br->test_tone().active_voices() == 1);
  CHECK(rig.br->status_count(EngineStatus::kEngineUnknown) == 0 && rig.br->status_count(EngineStatus::kEngineNotAvailable) == 0);
  rig.note_off(60);
  rig.run(kTail);
  CHECK(rig.br->test_tone().active_voices() == 0);
}

// Two keys (or keyboard sources) on the same (part, pitch): the note off goes out with the last one, in either order.
void test_shared_pitch() {
  PartRouter router;
  Snapshot s = Snapshot::defaults();
  s.live_set = true;
  s.parts[0].engine_type = 1;
  s.parts[0].state = PartState::kReady;
  router.configure(s);
  PartRouter::Fanout f;
  for (int order = 0; order < 2; ++order) {
    router.note_on(0, 60, f);
    router.note_on(1, 60, f);                     // another keyboard source, the same key
    router.note_off(order ? 1 : 0, 60, f);
    CHECK(f.count == 0);                          // the other still holds it
    router.note_off(order ? 0 : 1, 60, f);
    CHECK(f.count == 1 && f.t[0].pitch == 60);
    CHECK(router.held_keys() == 0);
  }
  // Adjacent keys landing on one pitch after a transposition change.
  router.note_on(0, 60, f);                       // pitch 60
  s.parts[0].transpose = -2;
  router.configure(s);
  router.note_on(0, 62, f);                       // pitch 60 as well
  router.note_off(0, 60, f);
  CHECK(f.count == 0);
  router.note_off(0, 62, f);
  CHECK(f.count == 1 && f.t[0].pitch == 60);
  // A re-strike at another pitch lets the old pitch go only if no other key holds it.
  s.parts[0].transpose = 0;
  router.configure(s);
  router.note_on(0, 60, f);
  router.note_on(1, 60, f);
  s.parts[0].transpose = 5;
  router.configure(s);
  router.note_on(0, 60, f);                       // key (0,60): 60 -> 65, but (1,60) still holds 60
  CHECK(f.count == 1 && !f.t[0].release_first);
  router.note_on(1, 60, f);                       // now the last holder of 60 moves too
  CHECK(f.count == 1 && f.t[0].release_first && f.t[0].old_pitch == 60);

  // In the renderer: one key's release does not cut the note the other still holds.
  for (int order = 0; order < 2; ++order) {
    Rig rig;
    rig.sample_part(0, TestInstrument::dc(0.1f));
    rig.publish();
    rig.run(2);
    rig.note_on(60, 0);
    rig.note_on(60, 1);
    rig.run(6);
    rig.note_off(60, order ? 1 : 0);
    rig.run(kTail);
    CHECK_NEAR(rig.level(), 0.2, 1e-4);           // both started voices still sound (one voice per note on)
    rig.note_off(60, order ? 0 : 1);
    rig.run(kTail);
    CHECK(rig.sample.active_voices() == 0);
  }
}

// A drawbar value set while the part was a sample part reaches the organ when the part flips to the organ in a new generation.
void test_engine_remap_syncs_parameters() {
  auto scenario = [](int mode) {  // 0: value set before the flip, 1: after the flip, 2: never set (the organ defaults)
    Rig rig(/*with_organ=*/true);
    rig.sample_part(0, TestInstrument::dc(0.1f));
    rig.publish();
    rig.run(4);
    auto set = [&] {
      CHECK(rig.br->submit({make_param_event(*params::find(params::kDrawbar16), 0, 8.0), 0}));
      CHECK(rig.br->submit({make_param_event(*params::find(params::kDrawbar8), 0, 0.0), 0}));
    };
    if (mode == 0) set();
    rig.run(4);
    rig.cur.parts[0].engine_type = static_cast<std::uint32_t>(EngineTypeId::kOrgan);
    rig.cur.parts[0].sample_bank = nullptr;
    rig.publish();
    rig.run(4);
    if (mode == 1) set();
    rig.run(4);
    rig.note_on(60);
    rig.run(300);
    std::vector<float> sig;
    for (std::size_t b = rig.blocks_l.size() - 8; b < rig.blocks_l.size(); ++b) sig.insert(sig.end(), rig.blocks_l[b].begin(), rig.blocks_l[b].end());
    return sig;
  };
  const std::vector<float> before = scenario(0), after = scenario(1), defaults = scenario(2);
  double same = 0, differ = 0;
  for (std::size_t i = 0; i < before.size(); ++i) {
    same = std::max(same, static_cast<double>(std::fabs(before[i] - after[i])));
    differ = std::max(differ, static_cast<double>(std::fabs(before[i] - defaults[i])));
  }
  CHECK(same < 1e-4);    // the organ got the value that was set while the part was a sample part
  CHECK(differ > 1e-2);  // and that value matters (16' alone sounds unlike the default 8')
}

// The pedal holds a note on part A; a new generation turns A's keyboard off; the pedal up still releases it.
void test_pedal_up_after_keyboard_off() {
  Rig rig;
  rig.sample_part(0, TestInstrument::dc(0.1f));
  rig.publish();
  rig.run(2);
  rig.cc64(true);
  rig.note_on(60);
  rig.run(4);
  rig.note_off(60);   // deferred by the pedal
  rig.run(4);
  CHECK(rig.sample.active_voices() == 1);
  rig.cur.parts[0].keyboard_on = 0;
  rig.publish();
  rig.run(2);
  CHECK(rig.sample.active_voices() == 1);
  rig.cc64(false);
  rig.run(kTail);
  CHECK(rig.sample.active_voices() == 0);
}

}  // namespace


// Story 4.5: two keys on the same (part, pitch, engine) but on different banks (one struck under the old Live Set, one under the new) do not
// share a reference count: each key's release fires on its own, in either order.
void test_ref_across_banks() {
  for (int order = 0; order < 2; ++order) {
    PartRouter router;
    Snapshot s = Snapshot::defaults();
    s.live_set = true;
    s.parts[0].engine_type = 1;
    s.parts[0].state = PartState::kReady;
    s.parts[0].sample_bank = std::make_shared<SampleBank>();
    router.configure(s);
    PartRouter::Fanout f;
    router.note_on(0, 60, f);  // K1 on the old bank
    s.parts[0].sample_bank = std::make_shared<SampleBank>();
    router.configure(s);       // the new Live Set
    router.note_on(1, 60, f);  // K2, same part, pitch and engine, new bank
    CHECK(f.count == 1);
    router.note_off(order ? 1 : 0, 60, f);
    CHECK(f.count == 1 && f.t[0].pitch == 60);  // its own release fires at once
    router.note_off(order ? 0 : 1, 60, f);
    CHECK(f.count == 1 && f.t[0].pitch == 60);
    CHECK(router.held_keys() == 0);
  }
  // The same key struck again under the new bank releases the old-bank note first.
  PartRouter router;
  Snapshot s = Snapshot::defaults();
  s.live_set = true;
  s.parts[0].engine_type = 1;
  s.parts[0].state = PartState::kReady;
  s.parts[0].sample_bank = std::make_shared<SampleBank>();
  router.configure(s);
  PartRouter::Fanout f;
  router.note_on(0, 60, f);
  s.parts[0].sample_bank = std::make_shared<SampleBank>();
  router.configure(s);
  router.note_on(0, 60, f);
  CHECK(f.count == 1 && f.t[0].release_first && f.t[0].old_pitch == 60);
}

// --- pitch bend in a Live Set generation (Story 3.10) -------------------------------------------------------------

// Records pitch bend deliveries (part, value) as the block loop applies them.
class PitchSpy final : public Engine {
 public:
  EngineTypeId type_id() const noexcept override { return EngineTypeId::kFm; }
  std::uint32_t cost_units_per_voice() const noexcept override { return 1; }
  std::uint32_t voice_capacity() const noexcept override { return 1; }
  EngineStatus start_voice(std::uint32_t, std::uint32_t, std::uint32_t) noexcept override { return EngineStatus::kOk; }
  EngineStatus stop_voice(std::uint32_t, std::uint32_t) noexcept override { return EngineStatus::kVoiceNotFound; }
  void set_param(std::uint32_t, float) noexcept override {}
  void render(float*, float*, std::uint32_t) noexcept override {}
  void render_split(float* const*, float* const*, std::uint32_t) noexcept override {}
  bool voice_info(std::uint32_t, VoiceInfo&) const noexcept override { return false; }
  bool begin_steal_fade(std::uint32_t, std::uint32_t) noexcept override { return false; }
  bool set_part_pitch_bend(std::uint32_t part, std::uint32_t value32) noexcept override {
    log.emplace_back(part, value32);
    return true;
  }
  std::vector<std::pair<std::uint32_t, std::uint32_t>> log;
};

void test_pitch_bend_live_set() {
  auto br = std::make_unique<BlockRenderer>(8);
  PitchSpy spy;
  CHECK(br->bind_engine(spy));
  std::vector<std::unique_ptr<Snapshot>> gens;
  Snapshot cur = Snapshot::defaults();
  cur.live_set = true;
  for (std::uint32_t p = 0; p < 3; ++p) {
    cur.parts[p].engine_type = static_cast<std::uint32_t>(EngineTypeId::kFm);
    cur.parts[p].state = PartState::kReady;
  }
  cur.parts[1].keyboard_on = 0;  // part 1 does not sound with the keyboard
  auto publish = [&] {
    auto g = std::make_unique<Snapshot>(cur);
    g->ledger_index = static_cast<std::uint32_t>(gens.size() % SnapshotExchange::kLedgerSlots);
    g->revision = gens.size() + 1;
    br->snapshots().publish(g.get());
    gens.push_back(std::move(g));
  };
  float l[kBlockFrames], r[kBlockFrames];
  publish();
  br->process_block(l, r);
  CHECK(br->router().active());
  const std::uint32_t bend = 0xC0000000u;
  CHECK(br->submit({make_pitch_bend(0, 9, bend), 0}));  // the channel names the keyboard source, not a part
  br->process_block(l, r);
  CHECK(spy.log.size() == 2 && spy.log[0] == std::make_pair(0u, bend) && spy.log[1] == std::make_pair(2u, bend));  // only the keyboard-on parts
  CHECK(br->ignored_events() == 0);
  // part 1 joins the keyboard in the next generation: it takes the keyboard's last bend, like the controllers
  spy.log.clear();
  cur.parts[1].keyboard_on = 1;
  publish();
  br->process_block(l, r);
  bool got1 = false;
  for (const auto& e : spy.log) got1 = got1 || (e.first == 1 && e.second == bend);
  CHECK(got1);
  // no part sounds with the keyboard: the bend reaches nobody and is counted as ignored
  for (std::uint32_t p = 0; p < 3; ++p) cur.parts[p].keyboard_on = 0;
  publish();
  br->process_block(l, r);
  spy.log.clear();
  const std::uint64_t before = br->ignored_events();
  CHECK(br->submit({make_pitch_bend(0, 0, 0u), 0}));
  br->process_block(l, r);
  CHECK(spy.log.empty() && br->ignored_events() == before + 1);
}

int main() {
  test_router_unit();
  test_ref_across_banks();
  test_layer();
  test_split_b2_c3();
  test_zone_uses_played_key();
  test_octave_transpose_pitch();
  test_out_of_range_dropped();
  test_keyboard_off();
  test_note_off_follows_note_on();
  test_retrigger_does_not_orphan();
  test_silent_parts();
  test_sample_and_organ();
  test_sustain_fanout();
  test_pedal_across_engine_change();
  test_part_volume();
  test_channel_conventions();
  test_bank_lifetime();
  test_partial_start_binds_generation();
  test_leave_live_set();
  test_release_trigger_bank_mismatch();
  test_full_engine_id();
  test_shared_pitch();
  test_engine_remap_syncs_parameters();
  test_pedal_up_after_keyboard_off();
  test_pitch_bend_live_set();
  if (g_fail) {
    std::printf("part_router_tests: %d FAILED\n", g_fail);
    return 1;
  }
  std::printf("part_router_tests: ok\n");
  return 0;
}
