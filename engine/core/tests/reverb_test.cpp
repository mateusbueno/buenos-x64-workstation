// Story 2.10: the reverb send. Registry, RT60 by impulse response (Schroeder backward integration) per type and time at 1 kHz,
// level normalisation, cross-fade on a type change, idle/transparency, per-part sends and the REV master send level through the Mixer,
// determinism.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/mixer.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/params_gen.hpp"
#include "velora/core/reverb.hpp"
#include "velora/core/snapshot.hpp"

using namespace velora::core;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 48000.0;
constexpr ReverbType kTypes[3] = {ReverbType::kRoom, ReverbType::kHall, ReverbType::kPlate};
const char* const kTypeNames[3] = {"room", "hall", "plate"};

// --- helpers ---------------------------------------------------------------------------------------------------------------
struct Stereo {
  std::vector<float> l, r;
};

// Runs a ReverbSend over `in` (mono, fed to both channels) in 64-frame blocks.
Stereo run_reverb(ReverbSend& rv, const std::vector<float>& in) {
  Stereo o;
  o.l.reserve(in.size());
  o.r.reserve(in.size());
  float l[64], r[64];
  for (std::size_t pos = 0; pos < in.size(); pos += 64) {
    const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(64, in.size() - pos));
    for (std::uint32_t i = 0; i < n; ++i) l[i] = r[i] = in[pos + i];
    StereoBlock b{l, r, n};
    rv.process(b);
    o.l.insert(o.l.end(), l, l + n);
    o.r.insert(o.r.end(), r, r + n);
  }
  return o;
}

// Third-octave band-pass around fc (RBJ constant 0 dB peak gain, Q = 4.32): the measurement filter.
std::vector<double> bandpass(const std::vector<float>& x, double fc) {
  const double w0 = 2.0 * kPi * fc / kFs, q = 4.32, alpha = std::sin(w0) / (2.0 * q);
  const double b0 = alpha, b1 = 0.0, b2 = -alpha, a0 = 1.0 + alpha, a1 = -2.0 * std::cos(w0), a2 = 1.0 - alpha;
  std::vector<double> y(x.size());
  double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  for (std::size_t i = 0; i < x.size(); ++i) {
    const double v = (b0 * x[i] + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0;
    x2 = x1;
    x1 = x[i];
    y2 = y1;
    y1 = v;
    y[i] = v;
  }
  return y;
}

// RT60 from an impulse response: Schroeder backward integration of the squared (band-passed) response, a least-squares line through
// the decay curve between `from_db` and `to_db` below the start (T20: -5..-25, T30: -5..-35), extrapolated to 60 dB.
double rt60_from_ir(const std::vector<double>& ir, double from_db, double to_db) {
  std::vector<double> edc(ir.size());
  double acc = 0;
  for (std::size_t i = ir.size(); i-- > 0;) {
    acc += ir[i] * ir[i];
    edc[i] = acc;
  }
  if (edc.empty() || edc[0] <= 0) return -1;
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  std::size_t n = 0;
  for (std::size_t i = 0; i < edc.size(); ++i) {
    if (edc[i] <= 0) break;
    const double db = 10.0 * std::log10(edc[i] / edc[0]);
    if (db > from_db) continue;
    if (db < to_db) break;
    const double t = static_cast<double>(i) / kFs;
    sx += t;
    sy += db;
    sxx += t * t;
    sxy += t * db;
    ++n;
  }
  if (n < 10) return -1;
  const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);  // dB per second (negative)
  return -60.0 / slope;
}

std::vector<float> impulse(std::size_t len) {
  std::vector<float> v(len, 0.0f);
  v[0] = 1.0f;
  return v;
}

// Deterministic white noise in [-1, 1).
std::vector<float> noise(std::size_t len, std::uint32_t seed = 12345) {
  std::vector<float> v(len);
  std::uint32_t s = seed;
  for (float& x : v) {
    s = s * 1664525u + 1013904223u;
    x = static_cast<float>(static_cast<double>(s >> 8) / 8388608.0 - 1.0);
  }
  return v;
}

bool same_bits_v(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}
double peak_of(const std::vector<float>& v) {
  double m = 0;
  for (float x : v) m = std::max(m, static_cast<double>(std::fabs(x)));
  return m;
}

