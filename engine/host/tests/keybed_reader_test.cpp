// Keybed reader (Story 3.5): drift-tracked device time, contact_k + Note On/Off, calibration tables, lost-open recovery,
// counters, and an offline organ render of a recorded slow press. Bare main, CHECK style as the other host tests.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/contact_events.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/params_gen.hpp"
#include "velora/host/keybed_calibration.hpp"
#include "velora/host/keybed_reader.hpp"

using namespace velora;
using namespace velora::host;

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

// A simulated keybed MCU: a tick counter running at 1 MHz * (1 + ppm), with its own frame numbering.
struct SimMcu {
  double ppm = 0;
  std::uint32_t tick0 = 123456789u;
  std::uint64_t t0_ns = 0;  // true time of tick0
  mcu::FrameWriter writer;
  std::uint32_t tick_at(std::uint64_t true_ns) const {
    const double us = static_cast<double>(true_ns - t0_ns) * (1.0 + ppm * 1e-6) / 1000.0;
    return tick0 + static_cast<std::uint32_t>(static_cast<std::uint64_t>(us));  // wraps modulo 2^32
  }
  std::vector<std::uint8_t> frame(const mcu::Message& m, std::uint64_t true_ns) { return writer.write(m, tick_at(true_ns)); }
  std::vector<std::uint8_t> key(std::uint8_t k, std::uint8_t contact, bool closed, std::uint64_t true_ns) {
    return frame(mcu::KeyContact{k, contact, closed}, true_ns);
  }
};

struct Out {
  std::vector<StampedUmp> ev;
};
void drain(HostEventQueue& q, Out& o) {
  StampedUmp s;
  while (q.pop(s)) o.ev.push_back(s);
}

enum class Kind { kContactClosed, kContactOpen, kNoteOn, kNoteOff, kOther };
struct Dec {
  Kind kind = Kind::kOther;
  std::uint32_t note = 0, k = 0, vel = 0;
  std::uint64_t ns = 0;
};
Dec decode(const StampedUmp& s) {
  Dec d;
  d.ns = s.host_ns;
  core::ContactEvent ce;
  if (core::decode_contact_event(s.ump, ce)) {
    d.kind = ce.closed ? Kind::kContactClosed : Kind::kContactOpen;
    d.note = ce.note;
    d.k = ce.contact;
  } else if (s.ump.is_note_on()) {
    d.kind = Kind::kNoteOn;
    d.note = s.ump.note();
    d.vel = s.ump.velocity16();
  } else if (s.ump.is_note_off()) {
    d.kind = Kind::kNoteOff;
    d.note = s.ump.note();
  }
  return d;
}
std::vector<Dec> decode_all(const Out& o) {
  std::vector<Dec> v;
  for (const StampedUmp& s : o.ev) v.push_back(decode(s));
  return v;
}
int count_kind(const std::vector<Dec>& v, Kind k) {
  int n = 0;
  for (const Dec& d : v) n += d.kind == k;
  return n;
}

