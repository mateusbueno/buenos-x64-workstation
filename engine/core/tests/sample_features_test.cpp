// Story 5.2: keyswitches, CC crossfades and basic filters in the sample engine. Unit tests with hand-built instruments (no formatlib, no
// files): the sfizz parity of the same behaviours is checked by the null tests in tools/offline-render (keyswitch_*, xfade_*, filters).
//
// How "which region played" is read: every region plays a looping constant at its own level, so the level of a rendered note is the sum
// of the levels of the regions that sounded it (distinct levels, so the sum names the set).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/core/sample_filter.hpp"
#include "velora/core/snapshot.hpp"
#include "velora/ports/sample_source.hpp"

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

constexpr double kPi = 3.14159265358979323846;
constexpr std::uint32_t kVel = 65535;

// Gain of a mono sample centred in the stereo field at full velocity (pan law -3 dB then sfizz's +3 dB compensation).
double unity() { return std::cos(2047.0 * 1.57079632679489661923 / 4094.0) * 1.4125375446227544; }

struct Instrument final : ports::SampleSource {
  std::vector<ports::SampleRegionDesc> regions;
  std::vector<ports::SamplePcm> pcm;
  std::vector<std::vector<float>> data;
  ports::InstrumentView instrument() const noexcept override { return {regions, pcm}; }
  std::uint32_t sample(std::vector<float> l, std::vector<float> r = {}) {
    const bool stereo = !r.empty();
    std::vector<float> buf = std::move(l);
    const std::size_t n = buf.size();
    if (stereo) buf.insert(buf.end(), r.begin(), r.end());
    data.push_back(std::move(buf));
    ports::SamplePcm p;
    p.channels = stereo ? 2 : 1;
    p.frames = n;
    p.left = data.back().data();
    p.right = stereo ? data.back().data() + n : nullptr;
    pcm.push_back(p);
    return static_cast<std::uint32_t>(pcm.size() - 1);
  }
  // A looping constant `level` on keys [lo, hi] (centre = lo, so the pitch ratio is 1 on `lo`; the constant has no pitch anyway).
  ports::SampleRegionDesc& dc(double level, int lo, int hi) {
    ports::SampleRegionDesc d;
    d.sample_index = sample(std::vector<float>(4800, static_cast<float>(level)));
    d.key_lo = static_cast<std::uint8_t>(lo);
    d.key_hi = static_cast<std::uint8_t>(hi);
    d.key_center = static_cast<std::uint8_t>(lo);
    d.loop_mode = ports::SampleLoopMode::kLoopContinuous;
    regions.push_back(d);
    return regions.back();
  }
  // A looping sine of `freq` Hz (exactly periodic over the 9600-frame sample for the frequencies of the tests) of amplitude 0.5.
  ports::SampleRegionDesc& sine(double freq, int key) {
    std::vector<float> x(9600);
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = 0.5f * static_cast<float>(std::sin(2.0 * kPi * freq * static_cast<double>(i) / 48000.0));
    ports::SampleRegionDesc d;
    d.sample_index = sample(std::move(x));
    d.key_lo = d.key_hi = d.key_center = static_cast<std::uint8_t>(key);
    d.loop_mode = ports::SampleLoopMode::kLoopContinuous;
    regions.push_back(d);
    return regions.back();
  }
};

SampleEngine::Config cfg(std::uint32_t voices = 32, bool extended = true) {
  SampleEngine::Config c;
  c.max_voices = voices;
  c.extended_voices = extended;
  return c;
}

struct Out {
  std::vector<float> l, r;
};
// Odd segment sizes on purpose: segmenting must not change the result.
Out render(SampleEngine& e, std::uint32_t frames, std::uint32_t seg = 37) {
  Out o;
  o.l.assign(frames, 0.0f);
  o.r.assign(frames, 0.0f);
  for (std::uint32_t done = 0; done < frames;) {
    const std::uint32_t n = std::min<std::uint32_t>(seg, frames - done);
    e.render(o.l.data() + done, o.r.data() + done, n);
    done += n;
  }
  return o;
}
void press(SampleEngine& e, int key, std::uint32_t ch = 0) { e.start_voice(ch, static_cast<std::uint32_t>(key), kVel); }
void lift(SampleEngine& e, int key, std::uint32_t ch = 0) {
  e.key_released(ch, static_cast<std::uint32_t>(key));
  e.stop_voice(ch, static_cast<std::uint32_t>(key));
}
void settle(SampleEngine& e) { render(e, 48000); }
// Plays `key` on `ch` and returns the level it settles at, in units of the DC levels; then releases it.
double play(SampleEngine& e, int key, std::uint32_t ch = 0) {
  press(e, key, ch);
  const Out o = render(e, 512);
  const double lvl = o.l.back() / unity();
  lift(e, key, ch);
  settle(e);
  return lvl;
}
std::uint32_t cc32(double cc_value) { return static_cast<std::uint32_t>(std::llround(cc_value / 127.0 * 4294967295.0)); }

// --- keyswitches ------------------------------------------------------------------------------------------------------------------

// sw_lokey/sw_hikey 36..39, sw_default 36, one articulation per switch on keys 48..84; unsorted on purpose (38 first).
std::unique_ptr<Instrument> articulations(bool with_default = true, bool map_switch_key = false) {
  auto ins = std::make_unique<Instrument>();
  auto add = [&](double level, int sw) {
    auto& r = ins->dc(level, 48, 84);
    r.sw.last = static_cast<std::int16_t>(sw);
    r.sw.lokey = 36;
    r.sw.hikey = 39;
    if (with_default) r.sw.default_key = 36;
  };
  add(0.04, 38);
  add(0.01, 36);
  add(0.02, 37);
  if (map_switch_key) {  // key 37 itself sounds (a region mapped to the keyswitch) and is the 37 articulation's switch
    auto& r = ins->dc(0.08, 37, 37);
    r.sw.last = 37;
  }
  return ins;
}

void test_sw_last_default_and_range() {
  auto ins = articulations();
  SampleEngine e(cfg());
  CHECK(e.set_instrument(*ins));
  CHECK(e.unplayable_regions() == 0);
  CHECK_NEAR(play(e, 60), 0.01, 1e-5);       // sw_default applies after load
  press(e, 37);                              // a keyswitch key makes no sound by itself
  CHECK(e.active_voices() == 0);
  lift(e, 37);
  CHECK_NEAR(play(e, 60), 0.02, 1e-5);       // region selection follows the active keyswitch
  CHECK_NEAR(play(e, 72), 0.02, 1e-5);       // ... for the whole key range
  press(e, 38); lift(e, 38);
  CHECK_NEAR(play(e, 60), 0.04, 1e-5);
  press(e, 39); lift(e, 39);                 // inside the range, no region uses 39 as sw_last: the switch still moves (nothing is active)
  CHECK(e.part_last_keyswitch(0) == 39);
  CHECK_NEAR(play(e, 60), 0.0, 1e-7);
  press(e, 36); lift(e, 36);
  CHECK_NEAR(play(e, 60), 0.01, 1e-5);
  press(e, 70); lift(e, 70);                 // a playing key outside the range changes nothing
  CHECK(e.part_last_keyswitch(0) == 36);
  press(e, 40); lift(e, 40);                 // outside sw_lokey..sw_hikey and nobody's sw_last: not a switch
  CHECK(e.part_last_keyswitch(0) == 36);
  press(e, 38); lift(e, 38);
  CHECK_NEAR(play(e, 60), 0.04, 1e-5);       // a held switch key is not needed: the last one pressed stays
}

void test_sw_without_default_and_mapped_switch_key() {
  {
    auto ins = articulations(/*with_default=*/false);
    SampleEngine e(cfg());
    e.set_instrument(*ins);
    CHECK(e.part_last_keyswitch(0) == -1);
    CHECK_NEAR(play(e, 60), 0.0, 1e-7);  // no sw_default: silent until the first keyswitch (sfizz)
    press(e, 37); lift(e, 37);
    CHECK_NEAR(play(e, 60), 0.02, 1e-5);
  }
  {
    auto ins = articulations(true, /*map_switch_key=*/true);  // key 37 has a region of its own with sw_last=37
    SampleEngine e(cfg());
    e.set_instrument(*ins);
    press(e, 37);  // the keyswitch is applied before the regions are looked at: the mapped region sounds on its own press
    CHECK(e.active_voices() == 1);
    const Out o = render(e, 512);
    CHECK_NEAR(o.l.back() / unity(), 0.08, 1e-5);
    lift(e, 37);
    settle(e);
    CHECK_NEAR(play(e, 60), 0.02, 1e-5);
  }
  {
    // sw_default naming a key that is nobody's sw_last: every sw_last region is off, a plain region is not affected.
    Instrument ins;
    auto& a = ins.dc(0.01, 48, 84); a.sw.last = 36; a.sw.default_key = 50;
    ins.dc(0.1, 48, 84);
    SampleEngine e(cfg());
    e.set_instrument(ins);
    CHECK_NEAR(play(e, 60), 0.1, 1e-5);
    press(e, 36); lift(e, 36);  // 36 is a switch key (a.sw.last): both regions
    CHECK_NEAR(play(e, 60), 0.11, 1e-5);
  }
}

void test_sw_down_up_previous() {
  Instrument ins;
  auto& up = ins.dc(0.01, 48, 84); up.sw.up = 36;
  auto& down = ins.dc(0.02, 48, 84); down.sw.down = 36;
  SampleEngine e(cfg());
  e.set_instrument(ins);
  CHECK_NEAR(play(e, 60), 0.01, 1e-5);  // 36 up: the sw_up region
  press(e, 36);                         // held
  CHECK(e.active_voices() == 0);
  CHECK_NEAR(play(e, 60), 0.02, 1e-5);
  CHECK_NEAR(play(e, 64), 0.02, 1e-5);
  lift(e, 36);
  CHECK_NEAR(play(e, 60), 0.01, 1e-5);
  // The physical release may come before the engine's stop_voice (sustain pedal): key_released alone is enough.
  press(e, 36);
  CHECK_NEAR(play(e, 60), 0.02, 1e-5);
  e.key_released(0, 36);
  CHECK_NEAR(play(e, 60), 0.01, 1e-5);
  e.stop_voice(0, 36);                  // the deferred stop does not change anything more
  CHECK_NEAR(play(e, 60), 0.01, 1e-5);
  // A re-struck key: down again.
  press(e, 36);
  press(e, 36);
  CHECK_NEAR(play(e, 60), 0.02, 1e-5);
  lift(e, 36);
  CHECK_NEAR(play(e, 60), 0.01, 1e-5);

  // sw_previous: the note on before this one.
  Instrument pv;
  pv.dc(0.01, 60, 72);
  auto& p = pv.dc(0.1, 62, 62); p.sw.previous = 60;
  SampleEngine f(cfg());
  f.set_instrument(pv);
  CHECK_NEAR(play(f, 62), 0.01, 1e-5);   // nothing before it
  press(f, 60); lift(f, 60);
  CHECK_NEAR(play(f, 62), 0.11, 1e-5);   // after 60
  press(f, 64); lift(f, 64);
  CHECK_NEAR(play(f, 62), 0.01, 1e-5);   // after 64
  CHECK_NEAR(play(f, 62), 0.01, 1e-5);   // after 62 itself
  press(f, 60); lift(f, 60);
  press(f, 62); lift(f, 62);
  CHECK_NEAR(play(f, 62), 0.01, 1e-5);   // after 62
}

void test_sw_combinations_and_gating() {
  // Several keyswitch opcodes on one region all have to hold.
  Instrument ins;
  auto& r = ins.dc(0.1, 48, 84);
  r.sw.last = 36;
  r.sw.down = 40;
  ins.dc(0.001, 48, 84);
  SampleEngine e(cfg());
  e.set_instrument(ins);
  CHECK_NEAR(play(e, 60), 0.001, 1e-6);
  press(e, 36); lift(e, 36);
  CHECK_NEAR(play(e, 60), 0.001, 1e-6);  // last ok, 40 not held
  press(e, 40);
  CHECK_NEAR(play(e, 60), 0.101, 1e-5);
  lift(e, 40);
  CHECK_NEAR(play(e, 60), 0.001, 1e-6);

  // Round robin counters advance whether or not a keyswitch lets the region play (sfizz: the counter is per key range hit).
  Instrument rr;
  auto& a = rr.dc(0.01, 60, 60); a.seq_length = 2; a.seq_position = 1; a.sw.last = 36; a.sw.default_key = 37;  // gated off at first
  auto& b = rr.dc(0.02, 60, 60); b.seq_length = 2; b.seq_position = 2; b.sw.last = 36;
  SampleEngine f(cfg());
  f.set_instrument(rr);
  CHECK_NEAR(play(f, 60), 0.0, 1e-7);  // counter 0 -> 1 (gated)
  CHECK_NEAR(play(f, 60), 0.0, 1e-7);  // counter 1 -> 2 (gated)
  press(f, 36); lift(f, 36);
  CHECK_NEAR(play(f, 60), 0.01, 1e-5);  // counter 2: position 1 plays
  CHECK_NEAR(play(f, 60), 0.02, 1e-5);

  // A release-trigger region is gated by the keyswitch state at the note off.
  Instrument rl;
  rl.dc(0.01, 60, 60);
  auto& rel = rl.dc(0.1, 60, 60);
  rel.trigger = ports::SampleTrigger::kRelease;
  rel.sw.down = 36;
  SampleEngine g(cfg());
  g.set_instrument(rl);
  press(g, 60);
  render(g, 64);
  lift(g, 60);
  CHECK(g.active_voices() == 1);  // the attack voice only (releasing)
  settle(g);
  press(g, 36);
  press(g, 60);
  render(g, 64);
  lift(g, 60);
  CHECK(g.active_voices() == 2);  // attack voice releasing + the release region (36 held)
}

void test_sw_state_per_part_and_resets() {
  auto ins = articulations();
  SampleEngine e(cfg());
  auto bank_a = e.prepare(*ins, nullptr);
  auto bank_b = e.prepare(*ins, nullptr);  // the same instrument twice: still two parts, two states
  CHECK(bank_a->uses_keyswitches && bank_a->default_switch == 36 && bank_a->switch_key[36] && bank_a->switch_key[39] && !bank_a->switch_key[40]);
  e.set_part_sample_bank(0, bank_a.get());
  e.set_part_sample_bank(1, bank_b.get());
  e.set_part_sample_bank(2, bank_a.get());  // the SAME bank on another part
  CHECK_NEAR(play(e, 60, 0), 0.01, 1e-5);
  press(e, 37, 0); lift(e, 37, 0);
  CHECK_NEAR(play(e, 60, 0), 0.02, 1e-5);
  CHECK_NEAR(play(e, 60, 1), 0.01, 1e-5);  // part 1 did not see the press
  CHECK_NEAR(play(e, 60, 2), 0.01, 1e-5);  // nor does the part that shares the bank
  press(e, 38, 1); lift(e, 38, 1);
  CHECK_NEAR(play(e, 60, 1), 0.04, 1e-5);
  CHECK_NEAR(play(e, 60, 0), 0.02, 1e-5);
  CHECK(e.part_last_keyswitch(0) == 37 && e.part_last_keyswitch(1) == 38 && e.part_last_keyswitch(2) == 36);
  // Binding the same bank again (the block loop does it at every generation) keeps the state.
  e.set_part_sample_bank(0, bank_a.get());
  CHECK(e.part_last_keyswitch(0) == 37);
  // A different instrument (rebind) starts from its own sw_default; so does the part that is bound to nothing.
  e.set_part_sample_bank(0, bank_b.get());
  CHECK(e.part_last_keyswitch(0) == 36);
  press(e, 37, 0); lift(e, 37, 0);
  CHECK(e.part_last_keyswitch(0) == 37);
  e.set_part_sample_bank(0, nullptr);
  CHECK(e.part_last_keyswitch(0) == -1);
  CHECK_NEAR(play(e, 60, 0), 0.0, 1e-7);  // bound to nothing: the part is silent
  // A Live Set switch (reset_play_state) returns every part to its sw_default and keeps the controllers.
  e.set_part_sample_bank(0, bank_a.get());
  press(e, 38, 0); lift(e, 38, 0);
  press(e, 37, 1); lift(e, 37, 1);
  e.set_part_cc(0, 1, cc32(50));
  e.reset_play_state();
  CHECK(e.part_last_keyswitch(0) == 36 && e.part_last_keyswitch(1) == 36);
  CHECK_NEAR(e.part_cc(0, 1), 50.0, 1e-6);
  // Held keys are forgotten with the state.
  Instrument hold;
  auto& d = hold.dc(0.1, 48, 84); d.sw.down = 36;
  SampleEngine h(cfg());
  h.set_instrument(hold);
  press(h, 36);
  CHECK_NEAR(play(h, 60), 0.1, 1e-5);
  h.reset_play_state();
  CHECK_NEAR(play(h, 60), 0.0, 1e-7);
  // set_instrument / clear_instrument start from the new default.
  e.set_instrument(*ins);
  CHECK(e.part_last_keyswitch(5) == 36);
  e.clear_instrument();
  CHECK(e.part_last_keyswitch(5) == -1);
}