// --- registry --------------------------------------------------------------------------------------------------------------
void test_registry() {
  const params::ParamDef* ty = params::find(params::kReverbType);
  const params::ParamDef* ti = params::find(params::kReverbTime);
  const params::ParamDef* lv = params::find(params::kReverbLevel);
  const params::ParamDef* rm = params::find(params::kSendReverbMaster);
  CHECK(ty && ti && lv && rm);
  if (!(ty && ti && lv && rm)) return;
  CHECK(ty->id == 530 && ti->id == 531 && lv->id == 532 && rm->id == 801 && params::kSendReverbLevel == 400);
  CHECK(ty->cls == params::Class::kStructural && ty->scope == params::Scope::kMaster && ty->type == params::Type::kEnum);
  CHECK(ty->min == 0 && ty->max == 2 && static_cast<std::uint32_t>(ty->def) == params::kReverbTypeHall && params::kReverbTypeCount == 3);
  CHECK(params::kReverbTypeRoom == 0 && params::kReverbTypeHall == 1 && params::kReverbTypePlate == 2);
  CHECK(ti->cls == params::Class::kStructural && ti->scope == params::Scope::kMaster && ti->min == 0.2 && ti->max == 10.0 && ti->def == 2.0);
  CHECK(lv->cls == params::Class::kStructural && lv->scope == params::Scope::kMaster && lv->min == 0.0 && lv->max == 1.0 && lv->def == 1.0);
  CHECK(rm->cls == params::Class::kPerformance && rm->scope == params::Scope::kMaster && rm->min == 0.0 && rm->max == 1.0 && rm->def == 1.0);
  CHECK(rm->perf_index >= 0 && rm->perf_index != params::find(params::kLineInLevel)->perf_index);
  // the engine's mapping: NaN -> default, clamped ranges
  CHECK(reverbmap::type(0.0f) == ReverbType::kRoom && reverbmap::type(1.0f) == ReverbType::kHall && reverbmap::type(2.0f) == ReverbType::kPlate);
  CHECK(reverbmap::type(NAN) == ReverbType::kHall && reverbmap::type(9.0f) == ReverbType::kPlate && reverbmap::type(-3.0f) == ReverbType::kRoom);
  CHECK(reverbmap::time_s(NAN) == 2.0f && reverbmap::time_s(0.0f) == 0.2f && reverbmap::time_s(99.0f) == 10.0f && reverbmap::time_s(1.5f) == 1.5f);
  CHECK(reverbmap::level(NAN) == 1.0f && reverbmap::level(2.0f) == 1.0f && reverbmap::level(-1.0f) == 0.0f && reverbmap::level(0.25f) == 0.25f);
  // the defaults of the snapshot
  const Snapshot s = Snapshot::defaults();
  CHECK(s.master[static_cast<std::size_t>(ty->master_index)] == static_cast<float>(params::kReverbTypeHall));
  CHECK(s.master[static_cast<std::size_t>(ti->master_index)] == 2.0f && s.master[static_cast<std::size_t>(lv->master_index)] == 1.0f);
  CHECK(s.perf_init[params::perf_slot(*rm, 0)] == 1.0f);
  CHECK(kReverbSendCost > 0 && ReverbSend().cost_units() == kReverbSendCost);
}

// --- RT60 by impulse response ------------------------------------------------------------------------------------------------
// Method (README "Reverb send"): an impulse through the send (both channels, return level 1), the left and the right response each
// band-passed around 1 kHz (third-octave filter), squared, backward-integrated (Schroeder); the line fit between -5 and -25 dB (T20) and between
// -5 and -35 dB (T30) is extrapolated to 60 dB. Pass: both within 15 % of the time parameter, for each type and each test point.
void test_rt60() {
  const double times[] = {0.3, 0.6, 1.2, 2.5, 5.0, 10.0};
  double worst = 0;
  for (int ty = 0; ty < 3; ++ty) {
    for (double t : times) {
      ReverbSend rv;
      rv.configure(kTypes[ty], static_cast<float>(t), 1.0f, /*instant=*/true);
      const std::size_t len = static_cast<std::size_t>((t * 1.6 + 0.5) * kFs) / 64 * 64;
      const Stereo ir = run_reverb(rv, impulse(len));
      for (int ch = 0; ch < 2; ++ch) {
        const std::vector<double> bp = bandpass(ch == 0 ? ir.l : ir.r, 1000.0);
        const double t20 = rt60_from_ir(bp, -5.0, -25.0), t30 = rt60_from_ir(bp, -5.0, -35.0);
        const double e20 = std::fabs(t20 / t - 1.0), e30 = std::fabs(t30 / t - 1.0);
        worst = std::max(worst, std::max(e20, e30));
        if (std::getenv("REVERB_VERBOSE") || !(e20 < 0.15 && e30 < 0.15)) std::printf("  rt60 %s time %.2f ch %d: T20 %.3f T30 %.3f (%.1f%% / %.1f%%)\n", kTypeNames[ty], t, ch, t20, t30, e20 * 100, e30 * 100);
        CHECK(e20 < 0.15 && e30 < 0.15);
      }
    }
  }
  std::printf("reverb rt60: worst deviation from the time parameter %.1f %% (limit 15 %%)\n", worst * 100.0);
  // The types are different parameter sets: their impulse responses differ.
  Stereo irs[3];
  for (int ty = 0; ty < 3; ++ty) {
    ReverbSend rv;
    rv.configure(kTypes[ty], 1.0f, 1.0f, true);
    irs[ty] = run_reverb(rv, impulse(48000));
  }
  CHECK(irs[0].l != irs[1].l && irs[1].l != irs[2].l && irs[0].l != irs[2].l);
  // the response is stereo: left and right differ (decorrelated taps)
  CHECK(irs[1].l != irs[1].r);
  // the time parameter works on a running instance (configure(), not instant) and settles to the new RT60
  {
    ReverbSend rv;
    rv.configure(ReverbType::kHall, 1.0f, 1.0f, true);
    run_reverb(rv, impulse(6400));
    rv.configure(ReverbType::kHall, 4.0f, 1.0f, false);
    const Stereo ir = run_reverb(rv, impulse(static_cast<std::size_t>(7.0 * kFs) / 64 * 64));
    const double t30 = rt60_from_ir(bandpass(ir.l, 1000.0), -5.0, -35.0);
    CHECK(std::fabs(t30 / 4.0 - 1.0) < 0.15);
  }
}

// --- level -----------------------------------------------------------------------------------------------------------------
double rms(const std::vector<float>& v, std::size_t from, std::size_t to) {
  double s = 0;
  for (std::size_t i = from; i < to; ++i) s += static_cast<double>(v[i]) * v[i];
  return std::sqrt(s / static_cast<double>(to - from));
}

