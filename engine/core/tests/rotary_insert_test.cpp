// Story 2.9: rotary speaker as an insert effect. Bare main, CHECK style as the other core tests.
//
// The DSP is the RotarySpeaker of Story 2.6 (tested in rotary_tests); here: the insert wrapper. Mono/stereo handling (sum to mono, rotary,
// width), the speed mapping with its thresholds, ramps identical to the unit's (and so to the organ's), click-free speed changes, state
// reset, bypass through a chain, non-finite input, determinism, and the one-instance rule next to the organ's built-in rotary: an organ
// part with its built-in rotary on and no insert has exactly one rotary stage; with an insert inserted by the user there are two.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/rotary_effect.hpp"
#include "velora/core/snapshot.hpp"

using namespace velora::core;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 48000.0;
using Params = std::array<float, velora::core::params::kInsertParamCount>;

struct Stereo {
  std::vector<float> l, r;
};

Stereo run(Effect& fx, const Stereo& in) {
  Stereo out = in;
  for (std::size_t i = 0; i < out.l.size(); i += 64) {
    StereoBlock b{out.l.data() + i, out.r.data() + i, static_cast<std::uint32_t>(std::min<std::size_t>(64, out.l.size() - i))};
    fx.process(b);
  }
  return out;
}

Stereo sine(std::size_t n, double hz, double amp) {
  Stereo s;
  s.l.resize(n);
  s.r.resize(n);
  for (std::size_t i = 0; i < n; ++i) s.l[i] = s.r[i] = static_cast<float>(amp * std::sin(2.0 * kPi * hz * static_cast<double>(i) / kFs));
  return s;
}

Stereo noise(std::size_t n, std::uint32_t seed, float amp = 0.3f) {
  Stereo s;
  s.l.resize(n);
  s.r.resize(n);
  std::uint32_t x = seed;
  for (std::size_t i = 0; i < n; ++i) {
    x = x * 1664525u + 1013904223u;
    s.l[i] = (static_cast<float>(x >> 8) / 8388608.0f - 1.0f) * amp;
    x = x * 1664525u + 1013904223u;
    s.r[i] = (static_cast<float>(x >> 8) / 8388608.0f - 1.0f) * amp;
  }
  return s;
}

std::unique_ptr<RotaryEffect> make(const Params& p) {
  auto fx = std::make_unique<RotaryEffect>();
  fx->set_params(p);
  fx->prepare(48000, 64);
  return fx;
}

double max_diff(const std::vector<float>& a, const std::vector<float>& b, std::size_t from = 0) {
  double m = 0;
  for (std::size_t i = from; i < a.size(); ++i) m = std::max(m, std::fabs(static_cast<double>(a[i]) - b[i]));
  return m;
}

double max_step(const std::vector<float>& v, std::size_t from, std::size_t to) {
  double m = 0.0;
  for (std::size_t i = std::max<std::size_t>(from, 1); i < to; ++i) m = std::max(m, std::fabs(static_cast<double>(v[i]) - v[i - 1]));
  return m;
}

// The unit alone on the mono sum of a stereo input: the reference for the insert at width 1 (parameter 0.5).
Stereo reference(const Stereo& in, RotarySpeed speed) {
  RotarySpeaker rot;
  rot.set_speed_immediate(speed);
  std::vector<float> mono(in.l.size());
  for (std::size_t i = 0; i < mono.size(); ++i) mono[i] = 0.5f * (in.l[i] + in.r[i]);
  Stereo o;
  o.l.assign(mono.size(), 0.0f);
  o.r.assign(mono.size(), 0.0f);
  for (std::size_t i = 0; i < mono.size(); i += 64) {
    const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(64, mono.size() - i));
    rot.process(&mono[i], &o.l[i], &o.r[i], n);
  }
  return o;
}