struct Rig {
  HostEventQueue q;
  SimMcu mcu;
  std::unique_ptr<KeybedReader> reader;
  Out out;
  std::uint64_t host0 = 10 * kSec;
  explicit Rig(KeybedCalibration table, double ppm = 0) {
    mcu.ppm = ppm;
    mcu.t0_ns = host0;
    reader = std::make_unique<KeybedReader>(q, std::move(table));
  }
  // The frame reaches the reader 300 us after the contact event.
  void feed(const std::vector<std::uint8_t>& wire, std::uint64_t true_ns) {
    reader->on_bytes(wire.data(), wire.size(), true_ns + 300'000);
    drain(q, out);
  }
  void key(std::uint8_t k, std::uint8_t c, bool closed, std::uint64_t true_ns) { feed(mcu.key(k, c, closed, true_ns), true_ns); }
};

// ---- the sections of the matrix ------------------------------------------------------------------------------------

// A full press and release of key `key` with the given table; returns the decoded events.
std::vector<Dec> press_release(const KeybedCalibration& table, std::uint8_t key, std::uint64_t t, std::uint64_t dt_close,
                               std::uint64_t dt_open, std::uint8_t lowest_contact_count) {
  Rig r(table);
  const std::uint64_t t0 = r.host0 + t;
  for (std::uint8_t c = 0; c < lowest_contact_count; ++c) r.key(key, c, true, t0 + c * dt_close);
  const std::uint64_t t1 = t0 + 2 * kSec;
  for (int c = lowest_contact_count - 1; c >= 0; --c) r.key(key, static_cast<std::uint8_t>(c), false, t1 + (lowest_contact_count - 1 - c) * dt_open);
  return decode_all(r.out);
}

void test_three_contact() {
  const KeybedCalibration t3 = KeybedCalibration::three_contact_73();
  const auto v = press_release(t3, 10, kSec, 20 * kMs, 15 * kMs, 3);  // key 10 -> note 38
  // contact_k 1,2,3 closed, Note On at contact 3, then open 3 (Note Off), open 2, open 1
  CHECK(v.size() == 3 + 1 + 3 + 1);
  if (v.size() == 8) {
    CHECK(v[0].kind == Kind::kContactClosed && v[0].k == 1 && v[0].note == 38);
    CHECK(v[1].kind == Kind::kContactClosed && v[1].k == 2);
    CHECK(v[2].kind == Kind::kContactClosed && v[2].k == 3);
    CHECK(v[3].kind == Kind::kNoteOn && v[3].note == 38 && v[3].ns == v[2].ns);
    // travel time contact 1 -> 3 = 40 ms: Story 3.6 chain (the table's 2..100 ms log axis, Normal curve), no longer the linear 3.5 mapper
    const VelocityRange vr = t3.velocity_range();
    CHECK(std::llabs(static_cast<long long>(v[3].vel) - static_cast<long long>(chain_velocity16(vr, VelocityCurve::kNormal, kFixedVelocity7, 40 * kMs))) < 40);
    CHECK(v[3].vel < 65535 && v[3].vel > 512);
    CHECK(v[4].kind == Kind::kContactOpen && v[4].k == 3);
    CHECK(v[5].kind == Kind::kNoteOff && v[5].note == 38);
    CHECK(v[6].kind == Kind::kContactOpen && v[6].k == 2);
    CHECK(v[7].kind == Kind::kContactOpen && v[7].k == 1);
    // times: 20 ms apart as sent (within the bootstrap clock's exactness)
    CHECK(std::llabs(static_cast<long long>(v[1].ns - v[0].ns) - 20'000'000) < 5000);
    CHECK(std::llabs(static_cast<long long>(v[2].ns - v[1].ns) - 20'000'000) < 5000);
    CHECK(v[4].ns > v[3].ns);
  }
  CHECK(count_kind(v, Kind::kNoteOff) == 1 && count_kind(v, Kind::kNoteOn) == 1);
}

void test_two_contact_and_table_switch() {
  const KeybedCalibration t2 = KeybedCalibration::two_contact_73();
  const auto v = press_release(t2, 0, kSec, 20 * kMs, 15 * kMs, 2);  // key 0 -> note 28
  CHECK(v.size() == 2 + 1 + 2 + 1);
  if (v.size() == 6) {
    CHECK(v[0].kind == Kind::kContactClosed && v[0].k == 1 && v[0].note == 28);
    CHECK(v[1].kind == Kind::kContactClosed && v[1].k == 2);
    CHECK(v[2].kind == Kind::kNoteOn && v[2].ns == v[1].ns);
    CHECK(v[3].kind == Kind::kContactOpen && v[3].k == 2);
    CHECK(v[4].kind == Kind::kNoteOff);
    CHECK(v[5].kind == Kind::kContactOpen && v[5].k == 1);
  }
  // Switching the table of a live reader: the same wire frames now behave as a 3-contact keybed. No code change.
  Rig r(t2);
  r.key(5, 0, true, r.host0 + 100 * kMs);
  r.key(5, 1, true, r.host0 + 120 * kMs);
  CHECK(r.reader->key_sounding(5));
  r.key(5, 2, true, r.host0 + 130 * kMs);  // contact 3 is not in a 2-contact table
  CHECK(r.reader->counters().bad_contact == 1);
  r.reader->set_calibration(KeybedCalibration::three_contact_73(), r.host0 + 200 * kMs);
  drain(r.q, r.out);
  CHECK(!r.reader->key_sounding(5));
  CHECK(count_kind(decode_all(r.out), Kind::kNoteOff) == 1);  // released by the switch, nothing stuck
  r.out.ev.clear();
  r.key(5, 0, true, r.host0 + 300 * kMs);
  r.key(5, 1, true, r.host0 + 320 * kMs);
  CHECK(!r.reader->key_sounding(5));  // contact 2 no longer sounds
  r.key(5, 2, true, r.host0 + 340 * kMs);
  CHECK(r.reader->key_sounding(5));
  CHECK(count_kind(decode_all(r.out), Kind::kNoteOn) == 1);

  // Calibration as data: a custom spec (key map, release at contact 2 of 3) and validation.
  KeybedCalibrationSpec s;
  s.contacts = 3;
  s.vel_from = 0;
  s.vel_to = 1;
  s.release_contact = 1;
  s.key_notes = {60, 62, 64};
  auto custom = KeybedCalibration::make(s);
  CHECK(custom && custom->note_of(1) == 62 && custom->note_of(3) == -1);
  Rig r2(*custom);
  r2.key(1, 0, true, r2.host0 + 10 * kMs);
  r2.key(1, 1, true, r2.host0 + 30 * kMs);
  r2.key(1, 2, true, r2.host0 + 40 * kMs);
  r2.key(1, 2, false, r2.host0 + 500 * kMs);  // contact 3 opens: not the release contact
  CHECK(r2.reader->key_sounding(1));
  r2.key(1, 1, false, r2.host0 + 510 * kMs);  // contact 2 opens: Note Off
  CHECK(!r2.reader->key_sounding(1));
  const auto v2 = decode_all(r2.out);
  CHECK(count_kind(v2, Kind::kNoteOn) == 1 && count_kind(v2, Kind::kNoteOff) == 1);
  for (const Dec& d : v2) CHECK(d.note == 62);
  KeybedCalibrationSpec bad = s;
  bad.contacts = 4;
  CHECK(!KeybedCalibration::make(bad));
  bad = s;
  bad.vel_from = 2;
  CHECK(!KeybedCalibration::make(bad));
  bad = s;
  bad.key_notes = {200};
  CHECK(!KeybedCalibration::make(bad));
  bad = s;
  bad.key_notes = {60, 62, 60};  // two keys on one note
  CHECK(!KeybedCalibration::make(bad));
  bad = KeybedCalibrationSpec{};  // 3 contacts, velocity 1 -> 3, release at contact 2: the note would end before it began
  bad.release_contact = 1;
  CHECK(!KeybedCalibration::make(bad));
  bad = KeybedCalibrationSpec{};
  bad.key_count = 0;
  CHECK(!KeybedCalibration::make(bad));
  bad = KeybedCalibrationSpec{};
  bad.first_note = 100;  // 73 keys from note 100 run past 127
  CHECK(!KeybedCalibration::make(bad));
}

void test_lost_open_and_key_state() {
  const KeybedCalibration t3 = KeybedCalibration::three_contact_73();
  {  // the opens of a note were lost; the next press of the same key shows it
    Rig r(t3);
    const std::uint64_t t = r.host0;
    r.key(20, 0, true, t + 10 * kMs);
    r.key(20, 1, true, t + 20 * kMs);
    r.key(20, 2, true, t + 30 * kMs);
    CHECK(r.reader->key_sounding(20));
    // (opens 3, 2, 1 lost)
    r.out.ev.clear();
    r.key(20, 0, true, t + 2 * kSec);
    const auto v = decode_all(r.out);
    CHECK(count_kind(v, Kind::kNoteOff) == 1);
    CHECK(!r.reader->key_sounding(20));
    CHECK(r.reader->key_mask(20) == 1);
    CHECK(count_kind(v, Kind::kContactOpen) == 3);
    CHECK(count_kind(v, Kind::kContactClosed) == 1);
    CHECK(r.reader->counters().recovered_opens == 3);
    // order: the stale state is released before the new closure
    CHECK(!v.empty() && v.back().kind == Kind::kContactClosed && v.back().k == 1);
  }
  {  // an open of a lower contact implies the higher ones opened
    Rig r(t3);
    const std::uint64_t t = r.host0;
    r.key(7, 0, true, t + 10 * kMs);
    r.key(7, 1, true, t + 20 * kMs);
    r.key(7, 2, true, t + 30 * kMs);
    r.out.ev.clear();
    r.key(7, 0, false, t + kSec);  // opens of 3 and 2 lost
    CHECK(!r.reader->key_sounding(7) && r.reader->key_mask(7) == 0);
    const auto v = decode_all(r.out);
    CHECK(count_kind(v, Kind::kNoteOff) == 1 && count_kind(v, Kind::kContactOpen) == 3);
  }
  {  // periodic key-state check
    Rig r(t3);
    const std::uint64_t t = r.host0;
    r.key(33, 0, true, t + 10 * kMs);
    r.key(33, 1, true, t + 20 * kMs);
    r.key(33, 2, true, t + 30 * kMs);
    r.key(34, 0, true, t + 30 * kMs);
    r.out.ev.clear();
    r.reader->report_key_state(34, 1, t + kSec);  // unchanged: nothing
    CHECK(r.q.size() == 0);
    r.reader->report_key_state(33, 0, t + kSec);  // all open: released
    drain(r.q, r.out);
    CHECK(!r.reader->key_sounding(33) && r.reader->key_mask(33) == 0);
    const auto v = decode_all(r.out);
    CHECK(count_kind(v, Kind::kNoteOff) == 1 && count_kind(v, Kind::kContactOpen) == 3);
    for (const Dec& d : v) CHECK(d.ns >= t + kSec - kMs);
    CHECK(r.reader->key_mask(34) == 1);
    r.reader->report_key_state(250, 0, t + kSec);  // outside the table
    CHECK(r.reader->counters().unknown_key == 1);
    r.out.ev.clear();
    CHECK(r.reader->release_all(t + 2 * kSec) == 1);  // key 34 was still down
    drain(r.q, r.out);
    CHECK(r.reader->key_mask(34) == 0 && !r.reader->recovery_pending());
  }
}

void test_bad_frames_and_unknown_key() {
  const KeybedCalibration t3 = KeybedCalibration::three_contact_73();
  Rig r(t3);
  const std::uint64_t t = r.host0;
  r.key(3, 0, true, t + 10 * kMs);
  r.key(3, 1, true, t + 20 * kMs);
  // CRC error on the contact-3 frame: counted, processing continues
  auto bad = r.mcu.key(3, 2, true, t + 30 * kMs);
  bad[bad.size() - 4] ^= 0x55;
  r.feed(bad, t + 30 * kMs);
  CHECK(r.reader->counters().crc_errors == 1);
  CHECK(r.reader->recovery_pending());
  CHECK(!r.reader->key_sounding(3));
  // a frame the MCU sent but the link lost: seq gap
  (void)r.mcu.key(3, 1, false, t + 40 * kMs);
  r.key(3, 0, false, t + 50 * kMs);
  CHECK(r.reader->counters().seq_gaps >= 1);
  // the open of contact 1 (and 2) is implied: nothing is stuck
  CHECK(r.reader->key_mask(3) == 0 && !r.reader->key_sounding(3));
  // unknown key and bad contact
  const std::size_t before = r.out.ev.size();
  r.key(200, 0, true, t + 60 * kMs);
  CHECK(r.reader->counters().unknown_key == 1 && r.out.ev.size() == before);
  r.key(73, 0, true, t + 61 * kMs);  // one past the last key
  CHECK(r.reader->counters().unknown_key == 2 && r.out.ev.size() == before);
  r.key(72, 0, true, t + 62 * kMs);  // the last key is fine
  CHECK(r.out.ev.size() > before && decode(r.out.ev.back()).note == 28 + 72);
  // framing garbage and a fresh press still work
  const std::uint8_t junk[] = {1, 2, 3, 0xA5, 0xFF, 0, 0};
  r.reader->on_bytes(junk, sizeof junk, t + 70 * kMs);
  r.key(9, 0, true, t + 80 * kMs);
  CHECK(r.reader->key_mask(9) == 1);
  r.reader->release_all(t + 90 * kMs);
  CHECK(!r.reader->recovery_pending());
}

// TIME_SYNC plumbing: requests go out through tx, replies are matched by token, losses are counted.
void test_time_sync_and_loss() {
  const KeybedCalibration t3 = KeybedCalibration::three_contact_73();
  Rig r(t3, 150.0);
  std::vector<std::uint8_t> sent;
  r.reader->set_tx([&](const std::uint8_t* d, std::size_t n) { sent.assign(d, d + n); });
  mcu::StreamDecoder sim_rx;
  auto serve = [&](std::uint64_t now) {  // the MCU answers the pending request, 400 us round trip
    auto frames = sim_rx.feed(sent);
    sent.clear();
    for (const mcu::Frame& f : frames) {
      auto m = mcu::parse_message(f);
      if (m && std::holds_alternative<mcu::TimeSync>(*m)) {
        const auto ts = std::get<mcu::TimeSync>(*m);
        const auto reply = r.mcu.frame(mcu::TimeSync{true, ts.token, r.mcu.tick_at(now + 200'000)}, now + 200'000);
        r.reader->on_bytes(reply.data(), reply.size(), now + 400'000);
      }
    }
  };
  std::uint64_t now = r.host0;
  r.reader->poll(now);
  CHECK(!sent.empty() && r.reader->counters().sync_requests == 1);
  serve(now);
  CHECK(r.reader->counters().sync_replies == 1 && r.reader->clock().samples() == 1);
  // stray reply (wrong token) is counted and ignored
  const auto stray = r.mcu.frame(mcu::TimeSync{true, 9999, 5}, now);
  r.reader->on_bytes(stray.data(), stray.size(), now + kSec);
  CHECK(r.reader->counters().sync_stray == 1);
  // a request that is never answered: missed after the timeout, last clock is kept
  now += 100 * kMs;
  r.reader->poll(now);
  CHECK(r.reader->counters().sync_requests == 2);
  sent.clear();  // lost
  DriftClock probe = r.reader->clock();
  const std::uint64_t kept = probe.to_host_ns(probe.unwrap(r.mcu.tick_at(now)));
  now += 600 * kMs;
  r.reader->poll(now);  // timeout counted, the next request goes out
  CHECK(r.reader->counters().missed_syncs == 1);
  CHECK(r.reader->clock().samples() == 1);
  CHECK(std::llabs(static_cast<long long>(kept) - static_cast<long long>(now - 600 * kMs)) < 200'000);
  // a long silence: events still convert (offset and drift kept)
  now += 3600 * kSec;  // longer than the 32-bit tick can be unwrapped: the clock restarts from the next frame
  r.out.ev.clear();
  r.key(1, 0, true, now);
  CHECK(r.out.ev.size() == 1);
  if (r.out.ev.size() == 1) CHECK(std::llabs(static_cast<long long>(r.out.ev[0].host_ns) - static_cast<long long>(now)) < 400'000);
  r.key(1, 1, true, now + 25 * kMs);
  CHECK(r.out.ev.size() == 2 && std::llabs(static_cast<long long>(r.out.ev[1].host_ns - r.out.ev[0].host_ns) - 25'000'000) < 5000);
  CHECK(r.reader->counters().missed_syncs >= 1);
}

// 12 simulated hours, MCU clock +-200 ppm, a sync every second with a jittery round trip.
void run_drift(double ppm) {
  const KeybedCalibration t3 = KeybedCalibration::three_contact_73();
  Rig r(t3, ppm);
  std::vector<std::uint8_t> sent;
  r.reader->set_tx([&](const std::uint8_t* d, std::size_t n) { sent.assign(d, d + n); });
  mcu::StreamDecoder sim_rx;
  std::uint64_t rng = 0x12345678abcdefULL;
  auto jitter = [&](std::uint64_t span) {  // deterministic LCG, 0..span-1
    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (rng >> 33) % span;
  };
  std::vector<std::uint64_t> expect_contact_ns;
  const std::uint64_t total = 12ull * 3600ull * kSec;
  std::uint64_t step = 0;
  for (std::uint64_t t = 0; t < total; t += 100 * kMs, ++step) {
    const std::uint64_t now = r.host0 + t;
    r.reader->poll(now);
    if (!sent.empty()) {
      // request travels 150-190 us, the MCU stamps its reply, the reply travels 150-190 us
      const std::uint64_t up = 150'000 + jitter(40'000), down = 150'000 + jitter(40'000);
      auto frames = sim_rx.feed(sent);
      sent.clear();
      for (const mcu::Frame& f : frames) {
        auto m = mcu::parse_message(f);
        if (!m || !std::holds_alternative<mcu::TimeSync>(*m)) continue;
        const std::uint64_t at_mcu = now + up;
        const auto reply = r.mcu.frame(mcu::TimeSync{true, std::get<mcu::TimeSync>(*m).token, r.mcu.tick_at(at_mcu)}, at_mcu);
        r.reader->on_bytes(reply.data(), reply.size(), at_mcu + down);
      }
    }
    // a key press every 7 s (after a 60 s warm-up): contact 1, 2, 3 within a few ms
    if (t > 60 * kSec && step % 70 == 3) {
      const std::uint8_t key = static_cast<std::uint8_t>((step / 70) % 73);
      for (std::uint8_t c = 0; c < 3; ++c) {
        const std::uint64_t te = now + 5 * kMs + c * (3 * kMs + jitter(2 * kMs));
        const auto w = r.mcu.key(key, c, true, te);
        r.reader->on_bytes(w.data(), w.size(), te + 100'000 + jitter(300'000));
        expect_contact_ns.push_back(te);
      }
      for (int c = 2; c >= 0; --c) {
        const std::uint64_t te = now + 60 * kMs + (2 - c) * (4 * kMs + jitter(3 * kMs));
        const auto w = r.mcu.key(key, static_cast<std::uint8_t>(c), false, te);
        r.reader->on_bytes(w.data(), w.size(), te + 100'000 + jitter(300'000));
        expect_contact_ns.push_back(te);
      }
    }
    drain(r.q, r.out);
  }
  // compare every contact_k event to the true time of the contact change; every Note On / Off is stamped like its contact
  std::vector<std::uint64_t> got;
  std::uint64_t prev = 0;
  bool ordered = true;
  for (const StampedUmp& s : r.out.ev) {
    if (s.host_ns < prev) ordered = false;
    prev = s.host_ns;
    if (decode(s).kind == Kind::kContactClosed || decode(s).kind == Kind::kContactOpen) got.push_back(s.host_ns);
  }
  CHECK(ordered);
  CHECK(got.size() == expect_contact_ns.size() && !got.empty());
  std::uint64_t worst = 0;
  for (std::size_t i = 0; i < got.size() && i < expect_contact_ns.size(); ++i) {
    const std::uint64_t d = got[i] > expect_contact_ns[i] ? got[i] - expect_contact_ns[i] : expect_contact_ns[i] - got[i];
    worst = std::max(worst, d);
  }
  std::printf("drift %+.0f ppm: %zu events, worst error %.1f us, fitted drift %+.1f ppm, clamps %llu, missed %llu\n", ppm, got.size(),
              static_cast<double>(worst) / 1000.0, r.reader->clock().drift_ppm(),
              static_cast<unsigned long long>(r.reader->counters().clock_clamps),
              static_cast<unsigned long long>(r.reader->counters().missed_syncs));
  CHECK(worst <= 20'000);
  CHECK(std::fabs(r.reader->clock().drift_ppm() - ppm) < 5.0);
  CHECK(r.reader->counters().clock_clamps == 0);
  CHECK(r.reader->counters().missed_syncs == 0 && r.reader->counters().queue_full == 0);
  CHECK(r.reader->counters().crc_errors == 0 && r.reader->counters().seq_gaps == 0);
}

void test_drift() {
  run_drift(+200.0);
  run_drift(-200.0);
}

// The 32-bit MCU tick wraps (71 minutes at 1 MHz): conversion stays continuous across it.
void test_tick_wrap() {
  Rig r(KeybedCalibration::three_contact_73());
  r.mcu.tick0 = 0xFFFFFF00u - 500'000u;  // wraps half a second in
  const std::uint64_t t = r.host0;
  r.key(4, 0, true, t + 100 * kMs);
  r.key(4, 1, true, t + 400 * kMs);
  r.key(4, 2, true, t + 700 * kMs);  // after the wrap
  const auto v = decode_all(r.out);
  CHECK(v.size() >= 4);
  if (v.size() >= 4) {
    CHECK(std::llabs(static_cast<long long>(v[2].ns - v[0].ns) - 600'000'000) < 5000);
    CHECK(v[0].ns < v[1].ns && v[1].ns < v[2].ns);
  }
}

// MCU restart (HELLO again): ticks start over, held keys are released.
void test_hello_restart() {
  Rig r(KeybedCalibration::three_contact_73());
  r.feed(r.mcu.frame(mcu::Hello{0x0100, mcu::kProtocolVersion, 1, 3}, r.host0), r.host0);
  CHECK(r.reader->hello() && r.reader->hello()->keybed_contacts == 3 && r.reader->counters().contact_mismatch == 0);
  r.key(2, 0, true, r.host0 + 10 * kMs);
  r.key(2, 1, true, r.host0 + 20 * kMs);
  r.key(2, 2, true, r.host0 + 30 * kMs);
  CHECK(r.reader->key_sounding(2));
  r.mcu.tick0 = 77;  // restarted MCU, ticks from near zero
  r.mcu.t0_ns = r.host0 + kSec;
  r.feed(r.mcu.frame(mcu::Hello{0x0100, mcu::kProtocolVersion, 1, 2}, r.host0 + kSec), r.host0 + kSec);
  CHECK(r.reader->counters().mcu_resets == 1 && r.reader->counters().contact_mismatch == 1);
  CHECK(!r.reader->key_sounding(2));
  r.key(2, 0, true, r.host0 + kSec + 50 * kMs);
  CHECK(r.reader->key_mask(2) == 1);
  const auto v = decode_all(r.out);
  for (std::size_t i = 1; i < v.size(); ++i) CHECK(v[i].ns >= v[i - 1].ns);  // order survives the restart
  // a HELLO from the panel MCU is not ours
  r.feed(r.mcu.frame(mcu::Hello{0x0100, mcu::kProtocolVersion, 2, 0}, r.host0 + 2 * kSec), r.host0 + 2 * kSec);
  CHECK(r.reader->counters().mcu_resets == 1);
}

// ---- organ render: a recorded slow press brings the busbar groups in at each contact time ----------------------------

std::string one_drawbar(int d) {
  std::string s(9, '0');
  s[static_cast<std::size_t>(d)] = '8';
  return s;
}

std::vector<float> render_with(const std::vector<StampedUmp>& evs, std::uint64_t t_base_ns, std::uint32_t contacts, const std::string& drawbars,
                               std::size_t frames, std::uint32_t first_frame) {
  auto organ = std::make_unique<core::OrganEngine>();
  auto br = std::make_unique<core::BlockRenderer>(8);
  br->bind_engine(*organ);
  br->set_channel_engine(0, static_cast<std::uint32_t>(core::EngineTypeId::kOrgan));
  organ->set_contact_count(contacts);
  for (int d = 0; d < 9; ++d) organ->set_part_param(0, core::params::kDrawbar16 + d, static_cast<float>(drawbars[static_cast<std::size_t>(d)] - '0'));
  organ->set_part_param(0, core::params::kKeyClick, 0.0f);
  organ->set_part_param(0, core::params::kLeakage, 0.0f);
  organ->set_part_param(0, core::params::kPercussionOn, 0.0f);
  std::vector<float> l(64), r(64);
  for (int i = 0; i < 12; ++i) br->process_block(l.data(), r.data());  // drawbar ramps settled, wheels running
  for (const StampedUmp& e : evs) {
    const double f = static_cast<double>(e.host_ns - t_base_ns) * core::kSampleRate / 1e9;
    br->submit({e.ump, first_frame + static_cast<std::uint32_t>(std::llround(f))});
  }
  std::vector<float> out;
  for (std::size_t b = 0; b < (frames + 63) / 64; ++b) {
    br->process_block(l.data(), r.data());
    out.insert(out.end(), l.begin(), l.end());
  }
  return out;
}

long first_nonzero(const std::vector<float>& x) {
  for (std::size_t i = 0; i < x.size(); ++i)
    if (x[i] != 0.0f) return static_cast<long>(i);
  return -1;
}

void test_organ_render() {
  const KeybedCalibration t3 = KeybedCalibration::three_contact_73();
  Rig r(t3, 50.0);
  // a recorded slow press of key 32 (note 60): contacts 1, 2, 3 close 120 ms and 180 ms apart, held, then released
  const std::uint64_t t0 = r.host0 + 3 * kSec;
  const std::uint64_t ct[3] = {t0, t0 + 120 * kMs, t0 + 300 * kMs};
  for (std::uint8_t c = 0; c < 3; ++c) r.key(32, c, true, ct[c]);
  const auto v = decode_all(r.out);
  CHECK(v.size() == 4 && v[3].kind == Kind::kNoteOn && v[3].vel < 2000);  // a slow press: a soft Note On

  constexpr std::uint32_t kFirst = 1000;
  const std::size_t frames = 24000;  // 0.5 s
  std::vector<StampedUmp> evs = r.out.ev;
  long onset[3] = {-1, -1, -1};
  for (int g = 0; g < 3; ++g) {
    // one drawbar of group g only: the group's own onset is the first non-zero sample
    int drawbar = -1;
    for (int d = 0; d < 9; ++d)
      if (core::OrganEngine::contact_group(3, static_cast<std::uint32_t>(d)) == static_cast<std::uint32_t>(g)) {
        drawbar = d;
        break;
      }
    const auto x = render_with(evs, t0 + 300'000, 3, one_drawbar(drawbar), frames, kFirst);
    onset[g] = first_nonzero(x);
    const long expected = static_cast<long>(kFirst) + std::lround(static_cast<double>(ct[g] - t0) * core::kSampleRate / 1e9);
    // the organ's own latency from event frame to first non-zero sample (gate attack, wheel phase) is not ours: the reference is the
    // same events at the contacts' exact times (the reader's clock here is the bootstrap one: it runs 300 us behind, the link latency)
    std::vector<StampedUmp> exact = evs;
    if (exact.size() < 4) break;
    for (std::size_t i = 0; i < 3; ++i) exact[i].host_ns = ct[i] + 300'000;
    exact[3].host_ns = ct[2] + 300'000;
    const long ref = first_nonzero(render_with(exact, t0 + 300'000, 3, one_drawbar(drawbar), frames, kFirst));
    CHECK(onset[g] >= 0 && ref >= expected && std::labs(onset[g] - ref) <= 1);
  }
  CHECK(onset[0] >= 0 && onset[0] < onset[1] && onset[1] < onset[2]);  // groups enter one contact at a time, in order
  CHECK(std::labs((onset[1] - onset[0]) - std::lround(0.120 * core::kSampleRate)) <= 4);
  CHECK(std::labs((onset[2] - onset[1]) - std::lround(0.180 * core::kSampleRate)) <= 4);

  // the same keybed read as a 2-contact keybed: two groups, the second at contact 2
  Rig r2(KeybedCalibration::two_contact_73(), 50.0);
  for (std::uint8_t c = 0; c < 2; ++c) r2.key(32, c, true, ct[c]);
  const auto x2 = render_with(r2.out.ev, t0 + 300'000, 2, one_drawbar(8), frames, kFirst);  // drawbar 9 is in group 2 of the 2-contact table
  const long on2 = first_nonzero(x2);
  std::vector<StampedUmp> exact2 = r2.out.ev;
  CHECK(exact2.size() == 3);
  if (exact2.size() < 3) return;
  exact2[0].host_ns = ct[0] + 300'000;
  exact2[1].host_ns = ct[1] + 300'000;
  exact2[2].host_ns = ct[1] + 300'000;
  const long ref2 = first_nonzero(render_with(exact2, t0 + 300'000, 2, one_drawbar(8), frames, kFirst));
  CHECK(on2 >= static_cast<long>(kFirst) + std::lround(0.120 * core::kSampleRate) && std::labs(on2 - ref2) <= 1);
}

// ---- review follow-ups ------------------------------------------------------------------------------------------------

std::vector<std::uint8_t> raw_frame(std::uint8_t type, std::uint16_t seq, const std::vector<std::uint8_t>& payload, std::uint8_t version = mcu::kProtocolVersion) {
  mcu_frame f;
  mcu_frame_init(&f, type, seq, 0);
  f.version = version;
  f.length = static_cast<std::uint8_t>(payload.size());
  for (std::size_t i = 0; i < payload.size(); ++i) f.payload[i] = payload[i];
  std::uint8_t buf[MCU_MAX_FRAME];
  const int n = mcu_encode_unchecked(&f, buf, sizeof buf);
  return std::vector<std::uint8_t>(buf, buf + n);
}

void test_duplicate_close_and_retrigger() {
  const KeybedCalibration t3 = KeybedCalibration::three_contact_73();
  {  // a duplicated close frame is not a new strike
    Rig r(t3);
    const std::uint64_t t = r.host0;
    r.key(6, 0, true, t + 10 * kMs);
    r.key(6, 1, true, t + 20 * kMs);
    r.key(6, 2, true, t + 30 * kMs);
    const std::size_t n = r.out.ev.size();
    r.key(6, 2, true, t + 30 * kMs);  // same frame again (same tick)
    CHECK(r.out.ev.size() == n && r.reader->counters().duplicate_closes == 1);
    CHECK(r.reader->key_sounding(6) && r.reader->key_mask(6) == 7);
    CHECK(count_kind(decode_all(r.out), Kind::kNoteOn) == 1 && count_kind(decode_all(r.out), Kind::kNoteOff) == 0);
  }
  {  // partial release (contact 3 opens, contacts 1 and 2 stay closed) and a re-strike: timed from the open, not the first strike
    Rig r(t3);
    const std::uint64_t t = r.host0;
    r.key(8, 0, true, t + 10 * kMs);
    r.key(8, 1, true, t + 20 * kMs);
    r.key(8, 2, true, t + 30 * kMs);
    r.key(8, 2, false, t + 2 * kSec);  // contacts 1 and 2 still closed: Story 3.6 waits for the release pair (3 -> 2), the Note Off is not out yet
    CHECK(r.reader->key_sounding(8) && r.reader->key_mask(8) == 3);
    r.out.ev.clear();
    r.key(8, 2, true, t + 2 * kSec + 30 * kMs);  // the original 3.5 scenario: re-strike 30 ms after the open, inside the release window
    const auto v = decode_all(r.out);
    // changed in 3.6: the Note Off now goes out first (release velocity from the 30 ms so far), then contact 3 closing, then the Note On
    CHECK(v.size() == 3 && v[0].kind == Kind::kNoteOff && v[1].kind == Kind::kContactClosed && v[2].kind == Kind::kNoteOn);
    CHECK(r.reader->counters().release_restrikes == 1 && r.reader->counters().release_timeouts == 0 && r.reader->counters().clock_clamps == 0);
    CHECK(!r.out.ev.empty() && std::llabs(static_cast<long long>(r.out.ev[0].ump.velocity16()) -
                                          static_cast<long long>(chain_velocity16(t3.release_range(), VelocityCurve::kNormal, kFixedVelocity7, 30 * kMs))) < 40);  // release velocity from 30 ms
    const VelocityRange vr = t3.velocity_range();
    auto chain = [&](std::uint64_t dt) { return chain_velocity16(vr, VelocityCurve::kNormal, kFixedVelocity7, dt); };
    if (v.size() == 3) CHECK(std::llabs(static_cast<long long>(v[2].vel) - static_cast<long long>(chain(30 * kMs))) < 40);
    CHECK(r.reader->counters().velocity_fallbacks == 0);
    // a re-strike after a full release is timed from the new first-contact closing
    r.key(8, 2, false, t + 3 * kSec);
    r.key(8, 1, false, t + 3 * kSec + 5 * kMs);  // the pair completes: Note Off with a fast release velocity
    r.key(8, 0, false, t + 3 * kSec + 10 * kMs);
    r.out.ev.clear();
    r.key(8, 0, true, t + 4 * kSec);
    r.key(8, 1, true, t + 4 * kSec + 10 * kMs);
    r.key(8, 2, true, t + 4 * kSec + 20 * kMs);
    const auto w = decode_all(r.out);
    CHECK(w.size() == 4 && w[3].kind == Kind::kNoteOn);
    if (w.size() == 4) CHECK(std::llabs(static_cast<long long>(w[3].vel) - static_cast<long long>(chain(20 * kMs))) < 40);
  }
  {  // partial release left alone past the slowest release time (poll), then a re-strike 90 ms after the open: timed from that open
    Rig r(t3);
    const std::uint64_t t = r.host0;
    r.key(8, 0, true, t + 10 * kMs);
    r.key(8, 1, true, t + 20 * kMs);
    r.key(8, 2, true, t + 30 * kMs);
    r.key(8, 2, false, t + 2 * kSec);
    r.reader->poll(t + 2 * kSec + 85 * kMs);  // the slowest release time has passed: the Note Off goes out at the slowest release velocity
    drain(r.q, r.out);
    CHECK(!r.reader->key_sounding(8) && r.reader->counters().release_timeouts == 1 && r.reader->counters().release_restrikes == 0);
    r.out.ev.clear();
    r.key(8, 2, true, t + 2 * kSec + 90 * kMs);
    const auto v = decode_all(r.out);
    CHECK(v.size() == 2 && v[0].kind == Kind::kContactClosed && v[1].kind == Kind::kNoteOn);
    const VelocityRange vr = t3.velocity_range();
    if (v.size() == 2) CHECK(std::llabs(static_cast<long long>(v[1].vel) - static_cast<long long>(chain_velocity16(vr, VelocityCurve::kNormal, kFixedVelocity7, 90 * kMs))) < 40);
    CHECK(v.size() == 2 && v[1].vel > 512 && v[1].vel < 6000);
  }
  {  // only the velocity-completing contact is seen (the first one's frame was lost): slowest velocity, counted
    Rig r(t3);
    r.key(12, 2, true, r.host0 + 10 * kMs);
    const auto v = decode_all(r.out);
    CHECK(v.size() == 2 && v[1].kind == Kind::kNoteOn && v[1].vel == 512);
    CHECK(r.reader->counters().velocity_fallbacks == 1);
  }
  {  // an open of a contact that never closed
    Rig r(t3);
    r.key(14, 1, false, r.host0 + 10 * kMs);
    CHECK(r.out.ev.empty() && r.reader->counters().spurious_opens == 1);
  }
}

void test_counters_and_hello() {
  const KeybedCalibration t3 = KeybedCalibration::three_contact_73();
  {  // parse failures and decoder rejections add up
    Rig r(t3);
    r.feed(raw_frame(MCU_TYPE_KEY_CONTACT, 0, {3, 0, 2}), r.host0);  // state 2: decodes, does not parse
    r.feed(raw_frame(MCU_TYPE_KEY_CONTACT, 1, {3, 0}), r.host0);     // wrong length: the decoder rejects it
    CHECK(r.reader->counters().rejected_frames == 2);
    CHECK(r.out.ev.empty());
  }
  {  // frames the decoder refuses may have carried a key event: recovery is pending
    Rig r(t3);
    r.feed(raw_frame(MCU_TYPE_KEY_CONTACT, 0, {3, 0, 1}, 0x30), r.host0);  // major version 3
    CHECK(r.reader->counters().version_rejects == 1 && r.reader->recovery_pending());
    r.reader->release_all(r.host0 + kMs);
    CHECK(!r.reader->recovery_pending());
    r.feed(raw_frame(MCU_TYPE_KEY_CONTACT, 1, {3, 0}), r.host0);  // bad length
    CHECK(r.reader->recovery_pending());
    r.reader->release_all(r.host0 + kMs);
    r.feed(raw_frame(99, 2, {1}), r.host0);  // unknown type
    CHECK(r.reader->recovery_pending());
  }
  {  // a repeated, identical HELLO is not a restart
    Rig r(t3);
    const mcu::Hello h{0x0100, mcu::kProtocolVersion, 1, 3};
    r.feed(r.mcu.frame(h, r.host0), r.host0);
    r.key(2, 0, true, r.host0 + 10 * kMs);
    r.key(2, 1, true, r.host0 + 20 * kMs);
    r.key(2, 2, true, r.host0 + 30 * kMs);
    const std::size_t n = r.out.ev.size();
    r.feed(r.mcu.frame(h, r.host0 + 40 * kMs), r.host0 + 40 * kMs);
    CHECK(r.reader->counters().mcu_resets == 0 && r.reader->key_sounding(2) && r.out.ev.size() == n);
    CHECK(r.reader->clock().valid());
    // the same HELLO with ticks that went back by seconds is a restart
    r.mcu.tick0 = 5;
    r.mcu.t0_ns = r.host0 + 50 * kMs;
    r.feed(r.mcu.frame(h, r.host0 + 50 * kMs), r.host0 + 50 * kMs);
    CHECK(r.reader->counters().mcu_resets == 1 && !r.reader->key_sounding(2));
  }
  {  // queue full: the dropped events are counted
    Rig r(t3);
    StampedUmp filler{};
    while (r.q.push(filler)) {}
    const auto w = r.mcu.key(1, 0, true, r.host0);
    r.reader->on_bytes(w.data(), w.size(), r.host0);
    CHECK(r.reader->counters().queue_full == 1);
  }
  {  // config sanitising
    HostEventQueue q;
    KeybedReaderConfig c;
    c.tick_hz = std::nan("");
    c.group = 77;
    c.channel = 99;
    KeybedReader rd(q, t3, c);
    const std::uint32_t t0 = 1000;
    mcu::FrameWriter w;
    const auto a = w.write(mcu::KeyContact{1, 0, true}, t0);
    rd.on_bytes(a.data(), a.size(), 5 * kSec);
    StampedUmp s;
    CHECK(q.pop(s) && s.ump.group() == 15 && s.ump.channel() == 15);
    CHECK(rd.clock().drift_ppm() == 0.0);  // nominal 1 MHz
    KeybedReaderConfig c2;
    c2.tick_hz = -5;
    KeybedReader rd2(q, t3, c2);
    CHECK(rd2.clock().drift_ppm() == 0.0);
  }
  static_assert(!std::is_copy_constructible_v<KeybedReader> && !std::is_move_constructible_v<KeybedReader> &&
                !std::is_copy_assignable_v<KeybedReader> && !std::is_move_assignable_v<KeybedReader>);
}

void test_drift_clock_rejection() {
  DriftClock c(1e6);
  std::uint64_t ns = 1'000'000'000;
  std::uint32_t tick = 1000;
  for (int i = 0; i < 6; ++i) {
    CHECK(c.add_sample(tick, ns, 400'000));
    tick += 1'000'000;
    ns += 1'000'000'000;
  }
  CHECK(c.samples() == 6 && c.rejected() == 0);
  const std::int64_t probe = c.unwrap(tick);
  const std::uint64_t before = c.to_host_ns(probe);
  // a delayed round trip is rejected and leaves the estimate alone
  CHECK(!c.add_sample(tick, ns, 5'000'000));
  CHECK(c.rejected() == 1 && c.samples() == 6 && c.to_host_ns(probe) == before);
  // 8 in a row: the link is slower for good, the best round trip is re-learned and the 8th is taken
  for (int i = 0; i < 6; ++i) {
    tick += 1'000'000;
    ns += 1'000'000'000;
    CHECK(!c.add_sample(tick, ns, 5'000'000));
  }
  CHECK(c.rejected() == 7 && c.samples() == 6 && c.to_host_ns(probe) == before);
  tick += 1'000'000;
  ns += 1'000'000'000;
  CHECK(c.add_sample(tick, ns, 5'000'000) && c.rejected() == 8 && c.samples() == 7);
  tick += 1'000'000;
  ns += 1'000'000'000;
  CHECK(c.add_sample(tick, ns, 5'200'000));  // within the re-learned bound
  // rejected samples still advance the unwrap reference
  DriftClock d(1e6);
  CHECK(d.add_sample(100, 1'000'000'000, 400'000));
  CHECK(!d.add_sample(0x70000000u, 2'000'000'000, 5'000'000));
  const std::int64_t u = d.unwrap(0xE0000000u);  // 0xE0000000 - 0x70000000 = 0x70000000 < 2^31: forward, not wrapped back
  CHECK(u > d.unwrap(0x70000000u));
  // through the reader: slow round trips show in sync_rejected
  Rig r(KeybedCalibration::three_contact_73());
  std::vector<std::uint8_t> sent;
  r.reader->set_tx([&](const std::uint8_t* b, std::size_t n) { sent.assign(b, b + n); });
  mcu::StreamDecoder sim_rx;
  std::uint64_t now = r.host0;
  for (int i = 0; i < 6; ++i, now += 100 * kMs) {
    r.reader->poll(now);
    auto frames = sim_rx.feed(sent);
    sent.clear();
    for (const mcu::Frame& f : frames) {
      auto m = mcu::parse_message(f);
      if (!m || !std::holds_alternative<mcu::TimeSync>(*m)) continue;
      const std::uint64_t delay = i < 4 ? 400'000 : 20 * kMs;  // the last two replies come late
      const auto reply = r.mcu.frame(mcu::TimeSync{true, std::get<mcu::TimeSync>(*m).token, r.mcu.tick_at(now + delay / 2)}, now + delay / 2);
      r.reader->on_bytes(reply.data(), reply.size(), now + delay);
    }
  }
  CHECK(r.reader->counters().sync_replies == 6 && r.reader->counters().sync_rejected == 2 && r.reader->clock().samples() == 4);
}

void test_velocity_mapper() {
  LinearVelocityMapper m;  // 2 ms .. 100 ms, minimum 512
  CHECK(m.velocity16(0) == 0xFFFF && m.velocity16(2 * kMs) == 0xFFFF);
  CHECK(m.velocity16(100 * kMs) == 512 && m.velocity16(10 * kSec) == 512);
  const std::uint16_t mid = m.velocity16(51 * kMs);
  CHECK(mid > 512 && mid < 0xFFFF && std::llabs(static_cast<long long>(mid) - 33024) < 4);
  std::uint16_t prev = 0xFFFF;
  for (std::uint64_t dt = 0; dt <= 120 * kMs; dt += kMs) {
    const std::uint16_t v = m.velocity16(dt);
    CHECK(v <= prev);
    prev = v;
  }
  LinearVelocityMapper degenerate(5 * kMs, 5 * kMs, 100);  // slow == fast: still defined
  CHECK(degenerate.velocity16(5 * kMs) == 0xFFFF && degenerate.velocity16(6 * kMs) == 100);
  struct Fixed final : VelocityMapper {
    std::uint16_t velocity16(std::uint64_t) const noexcept override { return 1234; }
  } fixed;
  Rig r(KeybedCalibration::two_contact_73());
  r.reader->set_velocity_mapper(&fixed);
  r.key(1, 0, true, r.host0 + 10 * kMs);
  r.key(1, 1, true, r.host0 + 20 * kMs);
  CHECK(decode(r.out.ev.back()).kind == Kind::kNoteOn && decode(r.out.ev.back()).vel == 1234);
  r.key(1, 1, false, r.host0 + 500 * kMs);
  r.key(1, 0, false, r.host0 + 510 * kMs);
  r.reader->set_velocity_mapper(nullptr);  // back to the linear default
  r.key(1, 0, true, r.host0 + kSec);
  r.key(1, 1, true, r.host0 + kSec + 10 * kMs);
  CHECK(decode(r.out.ev.back()).kind == Kind::kNoteOn && decode(r.out.ev.back()).vel != 1234);
}

}  // namespace

int main() {
  test_three_contact();
  test_two_contact_and_table_switch();
  test_lost_open_and_key_state();
  test_bad_frames_and_unknown_key();
  test_time_sync_and_loss();
  test_tick_wrap();
  test_hello_restart();
  test_drift();
  test_organ_render();
  test_duplicate_close_and_retrigger();
  test_counters_and_hello();
  test_drift_clock_rejection();
  test_velocity_mapper();
  if (g_fail) {
    std::printf("keybed_reader_test: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("keybed_reader_test: all passed\n");
  return 0;
}