// --- CC crossfades -----------------------------------------------------------------------------------------------------------------

void test_crossfade_curves_at_anchors() {
  using C = ports::SampleXfCurve;
  // xfin 20..60: 0 below 20, linear in gain to 1 at 60 (the usable length is hi - lo), sqrt in power.
  CHECK(sample_xf_in_gain(20, 60, 19, C::kGain) == 0.0f && sample_xf_in_gain(20, 60, 0, C::kPower) == 0.0f);
  CHECK(sample_xf_in_gain(20, 60, 20, C::kGain) == 0.0f && sample_xf_in_gain(20, 60, 20, C::kPower) == 0.0f);
  CHECK_NEAR(sample_xf_in_gain(20, 60, 40, C::kGain), 0.5, 1e-7);
  CHECK_NEAR(sample_xf_in_gain(20, 60, 40, C::kPower), std::sqrt(0.5), 1e-7);
  CHECK_NEAR(sample_xf_in_gain(20, 60, 30, C::kGain), 0.25, 1e-7);
  CHECK_NEAR(sample_xf_in_gain(20, 60, 59, C::kGain), 39.0 / 40.0, 1e-7);
  CHECK(sample_xf_in_gain(20, 60, 60, C::kGain) == 1.0f && sample_xf_in_gain(20, 60, 127, C::kPower) == 1.0f);
  // xfout 70..120: 1 up to 70, then falling to 0 at 120 (gain: 1 - t, power: sqrt(1 - t)).
  CHECK(sample_xf_out_gain(70, 120, 0, C::kGain) == 1.0f && sample_xf_out_gain(70, 120, 70, C::kPower) == 1.0f);
  CHECK_NEAR(sample_xf_out_gain(70, 120, 95, C::kGain), 0.5, 1e-7);
  CHECK_NEAR(sample_xf_out_gain(70, 120, 95, C::kPower), std::sqrt(0.5), 1e-7);
  CHECK_NEAR(sample_xf_out_gain(70, 120, 71, C::kGain), 1.0 - 1.0 / 50.0, 1e-7);
  CHECK(sample_xf_out_gain(70, 120, 120, C::kGain) == 0.0f && sample_xf_out_gain(70, 120, 127, C::kPower) == 0.0f);
  // Equal-power pair: sqrt(t) in and sqrt(1 - t) out add up in power; the gain pair adds up in amplitude.
  for (int x = 20; x <= 60; ++x) {
    const double in = sample_xf_in_gain(20, 60, x, C::kPower), out = sample_xf_out_gain(20, 60, x, C::kPower);
    CHECK_NEAR(in * in + out * out, 1.0, 1e-6);
    CHECK_NEAR(static_cast<double>(sample_xf_in_gain(20, 60, x, C::kGain)) + sample_xf_out_gain(20, 60, x, C::kGain), 1.0, 1e-6);
  }
  // hi = 127 does not fill the gap (sfizz): the usable length is 126 - lo, so the ramp ends one step early.
  CHECK_NEAR(sample_xf_in_gain(100, 127, 113, C::kGain), 13.0 / 26.0, 1e-7);
  CHECK(sample_xf_in_gain(100, 127, 126, C::kGain) == 1.0f);
  CHECK_NEAR(sample_xf_out_gain(100, 127, 113, C::kGain), 0.5, 1e-7);
  CHECK(sample_xf_out_gain(100, 127, 127, C::kGain) == 0.0f);
  // A range one step wide (or empty) has no usable length: xfin is a step at lo, xfout never fades (sfizz).
  CHECK(sample_xf_in_gain(50, 50, 49, C::kPower) == 0.0f && sample_xf_in_gain(50, 50, 50, C::kPower) == 1.0f);
  CHECK(sample_xf_out_gain(50, 50, 0, C::kGain) == 1.0f && sample_xf_out_gain(50, 50, 127, C::kGain) == 1.0f);
  // Monotone over the whole controller range, both curves.
  for (C c : {C::kGain, C::kPower}) {
    float pi = 0.0f, po = 1.0f;
    for (int x = 0; x <= 127; ++x) {
      const float i = sample_xf_in_gain(10, 100, x, c), o = sample_xf_out_gain(10, 100, x, c);
      CHECK(i >= pi && o <= po && i >= 0.0f && i <= 1.0f && o >= 0.0f && o <= 1.0f);
      pi = i;
      po = o;
    }
  }
  // A fractional controller value (MIDI 2.0 resolution) between hi and hi + 1 is clamped, not above 1.
  CHECK(sample_xf_in_gain(20, 60, 60.5, C::kPower) == 1.0f);
}

// Two regions share key 60: xfout on CC1 0..100 (level 0.1) and xfin on CC1 30..127 (level 0.2).
std::unique_ptr<Instrument> crossfader(ports::SampleXfCurve curve) {
  auto ins = std::make_unique<Instrument>();
  auto& a = ins->dc(0.1, 60, 60);
  a.xfout_count = 1; a.xfout[0] = {1, 0, 100}; a.xf_curve = curve;
  auto& b = ins->dc(0.2, 60, 60);
  b.xfin_count = 1; b.xfin[0] = {1, 30, 127}; b.xf_curve = curve;
  return ins;
}

void test_crossfade_follows_controller() {
  for (auto curve : {ports::SampleXfCurve::kGain, ports::SampleXfCurve::kPower}) {
    auto ins = crossfader(curve);
    SampleEngine e(cfg());
    e.set_instrument(*ins);
    auto expect = [&](double cc) {
      const double go = sample_xf_out_gain(0, 100, cc, curve), gi = sample_xf_in_gain(30, 127, cc, curve);
      return 0.1 * go + 0.2 * gi;
    };
    for (double cc : {0.0, 20.0, 30.0, 50.0, 64.0, 99.0, 100.0, 126.0, 127.0, 64.0, 0.0}) {
      e.set_part_cc(0, 1, cc32(cc));
      CHECK_NEAR(play(e, 60), expect(cc), 2e-6);
    }
  }
  // Defaults: the part's CcState starts at sfizz's defaults (CC7 100, CC10 63.5, CC11 127, the rest 0).
  SampleEngine e(cfg());
  CHECK(e.part_cc(3, 7) == 100.0f && e.part_cc(3, 10) == 63.5f && e.part_cc(3, 11) == 127.0f && e.part_cc(3, 1) == 0.0f);
  CHECK(!e.set_part_cc(0, 128, 0) && e.set_part_cc(0, 127, 0xFFFFFFFFu) && e.part_cc(0, 127) == 127.0f);
  // A 7-bit MIDI 1.0 value arrives bit-repeated to 32 bits and lands on the whole step.
  const std::uint32_t v = 0x2Au;
  e.set_part_cc(0, 1, (v << 25) | (v << 18) | (v << 11) | (v << 4) | (v >> 3));
  CHECK(e.part_cc(0, 1) == 42.0f);
}

void test_crossfade_sounding_voices_follow_without_steps() {
  auto ins = crossfader(ports::SampleXfCurve::kGain);
  SampleEngine e(cfg());
  e.set_instrument(*ins);
  e.set_part_cc(0, 1, cc32(0));  // only the xfout region is up (0.1)
  press(e, 60);
  Out o = render(e, 2000);
  CHECK_NEAR(o.l.back() / unity(), 0.1, 1e-6);
  // The wheel jumps to 127: the xfout region goes to 0 and the xfin region to 1; the voices already sounding follow with a ramp, not a step.
  e.set_part_cc(0, 1, cc32(127));
  o = render(e, 600);
  double max_step = 0.0;
  std::int32_t settled_at = -1;
  for (std::size_t i = 1; i < o.l.size(); ++i) {
    max_step = std::max(max_step, std::fabs(static_cast<double>(o.l[i]) - o.l[i - 1]) / unity());
    if (settled_at < 0 && std::fabs(o.l[i] / unity() - 0.2) < 1e-6) settled_at = static_cast<std::int32_t>(i);
  }
  CHECK_NEAR(o.l.back() / unity(), 0.2, 1e-6);
  CHECK(settled_at > 0 && settled_at <= 242);  // the 5 ms (240 frame) ramp of sfizz's smoother
  CHECK(max_step <= 0.1 / 240.0 * 2.0 + 1e-6);  // each region moves by at most 0.1 / 240 or 0.2 / 240 per frame (they move in opposite directions)
  // No zipper noise on a sweep: the controller moves 128 whole steps, 12 ms apart, and the output keeps changing in small steps only.
  e.set_part_cc(0, 1, cc32(0));
  render(e, 2000);
  double prev = 0.0, worst = 0.0;
  bool first = true;
  for (int cc = 0; cc <= 127; ++cc) {
    e.set_part_cc(0, 1, cc32(cc));
    const Out s = render(e, 576);
    for (std::size_t i = 0; i < s.l.size(); ++i) {
      const double x = s.l[i] / unity();
      if (!first) worst = std::max(worst, std::fabs(x - prev));
      prev = x;
      first = false;
    }
  }
  CHECK(worst < 0.0035);  // a step of the raw gain would be ~0.1 * (1 / 100 .. 1 step) in one frame; the ramp spreads it over 240 frames
  // The ramp restarts on a new target: a second jump mid-ramp keeps going from where it is (no jump back to the old value).
  e.set_part_cc(0, 1, cc32(0));
  render(e, 2000);
  e.set_part_cc(0, 1, cc32(127));
  render(e, 100);
  const double mid = render(e, 1).l[0] / unity();
  e.set_part_cc(0, 1, cc32(0));
  const Out back = render(e, 400);
  CHECK(mid > 0.1 && mid < 0.2 + 1e-6);
  CHECK(std::fabs(back.l.front() / unity() - mid) < 0.01);
  CHECK_NEAR(back.l.back() / unity(), 0.1, 1e-6);
}

void test_crossfade_new_voice_starts_at_the_current_gain() {
  auto ins = crossfader(ports::SampleXfCurve::kPower);
  SampleEngine e(cfg());
  e.set_instrument(*ins);
  e.set_part_cc(0, 1, cc32(70));
  press(e, 60);
  const Out o = render(e, 8);
  const double want = 0.1 * sample_xf_out_gain(0, 100, 70, ports::SampleXfCurve::kPower) + 0.2 * sample_xf_in_gain(30, 127, 70, ports::SampleXfCurve::kPower);
  CHECK_NEAR(o.l[0] / unity(), want, 2e-6);  // no ramp from silence at the note on (sfizz resetCrossfades)
}

void test_crossfade_two_controllers_and_isolation() {
  Instrument ins;
  auto& r = ins.dc(1.0, 60, 60);  // xfin CC1 20..60 times xfout CC11 70..120
  r.xfin_count = 1; r.xfin[0] = {1, 20, 60};
  r.xfout_count = 1; r.xfout[0] = {11, 70, 120};
  r.xf_curve = ports::SampleXfCurve::kGain;
  SampleEngine e(cfg());
  e.set_instrument(ins);
  e.set_part_cc(0, 1, cc32(40));
  e.set_part_cc(0, 11, cc32(95));
  CHECK_NEAR(play(e, 60, 0), 0.5 * 0.5, 2e-6);   // product of the two gains
  CHECK_NEAR(play(e, 60, 1), 0.0, 1e-7);         // part 1 has CC1 = 0 and CC11 = 127 (its own CcState): silent
  e.set_part_cc(1, 1, cc32(60));
  e.set_part_cc(1, 11, cc32(70));
  CHECK_NEAR(play(e, 60, 1), 1.0, 2e-6);
  CHECK_NEAR(play(e, 60, 0), 0.25, 2e-6);        // part 0 unchanged
  // Two xfade controllers on the same direction multiply as well.
  Instrument two;
  auto& t = two.dc(1.0, 60, 60);
  t.xfin_count = 2; t.xfin[0] = {1, 0, 127}; t.xfin[1] = {2, 0, 127};
  t.xf_curve = ports::SampleXfCurve::kGain;
  SampleEngine f(cfg());
  f.set_instrument(two);
  f.set_part_cc(0, 1, cc32(63));
  f.set_part_cc(0, 2, cc32(63));
  CHECK_NEAR(play(f, 60), (63.0 / 126.0) * (63.0 / 126.0), 2e-6);
}

void test_segmentation_does_not_change_the_render() {
  auto ins = crossfader(ports::SampleXfCurve::kPower);
  auto run = [&](std::uint32_t seg) {
    SampleEngine e(cfg());
    e.set_instrument(*ins);
    std::vector<float> all;
    press(e, 60);
    const std::uint32_t events[] = {100, 500, 1300, 2900};
    const double values[] = {127, 10, 64, 33};
    std::uint32_t at = 0;
    for (std::size_t k = 0; k <= 4; ++k) {
      const std::uint32_t until = k < 4 ? events[k] : 4000;
      while (at < until) {
        const std::uint32_t n = std::min(seg, until - at);
        const Out o = render(e, n, n);
        all.insert(all.end(), o.l.begin(), o.l.end());
        at += n;
      }
      if (k < 4) e.set_part_cc(0, 1, cc32(values[k]));
    }
    return all;
  };
  CHECK(run(4000) == run(64) && run(64) == run(7) && run(7) == run(1));
}

// --- filters -----------------------------------------------------------------------------------------------------------------------

// The textbook RBJ cookbook magnitudes, written out independently of the engine's coefficient code.
double rbj_mag(ports::SampleFilterType t, double fc, double res_db, double f) {
  const double q = std::pow(10.0, res_db / 20.0);
  const double w0 = 2.0 * kPi * fc / 48000.0, w = 2.0 * kPi * f / 48000.0;
  const double al = std::sin(w0) / (2.0 * q), cw = std::cos(w0);
  double b0, b1, b2;
  if (t == ports::SampleFilterType::kLpf2p) { b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = b0; }
  else if (t == ports::SampleFilterType::kHpf2p) { b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = b0; }
  else { b0 = al; b1 = 0; b2 = -al; }
  const double a0 = 1 + al, a1 = -2 * cw, a2 = 1 - al;
  const double nr = b0 + b1 * std::cos(w) + b2 * std::cos(2 * w), ni = -(b1 * std::sin(w) + b2 * std::sin(2 * w));
  const double dr = a0 + a1 * std::cos(w) + a2 * std::cos(2 * w), di = -(a1 * std::sin(w) + a2 * std::sin(2 * w));
  return std::sqrt(nr * nr + ni * ni) / std::sqrt(dr * dr + di * di);
}

void test_filter_response_at_anchors() {
  using T = ports::SampleFilterType;
  const double fs = 48000.0;
  // Analytic anchors of the 2-pole filters: gain Q at the cutoff for the low- and high-pass, 1 for the band-pass; DC and Nyquist limits.
  for (double fc : {200.0, 1000.0, 5000.0, 12000.0, 20000.0}) {
    for (double res : {0.0, 6.0, 20.0, -3.0}) {
      const double q = std::pow(10.0, res / 20.0);
      const auto lp = make_sample_filter(T::kLpf2p, fc, res, fs), hp = make_sample_filter(T::kHpf2p, fc, res, fs), bp = make_sample_filter(T::kBpf2p, fc, res, fs);
      CHECK_NEAR(sample_filter_magnitude(lp, fc, fs), q, 1e-9 * q);
      CHECK_NEAR(sample_filter_magnitude(hp, fc, fs), q, 1e-9 * q);
      CHECK_NEAR(sample_filter_magnitude(bp, fc, fs), 1.0, 1e-9);
      CHECK_NEAR(sample_filter_magnitude(lp, 1e-3, fs), 1.0, 1e-6);
      CHECK_NEAR(sample_filter_magnitude(hp, 0.5 * fs - 1e-3, fs), 1.0, 1e-6);
      CHECK(sample_filter_magnitude(lp, 0.5 * fs, fs) < 1e-6 * q + 1e-3);
      CHECK(sample_filter_magnitude(hp, 1e-3, fs) < 1e-6);
      CHECK(sample_filter_magnitude(bp, 1e-3, fs) < 1e-3 && sample_filter_magnitude(bp, 0.5 * fs, fs) < 1e-3);
      for (double mult : {0.125, 0.25, 0.5, 0.9, 1.1, 2.0, 4.0}) {  // the textbook formula at other frequencies
        const double f = fc * mult;
        if (f >= 0.5 * fs) continue;
        CHECK_NEAR(sample_filter_magnitude(lp, f, fs), rbj_mag(T::kLpf2p, fc, res, f), 1e-9 * std::max(1.0, q));
        CHECK_NEAR(sample_filter_magnitude(hp, f, fs), rbj_mag(T::kHpf2p, fc, res, f), 1e-9 * std::max(1.0, q));
        CHECK_NEAR(sample_filter_magnitude(bp, f, fs), rbj_mag(T::kBpf2p, fc, res, f), 1e-9);
      }
    }
  }
  // One pole: unity at DC (low-pass) / Nyquist (high-pass), the matched-pole filter is -3 dB at the cutoff to within 0.2 dB while
  // fc << fs, falls 6 dB per octave well above (low-pass) / below (high-pass).
  for (double fc : {50.0, 100.0, 200.0, 1000.0, 2000.0}) {
    const auto lp = make_sample_filter(T::kLpf1p, fc, 0.0, fs), hp = make_sample_filter(T::kHpf1p, fc, 0.0, fs);
    CHECK_NEAR(sample_filter_magnitude(lp, 1e-3, fs), 1.0, 1e-6);
    CHECK_NEAR(sample_filter_magnitude(hp, 0.5 * fs - 1e-3, fs), 1.0, 1e-5);
    CHECK_NEAR(20.0 * std::log10(sample_filter_magnitude(lp, fc, fs)), -3.0103, 0.2);
    CHECK_NEAR(20.0 * std::log10(sample_filter_magnitude(hp, fc, fs)), -3.0103, 0.2);
    if (fc <= 200.0) CHECK_NEAR(20.0 * std::log10(sample_filter_magnitude(lp, 8 * fc, fs) / sample_filter_magnitude(lp, 16 * fc, fs)), 6.02, 0.35);
    CHECK_NEAR(20.0 * std::log10(sample_filter_magnitude(hp, fc / 32, fs) / sample_filter_magnitude(hp, fc / 16, fs)), -6.02, 0.35);
  }
  // Resonance is the 2-pole peak: +dB at the cutoff, the low-pass peaks above 1 for Q > 0.707 and the response is monotone below 0.707.
  CHECK(sample_filter_magnitude(make_sample_filter(T::kLpf2p, 1000, 12.0, fs), 1000, fs) > 3.9);
  CHECK(sample_filter_magnitude(make_sample_filter(T::kLpf2p, 1000, -3.0103, fs), 1000, fs) < 0.71);  // Q = 0.707: -3 dB at the cutoff
  // No filter: the identity.
  const auto none = make_sample_filter(T::kNone, 1000, 0, fs);
  SampleFilterState st;
  CHECK(sample_filter_tick(none, st, 0.37) == 0.37 && sample_filter_magnitude(none, 777, fs) == 1.0);
}