void test_level() {
  const std::vector<float> in = noise(48000 * 3);
  const double in_rms = rms(in, 48000, 48000 * 3);
  for (int ty = 0; ty < 3; ++ty) {
    for (float t : {0.3f, 2.0f, 10.0f}) {
      ReverbSend rv;
      rv.configure(kTypes[ty], t, 1.0f, true);
      const Stereo o = run_reverb(rv, in);
      const double db = 20.0 * std::log10(rms(o.l, 48000, 48000 * 3) / in_rms);
      std::printf("  level %s T=%.1f: wet/send rms %.1f dB\n", kTypeNames[ty], t, db);
      CHECK(db > -6.0 && db < 6.0);  // unit-power normalisation: the wet signal is about as loud as the send, whatever the time
      // the return level scales linearly
      ReverbSend half;
      half.configure(kTypes[ty], t, 0.5f, true);
      const Stereo oh = run_reverb(half, in);
      double md = 0;
      for (std::size_t i = 0; i < o.l.size(); ++i) md = std::max(md, static_cast<double>(std::fabs(oh.l[i] - 0.5f * o.l[i])));
      CHECK(md < 1e-6);
    }
  }
  // level 0: silent and no work
  ReverbSend rv;
  rv.configure(ReverbType::kHall, 2.0f, 0.0f, true);
  const Stereo o = run_reverb(rv, noise(6400));
  CHECK(*std::max_element(o.l.begin(), o.l.end()) == 0.0f && *std::min_element(o.l.begin(), o.l.end()) == 0.0f);
  CHECK(rv.engine_blocks() == 0 && rv.quiet());
  // non-finite input is muted
  ReverbSend nf;
  nf.configure(ReverbType::kRoom, 1.0f, 1.0f, true);
  std::vector<float> bad(6400, 0.0f);
  bad[10] = NAN;
  bad[20] = INFINITY;
  bad[30] = 0.5f;
  const Stereo ob = run_reverb(nf, bad);
  bool finite = true;
  for (float v : ob.l) finite = finite && std::isfinite(v);
  CHECK(finite);
}

// --- type change cross-fade ------------------------------------------------------------------------------------------------
double max_step(const std::vector<float>& v, std::size_t from, std::size_t to) {
  double m = 0;
  for (std::size_t i = std::max<std::size_t>(from, 1); i < std::min(to, v.size()); ++i) m = std::max(m, static_cast<double>(std::fabs(v[i] - v[i - 1])));
  return m;
}

void test_type_change() {
  // A steady 220 Hz sine (so the output is a smooth tone: a switch between two different engines would be a step), then a fade-out of
  // the input; the type is changed during the sustained tone and during the ringing tail.
  const std::size_t total = 48000 * 4;
  std::vector<float> in(total);
  for (std::size_t i = 0; i < total; ++i) {
    const double env = i < 48000 * 2 ? 1.0 : std::max(0.0, 1.0 - static_cast<double>(i - 48000 * 2) / 4800.0);
    in[i] = static_cast<float>(0.5 * env * std::sin(2.0 * kPi * 220.0 * static_cast<double>(i) / kFs));
  }
  struct Case { ReverbType from, to; std::size_t at; const char* what; };
  const Case cases[] = {{ReverbType::kRoom, ReverbType::kPlate, 48000 + 640, "sustained room->plate"},
                        {ReverbType::kHall, ReverbType::kRoom, 48000 * 2 + 9600 + 320, "tail hall->room"},
                        {ReverbType::kPlate, ReverbType::kHall, 48000 * 2 + 14400 + 192, "tail plate->hall"}};
  for (const Case& c : cases) {
    auto rv_p = std::make_unique<ReverbSend>();
    ReverbSend& rv = *rv_p;
    rv.configure(c.from, 3.0f, 1.0f, true);
    Stereo o;
    float l[64], r[64];
    bool changed = false;
    for (std::size_t pos = 0; pos < total; pos += 64) {
      if (!changed && pos >= c.at) {
        rv.configure(c.to, 3.0f, 1.0f, false);
        changed = true;
        CHECK(rv.fading());
      }
      for (std::uint32_t i = 0; i < 64; ++i) l[i] = r[i] = in[pos + i];
      StereoBlock b{l, r, 64};
      rv.process(b);
      o.l.insert(o.l.end(), l, l + 64);
    }
    CHECK(!rv.fading() && rv.current_type() == c.to);
    const std::size_t before_from = c.at - 9600, fade_to = c.at + ReverbSend::kTypeFadeFrames + 256;
    // the natural step of either engine's own tone over the same window (the two engines' tones differ in amplitude)
    auto pa_p = std::make_unique<ReverbSend>(), pb_p = std::make_unique<ReverbSend>();
    ReverbSend &pa = *pa_p, &pb = *pb_p;
    pa.configure(c.from, 3.0f, 1.0f, true);
    pb.configure(c.to, 3.0f, 1.0f, true);
    const Stereo ra = run_reverb(pa, in), rb = run_reverb(pb, in);
    const double ref = std::max(max_step(ra.l, before_from, fade_to), max_step(rb.l, before_from, fade_to));
    const double during = max_step(o.l, c.at, fade_to);
    // the output is a tone before the change; the cross-fade of two tones is a tone: the largest step may grow by the amplitude change only
    std::printf("  type change %-22s: natural max step %.5f, during/after the cross-fade %.5f\n", c.what, ref, during);
    CHECK(during < 1.5 * ref + 1e-4);
    // the incoming engine took over: the tail now has the new type's character (the output differs from a run without the change)
    auto plain_p = std::make_unique<ReverbSend>();
    ReverbSend& plain = *plain_p;
    plain.configure(c.from, 3.0f, 1.0f, true);
    const Stereo p = run_reverb(plain, in);
    CHECK(max_step(o.l, 0, 1) == 0.0 && std::memcmp(o.l.data(), p.l.data(), c.at * sizeof(float)) == 0);  // identical up to the change
    CHECK(o.l != p.l);
    // a hard switch of two engines at the same instant is NOT smooth (the test has teeth)
    auto a_p = std::make_unique<ReverbSend>(), b2_p = std::make_unique<ReverbSend>();
    ReverbSend &a = *a_p, &b2 = *b2_p;
    a.configure(c.from, 3.0f, 1.0f, true);
    b2.configure(c.to, 3.0f, 1.0f, true);
    const Stereo oa = run_reverb(a, in), ob = run_reverb(b2, in);
    std::vector<float> hard(oa.l.begin(), oa.l.begin() + static_cast<std::ptrdiff_t>(c.at));
    hard.insert(hard.end(), ob.l.begin() + static_cast<std::ptrdiff_t>(c.at), ob.l.end());
    CHECK(max_step(hard, c.at - 2, c.at + 2) > 2.5 * ref + 1e-4);
  }
  // a request that arrives mid-fade waits and the latest wins: room -> plate -> hall -> room ends on room, one fade at a time
  {
    auto rv_p = std::make_unique<ReverbSend>();
    ReverbSend& rv = *rv_p;
    rv.configure(ReverbType::kRoom, 2.0f, 1.0f, true);
    run_reverb(rv, noise(6400));
    rv.configure(ReverbType::kPlate, 2.0f, 1.0f, false);
    rv.configure(ReverbType::kHall, 2.0f, 1.0f, false);
    rv.configure(ReverbType::kRoom, 2.0f, 1.0f, false);  // back to where it started: the final state is room
    run_reverb(rv, noise(48000));
    CHECK(!rv.fading() && rv.current_type() == ReverbType::kRoom);
    rv.configure(ReverbType::kPlate, 2.0f, 1.0f, false);
    rv.configure(ReverbType::kHall, 2.0f, 1.0f, false);
    run_reverb(rv, noise(48000 * 2));
    CHECK(!rv.fading() && rv.current_type() == ReverbType::kHall);
  }
}

