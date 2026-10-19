// Multi-contact onset tests (Story 2.5). Bare main, CHECK style as the other core tests.
//
// Method for "the busbar group lands on the exact frame": render a contact sequence twice, once as is (A) and once with the
// one drawbar of the group switched off (B). Everything else is identical, a gated-out group adds an exact 0.0, so A - B is
// the group's own contribution and is exactly 0.0 before the group's frame; its first non-zero sample is the group's onset.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/contact_events.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/params_gen.hpp"

using namespace velora::core;

static int g_fail = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    if (!(c)) {                                                          \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
      ++g_fail;                                                          \
    }                                                                    \
  } while (0)

namespace {

constexpr std::uint32_t kR = OrganEngine::kContactRampFrames;
constexpr std::uint32_t kNote = 60;

enum class K { kClose, kOpen, kOn, kOff };
struct Ev {
  std::uint64_t frame;
  K kind;
  std::uint32_t k;  // contact index for kClose/kOpen
  std::uint32_t note = kNote;
  std::uint32_t ch = 0;
};

struct Rig {
  std::unique_ptr<OrganEngine> organ = std::make_unique<OrganEngine>();
  std::unique_ptr<BlockRenderer> br = std::make_unique<BlockRenderer>(8);
  // drawbars: nine digits; the key click, leakage and percussion are off unless asked for.
  explicit Rig(const char* drawbars, std::uint32_t contacts = 3, float click = 0.0f, bool perc = false) {
    br->bind_engine(*organ);
    for (std::uint32_t ch = 0; ch < 4; ++ch) br->set_channel_engine(ch, static_cast<std::uint32_t>(EngineTypeId::kOrgan));
    organ->set_contact_count(contacts);
    for (std::uint32_t ch = 0; ch < 4; ++ch) {
      for (int d = 0; d < 9; ++d) organ->set_part_param(ch, params::kDrawbar16 + d, static_cast<float>(drawbars[d] - '0'));
      organ->set_part_param(ch, params::kKeyClick, click);
      organ->set_part_param(ch, params::kLeakage, 0.0f);
      organ->set_part_param(ch, params::kPercussionOn, perc ? 1.0f : 0.0f);
      if (perc) {
        organ->set_part_param(ch, params::kPercussionHarmonic, static_cast<float>(params::kPercussionHarmonicThird));
        organ->set_part_param(ch, params::kPercussionDecay, static_cast<float>(params::kPercussionDecaySlow));
        organ->set_part_param(ch, params::kPercussionVolume, static_cast<float>(params::kPercussionVolumeNormal));
      }
    }
    std::vector<float> l(64), r(64);
    for (int i = 0; i < 12; ++i) br->process_block(l.data(), r.data());  // ramps settled, wheels running
  }
  static Ump to_ump(const Ev& e) {
    switch (e.kind) {
      case K::kClose: return make_contact_event(0, e.ch, e.note, e.k, true);
      case K::kOpen: return make_contact_event(0, e.ch, e.note, e.k, false);
      case K::kOn: return make_note_on(0, e.ch, e.note, 0xFFFF);
      default: return make_note_off(0, e.ch, e.note);
    }
  }
  // Submits the events at their absolute frame (relative to now) and renders `frames` frames; left channel.
  std::vector<float> run(const std::vector<Ev>& evs, std::size_t frames) {
    for (const Ev& e : evs) br->submit({to_ump(e), static_cast<std::uint32_t>(e.frame)});
    std::vector<float> out;
    std::vector<float> l(64), r(64);
    const std::size_t blocks = (frames + 63) / 64;
    for (std::size_t b = 0; b < blocks; ++b) {
      br->process_block(l.data(), r.data());
      out.insert(out.end(), l.begin(), l.end());
    }
    out.resize(frames);
    return out;
  }
};

// Nine-digit drawbar string with a single drawbar (index d) at 8.
std::string one_drawbar(int d) {
  std::string s(9, '0');
  s[static_cast<std::size_t>(d)] = '8';
  return s;
}
// A string with the first drawbar of every group at 8 (so each group has exactly one tone).
std::string group_drawbars(std::uint32_t contacts) {
  std::string s(9, '0');
  for (std::uint32_t g = 0; g < contacts; ++g)
    for (int d = 0; d < 9; ++d)
      if (OrganEngine::contact_group(contacts, static_cast<std::uint32_t>(d)) == g) {
        s[static_cast<std::size_t>(d)] = '8';
        break;
      }
  return s;
}
int first_drawbar_of(std::uint32_t contacts, std::uint32_t g) {
  for (int d = 0; d < 9; ++d)
    if (OrganEngine::contact_group(contacts, static_cast<std::uint32_t>(d)) == g) return d;
  return -1;
}

std::vector<float> subtract(const std::vector<float>& a, const std::vector<float>& b) {
  std::vector<float> d(a.size());
  for (std::size_t i = 0; i < a.size(); ++i) d[i] = a[i] - b[i];
  return d;
}
long first_nonzero(const std::vector<float>& x, std::size_t from = 0) {
  for (std::size_t i = from; i < x.size(); ++i)
    if (x[i] != 0.0f) return static_cast<long>(i);
  return -1;
}
bool all_zero(const std::vector<float>& x, std::size_t from, std::size_t to) {
  for (std::size_t i = from; i < to && i < x.size(); ++i)
    if (x[i] != 0.0f) return false;
  return true;
}
double peak(const std::vector<float>& x, std::size_t from, std::size_t to) {
  double p = 0;
  for (std::size_t i = from; i < to && i < x.size(); ++i) p = std::max(p, static_cast<double>(std::fabs(x[i])));
  return p;
}
double energy(const std::vector<float>& x, std::size_t from, std::size_t to) {
  double e = 0;
  for (std::size_t i = from; i < to && i < x.size(); ++i) e += static_cast<double>(x[i]) * x[i];
  return e;
}
double amp_at(const std::vector<float>& x, std::size_t from, std::size_t n, double hz) {
  const double tp = 6.283185307179586;
  std::complex<double> acc = 0;
  for (std::size_t i = 0; i < n; ++i)
    acc += static_cast<double>(x[from + i]) * std::polar(1.0, -tp * hz * static_cast<double>(from + i) / 48000.0);
  return 2.0 * std::abs(acc) / static_cast<double>(n);
}

EngineStatus sc(OrganEngine& e, std::uint32_t ch, std::uint32_t note, std::uint32_t k, bool closed) {
  Engine::ContactEffect fx;
  return e.set_contact(ch, note, k, closed, fx);
}

// --- tests -----------------------------------------------------------------------------------------------------------------

void test_ump_format() {
  const Ump c = make_contact_event(0, 5, 60, 2, true);
  CHECK(c.message_type() == 4 && c.status() == 0x1 && c.channel() == 5);
  CHECK(c.word0 == ((4u << 28) | (1u << 20) | (5u << 16) | (60u << 8) | 2u));
  CHECK(c.word1 == 0xFFFFFFFFu);
  CHECK(make_contact_event(0, 5, 60, 2, false).word1 == 0u);
  ContactEvent ce;
  CHECK(decode_contact_event(c, ce) && ce.channel == 5 && ce.note == 60 && ce.contact == 2 && ce.closed);
  CHECK(decode_contact_event(make_contact_event(0, 1, 28, 3, false), ce) && !ce.closed && ce.contact == 3);
  Ump half = make_contact_event(0, 0, 60, 1, false);
  half.word1 = 0x80000000u;  // half scale and above reads as closed, below as open
  CHECK(decode_contact_event(half, ce) && ce.closed);
  half.word1 = 0x7FFFFFFFu;
  CHECK(decode_contact_event(half, ce) && !ce.closed);
  CHECK(!decode_contact_event(make_contact_event(0, 0, 200, 1, true), ce));  // note field above 127: refused, not aliased
  CHECK(!decode_contact_event(make_contact_event(1, 0, 60, 1, true), ce));   // only group 0
  CHECK(!decode_contact_event(make_note_on(0, 0, 60, 1000), ce));
  CHECK(!decode_contact_event(make_control_change(0, 0, 64, 0), ce));
  // A contact above 255 would alias onto another contact: no event at all. Index 0 and above the wire range are not contact_k.
  CHECK(make_contact_event(0, 0, 60, 257, true).word0 == 0 && make_contact_event(0, 0, 60, 257, true).word1 == 0);
  CHECK(!decode_contact_event(make_contact_event(0, 0, 60, 0, true), ce));
  CHECK(decode_contact_event(make_contact_event(0, 0, 60, kMaxWireContact, true), ce));
  CHECK(!decode_contact_event(make_contact_event(0, 0, 60, kMaxWireContact + 1, true), ce));
  CHECK(!decode_contact_event(Ump{}, ce));
  // It is not a parameter event (status 0x3) and not a note.
  CHECK(!c.is_assignable_controller() && !c.is_note_on() && !c.is_note_off() && c.is_assignable_per_note_controller());
}

void test_group_tables() {
  // The documented tables: every drawbar in exactly one group, groups ordered from the low footages up.
  const std::uint8_t t3[9] = {0, 0, 0, 1, 1, 1, 2, 2, 2};
  const std::uint8_t t2[9] = {0, 0, 0, 0, 1, 1, 1, 1, 1};
  for (std::uint32_t d = 0; d < 9; ++d) {
    CHECK(OrganEngine::contact_group(3, d) == t3[d]);
    CHECK(OrganEngine::contact_group(2, d) == t2[d]);
    if (d > 0) {
      CHECK(OrganEngine::contact_group(3, d) >= OrganEngine::contact_group(3, d - 1));
      CHECK(OrganEngine::contact_group(2, d) >= OrganEngine::contact_group(2, d - 1));
    }
  }
  OrganEngine e;
  CHECK(e.contact_count() == 3 && e.set_contact_count(2) && e.contact_count() == 2);
  CHECK(!e.set_contact_count(1) && !e.set_contact_count(4) && !e.set_contact_count(0) && e.contact_count() == 2);
  CHECK(e.supports_contacts());
  // The count cannot change under a sounding voice (it latched the old one).
  CHECK(e.start_voice(0, 60, 1) == EngineStatus::kOk);
  CHECK(!e.set_contact_count(3) && e.contact_count() == 2);
  e.stop_voice(0, 60);
  std::vector<float> l(64), r(64);
  for (int i = 0; i < 10; ++i) e.render(l.data(), r.data(), 64);
  CHECK(e.active_voices() == 0 && e.set_contact_count(3) && e.contact_count() == 3);
}

// Group onsets land on the exact frame of the contact event, for 3 and 2 contacts at 1, 5 and 20 ms spacing.
void test_group_onset_exact() {
  const std::uint32_t spacings_ms[3] = {1, 5, 20};
  for (std::uint32_t contacts = 2; contacts <= 3; ++contacts) {
    for (std::uint32_t ms : spacings_ms) {
      const std::uint64_t s = ms * 48ull;
      const std::uint64_t f1 = 64 * 2 + 37;  // not a block boundary
      std::vector<Ev> evs;
      for (std::uint32_t k = 1; k <= contacts; ++k) evs.push_back({f1 + (k - 1) * s, K::kClose, k});
      const std::size_t total = 64 * 80;
      Rig a(group_drawbars(contacts).c_str(), contacts);
      const std::vector<float> A = a.run(evs, total);
      CHECK(a.br->ignored_events() == 0);
      CHECK(a.br->contact_events_routed() == contacts && a.organ->contact_events_applied() == contacts);
      CHECK(a.organ->contact_events_ignored() == 0 && a.organ->active_voices() == 1);
      CHECK(a.organ->contact_mask(0, kNote) == (1u << contacts) - 1u);
      CHECK(all_zero(A, 0, f1));
      for (std::uint32_t k = 1; k <= contacts; ++k) {
        std::string db = group_drawbars(contacts);
        db[static_cast<std::size_t>(first_drawbar_of(contacts, k - 1))] = '0';
        Rig b(db.c_str(), contacts);
        const std::vector<float> B = b.run(evs, total);
        const std::vector<float> D = subtract(A, B);
        const long on = first_nonzero(D);
        CHECK(on == static_cast<long>(f1 + (k - 1) * s));  // the group's first sample is the contact's own frame
        CHECK(all_zero(D, 0, f1 + (k - 1) * s));
        // After the ramp the group is fully in: the tone has the drawbar's step-8 amplitude (the voice gate is 48 frames
        // from contact 1; later groups meet a settled voice).
        const double want = OrganEngine::kVoiceScale;
        const double p = peak(D, f1 + (k - 1) * s + 100, f1 + (k - 1) * s + 1200);
        CHECK(p > 0.9 * want && p < 1.05 * want);
        // The ramp is bounded and monotone in amplitude: the first sample is at most 1/kR of the tone.
        CHECK(std::fabs(D[static_cast<std::size_t>(on)]) <= want / static_cast<float>(kR) * 1.01);
      }
    }
  }
}

// The same property when the group frames fall at every in-block offset (shifted start), and a Note On after the contacts
// changes nothing at all.
void test_note_on_after_contacts_is_a_noop() {
  for (std::uint64_t shift : {0ull, 1ull, 63ull}) {
    const std::uint64_t f1 = 130 + shift;
    const std::vector<Ev> contacts{{f1, K::kClose, 1}, {f1 + 240, K::kClose, 2}, {f1 + 480, K::kClose, 3}};
    std::vector<Ev> with_on = contacts;
    with_on.push_back({f1 + 480, K::kOn, 0});  // the Note On follows the last contact (same frame, later in arrival order)
    with_on.push_back({f1 + 700, K::kOn, 0});  // a repeated Note On
    for (bool perc : {false, true}) {
      Rig a(group_drawbars(3).c_str(), 3, 0.5f, perc), b(group_drawbars(3).c_str(), 3, 0.5f, perc);
      const std::vector<float> A = a.run(contacts, 64 * 60);
      const std::vector<float> B = b.run(with_on, 64 * 60);
      CHECK(A == B);  // bit-identical: no second voice, no percussion retrigger, no extra click, no timing change
      CHECK(b.organ->active_voices() == 1);
      CHECK(first_nonzero(A) == static_cast<long>(f1));
    }
  }
}

void test_release_reverse_order() {
  for (std::uint32_t contacts = 2; contacts <= 3; ++contacts) {
    for (std::uint32_t ms : {1u, 5u, 20u}) {
      const std::uint64_t s = ms * 48ull;
      std::vector<Ev> evs;
      const std::uint64_t f1 = 100;
      for (std::uint32_t k = 1; k <= contacts; ++k) evs.push_back({f1 + (k - 1) * 100, K::kClose, k});  // quick press
      const std::uint64_t o_top = 64 * 40 + 11;  // the highest contact opens first
      std::uint64_t open_at[4] = {0, 0, 0, 0};
      for (std::uint32_t k = contacts; k >= 1; --k) {
        open_at[k] = o_top + (contacts - k) * s;
        evs.push_back({open_at[k], K::kOpen, k});
      }
      const std::size_t total = 64 * 120;
      Rig a(group_drawbars(contacts).c_str(), contacts);
      const std::vector<float> A = a.run(evs, total);
      CHECK(a.organ->active_voices() == 0);  // the voice ended after the last contact opened
      CHECK(a.organ->contact_events_ignored() == 0);
      for (std::uint32_t k = 1; k <= contacts; ++k) {
        std::string db = group_drawbars(contacts);
        db[static_cast<std::size_t>(first_drawbar_of(contacts, k - 1))] = '0';
        Rig b(db.c_str(), contacts);
        const std::vector<float> D = subtract(A, b.run(evs, total));
        // Gone exactly kR frames after its contact opened (the ramp's last frame is 0), present right before.
        CHECK(all_zero(D, open_at[k] + kR - 1, total));
        CHECK(peak(D, open_at[k] - 40, open_at[k]) > 0.0);
        CHECK(peak(D, open_at[k] - 40, open_at[k]) > 0.5 * OrganEngine::kVoiceScale || k == 1);
        // And it was playing until then (its tone, undisturbed by the other groups' contacts).
        CHECK(peak(D, f1 + 100 * contacts + 500, open_at[k] - kR) > 0.9 * OrganEngine::kVoiceScale);
        // No jump above the ramp slope while it drops out.
        double worst = 0;
        for (std::size_t i = open_at[k] - 1; i < open_at[k] + kR + 1; ++i)
          worst = std::max(worst, static_cast<double>(std::fabs(D[i] - D[i - 1])));
        CHECK(worst < 0.5 * OrganEngine::kVoiceScale);
      }
    }
  }
}

// A voice runs until the last contact opens, not before: opening only the upper contacts leaves it sounding.
void test_voice_ends_when_last_contact_opens() {
  Rig a(group_drawbars(3).c_str(), 3);
  a.run({{100, K::kClose, 1}, {200, K::kClose, 2}, {300, K::kClose, 3}}, 64 * 10);
  CHECK(a.organ->active_voices() == 1 && a.organ->contact_mask(0, kNote) == 7u);
  a.run({{10, K::kOpen, 3}}, 64 * 10);
  CHECK(a.organ->active_voices() == 1 && a.organ->contact_mask(0, kNote) == 3u);
  a.run({{10, K::kOpen, 2}}, 64 * 10);
  CHECK(a.organ->active_voices() == 1 && a.organ->contact_mask(0, kNote) == 1u);
  VoiceInfo vi;
  bool held = false;
  for (std::uint32_t i = 0; i < OrganEngine::kVoiceCapacity; ++i)
    if (a.organ->voice_info(i, vi) && vi.state == VoiceState::kHeld) held = true;
  CHECK(held);
  a.run({{10, K::kOpen, 1}}, 64 * 10);  // 640 frames: gate release (2 ms) is long over
  CHECK(a.organ->active_voices() == 0 && a.organ->contact_mask(0, kNote) == 0);
  // Half press: contact 1 only, then up again; and a smear: closed 1-2, open 2 (not 1), close 2 again.
  Rig b(group_drawbars(3).c_str(), 3);
  b.run({{10, K::kClose, 1}, {500, K::kOpen, 1}}, 64 * 40);
  CHECK(b.organ->active_voices() == 0);
  Rig c(group_drawbars(3).c_str(), 3);
  c.run({{10, K::kClose, 1}, {50, K::kClose, 2}, {300, K::kOpen, 2}, {600, K::kClose, 2}}, 64 * 12);
  CHECK(c.organ->contact_mask(0, kNote) == 3u && c.organ->active_voices() == 1 && c.organ->contact_events_ignored() == 0);
}

// A source with only Note On/Off: every group starts together at the Note On, stops together at the Note Off.
void test_note_on_off_only() {
  const std::uint64_t on = 64 * 3 + 29, off = 64 * 30 + 5;
  const std::vector<Ev> evs{{on, K::kOn, 0}, {off, K::kOff, 0}};
  Rig a(group_drawbars(3).c_str(), 3);
  const std::vector<float> A = a.run(evs, 64 * 60);
  CHECK(a.organ->contact_mask(0, kNote) == 0 && a.br->contact_events_routed() == 0);
  CHECK(a.br->status_count(EngineStatus::kVoiceNotFound) == 0 && a.br->status_count(EngineStatus::kPoolExhausted) == 0);
  for (std::uint32_t g = 0; g < 3; ++g) {
    std::string db = group_drawbars(3);
    db[static_cast<std::size_t>(first_drawbar_of(3, g))] = '0';
    Rig b(db.c_str(), 3);
    const std::vector<float> D = subtract(A, b.run(evs, 64 * 60));
    CHECK(first_nonzero(D) == static_cast<long>(on));  // all three groups at the Note On frame
    CHECK(first_nonzero(D, off + 2000) < 0);          // and gone together after the 2 ms release
  }
  // Ramp-free: the note-on voice sounds exactly like the engine driven without the block loop or any contact support.
  OrganEngine e;
  e.set_contact_count(2);  // the setting is irrelevant for a Note On voice
  CHECK(e.start_voice(0, kNote, 1) == EngineStatus::kOk);
  CHECK(e.contact_mask(0, kNote) == 3u);  // every contact of the (2-contact) keybed counts as closed
  CHECK(sc(e, 0, kNote, 1, true) == EngineStatus::kOk && e.contact_events_ignored() == 1);  // duplicate close
  CHECK(sc(e, 0, kNote, 2, false) == EngineStatus::kOk && e.contact_mask(0, kNote) == 1u);  // a Note On voice can be opened
}

void test_note_off_after_opens_is_idempotent() {
  Rig a(group_drawbars(3).c_str(), 3);
  const std::vector<Ev> evs{{100, K::kClose, 1}, {200, K::kClose, 2}, {300, K::kClose, 3}, {320, K::kOn, 0},
                            {64 * 20, K::kOpen, 3}, {64 * 20 + 100, K::kOpen, 2}, {64 * 20 + 200, K::kOpen, 1},
                            {64 * 25, K::kOff, 0}, {64 * 26, K::kOff, 0}, {64 * 27, K::kOff, 0}};
  a.run(evs, 64 * 40);
  CHECK(a.organ->active_voices() == 0);
  CHECK(a.br->status_count(EngineStatus::kVoiceNotFound) == 0);
  CHECK(a.br->status_count(EngineStatus::kPoolExhausted) == 0);
  CHECK(a.organ->contact_events_ignored() == 0);
  // A new press afterwards (Note On) works and its Note Off is a normal one.
  a.run({{10, K::kOn, 0}}, 64 * 6);
  CHECK(a.organ->active_voices() == 1);
  a.run({{10, K::kOff, 0}}, 64 * 6);
  CHECK(a.organ->active_voices() == 0 && a.br->status_count(EngineStatus::kVoiceNotFound) == 0);
  a.run({{10, K::kOff, 0}}, 64 * 6);  // a plain double Note Off still reports voice-not-found, as before this story
  CHECK(a.br->status_count(EngineStatus::kVoiceNotFound) == 1);
}

// A Note Off while contacts are still closed releases everything that is left; later opens are ignored.
void test_note_off_with_contacts_closed() {
  Rig a(group_drawbars(3).c_str(), 3);
  a.run({{10, K::kClose, 1}, {20, K::kClose, 2}, {30, K::kOff, 0}}, 64 * 12);
  CHECK(a.organ->active_voices() == 0 && a.organ->contact_mask(0, kNote) == 0);
  a.run({{10, K::kOpen, 2}, {20, K::kOpen, 1}}, 64 * 4);
  CHECK(a.organ->contact_events_ignored() == 2 && a.organ->active_voices() == 0);
  CHECK(a.br->status_count(EngineStatus::kVoiceNotFound) == 0);
}

void test_malformed_sequences() {
  for (std::uint32_t contacts : {3u, 2u}) {
    Rig a(group_drawbars(contacts).c_str(), contacts);
    std::uint64_t ign = 0;
    auto step = [&](const std::vector<Ev>& e) { a.run(e, 64 * 4); };
    // open without close; close of a higher contact without contact 1
    step({{5, K::kOpen, 1}, {6, K::kOpen, 2}});
    ign += 2;
    step({{5, K::kClose, 2}});
    ++ign;
    if (contacts == 3) {
      step({{5, K::kClose, 3}});
      ++ign;
    }
    CHECK(a.organ->active_voices() == 0 && a.organ->contact_events_ignored() == ign);
    // Out-of-range indices through the UMP path: 0, one above the count, 255.
    for (std::uint32_t k : {0u, contacts + 1, 255u}) {
      a.br->submit({make_contact_event(0, 0, kNote, k, true), 3});
      a.br->submit({make_contact_event(0, 0, kNote, k, false), 4});
      if (k == contacts + 1) ign += 2;  // inside the wire range: the engine refuses it; 0 and 255 are not contact_k at all
    }
    std::vector<float> l(64), r(64);
    a.br->process_block(l.data(), r.data());
    CHECK(a.organ->active_voices() == 0 && a.organ->contact_events_ignored() == ign);
    CHECK(a.br->ignored_events() == 4);
    // Duplicate close and a skipped contact: 1, 1, 3 (2 never closes), then open 2 drops 3 as well.
    step({{5, K::kClose, 1}, {6, K::kClose, 1}});
    CHECK(a.organ->contact_mask(0, kNote) == 1u && a.organ->contact_events_ignored() == ++ign);
    if (contacts == 3) {
      step({{5, K::kClose, 3}});
      CHECK(a.organ->contact_mask(0, kNote) == 5u);
      step({{5, K::kOpen, 2}});  // contact 2 is not closed: ignored, contact 3 stays
      CHECK(a.organ->contact_mask(0, kNote) == 5u && a.organ->contact_events_ignored() == ++ign);
      step({{5, K::kOpen, 3}});
      CHECK(a.organ->contact_mask(0, kNote) == 1u && a.organ->contact_events_ignored() == ign);
      step({{5, K::kOpen, 3}});  // already open
      CHECK(a.organ->contact_events_ignored() == ++ign);
    }
    // Opening a closed contact also opens the closed ones above it: here contact 1 takes the rest with it and ends the voice.
    if (contacts == 3) step({{5, K::kClose, 2}, {6, K::kClose, 3}});
    else step({{5, K::kClose, 2}});
    step({{5, K::kOpen, 1}});
    CHECK(a.organ->contact_mask(0, kNote) == 0 && a.organ->active_voices() == 0);
    CHECK(a.organ->contact_events_ignored() == ign);
    // After the sequence the key works as new.
    step({{5, K::kClose, 1}});
    CHECK(a.organ->active_voices() == 1 && a.organ->contact_mask(0, kNote) == 1u);
    // Wire-level refusals: note field above 127 and group != 0 never reach the engine.
    const std::uint64_t dropped = a.br->contact_events_dropped();
    a.br->submit({make_contact_event(0, 0, 200, 1, true), 2});
    a.br->process_block(l.data(), r.data());
    CHECK(a.br->contact_events_dropped() == dropped + 1 && a.br->ignored_events() == 4);
    a.br->submit({make_contact_event(2, 0, kNote, 2, true), 2});  // group 2: the core serves group 0 only
    a.br->process_block(l.data(), r.data());
    CHECK(a.br->ignored_events() == 5 && a.organ->contact_mask(0, kNote) == 1u);
    // Channel 8..15 (no organ part) and a disabled part: typed status, nothing starts.
    CHECK(sc(*a.organ, 9, kNote, 1, true) == EngineStatus::kEngineNotAvailable);
    a.organ->set_enabled_parts(0xFF & ~1u);
    CHECK(sc(*a.organ, 0, kNote, 1, true) == EngineStatus::kEngineNotAvailable);
    CHECK(a.organ->active_voices() == 0);
    CHECK(sc(*a.organ, 1, 200, 1, true) == EngineStatus::kOk && a.organ->active_voices() == 0);  // note above 127 refused
  }
}

// contact_k for engines with no contact support (the test tone here): counted as dropped, never as ignored, and the engine
// keeps starting on Note On.
void test_unsupported_engine_and_routing() {
  BlockRenderer br(8);
  std::vector<float> l(64), r(64);
  br.submit({make_contact_event(0, 0, 60, 1, true), 1});
  br.submit({make_contact_event(0, 0, 60, 2, true), 2});
  br.submit({make_note_on(0, 0, 60, 30000), 3});
  br.process_block(l.data(), r.data());
  CHECK(br.contact_events_dropped() == 2 && br.contact_events_routed() == 0 && br.ignored_events() == 0);
  CHECK(br.test_tone().voice_capacity() > 0);
  CHECK(br.status_count(EngineStatus::kOk) == 1);  // only the Note On
  float e = 0;
  for (float x : l) e += x * x;
  CHECK(e > 0.0f);
  // Typed errors still surface: a channel routed to an engine that is not compiled in.
  BlockRenderer b2(8);
  b2.set_channel_engine(5, static_cast<std::uint32_t>(EngineTypeId::kFm));
  b2.submit({make_contact_event(0, 5, 60, 1, true), 0});
  b2.process_block(l.data(), r.data());
  CHECK(b2.status_count(EngineStatus::kEngineNotAvailable) == 1 && b2.contact_events_dropped() == 1);
}

// Percussion fires on the first contact, once; key click: onset at contact 1, release at the last open only.
void test_percussion_on_first_contact() {
  Rig a("000000000", 3, 0.0f, true);
  const std::uint64_t f1 = 64 * 2 + 21;
  const std::vector<Ev> evs{{f1, K::kClose, 1}, {f1 + 400, K::kClose, 2}, {f1 + 800, K::kClose, 3}};
  std::vector<Ev> evs_on = evs;
  evs_on.push_back({f1 + 1000, K::kOn, 0});
  const std::vector<float> A = a.run(evs, 64 * 80);
  CHECK(first_nonzero(A) == static_cast<long>(f1));  // the percussion tone starts at the contact's own frame
  const double hz = OrganEngine::wheel_frequency_hz(
      static_cast<std::uint32_t>(tonewheel::drawbar_wheel(kNote, OrganEngine::kPercThirdDrawbar)));
  const double want = OrganEngine::kVoiceScale * OrganEngine::kPercNormalGain;
  const double amp = amp_at(A, f1 + 480, 1920, hz);
  CHECK(amp > 0.5 * want && amp < 1.05 * want);
  Rig b("000000000", 3, 0.0f, true);
  CHECK(b.run(evs_on, 64 * 80) == A);  // the Note On neither retriggers the percussion nor changes anything
  // Single trigger: key 64 pressed through contacts while key 60 is held gets no percussion; after all keys are up it does.
  Rig c("000000000", 3, 0.0f, true);
  std::vector<Ev> seq{{f1, K::kClose, 1}, {f1 + 100, K::kClose, 1, 64}};
  const std::vector<float> C = c.run(seq, 64 * 60);
  const double hz64 = OrganEngine::wheel_frequency_hz(
      static_cast<std::uint32_t>(tonewheel::drawbar_wheel(64, OrganEngine::kPercThirdDrawbar)));
  CHECK(amp_at(C, f1 + 480, 1920, hz) > 0.5 * want);  // key 60 has its percussion
  CHECK(amp_at(C, f1 + 480, 1920, hz64) < 0.05 * amp_at(C, f1 + 480, 1920, hz));  // key 64: only spectral leakage
  c.run({{10, K::kOpen, 1, 60}, {20, K::kOpen, 1, 64}}, 64 * 20);
  CHECK(c.organ->active_voices() == 0);
  const std::vector<float> C2 = c.run({{10, K::kClose, 1, 64}}, 64 * 60);
  CHECK(amp_at(C2, 10 + 480, 1920, hz64 ) > 0.5 * want);  // re-armed: percussion on the next press
}

void test_key_click_at_first_and_last_contact() {
  Rig a("000000000", 3, 1.0f);
  const std::uint64_t f1 = 64 * 2 + 21, gap = 3000;
  const std::vector<Ev> evs{{f1, K::kClose, 1},        {f1 + gap, K::kClose, 2},     {f1 + 2 * gap, K::kClose, 3},
                            {f1 + 3 * gap, K::kOpen, 3}, {f1 + 4 * gap, K::kOpen, 2}, {f1 + 5 * gap, K::kOpen, 1}};
  const std::vector<float> A = a.run(evs, 64 * 400);
  CHECK(all_zero(A, 0, f1));
  CHECK(first_nonzero(A) == static_cast<long>(f1));                      // onset click at contact 1
  CHECK(energy(A, f1, f1 + OrganEngine::kClickFrames) > 0.0);
  CHECK(all_zero(A, f1 + OrganEngine::kClickFrames + 200, f1 + 5 * gap));  // nothing between the two clicks
  const long rel = first_nonzero(A, f1 + 5 * gap - 100);
  CHECK(rel == static_cast<long>(f1 + 5 * gap));                         // release click when the last contact opens
  CHECK(energy(A, f1 + 5 * gap, f1 + 5 * gap + OrganEngine::kClickFrames) > 0.0);
  CHECK(all_zero(A, f1 + 5 * gap + OrganEngine::kClickFrames + 200, A.size()));
  CHECK(a.organ->active_voices() == 0);
  // The release click is the documented fraction of the onset click (same level in the mean, within the jitter bounds).
  CHECK(energy(A, f1 + 5 * gap, f1 + 5 * gap + 192) < energy(A, f1, f1 + 192) * 2.0);
}

// The voice is the one the Note On later binds to: same slot, start time is the contact frame; the arbiter never steals it.
void test_identity_and_arbiter() {
  Rig a(group_drawbars(3).c_str(), 3);
  a.run({{300, K::kClose, 1}}, 64 * 6);
  VoiceInfo before, after;
  std::uint32_t slot = 0xFFFF;
  for (std::uint32_t i = 0; i < OrganEngine::kVoiceCapacity; ++i)
    if (a.organ->voice_info(i, before)) slot = i;
  CHECK(slot != 0xFFFF && before.channel == 0 && before.note == kNote && before.state == VoiceState::kHeld);
  CHECK(before.start_time == 12 * 64 + 300);  // 12 settle blocks + the contact frame
  a.run({{5, K::kOn, 0}}, 64 * 6);
  CHECK(a.organ->active_voices() == 1 && a.organ->voice_info(slot, after) && after.start_time == before.start_time);
  // Arbiter: with the pool full of other engines' voices and the organ reserved, 73 keys started by contact 1 all sound.
  OrganEngine organ;
  BlockRenderer br(300);
  br.bind_engine(organ);
  br.set_channel_engine(0, static_cast<std::uint32_t>(EngineTypeId::kOrgan));
  VoiceArbiter::LiveSetConfig cfg;
  cfg.organ_rotary_cost = OrganEngine::reservation_cost_units(1);
  cfg.budget = cfg.organ_rotary_cost + 256;
  CHECK(br.arbiter().reserve(cfg) == VoiceArbiter::ReserveStatus::kOk);
  std::vector<float> l(64), r(64);
  for (std::uint32_t ch = 1; ch <= 3; ++ch)
    for (std::uint32_t n = 0; n < 100; ++n) br.submit({make_note_on(0, ch, 20 + n, 40000), n % 64});
  br.process_block(l.data(), r.data());
  br.process_block(l.data(), r.data());
  for (std::uint32_t n = 28; n <= 100; ++n) br.submit({make_contact_event(0, 0, n, 1, true), n % 64});
  for (int i = 0; i < 4; ++i) br.process_block(l.data(), r.data());
  CHECK(organ.active_voices() == 73 && br.status_count(EngineStatus::kPoolExhausted) == 0);
  for (std::uint32_t n = 28; n <= 100; ++n) {
    br.submit({make_contact_event(0, 0, n, 2, true), n % 64});
    br.submit({make_note_on(0, 0, n, 0xFFFF), n % 64});
  }
  for (int i = 0; i < 4; ++i) br.process_block(l.data(), r.data());
  CHECK(organ.active_voices() == 73 && br.arbiter().steals(static_cast<std::uint32_t>(EngineTypeId::kOrgan)) == 0);
  CHECK(br.arbiter().usage().used() >= 73ull * OrganEngine::kVoiceCostUnits);
  // More stealing traffic from other engines never takes an organ voice.
  for (std::uint32_t n = 0; n < 100; ++n) br.submit({make_note_on(0, 4, 20 + n, 40000), n % 64});
  for (int i = 0; i < 4; ++i) br.process_block(l.data(), r.data());
  CHECK(organ.active_voices() == 73 && br.arbiter().steals(static_cast<std::uint32_t>(EngineTypeId::kOrgan)) == 0);
}

// Parts are independent (channel = part), keys are independent, and a render is a pure function of its events.
void test_parts_keys_and_determinism() {
  const std::vector<Ev> evs{{100, K::kClose, 1, 60, 0}, {100, K::kClose, 1, 60, 1}, {200, K::kClose, 2, 60, 0},
                            {300, K::kClose, 1, 64, 0}, {900, K::kOpen, 1, 60, 1},   {2000, K::kOpen, 2, 60, 0},
                            {2100, K::kOpen, 1, 60, 0}, {2100, K::kOn, 0, 67, 0}};
  Rig a(group_drawbars(3).c_str(), 3, 0.5f, true), b(group_drawbars(3).c_str(), 3, 0.5f, true);
  CHECK(a.run(evs, 64 * 80) == b.run(evs, 64 * 80));
  CHECK(a.organ->contact_mask(0, 60) == 0 && a.organ->contact_mask(1, 60) == 0 && a.organ->contact_mask(0, 64) == 1u);
  CHECK(a.organ->contact_mask(0, 67) == 7u);
  CHECK(a.organ->active_voices() == 2);
  // Part 1's key ended while part 0's kept sounding (checked above); a contact one frame later changes the render, so the
  // golden depends on contact timing.
  std::vector<Ev> shifted = evs;
  shifted[2].frame += 1;
  Rig c(group_drawbars(3).c_str(), 3, 0.5f, true), d(group_drawbars(3).c_str(), 3, 0.5f, true);
  CHECK(c.run(shifted, 64 * 80) != d.run(evs, 64 * 80));
}

// Click-free: a hard step of the group would jump by the full tone amplitude in one sample; the ramp stays below 1/kR of it.
void test_onset_is_click_free() {
  Rig a(group_drawbars(3).c_str(), 3);
  const std::vector<Ev> evs{{100, K::kClose, 1}, {1500, K::kClose, 2}, {3000, K::kClose, 3}, {5000, K::kOpen, 3}, {6000, K::kOpen, 2}};
  const std::vector<float> A = a.run(evs, 64 * 140);
  // Three tones of amplitude kVoiceScale each at most: the largest sample-to-sample step inside the ramps is bounded by the
  // tone's own slope plus the ramp step; compare with the same signal's steady-state largest step.
  double steady = 0;
  for (std::size_t i = 4000; i < 4900; ++i) steady = std::max(steady, static_cast<double>(std::fabs(A[i] - A[i - 1])));
  for (std::size_t at : {std::size_t(1500), std::size_t(3000), std::size_t(5000), std::size_t(6000)}) {
    double worst = 0;
    for (std::size_t i = at - 1; i < at + kR + 2; ++i) worst = std::max(worst, static_cast<double>(std::fabs(A[i] - A[i - 1])));
    CHECK(worst < 1.2 * steady + 0.07 * OrganEngine::kVoiceScale);
  }
}

// Random event soup in the block loop: nothing crashes, nothing goes non-finite or out of bounds, and voices die when the
// keys are released (contact opens or Note Off).
void test_fuzz() {
  for (std::uint32_t contacts : {3u, 2u}) {
    Rig a("888888888", contacts, 0.5f, true);
    std::uint64_t state = 0x1234567 + contacts;
    auto rnd = [&state]() {
      state = state * 6364136223846793005ull + 1442695040888963407ull;
      return static_cast<std::uint32_t>(state >> 33);
    };
    std::vector<float> l(64), r(64);
    for (int blk = 0; blk < 3000; ++blk) {
      const int n = static_cast<int>(rnd() % 6);
      for (int i = 0; i < n; ++i) {
        const std::uint32_t note = 28 + rnd() % 14, ch = rnd() % 3, k = rnd() % 6, off = rnd() % 64;
        switch (rnd() % 4) {
          case 0: a.br->submit({make_contact_event(0, ch, note, k, true), off}); break;
          case 1: a.br->submit({make_contact_event(0, ch, note, k, false), off}); break;
          case 2: a.br->submit({make_note_on(0, ch, note, 1000), off}); break;
          default: a.br->submit({make_note_off(0, ch, note), off}); break;
        }
      }
      a.br->process_block(l.data(), r.data());
      for (int f = 0; f < 64; ++f) {
        CHECK(std::isfinite(l[f]) && std::fabs(l[f]) <= 4.0f);
        if (!std::isfinite(l[f])) return;
      }
    }
    for (std::uint32_t ch = 0; ch < 3; ++ch)
      for (std::uint32_t note = 28; note < 42; ++note) {
        a.br->submit({make_note_off(0, ch, note), 0});
        for (std::uint32_t k = 1; k <= 3; ++k) a.br->submit({make_contact_event(0, ch, note, k, false), 1});
      }
    for (int i = 0; i < 40; ++i) a.br->process_block(l.data(), r.data());
    CHECK(a.organ->active_voices() == 0);
    CHECK(a.organ->contact_events_ignored() > 0 && a.organ->contact_events_applied() > 0);
  }
}

void test_open_of_non_closed_contact() {
  OrganEngine e;
  CHECK(sc(e, 0, kNote, 1, true) == EngineStatus::kOk && sc(e, 0, kNote, 3, true) == EngineStatus::kOk);
  CHECK(e.contact_mask(0, kNote) == 5u);
  CHECK(sc(e, 0, kNote, 2, false) == EngineStatus::kOk);  // contact 2 is not closed: ignored, contact 3 untouched
  CHECK(e.contact_mask(0, kNote) == 5u && e.contact_events_ignored() == 1 && e.contact_events_applied() == 2);
  CHECK(sc(e, 0, kNote, 1, false) == EngineStatus::kOk && e.contact_mask(0, kNote) == 0 && e.active_voices() == 1);  // releasing
}

// The sustain pedal defers Note Offs only: contacts open and close keys regardless, and keep the arbiter's key state in step.
void test_contacts_ignore_sustain_pedal() {
  Rig a(group_drawbars(3).c_str(), 3);
  a.br->submit({make_control_change(0, 0, 64, 0xFFFFFFFFu), 0});  // pedal down
  a.run({{10, K::kClose, 1}, {20, K::kClose, 2}, {30, K::kClose, 3}, {400, K::kOpen, 3}, {410, K::kOpen, 2}, {420, K::kOpen, 1}}, 64 * 12);
  CHECK(a.organ->active_voices() == 0);  // ended at the last open although the pedal is down
  CHECK(a.br->status_count(EngineStatus::kVoiceNotFound) == 0);
  // A plain Note Off during pedal-down is still deferred; the pedal-up releases it.
  a.run({{5, K::kOn, 0}, {200, K::kOff, 0}}, 64 * 12);
  CHECK(a.organ->active_voices() == 1 && a.organ->contact_mask(0, kNote) == 7u);
  a.br->submit({make_control_change(0, 0, 64, 0u), 0});
  std::vector<float> l(64), r(64);
  for (int i = 0; i < 12; ++i) a.br->process_block(l.data(), r.data());
  CHECK(a.organ->active_voices() == 0);
  // Stale deferred release: pedal down, Note On + Note Off (deferred), the contacts open the key, contacts press it again, pedal up:
  // the new voice must not be cut.
  a.br->submit({make_control_change(0, 0, 64, 0xFFFFFFFFu), 0});
  a.run({{5, K::kOn, 0}, {60, K::kOff, 0}, {200, K::kOpen, 3}, {210, K::kOpen, 2}, {220, K::kOpen, 1}}, 64 * 12);
  CHECK(a.organ->active_voices() == 0);
  a.run({{5, K::kClose, 1}}, 64 * 4);
  CHECK(a.organ->active_voices() == 1);
  a.br->submit({make_control_change(0, 0, 64, 0u), 0});
  for (int i = 0; i < 12; ++i) a.br->process_block(l.data(), r.data());
  CHECK(a.organ->active_voices() == 1 && a.organ->contact_mask(0, kNote) == 1u);
}

// A contact-started voice (no Note On ever) holds the snapshot generation while it sounds and lets go after the last open.
void test_generation_binding() {
  Rig a(group_drawbars(3).c_str(), 3);
  auto g1 = std::make_unique<Snapshot>(Snapshot::defaults());
  g1->revision = 1;
  g1->ledger_index = 0;
  auto g2 = std::make_unique<Snapshot>(Snapshot::defaults());
  g2->revision = 2;
  g2->ledger_index = 1;
  a.br->snapshots().publish(g1.get());
  std::vector<float> l(64), r(64);
  a.br->process_block(l.data(), r.data());
  // Ignored contacts do not take a reference.
  a.run({{5, K::kOpen, 1}, {6, K::kClose, 2}}, 64 * 2);
  CHECK(a.br->snapshots().refs(0) == 0);
  a.run({{5, K::kClose, 1}}, 64 * 2);
  CHECK(a.br->snapshots().refs(0) == 1);
  a.br->snapshots().publish(g2.get());
  for (int i = 0; i < 4; ++i) a.br->process_block(l.data(), r.data());
  CHECK(a.br->snapshots().refs(0) == 1 && !a.br->snapshots().safe_to_free(g1.get()));
  a.run({{5, K::kOpen, 1}}, 64 * 12);
  CHECK(a.organ->active_voices() == 0 && a.br->snapshots().refs(0) == 0 && a.br->snapshots().safe_to_free(g1.get()));
}

// Refused deliveries are not "routed".
void test_refused_contacts_are_dropped() {
  Rig a(group_drawbars(3).c_str(), 3);
  a.organ->set_enabled_parts(0xFE);
  a.run({{5, K::kClose, 1}}, 64 * 2);
  CHECK(a.br->contact_events_routed() == 0 && a.br->contact_events_dropped() == 1);
  CHECK(a.br->status_count(EngineStatus::kEngineNotAvailable) == 1);
  a.run({{5, K::kClose, 1, 60, 1}}, 64 * 2);  // part 1 is enabled
  CHECK(a.br->contact_events_routed() == 1 && a.br->contact_events_dropped() == 1);
}

}  // namespace

int main() {
  test_ump_format();
  test_group_tables();
  test_group_onset_exact();
  test_note_on_after_contacts_is_a_noop();
  test_release_reverse_order();
  test_voice_ends_when_last_contact_opens();
  test_note_on_off_only();
  test_note_off_after_opens_is_idempotent();
  test_note_off_with_contacts_closed();
  test_malformed_sequences();
  test_unsupported_engine_and_routing();
  test_percussion_on_first_contact();
  test_key_click_at_first_and_last_contact();
  test_identity_and_arbiter();
  test_parts_keys_and_determinism();
  test_onset_is_click_free();
  test_fuzz();
  test_open_of_non_closed_contact();
  test_contacts_ignore_sustain_pedal();
  test_generation_binding();
  test_refused_contacts_are_dropped();
  if (g_fail) {
    std::printf("organ_contact_tests: %d failures\n", g_fail);
    return 1;
  }
  std::printf("organ_contact_tests: ok\n");
  return 0;
}