void test_mapping() {
  using namespace rotmap;
  CHECK(speed(0.0f) == RotarySpeed::kStop && speed(0.1f) == RotarySpeed::kStop);
  CHECK(speed(std::nextafter(0.25f, 0.0f)) == RotarySpeed::kStop && speed(0.25f) == RotarySpeed::kSlow);  // thresholds belong to the upper band
  CHECK(speed(0.5f) == RotarySpeed::kSlow && speed(std::nextafter(0.75f, 0.0f)) == RotarySpeed::kSlow && speed(0.75f) == RotarySpeed::kFast);
  CHECK(speed(1.0f) == RotarySpeed::kFast && speed(7.0f) == RotarySpeed::kFast && speed(-1.0f) == RotarySpeed::kStop);
  CHECK(speed(std::nanf("")) == RotarySpeed::kSlow);  // the registry default 0.5 and NaN both mean slow
  CHECK(static_cast<int>(RotarySpeed::kStop) == velora::core::params::kRotarySpeedStop && static_cast<int>(RotarySpeed::kSlow) == velora::core::params::kRotarySpeedSlow &&
        static_cast<int>(RotarySpeed::kFast) == velora::core::params::kRotarySpeedFast);
  CHECK(width(0.0f) == 0.0f && width(0.5f) == 1.0f && width(1.0f) == 2.0f && width(9.0f) == 2.0f);
  auto fx = make_effect(EffectType::kRotary);
  auto* re = dynamic_cast<RotaryEffect*>(fx.get());
  CHECK(re != nullptr && re->type() == EffectType::kRotary && fx->cost_units() == effect_cost_units(EffectType::kRotary));
  CHECK(effect_cost_units(EffectType::kRotary) == RotarySpeaker::kCostUnits);  // the same DSP, the same declared cost
  if (re) CHECK(re->target_speed() == RotarySpeed::kSlow && re->width() == 1.0f);
}

void test_mono_stereo_handling() {
  const Stereo in = noise(20000, 5);
  // width parameter 0.5 (= 1.0): the unit run on the mono sum, within float rounding of the recombination
  for (auto [p, sp] : {std::pair{0.5f, RotarySpeed::kSlow}, std::pair{1.0f, RotarySpeed::kFast}, std::pair{0.0f, RotarySpeed::kStop}}) {
    auto fx = make({p, 0.5f, 0.5f, 0.5f, 0.0f});
    const Stereo o = run(*fx, in), ref = reference(in, sp);
    CHECK(max_diff(o.l, ref.l) < 2e-6 && max_diff(o.r, ref.r) < 2e-6);
  }
  // a stereo source is summed: left-only and right-only inputs give the same output at half level each
  {
    Stereo lo = in, ro = in;
    std::fill(lo.r.begin(), lo.r.end(), 0.0f);
    std::fill(ro.l.begin(), ro.l.end(), 0.0f);
    Stereo both = in;
    for (std::size_t i = 0; i < in.l.size(); ++i) {
      both.l[i] = in.l[i];
      both.r[i] = in.l[i];
      ro.r[i] = in.l[i];  // same samples as lo.l
    }
    auto a = make({0.5f, 0.5f, 0.5f, 0.5f, 0.0f}), b = make({0.5f, 0.5f, 0.5f, 0.5f, 0.0f}), c = make({0.5f, 0.5f, 0.5f, 0.5f, 0.0f});
    const Stereo oa = run(*a, lo), ob = run(*b, ro), oc = run(*c, both);
    CHECK(max_diff(oa.l, ob.l) < 1e-7 && max_diff(oa.r, ob.r) < 1e-7);
    // and a mono source on both channels is twice a one-sided input (sum / 2 against sum / 2 of a doubled signal)
    double ea = 0, ec = 0;
    for (std::size_t i = 4000; i < oa.l.size(); ++i) {
      ea += static_cast<double>(oa.l[i]) * oa.l[i];
      ec += static_cast<double>(oc.l[i]) * oc.l[i];
    }
    CHECK(std::fabs(std::sqrt(ec / ea) - 2.0) < 1e-3);
  }
  // an out-of-phase pair (side only) sums to silence: the rotary sees nothing
  {
    Stereo side = in;
    for (std::size_t i = 0; i < side.l.size(); ++i) side.r[i] = -side.l[i];
    auto fx = make({0.75f, 0.5f, 0.5f, 0.5f, 0.0f});
    const Stereo o = run(*fx, side);
    bool zero = true;
    for (std::size_t i = 0; i < o.l.size(); ++i) zero = zero && o.l[i] == 0.0f && o.r[i] == 0.0f;
    CHECK(zero);
  }
  // width: 0 = mono output (both channels identical), 1 (parameter) = twice the side signal of the unit
  {
    Stereo mono = in;
    mono.r = mono.l;
    auto w0 = make({0.75f, 0.0f, 0.5f, 0.5f, 0.0f}), w1 = make({0.75f, 1.0f, 0.5f, 0.5f, 0.0f}), wn = make({0.75f, 0.5f, 0.5f, 0.5f, 0.0f});
    const Stereo o0 = run(*w0, mono), o1 = run(*w1, mono), on = run(*wn, mono);
    CHECK(o0.l == o0.r);
    double side_n = 0, side_1 = 0;
    for (std::size_t i = 4000; i < on.l.size(); ++i) {
      side_n += std::pow(0.5 * (static_cast<double>(on.l[i]) - on.r[i]), 2);
      side_1 += std::pow(0.5 * (static_cast<double>(o1.l[i]) - o1.r[i]), 2);
      CHECK(std::fabs(0.5 * (static_cast<double>(on.l[i]) + on.r[i]) - 0.5 * (static_cast<double>(o0.l[i]) + o0.r[i])) < 2e-6);  // the mid signal is the same
    }
    CHECK(side_n > 0.0 && std::fabs(std::sqrt(side_1 / side_n) - 2.0) < 1e-3);
  }
}

