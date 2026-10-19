// Story 2.11: the delay send. Registry, parameter mapping, echo timing in ms mode and in tempo-sync mode (every note value at several tempos),
// the tempo change at the next block, click-free glides (with hard-switch controls), feedback limits and stability, damping, ping-pong,
// the interpolator, idle/transparency, per-part sends through the Mixer, block-size independence, determinism. The 10-minute run is in
// delay_longrun_test.cpp.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/delay.hpp"
#include "velora/core/mixer.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/params_gen.hpp"
#include "velora/core/snapshot.hpp"

using namespace velora::core;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 48000.0;
constexpr DelayNote kNotes[kDelayNoteCount] = {DelayNote::kQuarter,       DelayNote::kEighth,         DelayNote::kSixteenth,
                                               DelayNote::kDottedQuarter, DelayNote::kDottedEighth,   DelayNote::kDottedSixteenth,
                                               DelayNote::kTripletQuarter, DelayNote::kTripletEighth, DelayNote::kTripletSixteenth};

struct Stereo {
  std::vector<float> l, r;
};

// A fresh delay: feedback 0, no damping, no ping-pong, return level 1 unless told otherwise.
std::unique_ptr<DelaySend> make_delay(float time_ms = 100.0f, float fb = 0.0f, float damping = 0.0f, float pp = 0.0f, float level = 1.0f) {
  auto d = std::make_unique<DelaySend>();
  d->configure(time_ms, false, DelayNote::kEighth, fb, level, damping, pp, true);
  return d;
}

// Runs the delay over `in` (mono, fed to both channels) in blocks of `block` frames.
Stereo run_delay(DelaySend& d, const std::vector<float>& in, std::uint32_t block = 64) {
  Stereo o;
  o.l.reserve(in.size());
  o.r.reserve(in.size());
  std::vector<float> l(block), r(block);
  for (std::size_t pos = 0; pos < in.size(); pos += block) {
    const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(block, in.size() - pos));
    for (std::uint32_t i = 0; i < n; ++i) l[i] = r[i] = in[pos + i];
    StereoBlock b{l.data(), r.data(), n};
    d.process(b);
    o.l.insert(o.l.end(), l.begin(), l.begin() + n);
    o.r.insert(o.r.end(), r.begin(), r.begin() + n);
  }
  return o;
}

std::vector<float> impulse(std::size_t len, std::size_t at = 0, float amp = 1.0f) {
  std::vector<float> v(len, 0.0f);
  v[at] = amp;
  return v;
}

std::vector<float> noise(std::size_t len, std::uint32_t seed = 12345, float amp = 1.0f) {
  std::vector<float> v(len);
  std::uint32_t s = seed;
  for (float& x : v) {
    s = s * 1664525u + 1013904223u;
    x = amp * static_cast<float>(static_cast<double>(s >> 8) / 8388608.0 - 1.0);
  }
  return v;
}

std::vector<float> sine(std::size_t len, double hz, float amp) {
  std::vector<float> v(len);
  for (std::size_t i = 0; i < len; ++i) v[i] = amp * static_cast<float>(std::sin(2.0 * kPi * hz * static_cast<double>(i) / kFs));
  return v;
}

std::size_t argmax_abs(const std::vector<float>& v, std::size_t from = 0, std::size_t to = SIZE_MAX) {
  to = std::min(to, v.size());
  std::size_t best = from;
  for (std::size_t i = from; i < to; ++i)
    if (std::fabs(v[i]) > std::fabs(v[best])) best = i;
  return best;
}

// First-moment centroid of the response around its peak (+-4 samples): sub-sample arrival time (the interpolator reproduces linear functions, so a
// fractional delay of an impulse has its centroid exactly at the delay).
double centroid(const std::vector<float>& v, std::size_t centre) {
  double sw = 0, sx = 0;
  for (std::size_t i = centre >= 4 ? centre - 4 : 0; i <= centre + 4 && i < v.size(); ++i) {
    sw += v[i];
    sx += v[i] * static_cast<double>(i);
  }
  return sw != 0.0 ? sx / sw : -1.0;
}

double window_sum(const std::vector<float>& v, std::size_t centre) {
  double sw = 0;
  for (std::size_t i = centre >= 4 ? centre - 4 : 0; i <= centre + 4 && i < v.size(); ++i) sw += v[i];
  return sw;
}

double peak_of(const std::vector<float>& v) {
  double m = 0;
  for (float x : v) m = std::max(m, static_cast<double>(std::fabs(x)));
  return m;
}
bool same_bits_v(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}
double max_step(const std::vector<float>& v, std::size_t from, std::size_t to) {
  double m = 0;
  for (std::size_t i = std::max<std::size_t>(from, 1); i < std::min(to, v.size()); ++i) m = std::max(m, static_cast<double>(std::fabs(v[i] - v[i - 1])));
  return m;
}

// --- registry and mapping -------------------------------------------------------------------------------------------------------
void test_registry() {
  struct Want { std::uint16_t id; params::Type type; double min, max, def; const char* unit; };
  static const Want kWant[] = {{540, params::Type::kFloat, 1, 2000, 350, "ms"},    {541, params::Type::kBool, 0, 1, 0, "flag"},
                               {543, params::Type::kFloat, 0, 0.95, 0.35, "ratio"}, {544, params::Type::kFloat, 0, 1, 1, "ratio"},
                               {545, params::Type::kFloat, 0, 1, 0.25, "ratio"},   {546, params::Type::kFloat, 0, 1, 0, "ratio"}};
  for (const Want& w : kWant) {
    const params::ParamDef* d = params::find(w.id);
    CHECK(d != nullptr);
    if (!d) continue;
    CHECK(d->cls == params::Class::kStructural && d->scope == params::Scope::kMaster && d->type == w.type);
    CHECK(d->min == w.min && d->max == w.max && d->def == w.def && d->master_index >= 0);
  }
  CHECK(params::kDelayTime == 540 && params::kDelaySync == 541 && params::kDelayNote == 542 && params::kDelayFeedback == 543 && params::kDelayLevel == 544 &&
        params::kDelayDamping == 545 && params::kDelayPingpong == 546 && params::kTempoBpm == 802);
  const params::ParamDef* nt = params::find(params::kDelayNote);
  CHECK(nt && nt->type == params::Type::kEnum && nt->min == 0 && nt->max == 8 && static_cast<std::uint32_t>(nt->def) == params::kDelayNoteEighth && params::kDelayNoteCount == 9);
  CHECK(params::kDelayNoteQuarter == 0 && params::kDelayNoteEighth == 1 && params::kDelayNoteSixteenth == 2 && params::kDelayNoteDottedQuarter == 3 &&
        params::kDelayNoteTripletSixteenth == 8);
  // the strict feedback cap is also the registry's range: nothing at or above 1 can be set through ws.v1
  CHECK(params::find(params::kDelayFeedback)->max < 1.0 && std::fabs(static_cast<double>(delaymap::kMaxFeedback) - params::find(params::kDelayFeedback)->max) < 1e-7);
  // tempo: a performance parameter with master scope (the RT plane's single tempo value), 20..300 bpm, default 120
  const params::ParamDef* tp = params::find(params::kTempoBpm);
  CHECK(tp && tp->cls == params::Class::kPerformance && tp->scope == params::Scope::kMaster && tp->min == 20 && tp->max == 300 && tp->def == 120 && tp->perf_index >= 0);
  CHECK(tp && tp->perf_index != params::find(params::kSendReverbMaster)->perf_index && tp->perf_index != params::find(params::kLineInLevel)->perf_index);
  // the DELAY encoder binding: the per-part delay send level keeps its id (401, part scope, performance, CC 94)
  const params::ParamDef* sd = params::find(params::kSendDelayLevel);
  CHECK(sd && sd->id == 401 && sd->cls == params::Class::kPerformance && sd->scope == params::Scope::kPart && sd->cc == 94 && params::find_by_cc(94) == sd);
  // the snapshot defaults
  const Snapshot s = Snapshot::defaults();
  auto mv = [&](std::uint16_t id) { return s.master[static_cast<std::size_t>(params::find(id)->master_index)]; };
  CHECK(mv(params::kDelayTime) == 350.0f && mv(params::kDelaySync) == 0.0f && mv(params::kDelayNote) == 1.0f && mv(params::kDelayFeedback) == 0.35f &&
        mv(params::kDelayLevel) == 1.0f && mv(params::kDelayDamping) == 0.25f && mv(params::kDelayPingpong) == 0.0f);
  CHECK(s.perf_init[params::perf_slot(*tp, 0)] == 120.0f);
  CHECK(kDelaySendCost > 0 && DelaySend().cost_units() == kDelaySendCost);
}