// A sine through the real voice: the steady-state output amplitude must be the analytic magnitude times the sample amplitude (0.5).
double measure_gain(ports::SampleFilterType t, double fc, double res_db, double freq, bool stereo_check = false) {
  Instrument ins;
  auto& r = ins.sine(freq, 60);
  r.filter_type = t;
  r.filter_cutoff_hz = static_cast<float>(fc);
  r.filter_resonance_db = static_cast<float>(res_db);
  (void)stereo_check;
  SampleEngine e(cfg());
  e.set_instrument(ins);
  CHECK(e.unplayable_regions() == 0);
  press(e, 60);
  render(e, 9600 * 3);
  const Out o = render(e, 9600);
  double s = 0;
  for (float x : o.l) s += static_cast<double>(x) * x;
  return std::sqrt(2.0 * s / static_cast<double>(o.l.size())) / unity() / 0.5;  // amplitude relative to the sine's 0.5
}

void test_filter_in_the_voice_matches_the_analytic_response() {
  using T = ports::SampleFilterType;
  struct Case { T t; double fc, res, f; };
  // Frequencies whose period divides the 9600-frame sample exactly (the loop is seamless): 100 Hz multiples.
  const Case cases[] = {
      {T::kLpf2p, 1000, 0, 500}, {T::kLpf2p, 1000, 0, 1000}, {T::kLpf2p, 1000, 0, 4000}, {T::kLpf2p, 1000, 12, 1000}, {T::kLpf2p, 1000, 12, 800},
      {T::kHpf2p, 2000, 0, 500}, {T::kHpf2p, 2000, 6, 2000}, {T::kHpf2p, 2000, 0, 8000},
      {T::kBpf2p, 1500, 3, 1500}, {T::kBpf2p, 1500, 3, 300}, {T::kBpf2p, 1500, 3, 6000},
      {T::kLpf1p, 800, 0, 800}, {T::kLpf1p, 800, 0, 6400}, {T::kHpf1p, 3000, 0, 3000}, {T::kHpf1p, 3000, 0, 400},
      {T::kLpf2p, 15000, 0, 12000}, {T::kLpf2p, 15000, 0, 15000},          // close to Nyquist
      {T::kLpf2p, 30000, 0, 20000}, {T::kHpf2p, 100, 0, 100}};              // a cutoff above the clamp: the 20 kHz limit
  for (const Case& c : cases) {
    const auto k = make_sample_filter(c.t, c.fc, c.res, 48000.0);
    const double want = sample_filter_magnitude(k, c.f, 48000.0);
    const double got = measure_gain(c.t, c.fc, c.res, c.f);
    CHECK_NEAR(got, want, std::max(2e-4, 2e-3 * want));
  }
  // The clamp: cutoff beyond 20 kHz is 20 kHz.
  CHECK_NEAR(sample_filter_magnitude(make_sample_filter(T::kLpf2p, 30000, 0, 48000.0), 12000, 48000.0),
             sample_filter_magnitude(make_sample_filter(T::kLpf2p, 20000, 0, 48000.0), 12000, 48000.0), 1e-12);
  CHECK_NEAR(sample_filter_magnitude(make_sample_filter(T::kLpf2p, 0.0, 0, 48000.0), 12000, 48000.0),
             sample_filter_magnitude(make_sample_filter(T::kLpf2p, 1.0, 0, 48000.0), 12000, 48000.0), 1e-12);
}

void test_filter_is_stable_for_any_cutoff() {
  using T = ports::SampleFilterType;
  for (T t : {T::kLpf1p, T::kLpf2p, T::kHpf1p, T::kHpf2p, T::kBpf2p}) {
    for (double fc : {0.0, 1.0, 20.0, 1000.0, 10000.0, 19999.0, 20000.0, 23000.0, 23999.0, 24000.0, 48000.0, 1e9}) {
      for (double res : {0.0, 12.0, 40.0, 96.0}) {
        for (double rate : {48000.0, 44100.0, 32000.0}) {
          const auto k = make_sample_filter(t, fc, res, rate);
          CHECK(k.kind != SampleFilterCoeffs::Kind::kNone);
          if (k.kind == SampleFilterCoeffs::Kind::kBiquad) CHECK(std::fabs(k.a2) < 1.0 && std::fabs(k.a1) < 1.0 + k.a2);  // poles inside the unit circle
          else CHECK(k.p > 0.0 && k.p < 1.0);
          SampleFilterState st;
          double peak = 0.0;
          std::uint32_t seed = 12345;
          for (int i = 0; i < 20000; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const double x = static_cast<double>(seed >> 8) / 8388608.0 - 1.0;
            const double y = sample_filter_tick(k, st, x);
            peak = std::max(peak, std::fabs(y));
          }
          double tail = 0.0;  // the response to silence dies away (a 1 Hz resonator with Q = 1000 rings for minutes: only checked where it should be quick)
          for (int i = 0; i < 400000; ++i) tail = std::fabs(sample_filter_tick(k, st, 0.0));
          CHECK(std::isfinite(peak) && peak < 1.0e5);
          if (fc >= 100.0 && res <= 40.0) CHECK(tail < 1e-6);
        }
      }
    }
  }
  // Non-finite parameters never reach the audio thread: the region is unplayable.
  Instrument ins;
  auto& r = ins.dc(0.5, 60, 60);
  r.filter_type = ports::SampleFilterType::kLpf2p;
  r.filter_cutoff_hz = std::numeric_limits<float>::quiet_NaN();
  ins.dc(0.5, 61, 61).filter_type = ports::SampleFilterType::kLpf2p;  // cutoff 0 is accepted (the engine clamps it to 1 Hz)
  auto& bad2 = ins.dc(0.5, 62, 62);
  bad2.filter_type = ports::SampleFilterType::kLpf2p; bad2.filter_cutoff_hz = 100.0f;
  bad2.filter_resonance_db = std::numeric_limits<float>::infinity();
  ins.dc(0.5, 63, 63).xfin_count = 5;                                       // more crossfades than the bound
  auto& bad3 = ins.dc(0.5, 64, 64); bad3.xfin_count = 1; bad3.xfin[0] = {1, 90, 10};  // inverted range
  auto& bad4 = ins.dc(0.5, 65, 65); bad4.sw.last = 200;                    // not a note
  auto& bad5 = ins.dc(0.5, 66, 66); bad5.filter_type = static_cast<ports::SampleFilterType>(77);
  SampleEngine e(cfg());
  e.set_instrument(ins);
  CHECK(e.unplayable_regions() == 6);
  for (int k = 60; k <= 66; ++k) {
    press(e, k);
    render(e, 64);
    lift(e, k);
  }
  CHECK(e.skipped_unplayable_starts() == 5);  // the sw_last = 200 region is gated by the keyswitch before it is counted
  (void)bad4; (void)bad5;
}

void test_filter_stereo_mono_and_bypass() {
  using T = ports::SampleFilterType;
  // A stereo sample: the channels are filtered separately (each with its own state).
  Instrument st;
  {
    std::vector<float> l(9600), r(9600);
    for (std::size_t i = 0; i < l.size(); ++i) {
      l[i] = 0.5f * static_cast<float>(std::sin(2.0 * kPi * 500.0 * static_cast<double>(i) / 48000.0));
      r[i] = 0.5f * static_cast<float>(std::sin(2.0 * kPi * 4000.0 * static_cast<double>(i) / 48000.0));
    }
    ports::SampleRegionDesc d;
    d.sample_index = st.sample(l, r);
    d.key_lo = d.key_hi = d.key_center = 60;
    d.loop_mode = ports::SampleLoopMode::kLoopContinuous;
    d.filter_type = T::kLpf2p;
    d.filter_cutoff_hz = 1000.0f;
    st.regions.push_back(d);
  }
  SampleEngine e(cfg());
  e.set_instrument(st);
  press(e, 60);
  render(e, 9600 * 2);
  const Out o = render(e, 9600);
  auto amp = [](const std::vector<float>& x) {
    double s = 0;
    for (float v : x) s += static_cast<double>(v) * v;
    return std::sqrt(2.0 * s / static_cast<double>(x.size()));
  };
  const auto k = make_sample_filter(T::kLpf2p, 1000, 0, 48000.0);
  const double stereo_comp = std::cos(2047.0 * 1.57079632679489661923 / 4094.0) * std::cos(2047.0 * 1.57079632679489661923 / 4094.0) * 1.9952623149688797;
  CHECK_NEAR(amp(o.l) / stereo_comp / 0.5, sample_filter_magnitude(k, 500, 48000.0), 2e-3);
  CHECK_NEAR(amp(o.r) / stereo_comp / 0.5, sample_filter_magnitude(k, 4000, 48000.0), 2e-3);

  // Bit-exact bypass: regions without a filter or crossfade render the same bits next to regions that have them, and a plain bank
  // renders the same bits whatever the extension fields of OTHER regions hold.
  auto plain = [&](bool with_extras) {
    Instrument ins;
    ins.sine(440.0, 60).ampeg_release = 0.02f;
    ins.sine(700.0, 62).pan = 30.0f;
    if (with_extras) {
      auto& f = ins.sine(300.0, 64); f.filter_type = T::kLpf2p; f.filter_cutoff_hz = 900.0f;
      auto& x = ins.sine(200.0, 65); x.xfin_count = 1; x.xfin[0] = {1, 10, 20};
      auto& k = ins.sine(100.0, 67); k.sw.last = 36;
    }
    SampleEngine eng(cfg());
    eng.set_instrument(ins);
    press(eng, 60);
    press(eng, 62);
    Out a = render(eng, 3000);
    lift(eng, 60);
    Out b = render(eng, 3000);
    a.l.insert(a.l.end(), b.l.begin(), b.l.end());
    a.r.insert(a.r.end(), b.r.begin(), b.r.end());
    return a;
  };
  const Out p0 = plain(false), p1 = plain(true);
  CHECK(p0.l == p1.l && p0.r == p1.r);
  // And the plain voice still is the Story 1.15 arithmetic: sample x envelope x velocity x pan gain.
  Instrument one;
  one.dc(0.5, 60, 60);
  SampleEngine pe(cfg());
  pe.set_instrument(one);
  press(pe, 60);
  CHECK_NEAR(render(pe, 4).l[3] / unity(), 0.5, 1e-6);
}

void test_cost_declaration() {
  using I = SampleInterpolation;
  CHECK(sample_voice_cost_units(I::kHermite3) == 5 && sample_voice_cost_units(I::kLinear) == 3);  // unchanged for the default engine
  CHECK(sample_voice_cost_units(I::kHermite3, true) == 5 + kSampleFilterCostUnits + kSampleCrossfadeCostUnits);
  CHECK(sample_voice_cost_units(I::kLinear, true) > sample_voice_cost_units(I::kLinear, false));
  SampleEngine plain(cfg(8, false)), ext(cfg(8, true));
  CHECK(plain.cost_units_per_voice() == 5 && ext.cost_units_per_voice() == sample_voice_cost_units(I::kHermite3, true));
  // A voice with a filter or a crossfade on an engine that did not declare the extended cost is counted.
  Instrument ins;
  ins.sine(440.0, 60).filter_type = ports::SampleFilterType::kLpf1p;
  ins.sine(440.0, 61).filter_cutoff_hz = 100.0f;  // a cutoff without a type: no filter
  ins.dc(0.1, 62, 62).xfin_count = 0;
  auto& x = ins.dc(0.1, 63, 63); x.xfin_count = 1; x.xfin[0] = {1, 0, 127};
  for (SampleEngine* e : {&plain, &ext}) {
    e->set_instrument(ins);
    press(*e, 60); press(*e, 61); press(*e, 62); press(*e, 63);
  }
  CHECK(plain.undeclared_extended_voices() == 2 && ext.undeclared_extended_voices() == 0);
  // Story 13.8: the second filter's worst case is a separate per-engine declaration; the default and the extended-only engine keep their figures.
  CHECK(sample_voice_cost_units(I::kHermite3, true, true) == 5 + kSampleFilterCostUnits + kSampleCrossfadeCostUnits + kSampleFilter2CostUnits);
  CHECK(sample_voice_cost_units(I::kHermite3, false, true) == 5 && sample_voice_cost_units(I::kHermite3, true, false) == sample_voice_cost_units(I::kHermite3, true));
  SampleEngine::Config c2 = cfg(8, true);
  c2.second_filter_voices = true;
  SampleEngine two(c2), one(cfg(8, true));
  CHECK(two.cost_units_per_voice() == sample_voice_cost_units(I::kHermite3, true, true) && one.cost_units_per_voice() == sample_voice_cost_units(I::kHermite3, true));
  Instrument tw;
  { auto& r = tw.sine(440.0, 60); r.filter_type = ports::SampleFilterType::kLpf2p; r.filter_cutoff_hz = 1000.0f; r.filter2_type = ports::SampleFilterType::kHpf2p; r.filter2_cutoff_hz = 100.0f; }
  for (SampleEngine* e : {&two, &one}) {
    e->set_instrument(tw);
    press(*e, 60);
  }
  CHECK(two.undeclared_second_filter_voices() == 0 && one.undeclared_second_filter_voices() == 1 && plain.undeclared_second_filter_voices() == 0);
}

// --- through the block loop -----------------------------------------------------------------------------------------------------------

constexpr std::uint32_t kKind = static_cast<std::uint32_t>(EngineTypeId::kSample);

struct Rig {
  std::unique_ptr<BlockRenderer> br = std::make_unique<BlockRenderer>(8);
  SampleEngine sample;
  std::vector<std::unique_ptr<Snapshot>> gens;
  Snapshot cur = Snapshot::defaults();
  float l[kBlockFrames], r[kBlockFrames];
  std::vector<float> last_block;
  Rig() : sample(cfg(64)) { CHECK(br->bind_engine(sample)); }
  void bank(std::uint32_t part, const Instrument& ins, bool keyboard = true) {
    PartCfg& c = cur.parts[part];
    c.engine_type = kKind;
    c.state = PartState::kReady;
    c.keyboard_on = keyboard ? 1 : 0;
    auto holder = std::make_shared<Instrument>(ins);  // the bank borrows the PCM: keep a copy alive with it
    // The copy's spans must point at its own storage: rebuild them.
    for (std::size_t i = 0; i < holder->pcm.size(); ++i) {
      holder->pcm[i].left = holder->data[i].data();
      if (holder->pcm[i].channels == 2) holder->pcm[i].right = holder->data[i].data() + holder->pcm[i].frames;
    }
    c.sample_bank = sample.prepare(*holder, holder);
  }
  void publish(bool live = true, std::uint64_t reservation_epoch = 0) {
    cur.live_set = live;
    auto s = std::make_unique<Snapshot>(cur);
    s->ledger_index = static_cast<std::uint32_t>(gens.size() % SnapshotExchange::kLedgerSlots);
    s->revision = gens.size() + 1;
    if (reservation_epoch) {
      VoiceArbiter::LiveSetConfig c;
      c.budget = 100000;
      c.waive_organ_reservation = true;
      c.waive_sample_floor = true;
      CHECK(br->arbiter().prepare_reservation(c, s->reservation) == VoiceArbiter::ReserveStatus::kOk);
      s->reservation_epoch = reservation_epoch;
    }
    br->snapshots().publish(s.get());
    gens.push_back(std::move(s));
  }
  void run(std::uint32_t blocks) {
    for (std::uint32_t b = 0; b < blocks; ++b) {
      br->process_block(l, r);
      last_block.assign(l, l + kBlockFrames);
    }
  }
  double level() const { return last_block.back() / unity(); }
  void on(std::uint32_t key, std::uint32_t ch = 0, std::uint32_t frame = 0) { CHECK(br->submit({make_note_on(0, ch, key, kVel), frame})); }
  void off(std::uint32_t key, std::uint32_t ch = 0, std::uint32_t frame = 0) { CHECK(br->submit({make_note_off(0, ch, key, 0), frame})); }
  void cc(std::uint32_t ch, std::uint32_t num, double value, std::uint32_t frame = 0) {
    CHECK(br->submit({make_control_change(0, ch, num, cc32(value)), frame}));
  }
  double play(std::uint32_t key, std::uint32_t ch = 0) {
    on(key, ch);
    run(8);
    const double v = level();
    off(key, ch);
    run(80);
    return v;
  }
};