// --- idle: quiet() and the work ---------------------------------------------------------------------------------------------
void test_idle() {
  ReverbSend rv;
  rv.configure(ReverbType::kRoom, 0.5f, 1.0f, true);
  CHECK(rv.quiet() && rv.engine_blocks() == 0);
  std::vector<float> silence(6400, 0.0f);
  const Stereo z = run_reverb(rv, silence);
  CHECK(rv.engine_blocks() == 0 && *std::max_element(z.l.begin(), z.l.end()) == 0.0f);
  // a burst starts it; it rings and, after the tail decayed, clears itself and does no more work
  std::vector<float> burst = noise(1280);
  burst.resize(48000 * 6, 0.0f);
  Stereo o = run_reverb(rv, burst);
  CHECK(rv.quiet());
  const std::uint64_t blocks = rv.engine_blocks();
  CHECK(blocks > 20 && blocks < 48000 * 6 / 64);  // it ran, and it stopped before the end of the silence
  const Stereo z2 = run_reverb(rv, silence);
  CHECK(rv.engine_blocks() == blocks && *std::max_element(z2.l.begin(), z2.l.end()) == 0.0f);
  // the cleared tail ended below -150 dBFS: the last samples before the flush are tiny
  CHECK(std::fabs(o.l.back()) == 0.0f);
}

// --- Mixer: sends, REV master, idle bit-exactness ------------------------------------------------------------------------------
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

bool same_bits(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}
double max_diff(const std::vector<float>& a, const std::vector<float>& b) {
  double m = 0;
  for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
  return m;
}

// Renders parts 0 (note 60), 1 (note 64) and 2 (note 67) with the given reverb send levels and settings. Parts are channels 0..2.
Cap render_sends(float s0, float s1, float s2, int blocks, float master = 1.0f, bool part2_aux = false, ReverbType ty = ReverbType::kHall,
                 float time = 1.0f, float level = 1.0f) {
  Rig rig;
  Snapshot s = Snapshot::defaults();
  s.perf_init[perf(params::kSendReverbLevel, 0)] = s0;
  s.perf_init[perf(params::kSendReverbLevel, 1)] = s1;
  s.perf_init[perf(params::kSendReverbLevel, 2)] = s2;
  s.perf_init[perf(params::kSendReverbMaster, 0)] = master;
  if (part2_aux) s.parts[2].output = params::kPartOutputAux;
  set_master(s, params::kReverbType, static_cast<float>(static_cast<int>(ty)));
  set_master(s, params::kReverbTime, time);
  set_master(s, params::kReverbLevel, level);
  rig.publish(s);
  rig.note(true, 0, 60);
  rig.note(true, 1, 64);
  rig.note(true, 2, 67);
  return rig.run(blocks);
}

