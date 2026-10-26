// Story 3.6: velocity curves, the calibration -> curve chain, release velocity, the sample engine's release-trigger layers and the curve setter.
//   * every curve reaches velocity 1 and 127 over the calibrated range with the 127 steps spread (no gap, no plateau); Fixed is constant; clamps; ordering Light > Normal > Heavy
//   * the reader applies the chain once: a known contact time reaches the engine's voice as chain(time), not chain(chain(time)) (BlockRenderer + SampleEngine)
//   * release velocity on the Note Off (fast > slow, default and counted without a pair or with lost data) and the sample engine's release-trigger layer / gain (render test)
//   * the curve setter is safe against the reader running on another thread and applies to the next note only
// Bare main, CHECK style as the other host tests.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/contact_events.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/host/keybed_calibration.hpp"
#include "velora/host/keybed_reader.hpp"
#include "velora/host/velocity_curve.hpp"
#include "velora/host/velocity_curve_holder.hpp"

using namespace velora;
using namespace velora::host;
namespace ports = velora::ports;

static int g_fail = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    if (!(c)) {                                                          \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
      ++g_fail;                                                          \
    }                                                                    \
  } while (0)

namespace {

constexpr std::uint64_t kMs = 1'000'000ull;
constexpr std::uint64_t kSec = 1'000'000'000ull;

// ---- 1. the curves ------------------------------------------------------------------------------------------------

void test_curves() {
  const VelocityRange range{};  // 2 ms .. 100 ms
  const VelocityCurve curves[] = {VelocityCurve::kNormal, VelocityCurve::kLight, VelocityCurve::kHeavy};
  for (VelocityCurve c : curves) {
    // Every 1 us of travel time strictly inside (fast, slow) (the MCU tick is 1 us): the 7-bit velocities are exactly 1..127, each bin non-empty with a minimum
    // width of a few us, monotonic non-increasing in time. The 16 -> 7 bit step is cross-checked against a literal copy of the sample engine's formula.
    std::uint64_t width[128] = {};
    std::uint16_t prev = 0xFFFF;
    bool monotonic = true, same7 = true;
    for (std::uint64_t dt = range.fast_ns + 1000; dt < range.slow_ns; dt += 1000) {
      const std::uint16_t v = chain_velocity16(range, c, kFixedVelocity7, dt);
      if (v > prev) monotonic = false;
      prev = v;
      std::uint8_t engine7 = static_cast<std::uint8_t>((static_cast<std::uint32_t>(v) * 127u + 32767u) / 65535u);  // sample_engine.cpp start_voice()
      if (engine7 == 0) engine7 = 1;
      if (engine7 != velocity16_to_7(v)) same7 = false;
      CHECK(engine7 >= 1 && engine7 <= 127);
      ++width[engine7];
    }
    CHECK(monotonic && same7);
    std::uint64_t thinnest = ~0ull;
    int thinnest_bin = 0;
    bool all = true;
    for (int v = 1; v <= 127; ++v) {
      all = all && width[v] > 0;
      if (width[v] < thinnest) { thinnest = width[v]; thinnest_bin = v; }
    }
    CHECK(all && thinnest >= 3);  // at 1 us resolution every bin is at least 3 us wide (the thinnest are Light bin 1 and Heavy bin 127, see the README)
    std::printf("curve %d: thinnest bin %d is %llu us wide\n", static_cast<int>(c), thinnest_bin, static_cast<unsigned long long>(thinnest));
    // clamps: faster than the fastest -> 127 (65535), slower than the slowest -> 1 (512)
    CHECK(chain_velocity16(range, c, kFixedVelocity7, 0) == 0xFFFF && chain_velocity16(range, c, kFixedVelocity7, range.fast_ns) == 0xFFFF);
    CHECK(chain_velocity16(range, c, kFixedVelocity7, range.slow_ns) == kMinVelocity16 && chain_velocity16(range, c, kFixedVelocity7, ~0ull >> 1) == kMinVelocity16);
    CHECK(velocity16_to_7(0xFFFF) == 127 && velocity16_to_7(kMinVelocity16) == 1);
    // strictly monotonic 16-bit values strictly inside the range
    bool strict = true;
    std::uint16_t last = 0xFFFF;
    for (int i = 1; i < 1500; ++i) {
      const double f = static_cast<double>(i) / 1500.0;
      const std::uint64_t dt = static_cast<std::uint64_t>(static_cast<double>(range.fast_ns) * std::pow(static_cast<double>(range.slow_ns) / range.fast_ns, f));
      const std::uint16_t v = chain_velocity16(range, c, kFixedVelocity7, dt);
      if (!(v < last)) strict = false;
      last = v;
    }
    CHECK(strict);
  }
  // ordering: Light above Normal above Heavy at every time strictly inside the range
  bool order = true;
  for (int i = 1; i < 200; ++i) {
    const std::uint64_t dt = static_cast<std::uint64_t>(2.0e6 * std::pow(50.0, i / 200.0));
    const std::uint16_t l = chain_velocity16(range, VelocityCurve::kLight, kFixedVelocity7, dt);
    const std::uint16_t nm = chain_velocity16(range, VelocityCurve::kNormal, kFixedVelocity7, dt);
    const std::uint16_t h = chain_velocity16(range, VelocityCurve::kHeavy, kFixedVelocity7, dt);
    if (!(l > nm && nm > h)) order = false;
  }
  CHECK(order);
  // Fixed: constant (the configured 7-bit value, 100 by default), whatever the time; it reaches the fixed velocity and is not part of the 1..127 spread
  CalibratedVelocityMapper m(range, VelocityCurve::kFixed);
  CHECK(m.velocity16(0) == m.velocity16(10 * kMs) && m.velocity16(10 * kMs) == m.velocity16(kSec) && velocity16_to_7(m.velocity16(5 * kMs)) == kFixedVelocity7);
  m.set_fixed_velocity(64);
  CHECK(velocity16_to_7(m.velocity16(5 * kMs)) == 64 && m.velocity16(1) == m.velocity16(~0ull >> 1));
  m.set_curve(VelocityCurve::kNormal);
  CHECK(m.curve() == VelocityCurve::kNormal && m.velocity16(2 * kMs) == 0xFFFF);
  // a degenerate range is still defined
  CHECK(chain_velocity16(VelocityRange{0, 0}, VelocityCurve::kNormal, 100, 5) == kMinVelocity16);
}

// ---- helpers shared by the reader tests ---------------------------------------------------------------------------

struct SimMcu {
  std::uint32_t tick0 = 1000;
  std::uint64_t t0_ns = 0;
  mcu::FrameWriter writer;
  std::vector<std::uint8_t> key(std::uint8_t k, std::uint8_t contact, bool closed, std::uint64_t true_ns) {
    const std::uint32_t tick = tick0 + static_cast<std::uint32_t>((true_ns - t0_ns) / 1000);
    return writer.write(mcu::KeyContact{k, contact, closed}, tick);
  }
};

struct Rig {
  HostEventQueue q;
  SimMcu mcu;
  std::unique_ptr<KeybedReader> reader;
  std::vector<StampedUmp> ev;
  std::uint64_t host0 = 10 * kSec;
  explicit Rig(KeybedCalibration t) {
    mcu.t0_ns = host0;
    reader = std::make_unique<KeybedReader>(q, std::move(t));
  }
  void drain() {
    StampedUmp s;
    while (q.pop(s)) ev.push_back(s);
  }
  void key(std::uint8_t k, std::uint8_t c, bool closed, std::uint64_t true_ns) {
    const auto w = mcu.key(k, c, closed, true_ns);
    reader->on_bytes(w.data(), w.size(), true_ns + 300'000);
    drain();
  }
  // contacts 1..3 close `d` apart starting at t (a Note On with travel time 2 * d), then the key is held
  void press3(std::uint8_t k, std::uint64_t t, std::uint64_t d) {
    key(k, 0, true, t);
    key(k, 1, true, t + d);
    key(k, 2, true, t + 2 * d);
  }
  // 3-contact release: contact 3 opens at t, contact 2 `d` later, contact 1 `d` after that
  void release3(std::uint8_t k, std::uint64_t t, std::uint64_t d) {
    key(k, 2, false, t);
    key(k, 1, false, t + d);
    key(k, 0, false, t + 2 * d);
  }
  std::vector<StampedUmp> of(bool on) const {
    std::vector<StampedUmp> v;
    for (const StampedUmp& s : ev)
      if (on ? s.ump.is_note_on() : s.ump.is_note_off()) v.push_back(s);
    return v;
  }
};

// ---- 2. the reader's chain ----------------------------------------------------------------------------------------

void test_reader_chain() {
  const KeybedCalibration t3 = KeybedCalibration::three_contact_73();
  const VelocityRange vr = t3.velocity_range();
  for (VelocityCurve c : {VelocityCurve::kNormal, VelocityCurve::kLight, VelocityCurve::kHeavy}) {
    Rig r(t3);
    r.reader->set_velocity_curve(c);
    r.press3(10, r.host0, 10 * kMs);  // travel time 20 ms
    const auto on = r.of(true);
    CHECK(on.size() == 1);
    if (on.size() == 1) {
      const std::uint16_t want = chain_velocity16(vr, c, kFixedVelocity7, 20 * kMs);
      CHECK(std::llabs(static_cast<long long>(on[0].ump.velocity16()) - static_cast<long long>(want)) < 40);
    }
  }
  // default (no config) is Normal
  Rig d(t3);
  CHECK(d.reader->velocity_curve() == VelocityCurve::kNormal);
  // Fixed: the same velocity for a fast and a slow key
  Rig f(t3);
  f.reader->set_velocity_curve(VelocityCurve::kFixed);
  f.press3(1, f.host0, kMs);
  f.release3(1, f.host0 + kSec, kMs);
  f.press3(1, f.host0 + 2 * kSec, 50 * kMs);
  const auto fon = f.of(true);
  CHECK(fon.size() == 2 && fon[0].ump.velocity16() == fon[1].ump.velocity16() && velocity16_to_7(static_cast<std::uint16_t>(fon[0].ump.velocity16())) == kFixedVelocity7);
}

void test_curve_change_next_note_only() {
  const KeybedCalibration t3 = KeybedCalibration::three_contact_73();
  Rig r(t3);
  r.press3(5, r.host0, 15 * kMs);  // Normal
  r.reader->set_velocity_curve(VelocityCurve::kLight);  // while key 5 sounds
  r.press3(6, r.host0 + 100 * kMs, 15 * kMs);
  r.release3(5, r.host0 + kSec, 5 * kMs);
  const auto on = r.of(true);
  CHECK(on.size() == 2);
  if (on.size() == 2) {
    const VelocityRange vr = t3.velocity_range();
    CHECK(std::llabs(static_cast<long long>(on[0].ump.velocity16()) - static_cast<long long>(chain_velocity16(vr, VelocityCurve::kNormal, 100, 30 * kMs))) < 40);
    CHECK(on[1].ump.velocity16() > on[0].ump.velocity16() + 1000);  // the next note: Light
  }
  CHECK(r.of(false).size() == 1 && !r.reader->key_sounding(5) && r.reader->key_sounding(6));  // the sounding note was released normally

  // The setter against the reader running on its own thread (an atomic store): no crash, every velocity is one of the three curves' values.
  Rig s(t3);
  std::atomic<bool> stop{false};
  std::thread toggler([&] {
    int i = 0;
    while (!stop.load()) s.reader->set_velocity_curve(static_cast<VelocityCurve>(i++ % 3));
  });
  std::uint64_t t = s.host0;
  for (int i = 0; i < 300; ++i) {
    s.press3(20, t, 10 * kMs);
    s.release3(20, t + 500 * kMs, 3 * kMs);
    t += kSec;
  }
  stop = true;
  toggler.join();
  const auto on2 = s.of(true);
  CHECK(on2.size() == 300);
  const VelocityRange vr = t3.velocity_range();
  bool valid = true;
  for (const StampedUmp& e : on2) {
    bool ok = false;
    for (VelocityCurve c : {VelocityCurve::kNormal, VelocityCurve::kLight, VelocityCurve::kHeavy})
      ok = ok || std::llabs(static_cast<long long>(e.ump.velocity16()) - static_cast<long long>(chain_velocity16(vr, c, 100, 20 * kMs))) < 40;
    valid = valid && ok;
  }
  CHECK(valid);
}

// ---- 3. release velocity ------------------------------------------------------------------------------------------

void test_release_velocity() {
  const KeybedCalibration t3 = KeybedCalibration::three_contact_73();
  const VelocityRange rr = t3.release_range();
  auto release_vel = [&](std::uint64_t d) {
    Rig r(t3);
    r.press3(7, r.host0, 10 * kMs);
    r.release3(7, r.host0 + kSec, d);
    const auto off = r.of(false);
    CHECK(off.size() == 1 && !r.reader->key_sounding(7));
    if (off.size() != 1) return 0u;
    // the Note Off is stamped with the release contact's own time, before the second contact's open event
    std::uint64_t open3 = 0;
    for (const StampedUmp& s : r.ev) {
      core::ContactEvent ce;
      if (core::decode_contact_event(s.ump, ce) && !ce.closed && ce.contact == 3) open3 = s.host_ns;
    }
    CHECK(off[0].host_ns == open3);
    return off[0].ump.velocity16();
  };
  const std::uint32_t fast = release_vel(4 * kMs), slow = release_vel(60 * kMs), mid = release_vel(20 * kMs);
  CHECK(fast > mid && mid > slow);
  CHECK(std::llabs(static_cast<long long>(fast) - static_cast<long long>(chain_velocity16(rr, VelocityCurve::kNormal, 100, 4 * kMs))) < 40);
  CHECK(std::llabs(static_cast<long long>(slow) - static_cast<long long>(chain_velocity16(rr, VelocityCurve::kNormal, 100, 60 * kMs))) < 40);
  CHECK(release_vel(kMs) == 0xFFFF && release_vel(500 * kMs) == kMinVelocity16);  // clamped
  // the release velocity is not shaped by the player's curve (applied once, to the attack only)
  Rig c(t3);
  c.reader->set_velocity_curve(VelocityCurve::kHeavy);
  c.press3(7, c.host0, 10 * kMs);
  c.release3(7, c.host0 + kSec, 20 * kMs);
  CHECK(c.of(false).size() == 1 && c.of(false)[0].ump.velocity16() == mid);

  // 2-contact keybed: no release pair, the documented default, counted
  Rig two(KeybedCalibration::two_contact_73());
  two.key(3, 0, true, two.host0);
  two.key(3, 1, true, two.host0 + 10 * kMs);
  two.key(3, 1, false, two.host0 + kSec);
  const auto off2 = two.of(false);
  CHECK(off2.size() == 1 && off2[0].ump.velocity16() == KeybedReaderConfig{}.release_velocity16);
  CHECK(two.reader->counters().release_velocity_defaults == 1 && two.reader->counters().release_timeouts == 0);

  // a lost contact: contact 3's open frame is lost (contact 2 opens while 3 is still believed closed): default, counted, and one recovered open
  Rig lost(t3);
  lost.press3(9, lost.host0, 10 * kMs);
  lost.key(9, 1, false, lost.host0 + kSec);
  const auto offl = lost.of(false);
  CHECK(offl.size() == 1 && offl[0].ump.velocity16() == KeybedReaderConfig{}.release_velocity16);
  CHECK(lost.reader->counters().release_velocity_defaults == 1 && lost.reader->counters().recovered_opens == 1);
  // a snapshot that shows the key up (link loss): default, counted
  Rig snap(t3);
  snap.press3(9, snap.host0, 10 * kMs);
  snap.key(9, 2, false, snap.host0 + kSec);  // the pair starts ...
  snap.reader->report_key_state(9, 0, snap.host0 + kSec + 2 * kMs);  // ... and the snapshot ends it
  snap.drain();
  CHECK(snap.of(false).size() == 1 && snap.of(false)[0].ump.velocity16() == KeybedReaderConfig{}.release_velocity16 && snap.reader->counters().release_velocity_defaults == 1);
  // a partial release past the slowest release time ends by itself at the slowest release velocity (poll), counted as a timeout
  Rig part(t3);
  part.press3(9, part.host0, 10 * kMs);
  part.key(9, 2, false, part.host0 + kSec);
  CHECK(part.reader->key_sounding(9) && part.of(false).empty());
  part.reader->poll(part.host0 + kSec + 100 * kMs);
  part.drain();
  CHECK(!part.reader->key_sounding(9) && part.of(false).size() == 1 && part.of(false)[0].ump.velocity16() == kMinVelocity16 && part.reader->counters().release_timeouts == 1);
  // a retransmitted close of the lower contact while the pair is measured does not end the measurement early
  Rig bounce(t3);
  bounce.press3(9, bounce.host0, 10 * kMs);
  bounce.key(9, 2, false, bounce.host0 + kSec);
  bounce.key(9, 1, true, bounce.host0 + kSec + 2 * kMs);  // the same close again (duplicate)
  CHECK(bounce.reader->key_sounding(9) && bounce.of(false).empty() && bounce.reader->counters().duplicate_closes == 1);
  bounce.key(9, 1, false, bounce.host0 + kSec + 20 * kMs);
  const auto offb = bounce.of(false);
  CHECK(offb.size() == 1 && std::llabs(static_cast<long long>(offb[0].ump.velocity16()) - static_cast<long long>(chain_velocity16(rr, VelocityCurve::kNormal, 100, 20 * kMs))) < 40);
  CHECK(bounce.reader->counters().release_restrikes == 0 && bounce.reader->counters().clock_clamps == 0);
  // a spec with the default (no release pair) stays valid for any release contact
  KeybedCalibrationSpec nopair;
  nopair.release_contact = 2;
  CHECK(KeybedCalibration::make(nopair) && !KeybedCalibration::make(nopair)->release_pair() && KeybedCalibration::three_contact_73().release_pair());
  // invalid release specs are refused
  KeybedCalibrationSpec bad;
  bad.release_pair = true;
  bad.rel_to = 2;  // not below the release contact
  CHECK(!KeybedCalibration::make(bad));
  bad = KeybedCalibrationSpec{};
  bad.vel_fast_ns = bad.vel_slow_ns;
  CHECK(!KeybedCalibration::make(bad));
  bad = KeybedCalibrationSpec{};
  bad.release_pair = true;
  bad.rel_fast_ns = 0;
  CHECK(!KeybedCalibration::make(bad));
}

// ---- 4. end to end through BlockRenderer and the sample engine ---------------------------------------------------

struct Instrument final : ports::SampleSource {
  std::vector<ports::SampleRegionDesc> regions;
  std::vector<ports::SamplePcm> pcm;
  std::vector<std::vector<float>> data;
  ports::InstrumentView instrument() const noexcept override { return {regions, pcm}; }
  std::uint32_t dc(float v, std::size_t frames) {
    data.emplace_back(frames, v);
    ports::SamplePcm p;
    p.sample_rate = 48000;
    p.channels = 1;
    p.frames = frames;
    p.left = data.back().data();
    p.right = nullptr;
    pcm.push_back(p);
    return static_cast<std::uint32_t>(pcm.size() - 1);
  }
  ports::SampleRegionDesc& region(std::uint32_t sample, int note) {
    ports::SampleRegionDesc d;
    d.sample_index = sample;
    d.key_lo = d.key_hi = d.key_center = static_cast<std::uint8_t>(note);
    regions.push_back(d);
    return regions.back();
  }
};

double mono_center_gain() { return std::cos(2047.0 * 1.57079632679489661923 / 4094.0) * 1.4125375446227544; }

struct Trace {
  std::vector<core::EmittedNoteEvent> ev;
  static void cb(void* ctx, const core::EmittedNoteEvent& e) noexcept { static_cast<Trace*>(ctx)->ev.push_back(e); }
};

// Renders the reader's events (host_ns relative to `base_ns`) through a BlockRenderer with a SampleEngine on channel 0.
std::vector<float> render(Instrument& ins, bool release_tracking, const std::vector<StampedUmp>& evs, std::uint64_t base_ns, std::size_t frames, Trace* trace = nullptr) {
  core::SampleEngine::Config cfg;
  cfg.max_voices = 16;
  cfg.release_velocity_triggers = release_tracking;
  core::SampleEngine e(cfg);
  e.set_instrument(ins);
  auto br = std::make_unique<core::BlockRenderer>();
  CHECK(br->bind_engine(e));
  br->set_channel_engine(0, static_cast<std::uint32_t>(core::EngineTypeId::kSample));
  if (trace) br->set_note_observer(&Trace::cb, trace);
  for (const StampedUmp& s : evs) {
    const double f = static_cast<double>(s.host_ns - base_ns) * core::kSampleRate / 1e9;
    br->submit({s.ump, static_cast<std::uint32_t>(std::llround(f))});
  }
  std::vector<float> out, l(core::kBlockFrames), r(core::kBlockFrames);
  for (std::size_t b = 0; b < (frames + core::kBlockFrames - 1) / core::kBlockFrames; ++b) {
    br->process_block(l.data(), r.data());
    out.insert(out.end(), l.begin(), l.end());
  }
  return out;
}

void test_end_to_end_velocity_once() {
  const double G = mono_center_gain();
  const KeybedCalibration t3 = KeybedCalibration::three_contact_73();
  const VelocityRange vr = t3.velocity_range();
  for (VelocityCurve c : {VelocityCurve::kNormal, VelocityCurve::kLight, VelocityCurve::kHeavy}) {
    Rig r(t3);
    r.reader->set_velocity_curve(c);
    const std::uint64_t t0 = r.host0 + 100 * kMs;
    r.press3(32, t0, 12 * kMs);  // key 32 -> note 60; travel time (contact 1 -> 3) 24 ms
    CHECK(r.of(true).size() == 1);
    Instrument ins;
    ins.region(ins.dc(0.5f, 40000), 60);
    Trace tr;
    const auto out = render(ins, false, r.ev, t0, 4000, &tr);
    const std::uint16_t once = chain_velocity16(vr, c, kFixedVelocity7, 24 * kMs);
    // the renderer sees exactly what the reader sent: no stage after the reader changes the velocity
    CHECK(tr.ev.size() == 1 && tr.ev[0].on && tr.ev[0].note == 60 && tr.ev[0].velocity16 == r.of(true)[0].ump.velocity16());
    CHECK(std::llabs(static_cast<long long>(tr.ev[0].velocity16) - static_cast<long long>(once)) < 40);
    // applying the curve a second time (to the velocity as a position) would land elsewhere: that is what "once" rules out
    const double pos_again = (static_cast<double>(once) - kMinVelocity16) / (kMaxVelocity16 - kMinVelocity16);
    const std::uint16_t twice = static_cast<std::uint16_t>(kMinVelocity16 + (kMaxVelocity16 - kMinVelocity16) * apply_velocity_curve(c, pos_again));
    if (c != VelocityCurve::kNormal) CHECK(std::llabs(static_cast<long long>(tr.ev[0].velocity16) - static_cast<long long>(twice)) > 1000);
    // the voice: the sample engine's gain is (7-bit velocity / 127)^2 and applies no curve of its own
    const double v7 = velocity16_to_7(static_cast<std::uint16_t>(tr.ev[0].velocity16)) / 127.0;
    const double level = out[3000] / G;
    CHECK(std::fabs(level - 0.5 * v7 * v7) < 1e-4);
  }
}

// A sample instrument with an attack region and two velocity layers of release-trigger samples on note 60: a fast release plays the louder layer / gain.
void test_release_render() {
  const double G = mono_center_gain();
  const KeybedCalibration t3 = KeybedCalibration::three_contact_73();
  auto run = [&](std::uint64_t release_dt, bool tracking, std::uint32_t& note_off_v16, std::vector<float>* levels_out = nullptr) {
    Rig r(t3);
    const std::uint64_t t0 = r.host0 + 100 * kMs;
    r.press3(32, t0, 10 * kMs);
    r.release3(32, t0 + 400 * kMs, release_dt);
    const auto off = r.of(false);
    CHECK(off.size() == 1);
    note_off_v16 = off.empty() ? 0 : off[0].ump.velocity16();
    Instrument ins;
    ins.region(ins.dc(0.1f, 60000), 60);                                                     // the attack
    auto& soft = ins.region(ins.dc(0.2f, 30000), 60); soft.trigger = ports::SampleTrigger::kRelease; soft.vel_lo = 1; soft.vel_hi = 63;
    auto& loud = ins.region(ins.dc(0.4f, 30000), 60); loud.trigger = ports::SampleTrigger::kRelease; loud.vel_lo = 64; loud.vel_hi = 127;
    const auto out = render(ins, tracking, r.ev, t0, 48000 * 9 / 10);
    if (levels_out) *levels_out = out;
    // the release voice's level (DC, constant) measured 120 ms after the Note Off; the attack voice has decayed to nothing by then at its 6 ms+ release...
    // ... but the attack voice's own tail is identical in every run, so take the difference with the same render without release triggers (a silent release layer)
    Instrument plain;
    plain.region(plain.dc(0.1f, 60000), 60);
    const auto base = render(plain, tracking, r.ev, t0, 48000 * 9 / 10);
    const std::size_t at = static_cast<std::size_t>((400 * kMs + release_dt * 2 + 100 * kMs) * core::kSampleRate / kSec);
    return (out[at] - base[at]) / G;
  };
  std::uint32_t fv = 0, sv = 0;
  const double fast = run(4 * kMs, true, fv);
  const double slow = run(70 * kMs, true, sv);
  CHECK(fv > sv && velocity16_to_7(static_cast<std::uint16_t>(fv)) >= 64 && velocity16_to_7(static_cast<std::uint16_t>(sv)) <= 63);
  // fast release: the loud layer at the fast release velocity; slow release: the soft layer at the slow one (gain = velocity^2)
  const double fv7 = velocity16_to_7(static_cast<std::uint16_t>(fv)) / 127.0, sv7 = velocity16_to_7(static_cast<std::uint16_t>(sv)) / 127.0;
  CHECK(std::fabs(fast - 0.4 * fv7 * fv7) < 2e-3);
  CHECK(std::fabs(slow - 0.2 * sv7 * sv7) < 2e-3);
  CHECK(fast > slow * 2.0);
  // same layer, different release velocity: the gain follows it (a single-layer release region)
  auto single = [&](std::uint64_t release_dt) {
    Rig r(t3);
    const std::uint64_t t0 = r.host0 + 100 * kMs;
    r.press3(32, t0, 10 * kMs);
    r.release3(32, t0 + 400 * kMs, release_dt);
    Instrument ins;
    ins.region(ins.dc(0.1f, 60000), 60);
    auto& rel = ins.region(ins.dc(0.3f, 30000), 60);
    rel.trigger = ports::SampleTrigger::kRelease;
    const auto out = render(ins, true, r.ev, t0, 48000 * 9 / 10);
    Instrument plain;
    plain.region(plain.dc(0.1f, 60000), 60);
    const auto base = render(plain, true, r.ev, t0, 48000 * 9 / 10);
    const std::size_t at = static_cast<std::size_t>((400 * kMs + release_dt * 2 + 100 * kMs) * core::kSampleRate / kSec);
    return (out[at] - base[at]) / G;
  };
  CHECK(single(4 * kMs) > single(70 * kMs) * 1.5);
  // without the option the sample engine keeps the note-on velocity (sfizz behaviour): both releases play the same
  std::uint32_t a = 0, b = 0;
  const double off_fast = run(4 * kMs, false, a), off_slow = run(70 * kMs, false, b);
  CHECK(a != b && std::fabs(off_fast - off_slow) < 1e-4);
}

}  // namespace

int main() {
  test_curves();
  test_reader_chain();
  test_curve_change_next_note_only();
  test_release_velocity();
  test_end_to_end_velocity_once();
  test_release_render();
  if (g_fail) {
    std::printf("velocity_curve_test: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("velocity_curve_test: all passed\n");
  return 0;
}