void test_cc_path_and_fan_out_through_the_block_loop() {
  Instrument fade;
  auto& a = fade.dc(1.0, 48, 84);
  a.xfin_count = 1; a.xfin[0] = {1, 0, 127};
  a.xf_curve = ports::SampleXfCurve::kGain;

  // Outside a Live Set generation the channel is the part: a CC reaches the engine of that channel only.
  {
    Rig rig;
    rig.sample.set_instrument(fade);
    rig.br->set_channel_engine(0, kKind);
    rig.br->set_channel_engine(1, kKind);
    rig.cc(0, 1, 100);
    rig.run(1);
    CHECK(rig.sample.part_cc(0, 1) == 100.0f && rig.sample.part_cc(1, 1) == 0.0f);
    CHECK_NEAR(rig.play(60, 0), 100.0 / 126.0, 2e-6);
    CHECK_NEAR(rig.play(60, 1), 0.0, 1e-7);
    CHECK(rig.br->ignored_events() == 0);
    rig.cc(5, 1, 40);  // channel 5 has the test tone: nobody reads controllers there
    rig.run(1);
    CHECK(rig.br->ignored_events() == 1);
  }
  // In a Live Set generation the channel names the keyboard source: the CC reaches every part that sounds with the keyboard on.
  {
    Rig rig;
    rig.bank(0, fade);
    rig.bank(1, fade);
    rig.bank(2, fade, /*keyboard=*/false);
    rig.publish();
    rig.run(1);
    rig.cc(9, 1, 80);  // any source channel
    rig.run(1);
    CHECK(rig.sample.part_cc(0, 1) == 80.0f && rig.sample.part_cc(1, 1) == 80.0f && rig.sample.part_cc(2, 1) == 0.0f);
    CHECK(rig.br->ignored_events() == 0);
    CHECK_NEAR(rig.play(60), 2.0 * 80.0 / 126.0, 4e-6);  // parts 0 and 1 both sound the key
    // The part with the keyboard off keeps its own value; a part whose keyboard comes on later takes the next controller move.
    rig.cc(0, 1, 20);
    rig.run(1);
    CHECK(rig.sample.part_cc(0, 1) == 20.0f && rig.sample.part_cc(2, 1) == 0.0f);
    // The sustain pedal still works through the arbiter; since Story 3.11 its continuous value is also kept in the CcState (half-pedal damping reads it).
    rig.cc(0, 64, 127);
    rig.run(1);
    CHECK(rig.sample.part_cc(0, 64) == 127.0f && rig.sample.part_cc(2, 64) == 0.0f);  // the part with the keyboard off is not reached by a pedal down
  }
  // A registry-mapped controller keeps its meaning (a parameter event) and the addressed part's CcState follows it as well.
  {
    Rig rig;
    rig.sample.set_instrument(fade);
    rig.br->set_channel_engine(0, kKind);
    const params::ParamDef* d = params::find_by_cc(72);  // drawbar 8'
    CHECK(d != nullptr);
    Ump pe;
    CHECK(cc_to_param_event(make_control_change(0, 0, 72, cc32(127)), pe));
    CHECK(rig.br->submit({pe, 0}));
    rig.run(1);
    CHECK(rig.br->param_events_applied() == 1);
    CHECK(rig.sample.part_cc(0, 72) == 127.0f);
  }
}

void test_live_set_switch_resets_keyswitches_and_keeps_controllers() {
  auto ins = articulations();
  Instrument fade;
  fade.dc(0.5, 48, 84).xfin_count = 0;
  Rig rig;
  rig.bank(0, *ins);
  rig.bank(1, *ins);
  rig.publish(true, 0);
  rig.run(1);
  CHECK_NEAR(rig.play(60), 0.01 + 0.01, 1e-5);  // both parts follow the keyboard: two sw_default articulations (0.01 each)
  rig.on(37);                                                // both parts get the keyswitch
  rig.run(2);
  rig.off(37);
  rig.run(2);
  CHECK_NEAR(rig.play(60), 0.04, 1e-5);
  CHECK(rig.sample.part_last_keyswitch(0) == 37 && rig.sample.part_last_keyswitch(1) == 37);
  rig.cc(0, 1, 77);
  rig.run(1);
  // Another generation of the same Live Set (a scene recall, an edit): nothing resets.
  rig.publish(true, 0);
  rig.run(2);
  CHECK(rig.sample.part_last_keyswitch(0) == 37);
  // A seamless Live Set switch (a new reservation epoch): keyswitches return to sw_default, controllers stay.
  rig.bank(0, *ins);  // the new Live Set loads its own instruments
  rig.bank(1, *ins);
  rig.publish(true, 1);
  rig.run(2);
  CHECK(rig.sample.part_last_keyswitch(0) == 36 && rig.sample.part_last_keyswitch(1) == 36);
  CHECK(rig.sample.part_cc(0, 1) == 77.0f && rig.sample.part_cc(1, 1) == 77.0f);
  // The same banks again with a new epoch: still a reset.
  rig.on(38);
  rig.run(2);
  rig.off(38);
  rig.run(2);
  CHECK(rig.sample.part_last_keyswitch(0) == 38);
  rig.publish(true, 2);
  rig.run(2);
  CHECK(rig.sample.part_last_keyswitch(0) == 36 && rig.sample.part_last_keyswitch(1) == 36);
  // The keyboard's controllers are physical: a part that sounds with the keyboard on after the switch takes their last values.
  rig.cc(0, 11, 33);
  rig.run(1);
  rig.bank(3, *ins);
  rig.publish(true, 3);
  rig.run(2);
  CHECK(rig.sample.part_cc(3, 11) == 33.0f && rig.sample.part_cc(3, 1) == 77.0f);
  // Leaving the Live Set (a plain generation) drops the bindings; the parts return to their default state.
  rig.publish(false, 0);
  rig.run(2);
  CHECK(rig.sample.part_last_keyswitch(0) == -1);
}

void test_pedal_defers_stop_but_not_key_up() {
  // sw_down follows the physical key even while the sustain pedal holds the voice.
  Instrument ins;
  auto& up = ins.dc(0.01, 48, 84); up.sw.up = 36;
  auto& down = ins.dc(0.02, 48, 84); down.sw.down = 36;
  Rig rig;
  rig.sample.set_instrument(ins);
  rig.br->set_channel_engine(0, kKind);
  rig.cc(0, 64, 127);                       // pedal down
  rig.on(36);
  rig.run(2);
  CHECK_NEAR(rig.play(60), 0.02, 1e-5);     // 36 held: the sw_down region (the pedal keeps this voice sounding after its key is up)
  rig.off(36);
  rig.run(2);                               // the pedal defers the engine's stop_voice, the key is physically up
  CHECK_NEAR(rig.play(62), 0.02 + 0.01, 1e-5);  // the voice of key 60 still sounds, and the new note is the sw_up region
  rig.cc(0, 64, 0);
  rig.run(2);
}


// --- review fixes: key state, bank identity, validation, filter flush -------------------------------------------------------------------

void test_held_keys_follow_physical_note_offs_only() {
  Instrument ins;
  ins.dc(0.01, 48, 84).sw.up = 36;
  ins.dc(0.02, 48, 84).sw.down = 36;
  SampleEngine e(cfg());
  e.set_instrument(ins);
  press(e, 36);
  e.key_released(0, 36);          // physical release; the sustain pedal keeps the engine's stop_voice back
  press(e, 36);                   // the key is down again
  CHECK(e.part_key_held(0, 36));
  e.stop_voice(0, 36);            // the deferred release of the FIRST press arrives (pedal up): the key is still held
  CHECK(e.part_key_held(0, 36));
  CHECK_NEAR(play(e, 60), 0.02, 1e-5);
  lift(e, 36);
  CHECK(!e.part_key_held(0, 36));
  CHECK_NEAR(play(e, 60), 0.01, 1e-5);

  // A part with no instrument (or an empty one) still follows the keyboard: held keys and the previous note.
  SampleEngine none(cfg());
  none.start_voice(0, 40, kVel);
  none.start_voice(0, 41, kVel);
  CHECK(none.part_key_held(0, 40) && none.part_key_held(0, 41) && none.part_previous_note(0) == 41);
  none.key_released(0, 40);
  CHECK(!none.part_key_held(0, 40));
  Instrument empty;
  SampleEngine ee(cfg());
  ee.set_instrument(empty);
  ee.start_voice(1, 50, kVel);
  CHECK(ee.part_key_held(1, 50) && ee.part_previous_note(1) == 50);
}

void test_bank_identity_is_a_token_not_an_address() {
  auto ins = articulations();
  SampleEngine e(cfg());
  auto a = e.prepare(*ins, nullptr);
  auto b = e.prepare(*ins, nullptr);
  CHECK(a->id != 0 && b->id != 0 && a->id != b->id);
  e.set_part_sample_bank(0, a.get());
  press(e, 37, 0); lift(e, 37, 0);
  CHECK(e.part_last_keyswitch(0) == 37);
  SampleBank* stale = a.get();
  a.reset();                                // the old bank is gone; a new one may be allocated at the same address
  auto c = e.prepare(*ins, nullptr);
  e.set_part_sample_bank(0, c.get());
  CHECK(e.part_last_keyswitch(0) == 36);    // another instrument: reset, whatever the address
  (void)stale;
  press(e, 38, 0); lift(e, 38, 0);
  e.clear_part_banks();                     // unbound: back to the default instrument (none), state reset
  CHECK(e.part_last_keyswitch(0) == -1);
}

void test_invalid_extensions() {
  Instrument ins;
  auto& inv = ins.dc(0.5, 60, 60); inv.sw.lokey = 50; inv.sw.hikey = 40;  // inverted keyswitch range
  auto& ok = ins.dc(0.5, 61, 61); ok.sw.lokey = 40; ok.sw.hikey = 50;
  auto& one = ins.dc(0.5, 62, 62); one.sw.lokey = 50;                     // one end only: fine
  SampleEngine e(cfg());
  e.set_instrument(ins);
  CHECK(e.unplayable_regions() == 1);
}

void test_filter_state_flushes_denormals() {
  using T = ports::SampleFilterType;
  for (T t : {T::kLpf1p, T::kLpf2p, T::kHpf1p, T::kHpf2p, T::kBpf2p}) {
    const auto k = make_sample_filter(t, 8000.0, 0.0, 48000.0);
    SampleFilterState st;
    sample_filter_tick(k, st, 1.0);
    for (int i = 0; i < 200000; ++i) sample_filter_tick(k, st, 0.0);
    CHECK(st.y1 == 0.0 && st.y2 == 0.0 && st.w1 == 0.0 && st.x1 == 0.0);  // exactly zero, never a denormal
  }
}

// A registry-mapped MIDI CC is a raw controller like any other: in a Live Set it reaches every part that listens, whatever the scope of the
// parameter and even when the parameter event is refused; a parameter event that did not come from a MIDI CC (UI, ws.v1) does not move it.
void test_mapped_cc_is_a_raw_controller_only_when_midi() {
  Instrument fade;
  fade.dc(1.0, 48, 84);
  Rig rig;
  rig.bank(0, fade);
  rig.bank(1, fade);
  rig.bank(2, fade, /*keyboard=*/false);
  rig.publish();
  rig.run(1);
  Ump pe;
  CHECK(cc_to_param_event(make_control_change(0, 3, 72, cc32(100)), pe));  // MIDI CC 72 on channel 3 (a part that has no sample engine)
  CHECK(is_midi_cc_origin(pe));
  CHECK(rig.br->submit({pe, 0}));
  rig.run(1);
  CHECK(rig.br->param_events_applied() == 1);
  CHECK(rig.sample.part_cc(0, 72) == 100.0f && rig.sample.part_cc(1, 72) == 100.0f);  // all listening parts, not only part == channel
  CHECK(rig.sample.part_cc(2, 72) == 0.0f && rig.sample.part_cc(3, 72) == 0.0f);
  CHECK(rig.br->ignored_events() == 0);
  // The same parameter moved by the UI / ws.v1: no origin mark, no raw controller.
  const params::ParamDef* d = params::find_by_cc(72);
  CHECK(d != nullptr);
  const Ump ui = make_param_event(*d, 0, 8.0);
  CHECK(!is_midi_cc_origin(ui));
  CHECK(rig.br->submit({ui, 0}));
  rig.run(1);
  CHECK(rig.br->param_events_applied() == 2);
  CHECK(rig.sample.part_cc(0, 72) == 100.0f);
  // Every channel is a part (16 parts, Story 4.1), so a MIDI CC parameter event is never refused for its part; it still is a controller.
  Ump bad = make_param_event(*d, 9, 8.0);
  bad.word0 |= kMidiCcOriginBit;
  CHECK(rig.br->submit({bad, 0}));
  rig.run(1);
  CHECK(rig.br->param_events_rejected() == 0);
  CHECK(rig.sample.part_cc(0, 72) == static_cast<float>(127.0 * 1.0));
  // Outside a Live Set the channel is the part.
  Rig plain;
  plain.sample.set_instrument(fade);
  plain.br->set_channel_engine(0, kKind);
  plain.br->set_channel_engine(1, kKind);
  Ump pe2;
  CHECK(cc_to_param_event(make_control_change(0, 1, 72, cc32(50)), pe2));
  CHECK(plain.br->submit({pe2, 0}));
  plain.run(1);
  CHECK(plain.sample.part_cc(1, 72) == 50.0f && plain.sample.part_cc(0, 72) == 0.0f);
}

void test_replay_without_epoch_and_stale_clear() {
  Instrument fade;
  fade.dc(1.0, 48, 84);
  Rig rig;
  rig.bank(0, fade);
  rig.bank(2, fade, /*keyboard=*/false);
  rig.publish();
  rig.run(1);
  rig.cc(0, 1, 77);
  rig.run(1);
  CHECK(rig.sample.part_cc(0, 1) == 77.0f && rig.sample.part_cc(2, 1) == 0.0f);
  // A generation without a new epoch turns part 2's keyboard on: it takes the keyboard's value.
  rig.cur.parts[2].keyboard_on = 1;
  rig.publish();
  rig.run(1);
  CHECK(rig.sample.part_cc(2, 1) == 77.0f);
  // A part that gets its instrument in a later generation (it was silent) also takes it.
  rig.cur.parts[5].engine_type = kKind;
  rig.cur.parts[5].state = PartState::kEmptyTimbre;
  rig.publish();
  rig.run(1);
  CHECK(rig.sample.part_cc(5, 1) == 0.0f);
  rig.bank(5, fade);
  rig.publish();
  rig.run(1);
  CHECK(rig.sample.part_cc(5, 1) == 77.0f);
  // Leaving the Live Set forgets the keyboard's controllers: they are not replayed into a later Live Set.
  rig.publish(false);
  rig.run(2);
  rig.sample.set_part_cc(6, 1, 0u);
  rig.bank(6, fade);
  rig.publish(true);
  rig.run(2);
  CHECK(rig.sample.part_cc(6, 1) == 0.0f);
}

void test_pedal_up_does_not_free_a_held_switch_key() {
  Instrument ins;
  ins.dc(0.01, 48, 84).sw.up = 36;
  ins.dc(0.02, 48, 84).sw.down = 36;
  Rig rig;
  rig.sample.set_instrument(ins);
  rig.br->set_channel_engine(0, kKind);
  rig.cc(0, 64, 127);
  rig.on(36);
  rig.run(2);
  rig.off(36);                 // physical release, the voice-less engine stop is deferred by the pedal
  rig.run(2);
  rig.on(36);                  // pressed again
  rig.run(2);
  rig.cc(0, 64, 0);            // pedal up
  rig.run(2);
  CHECK(rig.sample.part_key_held(0, 36));
}


// --- Story 13.8: per-voice filter ---------------------------------------------------------------------------------------------------

