// SampleEngine unit tests (Story 1.15). The engine is driven through the Engine interface with
// hand-built float instruments (no formatlib, no files). The sfizz parity of the same behaviours is
// checked by the null tests in tools/offline-render.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
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

  ports::InstrumentView instrument() const noexcept override { return {regions, pcm}; }

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

struct LR {
  double l, r;
};

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
  test_gain_and_pan();
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
