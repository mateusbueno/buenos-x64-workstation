// Story 13.10: the sfizz random-number replay (src/sfizz_random_replay.*): the LCG and its float mapping, the seed inverse, the draw sequence per event,
// and the property the null test needs: an engine re-seeded with the replay seed picks the layer the oracle's number picks.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <vector>

#include "render.hpp"
#include "sfizz_random_replay.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/ports/sample_source.hpp"

using namespace velora::offline;
using namespace velora::core;
namespace ports = velora::ports;

static int g_fail = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    if (!(c)) {                                                   \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);    \
      ++g_fail;                                                   \
    }                                                             \
  } while (0)

namespace {
// sfizz's fast_rand as a URBG, to compare the float mapping with the platform's std::uniform_real_distribution<float> (what Synth.cpp uses).
struct Urbg {
  using result_type = std::uint32_t;
  static constexpr std::uint32_t min() { return 0; }
  static constexpr std::uint32_t max() { return 0xFFFFFFFFu; }
  SfizzLcg g;
  std::uint32_t operator()() { return g.next(); }
};

struct TwoLayers final : ports::SampleSource {
  std::vector<ports::SampleRegionDesc> regions;
  std::vector<ports::SamplePcm> pcm;
  std::vector<float> a, b;
  std::vector<ports::SampleCcDefault> ccd;
  std::vector<ports::SampleCurve> curves;
  // only >= 0: just that layer (region 0 or 1), full random range; used to measure a layer's own output level
  explicit TwoLayers(float split, int only = -1) : a(4000, 0.1f), b(4000, 0.2f) {
    for (int i = 0; i < 2; ++i) {
      if (only >= 0 && i != only) continue;
      ports::SamplePcm p;
      p.sample_rate = 48000;
      p.channels = 1;
      p.frames = 4000;
      p.left = i == 0 ? a.data() : b.data();
      pcm.push_back(p);
      ports::SampleRegionDesc d;
      d.sample_index = static_cast<std::uint32_t>(pcm.size() - 1);
      d.key_lo = d.key_hi = d.key_center = 60;
      d.lorand = only >= 0 ? 0.0f : (i == 0 ? 0.0f : split);
      d.hirand = only >= 0 ? 1.0f : (i == 0 ? split : 1.0f);
      regions.push_back(d);
    }
  }
  ports::InstrumentView instrument() const noexcept override { return {regions, pcm, ccd, curves}; }
};

NoteEvent note(bool on, std::uint8_t n) {
  NoteEvent e;
  e.on = on;
  e.note = n;
  e.velocity16 = on ? 40000 : 0;
  return e;
}
NoteEvent cc(std::uint8_t n) {
  NoteEvent e;
  e.cc = true;
  e.note = n;
  e.value = 100;
  return e;
}
}  // namespace

static void test_lcg_and_float_mapping() {
  SfizzLcg g;
  CHECK(g.next() == 1013904223u);  // 0 * 1664525 + 1013904223
  CHECK(g.next() == static_cast<std::uint32_t>(1013904223ull * 1664525ull + 1013904223ull));
  Urbg u;
  std::uniform_real_distribution<float> dist(0.0f, 1.0f);
  SfizzLcg mine;
  for (int i = 0; i < 100000; ++i)
    if (dist(u) != sfizz_uniform_float(mine.next())) {
      CHECK(!"float mapping differs from std::uniform_real_distribution");
      break;
    }
  CHECK(sfizz_uniform_float(0xFFFFFFFFu) < 1.0f);
  CHECK(sfizz_uniform_float(0) == 0.0f);
}