void test_idle_bit_exact() {
  constexpr int kBlocks = 60;
  // pre-reverb behaviour: no snapshot at all (the pre-2.7 path)
  Cap plain;
  {
    Rig rig;
    rig.note(true, 0, 60);
    rig.note(true, 1, 64);
    rig.note(true, 2, 67);
    plain = rig.run(kBlocks);
  }
  // all send levels 0, whatever the reverb settings: bit for bit the same, the Mixer stays inactive, the reverb does no work
  for (int ty = 0; ty < 3; ++ty) {
    Rig rig;
    Snapshot s = Snapshot::defaults();
    set_master(s, params::kReverbType, static_cast<float>(ty));
    set_master(s, params::kReverbTime, 10.0f);
    rig.publish(s);
    rig.note(true, 0, 60);
    rig.note(true, 1, 64);
    rig.note(true, 2, 67);
    const Cap c = rig.run(kBlocks);
    CHECK(same_bits(c.l, plain.l) && same_bits(c.r, plain.r));
    CHECK(!rig.br->mixer().routed());
    CHECK(rig.br->mixer().reverb(0).engine_blocks() == 0 && rig.br->mixer().reverb(1).engine_blocks() == 0);
    CHECK(rig.br->mixer().reverb(0).quiet() && rig.br->mixer().reverb(1).quiet());
  }
  // REV master 0 with part sends up: the reverb is idle as well (output bit-identical, inactive mixer)
  {
    Rig rig;
    Snapshot s = Snapshot::defaults();
    s.perf_init[perf(params::kSendReverbLevel, 0)] = 1.0f;
    s.perf_init[perf(params::kSendReverbLevel, 1)] = 0.5f;
    s.perf_init[perf(params::kSendReverbMaster, 0)] = 0.0f;
    rig.publish(s);
    rig.note(true, 0, 60);
    rig.note(true, 1, 64);
    rig.note(true, 2, 67);
    const Cap c = rig.run(kBlocks);
    CHECK(same_bits(c.l, plain.l) && same_bits(c.r, plain.r));
    CHECK(!rig.br->mixer().routed() && rig.br->mixer().reverb(0).engine_blocks() == 0);
  }
  // return level 0 with part sends up: the wet signal is silent (output = dry), and the reverb does no work
  {
    const Cap c = render_sends(1.0f, 0.5f, 0.0f, kBlocks, 1.0f, false, ReverbType::kHall, 2.0f, 0.0f);
    CHECK(same_bits(c.l, plain.l) && same_bits(c.r, plain.r));
  }
  // sends turned off while the tail rings: the Mixer stays routed while the tail rings, then goes inactive and the output is exactly dry
  {
    Rig rig;
    Snapshot s = Snapshot::defaults();
    s.perf_init[perf(params::kSendReverbLevel, 0)] = 1.0f;
    set_master(s, params::kReverbTime, 0.5f);
    rig.publish(s);
    rig.note(true, 0, 60);
    rig.run(30);
    CHECK(rig.br->mixer().routed());
    rig.note(false, 0, 60);
    rig.param(params::kSendReverbLevel, 0, 0.0);
    rig.run(8);
    CHECK(rig.br->mixer().routed() && !rig.br->mixer().reverb(0).quiet());  // still ringing
    rig.run(48000 * 4 / 64);
    CHECK(rig.br->mixer().reverb(0).quiet() && !rig.br->mixer().routed());   // gone: transparent again
    const std::uint64_t blocks = rig.br->mixer().reverb(0).engine_blocks();
    const Cap c = rig.run(100);
    CHECK(rig.br->mixer().reverb(0).engine_blocks() == blocks);
    bool zero = true;
    for (float v : c.l) zero = zero && v == 0.0f;
    CHECK(zero);  // the note is off: silence, exactly
  }
}