void test_mapping() {
  const float nan = std::nanf(""), inf = INFINITY;
  // non-finite -> the registry default, finite out of range -> clamped
  CHECK(delaymap::time_ms(nan) == 350.0f && delaymap::time_ms(inf) == 350.0f && delaymap::time_ms(-inf) == 350.0f);
  CHECK(delaymap::time_ms(0.0f) == 1.0f && delaymap::time_ms(-5.0f) == 1.0f && delaymap::time_ms(1e9f) == 2000.0f && delaymap::time_ms(123.5f) == 123.5f);
  CHECK(delaymap::feedback(nan) == 0.35f && delaymap::feedback(inf) == 0.35f && delaymap::feedback(-inf) == 0.35f);
  CHECK(delaymap::feedback(1.0f) == 0.95f && delaymap::feedback(2.0f) == 0.95f && delaymap::feedback(1e30f) == 0.95f && delaymap::feedback(-1.0f) == 0.0f &&
        delaymap::feedback(0.5f) == 0.5f);
  for (float v : {0.99999f, 1.0f, 1.0000001f, 3.0f, 1e38f}) CHECK(delaymap::feedback(v) <= 0.95f && delaymap::feedback(v) < 1.0f);
  CHECK(delaymap::level(nan) == 1.0f && delaymap::level(2.0f) == 1.0f && delaymap::level(-1.0f) == 0.0f && delaymap::level(inf) == 1.0f);
  CHECK(delaymap::damping(nan) == 0.25f && delaymap::damping(7.0f) == 1.0f && delaymap::damping(-7.0f) == 0.0f);
  CHECK(delaymap::pingpong(nan) == 0.0f && delaymap::pingpong(7.0f) == 1.0f && delaymap::pingpong(-1.0f) == 0.0f);
  CHECK(delaymap::tempo_bpm(nan) == 120.0f && delaymap::tempo_bpm(inf) == 120.0f && delaymap::tempo_bpm(0.0f) == 20.0f && delaymap::tempo_bpm(1000.0f) == 300.0f &&
        delaymap::tempo_bpm(133.0f) == 133.0f);
  CHECK(!delaymap::sync(nan) && !delaymap::sync(0.0f) && !delaymap::sync(0.49f) && delaymap::sync(0.5f) && delaymap::sync(1.0f) && !delaymap::sync(inf));
  CHECK(delaymap::note(nan) == DelayNote::kEighth && delaymap::note(0.0f) == DelayNote::kQuarter && delaymap::note(8.0f) == DelayNote::kTripletSixteenth &&
        delaymap::note(99.0f) == DelayNote::kTripletSixteenth && delaymap::note(-3.0f) == DelayNote::kQuarter && delaymap::note(2.4f) == DelayNote::kSixteenth &&
        delaymap::note(2.6f) == DelayNote::kDottedQuarter);
  // note values in beats and the synced time: beats x 60 / bpm
  const double beats[kDelayNoteCount] = {1.0, 0.5, 0.25, 1.5, 0.75, 0.375, 2.0 / 3.0, 1.0 / 3.0, 1.0 / 6.0};
  for (std::uint32_t i = 0; i < kDelayNoteCount; ++i) {
    CHECK(std::fabs(delaymap::note_beats(kNotes[i]) - beats[i]) < 1e-12);
    for (float bpm : {20.0f, 60.0f, 120.0f, 300.0f}) CHECK(std::fabs(delaymap::sync_time_ms(kNotes[i], bpm) - beats[i] * 60000.0 / bpm) < 1e-9);
  }
  CHECK(delaymap::sync_time_ms(DelayNote::kQuarter, 120.0f) == 500.0);
  CHECK(delaymap::sync_time_ms(DelayNote::kDottedQuarter, 20.0f) == 4500.0);  // the longest of all: inside the line (5000 ms)
  CHECK(delaymap::sync_time_ms(DelayNote::kQuarter, 0.0f) == 3000.0);         // bpm clamped to 20
  CHECK(delaymap::damping_cutoff_hz(0.0f) == 0.0 && std::fabs(delaymap::damping_cutoff_hz(1.0f) - 1000.0) < 1e-6 && std::fabs(delaymap::damping_cutoff_hz(0.5f) - 4472.1) < 0.1);
  // a delay configured with garbage ends up with safe values
  auto d = std::make_unique<DelaySend>();
  d->configure(nan, true, static_cast<DelayNote>(77), inf, nan, nan, nan, true);
  CHECK(d->time_ms() == 350.0f && d->feedback() == 0.35f && d->level() == 1.0f && d->damping() == 0.25f && d->pingpong() == 0.0f && d->note() == DelayNote::kEighth);
  d->configure(-1e9f, false, DelayNote::kQuarter, 5.0f, 5.0f, 5.0f, 5.0f, true);
  CHECK(d->time_ms() == 1.0f && d->feedback() == 0.95f && d->level() == 1.0f && d->damping() == 1.0f && d->pingpong() == 1.0f);
  CHECK(d->target_delay_samples() == 48.0);
  d->set_tempo(nan);
  CHECK(d->tempo_bpm() == 120.0f);
}