static void test_seed_inverse() {
  std::mt19937 rng(12345);
  std::uniform_real_distribution<float> d(0.0f, 1.0f);
  for (int i = 0; i < 20000; ++i) {
    const float u = d(rng);
    const float got = engine_first_draw(engine_seed_for_first_draw(u));
    const float want = std::floor(u * 16777216.0f) / 16777216.0f;
    if (got != (want == 0.0f ? 1.0f / 16777216.0f : want) || !(got <= u || want == 0.0f) || !(u - got < 1.0f / 8388608.0f)) {
      CHECK(!"seed inverse");
      break;
    }
  }
  const float edges[] = {0.0f, 1e-9f, 0.25f, 0.5f, 0.999999f, std::nextafter(1.0f, 0.0f), -1.0f, 2.0f, std::numeric_limits<float>::quiet_NaN(),
                         std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
  for (float u : edges) {
    const float got = engine_first_draw(engine_seed_for_first_draw(u));
    CHECK(got >= 0.0f && got < 1.0f);  // clamped input never gives an out-of-range draw
    if (u >= 0.0f && u < 1.0f) CHECK(std::fabs(got - u) < 1.0f / 8388608.0f);
  }
}

static void test_draw_sequence() {
  // note on, CC7, bend, CC123 (no draw), contact (no draw), CC64, note off, note on.
  NoteEvent bend;
  bend.cc = true;
  bend.bend = true;
  NoteEvent contact;
  contact.contact = true;
  const std::vector<NoteEvent> ev = {note(true, 60), cc(7), bend, cc(123), contact, cc(64), note(false, 60), note(true, 62)};
  const std::vector<float> d = sfizz_note_draws(ev);
  CHECK(d.size() == 3);
  SfizzLcg g;
  const float n1 = sfizz_uniform_float(g.next());  // note on
  g.next();                                         // CC7
  g.next();                                         // CC64
  const float n2 = sfizz_uniform_float(g.next());   // note off
  const float n3 = sfizz_uniform_float(g.next());   // note on
  CHECK(d.size() == 3 && d[0] == n1 && d[1] == n2 && d[2] == n3);
  const std::vector<float> skipped = sfizz_note_draws({note(true, 60)}, 2);
  SfizzLcg h;
  h.next();
  h.next();
  CHECK(skipped.size() == 1 && skipped[0] == sfizz_uniform_float(h.next()));
}

// Which layer of a TwoLayers instrument an engine plays for its next note: the output level 2000 frames in (past the ramp-in) is compared with the DC level of
// each layer measured on its own; anything that matches neither is reported as -1.
struct LayerProbe {
  float level[2] = {0.0f, 0.0f};
  static float play(SampleEngine& e) {
    e.start_voice(0, 60, 65535);
    std::vector<float> l(4096, 0.0f), r(4096, 0.0f);
    e.render(l.data(), r.data(), 4096);  // the 4000-frame sample ends on its own
    return l[2000];
  }
  LayerProbe() {
    for (int k = 0; k < 2; ++k) {
      TwoLayers one(0.5f, k);
      SampleEngine::Config c;
      c.max_voices = 4;
      SampleEngine e(c);
      e.set_instrument(one);
      level[k] = play(e);
    }
  }
  int classify(float v) const {
    for (int k = 0; k < 2; ++k)
      if (std::fabs(v - level[k]) < 1e-4f * std::fabs(level[k])) return k;
    return -1;
  }
};

// Drift guard: engine_first_draw (the replay's model of set_seed + next_random) must agree with the REAL engine. A two-layer instrument split at 0.37:
// for 3000 arbitrary seeds the layer the engine plays is the layer the model's first draw selects. A change of the production generator (the xorshift, its
// scramble, the float mapping) makes this fail loudly.
static void test_model_matches_real_engine() {
  const float split = 0.37f;
  TwoLayers ins(split);
  SampleEngine::Config c;
  c.max_voices = 4;
  SampleEngine e(c);
  CHECK(e.set_instrument(ins));
  LayerProbe probe;
  CHECK(probe.level[0] > 0.0f && probe.level[1] > 1.5f * probe.level[0]);
  int seen[2] = {0, 0};
  for (std::uint64_t i = 0; i < 3000; ++i) {
    const std::uint64_t seed = i * 7919u + 13u;
    e.set_seed(seed);
    const int want = engine_first_draw(seed) < split ? 0 : 1;
    const int got = probe.classify(LayerProbe::play(e));
    if (got != want) {
      CHECK(!"the engine's first draw differs from engine_first_draw");
      break;
    }
    ++seen[want];
  }
  CHECK(seen[0] > 800 && seen[1] > 1400);
}

// The property the null test relies on: the engine, re-seeded for the oracle's number, plays the layer that number selects.
static void test_engine_layer_follows_oracle_number() {
  const float split = 0.3f;
  TwoLayers ins(split);
  SampleEngine::Config c;
  c.max_voices = 4;
  SampleEngine e(c);
  CHECK(e.set_instrument(ins));
  LayerProbe probe;
  std::vector<NoteEvent> ev;
  for (int i = 0; i < 600; ++i) {
    ev.push_back(note(true, 60));
    ev.push_back(cc(7));
    ev.push_back(note(false, 60));
  }
  const std::vector<float> draws = sfizz_note_draws(ev);
  int count[2] = {0, 0};
  std::size_t di = 0;
  for (const NoteEvent& x : ev) {
    if (x.cc) continue;
    const float u = draws[di++];
    if (!x.on) continue;  // the note-off number only matters for release-trigger regions (none here)
    e.set_seed(engine_seed_for_first_draw(u));
    const int want = u < split ? 0 : 1;
    if (probe.classify(LayerProbe::play(e)) != want) {
      CHECK(!"layer differs from the oracle number's layer");
      break;
    }
    ++count[want];
  }
  CHECK(count[0] > 100 && count[1] > 100);
  std::printf("layer parity: %d first-layer, %d second-layer notes\n", count[0], count[1]);
}

// End to end through render_events: note on/off, CCs and CC123 (no draw), notes on several channels. The layer of every note equals the layer
// sfizz_note_draws selects; without the flag the render is the default-seed render (bit-identical to itself and to an explicit seed 1) and differs from the replay.
static void test_render_events_end_to_end() {
  const float split = 0.5f;
  TwoLayers ins(split);
  LayerProbe probe;  // levels as the bare engine plays them; the block loop adds its own gain stage, so measure the layers through render_events too
  for (int k = 0; k < 2; ++k) {
    TwoLayers one(0.5f, k);
    RenderOptions o;
    o.engine = EngineKind::kSample;
    o.instrument = &one;
    o.voices = 8;
    o.tail_seconds = 0.2;
    NoteEvent n = note(true, 60);
    n.frame = 100;
    NoteEvent nf = note(false, 60);
    nf.frame = 3100;
    WavData w;
    std::string e;
    CHECK(render_events({n, nf}, o, w, e));
    probe.level[k] = w.samples[(100 + 2000) * 2];
  }
  CHECK(probe.level[0] > 0.0f && probe.level[1] > 1.5f * probe.level[0]);
  std::vector<NoteEvent> ev;
  std::uint64_t f = 100;
  for (int i = 0; i < 40; ++i) {
    NoteEvent on = note(true, 60);
    on.channel = static_cast<std::uint8_t>(i % 3 == 0 ? 9 : 0);  // a channel the part table does not cover must stay in step too
    on.frame = f;
    ev.push_back(on);
    NoteEvent c7 = cc(7);
    c7.frame = f + 50;
    ev.push_back(c7);
    NoteEvent all = cc(123);
    all.frame = f + 60;
    if (i % 5 == 0) ev.push_back(all);
    NoteEvent off = note(false, 60);
    off.channel = on.channel;
    off.frame = f + 3000;
    ev.push_back(off);
    f += 6000;
  }
  const std::vector<float> draws = sfizz_note_draws(ev);
  RenderOptions opt;
  opt.engine = EngineKind::kSample;
  opt.instrument = &ins;
  opt.voices = 8;
  opt.tail_seconds = 0.2;
  opt.sfizz_random_replay = true;
  WavData replay, off1, off2, seeded;
  std::string err;
  CHECK(render_events(ev, opt, replay, err) && err.empty());
  std::size_t di = 0;
  int count[2] = {0, 0};
  for (std::size_t i = 0; i < ev.size(); ++i) {
    if (ev[i].cc) continue;
    const float u = draws[di++];
    if (!ev[i].on) continue;
    const int want = u < split ? 0 : 1;
    const float v = replay.samples[(ev[i].frame + 2000) * 2];
    if (probe.classify(v) != want) {
      CHECK(!"rendered layer differs from the oracle number's layer");
      break;
    }
    ++count[want];
  }
  CHECK(di == draws.size() && count[0] > 5 && count[1] > 5);
  opt.sfizz_random_replay = false;
  CHECK(render_events(ev, opt, off1, err) && render_events(ev, opt, off2, err));
  opt.seed = 1;  // the documented default
  CHECK(render_events(ev, opt, seeded, err));
  CHECK(off1.samples == off2.samples && off1.samples == seeded.samples);
  CHECK(off1.samples != replay.samples);  // the default seed plays other layers somewhere

  // refused combinations
  opt.sfizz_random_replay = true;
  WavData junk;
  RenderOptions bad = opt;
  bad.engine = EngineKind::kTestTone;
  err.clear();
  { const bool ok = render_events(ev, bad, junk, err); if (ok || err.find("sfizz random replay") == std::string::npos) { std::printf("tt: ok=%d err=%s\n", ok, err.c_str()); CHECK(false);} }
  bad = opt;
  bad.routing.enabled = true;
  for (NoteEvent& x : ev) x.channel = 0;  // routing refuses channels above 7 first
  err.clear();
  CHECK(!render_events(ev, bad, junk, err) && err.find("sfizz random replay") != std::string::npos);
  bad = opt;
  std::vector<velora::core::EmittedNoteEvent> trace;
  bad.emitted_notes = &trace;
  err.clear();
  CHECK(!render_events(ev, bad, junk, err) && err.find("sfizz random replay") != std::string::npos);
}

int main() {
  test_lcg_and_float_mapping();
  test_seed_inverse();
  test_draw_sequence();
  test_model_matches_real_engine();
  test_engine_layer_follows_oracle_number();
  test_render_events_end_to_end();
  if (g_fail) {
    std::printf("%d FAILED\n", g_fail);
    return 1;
  }
  std::printf("sfizz_random_replay_test: ok\n");
  return 0;
}