void test_sends() {
  constexpr int kBlocks = 80;
  const Cap none = render_sends(0, 0, 0, kBlocks);
  CHECK(!none.l.empty());
  // The wet signal is added: a send makes the output differ, and it is linear in the send level.
  const Cap a5 = render_sends(0.5f, 0, 0, kBlocks), a10 = render_sends(1.0f, 0, 0, kBlocks);
  CHECK(max_diff(a5.l, none.l) > 0.01 && max_diff(a10.l, none.l) > max_diff(a5.l, none.l));
  {
    std::vector<float> w5(a5.l.size()), w10(a5.l.size());
    for (std::size_t i = 0; i < w5.size(); ++i) {
      w5[i] = a5.l[i] - none.l[i];
      w10[i] = a10.l[i] - none.l[i];
    }
    double md = 0;
    for (std::size_t i = 0; i < w5.size(); ++i) md = std::max(md, static_cast<double>(std::fabs(2.0f * w5[i] - w10[i])));
    CHECK(md < 2e-6);
  }
  // Only that part's contribution changes: the wet signals of the parts add (linearity). Changing part A's send changes the output by
  // exactly what A alone contributes; part B's wet contribution does not move.
  {
    const Cap b3 = render_sends(0, 0.3f, 0, kBlocks);                 // B alone
    const Cap ab = render_sends(0.5f, 0.3f, 0, kBlocks);              // A at 0.5, B at 0.3
    const Cap ab2 = render_sends(0.8f, 0.3f, 0, kBlocks);             // A raised to 0.8
    const Cap a8 = render_sends(0.8f, 0, 0, kBlocks);                 // A alone at 0.8
    std::vector<float> d1(none.l.size()), d2(none.l.size()), e1(none.l.size());
    for (std::size_t i = 0; i < d1.size(); ++i) {
      d1[i] = ab2.l[i] - ab.l[i];          // change of the output when A goes 0.5 -> 0.8
      d2[i] = a8.l[i] - a5.l[i];           // the same change with B silent
      e1[i] = ab.l[i] - a5.l[i] - (b3.l[i] - none.l[i]);  // the wet of B inside the mix equals B's wet alone
    }
    double m1 = 0, m2 = 0;
    for (std::size_t i = 0; i < d1.size(); ++i) {
      m1 = std::max(m1, static_cast<double>(std::fabs(d1[i] - d2[i])));
      m2 = std::max(m2, static_cast<double>(std::fabs(e1[i])));
    }
    CHECK(m1 < 3e-6);
    CHECK(m2 < 3e-6);
    CHECK(max_diff(b3.l, none.l) > 0.005);
  }
  // A live send change is taken at the next block and smoothed over a block: the dry signal is untouched before it, the wet starts at 0.
  {
    Rig rig;
    Snapshot s = Snapshot::defaults();
    set_master(s, params::kReverbType, 0.0f);  // room: short pre-delay
    rig.publish(s);
    rig.note(true, 0, 60);
    const Cap before = rig.run(10);
    rig.param(params::kSendReverbLevel, 0, 1.0, 3);
    const Cap after = rig.run(20);
    Rig ref;
    ref.note(true, 0, 60);
    ref.run(10);
    const Cap dry = ref.run(20);
    CHECK(max_diff(after.l, dry.l) > 0.001);
    // no wet before the next block begins: the first block after the event matches the dry render exactly
    bool first_block_dry = true;
    for (std::size_t i = 0; i < kBlockFrames; ++i) first_block_dry = first_block_dry && after.l[i] == dry.l[i];
    CHECK(first_block_dry);
    (void)before;
  }
  // REV master scales all parts' sends together: master 0.5 gives half the wet of every part, master 1 the full wet.
  {
    const Cap m10 = render_sends(0.6f, 0.4f, 0.2f, kBlocks, 1.0f);
    const Cap m05 = render_sends(0.6f, 0.4f, 0.2f, kBlocks, 0.5f);
    const Cap m00 = render_sends(0.6f, 0.4f, 0.2f, kBlocks, 0.0f);
    double md = 0;
    for (std::size_t i = 0; i < none.l.size(); ++i) {
      const float w1 = m10.l[i] - none.l[i], wh = m05.l[i] - none.l[i];
      md = std::max(md, static_cast<double>(std::fabs(wh - 0.5f * w1)));
    }
    CHECK(md < 3e-6);
    CHECK(same_bits(m00.l, none.l) && same_bits(m00.r, none.r));
    // equal to scaling every part's send by the master
    const Cap sc = render_sends(0.3f, 0.2f, 0.1f, kBlocks, 1.0f);
    CHECK(max_diff(sc.l, m05.l) < 3e-6);
  }
  // REV master is a live performance parameter (master scope: the part field of its event is ignored) and ramps
  {
    Rig rig;
    Snapshot s = Snapshot::defaults();
    s.perf_init[perf(params::kSendReverbLevel, 0)] = 1.0f;
    rig.publish(s);
    rig.note(true, 0, 60);
    rig.run(10);
    rig.param(params::kSendReverbMaster, 0, 0.0, 5);
    rig.run(3);
    CHECK(std::fabs(rig.br->last_values().load(perf(params::kSendReverbMaster, 0))) < 1e-6f);
    // master at 0 and a part send up: after the tail rings out the mixer goes inactive
    rig.note(false, 0, 60);
    rig.run(48000 * 6 / 64);
    CHECK(!rig.br->mixer().routed());
    rig.param(params::kSendReverbMaster, 0, 1.0);
    rig.note(true, 0, 60);
    const Cap c = rig.run(30);
    CHECK(rig.br->mixer().routed() && rig.br->mixer().reverb(0).engine_blocks() > 0);
    (void)c;
  }
  // Main and Aux each have their own reverb: an Aux part's send is returned to Aux, Main only holds the other parts
  {
    const Cap c = render_sends(0, 0, 1.0f, kBlocks, 1.0f, /*part2_aux=*/true);
    const Cap c0 = render_sends(0, 0, 0.0f, kBlocks, 1.0f, /*part2_aux=*/true);
    CHECK(same_bits(c.l, c0.l) && same_bits(c.r, c0.r));  // Main untouched
    CHECK(max_diff(c.al, c0.al) > 0.005 && max_diff(c.ar, c0.ar) > 0.005);
    Rig rig;
    Snapshot s = Snapshot::defaults();
    s.parts[2].output = params::kPartOutputAux;
    s.perf_init[perf(params::kSendReverbLevel, 2)] = 1.0f;
    rig.publish(s);
    rig.note(true, 2, 67);
    rig.run(30);
    CHECK(rig.br->mixer().reverb(1).engine_blocks() > 0 && rig.br->mixer().reverb(0).engine_blocks() == 0);
  }
  // type and time reach the instances of both buses through a new generation; the level too
  {
    Rig rig;
    Snapshot s = Snapshot::defaults();
    s.perf_init[perf(params::kSendReverbLevel, 0)] = 0.5f;
    set_master(s, params::kReverbType, 0.0f);
    set_master(s, params::kReverbTime, 3.0f);
    set_master(s, params::kReverbLevel, 0.25f);
    rig.publish(s);
    rig.note(true, 0, 60);
    rig.run(20);
    for (std::uint32_t b = 0; b < 2; ++b) {
      CHECK(rig.br->mixer().reverb(b).current_type() == ReverbType::kRoom && rig.br->mixer().reverb(b).time_s() == 3.0f &&
            rig.br->mixer().reverb(b).level() == 0.25f);
    }
    set_master(s, params::kReverbType, 2.0f);
    set_master(s, params::kReverbTime, 6.0f);
    rig.publish(s);
    rig.run(10);
    for (std::uint32_t b = 0; b < 2; ++b) CHECK(rig.br->mixer().reverb(b).time_s() == 6.0f);
    CHECK(rig.br->mixer().reverb(0).fading());
    rig.run(48000 / 64);
    CHECK(rig.br->mixer().reverb(0).current_type() == ReverbType::kPlate && !rig.br->mixer().reverb(0).fading());
  }
}


// --- block sizes, level changes, idle reconfiguration, level 0 (review follow-ups) ---------------------------------------------
void test_block_sizes() {
  const std::vector<float> in = noise(6400 + 7, 99);
  auto run_sizes = [&](std::uint32_t size) {
    auto rv = std::make_unique<ReverbSend>();
    rv->configure(ReverbType::kPlate, 1.0f, 0.8f, true);
    Stereo o;
    std::vector<float> l(size), r(size);
    for (std::size_t pos = 0; pos < in.size(); pos += size) {
      const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(size, in.size() - pos));
      std::copy(in.begin() + static_cast<std::ptrdiff_t>(pos), in.begin() + static_cast<std::ptrdiff_t>(pos + n), l.begin());
      std::copy(l.begin(), l.begin() + n, r.begin());
      StereoBlock b{l.data(), r.data(), n};
      rv->process(b);
      o.l.insert(o.l.end(), l.begin(), l.begin() + n);
      o.r.insert(o.r.end(), r.begin(), r.begin() + n);
    }
    return o;
  };
  const Stereo ref = run_sizes(64);
  for (std::uint32_t size : {200u, 1u, 7u, 64u * 5u}) {
    const Stereo o = run_sizes(size);
    CHECK(same_bits_v(o.l, ref.l) && same_bits_v(o.r, ref.r));
  }
}