// The steady-state amplitude of a 100 Hz-multiple sine through the real voice, relative to the sample's 0.5 and the unity pan gain. `setup` edits the region, `pre` the engine
// (controllers) before the note; `v16` is the note velocity.
template <class Setup, class Pre>
double voice_gain(double freq, int key, std::uint32_t v16, Setup setup, Pre pre) {
  Instrument ins;
  auto& r = ins.sine(freq, key);
  setup(r);
  SampleEngine e(cfg());
  e.set_instrument(ins);
  CHECK(e.unplayable_regions() == 0);
  pre(e);
  e.start_voice(0, static_cast<std::uint32_t>(key), v16);
  render(e, 9600 * 3);
  const Out o = render(e, 9600);
  double acc = 0;
  for (float x : o.l) acc += static_cast<double>(x) * x;
  // the velocity law (gain = velocity squared, vel7 as the engine derives it) is not part of the filter's response
  const double vel7 = std::max(1.0, std::floor((static_cast<double>(v16) * 127.0 + 32767.0) / 65535.0));
  return std::sqrt(2.0 * acc / static_cast<double>(o.l.size())) / unity() / 0.5 / ((vel7 / 127.0) * (vel7 / 127.0));
}
double lp_gain(double cutoff, double res, double f, ports::SampleFilterType t = ports::SampleFilterType::kLpf2p) {
  return sample_filter_magnitude(make_sample_filter(t, cutoff, res, 48000.0), f, 48000.0);
}
void lpf(ports::SampleRegionDesc& r, double cutoff, double res = 0.0) {
  r.filter_type = ports::SampleFilterType::kLpf2p;
  r.filter_cutoff_hz = static_cast<float>(cutoff);
  r.filter_resonance_db = static_cast<float>(res);
}
const auto none_pre = [](SampleEngine&) {};

void test_filter_keytrack_and_veltrack() {
  // fil_keytrack=100 (a cutoff that follows the key one octave per 12 keys) from fil_keycenter=60, all notes play their own sine at 1000 Hz.
  for (int key : {72, 60, 48, 84}) {
    const double want = lp_gain(1000.0 * std::pow(2.0, (key - 60) / 12.0), 0.0, 1000.0);
    const double got = voice_gain(1000.0, key, kVel, [](auto& r) { lpf(r, 1000.0); r.filter_tune.keytrack = 100.0f; }, none_pre);
    CHECK_NEAR(got, want, std::max(2e-4, 2e-3 * want));
  }
  // fil_keycenter moves the pivot; fil_keytrack of 50 cents per key
  CHECK_NEAR(voice_gain(1000.0, 72, kVel, [](auto& r) { lpf(r, 1000.0); r.filter_tune.keytrack = 50.0f; r.filter_tune.keycenter = 72; }, none_pre), lp_gain(1000.0, 0.0, 1000.0), 2e-3);
  CHECK_NEAR(voice_gain(1000.0, 72, kVel, [](auto& r) { lpf(r, 1000.0); r.filter_tune.keytrack = 50.0f; r.filter_tune.keycenter = 60; }, none_pre),
             lp_gain(1000.0 * std::pow(2.0, 600.0 / 1200.0), 0.0, 1000.0), 2e-3);
  // fil_veltrack=2400: velocity 127 opens two octaves, 0.5 opens one (v7 = 64 -> 2400 * 64 / 127)
  CHECK_NEAR(voice_gain(1000.0, 60, 65535, [](auto& r) { lpf(r, 500.0); r.filter_tune.veltrack = 2400.0f; }, none_pre), lp_gain(2000.0, 0.0, 1000.0), 2e-3);
  CHECK_NEAR(voice_gain(1000.0, 60, 32768, [](auto& r) { lpf(r, 500.0); r.filter_tune.veltrack = 2400.0f; }, none_pre),
             lp_gain(500.0 * std::pow(2.0, 2400.0 * 64.0 / 127.0 / 1200.0), 0.0, 1000.0), 2e-3);
  CHECK_NEAR(voice_gain(1000.0, 60, 1, [](auto& r) { lpf(r, 500.0); r.filter_tune.veltrack = 2400.0f; }, none_pre),
             lp_gain(500.0 * std::pow(2.0, 2400.0 * 1.0 / 127.0 / 1200.0), 0.0, 1000.0), 2e-3);
  // a negative veltrack closes the filter
  CHECK_NEAR(voice_gain(1000.0, 60, 65535, [](auto& r) { lpf(r, 4000.0); r.filter_tune.veltrack = -2400.0f; }, none_pre), lp_gain(1000.0, 0.0, 1000.0), 2e-3);
  // the 20 kHz clamp of the base: 18 kHz opened by 12000 cents is 20 kHz, not 36 MHz
  CHECK_NEAR(voice_gain(1000.0, 60, 65535, [](auto& r) { lpf(r, 18000.0); r.filter_tune.veltrack = 12000.0f; }, none_pre), lp_gain(20000.0, 0.0, 1000.0), 2e-3);
}

void test_filter_cc_entries() {
  using ports::SampleFilterMod;
  auto cut = [](ports::SampleRegionDesc& r, std::uint8_t cc, float depth, std::uint8_t curve = 0) {
    r.filter_tune.cutoff[r.filter_tune.cutoff_count++] = SampleFilterMod{cc, curve, depth};
  };
  // cutoff_oncc74=1200 on a 500 Hz filter: CC 0 leaves it, CC 127 opens one octave, CC 64 by 64/127 of an octave; the sine is at 1000 Hz
  for (double ccv : {0.0, 64.0, 127.0}) {
    const double want = lp_gain(500.0 * std::pow(2.0, 1200.0 * ccv / 127.0 / 1200.0), 0.0, 1000.0);
    const double got = voice_gain(1000.0, 60, kVel, [&](auto& r) { lpf(r, 500.0); cut(r, 74, 1200.0f); }, [&](SampleEngine& e) { e.set_part_cc(0, 74, cc32(ccv)); });
    CHECK_NEAR(got, want, std::max(2e-4, 2e-3 * want));
  }
  // fixed at voice start: the controller moving under a sounding voice changes nothing
  {
    Instrument ins;
    lpf(ins.sine(1000.0, 60), 500.0);
    ins.regions[0].filter_tune.cutoff[0] = {74, 0, 2400.0f};
    ins.regions[0].filter_tune.cutoff_count = 1;
    SampleEngine e(cfg());
    e.set_instrument(ins);
    e.set_part_cc(0, 74, cc32(0.0));
    press(e, 60);
    render(e, 9600 * 3);
    const Out a = render(e, 9600);
    e.set_part_cc(0, 74, cc32(127.0));
    const Out b = render(e, 9600);
    CHECK(a.l == b.l && a.r == b.r);
    lift(e, 60);
    settle(e);
    press(e, 60);  // a new voice reads the new value
    render(e, 9600 * 3);
    const Out c = render(e, 9600);
    CHECK(c.l != a.l);
  }
  // velocity as a controller (131), linear curve: 500 Hz opened by 2400 cents x (vel / 127)
  {
    const double want = lp_gain(500.0 * std::pow(2.0, 2400.0 * (64.0 / 127.0) / 1200.0), 0.0, 1000.0);
    CHECK_NEAR(voice_gain(1000.0, 60, 32768, [&](auto& r) { lpf(r, 500.0); cut(r, 131, 2400.0f); }, none_pre), want, std::max(2e-4, 2e-3 * want));
  }
  // two entries add: CC 1 and CC 2 together
  {
    const double want = lp_gain(500.0 * std::pow(2.0, (600.0 * 0.5 - 300.0) / 1200.0), 0.0, 1000.0);
    const double got = voice_gain(1000.0, 60, kVel, [&](auto& r) { lpf(r, 500.0); cut(r, 1, 600.0f); cut(r, 2, -600.0f); },
                                  [&](SampleEngine& e) { e.set_part_cc(0, 1, cc32(63.5)); e.set_part_cc(0, 2, cc32(63.5)); });
    CHECK_NEAR(got, want, std::max(2e-4, 2e-3 * want));
  }
  // the CC factor is applied to the clamped base: 18 kHz x veltrack (clamped to 20 kHz) then -2400 cents of CC 1 is 5 kHz, not 18 kHz * 2^(12000-2400)
  {
    const double want = lp_gain(20000.0 * 0.25, 0.0, 4000.0);
    const double got = voice_gain(4000.0, 60, kVel, [&](auto& r) { lpf(r, 18000.0); r.filter_tune.veltrack = 12000.0f; cut(r, 1, -2400.0f); },
                                  [&](SampleEngine& e) { e.set_part_cc(0, 1, cc32(127.0)); });
    CHECK_NEAR(got, want, std::max(2e-4, 2e-3 * want));
  }
  // resonance_oncc1=12 dB: Q at the cutoff is 10^(12/20)
  {
    for (double ccv : {0.0, 127.0}) {
      const double want = lp_gain(1000.0, 12.0 * ccv / 127.0, 1000.0);
      const double got = voice_gain(1000.0, 60, kVel, [&](auto& r) { lpf(r, 1000.0); r.filter_tune.res[r.filter_tune.res_count++] = SampleFilterMod{1, 0, 12.0f}; },
                                    [&](SampleEngine& e) { e.set_part_cc(0, 1, cc32(ccv)); });
      CHECK_NEAR(got, want, std::max(2e-4, 2e-3 * want));
    }
  }
}

// Filter entries on the pseudo-CCs 136 (bipolar random), 137 (alternate), 140 and 141 (key delta). Three regions of one sine (keys 60, 67, 55) with the same
// entry on a 500 Hz low-pass, played in that order: the alternate is 0, 1, 0; the key delta (note minus the previous note on) 0, +7, -12 semitones, raw (no curve).
void test_filter_pseudo_cc_entries() {
  struct Case { std::uint8_t cc; double depth; double x[3]; };
  const Case cases[] = {{137, 1200.0, {0.0, 1.0, 0.0}}, {140, 100.0, {0.0, 7.0, -12.0}}, {141, 100.0, {0.0, 7.0, 12.0}}};
  for (const Case& c : cases) {
    Instrument ins;
    for (int key : {60, 67, 55}) {
      auto& r = ins.sine(1000.0, key);
      lpf(r, 500.0);
      r.filter_tune.cutoff[0] = {c.cc, 0, static_cast<float>(c.depth)};
      r.filter_tune.cutoff_count = 1;
    }
    SampleEngine e(cfg());
    e.set_instrument(ins);
    const int keys[3] = {60, 67, 55};
    for (int n = 0; n < 3; ++n) {
      press(e, keys[n]);
      render(e, 9600 * 3);
      const Out o = render(e, 9600);
      double acc = 0;
      for (float v : o.l) acc += static_cast<double>(v) * v;
      const double got = std::sqrt(2.0 * acc / static_cast<double>(o.l.size())) / unity() / 0.5 / (1.0);
      const double want = lp_gain(500.0 * std::pow(2.0, c.depth * c.x[n] / 1200.0), 0.0, 1000.0);
      CHECK_NEAR(got, want, std::max(3e-4, 3e-3 * want));
      lift(e, keys[n]);
      settle(e);
    }
  }
  // 136: a uniform draw in [-1, 1) through the linear curve (a negative value clamps to 0): the cutoff is one of 500 * 2^(1200 * max(0, u) / 1200) Hz; find that u
  {
    Instrument ins;
    auto& r = ins.sine(1000.0, 60);
    lpf(r, 500.0);
    r.filter_tune.cutoff[0] = {136, 0, 1200.0f};
    r.filter_tune.cutoff_count = 1;
    SampleEngine e(cfg());
    e.set_instrument(ins);
    press(e, 60);
    render(e, 9600 * 3);
    const Out o = render(e, 9600);
    double acc = 0;
    for (float v : o.l) acc += static_cast<double>(v) * v;
    const double got = std::sqrt(2.0 * acc / static_cast<double>(o.l.size())) / unity() / 0.5;
    double best = 1e9;
    for (int i = 0; i <= 1000; ++i) best = std::min(best, std::fabs(got - lp_gain(500.0 * std::pow(2.0, i / 1000.0), 0.0, 1000.0)));
    CHECK(best < 3e-3);
  }
}

void test_filter_second_filter() {
  using T = ports::SampleFilterType;
  // lpf_2p at 2000 then hpf_2p at 500, in series: the response is the product
  for (double f : {200.0, 500.0, 1000.0, 2000.0, 4000.0}) {
    const double want = lp_gain(2000.0, 0.0, f) * lp_gain(500.0, 3.0, f, T::kHpf2p);
    const double got = voice_gain(f, 60, kVel, [](auto& r) {
      lpf(r, 2000.0);
      r.filter2_type = T::kHpf2p; r.filter2_cutoff_hz = 500.0f; r.filter2_resonance_db = 3.0f;
    }, none_pre);
    CHECK_NEAR(got, want, std::max(3e-4, 3e-3 * want));
  }
  // the second filter has its own tuning: key track on the second one only
  {
    const double want = lp_gain(8000.0, 0.0, 1000.0) * lp_gain(250.0 * 2.0, 0.0, 1000.0, T::kHpf2p);
    const double got = voice_gain(1000.0, 72, kVel, [](auto& r) {
      lpf(r, 8000.0);
      r.filter2_type = T::kHpf2p; r.filter2_cutoff_hz = 250.0f; r.filter2_tune.keytrack = 100.0f;
    }, none_pre);
    CHECK_NEAR(got, want, std::max(3e-4, 3e-3 * want));
  }
  // and a cutoff entry on the second
  {
    const double want = lp_gain(8000.0, 0.0, 1000.0) * lp_gain(250.0 * 2.0, 0.0, 1000.0, T::kHpf2p);
    const double got = voice_gain(1000.0, 60, kVel, [](auto& r) {
      lpf(r, 8000.0);
      r.filter2_type = T::kHpf2p; r.filter2_cutoff_hz = 250.0f;
      r.filter2_tune.cutoff[0] = {1, 0, 1200.0f}; r.filter2_tune.cutoff_count = 1;
    }, [](SampleEngine& e) { e.set_part_cc(0, 1, cc32(127.0)); });
    CHECK_NEAR(got, want, std::max(3e-4, 3e-3 * want));
  }
  // a stereo sample is filtered per channel through both
  {
    Instrument st;
    std::vector<float> l(9600), r(9600);
    for (std::size_t i = 0; i < l.size(); ++i) {
      l[i] = 0.5f * static_cast<float>(std::sin(2.0 * kPi * 500.0 * static_cast<double>(i) / 48000.0));
      r[i] = 0.5f * static_cast<float>(std::sin(2.0 * kPi * 4000.0 * static_cast<double>(i) / 48000.0));
    }
    ports::SampleRegionDesc d;
    d.sample_index = st.sample(l, r);
    d.key_lo = d.key_hi = d.key_center = 60;
    d.loop_mode = ports::SampleLoopMode::kLoopContinuous;
    lpf(d, 1000.0);
    d.filter2_type = T::kLpf1p; d.filter2_cutoff_hz = 3000.0f;
    st.regions.push_back(d);
    SampleEngine e(cfg());
    e.set_instrument(st);
    press(e, 60);
    render(e, 9600 * 2);
    const Out o = render(e, 9600);
    auto amp = [](const std::vector<float>& x) {
      double acc = 0;
      for (float v : x) acc += static_cast<double>(v) * v;
      return std::sqrt(2.0 * acc / static_cast<double>(x.size()));
    };
    const double stereo_comp = std::cos(2047.0 * kPi / 2.0 / 4094.0) * std::cos(2047.0 * kPi / 2.0 / 4094.0) * 1.9952623149688797;
    CHECK_NEAR(amp(o.l) / stereo_comp / 0.5, lp_gain(1000.0, 0.0, 500.0) * lp_gain(3000.0, 0.0, 500.0, T::kLpf1p), 2e-3);
    CHECK_NEAR(amp(o.r) / stereo_comp / 0.5, lp_gain(1000.0, 0.0, 4000.0) * lp_gain(3000.0, 0.0, 4000.0, T::kLpf1p), 2e-3);
  }
}

void test_filter_bit_identity_without_new_fields() {
  // A tuned region whose tuning evaluates to nothing (key track on the key centre) computes the very coefficients the region would have prepared: the same bits.
  auto render_key = [](bool tuned) {
    Instrument ins;
    auto& r = ins.sine(700.0, 60);
    lpf(r, 900.0, 4.0);
    if (tuned) { r.filter_tune.keytrack = 100.0f; r.filter_tune.keycenter = 60; }
    SampleEngine e(cfg());
    e.set_instrument(ins);
    press(e, 60);
    return render(e, 9600);
  };
  const Out a = render_key(false), b = render_key(true);
  CHECK(a.l == b.l && a.r == b.r);
  // a second filter on one region does not touch the output of another
  auto render_two = [](bool second) {
    Instrument ins;
    lpf(ins.sine(700.0, 60), 900.0, 4.0);
    auto& r = ins.sine(300.0, 62);
    lpf(r, 2000.0);
    if (second) { r.filter2_type = ports::SampleFilterType::kHpf2p; r.filter2_cutoff_hz = 100.0f; }
    SampleEngine e(cfg());
    e.set_instrument(ins);
    press(e, 60);
    return render(e, 9600);
  };
  const Out c = render_two(false), d = render_two(true);
  CHECK(c.l == d.l && c.r == d.r);
  // a key centre without a key track is no tuning: same bits, and the region is playable
  auto render_centre = [](bool centre) {
    Instrument ins;
    auto& r = ins.sine(700.0, 60);
    lpf(r, 900.0, 4.0);
    if (centre) r.filter_tune.keycenter = 72;
    SampleEngine e(cfg());
    e.set_instrument(ins);
    CHECK(e.unplayable_regions() == 0);
    press(e, 60);
    return render(e, 9600);
  };
  const Out g = render_centre(false), h = render_centre(true);
  CHECK(g.l == h.l && g.r == h.r);
}