// Speed ramps are the unit's: an insert whose speed changes at a block boundary has exactly the rotor speeds of the unit given the same target
// at the same sample.
void test_speed_ramps() {
  auto fx = make({0.75f, 0.5f, 0.5f, 0.5f, 0.0f});  // starts settled at fast
  RotarySpeaker ref;
  ref.set_speed_immediate(RotarySpeed::kFast);
  CHECK(fx->rotary().horn_hz() == ref.horn_hz() && fx->rotary().drum_hz() == ref.drum_hz() && ref.horn_hz() > 6.0);  // settled, no ramp at the start
  const Stereo in = sine(48000 * 12, 1500.0, 0.3);
  std::vector<float> mono(64), ol(64), orr(64);
  Stereo o = in;
  const struct { std::size_t block; float p; RotarySpeed s; } plan[] = {{100, 0.5f, RotarySpeed::kSlow}, {2000, 0.0f, RotarySpeed::kStop}, {5000, 1.0f, RotarySpeed::kFast}};
  double mid_horn = -1.0;
  bool equal = true, monotone_down = true;
  double prev = 1e9;
  for (std::size_t b = 0; b < in.l.size() / 64; ++b) {
    for (const auto& pl : plan)
      if (pl.block == b) {
        fx->set_params({pl.p, 0.5f, 0.5f, 0.5f, 0.0f});
        ref.set_target(pl.s);
      }
    StereoBlock blk{o.l.data() + b * 64, o.r.data() + b * 64, 64};
    fx->process(blk);
    for (std::size_t i = 0; i < 64; ++i) mono[i] = 0.5f * (in.l[b * 64 + i] + in.r[b * 64 + i]);
    ref.process(mono.data(), ol.data(), orr.data(), 64);
    equal = equal && fx->rotary().horn_hz() == ref.horn_hz() && fx->rotary().drum_hz() == ref.drum_hz() &&
            fx->rotary().horn_angle_cycles() == ref.horn_angle_cycles();
    if (b == 100 + 30) mid_horn = fx->rotary().horn_hz();  // 30 blocks (40 ms) after fast -> slow: still on its way
    if (b > 100 && b < 1000) {
      monotone_down = monotone_down && fx->rotary().horn_hz() <= prev + 1e-12;
      prev = fx->rotary().horn_hz();
    }
  }
  CHECK(equal);
  CHECK(mid_horn > 0.8 + 1.0 && mid_horn < 6.8);  // between fast and slow: the speed ramps, it does not jump
  CHECK(monotone_down);
  CHECK(fx->rotary().target() == RotarySpeed::kFast && fx->target_speed() == RotarySpeed::kFast);
  // stop really stops (rotor at rest) and a speed change keeps the output continuous
  auto st = make({0.75f, 0.5f, 0.5f, 0.5f, 0.0f});
  Stereo o2 = sine(48000 * 14, 700.0, 0.3);
  for (std::size_t b = 0; b < o2.l.size() / 64; ++b) {
    if (b == 750) st->set_params({0.0f, 0.5f, 0.5f, 0.5f, 0.0f});
    StereoBlock blk{o2.l.data() + b * 64, o2.r.data() + b * 64, 64};
    st->process(blk);
  }
  CHECK(st->rotary().horn_hz() == 0.0 && st->rotary().drum_hz() == 0.0 && st->rotary().horn_angle_cycles() == 0.0);
}