// --- timing ---------------------------------------------------------------------------------------------------------------------
// ms mode: the first echo of an impulse arrives at the requested time within 1 sample (the level is 1, so it has unit amplitude); the second
// echo of a feedback delay arrives at twice the time with the feedback as amplitude.
void test_ms_accuracy() {
  for (float ms : {1.0f, 2.5f, 10.0f, 33.3f, 100.0f, 250.7f, 350.0f, 999.99f, 1500.0f, 1999.9f, 2000.0f}) {
    auto d = make_delay(ms);
    const std::size_t len = static_cast<std::size_t>(ms * 48.0f) + 4000;
    const Stereo o = run_delay(*d, impulse(len));
    const double want = static_cast<double>(ms) * 48.0;
    const std::size_t pk = argmax_abs(o.l);
    CHECK(std::fabs(static_cast<double>(pk) - want) <= 1.0);
    CHECK(std::fabs(centroid(o.l, pk) - want) < 1e-3);  // sub-sample: the fractional delay is accurate, not just the nearest sample
    CHECK(std::fabs(window_sum(o.l, pk) - 1.0) < 1e-4);  // unit level (a fractional position spreads it over neighbouring samples; the weights sum to 1)
    CHECK(same_bits_v(o.l, o.r));                        // no ping-pong: dual mono
    // nothing before the first echo
    double early = 0;
    for (std::size_t i = 0; i + 4 < static_cast<std::size_t>(want); ++i) early = std::max(early, static_cast<double>(std::fabs(o.l[i])));
    CHECK(early == 0.0);
  }
  // integer delays are exact: amplitude 1.0 at exactly the sample
  {
    auto d = make_delay(100.0f);  // 4800 samples
    const Stereo o = run_delay(*d, impulse(6000));
    CHECK(o.l[4800] == 1.0f && argmax_abs(o.l) == 4800);
  }
  // the second echo (feedback 0.5, no damping): amplitude 0.5 at twice the time; third 0.25 (the feedback DC blocker changes an impulse's peak by < 0.1 %)
  {
    auto d = make_delay(10.0f, 0.5f);
    const Stereo o = run_delay(*d, impulse(2000));
    CHECK(std::fabs(o.l[480] - 1.0f) < 1e-6f && std::fabs(o.l[960] - 0.5f) < 2e-3f && std::fabs(o.l[1440] - 0.25f) < 2e-3f);
  }
  // the return level scales the echo
  {
    auto d = make_delay(10.0f, 0.0f, 0.0f, 0.0f, 0.4f);
    const Stereo o = run_delay(*d, impulse(1000));
    CHECK(std::fabs(o.l[480] - 0.4f) < 1e-6f);
  }
  // a mono sum of the stereo send: an impulse in the left channel only is half level on both outputs
  {
    auto d = make_delay(10.0f);
    std::vector<float> l = impulse(1000), r(1000, 0.0f);
    StereoBlock b{l.data(), r.data(), 1000};
    d->process(b);
    CHECK(std::fabs(l[480] - 0.5f) < 1e-6f && std::fabs(r[480] - 0.5f) < 1e-6f);
  }
  // a time change settles: after the glide the echo arrives at the new time within a sample
  {
    auto d = make_delay(100.0f, 0.5f);
    run_delay(*d, noise(2000, 5, 0.5f));  // the line is running (an idle delay arrives at its target without a glide)
    d->configure(777.7f, false, DelayNote::kEighth, 0.0f, 1.0f, 0.0f, 0.0f, false);
    run_delay(*d, std::vector<float>(48000 * 3, 0.0f));  // 3 s of silence: the glide (<= 0.25 samples per sample, ~0.6 s for 30000 samples plus the tail) is over
    CHECK(d->current_delay_samples() == d->target_delay_samples() && !d->quiet());
    const Stereo o = run_delay(*d, impulse(48000));
    const std::size_t pk = argmax_abs(o.l);
    CHECK(std::fabs(static_cast<double>(pk) - 777.7 * 48.0) <= 1.0);
  }
}

// Tempo-synced: the echo arrives at note value x 60 / bpm, for every note value at several tempos (sub-sample accurate).
void test_sync_accuracy() {
  for (float bpm : {20.0f, 47.0f, 90.0f, 120.0f, 133.0f, 174.0f, 300.0f}) {
    for (DelayNote n : kNotes) {
      auto d = std::make_unique<DelaySend>();
      d->set_tempo(bpm);
      d->configure(350.0f, true, n, 0.0f, 1.0f, 0.0f, 0.0f, true);
      const double want = delaymap::note_beats(n) * 60.0 / static_cast<double>(bpm) * kFs;
      CHECK(std::fabs(d->target_delay_samples() - std::clamp(want, 48.0, 240000.0)) < 1e-6);
      const std::size_t len = static_cast<std::size_t>(want) + 2000;
      const Stereo o = run_delay(*d, impulse(len));
      const std::size_t pk = argmax_abs(o.l);
      CHECK(std::fabs(static_cast<double>(pk) - want) <= 0.5 + 1e-6);
      CHECK(std::fabs(centroid(o.l, pk) - want) < 1e-3);
    }
  }
  // the time parameter is ignored while sync is on and the note is ignored while it is off
  {
    auto a = std::make_unique<DelaySend>(), b = std::make_unique<DelaySend>();
    a->configure(100.0f, true, DelayNote::kQuarter, 0, 1, 0, 0, true);
    b->configure(900.0f, true, DelayNote::kQuarter, 0, 1, 0, 0, true);
    CHECK(a->target_delay_samples() == b->target_delay_samples() && a->target_delay_samples() == 24000.0);
    a->configure(100.0f, false, DelayNote::kQuarter, 0, 1, 0, 0, true);
    b->configure(100.0f, false, DelayNote::kTripletSixteenth, 0, 1, 0, 0, true);
    CHECK(a->target_delay_samples() == 4800.0 && b->target_delay_samples() == 4800.0);
  }
}

// A tempo change moves the target at once (set_tempo is the "next block" call of the Mixer) and the delay glides there; an unchanged tempo does nothing;
// a tempo change is ignored while sync is off.
void test_tempo_change() {
  auto d = std::make_unique<DelaySend>();
  d->set_tempo(120.0f);
  d->configure(350.0f, true, DelayNote::kQuarter, 0.0f, 1.0f, 0.0f, 0.0f, true);
  CHECK(d->target_delay_samples() == 24000.0 && d->current_delay_samples() == 24000.0);
  run_delay(*d, noise(640, 3, 0.5f));  // the line is running (an idle delay arrives at its target without a glide)
  d->set_tempo(60.0f);
  CHECK(d->target_delay_samples() == 48000.0 && d->current_delay_samples() == 24000.0);  // the target moved, the glide has not started
  run_delay(*d, std::vector<float>(64, 0.0f));
  CHECK(d->current_delay_samples() > 24000.0 && d->current_delay_samples() < 48000.0);
  run_delay(*d, std::vector<float>(48000 * 3, 0.0f));
  CHECK(d->current_delay_samples() == 48000.0);
  const Stereo o = run_delay(*d, impulse(60000));
  CHECK(std::fabs(static_cast<double>(argmax_abs(o.l)) - 48000.0) <= 0.5);
  d->set_tempo(60.0f);  // unchanged
  CHECK(d->target_delay_samples() == 48000.0);
  d->configure(350.0f, false, DelayNote::kQuarter, 0.0f, 1.0f, 0.0f, 0.0f, false);
  d->set_tempo(200.0f);
  CHECK(d->target_delay_samples() == 350.0 * 48.0);
  d->configure(350.0f, true, DelayNote::kQuarter, 0.0f, 1.0f, 0.0f, 0.0f, false);  // sync on again: follows the tempo that was set meanwhile
  CHECK(d->target_delay_samples() == 60.0 / 200.0 * kFs);
}

// Through the BlockRenderer: the tempo is the RT plane's single value (BlockRenderer::tempo_bpm()), set by an ordinary performance parameter event; the
// Mixer's delays read it at the start of a block, so an event inside block k moves the target at block k + 1.
struct Rig;
struct Cap {
  std::vector<float> l, r, al, ar;
};

struct Rig {
  std::unique_ptr<BlockRenderer> br = std::make_unique<BlockRenderer>(32);
  std::vector<std::unique_ptr<Snapshot>> snaps;
  std::uint64_t rev = 0;
  std::uint32_t ledger = 0;
  void publish(Snapshot s) {
    s.revision = ++rev;
    s.ledger_index = ledger++ % SnapshotExchange::kLedgerSlots;
    snaps.push_back(std::make_unique<Snapshot>(std::move(s)));
    br->snapshots().publish(snaps.back().get());
  }
  void note(bool on, std::uint32_t ch, std::uint32_t n, std::uint32_t frame = 0) {
    br->submit({on ? make_note_on(0, ch, n, 40000) : make_note_off(0, ch, n), frame});
  }
  void param(std::uint16_t id, std::uint32_t part, double v, std::uint32_t frame = 0) { br->submit({make_param_event(*params::find(id), part, v), frame}); }
  Cap run(int blocks) {
    Cap c;
    float l[kBlockFrames], r[kBlockFrames], al[kBlockFrames], ar[kBlockFrames];
    for (int b = 0; b < blocks; ++b) {
      OutputBuses ob{l, r, al, ar};
      br->process_block(ob);
      c.l.insert(c.l.end(), l, l + kBlockFrames);
      c.r.insert(c.r.end(), r, r + kBlockFrames);
      c.al.insert(c.al.end(), al, al + kBlockFrames);
      c.ar.insert(c.ar.end(), ar, ar + kBlockFrames);
    }
    return c;
  }
};

