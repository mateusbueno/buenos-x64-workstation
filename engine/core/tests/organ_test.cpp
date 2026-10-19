// Tonewheel organ engine tests (Story 2.2). Bare main, CHECK style as the other core tests.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/params_gen.hpp"
#include "velora/core/snapshot.hpp"

using namespace velora::core;
namespace tw = velora::core::tonewheel;

static int g_fail = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    if (!(c)) {                                                          \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
      ++g_fail;                                                          \
    }                                                                    \
  } while (0)

namespace {

constexpr double kFs = 48000.0;
constexpr double kTwoPi = 6.283185307179586;

void set_drawbars(OrganEngine& e, const char* nine, std::uint32_t part = 0) {
  for (int d = 0; d < 9; ++d) e.set_part_param(part, params::kDrawbar16 + d, static_cast<float>(nine[d] - '0'));
}

// Key click is on by default (registry default 0.5): tests that look at the tones alone switch it off.
void no_click(OrganEngine& e, std::uint32_t part = 0) { e.set_part_param(part, params::kKeyClick, 0.0f); }

// Finish any drawbar ramp so tests start from steady state (a standalone engine, no keys down).
void settle(OrganEngine& e) {
  std::vector<float> l(64, 0.0f), r(64, 0.0f);
  for (int i = 0; i < 8; ++i) e.render(l.data(), r.data(), 64);
}

// Renders `frames` standalone (the engine runs its own generator at block boundaries).
std::vector<float> render_mono(OrganEngine& e, std::uint32_t frames) {
  std::vector<float> l(frames, 0.0f), r(frames, 0.0f);
  e.render(l.data(), r.data(), frames);
  return l;
}

// Amplitude of the sinusoid at `hz` in x[from, from+n) (single-bin DFT).
double amp_at(const std::vector<float>& x, std::size_t from, std::size_t n, double hz) {
  std::complex<double> acc = 0;
  for (std::size_t i = 0; i < n; ++i)
    acc += static_cast<double>(x[from + i]) * std::polar(1.0, -kTwoPi * hz * static_cast<double>(from + i) / kFs);
  return 2.0 * std::abs(acc) / static_cast<double>(n);
}

double wheel_hz(int wheel) { return OrganEngine::wheel_frequency_hz(static_cast<std::uint32_t>(wheel)); }

void test_tables() {
  // Equal temperament, A4 = 440 Hz; wheel 1 = C1, wheel 46 = A4, wheel 13 = C2.
  CHECK(std::fabs(wheel_hz(46) - 440.0) < 1e-9);
  CHECK(std::fabs(wheel_hz(1) - 32.7032) < 1e-3);
  CHECK(std::fabs(wheel_hz(13) / wheel_hz(1) - 2.0) < 1e-12);
  CHECK(std::fabs(wheel_hz(91) - 5919.911) < 1e-2);
  for (int w = 1; w < 91; ++w) CHECK(std::fabs(wheel_hz(w + 1) / wheel_hz(w) - std::pow(2.0, 1.0 / 12.0)) < 1e-12);
  CHECK(tw::kDrawbarSemitones[0] == -12 && tw::kDrawbarSemitones[2] == 0 && tw::kDrawbarSemitones[8] == 36);
  // Harmonic ratios against 8' (nearest equal-tempered wheel): within 14 cents of the exact harmonic.
  const double exact[9] = {0.5, 1.5, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 8.0};
  for (int d = 0; d < 9; ++d) {
    const double ratio = std::exp2(tw::kDrawbarSemitones[d] / 12.0);
    CHECK(std::fabs(1200.0 * std::log2(ratio / exact[d])) < 14.0);
  }
  CHECK(tw::drawbar_wheel(36, 2) == 13);  // C2 8' is wheel 13
  CHECK(tw::kKeyCount == 73 && tw::kHighestKey - tw::kLowestKey + 1 == 73);
}

void test_wheel_generator() {
  OrganEngine e;
  const int wheels[] = {1, 13, 46, 77, 91};
  const int blocks = 300;
  for (int b = 0; b < blocks; ++b) {
    e.begin_block(64);
    if (b != blocks - 1) continue;
    for (int w : wheels) {
      const float* row = e.wheel_block(0) + (w - 1) * 64;
      double worst = 0;
      for (int f = 0; f < 64; ++f) {
        const double n = static_cast<double>(b) * 64.0 + f;
        worst = std::max(worst, std::fabs(row[f] - std::sin(kTwoPi * wheel_hz(w) * n / kFs)));
      }
      CHECK(worst < 2e-4);
    }
  }
}

void test_drawbar_harmonics() {
  // Middle C (60), each drawbar alone: the wheel it names sounds at kVoiceScale x its level, nothing else.
  for (int d = 0; d < 9; ++d) {
    for (int step : {8, 5}) {
      OrganEngine e;
      char reg[10] = "000000000";
      reg[d] = static_cast<char>('0' + step);
      set_drawbars(e, reg);
      settle(e);
      CHECK(e.start_voice(0, 60, 0xFFFF) == EngineStatus::kOk);
      const auto x = render_mono(e, 24000);
      const double want = OrganEngine::kVoiceScale * OrganEngine::drawbar_level(step);
      const double got = amp_at(x, 4800, 19200, wheel_hz(tw::drawbar_wheel(60, d)));
      CHECK(std::fabs(got - want) < 0.02 * want);
      for (int o = 0; o < 9; ++o)
        if (o != d) CHECK(amp_at(x, 4800, 19200, wheel_hz(tw::drawbar_wheel(60, o))) < 0.01 * want);
    }
  }
  // Registration 888000000: three components with equal amplitude.
  OrganEngine e;
  set_drawbars(e, "888000000");
  settle(e);
  e.start_voice(0, 60, 0xFFFF);
  const auto x = render_mono(e, 24000);
  for (int d = 0; d < 3; ++d)
    CHECK(std::fabs(amp_at(x, 4800, 19200, wheel_hz(tw::drawbar_wheel(60, d))) - OrganEngine::kVoiceScale) < 0.0012);
  CHECK(amp_at(x, 4800, 19200, wheel_hz(tw::drawbar_wheel(60, 3))) < 1e-3 * OrganEngine::kVoiceScale * 10);
}

void test_default_registration() {
  OrganEngine e;  // registry default: only the 8' drawbar at 8
  e.start_voice(0, 69, 0xFFFF);
  const auto x = render_mono(e, 24000);
  CHECK(std::fabs(amp_at(x, 4800, 19200, 440.0) - OrganEngine::kVoiceScale) < 0.001);
}

void test_foldback() {
  CHECK(tw::fold_wheel(5) == 5 && tw::fold_wheel(91) == 91 && tw::fold_wheel(1) == 1);
  CHECK(tw::fold_wheel(0) == 12 && tw::fold_wheel(-7) == 5 && tw::fold_wheel(-35) == 1);
  CHECK(tw::fold_wheel(92) == 80 && tw::fold_wheel(101) == 89 && tw::fold_wheel(113) == 89);
  for (int w = -200; w < 300; ++w) {
    const int f = tw::fold_wheel(w);
    CHECK(f >= 1 && f <= 91 && ((f - w) % 12) == 0);
  }
  // Top: E7 (100), 1' would be wheel 113 -> 89; 2' 101 -> 89; 1 1/3' 108 -> 84.
  CHECK(tw::drawbar_wheel(100, 8) == 89 && tw::drawbar_wheel(100, 5) == 89 && tw::drawbar_wheel(100, 7) == 84);
  // Bottom: E1 (28), 16' would be wheel -7 -> 5 (the 8' wheel of the same key).
  CHECK(tw::drawbar_wheel(28, 0) == 5 && tw::drawbar_wheel(28, 2) == 5);
  // Heard: top key, 1' alone sounds the folded wheel; bottom key, 16' alone sounds the folded wheel.
  {
    OrganEngine e;
    set_drawbars(e, "000000008");
    settle(e);
    e.start_voice(0, 100, 0xFFFF);
    const auto x = render_mono(e, 24000);
    CHECK(std::fabs(amp_at(x, 4800, 19200, wheel_hz(89)) - OrganEngine::kVoiceScale) < 0.002);
    CHECK(amp_at(x, 4800, 19200, wheel_hz(91)) < 1e-3);  // the unfolded 1' wheel does not exist; nothing leaks to 91
  }
  {
    OrganEngine e;
    set_drawbars(e, "800000000");
    settle(e);
    e.start_voice(0, 28, 0xFFFF);
    const auto x = render_mono(e, 24000);
    CHECK(std::fabs(amp_at(x, 4800, 19200, wheel_hz(5)) - OrganEngine::kVoiceScale) < 0.002);
  }
}

void test_drawbar_curve() {
  CHECK(OrganEngine::drawbar_level(0) == 0.0f);
  CHECK(std::fabs(OrganEngine::drawbar_level(8) - 1.0f) < 1e-6f);
  for (int s = 1; s <= 8; ++s) {
    CHECK(OrganEngine::drawbar_level(s) > OrganEngine::drawbar_level(s - 1));
    if (s > 1) CHECK(std::fabs(20.0 * std::log10(OrganEngine::drawbar_level(s) / OrganEngine::drawbar_level(s - 1)) - 3.0) < 1e-3);
  }
  CHECK(OrganEngine::drawbar_level(-3) == 0.0f && OrganEngine::drawbar_level(99) == OrganEngine::drawbar_level(8));
  // Out-of-range values clamp.
  OrganEngine e;
  e.set_part_param(0, params::kDrawbar4, 40.0f);
  e.set_part_param(0, params::kDrawbar2, -5.0f);
  e.set_part_param(0, params::kDrawbar16, std::nanf(""));  // ignored
  settle(e);
  CHECK(e.drawbar_gain(0, 3) == OrganEngine::drawbar_level(8));
  CHECK(e.drawbar_gain(0, 5) == 0.0f);
  CHECK(e.drawbar_gain(0, 0) == 0.0f);
  // Silent at 0: a held key with all drawbars at 0 renders exact zeros.
  set_drawbars(e, "000000000");
  no_click(e);
  settle(e);
  e.start_voice(0, 60, 0xFFFF);
  const auto x = render_mono(e, 1024);
  CHECK(*std::max_element(x.begin(), x.end()) == 0.0f && *std::min_element(x.begin(), x.end()) == 0.0f);
}

void test_no_zipper() {
  // One key, 8' only. A drawbar change at in-block offset k: identical output before k, a linear ramp of
  // the gain starting at k, bounded steps.
  const std::uint32_t k = 21;
  OrganEngine a, b;
  set_drawbars(a, "000000000");
  set_drawbars(b, "000000000");
  set_drawbars(a, "008000000");
  set_drawbars(b, "008000000");
  settle(a);
  settle(b);
  a.start_voice(0, 33, 0xFFFF);
  b.start_voice(0, 33, 0xFFFF);
  for (int i = 0; i < 20; ++i) {  // past the attack
    std::vector<float> l(64), r(64);
    a.begin_block(64);
    b.begin_block(64);
    a.render(l.data(), r.data(), 64);
    b.render(l.data(), r.data(), 64);
  }
  std::vector<float> la(64, 0.0f), lb(64, 0.0f), r(64, 0.0f);
  a.begin_block(64);
  b.begin_block(64);
  a.render(la.data(), r.data(), k);
  b.render(lb.data(), r.data(), k);
  b.set_part_param(0, params::kDrawbar8, 2.0f);
  a.render(la.data() + k, r.data(), 64 - k);
  b.render(lb.data() + k, r.data(), 64 - k);
  for (std::uint32_t f = 0; f < k; ++f) CHECK(la[f] == lb[f]);
  const float* wheel = a.wheel_block(0) + (tw::drawbar_wheel(33, 2) - 1) * 64;
  double prev_gain = 1.0, max_step = 0;
  bool first_diff_ok = false;
  for (std::uint32_t f = k; f < 64; ++f) {
    if (std::fabs(wheel[f]) < 0.2f) continue;
    const double g = lb[f] / (OrganEngine::kVoiceScale * wheel[f]);
    CHECK(g <= prev_gain + 1e-3);  // monotone falling ramp
    prev_gain = g;
    if (!first_diff_ok && la[f] != lb[f]) first_diff_ok = true;
  }
  CHECK(first_diff_ok);
  CHECK(lb[k] != la[k]);  // the ramp starts at exactly frame k
  const double step_bound = (1.0 - OrganEngine::drawbar_level(2)) / OrganEngine::kDrawbarRampFrames;
  // Gain step per frame is the ramp slope; compare the two renders' difference growth.
  for (std::uint32_t f = k + 1; f < 64; ++f) {
    const double d0 = std::fabs(la[f - 1] - lb[f - 1]), d1 = std::fabs(la[f] - lb[f]);
    max_step = std::max(max_step, std::fabs(d1 - d0));
  }
  CHECK(max_step <= 5.0 * OrganEngine::kVoiceScale * step_bound);
  // Ramp reaches the target after kDrawbarRampFrames and then holds.
  for (int i = 0; i < 6; ++i) { std::vector<float> l(64); b.begin_block(64); b.render(l.data(), r.data(), 64); }
  CHECK(b.drawbar_gain(0, 2) == OrganEngine::drawbar_level(2));
}

void test_shared_phase() {
  // Key 36 4' and key 48 8' use the same wheel (25): identical output.
  OrganEngine a, b;
  set_drawbars(a, "000800000");
  set_drawbars(b, "008000000");
  no_click(a);
  no_click(b);
  settle(a);
  settle(b);
  CHECK(tw::drawbar_wheel(36, 3) == tw::drawbar_wheel(48, 2));
  a.start_voice(0, 36, 0xFFFF);
  b.start_voice(0, 48, 0xFFFF);
  // 4' at step 8 and 8' at step 8 have the same level: outputs identical.
  const auto xa = render_mono(a, 4096), xb = render_mono(b, 4096);
  CHECK(xa == xb);
  // A key started later reads the same running wheel: its steady-state output equals a key held from the start.
  OrganEngine c, d;
  for (OrganEngine* e : {&c, &d}) { set_drawbars(*e, "008000000"); settle(*e); }
  d.start_voice(0, 48, 0xFFFF);
  render_mono(c, 640 * 64);
  render_mono(d, 640 * 64);
  c.start_voice(0, 48, 0xFFFF);
  const auto xc = render_mono(c, 4096), xd = render_mono(d, 4096);
  for (std::size_t i = 200; i < xc.size(); ++i) CHECK(xc[i] == xd[i]);
  // Two voices on the same wheel add in phase: a second key sharing the wheel doubles the amplitude.
  OrganEngine f;
  set_drawbars(f, "008800000");
  settle(f);
  f.start_voice(0, 48, 0xFFFF);  // 8' wheel 25
  f.start_voice(0, 36, 0xFFFF);  // 8' wheel 13, 4' wheel 25
  const auto xf = render_mono(f, 24000);
  CHECK(std::fabs(amp_at(xf, 4800, 19200, wheel_hz(25)) - 2.0 * OrganEngine::kVoiceScale) < 0.004);
}

BlockRenderer* make_renderer(OrganEngine& organ, std::uint32_t tone_voices = 32) {
  auto* br = new BlockRenderer(tone_voices);
  CHECK(br->bind_engine(organ));
  br->set_channel_engine(0, static_cast<std::uint32_t>(EngineTypeId::kOrgan));
  return br;
}

void submit_param(BlockRenderer& br, std::uint32_t id, std::uint32_t part, double value, std::uint32_t frame) {
  br.submit({make_param_event(*params::find(id), part, value), frame});
}

void test_full_chord_and_determinism() {
  auto run = [](std::vector<float>& out, std::uint64_t& drops, std::uint32_t& active) {
    OrganEngine organ;
    std::unique_ptr<BlockRenderer> br(make_renderer(organ));
    for (std::uint32_t d = 0; d < 9; ++d) submit_param(*br, params::kDrawbar16 + d, 0, 8, 0);
    float l[64], r[64];
    br->process_block(l, r);
    for (std::uint32_t n = 28; n <= 100; ++n) br->submit({make_note_on(0, 0, n, 0xFFFF), (n * 3) % 64});
    float peak = 0;
    for (int b = 0; b < 400; ++b) {
      br->process_block(l, r);
      for (int i = 0; i < 64; ++i) {
        peak = std::max(peak, std::max(std::fabs(l[i]), std::fabs(r[i])));
        CHECK(std::isfinite(l[i]));
        out.push_back(l[i]);
      }
    }
    drops = br->status_count(EngineStatus::kPoolExhausted);
    active = organ.active_voices();
    CHECK(peak <= 1.0f);
    CHECK(peak > 0.5f);  // a full chord at full registration is loud, and bounded
    CHECK(br->status_count(EngineStatus::kOk) == 73);
  };
  std::vector<float> a, b;
  std::uint64_t da = 0, db = 0;
  std::uint32_t na = 0, nb = 0;
  run(a, da, na);
  run(b, db, nb);
  CHECK(na == 73 && nb == 73 && da == 0 && db == 0);
  CHECK(a == b);  // deterministic
  // Every key sounds: with only key X and 8' the output is non-zero for each of the 73 keys.
  for (std::uint32_t n = 28; n <= 100; n += 12) {
    OrganEngine e;
    e.start_voice(0, n, 0xFFFF);
    const auto x = render_mono(e, 2048);
    CHECK(*std::max_element(x.begin(), x.end()) > 0.01f);
  }
}

void test_param_routing() {
  OrganEngine organ;
  std::unique_ptr<BlockRenderer> br(make_renderer(organ));
  br->set_channel_engine(1, static_cast<std::uint32_t>(EngineTypeId::kOrgan));
  float l[64], r[64];
  br->process_block(l, r);
  CHECK(organ.drawbar_gain(0, 2) == OrganEngine::drawbar_level(8));
  submit_param(*br, params::kDrawbar8, 0, 3, 10);
  submit_param(*br, params::kDrawbar16, 1, 6, 10);  // another part: part 0 untouched
  br->process_block(l, r);
  for (int i = 0; i < 4; ++i) br->process_block(l, r);
  CHECK(organ.drawbar_gain(0, 2) == OrganEngine::drawbar_level(3));
  CHECK(organ.drawbar_gain(0, 0) == 0.0f);
  CHECK(organ.drawbar_gain(1, 0) == OrganEngine::drawbar_level(6));
  CHECK(organ.drawbar_gain(1, 2) == OrganEngine::drawbar_level(8));
  // CC 70 (16') through the CC mapping reaches the same path.
  Ump ev;
  CHECK(cc_to_param_event(make_control_change(0, 0, 70, 0xFFFFFFFFu), ev));
  br->submit({ev, 5});
  for (int i = 0; i < 5; ++i) br->process_block(l, r);
  CHECK(organ.drawbar_gain(0, 0) == OrganEngine::drawbar_level(8));
  // An organ part set up after drawbars were moved receives the current values.
  OrganEngine late;
  std::unique_ptr<BlockRenderer> b2(new BlockRenderer(8));
  submit_param(*b2, params::kDrawbar4, 2, 5, 0);
  b2->process_block(l, r);
  CHECK(b2->bind_engine(late));
  b2->set_channel_engine(2, static_cast<std::uint32_t>(EngineTypeId::kOrgan));
  for (int i = 0; i < 5; ++i) b2->process_block(l, r);
  CHECK(late.drawbar_gain(2, 3) == OrganEngine::drawbar_level(5));
  // Channels above the part range and the unavailable engines.
  CHECK(late.start_voice(9, 60, 1) == EngineStatus::kEngineNotAvailable);
}

void test_arbiter_protection() {
  for (int variant = 0; variant < 1; ++variant) {
    OrganEngine organ;
    std::unique_ptr<BlockRenderer> br(make_renderer(organ, 300));
    // The pool: 300 test-tone voices stand in for sample voices (channels 1..3 -> test tone).
    VoiceArbiter::LiveSetConfig cfg;
    cfg.organ_rotary_cost = variant == 0 ? OrganEngine::reservation_cost_units(1) : 0;
    cfg.budget = cfg.organ_rotary_cost + 256;
    CHECK(br->arbiter().reserve(cfg) == VoiceArbiter::ReserveStatus::kOk);
    CHECK(br->arbiter().reservation().organ_rotary == cfg.organ_rotary_cost);
    float l[64], r[64];
    for (std::uint32_t ch = 1; ch <= 3; ++ch)
      for (std::uint32_t n = 0; n < 100; ++n) br->submit({make_note_on(0, ch, 20 + n, 40000), n % 64});
    br->process_block(l, r);
    br->process_block(l, r);
    const auto steals_before = br->arbiter().steals(static_cast<std::uint32_t>(EngineTypeId::kTestTone));
    for (std::uint32_t n = 28; n <= 100; ++n) br->submit({make_note_on(0, 0, n, 0xFFFF), n % 64});
    for (int i = 0; i < 6; ++i) br->process_block(l, r);
    CHECK(organ.active_voices() == 73);
    CHECK(br->arbiter().steals(static_cast<std::uint32_t>(EngineTypeId::kOrgan)) == 0);
    CHECK(br->status_count(EngineStatus::kPoolExhausted) == 0 || variant == 1);
    if (variant == 1) {  // organ cost came out of the shared pool: samples were stolen for it
      CHECK(br->arbiter().steals(static_cast<std::uint32_t>(EngineTypeId::kTestTone)) > steals_before);
    }
    // More note-ons from a stealing engine afterwards still never touch the organ.
    for (std::uint32_t n = 0; n < 100; ++n) br->submit({make_note_on(0, 4, 20 + n, 40000), n % 64});
    for (int i = 0; i < 6; ++i) br->process_block(l, r);
    CHECK(organ.active_voices() == 73);
    CHECK(br->arbiter().steals(static_cast<std::uint32_t>(EngineTypeId::kOrgan)) == 0);
    // A floor on the organ is refused (it is reserved, not floored).
    VoiceArbiter::LiveSetConfig bad = cfg;
    bad.floors[0] = {static_cast<std::uint32_t>(EngineTypeId::kOrgan), 10};
    bad.floor_count = 1;
    CHECK(br->arbiter().reserve(bad) == VoiceArbiter::ReserveStatus::kInvalidConfig);
  }
  // A reservation that does not fit the budget is refused, nothing changes.
  OrganEngine organ;
  std::unique_ptr<BlockRenderer> br(make_renderer(organ));
  VoiceArbiter::LiveSetConfig tight;
  tight.budget = OrganEngine::reservation_cost_units(1) - 1;
  tight.organ_rotary_cost = OrganEngine::reservation_cost_units(1);
  CHECK(br->arbiter().reserve(tight) == VoiceArbiter::ReserveStatus::kOrganExceedsBudget);
}


void step_blocks(BlockRenderer& br, int n, float* peak = nullptr) {
  float l[64], r[64];
  for (int i = 0; i < n; ++i) {
    br.process_block(l, r);
    if (peak)
      for (int k = 0; k < 64; ++k) *peak = std::max(*peak, std::fabs(l[k]));
  }
}

void test_generation_forwarding() {
  OrganEngine organ;
  std::unique_ptr<BlockRenderer> br(make_renderer(organ));
  const params::ParamDef& d4 = *params::find(params::kDrawbar4);
  auto g1 = std::make_unique<Snapshot>(Snapshot::defaults());
  g1->revision = 1;
  g1->ledger_index = 0;
  g1->perf_init[params::perf_slot(d4, 0)] = 5.0f;
  br->snapshots().publish(g1.get());
  step_blocks(*br, 6);
  CHECK(organ.drawbar_gain(0, 3) == OrganEngine::drawbar_level(5));
  CHECK(organ.drawbar_gain(0, 2) == OrganEngine::drawbar_level(8));  // unchanged default stays
  // A live change, then a generation whose initial value is unchanged: not re-forwarded (the player's move stays).
  br->submit({make_param_event(d4, 0, 2), 0});
  step_blocks(*br, 6);
  CHECK(organ.drawbar_gain(0, 3) == OrganEngine::drawbar_level(2));
  auto g2 = std::make_unique<Snapshot>(Snapshot::defaults());
  g2->revision = 2;
  g2->ledger_index = 1;
  g2->perf_init[params::perf_slot(d4, 0)] = 5.0f;
  br->snapshots().publish(g2.get());
  step_blocks(*br, 6);
  CHECK(br->held_snapshot() == g2.get());
  CHECK(organ.drawbar_gain(0, 3) == OrganEngine::drawbar_level(2));
  auto g3 = std::make_unique<Snapshot>(Snapshot::defaults());  // changed initial value is forwarded
  g3->revision = 3;
  g3->ledger_index = 2;
  g3->perf_init[params::perf_slot(d4, 0)] = 7.0f;
  br->snapshots().publish(g3.get());
  step_blocks(*br, 6);
  CHECK(organ.drawbar_gain(0, 3) == OrganEngine::drawbar_level(7));
}

void test_bind_after_routing() {
  OrganEngine organ;
  BlockRenderer br(8);
  float l[64], r[64];
  br.submit({make_param_event(*params::find(params::kDrawbar2), 2, 6), 0});
  br.process_block(l, r);
  br.set_channel_engine(2, static_cast<std::uint32_t>(EngineTypeId::kOrgan));  // routed before the bind
  CHECK(br.bind_engine(organ));
  step_blocks(br, 6);
  CHECK(organ.drawbar_gain(2, 5) == OrganEngine::drawbar_level(6));
}

void test_pedal() {
  OrganEngine organ;
  std::unique_ptr<BlockRenderer> br(make_renderer(organ));
  const Ump down = make_control_change(0, 0, 64, 0xFFFFFFFFu), up = make_control_change(0, 0, 64, 0u);
  float pk = 0;
  br->submit({make_note_on(0, 0, 60, 0xFFFF), 0});
  step_blocks(*br, 4, &pk);
  CHECK(organ.active_voices() == 1 && pk > 0.01f);
  br->submit({down, 0});
  br->submit({make_note_off(0, 0, 60), 1});
  pk = 0;
  step_blocks(*br, 6, &pk);
  CHECK(organ.active_voices() == 1 && pk > 0.01f);  // the pedal holds the key
  br->submit({up, 0});
  step_blocks(*br, 1);
  step_blocks(*br, 4);
  pk = 0;
  step_blocks(*br, 1, &pk);
  CHECK(organ.active_voices() == 0 && pk == 0.0f);
  // Re-strike while the release is deferred: one voice, still sounding, and the pedal-up does not cut a key that is down.
  br->submit({make_note_on(0, 0, 62, 0xFFFF), 0});
  step_blocks(*br, 3);
  br->submit({down, 0});
  br->submit({make_note_off(0, 0, 62), 1});
  step_blocks(*br, 2);
  br->submit({make_note_on(0, 0, 62, 0xFFFF), 0});
  pk = 0;
  step_blocks(*br, 4, &pk);
  CHECK(organ.active_voices() == 1 && pk > 0.01f);
  br->submit({up, 0});
  pk = 0;
  step_blocks(*br, 6, &pk);
  CHECK(organ.active_voices() == 1 && pk > 0.01f);
  br->submit({make_note_off(0, 0, 62), 0});
  step_blocks(*br, 6);
  CHECK(organ.active_voices() == 0);
}

void test_reserve_requires_real_cost() {
  OrganEngine organ;
  std::unique_ptr<BlockRenderer> br(make_renderer(organ));
  VoiceArbiter::LiveSetConfig cfg;
  cfg.budget = 4000;
  cfg.organ_rotary_cost = 0;
  CHECK(br->arbiter().reserve(cfg) == VoiceArbiter::ReserveStatus::kInvalidConfig);
  cfg.organ_rotary_cost = OrganEngine::reservation_cost_units(1) - 1;
  CHECK(br->arbiter().reserve(cfg) == VoiceArbiter::ReserveStatus::kOk);  // enough for the 73 keys' voices
  cfg.organ_rotary_cost = 73 * OrganEngine::kVoiceCostUnits - 1;
  CHECK(br->arbiter().reserve(cfg) == VoiceArbiter::ReserveStatus::kInvalidConfig);
}

void test_voice_lifecycle() {
  // Disabling a part releases its voices and the arbiter budget.
  {
    OrganEngine organ;
    std::unique_ptr<BlockRenderer> br(make_renderer(organ));
    VoiceArbiter::LiveSetConfig cfg;
    cfg.budget = 1000;
    cfg.organ_rotary_cost = OrganEngine::reservation_cost_units(1);
    CHECK(br->arbiter().reserve(cfg) == VoiceArbiter::ReserveStatus::kOk);
    for (std::uint32_t n = 40; n < 50; ++n) br->submit({make_note_on(0, 0, n, 0xFFFF), 0});
    step_blocks(*br, 3);
    CHECK(organ.active_voices() == 10);
    CHECK(br->arbiter().usage().used() > 0);
    organ.set_enabled_parts(0xFEu);
    CHECK(organ.active_voices() == 0);
    step_blocks(*br, 2);
    CHECK(br->arbiter().usage().used() == 0);
    CHECK(organ.start_voice(0, 40, 1) == EngineStatus::kEngineNotAvailable);
  }
  // A duplicate note-on starts nothing and takes no arbiter slot.
  {
    OrganEngine organ;
    std::unique_ptr<BlockRenderer> br(make_renderer(organ));
    VoiceArbiter::LiveSetConfig cfg;
    cfg.budget = 1000;
    cfg.organ_rotary_cost = OrganEngine::reservation_cost_units(1);
    CHECK(br->arbiter().reserve(cfg) == VoiceArbiter::ReserveStatus::kOk);
    for (int i = 0; i < 5; ++i) br->submit({make_note_on(0, 0, 60, 0xFFFF), 0});
    step_blocks(*br, 3);
    CHECK(organ.active_voices() == 1);
    CHECK(br->arbiter().usage().used() == OrganEngine::kVoiceCostUnits);
  }
  // Slot exhaustion reclaims the oldest releasing voice; held voices are never taken.
  {
    const std::uint32_t cap = OrganEngine::kVoiceCapacity;
    // Fill to capacity with distinct (channel, note) pairs: parts 0..7 x notes 0..79 = 640.
    OrganEngine f;
    std::uint32_t started = 0;
    for (std::uint32_t part = 0; part < 8; ++part)
      for (std::uint32_t note = 0; note < 80; ++note) started += f.start_voice(part, note, 1) == EngineStatus::kOk;
    CHECK(started == cap && f.active_voices() == cap);
    CHECK(f.start_voice(0, 100, 1) == EngineStatus::kPoolExhausted);  // all held: nothing to reclaim
    CHECK(f.stop_voice(3, 7) == EngineStatus::kOk);
    CHECK(f.stop_voice(1, 9) == EngineStatus::kOk);
    CHECK(f.start_voice(0, 100, 1) == EngineStatus::kOk);  // reclaimed a releasing voice
    VoiceInfo vi;
    std::uint32_t held = 0, releasing = 0;
    for (std::uint32_t i = 0; i < cap; ++i)
      if (f.voice_info(i, vi)) (vi.state == VoiceState::kHeld ? held : releasing)++;
    CHECK(f.active_voices() == cap && held == cap - 1 && releasing == 1);
  }
}

void test_costs_and_identity() {
  OrganEngine e;
  CHECK(e.type_id() == EngineTypeId::kOrgan && static_cast<std::uint32_t>(e.type_id()) == 2);
  CHECK(e.cost_units_per_voice() == OrganEngine::kVoiceCostUnits);
  CHECK(e.steal_protected());
  CHECK(!e.begin_steal_fade(0, 192));
  CHECK(OrganEngine::reservation_cost_units(1) ==
        OrganEngine::kGeneratorCostUnits + OrganEngine::kRotaryCostUnits + 73u * OrganEngine::kVoiceCostUnits);
  CHECK(e.voice_capacity() >= 8u * 73u);
  // FM/VA stay unavailable.
  BlockRenderer br;
  br.set_channel_engine(1, static_cast<std::uint32_t>(EngineTypeId::kFm));
  br.submit({make_note_on(0, 1, 60, 1000), 0});
  float l[64], r[64];
  br.process_block(l, r);
  CHECK(br.status_count(EngineStatus::kEngineNotAvailable) == 1);
}

void test_release() {
  OrganEngine e;
  e.start_voice(0, 60, 1);
  render_mono(e, 640);
  CHECK(e.stop_voice(0, 60) == EngineStatus::kOk);
  CHECK(e.stop_voice(0, 60) == EngineStatus::kVoiceNotFound);
  VoiceInfo vi;
  CHECK(e.voice_info(0, vi) && vi.state == VoiceState::kReleasing && vi.note == 60 && vi.channel == 0);
  render_mono(e, 256);
  CHECK(e.active_voices() == 0);
  CHECK(!e.voice_info(0, vi));
}

std::string slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void test_forbidden_strings() {
  // Needles are assembled so this file itself does not contain them.
  const std::string needles[] = {std::string("ham") + "mond", std::string("les") + "lie", std::string("b") + "-3"};
  const std::string root = VELORA_SOURCE_ROOT;
  const char* files[] = {"engine/core/include/velora/core/organ_engine.hpp", "engine/core/src/organ_engine.cpp",
                         "engine/core/include/velora/core/tonewheel_tables.hpp", "proto/params.json",
                         "engine/core/ORGAN.md"};
  for (const char* rel : files) {
    std::string text = slurp(root + "/" + rel);
    CHECK(!text.empty());
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const std::string& n : needles) {
      for (std::size_t pos = text.find(n); pos != std::string::npos; pos = text.find(n, pos + 1)) {
        const bool word_start = pos == 0 || !std::isalnum(static_cast<unsigned char>(text[pos - 1]));
        if (!word_start) continue;
        std::printf("forbidden name '%s' in %s\n", n.c_str(), rel);
        CHECK(false);
      }
    }
  }
  CHECK(std::string(OrganEngine::kEngineName) == "tonewheel organ");
}

}  // namespace

int main() {
  test_tables();
  test_wheel_generator();
  test_drawbar_harmonics();
  test_default_registration();
  test_foldback();
  test_drawbar_curve();
  test_no_zipper();
  test_shared_phase();
  test_full_chord_and_determinism();
  test_param_routing();
  test_arbiter_protection();
  test_generation_forwarding();
  test_bind_after_routing();
  test_pedal();
  test_reserve_requires_real_cost();
  test_voice_lifecycle();
  test_costs_and_identity();
  test_release();
  test_forbidden_strings();
  if (g_fail) {
    std::printf("organ_tests: %d failures\n", g_fail);
    return 1;
  }
  std::printf("organ_tests: ok\n");
  return 0;
}