void test_click_free() {
  const std::size_t n = 48000 * 10;
  const Stereo in = sine(n, 440.0, 0.3);
  auto fx = make({0.75f, 0.5f, 0.5f, 0.5f, 0.0f});
  Stereo a = in;
  const double steady_in = 0.3 * 2.0 * kPi * 440.0 / kFs;
  const std::size_t changes[] = {1000, 2500, 4000};  // blocks
  for (std::size_t b = 0; b < n / 64; ++b) {
    if (b == changes[0]) fx->set_params({0.5f, 0.5f, 0.5f, 0.5f, 0.0f});
    if (b == changes[1]) fx->set_params({0.0f, 0.5f, 0.5f, 0.5f, 0.0f});
    if (b == changes[2]) fx->set_params({1.0f, 0.5f, 0.5f, 0.5f, 0.0f});
    StereoBlock blk{a.l.data() + b * 64, a.r.data() + b * 64, 64};
    fx->process(blk);
  }
  // steady fast, then the whole run including the speed changes: the step stays within the same small multiple of the input's own step
  const double steady = std::max(max_step(a.l, 2000, 60000), max_step(a.r, 2000, 60000));
  const double all = std::max(max_step(a.l, 1, n), max_step(a.r, 1, n));
  std::printf("rotary_insert_tests: max step steady fast %.4f, through the speed changes %.4f (input slope %.4f)\n", steady, all, steady_in);
  CHECK(all < 1.5 * steady);
  CHECK(all < 4.0 * steady_in);
  // a width change is smooth too
  auto w = make({0.75f, 0.5f, 0.5f, 0.5f, 0.0f});
  Stereo b2 = in;
  for (std::size_t b = 0; b < 2000; ++b) {
    if (b == 700) w->set_params({0.75f, 1.0f, 0.5f, 0.5f, 0.0f});
    StereoBlock blk{b2.l.data() + b * 64, b2.r.data() + b * 64, 64};
    w->process(blk);
  }
  CHECK(max_step(b2.l, 1, 2000 * 64) < 2.0 * steady);
}