std::size_t perf(std::uint16_t id, std::uint32_t part) { return params::perf_slot(*params::find(id), part); }
void set_master(Snapshot& s, std::uint16_t id, float v) { s.master[static_cast<std::size_t>(params::find(id)->master_index)] = v; }
bool same_bits(const std::vector<float>& a, const std::vector<float>& b) { return same_bits_v(a, b); }
double max_diff(const std::vector<float>& a, const std::vector<float>& b) {
  double m = 0;
  for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
  return m;
}

void test_tempo_through_renderer() {
  Rig rig;
  Snapshot s = Snapshot::defaults();
  set_master(s, params::kDelaySync, 1.0f);
  set_master(s, params::kDelayNote, static_cast<float>(params::kDelayNoteQuarter));
  rig.publish(s);
  rig.run(4);
  CHECK(rig.br->tempo_bpm() == 120.0f && rig.br->mixer().tempo_bpm() == 120.0f);
  CHECK(rig.br->mixer().delay(0).target_delay_samples() == 24000.0 && rig.br->mixer().delay(1).target_delay_samples() == 24000.0);
  rig.param(params::kTempoBpm, 0, 96.0, 20);  // an event in the middle of a block
  rig.run(1);
  CHECK(rig.br->tempo_bpm() == 96.0f);                                            // the RT plane's value changed with the event ...
  CHECK(rig.br->mixer().delay(0).target_delay_samples() == 24000.0);              // ... the delay still has the old target in this block
  rig.run(1);
  CHECK(rig.br->mixer().tempo_bpm() == 96.0f && rig.br->mixer().delay(0).target_delay_samples() == 30000.0 && rig.br->mixer().delay(1).target_delay_samples() == 30000.0);
  rig.param(params::kTempoBpm, 0, 1000.0);  // out of range: clamped by the UMP normalisation to 300
  rig.run(2);
  CHECK(rig.br->tempo_bpm() == 300.0f && rig.br->mixer().delay(0).target_delay_samples() == 9600.0);
  // the snapshot's initial value applies on generation change only, like every performance parameter
  Snapshot s2 = s;
  s2.perf_init[perf(params::kTempoBpm, 0)] = 75.0f;
  rig.publish(s2);
  rig.run(2);
  CHECK(rig.br->tempo_bpm() == 75.0f);
}

// --- click-free glides -----------------------------------------------------------------------------------------------------------
// A 1 kHz tone through the delay while its time changes: the output never steps by more than the tone's own slope at the fastest read speed (the glide
// limits it to 0.75 .. 1.25 x), whereas a hard switch of the same change steps by a large fraction of the amplitude.
void test_glide_clicks() {
  constexpr float kAmp = 0.5f;
  constexpr double kHz = 1000.0;
  const std::vector<float> tone = sine(48000 * 3, kHz, kAmp);
  const double slope = kAmp * 2.0 * kPi * kHz / kFs;  // peak step of the tone itself per sample
  const double bound = slope * 1.25 * 1.02 + 1e-4;
  const std::size_t change_at = 48000;  // frames; a block boundary
  for (bool glide : {true, false}) {
    for (int kind = 0; kind < 3; ++kind) {  // 0: ms 100 -> 400.25, 1: ms 400 -> 40, 2: tempo 120 -> 90 (quarter note)
      auto d = std::make_unique<DelaySend>();
      float t0 = 100.0f, t1 = 400.25f;
      if (kind == 1) { t0 = 400.0f; t1 = 40.3f; }
      const bool sync = kind == 2;
      d->set_tempo(120.0f);
      d->configure(t0, sync, DelayNote::kQuarter, 0.0f, 1.0f, 0.0f, 0.0f, true);
      const std::vector<float> first(tone.begin(), tone.begin() + static_cast<std::ptrdiff_t>(change_at)), rest(tone.begin() + static_cast<std::ptrdiff_t>(change_at), tone.end());
      Stereo a = run_delay(*d, first);
      if (kind == 2) {
        d->set_tempo(90.0f);
        if (!glide) d->configure(t0, sync, DelayNote::kQuarter, 0.0f, 1.0f, 0.0f, 0.0f, true);  // instant: jumps to the new target
      } else {
        d->configure(t1, false, DelayNote::kQuarter, 0.0f, 1.0f, 0.0f, 0.0f, !glide);
      }
      Stereo b = run_delay(*d, rest);
      a.l.insert(a.l.end(), b.l.begin(), b.l.end());
      // measure from where the tone has filled the line (after the longest delay) to the end
      const double step = max_step(a.l, 48000 / 2, a.l.size());
      if (glide) {
        CHECK(step < bound);
      } else {
        CHECK(step > 6.0 * slope);  // the control: a hard switch clicks
      }
      if (glide) CHECK(d->current_delay_samples() == d->target_delay_samples() || std::fabs(d->current_delay_samples() - d->target_delay_samples()) < 1.0);
    }
  }
  // a feedback change under the tone does not step the output either (feedback is smoothed): the tone is commensurate with the 20 ms loop, so the
  // echoes add in phase and the amplitude goes from 0.05 / (1 - 0.2) to 0.05 / (1 - 0.9) = 0.5; the steps stay within the final tone's own slope
  {
    auto d = make_delay(20.0f, 0.2f, 0.3f);
    const std::vector<float> t = sine(48000, 1000.0, 0.05f);
    Stereo a = run_delay(*d, t);
    d->configure(20.0f, false, DelayNote::kEighth, 0.9f, 1.0f, 0.3f, 0.0f, false);
    Stereo b = run_delay(*d, t);
    a.l.insert(a.l.end(), b.l.begin(), b.l.end());
    const double final_slope = 0.5 * 2.0 * kPi * 1000.0 / kFs;
    CHECK(max_step(a.l, 4800, a.l.size()) < 1.2 * final_slope);
    CHECK(peak_of(a.l) <= 1.0);
  }
  // damping and ping-pong changes: smoothed too
  {
    auto d = make_delay(20.0f, 0.6f, 0.0f, 0.0f);
    const std::vector<float> t = sine(48000, 3000.0, 0.3f);
    Stereo a = run_delay(*d, t);
    d->configure(20.0f, false, DelayNote::kEighth, 0.6f, 1.0f, 1.0f, 1.0f, false);
    Stereo b = run_delay(*d, t);
    a.l.insert(a.l.end(), b.l.begin(), b.l.end());
    a.r.insert(a.r.end(), b.r.begin(), b.r.end());
    CHECK(max_step(a.l, 4800, a.l.size()) < 0.3 * 2.0 * kPi * 3000.0 / kFs * 4.0);
    CHECK(max_step(a.r, 4800, a.r.size()) < 0.3 * 2.0 * kPi * 3000.0 / kFs * 4.0);
  }
}