void test_level_changes() {
  const std::vector<float> in = noise(48000);
  auto make = [] { auto rv = std::make_unique<ReverbSend>(); rv->configure(ReverbType::kHall, 1.5f, 1.0f, true); return rv; };
  auto run_to = [&](ReverbSend& rv, float new_level, std::size_t change_at) {
    Stereo o;
    float l[64], r[64];
    for (std::size_t pos = 0; pos < in.size(); pos += 64) {
      if (pos == change_at) rv.configure(ReverbType::kHall, 1.5f, new_level, false);
      for (std::uint32_t i = 0; i < 64; ++i) l[i] = r[i] = in[pos + i];
      StereoBlock b{l, r, 64};
      rv.process(b);
      o.l.insert(o.l.end(), l, l + 64);
    }
    return o;
  };
  auto base = make(), a = make(), z = make();
  const Stereo ref = run_to(*base, 1.0f, SIZE_MAX);
  const Stereo o3 = run_to(*a, 0.3f, 64 * 100);
  const std::size_t at = 64 * 100;
  double md = 0, step = 0, nat = 0;
  for (std::size_t i = at + 64; i < ref.l.size(); ++i) md = std::max(md, static_cast<double>(std::fabs(o3.l[i] - 0.3f * ref.l[i])));
  CHECK(md < 1e-6);
  for (std::size_t i = at + 1; i < at + 64; ++i) {
    step = std::max(step, static_cast<double>(std::fabs(o3.l[i] - o3.l[i - 1])));
    nat = std::max(nat, static_cast<double>(std::fabs(ref.l[i] - ref.l[i - 1])));
  }
  double pk = 0;
  for (std::size_t i = at; i < at + 64; ++i) pk = std::max(pk, static_cast<double>(std::fabs(ref.l[i])));
  CHECK(step <= nat + 0.7 / 64.0 * pk + 1e-6);  // ramp-limited: no jump to the new level
  // 1 -> 0: after the ramp the wet is exactly 0, the reverb is quiet and does no more work
  const Stereo o0 = run_to(*z, 0.0f, at);
  bool zero = true;
  for (std::size_t i = at + 64; i < o0.l.size(); ++i) zero = zero && o0.l[i] == 0.0f;
  CHECK(zero && z->quiet());
  const std::uint64_t eb = z->engine_blocks();
  run_reverb(*z, noise(6400));
  CHECK(z->engine_blocks() == eb);
  // back up from 0: the first block uses the target level (a ramp from 0 over one block) and the reverb runs again
  z->configure(ReverbType::kHall, 1.5f, 0.5f, false);
  const Stereo up = run_reverb(*z, noise(6400));
  CHECK(z->engine_blocks() > eb && peak_of(up.l) > 0.01);
  // a ramp left pending on an idle reverb is settled by the idle block
  auto id = make();
  id->configure(ReverbType::kHall, 1.5f, 1.0f, true);
  id->reset();
  id->configure(ReverbType::kHall, 1.5f, 0.4f, false);
  run_reverb(*id, std::vector<float>(64, 0.0f));
  auto fresh = std::make_unique<ReverbSend>();
  fresh->configure(ReverbType::kHall, 1.5f, 0.4f, true);
  const Stereo r1 = run_reverb(*id, noise(3200, 5)), r2 = run_reverb(*fresh, noise(3200, 5));
  CHECK(same_bits_v(r1.l, r2.l));
  // level 0 while a type change is in flight: the newest type is adopted
  auto lf = make();
  run_reverb(*lf, noise(6400));
  lf->configure(ReverbType::kPlate, 1.5f, 1.0f, false);
  lf->configure(ReverbType::kRoom, 1.5f, 0.0f, false);  // waits (fading) and level goes to 0
  run_reverb(*lf, noise(6400));
  CHECK(lf->quiet() && !lf->fading() && lf->current_type() == ReverbType::kRoom);
}

void test_idle_reconfigure() {
  auto idle = std::make_unique<ReverbSend>();
  idle->configure(ReverbType::kHall, 2.0f, 1.0f, true);
  idle->configure(ReverbType::kPlate, 3.0f, 1.0f, false);  // nothing rings: switches at once
  CHECK(idle->quiet() && !idle->fading() && idle->current_type() == ReverbType::kPlate && idle->time_s() == 3.0f);
  auto fresh = std::make_unique<ReverbSend>();
  fresh->configure(ReverbType::kPlate, 3.0f, 1.0f, true);
  const Stereo a = run_reverb(*idle, impulse(24000)), b = run_reverb(*fresh, impulse(24000));
  CHECK(same_bits_v(a.l, b.l) && same_bits_v(a.r, b.r));
}