void test_filter_per_voice_state_and_polyphony() {
  // two notes of one region with a key-tracked cutoff each use their own coefficients (a chord: the mix is the sum of the two single notes)
  auto one = [](std::vector<int> keys) {
    Instrument ins;
    auto& r = ins.dc(0.5, 0, 127);
    r.key_center = 60;
    lpf(r, 1000.0, 6.0);
    r.filter_tune.keytrack = 100.0f;
    SampleEngine e(cfg());
    e.set_instrument(ins);
    for (int k : keys) press(e, k);
    return render(e, 2000);
  };
  const Out a = one({48}), b = one({72}), ab = one({48, 72});
  double worst = 0;
  for (std::size_t i = 0; i < ab.l.size(); ++i) worst = std::max(worst, static_cast<double>(std::fabs(ab.l[i] - a.l[i] - b.l[i])));
  CHECK(worst < 1e-5);
  CHECK(a.l != b.l);
}

void test_filter_extremes_nan_and_subnormal() {
  using T = ports::SampleFilterType;
  // the per-voice parameters at their extremes never produce a non-finite coefficient or output
  struct P { double cut, res; };
  const P extremes[] = {{1.0, -60.0}, {1.0, 60.0}, {20000.0, 60.0}, {20000.0, -60.0}, {0.0, 0.0}, {1e30, 1e30}, {-5.0, -1e9}, {std::nan(""), std::nan("")},
                        {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}, {1e-310, 1e-310}};
  for (T t : {T::kLpf1p, T::kLpf2p, T::kHpf1p, T::kHpf2p, T::kBpf2p})
    for (const P& p : extremes) {
      const auto k = make_sample_filter(t, p.cut, p.res, 48000.0);
      CHECK(k.kind != SampleFilterCoeffs::Kind::kNone);
      CHECK(std::isfinite(k.b0) && std::isfinite(k.b1) && std::isfinite(k.b2) && std::isfinite(k.a1) && std::isfinite(k.a2) && std::isfinite(k.p) && std::isfinite(k.g));
      SampleFilterState st;
      std::uint32_t seed = 99;
      double peak = 0.0;
      for (int i = 0; i < 100000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        double x = static_cast<double>(seed >> 8) / 8388608.0 - 1.0;
        if (i % 1000 == 7) x = std::numeric_limits<double>::quiet_NaN();              // NaN input
        else if (i % 1000 == 11) x = std::numeric_limits<double>::infinity();           // infinity
        else if (i % 1000 == 13) x = -std::numeric_limits<double>::infinity();
        else if (i % 1000 == 17) x = 1e300;                                             // a magnitude no audio has
        else if (i % 1000 >= 100 && i % 1000 < 140) x = 1e-310 * (seed & 0xff);         // subnormal stream
        const double y = sample_filter_tick(k, st, x);
        CHECK(std::isfinite(y));
        peak = std::max(peak, std::fabs(y));
      }
      CHECK(peak < 1.0e6);
      CHECK(std::isfinite(st.x1) && std::isfinite(st.x2) && std::isfinite(st.y1) && std::isfinite(st.y2) && std::isfinite(st.w1));
    }
  // a state that is fed only subnormals flushes to exact zero
  for (T t : {T::kLpf1p, T::kLpf2p, T::kHpf1p, T::kHpf2p, T::kBpf2p}) {
    const auto k = make_sample_filter(t, 1000.0, 6.0, 48000.0);
    SampleFilterState st;
    for (int i = 0; i < 1000; ++i) sample_filter_tick(k, st, (i & 1) ? 1e-310 : -1e-310);
    CHECK(std::fpclassify(st.y1) != FP_SUBNORMAL && std::fpclassify(st.y2) != FP_SUBNORMAL && std::fpclassify(st.x1) != FP_SUBNORMAL && std::fpclassify(st.w1) != FP_SUBNORMAL);
  }
  // through the real voice: a sample full of NaN, infinity and subnormals, extreme tuning (every entry at the largest depth), both filters: the output stays finite
  {
    Instrument ins;
    std::vector<float> x(9600);
    for (std::size_t i = 0; i < x.size(); ++i) {
      x[i] = 0.5f * static_cast<float>(std::sin(0.05 * static_cast<double>(i)));
      if (i % 500 == 3) x[i] = std::numeric_limits<float>::quiet_NaN();
      if (i % 500 == 5) x[i] = std::numeric_limits<float>::infinity();
      if (i % 500 >= 100 && i % 500 < 120) x[i] = 1e-40f;  // subnormal float
    }
    ports::SampleRegionDesc d;
    d.sample_index = ins.sample(std::move(x));
    d.key_lo = d.key_hi = d.key_center = 60;
    d.loop_mode = ports::SampleLoopMode::kLoopContinuous;
    lpf(d, 20000.0, 40.0);
    d.filter_tune.keytrack = 1200.0f; d.filter_tune.keycenter = 0; d.filter_tune.veltrack = 12000.0f;
    d.filter_tune.cutoff[0] = {1, 0, 12000.0f}; d.filter_tune.cutoff[1] = {2, 0, 12000.0f}; d.filter_tune.cutoff[2] = {3, 0, 12000.0f}; d.filter_tune.cutoff[3] = {4, 0, 12000.0f};
    d.filter_tune.cutoff_count = 4;
    d.filter_tune.res[0] = {1, 0, 96.0f}; d.filter_tune.res[1] = {2, 0, 96.0f}; d.filter_tune.res_count = 2;
    d.filter2_type = T::kBpf2p; d.filter2_cutoff_hz = 1.0f; d.filter2_resonance_db = 40.0f;
    d.filter2_tune.cutoff[0] = {1, 0, -12000.0f}; d.filter2_tune.cutoff_count = 1;
    ins.regions.push_back(d);
    SampleEngine e(cfg());
    e.set_instrument(ins);
    CHECK(e.unplayable_regions() == 0);
    for (unsigned cc : {1u, 2u, 3u, 4u}) e.set_part_cc(0, cc, cc32(127.0));
    press(e, 60);
    bool finite = true;
    for (int i = 0; i < 20; ++i) {
      const Out o = render(e, 9600);
      for (std::size_t j = 0; j < o.l.size(); ++j) finite = finite && std::isfinite(o.l[j]) && std::isfinite(o.r[j]);
    }
    CHECK(finite);
  }
}

void test_filter_invalid_fields_are_unplayable() {
  using T = ports::SampleFilterType;
  Instrument ins;
  auto ok = [&](int key) -> ports::SampleRegionDesc& { auto& r = ins.dc(0.5, key, key); lpf(r, 1000.0); return r; };
  ok(60).filter_tune.keytrack = 100.0f;                                                                // fine
  ok(61).filter_tune.keytrack = 1201.0f;                                                               // keytrack out of range
  ok(62).filter_tune.veltrack = std::numeric_limits<float>::quiet_NaN();                               // NaN
  ok(63).filter_tune.keycenter = 128;                                                                  // not a note
  ok(64).filter_tune.cutoff_count = 5;                                                                 // more entries than the bound
  { auto& r = ok(65); r.filter_tune.cutoff[0] = {132, 0, 1.0f}; r.filter_tune.cutoff_count = 1; }     // not a source
  { auto& r = ok(66); r.filter_tune.cutoff[0] = {1, 0, 13000.0f}; r.filter_tune.cutoff_count = 1; }   // depth out of range
  { auto& r = ok(67); r.filter_tune.res[0] = {1, 0, -1.0f}; r.filter_tune.res_count = 1; }            // negative resonance depth
  { auto& r = ins.dc(0.5, 68, 68); r.filter_tune.veltrack = 100.0f; }                                  // a tuning without a first filter
  { auto& r = ins.dc(0.5, 69, 69); r.filter2_type = T::kLpf2p; r.filter2_cutoff_hz = 100.0f; }       // a second filter without a first
  ok(70).filter2_type = static_cast<T>(77);                                                            // unknown type
  { auto& r = ok(71); r.filter2_type = T::kLpf2p; r.filter2_cutoff_hz = std::numeric_limits<float>::infinity(); }
  { auto& r = ok(72); r.filter2_tune.keytrack = 10.0f; }                                               // a tuning of an absent second filter
  { auto& r = ok(73); r.filter2_type = T::kHpf1p; r.filter2_cutoff_hz = 100.0f; r.filter2_tune.keytrack = 10.0f; }  // fine
  SampleEngine e(cfg());
  e.set_instrument(ins);
  CHECK(e.unplayable_regions() == 12);
  // the unplayable ones are never started
  for (int k = 60; k <= 73; ++k) {
    press(e, k);
    render(e, 64);
    lift(e, k);
  }
  CHECK(e.skipped_unplayable_starts() == 12);
}

void test_filter_deterministic_with_random_pseudo_cc() {
  // a cutoff entry on the random pseudo-CC 135: deterministic for a seed, different between seeds
  auto run = [](float seed) {
    Instrument ins;
    auto& r = ins.sine(1000.0, 60);
    lpf(r, 500.0);
    r.filter_tune.cutoff[0] = {135, 0, 2400.0f};
    r.filter_tune.cutoff_count = 1;
    SampleEngine e(cfg());
    e.set_instrument(ins);
    e.set_param(SampleEngine::kParamSeed, seed);
    press(e, 60);
    return render(e, 2000);
  };
  const Out a = run(5.0f), b = run(5.0f), c = run(6.0f);
  CHECK(a.l == b.l);
  CHECK(a.l != c.l);
}

}  // namespace

// --- Story 13.9: region EQ and dedicated LFO / EG modulation -------------------------------------------------------------------------

// A mono ramp sample (value i / 20000): the output of a voice reads the playhead, so the slope per frame is the pitch ratio times gain.
ports::SampleRegionDesc& ramp(Instrument& ins, int key) {
  std::vector<float> x(20000);
  for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(i) / 20000.0f;
  ports::SampleRegionDesc d;
  d.sample_index = ins.sample(std::move(x));
  d.key_lo = d.key_hi = d.key_center = static_cast<std::uint8_t>(key);
  ins.regions.push_back(d);
  return ins.regions.back();
}
// Slope (output per frame, in units of the unity pan gain) of a ramp voice between two frames of its render.
double slope(const Out& o, std::size_t a, std::size_t b) { return (o.l[b] - o.l[a]) / unity() / static_cast<double>(b - a); }

// The wave of sfizz's LFO.cpp, written out here independently of the engine (double), for the phase p in [0, 1).
double lfo_ref(int wave, double p) {
  switch (wave) {
    case 0: return p < 0.25 ? 4 * p : p > 0.75 ? 4 * p - 4 : -4 * p + 2;
    case 2: return p < 0.75 ? 1 : 0;
    case 3: return p < 0.5 ? 1 : 0;
    case 4: return p < 0.25 ? 1 : 0;
    case 5: return p < 0.125 ? 1 : 0;
    case 6: return 2 * p - 1;
    case 7: return 1 - 2 * p;
    default: { const double x = 2 * p - 1; return -4 * x * (1 - std::fabs(x)); }
  }
}

void test_mod_cost_declaration() {
  using I = SampleInterpolation;
  CHECK(sample_voice_cost_units(I::kHermite3, true, false, true) == sample_voice_cost_units(I::kHermite3, true) + kSampleModCostUnits);
  CHECK(sample_voice_cost_units(I::kHermite3, false, false, true) == 5 && sample_voice_cost_units(I::kHermite3, true, true, true) == sample_voice_cost_units(I::kHermite3, true, true) + kSampleModCostUnits);
  SampleEngine::Config c = cfg(8, true);
  c.modulated_voices = true;
  SampleEngine yes(c), no(cfg(8, true)), plain(cfg(8, false));
  CHECK(yes.cost_units_per_voice() == sample_voice_cost_units(I::kHermite3, true, false, true) && no.cost_units_per_voice() == sample_voice_cost_units(I::kHermite3, true) && plain.cost_units_per_voice() == 5);
  Instrument ins;
  ins.dc(0.3, 60, 60).mod.amp = {true, 1, 2.0f, 5.0f, 0.0f, 0.0f};
  { auto& r = ins.dc(0.3, 61, 61); r.mod.eq_count = 1; r.mod.eq[0].freq = 1000.0f; r.mod.eq[0].gain = 3.0f; }
  { auto& r = ins.dc(0.3, 62, 62); r.mod.eq_count = 1; r.mod.eq[0].freq = 1000.0f; }  // a transparent band is not a modulated voice
  ins.dc(0.3, 63, 63);
  for (SampleEngine* e : {&yes, &no, &plain}) {
    e->set_instrument(ins);
    for (int k = 60; k <= 63; ++k) press(*e, k);
  }
  CHECK(yes.undeclared_modulated_voices() == 0 && no.undeclared_modulated_voices() == 2 && plain.undeclared_modulated_voices() == 2);
}

double playhead(const SampleEngine& e);

// Story 13.9: the pitch and filter EGs leave with the amplitude EG on every release path. A choke (off_mode normal: the voice's own release) plays exactly what a key up
// plays; an EG still in its delay when the amplitude EG is released goes idle instead of releasing from its start level.
void test_mod_egs_follow_every_release() {
  auto run = [](bool choke) {
    Instrument ins;
    auto& a = ins.sine(700.0, 60);
    lpf(a, 600.0, 3.0);
    a.ampeg_release = 0.3f;
    a.mod.pitch_eg = {true, 0, 0.05f, 0, 0, 0.4f, 0, 100.0f, 1200.0f, 0.0f};
    a.mod.fil_eg = {true, 0, 0.05f, 0, 0, 0.3f, 0, 100.0f, 2400.0f, 0.0f};
    a.group = 1; a.has_off_by = true; a.off_by = 2; a.off_mode = ports::SampleOffMode::kNormal;
    auto& b = ins.dc(0.0, 61, 61);  // silent: it only starts the choke
    b.group = 2;
    SampleEngine e(cfg());
    e.set_instrument(ins);
    CHECK(e.unplayable_regions() == 0);
    press(e, 60);
    Out o = render(e, 2000);
    if (choke) press(e, 61);
    else lift(e, 60);
    const Out t = render(e, 6000);
    o.l.insert(o.l.end(), t.l.begin(), t.l.end());
    return o;
  };
  const Out ch = run(true), up = run(false);
  CHECK(ch.l == up.l);
  double peak = 0;
  for (std::size_t i = 2100; i < ch.l.size(); ++i) peak = std::max(peak, static_cast<double>(std::fabs(ch.l[i])));
  CHECK(peak > 1e-4);
  {  // released during its delay: idle, not a release from the start level (start 100 % = +1200 cents until then)
    Instrument ins;
    ramp(ins, 60).ampeg_release = 0.5f;
    ins.regions[0].mod.pitch_eg = {true, 1.0f, 0, 0, 0, 0, 100.0f, 100.0f, 1200.0f, 0.0f};
    SampleEngine e(cfg());
    e.set_instrument(ins);
    press(e, 60);
    render(e, 400);
    const double p0 = playhead(e);
    render(e, 100);
    CHECK_NEAR((playhead(e) - p0) / 100.0, 2.0, 2e-3);  // in the delay at its start level
    lift(e, 60);
    render(e, 10);
    const double q0 = playhead(e);
    render(e, 100);
    CHECK_NEAR((playhead(e) - q0) / 100.0, 1.0, 2e-3);  // idle: the original speed at once
  }
}

// Story 13.9: modulation without the extended declaration is counted as both missing declarations.
void test_mod_counts_the_missing_extended_declaration() {
  Instrument ins;
  ins.dc(0.3, 60, 60).mod.amp = {true, 1, 2.0f, 5.0f, 0.0f, 0.0f};
  SampleEngine::Config c = cfg(8, false);
  c.modulated_voices = true;
  SampleEngine only_mod(c), none(cfg(8, false)), full(cfg(8, true));
  for (SampleEngine* e : {&only_mod, &none, &full}) {
    e->set_instrument(ins);
    press(*e, 60);
  }
  CHECK(only_mod.undeclared_extended_voices() == 1 && only_mod.undeclared_modulated_voices() == 1);
  CHECK(none.undeclared_extended_voices() == 1 && none.undeclared_modulated_voices() == 1);
  CHECK(full.undeclared_extended_voices() == 0 && full.undeclared_modulated_voices() == 1);
}