// --- feedback ---------------------------------------------------------------------------------------------------------------------
void test_feedback_stability() {
  // Out-of-range feedback values never reach unity, whatever is asked: the loop stays stable (small input, limiter inactive: linear decay).
  const float nan = std::nanf("");
  for (float fb : {0.95f, 1.0f, 1.5f, 1e30f, INFINITY, nan}) {
    auto d = make_delay(50.0f, fb, 0.0f, 0.0f);
    CHECK(d->feedback() < 1.0f && d->feedback() <= 0.95f);
    // a sparse input of small impulses: peak bounded by the geometric sum 0.01 / (1 - 0.95) = 0.2 (+ margin for interpolation / DC blocker overshoot)
    std::vector<float> in(48000 * 4, 0.0f);
    for (std::size_t i = 0; i < in.size(); i += 2400) in[i] = 0.01f;  // every 50 ms: aligned with the loop, the worst case
    const Stereo o = run_delay(*d, in);
    CHECK(peak_of(o.l) <= 0.21 && peak_of(o.l) > 0.01);
    // after the input stops the echoes die away (loop gain <= 0.95 per pass: 0.95^80 = 0.016 after 4 s)
    std::vector<float> tail_in(48000 * 8, 0.0f);
    const Stereo t = run_delay(*d, tail_in);
    const std::vector<float> last(t.l.end() - 4800, t.l.end());
    CHECK(peak_of(last) < 0.2 * 0.02);
  }
  // Full-scale adversarial input (impulses at 0.9 aligned with the loop, maximum feedback, no damping): the wet output stays below full scale.
  for (float pp : {0.0f, 0.5f, 1.0f}) {
    auto d = make_delay(50.0f, 0.95f, 0.0f, pp);
    std::vector<float> in(48000 * 20, 0.0f);
    for (std::size_t i = 0; i < in.size(); i += 2400) in[i] = 0.9f;
    const Stereo o = run_delay(*d, in);
    CHECK(peak_of(o.l) <= 1.0 && peak_of(o.r) <= 1.0 && peak_of(o.l) > 0.5);
    bool finite = true;
    for (float v : o.l) finite = finite && std::isfinite(v);
    CHECK(finite);
  }
  // Loud noise at maximum feedback: bounded, not NaN.
  {
    auto d = make_delay(30.0f, 0.95f, 0.0f, 0.3f);
    const Stereo o = run_delay(*d, noise(48000 * 10, 7, 1.0f));
    CHECK(peak_of(o.l) <= 1.0 && peak_of(o.r) <= 1.0);
  }
  // Non-finite input is silence for the delay (and does not poison the line).
  {
    auto d = make_delay(10.0f, 0.5f);
    std::vector<float> l(2000, 0.0f), r(2000, 0.0f);
    l[0] = r[0] = 1.0f;
    l[1] = NAN;
    r[2] = INFINITY;
    l[3] = -INFINITY;
    StereoBlock b{l.data(), r.data(), 2000};
    d->process(b);
    bool finite = true;
    for (int i = 0; i < 2000; ++i) finite = finite && std::isfinite(l[i]) && std::isfinite(r[i]);
    CHECK(finite);
    CHECK(l[480] == 1.0f);  // the impulse (left+right)/2 = 1 survives exactly; the others count as 0
  }
  // The soft limiter: identity up to the knee, odd, monotone, strictly below 1.
  CHECK(DelaySend::limit(0.0f, 1.0f) == 0.0f && DelaySend::limit(0.5f, 1.0f) == 0.5f && DelaySend::limit(-0.8f, 1.0f) == -0.8f && DelaySend::limit(0.8f, 1.0f) == 0.8f);
  float prev = DelaySend::limit(0.8f, 1.0f);
  bool mono = true;
  for (float x = 0.8f; x < 1e6f; x = x * 1.01f + 0.001f) {
    const float y = DelaySend::limit(x, 1.0f);
    mono = mono && y >= prev - 2e-7f && y <= 1.0f && DelaySend::limit(-x, 1.0f) == -y;
    prev = y;
  }
  CHECK(mono && DelaySend::limit(1e30f, 1.0f) <= 1.0f && DelaySend::limit(1e30f, 1.0f) > 0.999f);
  CHECK(std::fabs(DelaySend::limit(0.8001f, 1.0f) - 0.8001f) < 1e-4f);  // unit slope at the knee
  CHECK(DelaySend::limit(0.45f, 0.5f) < 0.5f && DelaySend::limit(0.45f, 0.5f) > 0.4f && DelaySend::limit(0.3f, 0.0f) == 0.0f && DelaySend::limit(0.3f, -1.0f) == 0.0f &&
        DelaySend::limit(0.3f, 1.0f) == 0.3f && DelaySend::limit(-0.45f, 0.5f) == -DelaySend::limit(0.45f, 0.5f));
  // DC does not accumulate in the loop (the feedback path blocks DC): a constant 0.5 comes back once.
  {
    auto d = make_delay(50.0f, 0.95f, 0.0f, 0.0f);
    const Stereo o = run_delay(*d, std::vector<float>(48000 * 10, 0.5f));
    CHECK(peak_of(o.l) <= 1.0);                                                    // the start transient of the loop is bounded
    CHECK(std::fabs(o.l.back() - 0.5f) < 0.01f && std::fabs(o.l[48000 * 5] - 0.5f) < 0.01f);  // settled: one echo's worth, no build-up
  }
}

// --- damping and the interpolator -----------------------------------------------------------------------------------------------
// DFT magnitude of a window of v at frequency hz.
double dft_mag(const std::vector<float>& v, std::size_t from, std::size_t to, double hz) {
  double re = 0, im = 0;
  for (std::size_t i = from; i < to && i < v.size(); ++i) {
    const double w = 2.0 * kPi * hz * static_cast<double>(i) / kFs;
    re += v[i] * std::cos(w);
    im -= v[i] * std::sin(w);
  }
  return std::sqrt(re * re + im * im);
}

void test_damping() {
  // The second echo of an impulse is the first one passed once through the feedback path: |E2(f)| / |E1(f)| = fb x |one-pole low-pass| x |DC blocker|.
  constexpr std::size_t kD = 4800;  // 100 ms
  for (float p : {0.0f, 0.3f, 0.6f, 1.0f}) {
    auto d = make_delay(100.0f, 0.6f, p, 0.0f);
    const Stereo o = run_delay(*d, impulse(kD * 3 + 100));
    const double fc = delaymap::damping_cutoff_hz(p);
    const double a = fc > 0 ? 1.0 - std::exp(-2.0 * kPi * fc / kFs) : 1.0;
    for (double f : {200.0, 1000.0, 4000.0, 10000.0, 16000.0}) {
      const double e1 = dft_mag(o.l, kD - 1500, kD + 1500, f), e2 = dft_mag(o.l, 2 * kD - 1500, 2 * kD + 1500, f);
      const double w = 2.0 * kPi * f / kFs;
      // one-pole y += a (x - y): H = a / (1 - (1 - a) z^-1); DC blocker k (1 - z^-1) / (1 - R z^-1), k = (1 + R) / 2, R = 0.9948
      const std::complex<double> z1 = std::polar(1.0, -w);
      const std::complex<double> hlp = a >= 1.0 ? std::complex<double>(1.0) : a / (1.0 - (1.0 - a) * z1);
      const double R = 0.9948, k = 0.5 * (1.0 + R);
      const std::complex<double> hdc = k * (1.0 - z1) / (1.0 - R * z1);
      const double want = 0.6 * std::abs(hlp) * std::abs(hdc);
      const double got = e2 / e1;
      CHECK(std::fabs(20.0 * std::log10(got / want)) < 0.3);
    }
    // damping only affects the feedback path: the first echo is flat
    CHECK(std::fabs(20.0 * std::log10(dft_mag(o.l, kD - 1500, kD + 1500, 16000.0) / dft_mag(o.l, kD - 1500, kD + 1500, 200.0))) < 0.05);
  }
}