void test_robustness() {
  const Stereo in = noise(30000, 11);
  const Params p = {0.9f, 0.7f, 0.5f, 0.5f, 0.5f};
  auto a = make(p), b = make(p);
  const Stereo oa = run(*a, in), ob = run(*b, in);
  CHECK(oa.l == ob.l && oa.r == ob.r);  // deterministic
  // a non-finite sample is exactly a zero sample
  Stereo dirty = noise(30000, 12), clean = dirty;
  const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
  for (std::size_t i = 1000; i < 29000; i += 997) {
    dirty.l[i] = nan;
    clean.l[i] = 0.0f;
    dirty.r[i + 1] = i % 2 ? inf : -inf;
    clean.r[i + 1] = 0.0f;
  }
  auto x = make(p), y = make(p);
  const Stereo od = run(*x, dirty), oc = run(*y, clean);
  bool finite = true;
  for (std::size_t i = 0; i < od.l.size(); ++i) finite = finite && std::isfinite(od.l[i]) && std::isfinite(od.r[i]);
  CHECK(finite && od.l == oc.l && od.r == oc.r);
  // NaN / out-of-range parameters
  auto w = make({std::nanf(""), 7.0f, 0.0f, 0.0f, 0.0f});
  const Stereo ow = run(*w, in);
  finite = true;
  for (std::size_t i = 0; i < ow.l.size(); ++i) finite = finite && std::isfinite(ow.l[i]);
  CHECK(finite);
  // reset equals fresh (what a bypassed slot gets)
  auto used = make(p), fresh = make(p);
  run(*used, noise(20000, 8));
  used->reset();
  const Stereo ra = run(*used, in), rb = run(*fresh, in);
  CHECK(ra.l == rb.l && ra.r == rb.r);
  // reset on a running instance returns the rotors to the rest angle at the target speed
  auto m = make({0.5f, 0.5f, 0.5f, 0.5f, 0.0f});
  run(*m, noise(5000, 2));
  CHECK(m->rotary().horn_angle_cycles() > 0.0);
  m->reset();
  CHECK(m->rotary().horn_angle_cycles() == 0.0 && m->rotary().horn_hz() == RotarySpeaker::target_hz(m->rotary().config().horn, RotarySpeed::kSlow));
  // silence in, silence out
  Stereo z;
  z.l.assign(4096, 0.0f);
  z.r.assign(4096, 0.0f);
  auto s = make(p);
  const Stereo os = run(*s, z);
  CHECK(os.l == z.l && os.r == z.r);
  // a bypassed slot is bit-exact and keeps its cost; an un-bypassed one changes the signal
  {
    FxChain chain;
    chain.set_slot(0, make(p), true);
    Stereo v = noise(640, 5);
    const Stereo ref = v;
    for (std::size_t i = 0; i < v.l.size(); i += 64) {
      StereoBlock bl{v.l.data() + i, v.r.data() + i, 64};
      chain.process(bl);
    }
    CHECK(v.l == ref.l && v.r == ref.r && chain.cost_units() == effect_cost_units(EffectType::kRotary));
  }
  // live-instance counting follows the lifetime
  const std::int64_t n0 = RotaryEffect::live_instances();
  {
    auto t = make(p);
    CHECK(RotaryEffect::live_instances() == n0 + 1);
  }
  CHECK(RotaryEffect::live_instances() == n0);
}

// ---- the organ's built-in rotary and the insert ----------------------------------------------------------------------------------------
struct OrganRig {
  std::unique_ptr<OrganEngine> organ = std::make_unique<OrganEngine>();
  std::unique_ptr<BlockRenderer> br = std::make_unique<BlockRenderer>(8);
  std::vector<std::unique_ptr<Snapshot>> snaps;
  std::vector<float> l, r;
  std::uint64_t rev = 0;
  OrganRig() {
    br->bind_engine(*organ);
    for (std::uint32_t ch = 0; ch < 8; ++ch) br->set_channel_engine(ch, static_cast<std::uint32_t>(EngineTypeId::kOrgan));
    param(params::kKeyClick, 0, 0.0);
    param(params::kLeakage, 0, 0.0);
  }
  void param(std::uint32_t id, std::uint32_t part, double v) { br->submit({make_param_event(*params::find(id), part, v), 0}); }
  void publish(Snapshot s) {
    s.revision = ++rev;
    s.ledger_index = static_cast<std::uint32_t>(rev % SnapshotExchange::kLedgerSlots);
    snaps.push_back(std::make_unique<Snapshot>(std::move(s)));
    br->snapshots().publish(snaps.back().get());
  }
  void blocks(int n) {
    for (int i = 0; i < n; ++i) {
      float a[kBlockFrames], b[kBlockFrames];
      br->process_block(a, b);
      l.insert(l.end(), a, a + kBlockFrames);
      r.insert(r.end(), b, b + kBlockFrames);
    }
  }
};

