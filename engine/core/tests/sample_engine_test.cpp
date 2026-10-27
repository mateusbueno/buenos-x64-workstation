// SampleEngine unit tests (Story 1.15). The engine is driven through the Engine interface with
// hand-built float instruments (no formatlib, no files). The sfizz parity of the same behaviours is
// checked by the null tests in tools/offline-render.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/sample_engine.hpp"
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
#define CHECK_NEAR(a, b, tol)                                                                          \
  do {                                                                                                 \
    const double va_ = (a), vb_ = (b);                                                                 \
    if (!(std::fabs(va_ - vb_) <= (tol))) {                                                            \
      std::printf("FAIL %s:%d: %s = %.9g, expected %.9g (+-%g)\n", __FILE__, __LINE__, #a, va_, vb_, (double)(tol)); \
      ++g_fail;                                                                                        \
    }                                                                                                  \
  } while (0)

// --- helpers ----------------------------------------------------------------------------------------

struct TestInstrument final : ports::SampleSource {
  std::vector<ports::SampleRegionDesc> regions;
  std::vector<ports::SamplePcm> pcm;
  std::vector<std::vector<float>> data;

  std::vector<ports::SampleCcDefault> cc_defaults;  // Story 13.5
  std::vector<std::pair<std::uint8_t, std::vector<ports::SampleCurvePoint>>> curve_src;
  std::vector<ports::SampleCurve> curves;
  void add_curve(std::uint8_t index, std::vector<ports::SampleCurvePoint> pts) {
    curve_src.emplace_back(index, std::move(pts));
    curves.clear();
    for (const auto& c : curve_src) curves.push_back({c.first, c.second});
  }

  ports::InstrumentView instrument() const noexcept override { return {regions, pcm, cc_defaults, curves}; }

  // Returns the sample index. Stereo samples are stored left then right.
  std::uint32_t add_sample(std::vector<float> left, std::vector<float> right = {}, std::uint32_t rate = 48000) {
    const bool stereo = !right.empty();
    std::vector<float> buf = std::move(left);
    const std::size_t n = buf.size();
    if (stereo) buf.insert(buf.end(), right.begin(), right.end());
    data.push_back(std::move(buf));
    ports::SamplePcm p;
    p.sample_rate = rate;
    p.channels = stereo ? 2 : 1;
    p.frames = n;
    p.left = data.back().data();
    p.right = stereo ? data.back().data() + n : nullptr;
    pcm.push_back(p);
    return static_cast<std::uint32_t>(pcm.size() - 1);
  }
  std::uint32_t dc(float v, std::size_t frames = 20000) { return add_sample(std::vector<float>(frames, v)); }
  std::uint32_t ramp(std::size_t frames, float slope = 0.001f, std::uint32_t rate = 48000) {
    std::vector<float> x(frames);
    for (std::size_t i = 0; i < frames; ++i) x[i] = slope * static_cast<float>(i + 1);
    return add_sample(std::move(x), {}, rate);
  }
  ports::SampleRegionDesc& region(std::uint32_t sample, int lo, int hi, int center = -1) {
    ports::SampleRegionDesc d;
    d.sample_index = sample;
    d.key_lo = static_cast<std::uint8_t>(lo);
    d.key_hi = static_cast<std::uint8_t>(hi);
    d.key_center = static_cast<std::uint8_t>(center < 0 ? lo : center);
    regions.push_back(d);
    return regions.back();
  }
};

constexpr double kPi = 3.14159265358979323846;
constexpr std::uint32_t v16(std::uint32_t v7) { return (v7 * 65535u + 63u) / 127u; }  // as the SMF reader maps it

// Gain of a mono sample centred in the stereo field (pan law -3 dB then sfizz's +3 dB compensation).
static double mono_center_gain() { return std::cos(2047.0 * 1.57079632679489661923 / 4094.0) * 1.4125375446227544; }

static SampleEngine::Config cfg(std::uint32_t voices = 16, SampleInterpolation q = SampleInterpolation::kHermite3) {
  SampleEngine::Config c;
  c.max_voices = voices;
  c.interpolation = q;
  return c;
}

struct Out {
  std::vector<float> l, r;
};
static Out render(SampleEngine& e, std::uint32_t frames) {
  Out o;
  o.l.assign(frames, 0.0f);
  o.r.assign(frames, 0.0f);
  std::uint32_t done = 0;
  while (done < frames) {  // odd segment sizes on purpose: segmenting must not change the result
    const std::uint32_t n = std::min<std::uint32_t>(37, frames - done);
    e.render(o.l.data() + done, o.r.data() + done, n);
    done += n;
  }
  return o;
}
// Releases everything and renders until silent.
static void drain(SampleEngine& e) {
  for (std::uint32_t n = 0; n < 128; ++n) e.stop_voice(0, n);
  render(e, 96000);
}

// --- tests ------------------------------------------------------------------------------------------

static void test_region_selection() {
  TestInstrument ins;
  ins.region(ins.dc(0.1f), 60, 60).vel_hi = 63;
  { auto& r = ins.region(ins.dc(0.2f), 60, 60); r.vel_lo = 64; }
  ins.region(ins.dc(0.3f), 62, 64, 62);
  SampleEngine e(cfg());
  CHECK(e.set_instrument(ins));
  const double G = mono_center_gain();
  auto level = [&](std::uint32_t note, std::uint32_t vel7) {
    CHECK(e.start_voice(0, note, v16(vel7)) == EngineStatus::kOk);
    const Out o = render(e, 4);
    const double v = o.l[3] / G / ((vel7 / 127.0) * (vel7 / 127.0));
    drain(e);
    return v;
  };
  CHECK_NEAR(level(60, 63), 0.1, 1e-5);   // velocity boundary: 63 -> first layer
  CHECK_NEAR(level(60, 64), 0.2, 1e-5);   // 64 -> second layer
  CHECK_NEAR(level(60, 1), 0.1, 3e-4);  // 1 - (1 - v^2) in float, as sfizz computes it, is coarse at v = 1/127
  CHECK_NEAR(level(60, 127), 0.2, 1e-5);
  CHECK_NEAR(level(63, 100), 0.3, 1e-5);  // key range 62..64
  CHECK(e.start_voice(0, 61, v16(100)) == EngineStatus::kOk);  // no region: nothing to play, not an error
  CHECK(e.active_voices() == 0);
  CHECK(e.start_voice(0, 65, v16(100)) == EngineStatus::kOk && e.active_voices() == 0);
  // Overlapping regions layer: both fire.
  TestInstrument two;
  two.region(two.dc(0.1f), 60, 60);
  two.region(two.dc(0.2f), 60, 60);
  SampleEngine e2(cfg());
  e2.set_instrument(two);
  e2.start_voice(0, 60, v16(127));
  CHECK(e2.active_voices() == 2);
  CHECK_NEAR(render(e2, 4).l[3], 0.3 * G, 1e-5);
}

static void test_round_robin() {
  TestInstrument ins;
  for (int p = 1; p <= 4; ++p) {
    auto& r = ins.region(ins.dc(0.1f * static_cast<float>(p)), 60, 60);
    r.seq_length = 4;
    r.seq_position = static_cast<std::uint32_t>(p);
  }
  for (int p = 1; p <= 2; ++p) {  // independent two-step cycle on another key
    auto& r = ins.region(ins.dc(0.5f + 0.1f * static_cast<float>(p)), 64, 64);
    r.seq_length = 2;
    r.seq_position = static_cast<std::uint32_t>(p);
  }
  SampleEngine e(cfg());
  e.set_instrument(ins);
  const double G = mono_center_gain();
  auto next = [&](std::uint32_t note) {
    e.start_voice(0, note, v16(127));
    CHECK(e.active_voices() == 1);  // exactly one position fires
    const double v = render(e, 4).l[3] / G;
    drain(e);
    return v;
  };
  const int order[] = {1, 2, 3, 4, 1, 2, 3, 4, 1};  // wraps
  for (int i = 0; i < 9; ++i) CHECK_NEAR(next(60), 0.1 * order[i], 1e-5);
  // Key 64 has its own counter and is not advanced by key 60 (which is why it starts at position 1).
  CHECK_NEAR(next(64), 0.6, 1e-5);
  CHECK_NEAR(next(64), 0.7, 1e-5);
  CHECK_NEAR(next(64), 0.6, 1e-5);
}

static void test_random_layers() {
  TestInstrument ins;
  { auto& r = ins.region(ins.dc(0.1f), 60, 60); r.lorand = 0.0f; r.hirand = 0.25f; }
  { auto& r = ins.region(ins.dc(0.2f), 60, 60); r.lorand = 0.25f; r.hirand = 1.0f; }
  const double G = mono_center_gain();
  auto sequence = [&](std::uint64_t seed, int n) {
    SampleEngine::Config c = cfg(4096);
    c.seed = seed;
    SampleEngine e(c);
    e.set_instrument(ins);
    std::vector<int> seq;
    double prev = 0.0;
    for (int i = 0; i < n; ++i) {
      e.start_voice(0, 60, v16(127));
      const double now = render(e, 1).l[0];
      seq.push_back(now - prev < 0.15 * G ? 1 : 2);  // 0.1 -> first region, 0.2 -> second
      prev = now;
    }
    return seq;
  };
  const auto a = sequence(7, 3000), b = sequence(7, 3000), c = sequence(8, 3000);
  CHECK(a == b);  // reproducible for a seed
  CHECK(a != c);  // and different for another
  int first = 0;
  for (int v : a) first += v == 1;
  CHECK(first > 3000 * 0.21 && first < 3000 * 0.29);  // [0, 0.25) gets ~25%
  // set_param(kParamSeed) and set_seed() agree.
  SampleEngine e1(cfg(64)), e2(cfg(64));
  e1.set_instrument(ins);
  e2.set_instrument(ins);
  e1.set_seed(5);
  e2.set_param(SampleEngine::kParamSeed, 5.0f);
  for (int i = 0; i < 20; ++i) {
    e1.start_voice(0, 60, v16(127));
    e2.start_voice(0, 60, v16(127));
  }
  const Out o1 = render(e1, 8), o2 = render(e2, 8);
  CHECK(o1.l == o2.l);
}

static void test_pitch() {
  const double G = mono_center_gain();
  auto first_frames = [&](int center, int note, int transpose, float tune, std::uint32_t rate, std::uint32_t n,
                          SampleInterpolation q, std::vector<float>& x_out) {
    TestInstrument ins;
    auto& r = ins.region(ins.ramp(20000, 0.00001f, rate), 0, 127, center);
    r.transpose = transpose;
    r.tune_cents = tune;
    SampleEngine e(cfg(4, q));
    e.set_instrument(ins);
    e.start_voice(0, static_cast<std::uint32_t>(note), v16(127));
    x_out = ins.data[0];
    return render(e, n);
  };
  struct Case { int center, note, transpose; float tune; std::uint32_t rate; double ratio; };
  const Case cases[] = {
      {60, 60, 0, 0.0f, 48000, 1.0},          {60, 72, 0, 0.0f, 48000, 2.0},    {60, 48, 0, 0.0f, 48000, 0.5},
      {60, 12, 0, 0.0f, 48000, 1.0 / 16.0},   {60, 96, 0, 0.0f, 48000, 8.0},    {60, 60, 0, 100.0f, 48000, std::pow(2.0, 1.0 / 12)},
      {60, 60, -12, 0.0f, 48000, 0.5},        {60, 67, 0, 37.0f, 48000, std::pow(2.0, 7.37 / 12)},
      {60, 60, 0, 0.0f, 24000, 0.5},          {60, 60, 0, 0.0f, 44100, 44100.0 / 48000.0}};
  for (const SampleInterpolation q : {SampleInterpolation::kHermite3, SampleInterpolation::kLinear}) {
    for (const auto& c : cases) {
      std::vector<float> x;
      const Out o = first_frames(c.center, c.note, c.transpose, c.tune, c.rate, 200, q, x);
      // A ramp is interpolated exactly by both kernels: out[f] = x(position), x(p) = 1e-5 * (p + 1).
      for (std::uint32_t f : {1u, 50u, 199u}) CHECK_NEAR(o.l[f] / G, 1e-5 * (c.ratio * f + 1.0), 2e-7);
    }
  }
}

// Envelope reference values: the documented sfizz curves (see engine/core/README.md).
static void test_envelope() {
  const double G = mono_center_gain();
  const double sr = 48000.0;
  TestInstrument ins;
  auto& r = ins.region(ins.dc(1.0f, 400000), 60, 60);
  r.ampeg_delay = 0.005f;      // 240 frames of `start`
  r.ampeg_start = 0.0f;
  r.ampeg_attack = 0.01f;      // 480 frames, linear
  r.ampeg_hold = 0.01f;        // 480 frames at 1
  r.ampeg_decay = 0.02f;       // exponential, -9 time constants over 0.02 s
  r.ampeg_sustain = 50.0f;
  r.ampeg_release = 0.02f;
  SampleEngine e(cfg());
  e.set_instrument(ins);
  e.start_voice(0, 60, v16(127));
  const Out o = render(e, 6000);
  auto env = [&](std::uint32_t f) { return o.l[f] / G; };
  for (std::uint32_t f = 0; f < 240; ++f) CHECK(o.l[f] == 0.0f);          // delay
  for (std::uint32_t k = 0; k < 470; k += 37) CHECK_NEAR(env(240 + k), (k + 1) / 480.0, 1e-5);  // attack
  CHECK(env(240 + 478) < 1.0);
  std::uint32_t hold_frames = 0;
  for (std::uint32_t f = 240; f < 2000; ++f) hold_frames += std::fabs(env(f) - 1.0) < 1e-6;
  CHECK(hold_frames == 480);
  const double dec = std::exp(-9.0 / (0.02 * sr));
  std::uint32_t f0 = 240 + 479 + 480;  // first decay frame is about here; find it by value
  while (env(f0) > 0.9999) ++f0;
  for (std::uint32_t f = f0 + 1; f < f0 + 60; f += 9) CHECK_NEAR(env(f + 1) / env(f), dec, 1e-5);
  CHECK_NEAR(env(5999), 0.5, 1e-4);  // sustain
  // Release: exponential at the release time.
  e.stop_voice(0, 60);
  const Out rel = render(e, 4000);
  const double rr = std::exp(-9.0 / (0.02 * sr));
  for (std::uint32_t f = 10; f < 400; f += 60) CHECK_NEAR(rel.l[f + 1] / rel.l[f], rr, 1e-5);
  CHECK_NEAR(rel.l[0] / G, 0.5 * rr, 1e-4);
  CHECK(rel.l[3999] == 0.0f && e.active_voices() == 0);  // exponential tail, 50 ms fade-out, then the voice frees
  // A release below 6 ms (and the unspecified default) is held at the 6 ms minimum.
  for (const float rt : {0.001f, 0.0f}) {
    TestInstrument i2;
    i2.region(i2.dc(1.0f, 100000), 60, 60).ampeg_release = rt;
    SampleEngine e2(cfg());
    e2.set_instrument(i2);
    e2.start_voice(0, 60, v16(127));
    render(e2, 100);
    e2.stop_voice(0, 60);
    const Out q = render(e2, 400);
    CHECK_NEAR(q.l[101] / q.l[100], std::exp(-9.0 / (0.006 * sr)), 1e-5);
  }
  // A (near) zero sustain level releases by itself.
  TestInstrument i3;
  { auto& rg = i3.region(i3.dc(1.0f, 400000), 60, 60); rg.ampeg_decay = 0.01f; rg.ampeg_sustain = 0.0f; }
  SampleEngine e3(cfg());
  e3.set_instrument(i3);
  e3.start_voice(0, 60, v16(127));
  render(e3, 96000);
  CHECK(e3.active_voices() == 0);
  // Releasing during the delay: the voice never sounds.
  TestInstrument i4;
  i4.region(i4.dc(1.0f, 100000), 60, 60).ampeg_delay = 0.1f;
  SampleEngine e4(cfg());
  e4.set_instrument(i4);
  e4.start_voice(0, 60, v16(127));
  render(e4, 100);
  CHECK(e4.stop_voice(0, 60) == EngineStatus::kOk);
  CHECK(e4.active_voices() == 0);
  for (float x : render(e4, 12000).l) CHECK(x == 0.0f);
}

static void test_loops_and_end() {
  const double G = mono_center_gain();
  const std::size_t n = 100;
  auto idx_of = [&](double v) { return static_cast<int>(std::lround(v / G * 1000.0)); };  // x[i] = (i+1)/1000
  auto make = [&](ports::SampleLoopMode m, float release) {
    auto ins = std::make_unique<TestInstrument>();
    auto& r = ins->region(ins->ramp(n), 60, 60);
    r.loop_mode = m;
    r.has_loop_points = true;
    r.loop_start = 10;
    r.loop_end = 19;
    r.ampeg_release = release;
    return ins;
  };
  // loop_continuous: positions 0..19 then 10..19 forever; note off releases out of the loop.
  {
    auto ins = make(ports::SampleLoopMode::kLoopContinuous, 0.05f);
    SampleEngine e(cfg());
    e.set_instrument(*ins);
    e.start_voice(0, 60, v16(127));
    const Out o = render(e, 200);
    for (int f = 0; f < 200; ++f) {
      const int expect = f <= 19 ? f : 10 + (f - 10) % 10;
      CHECK(std::abs(idx_of(o.l[f]) - (expect + 1)) <= 1);  // 5 s release: envelope still ~1 (it is not released here)
    }
    CHECK(e.active_voices() == 1);
    e.stop_voice(0, 60);
    drain(e);
    CHECK(e.active_voices() == 0);
  }
  // loop_sustain: loops while held, continues through the rest of the sample after the note off.
  {
    auto ins = make(ports::SampleLoopMode::kLoopSustain, 5.0f);
    SampleEngine e(cfg());
    e.set_instrument(*ins);
    e.start_voice(0, 60, v16(127));
    const Out held = render(e, 55);  // still looping at frame 55
    CHECK(std::abs(idx_of(held.l[54]) - (10 + (54 - 10) % 10 + 1)) <= 1);
    const double pos_before = 10 + (55 - 10) % 10;  // the next frame would read this index
    e.stop_voice(0, 60);
    const Out after = render(e, 40);
    for (int f = 0; f < 40; ++f) CHECK(std::abs(idx_of(after.l[f] / std::exp(-9.0 / (5.0 * 48000.0) * (f + 1))) -
                                                 (static_cast<int>(pos_before) + f + 1)) <= 1);
  }
  // one_shot: the note off is ignored, the sample plays through to its end.
  {
    auto ins = make(ports::SampleLoopMode::kOneShot, 0.05f);
    SampleEngine e(cfg());
    e.set_instrument(*ins);
    e.start_voice(0, 60, v16(127));
    render(e, 10);
    CHECK(e.stop_voice(0, 60) == EngineStatus::kOk);
    VoiceInfo vi;
    CHECK(e.voice_info(0, vi) && vi.state == VoiceState::kSustained);
    const Out o = render(e, 120);
    CHECK(std::abs(idx_of(o.l[50]) - (10 + 50 + 1)) <= 1);  // ten frames already rendered
    CHECK(std::abs(idx_of(o.l[85]) - (10 + 85 + 1)) <= 1);
  }
  // no_loop: plays to its end, holds the last frame while a 6 ms release fades it out, then frees.
  {
    auto ins = make(ports::SampleLoopMode::kNoLoop, 0.05f);
    SampleEngine e(cfg());
    e.set_instrument(*ins);
    e.start_voice(0, 60, v16(127));
    const Out o = render(e, 400);
    CHECK(std::abs(idx_of(o.l[97]) - 98) <= 1);
    const double rate = std::exp(-9.0 / (0.006 * 48000.0));
    CHECK_NEAR(o.l[98] / G, 0.1 * rate, 1e-6);          // x[99] held, release already running on this frame
    CHECK_NEAR(o.l[99] / G, 0.1 * rate * rate, 1e-6);
    CHECK(e.active_voices() == 1);
    render(e, 6000);
    CHECK(e.active_voices() == 0);
  }
  // offset: playback starts at that frame.
  {
    TestInstrument ins;
    ins.region(ins.ramp(2000), 60, 60).offset = 500;
    SampleEngine e(cfg());
    e.set_instrument(ins);
    e.start_voice(0, 60, v16(127));
    const Out o = render(e, 4);
    CHECK(idx_of(o.l[0]) == 501 && idx_of(o.l[3]) == 504);
  }
}

static void test_release_trigger() {
  const double G = mono_center_gain();
  TestInstrument ins;
  ins.region(ins.dc(0.1f, 6000), 60, 60);
  { auto& r = ins.region(ins.dc(0.2f, 20000), 60, 60); r.trigger = ports::SampleTrigger::kRelease; }
  SampleEngine e(cfg());
  e.set_instrument(ins);
  e.start_voice(0, 60, v16(127));
  CHECK(e.active_voices() == 1);  // the release region does not fire on note on
  CHECK_NEAR(render(e, 8).l[7], 0.1 * G, 1e-5);
  CHECK(e.stop_voice(0, 60) == EngineStatus::kOk);
  CHECK(e.active_voices() == 2);  // ...it fires on note off
  VoiceInfo vi;
  bool saw_release_voice = false;
  for (std::uint32_t s = 0; s < 16; ++s)
    if (e.voice_info(s, vi) && vi.state == VoiceState::kReleasing) saw_release_voice = true;
  CHECK(saw_release_voice);
  CHECK(render(e, 4).l[3] / G > 0.19);  // the 0.2 DC voice is audible
  drain(e);
  // Uses the note-on velocity, not the (zero) note-off velocity.
  e.start_voice(0, 60, v16(64));
  render(e, 100);
  e.stop_voice(0, 60);
  const double rel_level = render(e, 4).l[3] / G;
  CHECK_NEAR(rel_level, 0.2 * (64.0 / 127.0) * (64.0 / 127.0) + 0.1 * (64.0 / 127.0) * (64.0 / 127.0) * std::exp(-9.0 / (0.006 * 48000.0) * 5),
             3e-3);
  drain(e);
  // sfizz rule: no attack voice alive any more (6000-frame sample finished) -> no release sample.
  e.start_voice(0, 60, v16(127));
  render(e, 30000);
  CHECK(e.active_voices() == 0);
  CHECK(e.stop_voice(0, 60) == EngineStatus::kVoiceNotFound);
  CHECK(e.active_voices() == 0);
  // A note off without a prior note on fires nothing either.
  CHECK(e.stop_voice(0, 60) == EngineStatus::kVoiceNotFound);
}

// --- Story 13.3: sample end, trigger=first / legato / release_key ----------------------------------------

// A sample of `len` frames: 0.1 up to frame end-2, 0.3 at frame end-1 (the last frame a region with `end` may play), 5.0 from frame `end` on.
static std::vector<float> end_probe(std::size_t len, std::size_t end) {
  std::vector<float> x(len, 5.0f);
  for (std::size_t i = 0; i < std::min(len, end); ++i) x[i] = i + 1 == end ? 0.3f : 0.1f;
  return x;
}
static float peak(const Out& o) {
  float p = 0.0f;
  for (float v : o.l) p = std::max(p, std::fabs(v));
  return p;
}

static void test_sample_end() {
  const double G = mono_center_gain();
  const std::size_t len = 1000;
  // Truncated: end=100 of a 1000 frame sample. The last played frame is 99 (0.3); the 5.0 behind it is never heard.
  {
    TestInstrument ins;
    ins.region(ins.add_sample(end_probe(len, 100)), 60, 60).has_end = true;
    ins.regions.back().end = 100;
    SampleEngine e(cfg());
    e.set_instrument(ins);
    CHECK(e.start_voice(0, 60, v16(127)) == EngineStatus::kOk);
    const Out o = render(e, 4000);
    CHECK(peak(o) <= 0.3 * G + 1e-4 && peak(o) > 0.25 * G);
    CHECK_NEAR(o.l[50], 0.1 * G, 1e-4);
    CHECK(e.active_voices() == 0);  // the voice ended by itself after the 6 ms release
    e.stop_voice(0, 60);
  }
  // end beyond the file: min(end, length) - 1 is the last frame, identical to a region without end (bit for bit).
  // end equal to the length: the same.
  {
    const std::vector<float> data = end_probe(len, len);  // 0.1 ... 0.3 at the last frame
    auto run = [&](bool has_end, std::uint64_t end) {
      TestInstrument ins;
      auto& r = ins.region(ins.add_sample(data), 60, 60);
      r.has_end = has_end;
      r.end = end;
      SampleEngine e(cfg());
      e.set_instrument(ins);
      e.start_voice(0, 60, v16(127));
      return render(e, 3000);
    };
    const Out none = run(false, 0), equal = run(true, len), beyond = run(true, 5000), beyond_big = run(true, std::uint64_t(1) << 40);
    CHECK(none.l == equal.l && none.r == equal.r);
    CHECK(none.l == beyond.l && none.r == beyond.r);
    CHECK(none.l == beyond_big.l);
    CHECK(peak(none) > 0.25 * G && peak(none) <= 0.3 * G + 1e-4);
    // One frame short of the length: the last played frame is len - 2 (0.1), so the 0.3 of the final frame is gone.
    const Out shorter = run(true, len - 1);
    CHECK(peak(shorter) <= 0.1 * G + 1e-4);
  }
  // end=0 disables the region: no voice, and it is not an unplayable region.
  {
    TestInstrument ins;
    ins.region(ins.dc(0.5f, 1000), 60, 60).has_end = true;
    ins.region(ins.dc(0.25f, 1000), 60, 60);
    SampleEngine e(cfg());
    e.set_instrument(ins);
    CHECK(e.unplayable_regions() == 0);
    CHECK(e.start_voice(0, 60, v16(127)) == EngineStatus::kOk);
    CHECK(e.active_voices() == 1);
    CHECK(e.skipped_unplayable_starts() == 0);
    CHECK_NEAR(render(e, 8).l[5], 0.25 * G, 1e-4);  // only the region without end sounds
  }
  // A loop must lie inside the played part: loop_continuous without loop points loops [0, end), never past it.
  {
    TestInstrument ins;
    auto& r = ins.region(ins.add_sample(end_probe(len, 20)), 60, 60);
    r.has_end = true;
    r.end = 20;
    r.loop_mode = ports::SampleLoopMode::kLoopContinuous;
    SampleEngine e(cfg());
    e.set_instrument(ins);
    e.start_voice(0, 60, v16(127));
    const Out o = render(e, 2000);
    CHECK(peak(o) <= 0.3 * G + 1e-4);  // the 5.0 behind `end` is never reached
    CHECK(e.active_voices() == 1);     // and it still loops
  }
  // An offset at or past the end cannot play: unplayable, counted, silent.
  {
    TestInstrument ins;
    auto& r = ins.region(ins.dc(0.5f, 1000), 60, 60);
    r.has_end = true;
    r.end = 10;
    r.offset = 10;
    SampleEngine e(cfg());
    e.set_instrument(ins);
    CHECK(e.unplayable_regions() == 1);
  }
  // A sample with a tail (only a 400 frame head resident, 5000 stored): end inside the head ends the voice there without an underrun; end beyond the head
  // is still an underrun at the head end, as without `end`.
  {
    auto run = [&](std::uint64_t end, bool has_end) {
      TestInstrument ins;
      const std::uint32_t si = ins.add_sample(end_probe(400, 300));  // 0.1 .. frame 298, 0.3 at frame 299, 5.0 from frame 300 (inside the head)
      ins.pcm[si].total_frames = 5000;
      ins.pcm[si].has_tail = true;
      auto& r = ins.region(si, 60, 60);
      r.has_end = has_end;
      r.end = end;
      SampleEngine e(cfg());
      e.set_instrument(ins);
      e.start_voice(0, 60, v16(127));
      const Out o = render(e, 6000);
      return std::make_tuple(peak(o), e.stream_underrun_events(), e.active_voices());
    };
    const auto inside = run(300, true);
    CHECK(std::get<0>(inside) <= 0.3 * G + 1e-4 && std::get<0>(inside) > 0.25 * G);  // last played frame is 299
    CHECK(std::get<1>(inside) == 0 && std::get<2>(inside) == 0);
    const auto at_head = run(400, true);  // end at the head end: still a clean end
    CHECK(std::get<1>(at_head) == 0 && std::get<2>(at_head) == 0);
    const auto beyond = run(2000, true);
    CHECK(std::get<1>(beyond) == 1 && std::get<2>(beyond) == 0);
    const auto none = run(0, false);
    CHECK(std::get<1>(none) == 1);
  }
}

static void test_all_keys_released_clears_held_keys() {
  const double G = mono_center_gain();
  TestInstrument ins;
  ins.region(ins.dc(0.1f, 40000), 60, 64);
  { auto& r = ins.region(ins.dc(0.2f, 40000), 60, 64); r.trigger = ports::SampleTrigger::kFirst; }
  { auto& r = ins.region(ins.dc(0.4f, 40000), 60, 64); r.trigger = ports::SampleTrigger::kLegato; }
  SampleEngine e(cfg());
  e.set_instrument(ins);
  e.start_voice(0, 60, v16(127));  // the key up never arrives (all notes off / panic instead)
  drain(e);
  e.start_voice(0, 62, v16(127));
  CHECK_NEAR(render(e, 8).l[7] / G, 0.5, 1e-3);  // stuck key: attack + legato
  drain(e);
  e.start_voice(0, 60, v16(127));
  e.all_keys_released(0);
  drain(e);
  e.start_voice(0, 62, v16(127));
  CHECK_NEAR(render(e, 8).l[7] / G, 0.3, 1e-3);  // keys forgotten: attack + first
  // Another channel's keys are not touched.
  drain(e);
  e.start_voice(1, 60, v16(127));
  e.all_keys_released(0);
  e.start_voice(1, 62, v16(127));
  CHECK(e.active_voices() == 4);
}

static void test_first_and_legato() {
  const double G = mono_center_gain();
  TestInstrument ins;
  ins.region(ins.dc(0.1f, 40000), 60, 64);
  { auto& r = ins.region(ins.dc(0.2f, 40000), 60, 64); r.trigger = ports::SampleTrigger::kFirst; }
  { auto& r = ins.region(ins.dc(0.4f, 40000), 60, 64); r.trigger = ports::SampleTrigger::kLegato; }
  SampleEngine e(cfg());
  e.set_instrument(ins);
  auto level = [&](std::uint32_t note) {  // level one note adds on its own, with the others silenced by a fresh engine state
    (void)note;
    return render(e, 8).l[7] / G;
  };
  // A first key: attack + first.
  e.start_voice(0, 60, v16(127));
  CHECK(e.active_voices() == 2);
  CHECK_NEAR(level(60), 0.3, 1e-3);
  // A second key while the first is still down: attack + legato (not first).
  e.start_voice(0, 62, v16(127));
  CHECK(e.active_voices() == 4);
  CHECK_NEAR(level(62), 0.3 + 0.1 + 0.4, 1e-3);
  // Both keys come up (the engine is told at the physical key up, then the voice stops): the next key starts a phrase again.
  e.key_released(0, 60);
  e.key_released(0, 62);
  e.stop_voice(0, 60);
  e.stop_voice(0, 62);
  drain(e);
  CHECK(e.active_voices() == 0);
  e.start_voice(0, 64, v16(127));
  CHECK(e.active_voices() == 2);
  CHECK_NEAR(level(64), 0.3, 1e-3);
  // The sustain pedal keeps voices sounding but not keys down: a new key after the physical key up is a first note.
  e.key_released(0, 64);  // (stop_voice is deferred by the pedal: the voices keep sounding)
  e.start_voice(0, 61, v16(127));
  CHECK(e.active_voices() == 4);  // 2 old voices + attack + first (not legato)
  // A key retriggered while still down does not count itself as "another key".
  e.key_released(0, 61);
  drain(e);
  e.start_voice(0, 63, v16(127));
  e.start_voice(0, 63, v16(127));
  CHECK(e.active_voices() == 4);  // two note-ons of the same key, both first
  drain(e);
  // A key up for a key that was never down changes nothing.
  e.key_released(0, 70);
  e.start_voice(0, 60, v16(127));
  CHECK(e.active_voices() == 2);
}

static void test_release_key_vs_release_with_pedal() {
  const double G = mono_center_gain();
  TestInstrument ins;
  ins.region(ins.dc(0.1f, 60000), 60, 60);
  { auto& r = ins.region(ins.dc(0.2f, 60000), 60, 60); r.trigger = ports::SampleTrigger::kRelease; }
  { auto& r = ins.region(ins.dc(0.4f, 60000), 60, 60); r.trigger = ports::SampleTrigger::kReleaseKey; }
  auto run = [&](bool pedal) {
    auto br = std::make_unique<BlockRenderer>();
    SampleEngine e(cfg());
    e.set_instrument(ins);
    br->set_channel_engine(2, static_cast<std::uint32_t>(EngineTypeId::kSample));
    CHECK(br->bind_engine(e));
    float l[kBlockFrames], r[kBlockFrames];
    auto block = [&]() { br->process_block(l, r); };
    if (pedal) br->submit({make_control_change(0, 2, Ump::kControllerSustain, 0xFFFFFFFFu), 0});
    br->submit({make_note_on(0, 2, 60, 0xFFFF), 0});
    block();
    CHECK(e.active_voices() == 1);  // neither release region fires on note on
    br->submit({make_note_off(0, 2, 60), 0});
    block();
    struct R { std::uint32_t after_key_up, after_pedal_up; double level_key_up; };
    R out{};
    out.after_key_up = e.active_voices();
    out.level_key_up = static_cast<double>(l[kBlockFrames - 1]) / G;
    if (pedal) {
      br->submit({make_control_change(0, 2, Ump::kControllerSustain, 0u), 0});
      block();
    }
    out.after_pedal_up = e.active_voices();
    return out;
  };
  // Pedal down: the key up fires release_key at once (attack voice held by the pedal + release_key = 2 voices); release waits.
  const auto held = run(true);
  CHECK(held.after_key_up == 2);
  CHECK(held.level_key_up > 0.1 + 0.4 - 0.02 && held.level_key_up < 0.1 + 0.4 + 0.02);  // attack 0.1 + release_key 0.4, no release 0.2
  CHECK(held.after_pedal_up == 3);                                                          // the pedal up lets release fire
  // No pedal: both fire at the key up.
  const auto free_ = run(false);
  CHECK(free_.after_key_up == 3);
  CHECK(free_.after_pedal_up == 3);
}

static void test_release_key_gates() {
  auto make = [](auto&& tweak) {
    auto ins = std::make_unique<TestInstrument>();
    ins->region(ins->dc(0.1f, 60000), 60, 60);
    auto& r = ins->region(ins->dc(0.4f, 60000), 60, 60);
    r.trigger = ports::SampleTrigger::kReleaseKey;
    tweak(r);
    return ins;
  };
  // vel_hi below the note-on velocity: nothing fires; inside the range it does.
  {
    auto ins = make([](ports::SampleRegionDesc& r) { r.vel_hi = 50; });
    SampleEngine e(cfg());
    e.set_instrument(*ins);
    e.start_voice(0, 60, v16(127));
    CHECK(!e.key_released(0, 60) && e.active_voices() == 1);
    e.stop_voice(0, 60);
    drain(e);
    e.start_voice(0, 60, v16(40));
    CHECK(e.key_released(0, 60) && e.active_voices() == 2);
  }
  // seq_length=2: fires for every second note on.
  {
    auto ins = make([](ports::SampleRegionDesc& r) { r.seq_length = 2; r.seq_position = 1; });
    SampleEngine e(cfg());
    e.set_instrument(*ins);
    bool fired[4];
    for (bool& f : fired) {
      e.start_voice(0, 60, v16(100));
      f = e.key_released(0, 60);
      e.stop_voice(0, 60);
      drain(e);
    }
    CHECK(fired[0] && !fired[1] && fired[2] && !fired[3]);
  }
  // A key up without a note on, or twice, fires nothing.
  {
    auto ins = make([](ports::SampleRegionDesc&) {});
    SampleEngine e(cfg());
    e.set_instrument(*ins);
    CHECK(!e.key_released(0, 60));
    e.start_voice(0, 60, v16(100));
    CHECK(e.key_released(0, 60) && !e.key_released(0, 60));
  }
  // A bank without release_key regions: a key up leaves the random sequence (and so the render) untouched.
  {
    TestInstrument ins;
    { auto& r = ins.region(ins.dc(0.1f, 3000), 60, 60); r.hirand = 0.5f; }
    { auto& r = ins.region(ins.dc(0.4f, 3000), 60, 60); r.lorand = 0.5f; }
    auto run = [&](bool key_ups) {
      SampleEngine e(cfg());
      e.set_instrument(ins);
      Out all;
      for (int n = 0; n < 12; ++n) {
        e.start_voice(0, 60, v16(100));
        const Out a = render(e, 500);
        all.l.insert(all.l.end(), a.l.begin(), a.l.end());
        if (key_ups) CHECK(!e.key_released(0, 60));
        e.stop_voice(0, 60);
        const Out b = render(e, 4000);
        all.l.insert(all.l.end(), b.l.begin(), b.l.end());
      }
      return all;
    };
    CHECK(run(true).l == run(false).l);
  }
}

// --- Story 13.4: velocity and amplitude model --------------------------------------------------------

template <typename F>
static double amp_level(F setup, std::uint32_t vel7, int key = 60, int lo = 48, int hi = 84) {
  TestInstrument ins;
  auto& r = ins.region(ins.dc(1.0f, 5000), lo, hi, 60);
  setup(r);
  SampleEngine e(cfg());
  e.set_instrument(ins);
  e.start_voice(0, static_cast<std::uint32_t>(key), v16(vel7));
  return static_cast<double>(render(e, 4).l[3]);
}
static double vsq(std::uint32_t vel7) { return (vel7 / 127.0) * (vel7 / 127.0); }

static void test_velocity_and_amplitude_model() {
  const double G = mono_center_gain();
  // A region without the opcodes keeps the velocity-squared law bit for bit; an unused amp_keycenter does not change it either.
  for (const std::uint32_t vel : {1u, 17u, 64u, 100u, 127u}) {
    const double plain = amp_level([](ports::SampleRegionDesc&) {}, vel);
    CHECK_NEAR(plain, G * vsq(vel), 1e-5);
    CHECK(amp_level([](ports::SampleRegionDesc& r) { r.amp_veltrack = 100.0f; r.amp_keycenter = 30; }, vel) == plain);
    CHECK(amp_level([](ports::SampleRegionDesc& r) { r.amplitude = 100.0f; r.amp_keytrack = 0.0f; r.vel2attack = 0.0f; }, vel) == plain);
  }
  // amp_veltrack: 0 = no velocity law, 50 = halfway, negative = inverted.
  for (const std::uint32_t vel : {1u, 40u, 64u, 127u}) {
    const double v2 = vsq(vel);
    CHECK_NEAR(amp_level([](ports::SampleRegionDesc& r) { r.amp_veltrack = 0.0f; }, vel), G, 1e-5);
    CHECK_NEAR(amp_level([](ports::SampleRegionDesc& r) { r.amp_veltrack = 50.0f; }, vel), G * (1.0 - 0.5 * (1.0 - v2)), 1e-5);
    CHECK_NEAR(amp_level([](ports::SampleRegionDesc& r) { r.amp_veltrack = -100.0f; }, vel), G * (1.0 - v2), 1e-5);
    CHECK_NEAR(amp_level([](ports::SampleRegionDesc& r) { r.amp_veltrack = -50.0f; }, vel), G * 0.5 * (1.0 - v2), 1e-5);
  }
  // amp_velcurve_N: ends default to (0, 0) and (127, 1), gaps are filled linearly, the value between two velocities is interpolated by sfizz's rule
  // (here velocities are whole, so the table entry itself).
  {
    auto curve = [](std::initializer_list<std::pair<int, float>> pts, float veltrack = 100.0f) {
      return [pts, veltrack](ports::SampleRegionDesc& r) {
        r.amp_veltrack = veltrack;
        for (const auto& [v, g] : pts) r.vel_points[r.vel_point_count++] = {static_cast<std::uint8_t>(v), g};
      };
    };
    const auto c = curve({{1, 0.2f}, {64, 0.9f}});
    CHECK_NEAR(amp_level(c, 1), G * 0.2, 1e-5);
    CHECK_NEAR(amp_level(c, 64), G * 0.9, 1e-5);
    CHECK_NEAR(amp_level(c, 127), G * 1.0, 1e-5);
    CHECK_NEAR(amp_level(c, 32), G * (0.2 + (0.9 - 0.2) * 31.0 / 63.0), 1e-5);  // ramp from point 1 to point 64
    CHECK_NEAR(amp_level(c, 100), G * (0.9 + (1.0 - 0.9) * 36.0 / 63.0), 1e-5);  // ramp from point 64 to the default end (127, 1)
    CHECK_NEAR(amp_level(curve({{30, 0.5f}}), 15), G * (0.5 * 15.0 / 30.0), 1e-5);  // only a middle point: the default start (0, 0) ramps to it
    CHECK_NEAR(amp_level(curve({{0, 1.0f}, {127, 0.0f}}), 20), G * (1.0 - 20.0 / 127.0), 1e-5);  // an inverted curve
    CHECK_NEAR(amp_level(curve({{1, 0.5f}, {127, 0.5f}}, 50.0f), 90), G * (1.0 - 0.5 * (1.0 - 0.5)), 1e-5);  // curve and a partial veltrack
    CHECK_NEAR(amp_level(curve({{64, 1.0f}}, -100.0f), 32), G * (1.0 - 32.0 / 64.0), 1e-5);  // curve and a negative veltrack
    CHECK(amp_level(curve({{40, 0.25f}, {80, 0.75f}}), 77) == amp_level(curve({{40, 0.25f}, {80, 0.75f}}), 77));  // deterministic
  }
  // amplitude is a plain gain (percent); amplitude=0 silences the region; it multiplies volume.
  CHECK_NEAR(amp_level([](ports::SampleRegionDesc& r) { r.amplitude = 50.0f; }, 127), 0.5 * G, 1e-5);
  CHECK_NEAR(amp_level([](ports::SampleRegionDesc& r) { r.amplitude = 150.0f; }, 127), 1.5 * G, 1e-5);
  CHECK(amp_level([](ports::SampleRegionDesc& r) { r.amplitude = 0.0f; }, 127) == 0.0);
  CHECK_NEAR(amp_level([](ports::SampleRegionDesc& r) { r.amplitude = 50.0f; r.volume_db = -6.0206f; }, 127), 0.25 * G, 1e-5);
  // amp_keytrack: dB per key around amp_keycenter (60 by default).
  for (const int key : {48, 60, 66, 84}) {
    CHECK_NEAR(amp_level([](ports::SampleRegionDesc& r) { r.amp_keytrack = 0.5f; }, 127, key), G * std::pow(10.0, 0.5 * (key - 60) / 20.0), 1e-4);
    CHECK_NEAR(amp_level([](ports::SampleRegionDesc& r) { r.amp_keytrack = -1.0f; r.amp_keycenter = 72; }, 127, key), G * std::pow(10.0, -1.0 * (key - 72) / 20.0), 1e-4);
  }
  // Envelope scalars: sfizz EGDescription: value + velocity * vel2*, at note-on (velocity = vel7 / 127).
  const double sr = 48000.0;
  auto render_voice = [&](auto setup, std::uint32_t vel7, std::uint32_t frames) {
    TestInstrument ins;
    auto& r = ins.region(ins.dc(1.0f, 400000), 60, 60);
    setup(r);
    SampleEngine e(cfg());
    e.set_instrument(ins);
    e.start_voice(0, 60, v16(vel7));
    return render(e, frames);
  };
  {  // vel2attack: a linear attack of ampeg_attack + vf * vel2attack seconds
    auto at = [&](std::uint32_t vel) {
      return render_voice([](ports::SampleRegionDesc& r) { r.ampeg_attack = 0.01f; r.vel2attack = 0.02f; r.amp_veltrack = 0.0f; }, vel, 4000);
    };
    for (const std::uint32_t vel : {127u, 64u, 1u}) {
      const double t = 0.01 + 0.02 * (vel / 127.0), step = 1.0 / (sr * t);
      const Out o = at(vel);
      for (const std::uint32_t f : {0u, 99u, 250u}) CHECK_NEAR(o.l[f] / G, std::min(1.0, (f + 1) * step), 2e-5);
    }
    // The same attack without vel2attack is not velocity dependent; a negative total attack is instantaneous (sfizz: time <= 0 -> rate 1).
    const Out flat = render_voice([](ports::SampleRegionDesc& r) { r.ampeg_attack = 0.01f; r.amp_veltrack = 0.0f; }, 127, 600);
    CHECK_NEAR(flat.l[99] / G, 100.0 / 480.0, 2e-5);
    const Out neg = render_voice([](ports::SampleRegionDesc& r) { r.ampeg_attack = 0.01f; r.vel2attack = -0.05f; r.amp_veltrack = 0.0f; }, 127, 8);
    CHECK_NEAR(neg.l[0] / G, 1.0, 1e-6);
  }
  {  // vel2decay: exponential decay time ampeg_decay + vf * vel2decay
    for (const std::uint32_t vel : {127u, 64u}) {
      const Out o = render_voice([](ports::SampleRegionDesc& r) { r.ampeg_decay = 0.01f; r.vel2decay = 0.02f; r.ampeg_sustain = 50.0f; r.amp_veltrack = 0.0f; }, vel, 3000);
      const double t = 0.01 + 0.02 * (vel / 127.0), rate = std::exp(-9.0 / (t * sr));
      for (const std::uint32_t f : {3u, 20u, 60u}) CHECK_NEAR(o.l[f + 1] / o.l[f], rate, 1e-5);
    }
  }
  {  // vel2sustain: percent added at velocity 127, the level is clamped to 0..100 %
    for (const std::uint32_t vel : {127u, 64u, 30u}) {
      const Out o = render_voice([](ports::SampleRegionDesc& r) { r.ampeg_decay = 0.005f; r.ampeg_sustain = 80.0f; r.vel2sustain = -50.0f; r.amp_veltrack = 0.0f; }, vel, 20000);
      CHECK_NEAR(o.l[19999] / G, (80.0 - 50.0 * (vel / 127.0)) / 100.0, 1e-4);
    }
    const Out up = render_voice([](ports::SampleRegionDesc& r) { r.ampeg_decay = 0.005f; r.ampeg_sustain = 80.0f; r.vel2sustain = 50.0f; r.amp_veltrack = 0.0f; }, 127, 20000);
    CHECK_NEAR(up.l[19999] / G, 1.0, 1e-5);  // clamped to 100 %
    TestInstrument zero;  // sustain below zero: clamped to 0, which releases by itself like any zero sustain
    { auto& rg = zero.region(zero.dc(1.0f, 400000), 60, 60); rg.ampeg_decay = 0.01f; rg.ampeg_sustain = 20.0f; rg.vel2sustain = -50.0f; }
    SampleEngine ez(cfg());
    ez.set_instrument(zero);
    ez.start_voice(0, 60, v16(127));
    render(ez, 96000);
    CHECK(ez.active_voices() == 0);
    TestInstrument kept;  // the same region at velocity 1: sustain 20 - 0.4 = 19.6 %, the voice keeps sounding
    { auto& rg = kept.region(kept.dc(1.0f, 400000), 60, 60); rg.ampeg_decay = 0.01f; rg.ampeg_sustain = 20.0f; rg.vel2sustain = -50.0f; }
    SampleEngine ek(cfg());
    ek.set_instrument(kept);
    ek.start_voice(0, 60, v16(1));
    render(ek, 96000);
    CHECK(ek.active_voices() == 1);
  }
  {  // vel2release: exponential release time ampeg_release + vf * vel2release, evaluated at note-on
    for (const std::uint32_t vel : {127u, 64u}) {
      TestInstrument ins;
      { auto& rg = ins.region(ins.dc(1.0f, 400000), 60, 60); rg.ampeg_release = 0.02f; rg.vel2release = 0.03f; rg.amp_veltrack = 0.0f; }
      SampleEngine e(cfg());
      e.set_instrument(ins);
      e.start_voice(0, 60, v16(vel));
      render(e, 200);
      e.stop_voice(0, 60);
      const Out rel = render(e, 2000);
      const double t = 0.02 + 0.03 * (vel / 127.0), rate = std::exp(-9.0 / (t * sr));
      for (const std::uint32_t f : {10u, 100u, 400u}) CHECK_NEAR(rel.l[f + 1] / rel.l[f], rate, 1e-5);
    }
  }
  {  // a region with vel2 scalars at zero (and a plain one) renders the old envelope bit for bit
    auto base = [](ports::SampleRegionDesc& r) { r.ampeg_attack = 0.01f; r.ampeg_decay = 0.02f; r.ampeg_sustain = 40.0f; r.ampeg_release = 0.02f; };
    const Out a = render_voice(base, 90, 6000);
    const Out b = render_voice([&](ports::SampleRegionDesc& r) { base(r); r.vel2attack = 0.0f; r.vel2sustain = 0.0f; r.amp_keycenter = 20; }, 90, 6000);
    CHECK(a.l == b.l && a.r == b.r);
  }
  {  // a part's attack / release override wins over vel2attack / vel2release and does not depend on velocity
    for (const std::uint32_t vel : {127u, 20u}) {
      TestInstrument ins;
      { auto& rg = ins.region(ins.dc(1.0f, 400000), 60, 60); rg.ampeg_attack = 0.01f; rg.vel2attack = 0.05f; rg.ampeg_release = 0.02f; rg.vel2release = 0.03f; rg.amp_veltrack = 0.0f; }
      SampleEngine e(cfg());
      e.set_instrument(ins);
      e.set_part_param_edit(0, velora::core::params::kPartAttack, 100.0f);
      e.set_part_param_edit(0, velora::core::params::kPartRelease, 50.0f);
      e.start_voice(0, 60, v16(vel));
      const Out o = render(e, 400);
      for (const std::uint32_t f : {0u, 99u, 300u}) CHECK_NEAR(o.l[f] / G, (f + 1) / (sr * 0.1), 2e-5);
      e.stop_voice(0, 60);
      const Out rel = render(e, 1000);
      for (const std::uint32_t f : {10u, 100u}) CHECK_NEAR(rel.l[f + 1] / rel.l[f], std::exp(-9.0 / (0.05 * sr)), 1e-5);
    }
  }
  {  // extreme opcodes stay finite
    TestInstrument ins;
    auto& rg = ins.region(ins.dc(1.0f, 2000), 0, 127, 60);
    rg.amp_keytrack = 12.0f; rg.amp_keycenter = 0; rg.amplitude = 10000.0f; rg.volume_db = 48.0f;
    SampleEngine e(cfg());
    e.set_instrument(ins);
    e.start_voice(0, 127, v16(127));
    const Out o = render(e, 64);
    bool fin = true;
    for (float x : o.l) fin = fin && std::isfinite(x);
    for (float x : o.r) fin = fin && std::isfinite(x);
    CHECK(fin && peak(o) > 1.0f);
  }
  // Insane maps make the region unplayable (counted), they never reach the audio thread.
  {
    auto unplayable = [](auto setup) {
      TestInstrument ins;
      setup(ins.region(ins.dc(1.0f, 100), 60, 60));
      SampleEngine e(cfg());
      e.set_instrument(ins);
      return e.unplayable_regions();
    };
    CHECK(unplayable([](ports::SampleRegionDesc& r) { r.amp_veltrack = std::numeric_limits<float>::quiet_NaN(); }) == 1);
    CHECK(unplayable([](ports::SampleRegionDesc& r) { r.amplitude = -1.0f; }) == 1);
    CHECK(unplayable([](ports::SampleRegionDesc& r) { r.amp_keytrack = 50.0f; }) == 1);
    CHECK(unplayable([](ports::SampleRegionDesc& r) { r.vel2release = 1000.0f; }) == 1);
    CHECK(unplayable([](ports::SampleRegionDesc& r) { r.vel_point_count = 1; r.vel_points[0] = {5, 2.0f}; }) == 1);
    CHECK(unplayable([](ports::SampleRegionDesc& r) { r.vel_point_count = 2; r.vel_points[0] = {9, 0.5f}; r.vel_points[1] = {9, 0.6f}; }) == 1);
    CHECK(unplayable([](ports::SampleRegionDesc& r) { r.vel_point_count = 200; }) == 1);
    CHECK(unplayable([](ports::SampleRegionDesc& r) { r.amp_veltrack = 50.0f; r.vel_point_count = 1; r.vel_points[0] = {5, 0.5f}; }) == 0);
  }
  // Two voices of one region at different velocities are evaluated independently and the render is deterministic.
  {
    auto run = [&]() {
      TestInstrument ins;
      auto& r = ins.region(ins.dc(1.0f, 20000), 48, 84, 60);
      r.amp_veltrack = 70.0f; r.amp_keytrack = 0.2f; r.vel_point_count = 2; r.vel_points[0] = {20, 0.3f}; r.vel_points[1] = {100, 0.8f};
      r.ampeg_attack = 0.003f; r.ampeg_decay = 0.01f; r.ampeg_sustain = 60.0f; r.vel2attack = 0.01f; r.vel2decay = 0.02f; r.vel2sustain = -30.0f; r.vel2release = 0.05f;
      SampleEngine e(cfg());
      e.set_instrument(ins);
      e.start_voice(0, 60, v16(30));
      e.start_voice(0, 67, v16(120));
      Out o = render(e, 3000);
      e.stop_voice(0, 60);
      const Out tail = render(e, 3000);
      o.l.insert(o.l.end(), tail.l.begin(), tail.l.end());
      return o;
    };
    const Out a = run(), b = run();
    CHECK(a.l == b.l);
    CHECK(peak(a) > 0.1f);
  }
}

struct LR {
  double l, r;
};

// --- Story 13.5: CC gates, pseudo-CCs and CC modulation at voice start ---------------------------------------------------------

static std::uint32_t cc32(double units) { return static_cast<std::uint32_t>(std::llround(units * 4294967295.0 / 127.0)); }

// Level of a DC voice (amplitude, volume mods) after `setup`, with the given controller values set before the note. Mono centred, vel7 127 and amp_veltrack 0 by default.
template <typename F>
static double cc_level(F setup, std::initializer_list<std::pair<int, double>> ccs, std::uint32_t vel7 = 127, int key = 60) {
  TestInstrument ins;
  auto& r = ins.region(ins.dc(1.0f, 5000), 48, 84, 60);
  r.amp_veltrack = 0.0f;
  setup(r, ins);
  SampleEngine e(cfg());
  e.set_instrument(ins);
  for (const auto& [cc, v] : ccs) e.set_part_cc(0, static_cast<std::uint32_t>(cc), cc32(v));
  e.start_voice(0, static_cast<std::uint32_t>(key), v16(vel7));
  return static_cast<double>(render(e, 4).l[3]);
}
using Cc = ports::SampleCcTarget;
static void add_mod(ports::SampleRegionDesc& r, Cc target, int cc, float depth, int curve = 0) {
  r.cc_mods[r.cc_mod_count++] = {target, static_cast<std::uint8_t>(cc), static_cast<std::uint8_t>(curve), depth};
}
static void add_gate(ports::SampleRegionDesc& r, int cc, float lo, float hi) { r.cc_gates[r.cc_gate_count++] = {static_cast<std::uint8_t>(cc), lo, hi}; }

static void test_cc_gates_and_seed() {
  const double G = mono_center_gain();
  // Boundaries: locc64=64 hicc64=100 is [64, nextafter(101, 0)] in CC units: 64 and 100 play, 63 and 101 do not (the importer's fill-gap form).
  auto gate = [](ports::SampleRegionDesc& r, TestInstrument&) { add_gate(r, 64, 64.0f, std::nextafter(101.0f, 0.0f)); };
  for (const auto& [v, plays] : std::initializer_list<std::pair<double, bool>>{{0, false}, {63, false}, {63.5, false}, {64, true}, {64.5, true}, {100, true}, {100.75, true}, {101, false}, {127, false}}) {
    const double l = cc_level(gate, {{64, v}});
    CHECK(plays ? std::fabs(l - G) < 1e-5 : l == 0.0);
  }
  // hdcc: a fractional end is inclusive as written.
  auto hd = [](ports::SampleRegionDesc& r, TestInstrument&) { add_gate(r, 1, 63.5f, 95.25f); };
  CHECK(cc_level(hd, {{1, 63.5}}) > 0.0 && cc_level(hd, {{1, 95.25}}) > 0.0 && cc_level(hd, {{1, 63.0}}) == 0.0 && cc_level(hd, {{1, 96.0}}) == 0.0);
  // Every gate of a region must hold; each controller is its own condition.
  auto two = [](ports::SampleRegionDesc& r, TestInstrument&) { add_gate(r, 2, 10.0f, 20.0f); add_gate(r, 3, 0.0f, 5.0f); };
  CHECK(cc_level(two, {{2, 15}, {3, 5}}) > 0.0 && cc_level(two, {{2, 15}, {3, 6}}) == 0.0 && cc_level(two, {{2, 9}, {3, 0}}) == 0.0);
  // A CC change between notes: the gate follows the controller value at each note on, not a past one. Two layers split by a pedal-like controller.
  {
    TestInstrument ins;
    const std::uint32_t smp = ins.dc(1.0f, 20000), smp2 = ins.dc(0.5f, 20000);
    add_gate(ins.region(smp, 60, 60), 64, 0.0f, std::nextafter(64.0f, 0.0f));   // up
    add_gate(ins.region(smp2, 60, 60), 64, 64.0f, 127.0f);                       // down
    SampleEngine e(cfg());
    e.set_instrument(ins);
    auto level = [&]() { e.start_voice(0, 60, v16(127)); const double l = render(e, 8).l[7]; e.stop_voice(0, 60); drain(e); return l; };
    CHECK_NEAR(level(), G, 1e-5);                       // CC64 = 0 (default)
    e.set_part_cc(0, 64, cc32(127));
    CHECK_NEAR(level(), 0.5 * G, 1e-5);
    e.set_part_cc(0, 64, cc32(63));
    CHECK_NEAR(level(), G, 1e-5);
    // Per part: another part's controller does not gate this one.
    e.set_part_cc(1, 64, cc32(127));
    CHECK_NEAR(level(), G, 1e-5);
    // A held note keeps playing when the controller leaves the range (voice-start semantics).
    e.start_voice(0, 60, v16(127));
    e.set_part_cc(0, 64, cc32(127));
    CHECK_NEAR(render(e, 8).l[7], G, 1e-5);
    drain(e);
  }
  // set_cc seeds: the instrument's defaults are the part's initial controller values; a later controller message wins.
  {
    TestInstrument ins;
    add_gate(ins.region(ins.dc(1.0f, 20000), 60, 60), 64, 64.0f, 127.0f);
    ins.cc_defaults = {{64, 90.0f}, {100, 12.5f}};
    SampleEngine e(cfg());
    e.set_instrument(ins);
    CHECK(e.part_cc(0, 64) == 90.0f && e.part_cc(15, 64) == 90.0f && e.part_cc(3, 100) == 12.5f && e.part_cc(0, 7) == 100.0f);  // CC7 keeps sfizz's default
    e.start_voice(0, 60, v16(127));
    CHECK_NEAR(render(e, 8).l[7], G, 1e-5);
    drain(e);
    e.set_part_cc(0, 64, cc32(10));
    e.start_voice(0, 60, v16(127));
    CHECK(render(e, 8).l[7] == 0.0f);
    // A part that binds the bank later is seeded at the bind; binding the same bank again does not reseed.
    auto bank = e.prepare(ins);
    e.set_part_sample_bank(5, bank.get());
    CHECK(e.part_cc(5, 64) == 90.0f);
    e.set_part_cc(5, 64, cc32(1));
    e.set_part_sample_bank(5, bank.get());
    CHECK(e.part_cc(5, 64) == 1.0f);
    // Defaults outside 0..127 or non-finite are not trusted.
    ins.cc_defaults = {{64, 200.0f}, {65, std::numeric_limits<float>::quiet_NaN()}};
    SampleEngine e2(cfg());
    e2.set_instrument(ins);
    CHECK(e2.part_cc(0, 64) == 0.0f && e2.part_cc(0, 65) == 0.0f);
  }
  // A gate that always holds changes neither the audio nor the random sequence (bit-identical to the region without it).
  {
    auto build = [](bool gated) {
      auto ins = std::make_unique<TestInstrument>();
      for (int i = 0; i < 4; ++i) {
        auto& r = ins->region(ins->dc(0.1f * (i + 1), 20000), 60, 60);
        r.lorand = 0.25f * i;
        r.hirand = 0.25f * (i + 1);
        if (gated) add_gate(r, 64, 0.0f, 127.0f);
      }
      return ins;
    };
    auto a = build(false), b = build(true);
    SampleEngine ea(cfg()), eb(cfg());
    ea.set_instrument(*a);
    eb.set_instrument(*b);
    for (int n = 0; n < 24; ++n) {
      ea.start_voice(0, 60, v16(100));
      eb.start_voice(0, 60, v16(100));
      const Out oa = render(ea, 16), ob = render(eb, 16);
      CHECK(oa.l == ob.l && oa.r == ob.r);
      ea.stop_voice(0, 60);
      eb.stop_voice(0, 60);
      render(ea, 64);
      render(eb, 64);
    }
  }
  // Release-trigger regions are gated too (at the controller values of the note off).
  {
    TestInstrument ins;
    ins.region(ins.dc(0.0f, 20000), 60, 60);
    auto& rel = ins.region(ins.dc(1.0f, 20000), 60, 60);
    rel.trigger = ports::SampleTrigger::kRelease;
    add_gate(rel, 20, 50.0f, 127.0f);
    SampleEngine e(cfg());
    e.set_instrument(ins);
    for (const auto& [v, fires] : std::initializer_list<std::pair<double, bool>>{{0, false}, {80, true}}) {
      e.set_part_cc(0, 20, cc32(v));
      e.start_voice(0, 60, v16(127));
      render(e, 8);
      e.stop_voice(0, 60);
      const Out o = render(e, 64);
      CHECK(fires ? o.l[63] > 0.1f : o.l[63] == 0.0f);
      drain(e);
    }
  }
}

static void test_cc_modulation_at_voice_start() {
  const double G = mono_center_gain();
  // amplitude: gain x (depth / 100 x curve(CC)); the default curve is linear 0..1 over the CC range.
  auto amp = [](ports::SampleRegionDesc& r, TestInstrument&) { add_mod(r, Cc::kAmplitude, 74, 100.0f); };
  CHECK_NEAR(cc_level(amp, {{74, 0}}), 0.0, 1e-7);
  CHECK_NEAR(cc_level(amp, {{74, 64}}), G * 64.0 / 127.0, 1e-5);
  CHECK_NEAR(cc_level(amp, {{74, 127}}), G, 1e-5);
  CHECK_NEAR(cc_level([](ports::SampleRegionDesc& r, TestInstrument&) { add_mod(r, Cc::kAmplitude, 74, 50.0f); }, {{74, 127}}), 0.5 * G, 1e-5);
  // Two sources multiply. A depth of 0 on amplitude still silences (a multiplicative connection, as sfizz).
  CHECK_NEAR(cc_level([](ports::SampleRegionDesc& r, TestInstrument&) { add_mod(r, Cc::kAmplitude, 74, 100.0f); add_mod(r, Cc::kAmplitude, 75, 100.0f); }, {{74, 127}, {75, 127.0 / 2}}), G * 0.5, 1e-5);
  CHECK(cc_level([](ports::SampleRegionDesc& r, TestInstrument&) { add_mod(r, Cc::kAmplitude, 74, 0.0f); }, {{74, 127}}) == 0.0);
  // volume: dB added.
  auto vol = [](ports::SampleRegionDesc& r, TestInstrument&) { add_mod(r, Cc::kVolume, 11, -6.0206f); };
  CHECK_NEAR(cc_level(vol, {{11, 127}}), 0.5 * G, 1e-4);
  CHECK_NEAR(cc_level(vol, {{11, 0}}), G, 1e-5);
  // The CC is sampled once at voice start: a later change does not move a sounding voice.
  {
    TestInstrument ins;
    auto& r = ins.region(ins.dc(1.0f, 20000), 60, 60);
    r.amp_veltrack = 0.0f;
    add_mod(r, Cc::kAmplitude, 74, 100.0f);
    SampleEngine e(cfg());
    e.set_instrument(ins);
    e.set_part_cc(0, 74, cc32(127));
    e.start_voice(0, 60, v16(127));
    render(e, 16);
    e.set_part_cc(0, 74, cc32(0));
    CHECK_NEAR(render(e, 16).l[15], G, 1e-5);
    e.start_voice(0, 60, v16(127));  // a new note at CC 0 is silent, the first still sounds
    CHECK_NEAR(render(e, 16).l[15], G, 1e-5);
  }
  // Curves: a custom curve by index, the predefined ones, the linear default for an unknown index.
  auto with_curve = [&](int curve, double cc) {
    return cc_level([curve](ports::SampleRegionDesc& r, TestInstrument& ins) {
      add_mod(r, Cc::kAmplitude, 74, 100.0f, curve);
      ins.add_curve(7, {{0, 0.0f}, {64, 0.9f}, {127, 1.0f}});
      ins.add_curve(8, {{0, 1.0f}, {127, 0.0f}});
    }, {{74, cc}});
  };
  CHECK_NEAR(with_curve(7, 64), G * 0.9, 1e-4);
  CHECK_NEAR(with_curve(7, 32), G * 0.45, 1e-4);        // linear between (0, 0) and (64, 0.9)
  CHECK_NEAR(with_curve(8, 127), 0.0, 1e-6);
  CHECK_NEAR(with_curve(8, 0), G, 1e-5);
  CHECK_NEAR(with_curve(4, 64), G * std::pow(64.0 / 127.0, 2.0), 1e-4);                    // predefined 4: x squared
  CHECK_NEAR(with_curve(5, 64), G * std::sqrt(64.0 / 127.0), 1e-4);                        // predefined 5: square root
  CHECK_NEAR(with_curve(6, 64), G * std::sqrt(1.0 - 64.0 / 127.0), 1e-4);                  // predefined 6: inverse square root
  CHECK_NEAR(with_curve(2, 127), 0.0, 1e-6);                                               // predefined 2: 1 -> 0
  CHECK_NEAR(with_curve(200, 64), G * 64.0 / 127.0, 1e-5);                                 // unknown index: linear default
  CHECK_NEAR(with_curve(3, 127), -0.0, 1e-6);                                              // predefined 3 goes -1 at the top: gain is clamped at 0
  // pan and tune and offset: the same render as the region written with the summed constant.
  auto same = [&](auto mod_setup, auto const_setup, std::initializer_list<std::pair<int, double>> ccs, int key = 60, bool ramp = false) {
    auto run = [&](auto setup) {
      TestInstrument ins;
      auto& r = ins.region(ramp ? ins.ramp(40000, 0.0001f) : ins.dc(0.8f, 40000), 48, 84, 60);
      r.amp_veltrack = 0.0f;
      setup(r);
      SampleEngine e(cfg());
      e.set_instrument(ins);
      for (const auto& [cc, v] : ccs) e.set_part_cc(0, static_cast<std::uint32_t>(cc), cc32(v));
      e.start_voice(0, static_cast<std::uint32_t>(key), v16(127));
      return render(e, 600);
    };
    const Out a = run(mod_setup), b = run(const_setup);
    double worst = 0.0;
    for (std::size_t i = 0; i < a.l.size(); ++i) worst = std::max({worst, (double)std::fabs(a.l[i] - b.l[i]), (double)std::fabs(a.r[i] - b.r[i])});
    return worst;
  };
  CHECK(same([](ports::SampleRegionDesc& r) { add_mod(r, Cc::kPan, 10, 100.0f); }, [](ports::SampleRegionDesc& r) { r.pan = 100.0f; }, {{10, 127}}) < 1e-6);
  CHECK(same([](ports::SampleRegionDesc& r) { r.pan = -20.0f; add_mod(r, Cc::kPan, 10, 50.0f); }, [](ports::SampleRegionDesc& r) { r.pan = -20.0f + 50.0f * 64.0f / 127.0f; }, {{10, 64}}) < 1e-6);
  CHECK(same([](ports::SampleRegionDesc& r) { add_mod(r, Cc::kPan, 10, 100.0f); }, [](ports::SampleRegionDesc&) {}, {{10, 0}}) < 1e-7);  // CC 0 adds nothing
  CHECK(same([](ports::SampleRegionDesc& r) { add_mod(r, Cc::kTune, 94, 1200.0f); }, [](ports::SampleRegionDesc& r) { r.tune_cents = 1200.0f; }, {{94, 127}}, 60, true) < 1e-6);
  CHECK(same([](ports::SampleRegionDesc& r) { r.tune_cents = 7.0f; add_mod(r, Cc::kTune, 94, -100.0f); }, [](ports::SampleRegionDesc& r) { r.tune_cents = -93.0f; }, {{94, 127}}, 60, true) < 1e-6);
  CHECK(same([](ports::SampleRegionDesc& r) { add_mod(r, Cc::kOffset, 73, 1000.0f); }, [](ports::SampleRegionDesc& r) { r.offset = 1000; }, {{73, 127}}, 60, true) < 1e-7);
  CHECK(same([](ports::SampleRegionDesc& r) { r.offset = 100; add_mod(r, Cc::kOffset, 73, 1000.0f); }, [](ports::SampleRegionDesc& r) { r.offset = 600; }, {{73, 127.0 / 2}}, 60, true) < 1e-7);  // int64(1000 * 0.5)
  // An offset beyond the sample is clamped to its last resident frame, never past it.
  {
    TestInstrument ins;
    auto& r = ins.region(ins.ramp(1000, 0.001f), 60, 60);
    r.amp_veltrack = 0.0f;
    add_mod(r, Cc::kOffset, 73, 2.0e9f);
    SampleEngine e(cfg());
    e.set_instrument(ins);
    e.set_part_cc(0, 73, cc32(127));
    e.start_voice(0, 60, v16(127));
    const Out o = render(e, 64);
    CHECK(std::isfinite(o.l[0]) && std::fabs(o.l[0]) <= 1.2 * G);
  }
  // Envelope entries: seconds (percent for start and sustain) added to the region's value, same render as the constant.
  auto eg = [&](Cc target, float depth, double cc, auto const_setup, float base_attack = 0.0f) {
    return same([=](ports::SampleRegionDesc& r) { r.ampeg_attack = base_attack; r.ampeg_decay = 0.02f; r.ampeg_sustain = 60.0f; r.ampeg_release = 0.01f; add_mod(r, target, 100, depth); },
                [=](ports::SampleRegionDesc& r) { r.ampeg_attack = base_attack; r.ampeg_decay = 0.02f; r.ampeg_sustain = 60.0f; r.ampeg_release = 0.01f; const_setup(r); }, {{100, cc}});
  };
  CHECK(eg(Cc::kEgAttack, 0.01f, 127, [](ports::SampleRegionDesc& r) { r.ampeg_attack = 0.01f; }) < 1e-6);
  CHECK(eg(Cc::kEgAttack, 0.02f, 127, [](ports::SampleRegionDesc& r) { r.ampeg_attack = 0.025f; }, 0.005f) < 1e-6);
  CHECK(eg(Cc::kEgDelay, 0.004f, 127, [](ports::SampleRegionDesc& r) { r.ampeg_delay = 0.004f; }) < 1e-6);
  CHECK(eg(Cc::kEgHold, 0.003f, 127, [](ports::SampleRegionDesc& r) { r.ampeg_hold = 0.003f; }) < 1e-6);
  CHECK(eg(Cc::kEgDecay, 0.03f, 127, [](ports::SampleRegionDesc& r) { r.ampeg_decay = 0.05f; }) < 1e-6);
  CHECK(eg(Cc::kEgSustain, -40.0f, 127, [](ports::SampleRegionDesc& r) { r.ampeg_sustain = 20.0f; }) < 1e-6);
  CHECK(eg(Cc::kEgStart, 30.0f, 127, [](ports::SampleRegionDesc& r) { r.ampeg_start = 30.0f; r.ampeg_delay = 0.0f; }) < 1e-6);
  CHECK(eg(Cc::kEgAttack, 0.01f, 0, [](ports::SampleRegionDesc&) {}) < 1e-7);  // CC 0 adds nothing
  CHECK(eg(Cc::kEgSustain, -40.0f, 127.0 / 2, [](ports::SampleRegionDesc& r) { r.ampeg_sustain = 40.0f; }) < 1e-5);  // CC 63.5 is half the range: 60 - 40 x 0.5
  {  // release: read when the voice starts, applied when it is released
    auto rel = [&](float depth, double cc, float base, bool const_form) {
      TestInstrument ins;
      auto& r = ins.region(ins.dc(0.8f, 80000), 60, 60);
      r.amp_veltrack = 0.0f;
      r.ampeg_release = base;
      if (const_form) r.ampeg_release = base + depth;
      else add_mod(r, Cc::kEgRelease, 100, depth);
      SampleEngine e(cfg());
      e.set_instrument(ins);
      e.set_part_cc(0, 100, cc32(cc));
      e.start_voice(0, 60, v16(127));
      render(e, 100);
      e.set_part_cc(0, 100, cc32(0));  // moving the CC after the note on changes nothing
      e.stop_voice(0, 60);
      return render(e, 20000);
    };
    const Out m = rel(0.2f, 127, 0.05f, false), c = rel(0.2f, 127, 0.05f, true);
    double worst = 0.0;
    for (std::size_t i = 0; i < m.l.size(); ++i) worst = std::max(worst, (double)std::fabs(m.l[i] - c.l[i]));
    CHECK(worst < 1e-6);
    CHECK(m.l[1000] > 0.2f);  // the long release is really there (0.25 s: e^-0.75 of the level after 1000 frames)
  }
  // A region without entries is untouched, and so is its random sequence: a bank that only reads real controllers draws nothing extra.
  {
    auto build = [](bool mod) {
      auto ins = std::make_unique<TestInstrument>();
      for (int i = 0; i < 3; ++i) {
        auto& r = ins->region(ins->dc(0.2f * (i + 1), 20000), 60, 60);
        r.lorand = i / 3.0f;
        r.hirand = (i + 1) / 3.0f;
        if (mod) add_mod(r, Cc::kVolume, 11, 0.0f);  // depth 0 dB at any value: no audible change
      }
      return ins;
    };
    auto a = build(false), b = build(true);
    SampleEngine ea(cfg()), eb(cfg());
    ea.set_instrument(*a);
    eb.set_instrument(*b);
    for (int n = 0; n < 30; ++n) {
      ea.start_voice(0, 60, v16(90));
      eb.start_voice(0, 60, v16(90));
      const Out oa = render(ea, 8), ob = render(eb, 8);
      CHECK(oa.l == ob.l);
      ea.stop_voice(0, 60);
      eb.stop_voice(0, 60);
      render(ea, 64);
      render(eb, 64);
    }
  }
  // Ill-formed entries make the region unplayable (counted), never a different sound.
  {
    TestInstrument ins;
    ins.region(ins.dc(1.0f), 60, 60).cc_gate_count = 9;
    ins.region(ins.dc(1.0f), 61, 61).cc_mod_count = 17;
    add_gate(ins.region(ins.dc(1.0f), 62, 62), 64, -1.0f, 10.0f);
    { auto& r = ins.region(ins.dc(1.0f), 63, 63); r.cc_mods[0] = {static_cast<Cc>(99), 1, 0, 1.0f}; r.cc_mod_count = 1; }
    { auto& r = ins.region(ins.dc(1.0f), 64, 64); r.cc_mods[0] = {Cc::kPan, 133, 0, 1.0f}; r.cc_mod_count = 1; }
    { auto& r = ins.region(ins.dc(1.0f), 65, 65); r.cc_mods[0] = {Cc::kPan, 1, 0, 1000.0f}; r.cc_mod_count = 1; }
    { auto& r = ins.region(ins.dc(1.0f), 66, 66); r.cc_mods[0] = {Cc::kVolume, 1, 0, std::numeric_limits<float>::infinity()}; r.cc_mod_count = 1; }
    { auto& r = ins.region(ins.dc(1.0f), 67, 67); add_gate(r, 200, 0.0f, 10.0f); }
    SampleEngine e(cfg());
    e.set_instrument(ins);
    CHECK(e.unplayable_regions() == 8);
  }
}

// Review fixes of Story 13.5.
static void test_cc_review_fixes() {
  const double G = mono_center_gain();
  // A note off with no release region consumes no random, even in a bank that draws the pseudo-CC randoms: the layer selection of a bank with a 135 entry on
  // another key follows the one of a plain bank whose note offs are padded to the same number of draws (note on: 1 + 2 draws in the 135 bank, note off: 1).
  {
    auto build = [](bool random_bank) {
      auto ins = std::make_unique<TestInstrument>();
      for (int i = 0; i < 3; ++i) {
        auto& r = ins->region(ins->dc(0.2f * (i + 1), 20000), 60, 60);
        r.lorand = i / 3.0f;
        r.hirand = (i + 1) / 3.0f;
      }
      if (random_bank) add_mod(ins->region(ins->dc(1.0f, 20000), 70, 70), Cc::kAmplitude, 135, 100.0f);
      return ins;
    };
    auto plain = build(false), rnd = build(true);
    SampleEngine ep(cfg()), er(cfg());
    ep.set_instrument(*plain);
    er.set_instrument(*rnd);
    for (int n = 0; n < 24; ++n) {
      ep.start_voice(0, 60, v16(90));  // 1 draw
      er.start_voice(0, 60, v16(90));  // 1 + 2 draws
      const Out a = render(ep, 8), b = render(er, 8);
      CHECK(a.l == b.l);
      ep.stop_voice(0, 60);            // 1 draw
      er.stop_voice(0, 60);            // 1 draw (nothing extra: no release region)
      ep.stop_voice(0, 61);            // two more, to match the two extra draws of the 135 bank's note on
      ep.stop_voice(0, 61);
      render(ep, 64);
      render(er, 64);
    }
  }
  // set_cc rebind: A (set_cc7=50) -> B (set_cc7=100) -> C (names nothing: the controller goes back to the engine's own value); a real message wins over a seed.
  {
    TestInstrument a, b, c;
    a.region(a.dc(1.0f, 100), 60, 60); a.cc_defaults = {{7, 50.0f}, {20, 9.0f}};
    b.region(b.dc(1.0f, 100), 60, 60); b.cc_defaults = {{7, 100.0f}};
    c.region(c.dc(1.0f, 100), 60, 60);
    SampleEngine e(cfg());
    e.set_instrument(c);
    auto ba = e.prepare(a), bb = e.prepare(b), bc = e.prepare(c);
    e.set_part_sample_bank(2, ba.get());
    CHECK(e.part_cc(2, 7) == 50.0f && e.part_cc(2, 20) == 9.0f);
    e.set_part_sample_bank(2, bb.get());
    CHECK(e.part_cc(2, 7) == 100.0f && e.part_cc(2, 20) == 0.0f);  // 20 was A's seed only: restored
    e.set_part_sample_bank(2, ba.get());
    e.set_part_cc(2, 7, cc32(30));                                  // a real message after the bind ...
    e.set_part_cc(2, 20, cc32(77));
    e.set_part_sample_bank(2, bc.get());                            // ... is kept when another bank is bound (A's seeds do not overwrite or reset it)
    CHECK(e.part_cc(2, 7) == 30.0f && e.part_cc(2, 20) == 77.0f);
  }
  // 131 on the envelope of a release voice is the released note's own velocity, not the part's latest note on.
  {
    TestInstrument ins;
    ins.region(ins.dc(0.0f, 20000), 60, 61);
    auto& rel = ins.region(ins.dc(1.0f, 20000), 60, 60);
    rel.trigger = ports::SampleTrigger::kRelease;
    rel.amp_veltrack = 0.0f;
    rel.ampeg_attack = 0.0f;
    add_mod(rel, Cc::kEgAttack, 131, 0.01f);
    SampleEngine e(cfg());
    e.set_instrument(ins);
    e.start_voice(0, 60, v16(127));
    e.start_voice(0, 61, v16(20));   // the part's latest velocity is now 20
    render(e, 8);
    e.stop_voice(0, 60);
    const Out o = render(e, 128);
    CHECK_NEAR(o.l[99] / G, 100.0 / 480.0, 2e-3);  // attack 0.01 s x 127/127, not x 20/127
  }
  // NaN curve points (and a NaN controller value) are never played.
  {
    TestInstrument ins;
    add_mod(ins.region(ins.dc(1.0f), 60, 60), Cc::kAmplitude, 74, 100.0f, 7);
    add_mod(ins.region(ins.dc(1.0f), 61, 61), Cc::kAmplitude, 74, 100.0f, 8);
    ins.add_curve(7, {{0, 0.0f}, {64, std::numeric_limits<float>::quiet_NaN()}});
    ins.add_curve(8, {{0, 0.0f}, {64, 2.0e6f}});
    SampleEngine e(cfg());
    e.set_instrument(ins);
    CHECK(e.unplayable_regions() == 2);
  }
  {  // an inverted gate is not a playable region
    TestInstrument ins;
    add_gate(ins.region(ins.dc(1.0f), 60, 60), 64, 80.0f, 10.0f);
    SampleEngine e(cfg());
    e.set_instrument(ins);
    CHECK(e.unplayable_regions() == 1);
  }
  // trigger=release_key with a gate and an entry: fires with the controller inside the gate at the key up, silent outside (twin of the trigger=release block).
  {
    TestInstrument ins;
    ins.region(ins.dc(0.0f, 20000), 60, 60);
    auto& rel = ins.region(ins.dc(1.0f, 20000), 60, 60);
    rel.trigger = ports::SampleTrigger::kReleaseKey;
    rel.amp_veltrack = 0.0f;
    add_gate(rel, 20, 50.0f, 127.0f);
    add_mod(rel, Cc::kAmplitude, 21, 100.0f);
    SampleEngine e(cfg());
    e.set_instrument(ins);
    e.set_part_cc(0, 21, cc32(127));
    for (const auto& [v, fires] : std::initializer_list<std::pair<double, bool>>{{0, false}, {80, true}}) {
      e.set_part_cc(0, 20, cc32(v));
      e.start_voice(0, 60, v16(127));
      render(e, 8);
      e.key_released(0, 60);
      const Out o = render(e, 64);
      CHECK(fires ? std::fabs(o.l[63] - G) < 1e-3 : o.l[63] == 0.0f);
      drain(e);
    }
  }
}

static void test_pseudo_ccs() {
  const double G = mono_center_gain();
  // The pseudo-controllers cannot be written through the controller path: set_part_cc refuses them, and a real controller number never reaches them.
  {
    SampleEngine e(cfg());
    for (const std::uint32_t cc : {128u, 129u, 130u, 131u, 135u, 136u, 137u, 140u, 141u, 511u}) CHECK(!e.set_part_cc(0, cc, 0xFFFFFFFFu));
    TestInstrument ins;
    auto& r = ins.region(ins.dc(1.0f, 20000), 60, 60);
    r.amp_veltrack = 0.0f;
    add_mod(r, Cc::kAmplitude, 131, 100.0f);
    e.set_instrument(ins);
    for (std::uint32_t cc = 0; cc < 128; ++cc) e.set_part_cc(0, cc, 0xFFFFFFFFu);  // every real controller at the top
    e.start_voice(0, 60, v16(32));
    CHECK_NEAR(render(e, 8).l[7], G * 32.0 / 127.0, 1e-5);  // still the note-on velocity
  }
  // 131: the note-on velocity, sampled through the curve.
  for (const std::uint32_t vel : {1u, 32u, 64u, 127u})
    CHECK_NEAR(cc_level([](ports::SampleRegionDesc& r, TestInstrument&) { add_mod(r, Cc::kAmplitude, 131, 100.0f); }, {}, vel), G * vel / 127.0, 1e-5);
  CHECK_NEAR(cc_level([](ports::SampleRegionDesc& r, TestInstrument&) { add_mod(r, Cc::kAmplitude, 131, 100.0f, 4); }, {}, 64), G * std::pow(64.0 / 127.0, 2.0), 1e-4);
  // 137 alternate: 0, 1, 0, 1 per note on of the part, a note without any region counts too; the second part has its own sequence.
  {
    TestInstrument ins;
    auto& r = ins.region(ins.dc(1.0f, 20000), 60, 60);
    r.amp_veltrack = 0.0f;
    add_mod(r, Cc::kAmplitude, 137, 100.0f);
    SampleEngine e(cfg());
    e.set_instrument(ins);
    auto note = [&](std::uint32_t part = 0) { e.start_voice(part, 60, v16(127)); const double l = render(e, 8).l[7]; e.stop_voice(part, 60); drain(e); return l; };
    CHECK_NEAR(note(), 0.0, 1e-7);
    CHECK_NEAR(note(), G, 1e-5);
    CHECK_NEAR(note(), 0.0, 1e-7);
    e.start_voice(0, 100, v16(100));  // no region: the alternate still flips
    drain(e);
    CHECK_NEAR(note(), 0.0, 1e-7);   // 4th flip -> value of the 5th note = 0
    CHECK_NEAR(note(1), 0.0, 1e-7);  // part 1: its own first note
    CHECK_NEAR(note(1), G, 1e-5);
  }
  // 140 / 141: raw semitone distance to the previous note on (0 for the first). Through the matrix targets the value is not curved (volume dB per semitone).
  {
    TestInstrument ins;
    auto& hi = ins.region(ins.dc(1.0f, 20000), 40, 80, 60);
    hi.amp_veltrack = 0.0f;
    add_mod(hi, Cc::kVolume, 140, 1.0f);
    auto& lo = ins.region(ins.dc(1.0f, 20000), 40, 80, 60);
    lo.amp_veltrack = 0.0f;
    add_mod(lo, Cc::kVolume, 141, 2.0f);
    lo.trigger = ports::SampleTrigger::kAttack;
    SampleEngine e(cfg());
    e.set_instrument(ins);
    auto play = [&](std::uint32_t key) {
      e.start_voice(0, key, v16(127));
      const Out o = render(e, 8);
      e.stop_voice(0, key);
      drain(e);
      return static_cast<double>(o.l[7]);
    };
    const double none = play(60);  // first note: key delta 0 on both regions -> 2 x G
    CHECK_NEAR(none, 2.0 * G, 1e-4);
    const double up = play(65);    // +5: first region +5 dB, second |5| * 2 = +10 dB
    CHECK_NEAR(up, G * (std::pow(10.0, 5.0 / 20.0) + std::pow(10.0, 10.0 / 20.0)), 1e-3);
    const double down = play(63);  // -2: first -2 dB, second +4 dB
    CHECK_NEAR(down, G * (std::pow(10.0, -2.0 / 20.0) + std::pow(10.0, 4.0 / 20.0)), 1e-3);
  }
  // 135 / 136: deterministic for a seed, in range, different for another seed; 136 is bipolar in [-1, 1) but sfizz's curve clamps its negative half to the curve's first
  // entry (0 for the default curve), so volume_oncc136 only ever adds.
  {
    auto run = [&](std::uint64_t seed, int cc) {
      TestInstrument ins;
      auto& r = ins.region(ins.dc(1.0f, 20000), 60, 60);
      r.amp_veltrack = 0.0f;
      if (cc == 135) add_mod(r, Cc::kAmplitude, 135, 100.0f);
      else add_mod(r, Cc::kVolume, 136, 6.0206f);
      SampleEngine::Config c = cfg();
      c.seed = seed;
      SampleEngine e(c);
      e.set_instrument(ins);
      std::vector<double> v;
      for (int n = 0; n < 16; ++n) {
        e.start_voice(0, 60, v16(127));
        v.push_back(render(e, 8).l[7]);
        e.stop_voice(0, 60);
        drain(e);
      }
      return v;
    };
    const auto a = run(7, 135), b = run(7, 135), c = run(8, 135);
    CHECK(a == b && a != c);
    for (double x : a) CHECK(x >= 0.0 && x <= G + 1e-6);
    double lo = 1e9, hi = -1e9;
    for (double x : run(11, 136)) { lo = std::min(lo, x); hi = std::max(hi, x); CHECK(x >= G - 1e-5 && x <= 2.0 * G + 1e-5); }
    CHECK(lo < G * 1.0001 && hi > G * 1.1);  // the negative half gives 0 dB, the positive half up to +6 dB
  }
  // A release-trigger voice reads 0 for the velocity controller on the matrix targets (sfizz: a note off has no velocity), a fresh random, the last alternate.
  {
    TestInstrument ins;
    ins.region(ins.dc(0.0f, 20000), 60, 60);
    auto& rel = ins.region(ins.dc(1.0f, 20000), 60, 60);
    rel.trigger = ports::SampleTrigger::kRelease;
    rel.amp_veltrack = 0.0f;
    add_mod(rel, Cc::kAmplitude, 131, 100.0f);
    SampleEngine e(cfg());
    e.set_instrument(ins);
    e.start_voice(0, 60, v16(127));
    render(e, 8);
    e.stop_voice(0, 60);
    const Out o = render(e, 64);
    CHECK(o.l[63] == 0.0f);
  }
}

static void test_gain_and_pan() {
  const double G = mono_center_gain();
  auto level = [&](float vol, float pan, bool stereo, std::uint32_t vel7 = 127) {
    TestInstrument ins;
    const std::uint32_t s = stereo ? ins.add_sample(std::vector<float>(5000, 1.0f), std::vector<float>(5000, 0.5f))
                                   : ins.dc(1.0f, 5000);
    auto& r = ins.region(s, 60, 60);
    r.volume_db = vol;
    r.pan = pan;
    SampleEngine e(cfg());
    e.set_instrument(ins);
    e.start_voice(0, 60, v16(vel7));
    const Out o = render(e, 4);
    return LR{static_cast<double>(o.l[3]), static_cast<double>(o.r[3])};
  };
  auto [l0, r0] = level(0, 0, false);
  CHECK_NEAR(l0, G, 1e-6);
  CHECK_NEAR(r0, G, 1e-6);
  auto [l1, r1] = level(-6.0206f, 0, false);
  CHECK_NEAR(l1, 0.5 * G, 1e-5);
  auto [l2, r2] = level(0, -100, false);
  CHECK_NEAR(l2, 1.4125375, 1e-5);
  CHECK(std::fabs(r2) < 1e-6);
  auto [l3, r3] = level(0, 100, false);
  CHECK(std::fabs(l3) < 1e-6);
  CHECK_NEAR(r3, 1.4125375, 1e-5);
  auto [l4, r4] = level(0, 50, false);  // equal power law
  CHECK_NEAR(l4 * l4 + r4 * r4, 2.0 * 0.998814 * 0.998814 * 0.999, 2e-3);
  auto [sl, sr] = level(0, 0, true);  // stereo sample: channels stay separate, +6 dB compensation
  CHECK_NEAR(sl, 0.7071068 * 0.7071068 * 1.9952623, 1e-5);
  CHECK_NEAR(sr, 0.5 * 0.7071068 * 0.7071068 * 1.9952623, 1e-5);
  auto [sl2, sr2] = level(0, 100, true);  // hard right: the left channel is silenced, not summed
  CHECK(std::fabs(sl2) < 1e-6);
  CHECK_NEAR(sr2, 0.5 * 0.7071068 * 1.9952623, 1e-5);
  auto [vl, vr] = level(0, 0, false, 64);  // velocity: gain = v^2 (SFZ amp_veltrack=100)
  CHECK_NEAR(vl, G * (64.0 / 127.0) * (64.0 / 127.0), 1e-5);
}

static void test_pool_missing_cost_and_info() {
  // Pool exhaustion: typed status, the voices that fit still play.
  {
    TestInstrument ins;
    for (int i = 0; i < 3; ++i) ins.region(ins.dc(0.1f), 60, 60);
    SampleEngine e(cfg(2));
    e.set_instrument(ins);
    CHECK(e.voice_capacity() == 2);
    CHECK(e.start_voice(0, 60, v16(100)) == EngineStatus::kPoolExhausted);
    CHECK(e.active_voices() == 2);
    CHECK(e.start_voice(0, 60, v16(100)) == EngineStatus::kPoolExhausted && e.active_voices() == 2);
  }
  // Missing sample data: silently skipped and counted, never garbage.
  {
    TestInstrument ins;
    ins.region(99, 60, 60);                                // sample index outside the instrument
    ins.region(ins.add_sample({}), 60, 60);                // present but empty
    { auto& r = ins.region(ins.dc(0.5f, 1000), 60, 60); r.offset = 1000; }  // offset beyond the sample
    ins.region(ins.dc(0.25f, 1000), 61, 61);
    SampleEngine e(cfg());
    e.set_instrument(ins);
    CHECK(e.unplayable_regions() == 3);
    CHECK(e.start_voice(0, 60, v16(127)) == EngineStatus::kOk);
    CHECK(e.active_voices() == 0);
    CHECK(e.skipped_unplayable_starts() == 3);
    for (float x : render(e, 256).l) CHECK(x == 0.0f);
    CHECK(e.start_voice(0, 61, v16(127)) == EngineStatus::kOk && e.active_voices() == 1);  // the rest still plays
  }
  // Cost units reflect the interpolation quality.
  {
    SampleEngine h(cfg(4, SampleInterpolation::kHermite3)), l(cfg(4, SampleInterpolation::kLinear));
    CHECK(h.cost_units_per_voice() == sample_voice_cost_units(SampleInterpolation::kHermite3));
    CHECK(l.cost_units_per_voice() == sample_voice_cost_units(SampleInterpolation::kLinear));
    CHECK(h.cost_units_per_voice() > l.cost_units_per_voice());
    CHECK(h.type_id() == EngineTypeId::kSample);
  }
  // Steal metadata.
  {
    TestInstrument ins;
    ins.region(ins.dc(0.5f), 60, 60);
    SampleEngine e(cfg());
    e.set_instrument(ins);
    VoiceInfo vi;
    CHECK(!e.voice_info(0, vi));
    render(e, 100);
    e.start_voice(0, 60, v16(127));
    CHECK(e.voice_info(0, vi) && vi.state == VoiceState::kHeld && vi.start_time == 100);
    render(e, 10);
    CHECK(e.voice_info(0, vi) && vi.level > 0.4f);
    e.stop_voice(0, 60);
    CHECK(e.voice_info(0, vi) && vi.state == VoiceState::kReleasing);
    CHECK(!e.voice_info(99, vi));
    CHECK(e.stop_voice(0, 61) == EngineStatus::kVoiceNotFound);
    CHECK(e.stop_voice(0, 60) == EngineStatus::kVoiceNotFound);  // already releasing: not held
  }
}

static void test_interpolation_quality() {
  // A pitched sine: Hermite is far closer to the ideal value than linear.
  std::vector<float> sine(4000);
  for (std::size_t i = 0; i < sine.size(); ++i) sine[i] = static_cast<float>(std::sin(2.0 * kPi * 0.05 * static_cast<double>(i)));
  double err[2] = {0, 0};
  const double G = mono_center_gain();
  int k = 0;
  for (const SampleInterpolation q : {SampleInterpolation::kHermite3, SampleInterpolation::kLinear}) {
    TestInstrument ins2;
    ins2.region(ins2.add_sample(sine), 60, 60).tune_cents = 33.0f;  // fractional positions
    SampleEngine e2(cfg(4, q));
    e2.set_instrument(ins2);
    e2.start_voice(0, 60, v16(127));
    const Out o = render(e2, 600);
    const double rr = static_cast<double>(std::pow(2.0f, 33.0f / 1200.0f));
    for (int f = 20; f < 600; ++f)
      err[k] = std::max(err[k], std::fabs(o.l[f] / G - std::sin(2.0 * kPi * 0.05 * rr * f)));
    ++k;
  }
  CHECK(err[0] < 2e-3);
  CHECK(err[1] > 5 * err[0]);
}

static void test_oracle_block_stall() {
  // Test-only sfizz emulation: the position holds for one frame at the first block boundary after the start block.
  TestInstrument ins;
  ins.region(ins.ramp(2000), 60, 60);
  SampleEngine::Config c = cfg();
  c.oracle_block_frames = 64;
  SampleEngine e(c);
  e.set_instrument(ins);
  render(e, 10);
  e.start_voice(0, 60, v16(127));
  const Out o = render(e, 200);  // clock 10..210: block boundaries at 64, 128, 192
  CHECK(o.l[0] < o.l[1] && o.l[52] < o.l[53]);
  CHECK(o.l[54] == o.l[53]);  // the first boundary (clock 64 = index 54) repeats the previous position
  SampleEngine plain(cfg());
  plain.set_instrument(ins);
  render(plain, 10);
  plain.start_voice(0, 60, v16(127));
  const Out p = render(plain, 200);
  CHECK(p.l[54] > p.l[53] && o.l[54] == p.l[53]);
  CHECK(o.l[55] == p.l[54] && o.l[150] == p.l[149]);  // everything after is shifted by one frame, once
}

static void test_block_renderer_integration() {
  TestInstrument ins;
  ins.region(ins.dc(0.5f, 20000), 60, 60);
  auto br = std::make_unique<BlockRenderer>();
  SampleEngine e(cfg());
  e.set_instrument(ins);
  br->set_channel_engine(2, static_cast<std::uint32_t>(EngineTypeId::kSample));
  float l[kBlockFrames], r[kBlockFrames];
  br->submit({make_note_on(0, 2, 60, 0xFFFF), 5});
  br->process_block(l, r);
  CHECK(br->status_count(EngineStatus::kEngineNotAvailable) == 1);  // Sample not compiled in yet: silence
  for (float x : l) CHECK(x == 0.0f);
  CHECK(br->bind_engine(e));
  CHECK(!br->bind_engine(e));  // already bound
  br->submit({make_note_on(0, 2, 60, 0xFFFF), 21});
  br->process_block(l, r);
  CHECK(br->status_count(EngineStatus::kOk) == 1);
  for (int i = 0; i < 21; ++i) CHECK(l[i] == 0.0f);
  CHECK(l[21] > 0.4f && r[21] > 0.4f);  // onset at the exact in-block frame
  br->set_channel_engine(3, static_cast<std::uint32_t>(EngineTypeId::kFm));
  br->submit({make_note_on(0, 3, 60, 0xFFFF), 0});
  br->process_block(l, r);
  CHECK(br->status_count(EngineStatus::kEngineNotAvailable) == 2);  // FM stays unavailable
  br->submit({make_note_off(0, 2, 60), 0});
  br->process_block(l, r);
  CHECK(e.active_voices() == 1);  // releasing, not yet gone
  e.set_param(SampleEngine::kParamGain, 0.5f);
  br->submit({make_note_on(0, 3, 60, 0xFFFF), 0});
  br->submit({make_note_on(0, 2, 60, 0xFFFF), 0});
  br->process_block(l, r);
  CHECK(l[1] > 0.0f && l[1] < 0.5f * 1.0f * 0.5f * 2.0f);  // gain 0.5 applies: 2 voices x 0.5 x 0.5 x 0.9988 ~ 0.5 (1 DC voice already held)
}

static void test_instrument_swap_and_determinism() {
  TestInstrument a, b;
  a.region(a.dc(0.1f), 60, 60);
  b.region(b.dc(0.2f), 60, 60);
  SampleEngine e(cfg());
  e.set_instrument(a);
  e.start_voice(0, 60, v16(127));
  CHECK(e.active_voices() == 1);
  e.set_instrument(b);  // stops everything, resets state
  CHECK(e.active_voices() == 0);
  e.start_voice(0, 60, v16(127));
  CHECK_NEAR(render(e, 4).l[3], 0.2 * mono_center_gain(), 1e-5);
  e.clear_instrument();
  CHECK(e.active_voices() == 0 && e.region_count() == 0);
  CHECK(e.start_voice(0, 60, v16(127)) == EngineStatus::kOk && e.active_voices() == 0);
  // Identical inputs give bit-identical output, whatever the render segmentation.
  auto run = [&](std::uint32_t seg) {
    SampleEngine x(cfg());
    x.set_instrument(a);
    x.start_voice(0, 60, v16(127));
    std::vector<float> l(1000), r(1000);
    for (std::uint32_t d = 0; d < 1000; d += seg) x.render(l.data() + d, r.data() + d, std::min<std::uint32_t>(seg, 1000 - d));
    return l;
  };
  CHECK(run(1000) == run(64) && run(64) == run(7));
}


static void test_robustness() {
  const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
  // Each bad parameter makes its region unplayable (counted), and starting it is silent and safe.
  auto bad = [&](auto&& tweak) {
    TestInstrument ins;
    auto& r = ins.region(ins.dc(0.5f, 2000), 60, 60);
    tweak(r);
    ins.region(ins.dc(0.25f, 2000), 61, 61);  // a good one next to it
    SampleEngine e(cfg());
    e.set_instrument(ins);
    CHECK(e.unplayable_regions() == 1);
    CHECK(e.start_voice(0, 60, v16(127)) == EngineStatus::kOk && e.active_voices() == 0);
    CHECK(e.skipped_unplayable_starts() <= 1);  // 0 when round robin / random never select the region
    for (float x : render(e, 64).l) CHECK(x == 0.0f);
    CHECK(e.start_voice(0, 61, v16(127)) == EngineStatus::kOk && e.active_voices() == 1);
  };
  bad([&](auto& r) { r.tune_cents = 1.0e9f; });
  bad([&](auto& r) { r.tune_cents = inf; });
  bad([&](auto& r) { r.transpose = 1000000; });
  bad([&](auto& r) { r.transpose = -1000000; });
  bad([&](auto& r) { r.key_center = 0; r.key_lo = 127; r.key_hi = 127; r.transpose = 100; });
  bad([&](auto& r) { r.ampeg_attack = inf; });
  bad([&](auto& r) { r.ampeg_decay = nan; });
  bad([&](auto& r) { r.ampeg_release = -1.0f; });
  bad([&](auto& r) { r.ampeg_delay = 1.0e30f; });
  bad([&](auto& r) { r.ampeg_hold = 1000.0f; });
  bad([&](auto& r) { r.ampeg_sustain = nan; });
  bad([&](auto& r) { r.ampeg_start = inf; });
  bad([&](auto& r) { r.seq_position = 0; });
  bad([&](auto& r) { r.seq_length = 2; r.seq_position = 3; });
  bad([&](auto& r) { r.seq_length = 0; });
  bad([&](auto& r) { r.lorand = 0.5f; r.hirand = 0.5f; });
  bad([&](auto& r) { r.lorand = 0.7f; r.hirand = 0.2f; });
  bad([&](auto& r) { r.lorand = nan; });
  bad([&](auto& r) { r.volume_db = 1000.0f; });
  bad([&](auto& r) { r.pan = nan; });

  // A tiny (but sane) ratio together with the oracle stall never reads before the sample.
  {
    TestInstrument ins;
    ins.region(ins.ramp(5000), 0, 0, 127);  // key 0 against centre 127: ratio 2^(-127/12)
    SampleEngine::Config c = cfg();
    c.oracle_block_frames = 64;
    SampleEngine e(c);
    e.set_instrument(ins);
    CHECK(e.unplayable_regions() == 0);
    e.start_voice(0, 0, v16(127));
    for (float x : render(e, 300).l) CHECK(std::isfinite(x) && x >= 0.0f);
    SampleEngine::Config lc = c;
    lc.interpolation = SampleInterpolation::kLinear;
    SampleEngine el(lc);
    el.set_instrument(ins);
    el.start_voice(0, 0, v16(127));
    for (float x : render(el, 300).l) CHECK(std::isfinite(x) && x >= 0.0f);
  }
  // A looping region fired as a release trigger plays once, it does not loop forever.
  {
    TestInstrument ins;
    ins.region(ins.dc(0.1f, 100000), 60, 60);
    auto& r = ins.region(ins.ramp(300), 60, 60);
    r.trigger = ports::SampleTrigger::kRelease;
    r.loop_mode = ports::SampleLoopMode::kLoopContinuous;
    r.has_loop_points = true;
    r.loop_start = 10;
    r.loop_end = 99;
    SampleEngine e(cfg());
    e.set_instrument(ins);
    e.start_voice(0, 60, v16(127));
    render(e, 100);
    e.stop_voice(0, 60);
    render(e, 20000);
    CHECK(e.active_voices() == 0);  // the attack voice released and the one-pass release voice ended
    VoiceInfo vi;
    int releasing = 0;
    for (std::uint32_t s = 0; s < 16; ++s) releasing += e.voice_info(s, vi) && vi.state == VoiceState::kReleasing;
    CHECK(releasing == 0);
  }
  // The same note on two channels is two independent notes.
  {
    TestInstrument ins;
    ins.region(ins.dc(0.1f, 100000), 60, 60);
    ins.region(ins.dc(0.2f, 100000), 60, 60).trigger = ports::SampleTrigger::kRelease;
    SampleEngine e(cfg());
    e.set_instrument(ins);
    e.start_voice(0, 60, v16(127));
    e.start_voice(1, 60, v16(64));
    CHECK(e.stop_voice(1, 60) == EngineStatus::kOk);
    CHECK(e.active_voices() == 3);  // ch0 attack still held, ch1 attack releasing, ch1 release voice
    CHECK(e.stop_voice(2, 60) == EngineStatus::kVoiceNotFound);  // never played on channel 2: nothing fires
    CHECK(e.active_voices() == 3);
  }
  // A release region that finds no free voice is reported, not silently dropped.
  {
    TestInstrument ins;
    ins.region(ins.dc(0.1f, 100000), 60, 60);
    ins.region(ins.dc(0.2f, 100000), 60, 60).trigger = ports::SampleTrigger::kRelease;
    SampleEngine e(cfg(1));
    e.set_instrument(ins);
    e.start_voice(0, 60, v16(127));
    CHECK(e.stop_voice(0, 60) == EngineStatus::kPoolExhausted);
  }
  // Parameter edge values.
  {
    TestInstrument ins;
    ins.region(ins.dc(1.0f, 100000), 60, 60);
    SampleEngine e(cfg());
    e.set_instrument(ins);
    e.start_voice(0, 60, v16(127));
    const double G = mono_center_gain();
    e.set_param(SampleEngine::kParamGain, 1.0e9f);
    CHECK_NEAR(render(e, 4).l[3], G * SampleEngine::kMaxGain, 1e-3);
    e.set_param(SampleEngine::kParamGain, -3.0f);
    CHECK(render(e, 4).l[3] == 0.0f);
    e.set_param(SampleEngine::kParamGain, nan);  // ignored
    CHECK(render(e, 4).l[3] == 0.0f);
    e.set_param(SampleEngine::kParamGain, 1.0f);
    for (float sv : {-1.0f, nan, inf, 1.0e30f}) e.set_param(SampleEngine::kParamSeed, sv);  // ignored, no UB
    e.set_param(SampleEngine::kParamSeed, 8.0e18f);
    CHECK_NEAR(render(e, 4).l[3], G, 1e-5);
  }
}

// Contract used by BlockRenderer::reap_generation_refs (AD-23): every kOk start is reported by voice_info with the
// channel and note it was started for (all layers of a multi-region note), and nothing else is reported.
static void test_voice_info_reports_started_notes() {
  TestInstrument ins;
  ins.region(ins.dc(0.1f), 60, 62);
  ins.region(ins.dc(0.2f), 60, 60);  // second layer on the same key
  SampleEngine e(cfg());
  CHECK(e.set_instrument(ins));
  CHECK(e.start_voice(5, 60, v16(100)) == EngineStatus::kOk);
  CHECK(e.start_voice(5, 61, v16(100)) == EngineStatus::kOk);
  std::uint32_t n60 = 0, n61 = 0, other = 0;
  VoiceInfo vi;
  for (std::uint32_t i = 0; i < e.voice_capacity(); ++i) {
    if (!e.voice_info(i, vi)) continue;
    if (vi.channel == 5 && vi.note == 60) ++n60;
    else if (vi.channel == 5 && vi.note == 61) ++n61;
    else ++other;
  }
  CHECK(n60 == 2 && n61 == 1 && other == 0);
  drain(e);
  for (std::uint32_t i = 0; i < e.voice_capacity(); ++i) CHECK(!e.voice_info(i, vi));
}

// Story 1.16: a sample with a tail has only its head resident. Inside the head the render is the whole-sample render bit for bit; at the head end the voice fades
// out with a short release and is counted as a stream underrun (once per voice). A head that is the whole sample is not a tail and never underruns.
static void test_head_with_tail_underrun() {
  std::vector<float> wave(5000);
  for (std::size_t i = 0; i < wave.size(); ++i) wave[i] = 0.5f * static_cast<float>(std::sin(0.05 * static_cast<double>(i)));
  TestInstrument whole, head;
  whole.region(whole.add_sample(wave), 60, 60);
  head.region(head.add_sample(std::vector<float>(wave.begin(), wave.begin() + 400)), 60, 60);
  head.pcm[0].total_frames = 5000;
  head.pcm[0].has_tail = true;
  SampleEngine ew(cfg()), eh(cfg());
  CHECK(ew.set_instrument(whole) && eh.set_instrument(head));
  CHECK(ew.start_voice(0, 60, 65535) == EngineStatus::kOk && eh.start_voice(0, 60, 65535) == EngineStatus::kOk);
  const Out a = render(ew, 300), b = render(eh, 300);
  CHECK(std::equal(a.l.begin(), a.l.end(), b.l.begin()) && std::equal(a.r.begin(), a.r.end(), b.r.begin()));
  CHECK(eh.stream_underrun_events() == 0 && eh.active_voices() == 1);
  render(eh, 6000);
  CHECK(eh.stream_underrun_events() == 1 && eh.active_voices() == 0);
  CHECK(ew.stream_underrun_events() == 0 && ew.active_voices() == 1);
  // frames == total_frames (a head that reaches the end of the sample): no tail, the Story 1.15 end of sample, no count.
  TestInstrument full;
  const std::uint32_t si = full.add_sample(std::vector<float>(wave.begin(), wave.begin() + 400));
  full.pcm[si].total_frames = 400;
  full.region(si, 60, 60);
  SampleEngine ef(cfg());
  CHECK(ef.set_instrument(full) && ef.start_voice(0, 60, 65535) == EngineStatus::kOk);
  render(ef, 6000);
  CHECK(ef.stream_underrun_events() == 0 && ef.active_voices() == 0);
}

// Story 1.16: a looping voice whose loop end lies just past the head ends at the head end like a non-looping one and must not wrap back into head audio mid-fade.
static void test_loop_past_head_does_not_wrap_while_fading() {
  std::vector<float> wave(5000);
  for (std::size_t i = 0; i < wave.size(); ++i) wave[i] = 0.5f * static_cast<float>(std::sin(0.05 * static_cast<double>(i)));
  TestInstrument loop, plain;
  for (TestInstrument* t : {&loop, &plain}) {
    const std::uint32_t si = t->add_sample(std::vector<float>(wave.begin(), wave.begin() + 400));
    t->pcm[si].total_frames = 5000;
    t->pcm[si].has_tail = true;
    ports::SampleRegionDesc& d = t->region(si, 60, 60);
    if (t == &loop) {
      d.loop_mode = ports::SampleLoopMode::kLoopContinuous;
      d.has_loop_points = true;
      d.loop_start = 0;
      d.loop_end = 410;
    }
  }
  SampleEngine el(cfg()), ep(cfg());
  CHECK(el.set_instrument(loop) && ep.set_instrument(plain));
  el.start_voice(0, 60, 65535);
  ep.start_voice(0, 60, 65535);
  const Out a = render(el, 6000), b = render(ep, 6000);
  CHECK(std::equal(a.l.begin(), a.l.end(), b.l.begin()));  // identical: the loop never wraps back
  CHECK(el.stream_underrun_events() == 1 && el.active_voices() == 0);
}

// --- Story 13.6: choke groups and polyphony limits ---------------------------------------------------------------------------------------

namespace {
// The state of every voice that plays `note`, by slot order: 'H' held, 'S' sustained, 'R' releasing.
std::string voice_states(const SampleEngine& e, std::uint32_t note) {
  std::string out;
  VoiceInfo vi;
  for (std::uint32_t slot = 0; slot < e.voice_capacity(); ++slot)
    if (e.voice_info(slot, vi) && vi.note == note) out += vi.state == VoiceState::kHeld ? 'H' : vi.state == VoiceState::kSustained ? 'S' : 'R';
  return out;
}
// Level of a DC voice's envelope at output frame k: (output / gain - the constant rest) / its sample value.
double dc_env(const Out& o, std::size_t k, double rest, double dc, double G) { return (o.l[k] / G - rest) / dc; }
}  // namespace

static void test_choke_groups() {
  const double G = mono_center_gain();
  const double rate_fast = std::exp(-9.0 / (0.006 * 48000.0));
  // The hi-hat: note 60 (open, group 1, off_by 2) is cut by note 62 (closed, group 2). `tweak` sets the cut's off_mode on the open region.
  auto cut_env = [&](auto&& tweak, std::size_t k) {
    TestInstrument ins;
    { auto& r = ins.region(ins.dc(0.1f, 200000), 60, 60); r.group = 1; r.has_off_by = true; r.off_by = 2; r.ampeg_release = 0.1f; tweak(r); }
    { auto& r = ins.region(ins.dc(0.2f, 200000), 62, 62); r.group = 2; }
    SampleEngine e(cfg());
    CHECK(e.set_instrument(ins));
    e.start_voice(0, 60, v16(127));
    render(e, 100);
    e.start_voice(0, 62, v16(127));
    CHECK(voice_states(e, 60) == "R" && voice_states(e, 62) == "H");
    const Out o = render(e, 6000);
    return dc_env(o, k, 0.2, 0.1, G);
  };
  // fast (the default): the 6 ms exponential release, whatever ampeg_release says
  for (std::size_t k : {0u, 70u, 143u, 287u}) CHECK_NEAR(cut_env([](ports::SampleRegionDesc&) {}, k), std::pow(rate_fast, static_cast<double>(k + 1)), 3e-3);
  // normal: the voice's own ampeg_release (0.1 s)
  {
    const double rate = std::exp(-9.0 / (0.1 * 48000.0));
    for (std::size_t k : {0u, 1199u, 2399u, 4799u}) CHECK_NEAR(cut_env([](ports::SampleRegionDesc& r) { r.off_mode = ports::SampleOffMode::kNormal; }, k), std::pow(rate, static_cast<double>(k + 1)), 3e-3);
  }
  // time: off_time seconds
  {
    const double rate = std::exp(-9.0 / (0.02 * 48000.0));
    for (std::size_t k : {0u, 479u, 959u}) CHECK_NEAR(cut_env([](ports::SampleRegionDesc& r) { r.off_mode = ports::SampleOffMode::kTime; r.off_time = 0.02f; }, k), std::pow(rate, static_cast<double>(k + 1)), 3e-3);
    // times below 6 ms behave as 6 ms
    for (std::size_t k : {0u, 143u}) CHECK_NEAR(cut_env([](ports::SampleRegionDesc& r) { r.off_mode = ports::SampleOffMode::kTime; r.off_time = 0.001f; }, k), std::pow(rate_fast, static_cast<double>(k + 1)), 3e-3);
  }
  {  // not cut by a note outside its off_by; the cut voice ends (release, then the 50 ms fade-out) and frees its slot
    TestInstrument ins;
    { auto& r = ins.region(ins.dc(0.1f, 200000), 60, 60); r.group = 1; r.has_off_by = true; r.off_by = 2; }
    { auto& r = ins.region(ins.dc(0.2f, 200000), 62, 62); r.group = 3; }
    { auto& r = ins.region(ins.dc(0.3f, 200000), 64, 64); r.group = 2; }
    SampleEngine e(cfg());
    e.set_instrument(ins);
    e.start_voice(0, 60, v16(127));
    e.start_voice(0, 62, v16(127));
    render(e, 2000);
    CHECK(voice_states(e, 60) == "H" && e.active_voices() == 2);
    e.start_voice(0, 64, v16(127));
    CHECK(voice_states(e, 60) == "R" && voice_states(e, 62) == "H");
    render(e, 20000);
    CHECK(voice_states(e, 60).empty() && e.active_voices() == 2);
  }
  {  // the cut reaches a voice that already got its note off (still ringing out), and a one-shot voice that ignores note offs
    TestInstrument ins;
    { auto& r = ins.region(ins.dc(0.1f, 200000), 60, 60); r.group = 1; r.has_off_by = true; r.off_by = 2; r.ampeg_release = 2.0f; }
    { auto& r = ins.region(ins.dc(0.1f, 200000), 61, 61); r.group = 1; r.has_off_by = true; r.off_by = 2; r.loop_mode = ports::SampleLoopMode::kOneShot; }
    { auto& r = ins.region(ins.dc(0.2f, 200000), 62, 62); r.group = 2; }
    SampleEngine e(cfg());
    e.set_instrument(ins);
    e.start_voice(0, 60, v16(127));
    e.start_voice(0, 61, v16(127));
    render(e, 100);
    e.stop_voice(0, 60);
    e.stop_voice(0, 61);
    render(e, 1000);
    CHECK(voice_states(e, 60) == "R" && voice_states(e, 61) == "S");
    const Out slow = render(e, 4800);  // release of 2 s: still near 1
    CHECK(dc_env(slow, 4799, 0.0, 0.1, G) > 0.0 && slow.l[4799] / G > 0.1 * 0.8);
    e.start_voice(0, 62, v16(127));
    const Out o = render(e, 600);
    CHECK_NEAR(dc_env(o, 287, 0.2, 0.2, G) * 0.0 + (o.l[287] / G - 0.2), 0.0, 3e-3);  // both are down to the 6 ms tail
    CHECK(voice_states(e, 60) == "R" && voice_states(e, 61) == "R");
  }
  {  // the same group and the same note: other layers and the previous instance of the note are spared; another note of the group is cut
    TestInstrument ins;
    { auto& r = ins.region(ins.dc(0.1f, 200000), 60, 62); r.group = 1; r.has_off_by = true; r.off_by = 1; }
    SampleEngine e(cfg());
    e.set_instrument(ins);
    e.start_voice(0, 60, v16(127));
    e.start_voice(0, 60, v16(127));
    CHECK(voice_states(e, 60) == "HH");
    e.start_voice(0, 61, v16(127));
    CHECK(voice_states(e, 60) == "RR" && voice_states(e, 61) == "H");
  }
  {  // a group gate that no longer holds on the victim does not spare it (sfizz: ccSwitched_.all())
    TestInstrument ins;
    { auto& r = ins.region(ins.dc(0.1f, 200000), 60, 60); r.group = 1; r.has_off_by = true; r.off_by = 1; r.cc_gate_count = 1; r.cc_gates[0] = {20, 0.0f, 50.0f}; }
    SampleEngine e(cfg());
    e.set_instrument(ins);
    e.start_voice(0, 60, v16(127));
    e.start_voice(0, 60, v16(127));
    CHECK(voice_states(e, 60) == "HH");           // gate holds: spared
    e.set_part_cc(0, 20, 0xFFFFFFFFu);            // the controller leaves the gate while the voices sound
    e.set_part_cc(0, 20, 0u);                     // and comes back for the new note
    e.start_voice(0, 60, v16(127));
    CHECK(voice_states(e, 60) == "HHH");
    // move it out of the gate, then retrigger: the gated region does not play, nothing is cut
    e.set_part_cc(0, 20, 0xFFFFFFFFu);
    e.start_voice(0, 60, v16(127));
    CHECK(voice_states(e, 60) == "HHH");
  }
  {  // voices fired by a note off are never cut, and a release-trigger voice that starts also cuts (it is in a group)
    TestInstrument ins;
    { auto& r = ins.region(ins.dc(0.1f, 200000), 60, 60); r.group = 1; }
    { auto& r = ins.region(ins.dc(0.2f, 200000), 60, 60); r.trigger = ports::SampleTrigger::kRelease; r.group = 1; r.has_off_by = true; r.off_by = 1; }
    { auto& r = ins.region(ins.dc(0.3f, 200000), 61, 61); r.group = 1; r.has_off_by = true; r.off_by = 1; }
    SampleEngine e(cfg());
    e.set_instrument(ins);
    e.start_voice(0, 60, v16(127));
    e.stop_voice(0, 60);  // starts the release voice (its group 1 cuts the attack voice of group 1 whose off_by is 1: it has none, so it only releases by the note off)
    CHECK(e.active_voices() == 2);
    e.start_voice(0, 61, v16(127));  // group 1 off_by 1: cuts voices with off_by 1 that are not release-triggered: the release voice survives
    render(e, 200);
    VoiceInfo vi;
    unsigned releasing = 0, held = 0;
    for (std::uint32_t slot = 0; slot < e.voice_capacity(); ++slot)
      if (e.voice_info(slot, vi)) (vi.state == VoiceState::kHeld ? held : releasing)++;
    CHECK(held == 1);  // note 61 only; the note-60 attack voice is releasing, the release voice reports releasing too
    (void)releasing;
  }
  {  // determinism: the same sequence gives the same bits
    auto run = [&]() {
      TestInstrument ins;
      { auto& r = ins.region(ins.ramp(30000), 60, 61); r.group = 1; r.has_off_by = true; r.off_by = 2; r.ampeg_release = 0.3f; }
      { auto& r = ins.region(ins.ramp(30000, 0.0005f), 62, 62); r.group = 2; r.polyphony = 1; }
      SampleEngine e(cfg());
      e.set_instrument(ins);
      Out all;
      for (int i = 0; i < 6; ++i) {
        e.start_voice(0, 60 + (i % 3), v16(60 + 10 * i));
        const Out o = render(e, 500);
        all.l.insert(all.l.end(), o.l.begin(), o.l.end());
        all.r.insert(all.r.end(), o.r.begin(), o.r.end());
      }
      return all;
    };
    const Out a = run(), b = run();
    CHECK(a.l.size() == b.l.size() && std::memcmp(a.l.data(), b.l.data(), a.l.size() * sizeof(float)) == 0 && std::memcmp(a.r.data(), b.r.data(), a.r.size() * sizeof(float)) == 0);
  }
}

// The sustain pedal: a cut is not a key release, so it does not wait for the pedal; the pedal's own deferred note off finds nothing left to do.
static void test_choke_with_sustain_pedal() {
  const double G = mono_center_gain();
  auto build = [](bool choke) {
    auto ins = std::make_unique<TestInstrument>();
    { auto& r = ins->region(ins->dc(0.1f, 400000), 60, 60); r.group = 1; r.ampeg_release = 5.0f; if (choke) { r.has_off_by = true; r.off_by = 2; } }
    { auto& r = ins->region(ins->dc(0.2f, 400000), 62, 62); r.group = 2; }
    return ins;
  };
  struct R { std::uint32_t before_close, after_close, after_pedal_up; double level_after_close; std::string states_60, states_after_up; };
  auto run = [&](bool choke) {
    auto ins = build(choke);
    auto br = std::make_unique<BlockRenderer>();
    SampleEngine e(cfg());
    e.set_instrument(*ins);
    br->set_channel_engine(2, static_cast<std::uint32_t>(EngineTypeId::kSample));
    CHECK(br->bind_engine(e));
    float l[kBlockFrames], r[kBlockFrames];
    auto block = [&]() { br->process_block(l, r); };
    R out{};
    br->submit({make_control_change(0, 2, Ump::kControllerSustain, 0xFFFFFFFFu), 0});
    br->submit({make_note_on(0, 2, 60, 0xFFFF), 0});
    block();
    br->submit({make_note_off(0, 2, 60), 0});  // the pedal keeps the voice: the engine sees no note off
    block();
    out.before_close = e.active_voices();
    br->submit({make_note_on(0, 2, 62, 0xFFFF), 0});
    for (int i = 0; i < 6; ++i) block();  // 6 x 64 = 384 frames: past the 6 ms (288 frames) cut
    out.states_60 = voice_states(e, 60);
    out.level_after_close = std::fabs(static_cast<double>(l[kBlockFrames - 1])) / G;
    for (int i = 0; i < 60; ++i) block();  // 80 ms: the cut voice has faded out
    out.after_close = e.active_voices();
    br->submit({make_control_change(0, 2, Ump::kControllerSustain, 0u), 0});  // the pedal up delivers the note off of 60
    block();
    block();
    out.after_pedal_up = e.active_voices();
    out.states_after_up = voice_states(e, 60);
    return out;
  };
  const R cut = run(true), keep = run(false);
  CHECK(cut.before_close == 1 && keep.before_close == 1);
  CHECK(cut.states_60 == "R" && keep.states_60 == "H");                 // cut at once although the pedal holds the key; without off_by the pedal holds it
  CHECK_NEAR(cut.level_after_close, 0.2, 2e-3);                          // only the closed hat is left in the mix
  CHECK(keep.level_after_close > 0.28);                                  // 0.1 + 0.2 with the open hat held
  CHECK(cut.after_close == 1 && keep.after_close == 2);                  // the cut voice is gone, the held one is not
  CHECK(cut.after_pedal_up == 1 && cut.states_after_up.empty());         // the pedal up does not bring it back or start anything
  CHECK(keep.after_pedal_up == 2 && keep.states_after_up == "R");        // without the cut, the pedal up starts the 5 s release
}

static void test_polyphony_limits() {
  const double G = mono_center_gain();
  (void)G;
  {  // group polyphony: the oldest voice of the group goes when the limit is reached
    TestInstrument ins;
    { auto& r = ins.region(ins.dc(0.1f, 200000), 60, 70); r.group = 4; r.polyphony = 2; r.group_polyphony = 2; r.ampeg_release = 0.5f; }
    SampleEngine e(cfg());
    e.set_instrument(ins);
    e.start_voice(0, 60, v16(100));
    render(e, 50);
    e.start_voice(0, 61, v16(100));
    CHECK(voice_states(e, 60) == "H" && voice_states(e, 61) == "H");
    e.start_voice(0, 62, v16(100));
    CHECK(voice_states(e, 60) == "R" && voice_states(e, 61) == "H" && voice_states(e, 62) == "H");
    e.start_voice(0, 63, v16(100));
    CHECK(voice_states(e, 61) == "R" && voice_states(e, 62) == "H" && voice_states(e, 63) == "H");  // 60 is already cut and is not counted again
    render(e, 20000);
    CHECK(voice_states(e, 60).empty());  // the cut one ended on its own 6 ms release (not the 0.5 s of ampeg_release)
  }
  {  // a voice that was only released by its note off (still ringing) counts and is cut fast; the limit is per part
    TestInstrument ins;
    { auto& r = ins.region(ins.dc(0.1f, 200000), 60, 70); r.group = 4; r.group_polyphony = 1; r.ampeg_release = 2.0f; }
    SampleEngine e(cfg());
    e.set_instrument(ins);
    e.start_voice(0, 60, v16(100));
    e.start_voice(1, 60, v16(100));  // another part: independent
    CHECK(voice_states(e, 60) == "HH");
    e.stop_voice(0, 60);
    render(e, 100);
    e.start_voice(0, 61, v16(100));
    const Out o = render(e, 700);
    // part 0's ringing note was cut with the 6 ms release (a 2 s release would leave it near 0.1 x 0.1)
    VoiceInfo vi;
    unsigned held = 0;
    for (std::uint32_t slot = 0; slot < e.voice_capacity(); ++slot)
      if (e.voice_info(slot, vi) && vi.state == VoiceState::kHeld) ++held;
    CHECK(held == 2);  // part 1's note 60 and part 0's note 61
    (void)o;
  }
  {  // region polyphony (a mono region); two regions of one group share the group limit
    TestInstrument ins;
    { auto& r = ins.region(ins.dc(0.1f, 200000), 60, 64); r.polyphony = 1; }
    { auto& r = ins.region(ins.dc(0.1f, 200000), 70, 70); r.group = 9; r.group_polyphony = 1; }
    { auto& r = ins.region(ins.dc(0.1f, 200000), 72, 72); r.group = 9; r.group_polyphony = 1; }
    SampleEngine e(cfg());
    e.set_instrument(ins);
    e.start_voice(0, 60, v16(100));
    e.start_voice(0, 61, v16(100));
    CHECK(voice_states(e, 60) == "R" && voice_states(e, 61) == "H");
    e.start_voice(0, 70, v16(100));
    e.start_voice(0, 72, v16(100));
    CHECK(voice_states(e, 70) == "R" && voice_states(e, 72) == "H");
  }
  {  // the sister voices of one note on go together
    TestInstrument ins;
    for (int layer = 0; layer < 2; ++layer) { auto& r = ins.region(ins.dc(0.1f, 200000), 60, 60); r.group = 7; r.group_polyphony = 2; }
    { auto& r = ins.region(ins.dc(0.1f, 200000), 61, 61); r.group = 7; r.group_polyphony = 2; }
    SampleEngine e(cfg());
    e.set_instrument(ins);
    e.start_voice(0, 60, v16(100));
    CHECK(voice_states(e, 60) == "HH");
    e.start_voice(0, 61, v16(100));
    CHECK(voice_states(e, 60) == "RR" && voice_states(e, 61) == "H");
  }
  {  // note polyphony: a retrigger of the same note releases the previous voice; another note and another group are untouched
    TestInstrument ins;
    { auto& r = ins.region(ins.dc(0.1f, 200000), 60, 62); r.note_polyphony = 1; }
    { auto& r = ins.region(ins.dc(0.1f, 200000), 70, 70); r.group = 3; r.note_polyphony = 1; }
    { auto& r = ins.region(ins.dc(0.1f, 200000), 70, 70); r.group = 5; }
    SampleEngine e(cfg());
    e.set_instrument(ins);
    e.start_voice(0, 60, v16(100));
    e.start_voice(0, 61, v16(100));
    CHECK(voice_states(e, 60) == "H" && voice_states(e, 61) == "H");
    e.start_voice(0, 60, v16(100));
    CHECK(voice_states(e, 60) == "RH" || voice_states(e, 60) == "HR");
    CHECK(voice_states(e, 61) == "H");
    e.start_voice(0, 70, v16(100));
    e.start_voice(0, 70, v16(100));
    const std::string st70 = voice_states(e, 70);
    CHECK(st70.size() == 4 && std::count(st70.begin(), st70.end(), 'H') == 2);  // the first note on goes whole (its group-5 layer is a sister voice), the second stays
  }
  {  // note_selfmask: a quieter retrigger does not cut a louder voice (default), the oldest goes with note_selfmask off; np = 2 releases the quietest first
    auto build = [](bool mask, unsigned np) {
      auto ins = std::make_unique<TestInstrument>();
      auto& r = ins->region(ins->dc(0.1f, 200000), 60, 60);
      r.note_polyphony = static_cast<std::uint16_t>(np);
      r.note_selfmask = mask;
      return ins;
    };
    {
      auto ins = build(true, 1);
      SampleEngine e(cfg());
      e.set_instrument(*ins);
      e.start_voice(0, 60, v16(100));
      e.start_voice(0, 60, v16(40));
      CHECK(voice_states(e, 60) == "HH");  // quieter: no cut
      e.start_voice(0, 60, v16(120));
      CHECK(voice_states(e, 60) == "RRH");  // louder: both go (the count is reduced for every candidate)
    }
    {
      auto ins = build(false, 1);
      SampleEngine e(cfg());
      e.set_instrument(*ins);
      e.start_voice(0, 60, v16(100));
      e.start_voice(0, 60, v16(40));
      CHECK(voice_states(e, 60) == "RH");  // off: the oldest goes whatever the velocity
    }
    {
      auto ins = build(true, 2);
      SampleEngine e(cfg());
      e.set_instrument(*ins);
      e.start_voice(0, 60, v16(100));  // slot order = start order here
      e.start_voice(0, 60, v16(30));
      CHECK(voice_states(e, 60) == "HH");
      e.start_voice(0, 60, v16(110));
      CHECK(voice_states(e, 60) == "HRH");  // the quietest of the two goes, not the oldest
    }
    {
      auto ins = build(false, 2);
      SampleEngine e(cfg());
      e.set_instrument(*ins);
      e.start_voice(0, 60, v16(100));
      e.start_voice(0, 60, v16(30));
      e.start_voice(0, 60, v16(110));
      CHECK(voice_states(e, 60) == "RHH");  // the oldest
    }
  }
  {  // no allocation, no growth: a long churn of limited and choking regions keeps the voice table size and stays deterministic
    auto run = [&]() {
      TestInstrument ins;
      { auto& r = ins.region(ins.ramp(20000), 60, 72); r.group = 1; r.group_polyphony = 3; r.note_polyphony = 2; r.has_off_by = true; r.off_by = 2; r.ampeg_release = 0.2f; }
      { auto& r = ins.region(ins.ramp(20000, 0.0003f), 73, 84); r.group = 2; r.polyphony = 2; r.off_mode = ports::SampleOffMode::kTime; r.off_time = 0.01f; }
      SampleEngine e(cfg(8));
      e.set_instrument(ins);
      Out all;
      std::uint32_t x = 7;
      for (int i = 0; i < 200; ++i) {
        x = x * 1664525u + 1013904223u;
        e.start_voice(0, 60 + (x >> 20) % 25, v16(20 + (x >> 8) % 100));
        if ((x >> 28) == 3) e.stop_voice(0, 60 + (x >> 12) % 25);
        const Out o = render(e, 200);
        all.l.insert(all.l.end(), o.l.begin(), o.l.end());
      }
      CHECK(e.voice_capacity() == 8);
      return all;
    };
    const Out a = run(), b = run();
    CHECK(a.l.size() == b.l.size() && std::memcmp(a.l.data(), b.l.data(), a.l.size() * sizeof(float)) == 0);
  }
  {  // an instrument without any of the fields: not one bit changes when other regions carry limits that never apply
    auto run = [&](bool limits) {
      TestInstrument ins;
      ins.region(ins.ramp(20000), 60, 72);
      if (limits) { auto& r = ins.region(ins.ramp(20000), 100, 110); r.group = 5; r.has_off_by = true; r.off_by = 6; r.polyphony = 2; r.group_polyphony = 3; r.note_polyphony = 1; }
      SampleEngine e(cfg());
      e.set_instrument(ins);
      Out all;
      for (int i = 0; i < 12; ++i) {
        e.start_voice(0, 60 + i, v16(30 + 7 * i));
        if (i % 3 == 2) e.stop_voice(0, 60 + i - 1);
        const Out o = render(e, 300);
        all.l.insert(all.l.end(), o.l.begin(), o.l.end());
        all.r.insert(all.r.end(), o.r.begin(), o.r.end());
      }
      return all;
    };
    const Out a = run(false), b = run(true);
    CHECK(std::memcmp(a.l.data(), b.l.data(), a.l.size() * sizeof(float)) == 0 && std::memcmp(a.r.data(), b.r.data(), a.r.size() * sizeof(float)) == 0);
  }
  {  // invalid fields make the region unplayable (counted), never undefined behaviour
    TestInstrument ins;
    { auto& r = ins.region(ins.dc(0.1f, 2000), 60, 60); r.off_time = std::nanf(""); }
    { auto& r = ins.region(ins.dc(0.1f, 2000), 61, 61); r.polyphony = 300; }
    { auto& r = ins.region(ins.dc(0.1f, 2000), 62, 62); r.off_by = 4; }  // off_by without has_off_by
    ins.region(ins.dc(0.1f, 2000), 63, 63);
    SampleEngine e(cfg());
    e.set_instrument(ins);
    CHECK(e.unplayable_regions() == 3);
  }
}

// Review fixes of Story 13.6: off_time=0, release-triggered voices, slot reuse.
static void test_choke_review_fixes() {
  const double G = mono_center_gain();
  {  // off_time=0 (off_mode=time) is a fast cut (the 6 ms floor), not an instant one
    TestInstrument ins;
    { auto& r = ins.region(ins.dc(0.1f, 200000), 60, 60); r.group = 1; r.has_off_by = true; r.off_by = 2; r.off_mode = ports::SampleOffMode::kTime; r.off_time = 0.0f; }
    { auto& r = ins.region(ins.dc(0.2f, 200000), 62, 62); r.group = 2; }
    SampleEngine e(cfg());
    e.set_instrument(ins);
    e.start_voice(0, 60, v16(127));
    render(e, 100);
    e.start_voice(0, 62, v16(127));
    const Out o = render(e, 600);
    const double rate = std::exp(-9.0 / (0.006 * 48000.0));
    CHECK_NEAR(dc_env(o, 0, 0.2, 0.1, G), rate, 3e-3);
    CHECK_NEAR(dc_env(o, 143, 0.2, 0.1, G), std::pow(rate, 144.0), 3e-3);
  }
  {  // (a) a release-triggered voice is never cut: its level stays flat after the choking note, and equals the run without the choke
    auto run = [&](bool choke) {
      TestInstrument ins;
      { auto& r = ins.region(ins.dc(0.1f, 200000), 60, 60); r.ampeg_release = 0.001f; }
      { auto& r = ins.region(ins.dc(0.3f, 200000), 60, 60); r.trigger = ports::SampleTrigger::kRelease; r.group = 2; if (choke) { r.has_off_by = true; r.off_by = 5; } }
      { auto& r = ins.region(ins.dc(0.2f, 200000), 62, 62); r.group = 5; }
      SampleEngine e(cfg());
      e.set_instrument(ins);
      e.start_voice(0, 60, v16(127));
      e.stop_voice(0, 60);
      render(e, 1000);
      e.start_voice(0, 62, v16(127));
      return render(e, 6000);
    };
    const Out a = run(true), b = run(false);
    CHECK_NEAR(a.l[5999] / G, 0.5, 2e-3);  // 0.3 release voice + 0.2 choking note
    CHECK(std::memcmp(a.l.data(), b.l.data(), a.l.size() * sizeof(float)) == 0);
  }
  {  // (b) release regions run the limit checks: polyphony=1 on a trigger=release region cuts the older release voice at the second note off
    for (int which = 0; which < 2; ++which) {
      TestInstrument ins;
      { auto& r = ins.region(ins.dc(0.1f, 200000), 60, 60); r.ampeg_release = 0.001f; }
      { auto& r = ins.region(ins.dc(0.3f, 200000), 60, 60); r.trigger = ports::SampleTrigger::kRelease; r.group = 3;
        if (which == 0) r.polyphony = 1; else r.group_polyphony = 1; }
      SampleEngine e(cfg());
      e.set_instrument(ins);
      for (int i = 0; i < 2; ++i) {
        e.start_voice(0, 60, v16(127));
        e.stop_voice(0, 60);
        render(e, 500);
      }
      render(e, 20000);
      CHECK(e.active_voices() == 1);  // the first release voice was cut and has faded out; the second sustains
    }
  }
  {  // a slot reused after a cut starts clean (offed / mark reset): the new voice is counted and cut by the next one
    TestInstrument ins;
    { auto& r = ins.region(ins.dc(0.1f, 200000), 60, 70); r.group = 4; r.group_polyphony = 1; r.note_polyphony = 1; }
    SampleEngine e(cfg(3));
    e.set_instrument(ins);
    e.start_voice(0, 60, v16(100));      // slot 0
    e.start_voice(0, 61, v16(100));      // cuts 60 (offed); slot 1
    render(e, 20000);                    // 60 has faded out: slot 0 is free again
    e.start_voice(0, 62, v16(100));      // reuses slot 0, cuts 61
    CHECK(voice_states(e, 62) == "H" && voice_states(e, 61) == "R");
    e.start_voice(0, 63, v16(100));      // the voice in the reused slot must count and be cut (offed / mark were reset)
    CHECK(voice_states(e, 62) == "R" && voice_states(e, 63) == "H");
  }
}

int main() {
  test_loop_past_head_does_not_wrap_while_fading();
  test_head_with_tail_underrun();
  test_voice_info_reports_started_notes();
  test_robustness();
  test_region_selection();
  test_round_robin();
  test_random_layers();
  test_pitch();
  test_envelope();
  test_loops_and_end();
  test_release_trigger();
  test_sample_end();
  test_first_and_legato();
  test_all_keys_released_clears_held_keys();
  test_release_key_gates();
  test_release_key_vs_release_with_pedal();
  test_gain_and_pan();
  test_velocity_and_amplitude_model();
  test_cc_gates_and_seed();
  test_cc_modulation_at_voice_start();
  test_pseudo_ccs();
  test_cc_review_fixes();
  test_choke_groups();
  test_choke_with_sustain_pedal();
  test_polyphony_limits();
  test_choke_review_fixes();
  test_pool_missing_cost_and_info();
  test_interpolation_quality();
  test_oracle_block_stall();
  test_block_renderer_integration();
  test_instrument_swap_and_determinism();
  if (g_fail) {
    std::printf("sample_engine_test: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("sample_engine_test: ok\n");
  return 0;
}