// The fractional-delay interpolator never amplifies: sine sweep at a fractional delay, steady-state gain <= 1 (+ rounding).
void test_interpolator_gain() {
  for (float ms : {2.1f, 2.1104f, 2.0833f, 3.0f + 0.37f / 48.0f}) {  // different fractions
    double worst = 0;
    for (double f : {100.0, 1000.0, 5000.0, 10000.0, 15000.0, 18000.0, 21000.0, 23000.0, 23900.0}) {
      auto d = make_delay(ms);
      const std::vector<float> in = sine(9600, f, 0.5f);
      const Stereo o = run_delay(*d, in);
      double pk = 0;
      for (std::size_t i = 2400; i < o.l.size(); ++i) pk = std::max(pk, static_cast<double>(std::fabs(o.l[i])));
      worst = std::max(worst, pk / 0.5);
    }
    CHECK(worst <= 1.0005);
  }
}

// --- ping-pong --------------------------------------------------------------------------------------------------------------------
void test_pingpong() {
  constexpr std::size_t kD = 480;  // 10 ms
  // 0: both channels carry the same echoes
  {
    auto d = make_delay(10.0f, 0.5f, 0.0f, 0.0f);
    const Stereo o = run_delay(*d, impulse(4000));
    CHECK(same_bits_v(o.l, o.r));
    CHECK(o.l[kD] > 0.9f && o.l[2 * kD] > 0.4f);
  }
  // 1: the input enters the left line only; the echoes alternate left, right, left, right with the feedback as amplitude
  {
    auto d = make_delay(10.0f, 0.5f, 0.0f, 1.0f);
    const Stereo o = run_delay(*d, impulse(4000));
    CHECK(std::fabs(o.l[kD] - 1.0f) < 1e-6f && o.r[kD] == 0.0f);
    CHECK(std::fabs(o.l[2 * kD]) < 2e-3f && std::fabs(o.r[2 * kD] - 0.5f) < 2e-3f);  // (the feedback DC blocker leaves a tiny tail)
    CHECK(std::fabs(o.l[3 * kD] - 0.25f) < 2e-3f && std::fabs(o.r[3 * kD]) < 2e-3f);
    CHECK(std::fabs(o.l[4 * kD]) < 2e-3f && std::fabs(o.r[4 * kD] - 0.125f) < 2e-3f);
  }
  // 0.5: the energy is shared (left and right differ, both nonzero at the first echo)
  {
    auto d = make_delay(10.0f, 0.5f, 0.0f, 0.5f);
    const Stereo o = run_delay(*d, impulse(4000));
    CHECK(std::fabs(o.l[kD] - 1.0f) < 1e-6f && std::fabs(o.r[kD] - 0.5f) < 1e-6f);
  }
}

// --- block sizes, determinism, restart from silence -------------------------------------------------------------------------------
void test_block_sizes() {
  const std::vector<float> in = noise(48000 + 7, 99, 0.5f);
  auto make = [] {
    auto d = std::make_unique<DelaySend>();
    d->set_tempo(100.0f);
    d->configure(210.0f, true, DelayNote::kDottedEighth, 0.7f, 0.8f, 0.4f, 0.3f, true);
    return d;
  };
  auto ref_d = make();
  const Stereo ref = run_delay(*ref_d, in);
  for (std::uint32_t size : {1u, 7u, 200u, 64u * 5u, 4096u}) {
    auto d = make();
    const Stereo o = run_delay(*d, in, size);
    CHECK(same_bits_v(o.l, ref.l) && same_bits_v(o.r, ref.r));
  }
  // determinism: a second instance gives the same bits
  auto d2 = make();
  const Stereo again = run_delay(*d2, in);
  CHECK(same_bits_v(again.l, ref.l) && same_bits_v(again.r, ref.r));
  bool finite = true;
  for (float v : ref.l) finite = finite && std::isfinite(v);
  CHECK(finite);
}

void test_idle_and_restart() {
  // an idle delay does no work and outputs exact zeros
  {
    auto d = make_delay(100.0f, 0.5f);
    CHECK(d->quiet() && d->engine_blocks() == 0);
    const Stereo z = run_delay(*d, std::vector<float>(6400, 0.0f));
    CHECK(d->engine_blocks() == 0 && peak_of(z.l) == 0.0);
  }
  // a burst starts it; it rings out, flushes itself after a whole line length below -120 dBFS and does no more work
  {
    auto d = make_delay(100.0f, 0.5f, 0.3f);
    std::vector<float> burst = noise(1280, 5, 0.5f);
    burst.resize(48000 * 14, 0.0f);
    const Stereo o = run_delay(*d, burst);
    CHECK(d->quiet());
    const std::uint64_t blocks = d->engine_blocks();
    CHECK(blocks > 20 && blocks < 48000 * 14 / 64);
    const Stereo z = run_delay(*d, std::vector<float>(6400, 0.0f));
    CHECK(d->engine_blocks() == blocks && peak_of(z.l) == 0.0);
    CHECK(std::fabs(o.l.back()) == 0.0f);
    // restart: bit-identical to a fresh instance with the same settings (the buffers are clean)
    auto fresh = make_delay(100.0f, 0.5f, 0.3f);
    const std::vector<float> in2 = noise(20000, 11, 0.5f);
    const Stereo a = run_delay(*d, in2), b = run_delay(*fresh, in2);
    CHECK(same_bits_v(a.l, b.l) && same_bits_v(a.r, b.r));
  }
  // Level 0 (steady) goes idle at once and clears in slices; input right after still starts from silence (the clear is finished on wake-up).
  {
    auto d = make_delay(100.0f, 0.8f, 0.0f, 0.0f, 1.0f);
    run_delay(*d, noise(20000, 3, 0.6f));
    d->configure(100.0f, false, DelayNote::kEighth, 0.8f, 0.0f, 0.0f, 0.0f, false);
    run_delay(*d, std::vector<float>(64 * 3, 0.0f));  // the ramp finishes, then the delay goes idle and starts clearing
    CHECK(d->quiet());
    const std::uint64_t blocks = d->engine_blocks();
    CHECK(peak_of(run_delay(*d, noise(640, 4, 0.5f)).l) == 0.0);  // level 0: silent, still no work
    CHECK(d->engine_blocks() == blocks);
    d->configure(100.0f, false, DelayNote::kEighth, 0.8f, 1.0f, 0.0f, 0.0f, false);
    auto fresh = make_delay(100.0f, 0.8f, 0.0f, 0.0f, 0.0f);
    fresh->configure(100.0f, false, DelayNote::kEighth, 0.8f, 1.0f, 0.0f, 0.0f, false);
    const std::vector<float> in2 = noise(12000, 21, 0.5f);
    const Stereo a = run_delay(*d, in2), b = run_delay(*fresh, in2);
    CHECK(same_bits_v(a.l, b.l) && same_bits_v(a.r, b.r));
    CHECK(d->engine_blocks() > blocks);
  }
  // reset() equals a fresh instance
  {
    auto d = make_delay(30.0f, 0.7f);
    run_delay(*d, noise(30000, 9, 0.7f));
    d->reset();
    CHECK(d->quiet());
    auto fresh = make_delay(30.0f, 0.7f);
    const std::vector<float> in2 = noise(9000, 13, 0.5f);
    const Stereo a = run_delay(*d, in2), b = run_delay(*fresh, in2);
    CHECK(same_bits_v(a.l, b.l) && same_bits_v(a.r, b.r));
  }
}