void test_organ_has_one_rotary_unless_inserted() {
  const std::int64_t n0 = RotaryEffect::live_instances();
  auto scenario = [](bool insert, std::uint8_t insert_params_speed_fast, std::vector<float>& l, std::vector<float>& r, int& inserts, int& builtin) {
    OrganRig rig;
    Snapshot s = Snapshot::defaults();
    if (insert) {
      s.parts[0].insert_count = 1;
      s.parts[0].inserts[0].type = params::kInsertTypeRotary;
      s.parts[0].inserts[0].params = {insert_params_speed_fast ? 1.0f : 0.5f, 0.5f, 0.5f, 0.5f, 0.0f};
      build_chains(s);
    }
    rig.publish(std::move(s));
    rig.param(params::kRotaryOn, 0, 1.0);
    rig.param(params::kRotarySpeed, 0, params::kRotarySpeedFast);
    for (std::uint32_t n : {48u, 55u, 60u, 64u}) rig.br->submit({make_note_on(0, 0, n, 40000), 0});
    rig.blocks(300);
    inserts = static_cast<int>(RotaryEffect::live_instances());
    builtin = rig.organ->rotary_status(0).on ? 1 : 0;
    l = rig.l;
    r = rig.r;
  };
  std::vector<float> l1, r1, l2, r2;
  int ins1 = 0, bi1 = 0, ins2 = 0, bi2 = 0;
  scenario(false, 0, l1, r1, ins1, bi1);
  CHECK(ins1 - n0 == 0 && bi1 == 1);  // built-in rotary only: exactly one rotary stage; nothing inserted automatically
  scenario(true, 1, l2, r2, ins2, bi2);
  CHECK(ins2 - n0 == 1 && bi2 == 1);  // the user's insert: two rotary stages (the built-in one and the insert)
  CHECK(RotaryEffect::live_instances() == n0);  // all gone
  // the inserted version is the built-in output run through a rotary insert (same DSP, applied to the part bus)
  Stereo organ_only{l1, r1};
  Stereo through;
  {
    auto fx = make({1.0f, 0.5f, 0.5f, 0.5f, 0.0f});
    through = run(*fx, organ_only);
  }
  double diff = 0.0, delta_vs_plain = 0.0;
  for (std::size_t i = 512; i < through.l.size(); ++i) {  // after the 5 ms chain fade-in
    diff = std::max(diff, std::fabs(static_cast<double>(through.l[i]) - l2[i]));
    delta_vs_plain = std::max(delta_vs_plain, std::fabs(static_cast<double>(l1[i]) - l2[i]));
  }
  std::printf("rotary_insert_tests: organ + insert vs built-in output through a standalone insert: max diff %.2e (insert changes the signal by %.3f)\n", diff, delta_vs_plain);
  CHECK(diff < 1e-5 && delta_vs_plain > 0.01);
  // an organ part without the built-in rotary can still use the insert (the insert is the only rotary stage)
  {
    OrganRig rig;
    Snapshot s = Snapshot::defaults();
    s.parts[0].insert_count = 1;
    s.parts[0].inserts[0].type = params::kInsertTypeRotary;
    build_chains(s);
    rig.publish(std::move(s));
    for (std::uint32_t n : {48u, 55u, 60u}) rig.br->submit({make_note_on(0, 0, n, 40000), 0});
    rig.blocks(200);
    CHECK(!rig.organ->rotary_status(0).on && RotaryEffect::live_instances() - n0 == 1);
    double side = 0.0;
    for (std::size_t i = 4000; i < rig.l.size(); ++i) side = std::max(side, std::fabs(static_cast<double>(rig.l[i]) - rig.r[i]));
    CHECK(side > 0.001);  // a stereo output from the mono organ: the insert spreads it
  }
  CHECK(RotaryEffect::live_instances() == n0);
}

void test_cost() {
  auto fx = make({0.75f, 0.5f, 0.5f, 0.5f, 0.0f});
  Stereo a = sine(64, 700.0, 0.3);
  const int iters = 20000;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < iters; ++i) {
    StereoBlock b{a.l.data(), a.r.data(), 64};
    fx->process(b);
    for (std::size_t k = 0; k < 64; ++k) a.l[k] = a.r[k] = 0.3f * std::sin(0.1f * static_cast<float>(k + static_cast<std::size_t>(i) * 64));
  }
  const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / iters;
  std::printf("host cost per 64-frame block: rotary insert %.2f us (declared %u units)\n", us, effect_cost_units(EffectType::kRotary));
  CHECK(effect_cost_units(EffectType::kRotary) <= kMaxInsertCost);
}