void test_eq_band_response() {
  // The bands in the voice match the analytic response of sample_filter_magnitude(make_sample_eq(...)): peak at its centre, shelves far from their corner.
  auto eq_voice = [](double freq, int key, double band_hz, double bw, double gain, unsigned type) {
    return voice_gain(freq, key, kVel, [&](auto& r) {
      r.mod.eq_count = 1;
      r.mod.eq[0].type = static_cast<std::uint8_t>(type);
      r.mod.eq[0].freq = static_cast<float>(band_hz);
      r.mod.eq[0].bw = static_cast<float>(bw);
      r.mod.eq[0].gain = static_cast<float>(gain);
    }, none_pre);
  };
  auto ana = [](unsigned type, double band_hz, double bw, double gain, double f) { return sample_filter_magnitude(make_sample_eq(type, band_hz, bw, gain, 48000.0), f, 48000.0); };
  for (double g : {6.0, -9.0, 12.0}) {
    CHECK_NEAR(eq_voice(1000.0, 60, 1000.0, 1.0, g, 1), std::pow(10.0, g / 20.0), 2e-3 * std::pow(10.0, std::fabs(g) / 20.0));  // the peak's centre gain is the band gain
    CHECK_NEAR(eq_voice(1000.0, 60, 1000.0, 1.0, g, 1), ana(1, 1000.0, 1.0, g, 1000.0), 2e-3 * std::pow(10.0, std::fabs(g) / 20.0));
    CHECK_NEAR(eq_voice(2000.0, 60, 1000.0, 0.5, g, 1), ana(1, 1000.0, 0.5, g, 2000.0), 2e-3 * std::pow(10.0, std::fabs(g) / 20.0));
  }
  CHECK_NEAR(eq_voice(100.0, 60, 3000.0, 1.0, -6.0, 2), ana(2, 3000.0, 1.0, -6.0, 100.0), 2e-3);   // low shelf: the lows move
  CHECK_NEAR(eq_voice(100.0, 60, 3000.0, 1.0, -6.0, 2), std::pow(10.0, -6.0 / 20.0), 4e-3);
  CHECK_NEAR(eq_voice(10000.0, 60, 3000.0, 1.0, -6.0, 2), ana(2, 3000.0, 1.0, -6.0, 10000.0), 3e-3);  // ... and the highs hardly do (the shelf's skirt is wide)
  CHECK_NEAR(eq_voice(10000.0, 60, 3000.0, 1.0, 8.0, 3), ana(3, 3000.0, 1.0, 8.0, 10000.0), 3e-3);  // high shelf
  CHECK_NEAR(eq_voice(10000.0, 60, 3000.0, 1.0, 8.0, 3), std::pow(10.0, 8.0 / 20.0), 0.15);
  CHECK_NEAR(eq_voice(100.0, 60, 3000.0, 1.0, 8.0, 3), 1.0, 4e-3);
  // a band at 0 dB is skipped: bit-identical to a region without it
  auto render_band = [](bool band) {
    Instrument ins;
    auto& r = ins.sine(700.0, 60);
    if (band) { r.mod.eq_count = 2; r.mod.eq[0].freq = 100.0f; r.mod.eq[1].freq = 500.0f; r.mod.eq[1].bw = 2.0f; }
    SampleEngine e(cfg());
    e.set_instrument(ins);
    CHECK(e.unplayable_regions() == 0);
    press(e, 60);
    return render(e, 4800);
  };
  const Out a = render_band(false), b = render_band(true);
  CHECK(a.l == b.l && a.r == b.r);
  // bands run in series and after the filter: lpf 1000 Hz then +6 dB peak at 3000 Hz = the product of the responses
  const double got = voice_gain(2000.0, 60, kVel, [](auto& r) { lpf(r, 1000.0, 0.0); r.mod.eq_count = 1; r.mod.eq[0].freq = 3000.0f; r.mod.eq[0].gain = 6.0f; r.mod.eq[0].bw = 3.0f; }, none_pre);
  CHECK_NEAR(got, lp_gain(1000.0, 0.0, 2000.0) * sample_filter_magnitude(make_sample_eq(1, 3000.0, 3.0, 6.0, 48000.0), 2000.0, 48000.0), 2e-3);
  // two bands: the product
  const double two = voice_gain(1000.0, 60, kVel, [](auto& r) { r.mod.eq_count = 2; r.mod.eq[0].freq = 1000.0f; r.mod.eq[0].gain = 4.0f; r.mod.eq[1].type = 2; r.mod.eq[1].freq = 4000.0f; r.mod.eq[1].gain = -3.0f; }, none_pre);
  CHECK_NEAR(two, ana(1, 1000.0, 1.0, 4.0, 1000.0) * ana(2, 4000.0, 1.0, -3.0, 1000.0), 3e-3);
  // stereo and mono sources get the same band
  Instrument st;
  { auto& r = st.sine(1000.0, 60); r.mod.eq_count = 1; r.mod.eq[0].freq = 1000.0f; r.mod.eq[0].gain = 6.0f; }
  SampleEngine e(cfg());
  e.set_instrument(st);
  press(e, 60);
  render(e, 9600 * 3);
  const Out o = render(e, 9600);
  double al = 0, ar = 0;
  for (std::size_t i = 0; i < o.l.size(); ++i) { al += o.l[i] * o.l[i]; ar += o.r[i] * o.r[i]; }
  CHECK_NEAR(al, ar, 1e-9 * al);
}

void test_eq_velocity_and_controllers() {
  // freq / gain at the note velocity and the controller state at the note on: gain = 0 + vel * vel2gain + sum(depth * curve(cc))
  using ports::SampleFilterMod;
  auto gain_at = [](std::uint32_t v16, double cc1, double base_gain, double vel2gain, double cc_depth) {
    const std::uint32_t v7 = static_cast<std::uint32_t>(std::max(1.0, std::floor((static_cast<double>(v16) * 127.0 + 32767.0) / 65535.0)));
    (void)v7;
    return voice_gain(1000.0, 60, v16, [&](auto& r) {
      r.mod.eq_count = 1;
      r.mod.eq[0].freq = 1000.0f; r.mod.eq[0].gain = static_cast<float>(base_gain); r.mod.eq[0].vel2gain = static_cast<float>(vel2gain);
      if (cc_depth != 0.0) { r.mod.eq[0].gain_count = 1; r.mod.eq[0].gain_mods[0] = SampleFilterMod{1, 0, static_cast<float>(cc_depth)}; }
    }, [&](SampleEngine& e) { e.set_part_cc(0, 1, cc32(cc1)); });
  };
  CHECK_NEAR(gain_at(kVel, 0.0, 0.0, 12.0, 0.0), std::pow(10.0, 12.0 / 20.0), 2e-2);                    // velocity 127: +12 dB
  CHECK_NEAR(gain_at(32768, 0.0, 0.0, 12.0, 0.0), std::pow(10.0, 12.0 * 64.0 / 127.0 / 20.0), 2e-2);    // vel7 64
  CHECK_NEAR(gain_at(kVel, 127.0, 0.0, 0.0, 6.0), std::pow(10.0, 6.0 / 20.0), 1e-2);                    // controller at full: +6 dB
  CHECK_NEAR(gain_at(kVel, 63.5, 0.0, 0.0, 6.0), std::pow(10.0, 3.0 / 20.0), 1e-2);                     // half
  CHECK_NEAR(gain_at(kVel, 0.0, 0.0, 0.0, 6.0), 1.0, 2e-3);                                              // controller at 0: transparent
  CHECK_NEAR(gain_at(kVel, 127.0, -3.0, 0.0, 9.0), std::pow(10.0, 6.0 / 20.0), 1e-2);                   // sums with the base
  // the frequency moves with velocity: peak at 500 + 1000 * vel Hz, measured on a 1500 Hz sine
  const double hi = voice_gain(1500.0, 60, kVel, [](auto& r) { r.mod.eq_count = 1; r.mod.eq[0].freq = 500.0f; r.mod.eq[0].vel2freq = 1000.0f; r.mod.eq[0].gain = 6.0f; r.mod.eq[0].bw = 0.25f; }, none_pre);
  const double lo = voice_gain(1500.0, 60, 1, [](auto& r) { r.mod.eq_count = 1; r.mod.eq[0].freq = 500.0f; r.mod.eq[0].vel2freq = 1000.0f; r.mod.eq[0].gain = 6.0f; r.mod.eq[0].bw = 0.25f; }, none_pre);
  CHECK_NEAR(hi, std::pow(10.0, 6.0 / 20.0), 2e-2);
  CHECK(lo < 1.05);
}

void test_lfo_amplitude_waves_delay_fade() {
  // Every wave: the gain of frame n is 10^(depth * wave(phase_n) / 20), the phase advancing by freq / fs from 0 (sfizz LFO.cpp), after a delay with no phase advance.
  for (int wave = 0; wave <= 7; ++wave) {
    Instrument ins;
    auto& r = ins.dc(0.5, 60, 60);
    r.mod.amp = {true, static_cast<std::uint8_t>(wave), 6.0f, 37.0f, 0.0f, 0.0f};
    SampleEngine e(cfg());
    e.set_instrument(ins);
    CHECK(e.unplayable_regions() == 0);
    press(e, 60);
    const Out o = render(e, 2400);
    double worst = 0;
    double phase = 0;
    for (std::size_t n = 4; n < o.l.size(); ++n) {  // the first frames hold the envelope's attack
      double ph = 0;
      for (std::size_t k = 0; k < n; ++k) ph = std::fmod(ph + 37.0 / 48000.0, 1.0);
      phase = ph;
      const double want = 0.5 * std::pow(10.0, 6.0 * lfo_ref(wave, phase) / 20.0);
      worst = std::max(worst, std::fabs(o.l[n] / unity() - want) / want);
    }
    CHECK(worst < 5e-4);
    if (worst >= 5e-4) std::printf("  wave %d worst relative error %g\n", wave, worst);
  }
  {  // delay: unity gain for ceil(fs * delay) frames, then the LFO from phase 0; fade: gain ramps 0 -> 1 over fade seconds from the end of the delay
    Instrument ins;
    auto& r = ins.dc(0.5, 60, 60);
    r.mod.amp = {true, 6 /* ramp: -1 at phase 0 */, 10.0f, 10.0f, 0.01f, 0.02f};
    SampleEngine e(cfg());
    e.set_instrument(ins);
    press(e, 60);
    const Out o = render(e, 4000);
    for (std::size_t n = 4; n < 480; ++n) CHECK_NEAR(o.l[n] / unity(), 0.5, 1e-5);   // delay 0.01 s = 480 frames
    double ph = 0, fade = 0;
    double worst = 0;
    for (std::size_t n = 480; n < o.l.size(); ++n) {
      const double y = lfo_ref(6, ph) * fade;
      const double want = 0.5 * std::pow(10.0, 10.0 * y / 20.0);
      worst = std::max(worst, std::fabs(o.l[n] / unity() - want) / want);
      ph = std::fmod(ph + 10.0 / 48000.0, 1.0);
      fade = std::min(1.0, fade + (1.0 / 48000.0) / 0.02);
    }
    CHECK(worst < 1e-3);
    CHECK(std::fabs(o.l[480] / unity() - 0.5) < 1e-5);  // the fade starts at 0
  }
}

void test_lfo_pitch_and_eg_pitch() {
  const double base = [] { Instrument ins; ramp(ins, 60); SampleEngine e(cfg()); e.set_instrument(ins); press(e, 60); return slope(render(e, 400), 100, 300); }();
  CHECK_NEAR(base, 1.0 / 20000.0, 1e-8);
  {  // a pulse LFO (square: 1 for the first half period, then 0) of 1200 cents doubles the speed for half of each period, then it is the original again
    Instrument ins;
    ramp(ins, 60).mod.pitch = {true, 3, 1200.0f, 50.0f, 0.0f, 0.0f};  // 50 Hz: 960 frames per period, high for the first 480
    SampleEngine e(cfg());
    e.set_instrument(ins);
    CHECK(e.unplayable_regions() == 0);
    press(e, 60);
    const Out o = render(e, 1200);
    CHECK_NEAR(slope(o, 100, 400) / base, 2.0, 1e-3);
    CHECK_NEAR(slope(o, 600, 900) / base, 1.0, 1e-3);
  }
  {  // the pitch EG: sustain 100 %, no attack time: depth 1200 doubles the speed from the first frames; velocity depth adds
    Instrument ins;
    auto& r = ramp(ins, 60);
    r.mod.pitch_eg = {true, 0, 0, 0, 0, 0, 0, 100.0f, 1200.0f, 0.0f};
    SampleEngine e(cfg());
    e.set_instrument(ins);
    press(e, 60);
    CHECK_NEAR(slope(render(e, 400), 100, 300) / base, 2.0, 1e-3);
    auto at_vel = [](bool eg_on) {  // vel2depth only: 1200 cents at velocity 127, scaled by the note velocity (vel7 64 -> 64 / 127)
      Instrument iv;
      auto& rv = ramp(iv, 60);
      if (eg_on) rv.mod.pitch_eg = {true, 0, 0, 0, 0, 0, 0, 100.0f, 0.0f, 1200.0f};
      SampleEngine ev(cfg());
      ev.set_instrument(iv);
      ev.start_voice(0, 60, 32768);
      return slope(render(ev, 400), 100, 300);
    };
    CHECK_NEAR(at_vel(true) / at_vel(false), std::pow(2.0, 64.0 / 127.0), 2e-3);
  }
  {  // the EG shape: attack 0.01 s (480 frames) ramps the factor from 1 to 2; the delay 0.005 s holds the start level (0)
    Instrument ins;
    auto& r = ramp(ins, 60);
    r.mod.pitch_eg = {true, 0.005f, 0.01f, 0, 0, 0, 0, 100.0f, 1200.0f, 0.0f};
    SampleEngine e(cfg());
    e.set_instrument(ins);
    press(e, 60);
    const Out o = render(e, 1200);
    CHECK_NEAR(slope(o, 100, 200) / base, 1.0, 1e-3);                  // in the delay (240 frames)
    const double mid = slope(o, 380, 480) / base;                      // inside the attack: between 1 and 2, rising
    CHECK(mid > 1.1 && mid < 1.9);
    CHECK_NEAR(slope(o, 900, 1100) / base, 2.0, 1e-3);                 // after it
  }
}

// The read position of the first sounding voice slot (the playhead, independent of the envelope's gain).
double playhead(const SampleEngine& e) {
  for (std::uint32_t k = 0; k < 32; ++k)
    if (e.voice_position(k) > 0.0) return e.voice_position(k);
  return 0.0;
}

void test_eg_follows_the_amplitude_release() {
  Instrument ins;
  auto& r = ramp(ins, 60);
  r.mod.pitch_eg = {true, 0, 0, 0, 0, 0.4f, 0, 100.0f, 1200.0f, 0.0f};  // release 0.4 s
  r.ampeg_release = 0.5f;
  SampleEngine e(cfg());
  e.set_instrument(ins);
  press(e, 60);
  render(e, 600);
  const double p0 = playhead(e);
  render(e, 100);
  CHECK_NEAR((playhead(e) - p0) / 100.0, 2.0, 2e-3);  // held: the factor is 2
  lift(e, 60);
  render(e, 10);
  const double q0 = playhead(e);
  render(e, 100);
  const double early = (playhead(e) - q0) / 100.0;
  render(e, 700);
  const double r0 = playhead(e);
  render(e, 100);
  const double late = (playhead(e) - r0) / 100.0;
  // the EG falls as exp(-9 t / release) from 1: the speed factor 2^eg goes from 2 towards 1
  CHECK(early > 1.9 && early <= 2.0 + 1e-3);
  CHECK(late < early && late > 1.0);
  CHECK_NEAR(late, std::pow(2.0, std::exp(-9.0 * 810.0 / 48000.0 / 0.4)), 2e-2);
}

void test_filter_cutoff_lfo_and_eg() {
  auto rms = [](const Out& o, std::size_t a, std::size_t b) {
    double acc = 0;
    for (std::size_t i = a; i < b; ++i) acc += static_cast<double>(o.l[i]) * o.l[i];
    return std::sqrt(2.0 * acc / static_cast<double>(b - a)) / unity() / 0.5;
  };
  {  // filter EG: cutoff 500 Hz opened by 2400 cents over 0.1 s attack: starts at 500, settles at 2000
    Instrument ins;
    auto& r = ins.sine(1000.0, 60);
    lpf(r, 500.0);
    r.mod.fil_eg = {true, 0, 0.1f, 0, 0, 0, 0, 100.0f, 2400.0f, 0.0f};
    SampleEngine e(cfg());
    e.set_instrument(ins);
    CHECK(e.unplayable_regions() == 0);
    press(e, 60);
    const Out o = render(e, 48000);
    CHECK_NEAR(rms(o, 600, 840), lp_gain(500.0 * std::pow(2.0, 2400.0 * (720.0 / 4800.0) / 1200.0), 0.0, 1000.0), 0.03);  // early in the attack: the EG is at 720 / 4800
    CHECK_NEAR(rms(o, 40000, 48000), lp_gain(2000.0, 0.0, 1000.0), 3e-3);
    CHECK(rms(o, 40000, 48000) > 1.05 * rms(o, 600, 840));
  }
  {  // filter LFO: a square wave of 2 Hz moves the cutoff between 500 x 4 (first half period, wave = 1) and 500 (second half)
    Instrument ins;
    auto& r = ins.sine(1000.0, 60);
    lpf(r, 500.0);
    r.mod.fil = {true, 3, 1200.0f, 2.0f, 0.0f, 0.0f};
    SampleEngine e(cfg());
    e.set_instrument(ins);
    press(e, 60);
    const Out o = render(e, 48000);
    CHECK_NEAR(rms(o, 3000, 11000), lp_gain(1000.0, 0.0, 1000.0), 3e-3);
    CHECK_NEAR(rms(o, 15000, 23000), lp_gain(500.0, 0.0, 1000.0), 3e-3);
  }
  {  // with the cutoff_oncc entry of 13.8 and the LFO both: the cents add before the one conversion
    Instrument ins;
    auto& r = ins.sine(1000.0, 60);
    lpf(r, 500.0);
    r.filter_tune.cutoff[0] = ports::SampleFilterMod{74, 0, 1200.0f};
    r.filter_tune.cutoff_count = 1;
    r.mod.fil = {true, 3, 1200.0f, 2.0f, 0.0f, 0.0f};
    SampleEngine e(cfg());
    e.set_instrument(ins);
    e.set_part_cc(0, 74, cc32(127.0));
    press(e, 60);
    const Out o = render(e, 48000);
    CHECK_NEAR(rms(o, 3000, 11000), lp_gain(500.0 * 4.0, 0.0, 1000.0), 3e-3);
    CHECK_NEAR(rms(o, 15000, 23000), lp_gain(1000.0, 0.0, 1000.0), 3e-3);
  }
  {  // a filter LFO or EG on a region without a filter does nothing (sfizz has no filter to move)
    Instrument ins;
    auto& r = ins.sine(1000.0, 60);
    r.mod.fil = {true, 3, 1200.0f, 2.0f, 0.0f, 0.0f};
    r.mod.fil_eg = {true, 0, 0.1f, 0, 0, 0, 0, 100.0f, 2400.0f, 0.0f};
    Instrument plain;
    plain.sine(1000.0, 60);
    SampleEngine a(cfg()), b(cfg());
    a.set_instrument(ins);
    b.set_instrument(plain);
    press(a, 60);
    press(b, 60);
    const Out oa = render(a, 4800), ob = render(b, 4800);
    CHECK(oa.l == ob.l && oa.r == ob.r);
  }
}