// --- Mixer: sends, idle bit-exactness, generations -------------------------------------------------------------------------------
// The test-tone engine is mono, so the wet signal of a send is the dry signal of the part delayed: references are built from part-alone renders.
Cap part_alone(std::uint32_t ch, std::uint32_t n, int blocks) {
  Rig rig;
  rig.note(true, ch, n);
  return rig.run(blocks);
}

void short_delay(Snapshot& s, float fb = 0.0f) {
  set_master(s, params::kDelayTime, 1.0f);
  set_master(s, params::kDelaySync, 0.0f);
  set_master(s, params::kDelayFeedback, fb);
  set_master(s, params::kDelayDamping, 0.0f);
  set_master(s, params::kDelayPingpong, 0.0f);
  set_master(s, params::kDelayLevel, 1.0f);
}
float echo_at(const std::vector<float>& x, std::size_t i) { return i >= 48 ? x[i - 48] : 0.0f; }

void test_idle_bit_exact() {
  constexpr int kBlocks = 60;
  Cap plain;
  {
    Rig rig;
    rig.note(true, 0, 60);
    rig.note(true, 1, 64);
    rig.note(true, 2, 67);
    plain = rig.run(kBlocks);
  }
  // all delay sends 0, whatever the delay settings: bit for bit the pre-delay behaviour, the Mixer stays inactive, the delay does no work
  for (int variant = 0; variant < 3; ++variant) {
    Rig rig;
    Snapshot s = Snapshot::defaults();
    set_master(s, params::kDelayTime, variant == 0 ? 2000.0f : 7.0f);
    set_master(s, params::kDelaySync, variant == 2 ? 1.0f : 0.0f);
    set_master(s, params::kDelayFeedback, 0.95f);
    set_master(s, params::kDelayPingpong, 1.0f);
    rig.publish(s);
    rig.note(true, 0, 60);
    rig.note(true, 1, 64);
    rig.note(true, 2, 67);
    const Cap c = rig.run(kBlocks);
    CHECK(same_bits(c.l, plain.l) && same_bits(c.r, plain.r));
    CHECK(!rig.br->mixer().routed());
    CHECK(rig.br->mixer().delay(0).engine_blocks() == 0 && rig.br->mixer().delay(1).engine_blocks() == 0 && rig.br->mixer().delay(0).quiet());
  }
  // return level 0 with sends up: routed (a send is up), the delay does no work, the output equals the dry one exactly
  {
    Rig rig;
    Snapshot s = Snapshot::defaults();
    set_master(s, params::kDelayLevel, 0.0f);
    s.perf_init[perf(params::kSendDelayLevel, 0)] = 1.0f;
    s.perf_init[perf(params::kSendDelayLevel, 1)] = 0.5f;
    rig.publish(s);
    rig.note(true, 0, 60);
    rig.note(true, 1, 64);
    rig.note(true, 2, 67);
    const Cap c = rig.run(kBlocks);
    CHECK(rig.br->mixer().routed());
    CHECK(same_bits(c.l, plain.l) && same_bits(c.r, plain.r));
    CHECK(rig.br->mixer().delay(0).engine_blocks() == 0 && rig.br->mixer().delay(0).quiet());
  }
  // sends turned off while the tail rings: the Mixer stays routed while it rings, then goes inactive; afterwards the output is exactly silent
  {
    Rig rig;
    Snapshot s = Snapshot::defaults();
    short_delay(s, 0.35f);
    set_master(s, params::kDelayTime, 100.0f);
    s.perf_init[perf(params::kSendDelayLevel, 0)] = 1.0f;
    rig.publish(s);
    rig.note(true, 0, 60);
    rig.run(30);
    CHECK(rig.br->mixer().routed());
    rig.note(false, 0, 60);
    rig.param(params::kSendDelayLevel, 0, 0.0);
    rig.run(8);
    CHECK(rig.br->mixer().routed() && !rig.br->mixer().delay(0).quiet());  // still ringing
    rig.run(48000 * 9 / 64);
    CHECK(rig.br->mixer().delay(0).quiet() && !rig.br->mixer().routed());   // gone: transparent again
    const std::uint64_t blocks = rig.br->mixer().delay(0).engine_blocks();
    const Cap c = rig.run(100);
    CHECK(rig.br->mixer().delay(0).engine_blocks() == blocks);
    bool zero = true;
    for (float v : c.l) zero = zero && v == 0.0f;
    CHECK(zero);
  }
}

// Renders parts 0 (note 60), 1 (note 64), 2 (note 67) with the given delay send levels.
Cap render_sends(float s0, float s1, float s2, int blocks, bool part2_aux = false, float fb = 0.0f) {
  Rig rig;
  Snapshot s = Snapshot::defaults();
  s.perf_init[perf(params::kSendDelayLevel, 0)] = s0;
  s.perf_init[perf(params::kSendDelayLevel, 1)] = s1;
  s.perf_init[perf(params::kSendDelayLevel, 2)] = s2;
  if (part2_aux) s.parts[2].output = params::kPartOutputAux;
  short_delay(s, fb);
  rig.publish(s);
  rig.note(true, 0, 60);
  rig.note(true, 1, 64);
  rig.note(true, 2, 67);
  return rig.run(blocks);
}