void test_blocks_prepare_reset_running() {
  const Stereo in = noise(1000, 51);
  const Params p = {0.75f, 0.7f, 0.5f, 0.5f, 0.0f};
  auto ref = make(p), big = make(p), one = make(p);
  const Stereo r64 = run(*ref, in);
  Stereo b200 = in, b1 = in;
  for (std::size_t i = 0; i < 1000; i += 200) {
    StereoBlock b{b200.l.data() + i, b200.r.data() + i, 200};  // larger than the scratch arrays: processed in chunks
    big->process(b);
  }
  for (std::size_t i = 0; i < 1000; ++i) {
    StereoBlock b{b1.l.data() + i, b1.r.data() + i, 1};
    one->process(b);
  }
  CHECK(b200.l == r64.l && b200.r == r64.r && b1.l == r64.l && b1.r == r64.r);
  // prepare() refusals
  {
    RotaryEffect fx;
    CHECK(fx.prepare_ok());
    fx.prepare(44100, 64);
    CHECK(!fx.prepare_ok());
    CHECK(run(fx, in).l == in.l);
    fx.prepare(48000, 65);
    CHECK(!fx.prepare_ok());
    fx.prepare(48000, 64);
    CHECK(fx.prepare_ok());
    CHECK(run(fx, in).l != in.l);
  }
  // reset(): a set_params() after it applies speed and width at once (settled start), not a ramp from the old speed
  {
    auto fx = make({1.0f, 0.5f, 0.5f, 0.5f, 0.0f});  // fast
    run(*fx, noise(4000, 3));
    fx->reset();
    fx->set_params({0.5f, 1.0f, 0.5f, 0.5f, 0.0f});  // slow, width 2
    CHECK(fx->rotary().horn_hz() == RotarySpeaker::target_hz(fx->rotary().config().horn, RotarySpeed::kSlow));
    CHECK(fx->rotary().drum_hz() == RotarySpeaker::target_hz(fx->rotary().config().drum, RotarySpeed::kSlow));
    CHECK(fx->width() == 2.0f);
  }
  // running instance: WIDTH 0.5 -> 1.0 doubles the side signal (two identical warmed-up instances, one changed), the mid is unchanged
  {
    auto a = make({0.75f, 0.5f, 0.5f, 0.5f, 0.0f}), b = make({0.75f, 0.5f, 0.5f, 0.5f, 0.0f});
    const Stereo warm = noise(9600, 7);
    run(*a, warm);
    run(*b, warm);
    b->set_params({0.75f, 1.0f, 0.5f, 0.5f, 0.0f});
    const Stereo sig = noise(9600, 8);
    const Stereo oa = run(*a, sig), ob = run(*b, sig);
    double sa = 0, sb = 0, md = 0;
    for (std::size_t i = 4800; i < 9600; ++i) {
      sa += std::pow(0.5 * (static_cast<double>(oa.l[i]) - oa.r[i]), 2);
      sb += std::pow(0.5 * (static_cast<double>(ob.l[i]) - ob.r[i]), 2);
      md = std::max(md, std::fabs(0.5 * (static_cast<double>(oa.l[i]) + oa.r[i]) - 0.5 * (static_cast<double>(ob.l[i]) + ob.r[i])));
    }
    CHECK(sa > 0 && std::fabs(std::sqrt(sb / sa) - 2.0) < 0.02 && md < 1e-5);
  }
}

}  // namespace

int main() {
  test_mapping();
  test_mono_stereo_handling();
  test_speed_ramps();
  test_click_free();
  test_robustness();
  test_organ_has_one_rotary_unless_inserted();
  test_blocks_prepare_reset_running();
  test_cost();
  if (g_fail) {
    std::printf("rotary_insert_tests: %d failures\n", g_fail);
    return 1;
  }
  std::printf("rotary_insert_tests: ok\n");
  return 0;
}