void test_mixer_generations() {
  constexpr int kPre = 20, kPost = 30;
  auto gens = [&](float level2) {
    Rig rig;
    Snapshot s = Snapshot::defaults();
    s.perf_init[perf(params::kSendReverbLevel, 0)] = 1.0f;
    rig.publish(s);
    rig.note(true, 0, 60);
    Cap c = rig.run(kPre);
    set_master(s, params::kReverbLevel, level2);
    rig.publish(s);
    const Cap c2 = rig.run(kPost);
    c.l.insert(c.l.end(), c2.l.begin(), c2.l.end());
    CHECK(rig.br->mixer().reverb(0).level() == level2 && rig.br->mixer().reverb(1).level() == level2);
    return c;
  };
  const Cap full = gens(1.0f), part = gens(0.3f);
  Cap dry;
  {
    Rig d;
    d.note(true, 0, 60);
    dry = d.run(kPre + kPost);
  }
  double md = 0;
  for (std::size_t i = static_cast<std::size_t>(kPre + 2) * 64; i < full.l.size(); ++i)
    md = std::max(md, static_cast<double>(std::fabs((part.l[i] - dry.l[i]) - 0.3f * (full.l[i] - dry.l[i]))));
  CHECK(md < 3e-6);
  // inactive Mixer, new generation with another type and time: the idle reverb adopts them, and the result equals a Mixer that had them from the start
  auto run_idle = [&](bool late) {
    Rig rig;
    Snapshot s = Snapshot::defaults();
    if (late) {
      rig.publish(s);
      rig.run(10);
    }
    set_master(s, params::kReverbType, 2.0f);
    set_master(s, params::kReverbTime, 3.0f);
    s.perf_init[perf(params::kSendReverbLevel, 0)] = 1.0f;
    rig.publish(s);
    rig.run(5);
    if (late) CHECK(rig.br->mixer().reverb(0).current_type() == ReverbType::kPlate && !rig.br->mixer().reverb(0).fading());
    rig.note(true, 0, 60);
    return rig.run(60);
  };
  const Cap early = run_idle(false), late = run_idle(true);
  CHECK(same_bits(early.l, late.l) && same_bits(early.r, late.r));
  // an inactive Mixer whose reverb was reconfigured (no sends): still inactive and idle
  {
    Rig rig;
    rig.publish(Snapshot::defaults());
    rig.run(4);
    Snapshot s = Snapshot::defaults();
    set_master(s, params::kReverbType, 0.0f);
    rig.publish(s);
    rig.run(4);
    CHECK(!rig.br->mixer().routed() && rig.br->mixer().reverb(0).current_type() == ReverbType::kRoom && !rig.br->mixer().reverb(0).fading());
  }
}

// Level 0 with sends up (documented in the README): the Mixer stays routed (a send is up), the reverb does no work, and the output is exactly the dry one.
void test_level_zero_routed() {
  Rig rig;
  Snapshot s = Snapshot::defaults();
  s.perf_init[perf(params::kSendReverbLevel, 0)] = 1.0f;
  set_master(s, params::kReverbLevel, 0.0f);
  rig.publish(s);
  rig.note(true, 0, 60);
  const Cap c = rig.run(40);
  CHECK(rig.br->mixer().routed());
  CHECK(rig.br->mixer().reverb(0).engine_blocks() == 0 && rig.br->mixer().reverb(0).quiet());
  const Cap dry = render_sends(0, 0, 0, 40);  // render_sends plays 3 notes; compare part 0 alone below
  Rig ref;
  ref.note(true, 0, 60);
  const Cap plain = ref.run(40);
  CHECK(max_diff(c.l, plain.l) < 1e-6);
  (void)dry;
}

// A part send raised while the REV master is 0, then the master raised: no step beyond the block ramp.
void test_master_leaves_zero() {
  Rig rig;
  Snapshot s = Snapshot::defaults();
  s.perf_init[perf(params::kSendReverbMaster, 0)] = 0.0f;
  set_master(s, params::kReverbType, 0.0f);
  rig.publish(s);
  rig.note(true, 0, 60);
  rig.run(10);
  rig.param(params::kSendReverbLevel, 0, 1.0);  // part send up, master still 0
  const Cap before = rig.run(10);
  CHECK(!rig.br->mixer().routed());
  rig.param(params::kSendReverbMaster, 0, 1.0);
  const Cap after = rig.run(40);
  Rig ref;
  ref.note(true, 0, 60);
  ref.run(20);
  const Cap dry = ref.run(40);
  // wet = after - dry: it starts from 0 and grows with the one-block master ramp; the first block after the event is still dry
  bool first_dry = true;
  for (std::size_t i = 0; i < 64; ++i) first_dry = first_dry && std::fabs(after.l[i] - dry.l[i]) < 1e-7;
  CHECK(first_dry);
  double step = 0, pk = 0;
  for (std::size_t i = 64; i < after.l.size(); ++i) {
    const double w = after.l[i] - dry.l[i], w1 = after.l[i - 1] - dry.l[i - 1];
    step = std::max(step, std::fabs(w - w1));
    pk = std::max(pk, std::fabs(w));
  }
  CHECK(pk > 0.001);
  CHECK(step < 0.2 * pk + 1e-6);  // continuous: no jump to the full send level
  (void)before;
}

void test_determinism_and_finite() {
  const Cap a = render_sends(0.7f, 0.5f, 0.3f, 100, 1.0f, true, ReverbType::kPlate, 7.0f);
  const Cap b = render_sends(0.7f, 0.5f, 0.3f, 100, 1.0f, true, ReverbType::kPlate, 7.0f);
  CHECK(same_bits(a.l, b.l) && same_bits(a.r, b.r) && same_bits(a.al, b.al) && same_bits(a.ar, b.ar));
  bool finite = true;
  for (float v : a.l) finite = finite && std::isfinite(v);
  CHECK(finite);
}

}  // namespace

int main() {
  test_registry();
  test_rt60();
  test_level();
  test_type_change();
  test_idle();
  test_idle_bit_exact();
  test_sends();
  test_block_sizes();
  test_level_changes();
  test_idle_reconfigure();
  test_mixer_generations();
  test_level_zero_routed();
  test_master_leaves_zero();
  test_determinism_and_finite();
  if (g_fail) std::printf("reverb_tests: %d failure(s)\n", g_fail);
  else std::printf("reverb_tests: ok\n");
  return g_fail ? 1 : 0;
}