void test_sends() {
  constexpr int kBlocks = 60;
  const Cap p0 = part_alone(0, 60, kBlocks), p1 = part_alone(1, 64, kBlocks), p2 = part_alone(2, 67, kBlocks);
  // analytic: dry mix of the three parts + the send-weighted echo of each part (1 ms, no feedback)
  {
    const Cap c = render_sends(0.5f, 0.25f, 1.0f, kBlocks);
    std::vector<float> e(c.l.size());
    for (std::size_t i = 0; i < e.size(); ++i) e[i] = p0.l[i] + p1.l[i] + p2.l[i] + 0.5f * echo_at(p0.l, i) + 0.25f * echo_at(p1.l, i) + 1.0f * echo_at(p2.l, i);
    CHECK(max_diff(c.l, e) < 3e-6 && max_diff(c.r, e) < 3e-6);
  }
  // linearity per part: the wet signals add; raising one part's send changes the output by exactly that part's contribution
  {
    const Cap a5 = render_sends(0.5f, 0, 0, kBlocks), b3 = render_sends(0, 0.3f, 0, kBlocks), ab = render_sends(0.5f, 0.3f, 0, kBlocks), none = render_sends(0, 0, 0, kBlocks);
    double md = 0;
    for (std::size_t i = 0; i < none.l.size(); ++i) md = std::max(md, static_cast<double>(std::fabs((ab.l[i] - none.l[i]) - (a5.l[i] - none.l[i]) - (b3.l[i] - none.l[i]))));
    CHECK(md < 3e-6);
    CHECK(max_diff(a5.l, none.l) > 0.01);
  }
  // Main and Aux each have their own delay instance: an Aux part's send is returned to Aux, Main stays untouched by it
  {
    const Cap c = render_sends(0, 0, 1.0f, kBlocks, /*part2_aux=*/true), c0 = render_sends(0, 0, 0.0f, kBlocks, /*part2_aux=*/true);
    CHECK(same_bits(c.l, c0.l) && same_bits(c.r, c0.r));
    CHECK(max_diff(c.al, c0.al) > 0.01);
    std::vector<float> ea(c.al.size());
    for (std::size_t i = 0; i < ea.size(); ++i) ea[i] = p2.l[i] + echo_at(p2.l, i);
    CHECK(max_diff(c.al, ea) < 3e-6 && max_diff(c.ar, ea) < 3e-6);
    Rig rig;
    Snapshot s = Snapshot::defaults();
    s.parts[2].output = params::kPartOutputAux;
    s.perf_init[perf(params::kSendDelayLevel, 2)] = 1.0f;
    rig.publish(s);
    rig.note(true, 2, 67);
    rig.run(30);
    CHECK(rig.br->mixer().delay(1).engine_blocks() > 0 && rig.br->mixer().delay(0).engine_blocks() == 0);
  }
  // a live send change is taken at the next block and ramps over a block: the first block after the event is dry
  {
    Rig rig;
    Snapshot s = Snapshot::defaults();
    short_delay(s);
    rig.publish(s);
    rig.note(true, 0, 60);
    rig.run(10);
    rig.param(params::kSendDelayLevel, 0, 1.0, 3);
    const Cap after = rig.run(20);
    bool first_dry = true;
    for (std::size_t i = 0; i < 64; ++i) first_dry = first_dry && after.l[i] == p0.l[10 * 64 + i];
    CHECK(first_dry);
    bool settled = true;
    for (std::size_t i = 4 * 64; i < 8 * 64; ++i) settled = settled && std::fabs(after.l[i] - (p0.l[10 * 64 + i] + echo_at(p0.l, 10 * 64 + i))) < 3e-6f;
    CHECK(settled);
  }
  // settings reach the instances of both buses through a new generation
  {
    Rig rig;
    Snapshot s = Snapshot::defaults();
    s.perf_init[perf(params::kSendDelayLevel, 0)] = 0.5f;
    set_master(s, params::kDelayTime, 250.0f);
    set_master(s, params::kDelaySync, 1.0f);
    set_master(s, params::kDelayNote, static_cast<float>(params::kDelayNoteDottedEighth));
    set_master(s, params::kDelayFeedback, 0.6f);
    set_master(s, params::kDelayLevel, 0.25f);
    set_master(s, params::kDelayDamping, 0.7f);
    set_master(s, params::kDelayPingpong, 0.4f);
    rig.publish(s);
    rig.note(true, 0, 60);
    rig.run(20);
    for (std::uint32_t b = 0; b < 2; ++b) {
      const DelaySend& d = rig.br->mixer().delay(b);
      CHECK(d.sync_on() && d.note() == DelayNote::kDottedEighth && d.time_ms() == 250.0f && d.feedback() == 0.6f && d.level() == 0.25f && d.damping() == 0.7f &&
            d.pingpong() == 0.4f && d.target_delay_samples() == 0.75 * 60.0 / 120.0 * kFs);
    }
    set_master(s, params::kDelaySync, 0.0f);
    set_master(s, params::kDelayTime, 700.0f);
    rig.publish(s);
    rig.run(10);
    for (std::uint32_t b = 0; b < 2; ++b) CHECK(!rig.br->mixer().delay(b).sync_on() && rig.br->mixer().delay(b).target_delay_samples() == 700.0 * 48.0);
  }
  // determinism and finiteness through the whole renderer
  {
    const Cap a = render_sends(0.7f, 0.5f, 0.3f, 100, true, 0.9f), b = render_sends(0.7f, 0.5f, 0.3f, 100, true, 0.9f);
    CHECK(same_bits(a.l, b.l) && same_bits(a.r, b.r) && same_bits(a.al, b.al) && same_bits(a.ar, b.ar));
    bool finite = true;
    for (float v : a.l) finite = finite && std::isfinite(v);
    CHECK(finite);
  }
}


// --- review follow-ups: damping state, huge input, denormals, tempo at the first generation -------------------------------------
void test_robustness() {
  // Raising the damping mid-tail starts from a settled low-pass state: no transient (a stale zero state would step the loop by the tone's amplitude).
  {
    auto d = make_delay(20.0f, 0.5f, 0.0f);
    const std::vector<float> t = sine(48000, 1000.0, 0.05f);
    Stereo a = run_delay(*d, t);
    d->configure(20.0f, false, DelayNote::kEighth, 0.5f, 1.0f, 0.8f, 0.0f, false);
    Stereo b = run_delay(*d, t);
    a.l.insert(a.l.end(), b.l.begin(), b.l.end());
    CHECK(max_step(a.l, 4800, a.l.size()) < 1.2 * 0.1 * 2.0 * kPi * 1000.0 / kFs);  // the tone's own slope at the steady amplitude (0.05 / (1 - 0.5) = 0.1)
  }
  // Inputs near FLT_MAX: nothing non-finite enters the filters or the lines, and normal input afterwards behaves normally.
  {
    auto d = make_delay(10.0f, 0.5f, 0.3f);
    std::vector<float> huge(3000, 2e38f), l(3000, 2e38f), r(3000, -3.4e38f);
    StereoBlock b{l.data(), r.data(), 3000};
    d->process(b);
    bool finite = true;
    for (int i = 0; i < 3000; ++i) finite = finite && std::isfinite(l[i]) && std::isfinite(r[i]) && std::fabs(l[i]) <= 1.0f && std::fabs(r[i]) <= 1.0f;
    CHECK(finite);
    run_delay(*d, std::vector<float>(48000 * 2, 0.0f));
    const Stereo o = run_delay(*d, impulse(2000));
    bool fin2 = true;
    for (float v : o.l) fin2 = fin2 && std::isfinite(v);
    CHECK(fin2 && std::fabs(o.l[480] - 1.0f) < 1e-5f && o.l[960] > 0.2f);
  }
  // A decaying tail never produces denormal samples.
  {
    auto d = make_delay(5.0f, 0.5f, 0.3f, 0.5f);
    std::vector<float> in = noise(2400, 3, 0.5f);
    in.resize(48000 * 3, 0.0f);
    const Stereo o = run_delay(*d, in);
    bool sub = false;
    for (std::size_t i = 0; i < o.l.size(); ++i) sub = sub || std::fpclassify(o.l[i]) == FP_SUBNORMAL || std::fpclassify(o.r[i]) == FP_SUBNORMAL;
    CHECK(!sub);
  }
  // The first generation uses the live tempo: sync target right at once (no glide) when the tempo is not 120.
  {
    Rig rig;
    Snapshot s = Snapshot::defaults();
    set_master(s, params::kDelaySync, 1.0f);
    set_master(s, params::kDelayNote, static_cast<float>(params::kDelayNoteQuarter));
    s.perf_init[perf(params::kTempoBpm, 0)] = 60.0f;
    rig.publish(s);
    rig.run(1);
    CHECK(rig.br->mixer().delay(0).target_delay_samples() == 48000.0 && rig.br->mixer().delay(0).current_delay_samples() == 48000.0);
    CHECK(rig.br->mixer().delay(1).current_delay_samples() == 48000.0);
  }
  // After a long idle spell a restart is exactly a fresh start (no bulk clear exists any more; stale data is never read).
  {
    auto d = make_delay(2000.0f, 0.9f);
    run_delay(*d, noise(48000, 8, 0.7f));
    auto fresh = make_delay(2000.0f, 0.9f);
    d->reset();
    const std::vector<float> in2 = noise(48000, 17, 0.5f);
    const Stereo a = run_delay(*d, in2), b = run_delay(*fresh, in2);
    CHECK(same_bits_v(a.l, b.l) && same_bits_v(a.r, b.r));
  }
}

}  // namespace

int main() {
  test_registry();
  test_mapping();
  test_ms_accuracy();
  test_sync_accuracy();
  test_tempo_change();
  test_tempo_through_renderer();
  test_glide_clicks();
  test_feedback_stability();
  test_damping();
  test_interpolator_gain();
  test_pingpong();
  test_block_sizes();
  test_idle_and_restart();
  test_idle_bit_exact();
  test_sends();
  test_robustness();
  if (g_fail == 0) std::printf("delay_tests: ok\n");
  else std::printf("delay_tests: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