// An instrument with all of it: three LFOs, two EGs, three bands, a filter, stereo and mono samples.
std::unique_ptr<Instrument> everything() {
  auto ins = std::make_unique<Instrument>();
  {
    auto& r = ins->sine(440.0, 60);
    lpf(r, 1500.0, 3.0);
    r.mod.amp = {true, 1, 3.0f, 6.0f, 0.01f, 0.1f};
    r.mod.pitch = {true, 0, 50.0f, 5.0f, 0.0f, 0.0f};
    r.mod.fil = {true, 3, 1200.0f, 3.0f, 0.0f, 0.0f};
    r.mod.pitch_eg = {true, 0.001f, 0.05f, 0.01f, 0.2f, 0.3f, 10.0f, 50.0f, 300.0f, 100.0f};
    r.mod.fil_eg = {true, 0, 0.1f, 0, 0.3f, 0.2f, 0, 40.0f, 1200.0f, 0.0f};
    r.mod.eq_count = 3;
    r.mod.eq[0].freq = 100.0f; r.mod.eq[0].gain = -6.0f; r.mod.eq[0].type = 2;
    r.mod.eq[1].freq = 1200.0f; r.mod.eq[1].gain = 4.0f; r.mod.eq[1].vel2gain = 3.0f;
    r.mod.eq[2].freq = 5000.0f; r.mod.eq[2].gain = 2.0f; r.mod.eq[2].type = 3;
    r.mod.eq[2].gain_count = 1; r.mod.eq[2].gain_mods[0] = ports::SampleFilterMod{1, 0, 5.0f};
  }
  {
    std::vector<float> l(9600), rr(9600);
    for (std::size_t i = 0; i < l.size(); ++i) { l[i] = 0.4f * static_cast<float>(std::sin(2 * kPi * 330.0 * static_cast<double>(i) / 48000.0)); rr[i] = 0.3f * static_cast<float>(std::sin(2 * kPi * 495.0 * static_cast<double>(i) / 48000.0)); }
    ports::SampleRegionDesc d;
    d.sample_index = ins->sample(std::move(l), std::move(rr));
    d.key_lo = d.key_hi = d.key_center = 64;
    d.loop_mode = ports::SampleLoopMode::kLoopContinuous;
    d.mod.amp = {true, 2, 2.0f, 7.0f, 0.0f, 0.0f};
    d.mod.pitch = {true, 1, 20.0f, 4.0f, 0.0f, 0.0f};
    d.mod.eq_count = 1; d.mod.eq[0].freq = 800.0f; d.mod.eq[0].gain = 5.0f;
    ins->regions.push_back(d);
  }
  return ins;
}

void test_mod_segmentation_polyphony_and_determinism() {
  auto run = [](std::uint32_t seg) {
    auto ins = everything();
    SampleEngine e(cfg());
    e.set_instrument(*ins);
    CHECK(e.unplayable_regions() == 0);
    press(e, 60);
    Out a = render(e, 3000, seg);
    press(e, 64);
    Out b = render(e, 12000, seg);
    lift(e, 60);
    Out c = render(e, 12000, seg);
    a.l.insert(a.l.end(), b.l.begin(), b.l.end());
    a.l.insert(a.l.end(), c.l.begin(), c.l.end());
    a.r.insert(a.r.end(), b.r.begin(), b.r.end());
    a.r.insert(a.r.end(), c.r.begin(), c.r.end());
    return a;
  };
  const Out x = run(37), y = run(480), z = run(1), w = run(37);
  CHECK(x.l == y.l && x.r == y.r && x.l == z.l && x.r == z.r);  // voice-local clocks: the host block size cannot change the sound
  CHECK(x.l == w.l && x.r == w.r);                               // deterministic
  double peak = 0;
  for (float v : x.l) { CHECK(std::isfinite(v)); peak = std::max(peak, static_cast<double>(std::fabs(v))); }
  CHECK(peak > 0.05 && peak < 4.0);
  // per-voice state: a note started later plays exactly what the same note plays alone, shifted
  auto solo = [](int key, std::uint32_t lead) {
    auto ins = everything();
    SampleEngine e(cfg());
    e.set_instrument(*ins);
    render(e, lead);
    press(e, key);
    return render(e, 4000);
  };
  auto both = [] {
    auto ins = everything();
    SampleEngine e(cfg());
    e.set_instrument(*ins);
    press(e, 60);
    render(e, 1000);
    press(e, 64);
    return render(e, 4000);
  };
  const Out s60 = [] { auto ins = everything(); SampleEngine e(cfg()); e.set_instrument(*ins); press(e, 60); render(e, 1000); return render(e, 4000); }();
  const Out s64 = solo(64, 0), mix = both();
  double worst = 0;
  for (std::size_t i = 0; i < mix.l.size(); ++i) worst = std::max(worst, static_cast<double>(std::fabs(mix.l[i] - s60.l[i] - s64.l[i])));
  CHECK(worst < 1e-5);
}

void test_mod_invalid_fields_are_unplayable() {
  auto unplayable = [](auto mutate) {
    Instrument ins;
    auto& r = ins.sine(440.0, 60);
    lpf(r, 1000.0);
    mutate(r);
    SampleEngine e(cfg());
    e.set_instrument(ins);
    return e.unplayable_regions() == 1;
  };
  CHECK(!unplayable([](auto& r) { r.mod.amp = {true, 1, 2.0f, 5.0f, 0.0f, 0.0f}; }));
  CHECK(unplayable([](auto& r) { r.mod.amp = {true, 1, 11.0f, 5.0f, 0.0f, 0.0f}; }));
  CHECK(unplayable([](auto& r) { r.mod.amp = {true, 8, 1.0f, 5.0f, 0.0f, 0.0f}; }));
  CHECK(unplayable([](auto& r) { r.mod.pitch = {true, 1, std::nanf(""), 5.0f, 0.0f, 0.0f}; }));
  CHECK(unplayable([](auto& r) { r.mod.pitch = {true, 1, 10.0f, std::numeric_limits<float>::infinity(), 0.0f, 0.0f}; }));
  CHECK(unplayable([](auto& r) { r.mod.fil = {true, 1, 10.0f, 5.0f, 31.0f, 0.0f}; }));
  CHECK(unplayable([](auto& r) { r.mod.pitch_eg = {true, 0, 101.0f, 0, 0, 0, 0, 100.0f, 10.0f, 0.0f}; }));
  CHECK(unplayable([](auto& r) { r.mod.fil_eg = {true, 0, 0, 0, 0, 0, 0, 100.0f, 12001.0f, 0.0f}; }));
  CHECK(unplayable([](auto& r) { r.mod.eq_count = 4; }));
  CHECK(unplayable([](auto& r) { r.mod.eq_count = 1; r.mod.eq[0].type = 4; }));
  CHECK(unplayable([](auto& r) { r.mod.eq_count = 1; r.mod.eq[0].bw = 0.0f; }));
  CHECK(unplayable([](auto& r) { r.mod.eq_count = 1; r.mod.eq[0].bw = 0.005f; }));  // below what the DSP honours
  CHECK(unplayable([](auto& r) { r.mod.eq_count = 1; r.mod.eq[0].gain = 61.0f; }));
  CHECK(unplayable([](auto& r) { r.mod.amp = {true, 1, 0.0f, 5.0f, 0.0f, 0.0f}; }));  // on without a depth: not canonical in the format either
  CHECK(unplayable([](auto& r) { r.mod.pitch_eg = {true, 0, 0, 0, 0, 0, 0, 100.0f, 0.0f, 0.0f}; }));
  CHECK(!unplayable([](auto& r) { r.mod.eq_count = 1; r.mod.eq[0].bw = 0.01f; r.mod.eq[0].gain = 60.0f; }));
  CHECK(unplayable([](auto& r) { r.mod.eq_count = 1; r.mod.eq[0].gain = std::nanf(""); }));
  CHECK(unplayable([](auto& r) { r.mod.eq_count = 1; r.mod.eq[0].gain_count = 5; }));
  CHECK(unplayable([](auto& r) { r.mod.eq_count = 1; r.mod.eq[0].gain_count = 1; r.mod.eq[0].gain_mods[0] = ports::SampleFilterMod{200, 0, 1.0f}; }));
  // a pitch LFO whose depth would take the region beyond the supported ratio at its extreme key is unplayable
  CHECK(unplayable([](auto& r) { r.key_hi = 127; r.key_lo = 0; r.key_center = 0; r.mod.pitch = {true, 1, 1200.0f, 5.0f, 0.0f, 0.0f}; r.mod.pitch_eg = {true, 0, 0, 0, 0, 0, 0, 100.0f, 12000.0f, 0.0f}; }));
}

void test_mod_extremes_stay_finite() {
  Instrument ins;
  for (int k = 0; k < 4; ++k) {
    auto& r = ins.sine(300.0 * (k + 1), 60 + k);
    lpf(r, k == 0 ? 20.0 : 18000.0, k == 1 ? 60.0 : 0.0);
    r.mod.amp = {true, static_cast<std::uint8_t>(k), 10.0f, 100.0f, 0.0f, 0.0f};
    r.mod.pitch = {true, static_cast<std::uint8_t>(7 - k), 1200.0f, 100.0f, 0.0f, 0.0f};
    r.mod.fil = {true, 3, 1200.0f, 100.0f, 0.0f, 0.0f};
    r.mod.pitch_eg = {true, 0, 0, 0, 0, 0, 0, 100.0f, 2000.0f, 2000.0f};
    r.mod.fil_eg = {true, 0, 0, 0, 0, 0, 0, 100.0f, -12000.0f, -12000.0f};
    r.mod.eq_count = 3;
    for (unsigned b = 0; b < 3; ++b) { r.mod.eq[b].freq = b == 0 ? 0.0f : b == 1 ? 20000.0f : 1.0f; r.mod.eq[b].bw = b == 0 ? 0.01f : 4.0f; r.mod.eq[b].gain = (b % 2) ? -96.0f : 60.0f; r.mod.eq[b].type = static_cast<std::uint8_t>(1 + (b + static_cast<unsigned>(k)) % 3); }
  }
  SampleEngine e(cfg());
  e.set_instrument(ins);
  CHECK(e.unplayable_regions() == 0);
  for (int k = 60; k < 64; ++k) press(e, k);
  double peak = 0;
  for (int blk = 0; blk < 200; ++blk) {
    const Out o = render(e, 480);
    for (std::size_t i = 0; i < o.l.size(); ++i) { CHECK(std::isfinite(o.l[i]) && std::isfinite(o.r[i])); peak = std::max(peak, static_cast<double>(std::fabs(o.l[i]))); }
  }
  CHECK(peak < 1.0e9);
  for (int k = 60; k < 64; ++k) lift(e, k);
  settle(e);
  const Out tail = render(e, 480);
  for (float v : tail.l) CHECK(std::isfinite(v));
  // an eq coefficient set is finite for every clamp corner
  for (unsigned type = 1; type <= 3; ++type)
    for (double f : {-1.0, 0.0, 1.0, 20000.0, 1.0e9, std::numeric_limits<double>::quiet_NaN()})
      for (double bw : {0.0, 0.001, 4.0, 100.0})
        for (double g : {-500.0, -96.0, 96.0, 500.0}) {
          const SampleFilterCoeffs k = make_sample_eq(type, f, bw, g, 48000.0);
          CHECK(std::isfinite(k.b0) && std::isfinite(k.b1) && std::isfinite(k.b2) && std::isfinite(k.a1) && std::isfinite(k.a2));
        }
}

void test_mod_regions_without_the_opcodes_are_bit_identical() {
  // A plain region next to a modulated one renders what it rendered alone; a region that carries only default mod fields takes the old paths.
  auto render_pair = [](bool with_other) {
    Instrument ins;
    auto& a = ins.sine(700.0, 60);
    lpf(a, 900.0, 4.0);
    if (with_other) {
      auto& b = ins.sine(500.0, 62);
      b.mod.amp = {true, 1, 6.0f, 9.0f, 0.0f, 0.0f};
      b.mod.eq_count = 1; b.mod.eq[0].freq = 500.0f; b.mod.eq[0].gain = 6.0f;
    }
    SampleEngine e(cfg());
    e.set_instrument(ins);
    press(e, 60);
    return render(e, 9600);
  };
  const Out a = render_pair(false), b = render_pair(true);
  CHECK(a.l == b.l && a.r == b.r);
  // an LFO of depth 0 dB-equivalent (wave 2 pulse at its low half is 0) ... the amplitude path with a unity factor equals the plain voice up to float rounding of the extra multiply
  Instrument m, p;
  m.dc(0.5, 60, 60).mod.pitch = {true, 1, 1.0f, 0.0f, 0.0f, 0.0f};  // freq 0: the sine of phase 0 is 0 cents: unity
  p.dc(0.5, 60, 60);
  SampleEngine em(cfg()), ep(cfg());
  em.set_instrument(m);
  ep.set_instrument(p);
  press(em, 60);
  press(ep, 60);
  const Out om = render(em, 2000), op = render(ep, 2000);
  CHECK(om.l == op.l && om.r == op.r);
}

int main() {
  test_sw_last_default_and_range();
  test_sw_without_default_and_mapped_switch_key();
  test_sw_down_up_previous();
  test_sw_combinations_and_gating();
  test_sw_state_per_part_and_resets();
  test_crossfade_curves_at_anchors();
  test_crossfade_follows_controller();
  test_crossfade_sounding_voices_follow_without_steps();
  test_crossfade_new_voice_starts_at_the_current_gain();
  test_crossfade_two_controllers_and_isolation();
  test_segmentation_does_not_change_the_render();
  test_filter_response_at_anchors();
  test_filter_in_the_voice_matches_the_analytic_response();
  test_filter_is_stable_for_any_cutoff();
  test_filter_stereo_mono_and_bypass();
  test_cost_declaration();
  test_cc_path_and_fan_out_through_the_block_loop();
  test_live_set_switch_resets_keyswitches_and_keeps_controllers();
  test_pedal_defers_stop_but_not_key_up();
  test_held_keys_follow_physical_note_offs_only();
  test_bank_identity_is_a_token_not_an_address();
  test_invalid_extensions();
  test_filter_state_flushes_denormals();
  test_mapped_cc_is_a_raw_controller_only_when_midi();
  test_replay_without_epoch_and_stale_clear();
  test_pedal_up_does_not_free_a_held_switch_key();
  test_filter_keytrack_and_veltrack();
  test_filter_cc_entries();
  test_filter_pseudo_cc_entries();
  test_filter_second_filter();
  test_filter_bit_identity_without_new_fields();
  test_filter_per_voice_state_and_polyphony();
  test_filter_extremes_nan_and_subnormal();
  test_filter_invalid_fields_are_unplayable();
  test_filter_deterministic_with_random_pseudo_cc();
  test_mod_cost_declaration();
  test_mod_counts_the_missing_extended_declaration();
  test_eq_band_response();
  test_eq_velocity_and_controllers();
  test_lfo_amplitude_waves_delay_fade();
  test_lfo_pitch_and_eg_pitch();
  test_eg_follows_the_amplitude_release();
  test_mod_egs_follow_every_release();
  test_filter_cutoff_lfo_and_eg();
  test_mod_segmentation_polyphony_and_determinism();
  test_mod_invalid_fields_are_unplayable();
  test_mod_extremes_stay_finite();
  test_mod_regions_without_the_opcodes_are_bit_identical();
  if (g_fail) {
    std::printf("sample_features_test: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("sample_features_test: ok\n");
  return 0;
}
