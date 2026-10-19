// Story 1.19: VoiceArbiter scenarios -- reservation, steal policy (held bass note, sustain pedal,
// mixed velocities), floors, steal ramp, counters, cost accounting with mixed costs, determinism.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/voice_arbiter.hpp"
#include "velora/ports/sample_source.hpp"

using namespace velora::core;
namespace ports = velora::ports;
using RS = VoiceArbiter::ReserveStatus;

static int g_fail = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    if (!(c)) {                                                          \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
      ++g_fail;                                                          \
    }                                                                    \
  } while (0)

namespace {

constexpr std::uint32_t kFmId = static_cast<std::uint32_t>(EngineTypeId::kFm);
constexpr std::uint32_t kToneId = static_cast<std::uint32_t>(EngineTypeId::kTestTone);
constexpr std::uint32_t kSampleId = static_cast<std::uint32_t>(EngineTypeId::kSample);

// A controllable engine standing in for FM/VA: level = velocity, release lasts a fixed number of frames.
class FakeEngine final : public Engine {
 public:
  static constexpr std::uint32_t kReleaseFrames = 100;
  FakeEngine(EngineTypeId id, std::uint32_t cost, std::uint32_t cap) : id_(id), cost_(cost), v_(cap) {}
  EngineTypeId type_id() const noexcept override { return id_; }
  std::uint32_t cost_units_per_voice() const noexcept override { return cost_; }
  std::uint32_t voice_capacity() const noexcept override { return static_cast<std::uint32_t>(v_.size()); }
  EngineStatus start_voice(std::uint32_t ch, std::uint32_t note, std::uint32_t vel16) noexcept override {
    const std::uint32_t s = acquire_slot(ch, note);
    if (s == VoiceAllocator::kNoSlot) return EngineStatus::kPoolExhausted;
    v_[s] = V{};
    v_[s].active = true;
    v_[s].ch = ch;
    v_[s].note = note;
    v_[s].level = static_cast<float>(vel16) / 65535.0f;
    v_[s].start = clock_;
    return EngineStatus::kOk;
  }
  EngineStatus stop_voice(std::uint32_t ch, std::uint32_t note) noexcept override {
    bool found = false;
    for (auto& v : v_)
      if (v.active && !v.stolen && !v.releasing && v.ch == ch && v.note == note) {
        v.releasing = true;
        v.left = kReleaseFrames;
        found = true;
      }
    return found ? EngineStatus::kOk : EngineStatus::kVoiceNotFound;
  }
  void set_param(std::uint32_t, float) noexcept override {}
  void render_split(float* const* l, float* const* r, std::uint32_t frames) noexcept override { render(l[0], r[0], frames); }
  void render(float* l, float* r, std::uint32_t frames) noexcept override {
    for (auto& v : v_) {
      if (!v.active) continue;
      for (std::uint32_t f = 0; f < frames; ++f) {
        float g = 1.0f;
        if (v.stolen) {
          if (v.left == 0) { v.active = false; break; }
          g = static_cast<float>(v.left) / static_cast<float>(v.total);
          --v.left;
        } else if (v.releasing) {
          if (v.left == 0) { v.active = false; break; }
          --v.left;
        }
        l[f] += 0.001f * v.level * g;
        r[f] += 0.001f * v.level * g;
      }
    }
    clock_ += frames;
  }
  bool voice_info(std::uint32_t slot, VoiceInfo& out) const noexcept override {
    if (slot >= v_.size() || !v_[slot].active) return false;
    const V& v = v_[slot];
    out.start_time = v.start;
    out.level = v.level;
    out.state = (v.stolen || v.releasing) ? VoiceState::kReleasing : VoiceState::kHeld;
    out.channel = v.ch;
    out.note = v.note;
    return true;
  }
  bool begin_steal_fade(std::uint32_t slot, std::uint32_t ramp) noexcept override {
    if (slot >= v_.size() || !v_[slot].active || v_[slot].stolen || static_cast<int>(slot) == refuse) return false;
    v_[slot].stolen = true;
    v_[slot].total = v_[slot].left = ramp;
    return true;
  }
  int refuse = -1;  // slot whose steal fade is refused
  void resize(std::size_t n) { v_.resize(n); }
  std::uint32_t active() const {
    std::uint32_t n = 0;
    for (const auto& v : v_) n += v.active;
    return n;
  }
  bool note_alive(std::uint32_t ch, std::uint32_t note) const {
    for (const auto& v : v_)
      if (v.active && !v.stolen && v.ch == ch && v.note == note) return true;
    return false;
  }

 private:
  struct V {
    bool active = false, stolen = false, releasing = false;
    std::uint32_t ch = 0, note = 0, left = 0, total = 1;
    float level = 0;
    std::uint64_t start = 0;
  };
  EngineTypeId id_;
  std::uint32_t cost_;
  std::vector<V> v_;
  std::uint64_t clock_ = 0;
};

struct Block { float l[kBlockFrames], r[kBlockFrames]; };
Block render(BlockRenderer& br, int n = 1) {
  Block b{};
  for (int i = 0; i < n; ++i) br.process_block(b.l, b.r);
  return b;
}
void on(BlockRenderer& br, std::uint32_t ch, std::uint32_t note, std::uint32_t vel, std::uint32_t frame = 0) {
  br.submit({make_note_on(0, ch, note, vel), frame});
}
void off(BlockRenderer& br, std::uint32_t ch, std::uint32_t note, std::uint32_t frame = 0) {
  br.submit({make_note_off(0, ch, note), frame});
}
void route_all(BlockRenderer& br, std::uint32_t id) {
  for (std::uint32_t c = 0; c < 16; ++c) br.set_channel_engine(c, id);
}
VoiceArbiter::LiveSetConfig cfg(std::uint64_t budget) {
  VoiceArbiter::LiveSetConfig c;
  c.budget = budget;
  return c;
}
void add_floor(VoiceArbiter::LiveSetConfig& c, std::uint32_t id, std::uint32_t voices) {
  c.floors[c.floor_count++] = {id, voices};
}

// --- reservation ------------------------------------------------------------------------------------

void test_reservation() {
  auto br = std::make_unique<BlockRenderer>(80);
  FakeEngine fm(EngineTypeId::kFm, 4, 80);
  CHECK(br->bind_engine(fm));
  auto& a = br->arbiter();
  CHECK(!a.reserved());

  auto c = cfg(100);
  c.fx_cost = 10;
  c.organ_rotary_cost = 20;
  add_floor(c, kFmId, 3);      // 12
  add_floor(c, kToneId, 5);    // 5
  CHECK(a.reserve(c) == RS::kOk);
  CHECK(a.reserved());
  CHECK(a.reservation().voice_budget == 70 && a.reservation().floors == 17 && a.reservation().shared == 53);
  CHECK(a.floor_cost(kFmId) == 12 && a.floor_cost(kToneId) == 5);

  // Each failure names the step that did not fit, and leaves the previous reservation untouched.
  auto bad = cfg(100); bad.fx_cost = 101;
  CHECK(a.reserve(bad) == RS::kFxExceedsBudget);
  bad = cfg(100); bad.fx_cost = 60; bad.organ_rotary_cost = 41;
  CHECK(a.reserve(bad) == RS::kOrganExceedsBudget);
  bad = cfg(100); bad.fx_cost = 60; bad.organ_rotary_cost = 30; add_floor(bad, kFmId, 3);  // 12 > 10
  CHECK(a.reserve(bad) == RS::kFloorsExceedBudget);
  bad = cfg(100); add_floor(bad, kFmId, 17);  // 17 + headroom 64 > capacity 80
  CHECK(a.reserve(bad) == RS::kFloorExceedsCapacity);
  bad = cfg(100); add_floor(bad, 77, 1);
  CHECK(a.reserve(bad) == RS::kUnknownEngine);
  bad = cfg(100); add_floor(bad, kFmId, 1); add_floor(bad, kFmId, 1);
  CHECK(a.reserve(bad) == RS::kInvalidConfig);
  bad = cfg(100); bad.steal_ramp_frames = 0;
  CHECK(a.reserve(bad) == RS::kInvalidConfig);
  bad = cfg(100); bad.floor_count = 99;
  CHECK(a.reserve(bad) == RS::kInvalidConfig);
  CHECK(a.reserved() && a.reservation().budget == 100 && a.reservation().shared == 53 &&
        a.floor_cost(kFmId) == 12 && a.steal_ramp_frames() == VoiceArbiter::kStealRampFrames);

  // Exactly feasible: shared remainder 0. Huge budgets do not overflow.
  auto exact = cfg(12); add_floor(exact, kFmId, 3);
  CHECK(a.reserve(exact) == RS::kOk && a.reservation().shared == 0);
  auto big = cfg(~std::uint64_t{0}); big.fx_cost = ~std::uint64_t{0} - 5;
  CHECK(a.reserve(big) == RS::kOk && a.reservation().voice_budget == 5);
  big = cfg(~std::uint64_t{0}); big.fx_cost = ~std::uint64_t{0};
  big.organ_rotary_cost = 1;
  CHECK(a.reserve(big) == RS::kOrganExceedsBudget);

  // A changed engine since bind() is rejected; a zero-cost engine cannot be bound; no bind once reserved.
  CHECK(a.reserve(exact) == RS::kOk);
  FakeEngine va(EngineTypeId::kVa, 1, 4);
  CHECK(!a.bind(va));  // reservation active
  CHECK(!br->bind_engine(va) && br->registry().size() == 2);  // unwound
  a.clear_reservation();
  FakeEngine zero(EngineTypeId::kVa, 0, 4);
  CHECK(!a.bind(zero));
  fm.resize(90);
  CHECK(a.reserve(exact) == RS::kInvalidConfig);
  fm.resize(80);
  CHECK(a.reserve(exact) == RS::kOk);
  a.clear_reservation();
  CHECK(!a.reserved() && a.reservation().budget == 0);
}

void test_sample_floor_minimum() {
  ports::SampleRegionDesc d;
  struct Src final : ports::SampleSource {
    std::vector<ports::SampleRegionDesc> r; std::vector<ports::SamplePcm> p; std::vector<float> data;
    ports::InstrumentView instrument() const noexcept override { return {r, p}; }
  } src;
  src.data.assign(1000, 0.5f);
  ports::SamplePcm pcm; pcm.frames = 1000; pcm.left = src.data.data();
  src.p.push_back(pcm);
  src.r.push_back(d);
  SampleEngine::Config sc;
  sc.max_voices = 320;
  SampleEngine se(sc);
  se.set_instrument(src);
  auto br = std::make_unique<BlockRenderer>(4);
  CHECK(br->bind_engine(se));
  auto& a = br->arbiter();
  VoiceArbiter::LiveSetConfig c;  // production enforcement: the sample floor is at least 256
  c.budget = 100000;
  CHECK(a.reserve(c) == RS::kSampleFloorTooLow);  // no sample floor at all
  add_floor(c, kSampleId, 255);
  CHECK(a.reserve(c) == RS::kSampleFloorTooLow);
  CHECK(!a.reserved());
  c.floor_count = 0;
  add_floor(c, kSampleId, 256);
  CHECK(a.reserve(c) == RS::kOk);
  CHECK(a.floor_cost(kSampleId) == 256ull * se.cost_units_per_voice());
  c.floor_count = 0;
  add_floor(c, kSampleId, 257);  // 257 + 64 headroom > 320
  CHECK(a.reserve(c) == RS::kFloorExceedsCapacity);
  CHECK(a.floor_cost(kSampleId) == 256ull * se.cost_units_per_voice());  // unchanged
  c.floor_count = 0;
  add_floor(c, kSampleId, 256);
  c.budget = 256ull * se.cost_units_per_voice() - 1;
  CHECK(a.reserve(c) == RS::kFloorsExceedBudget);

  // Story 4.3: a Live Set without a sample part waives the floor, but only a floor of exactly zero.
  c = VoiceArbiter::LiveSetConfig{};
  c.budget = 100000;
  c.waive_sample_floor = true;
  CHECK(a.reserve(c) == RS::kOk);                       // no sample floor at all, waived
  CHECK(a.floor_cost(kSampleId) == 0);
  add_floor(c, kSampleId, 255);
  CHECK(a.reserve(c) == RS::kSampleFloorTooLow);        // a non-zero floor below 256 is still refused
  c.floor_count = 0;
  add_floor(c, kSampleId, 1);
  CHECK(a.reserve(c) == RS::kSampleFloorTooLow);
  c.floor_count = 0;
  add_floor(c, kSampleId, 256);
  CHECK(a.reserve(c) == RS::kOk);
  CHECK(a.config().floors[0].voices == 256);            // config() is what the reservation was made with

  // The same for the organ: only an organ reservation of exactly zero is waived.
  OrganEngine organ;
  auto br2 = std::make_unique<BlockRenderer>(4);
  CHECK(br2->bind_engine(organ));
  VoiceArbiter::LiveSetConfig o;
  o.budget = 100000;
  CHECK(br2->arbiter().reserve(o) == RS::kInvalidConfig);  // a bound organ needs its 73 keys reserved
  o.waive_organ_reservation = true;
  CHECK(br2->arbiter().reserve(o) == RS::kOk);
  o.organ_rotary_cost = 1;
  CHECK(br2->arbiter().reserve(o) == RS::kInvalidConfig);  // non-zero but too small: still refused
}

// --- steal policy -----------------------------------------------------------------------------------

// Pool of 8 voices for the fake engine, everything on the fake engine.
struct Rig {
  std::unique_ptr<BlockRenderer> br = std::make_unique<BlockRenderer>(2);
  FakeEngine fm{EngineTypeId::kFm, 1, 24};
  explicit Rig(std::uint64_t pool = 8) {
    br->bind_engine(fm);
    route_all(*br, kFmId);
    CHECK(br->arbiter().reserve(cfg(pool)) == RS::kOk);
  }
  VoiceArbiter& a() { return br->arbiter(); }
};

void test_full_pool_held_bass() {
  Rig r;
  on(*r.br, 0, 36, 60000, 0);  // bass: the lowest held note
  for (std::uint32_t i = 0; i < 7; ++i) on(*r.br, 0, 60 + i, 60000, 1 + i);
  render(*r.br, 2);
  CHECK(r.fm.active() == 8 && r.a().steals() == 0);
  // Equal levels: the newer voice goes first (note 66, started last); the bass is never chosen.
  on(*r.br, 0, 70, 60000);
  render(*r.br);
  CHECK(r.a().steals() == 1 && r.a().last_steal().note == 66 && r.a().last_steal().engine_type_id == kFmId);
  render(*r.br, 4);
  CHECK(r.fm.active() == 8 && !r.fm.note_alive(0, 66));
  // Dense playing: 30 more notes, the bass note survives all of them.
  for (std::uint32_t i = 0; i < 30; ++i) {
    on(*r.br, 0, 72 + i % 20, 60000);
    render(*r.br, 2);
    CHECK(r.a().last_steal().note != 36);
    CHECK(r.fm.note_alive(0, 36));
  }
  CHECK(r.a().steals() == 31);
  CHECK(r.a().steals(kFmId) == 31 && r.a().steals(kToneId) == 0);
}

void test_mixed_velocities() {
  Rig r;
  const std::uint32_t vel[8] = {50000, 40000, 30000, 6000, 52000, 3000, 45000, 20000};
  for (std::uint32_t i = 0; i < 8; ++i) on(*r.br, 0, 40 + i, vel[i], i);  // lowest held = 40 (vel 50000)
  render(*r.br, 2);
  // Quietest unprotected voice first: 45 (3000), then 43 (6000), then 47 (20000).
  const std::uint32_t expect[3] = {45, 43, 47};
  for (std::uint32_t i = 0; i < 3; ++i) {
    on(*r.br, 0, 90 + i, 65535);
    render(*r.br, 3);
    CHECK(r.a().last_steal().note == expect[i]);
  }
  CHECK(r.a().steals() == 3 && r.fm.note_alive(0, 40));
}

void test_sustain_pedal() {
  const std::uint32_t kSus = 0xFFFFFFFFu;
  Rig q;
  on(*q.br, 0, 36, 60000);
  for (std::uint32_t i = 0; i < 3; ++i) on(*q.br, 1, 60 + i, 20000, 1 + i);
  for (std::uint32_t i = 0; i < 4; ++i) on(*q.br, 0, 70 + i, 60000, 5 + i);
  render(*q.br, 2);
  q.br->submit({make_control_change(0, 1, 64, kSus), 0});
  for (std::uint32_t i = 0; i < 3; ++i) off(*q.br, 1, 60 + i, 1 + i);
  render(*q.br, 2);
  // The pedal notes are quiet (they would go first by level) but protected: held voices (and the newer
  // arrivals replacing each other) are stolen, the bass and the three pedal notes stay.
  for (std::uint32_t i = 0; i < 6; ++i) {
    on(*q.br, 0, 80 + i, 60000);
    render(*q.br, 3);
    CHECK(q.a().last_steal().channel == 0 && q.a().last_steal().note != 36);
  }
  CHECK(q.fm.note_alive(1, 60) && q.fm.note_alive(1, 61) && q.fm.note_alive(1, 62) && q.fm.note_alive(0, 36));
  // Pedal-sustained are the last resort: remove every other candidate.
  Rig s(4);
  on(*s.br, 0, 36, 60000);
  for (std::uint32_t i = 0; i < 3; ++i) on(*s.br, 1, 60 + i, 20000 + 1000 * i, 1 + i);
  render(*s.br, 2);
  s.br->submit({make_control_change(0, 1, 64, kSus), 0});
  for (std::uint32_t i = 0; i < 3; ++i) off(*s.br, 1, 60 + i, 1 + i);
  render(*s.br, 2);
  on(*s.br, 0, 90, 60000);
  render(*s.br, 3);
  // Only the bass (lowest held) and the sustained voices exist: a sustained voice goes, the quietest (60).
  CHECK(s.a().steals() == 1 && s.a().last_steal().channel == 1 && s.a().last_steal().note == 60);
  CHECK(s.fm.note_alive(0, 36));
  // Next note: the new note 90 (a plain held voice) is the candidate before sustained/bass.
  on(*s.br, 0, 91, 60000);
  render(*s.br, 3);
  CHECK(s.a().last_steal().note == 90);
  // Pedal up releases the deferred notes through the engine.
  s.br->submit({make_control_change(0, 1, 64, 0), 0});
  render(*s.br, 1);
  CHECK(!s.a().sustain_down(1));
  render(*s.br, 4);
  CHECK(!s.fm.note_alive(1, 61) && !s.fm.note_alive(1, 62));
}

void test_releasing_first_and_last_resort() {
  Rig r(3);
  on(*r.br, 0, 36, 60000, 0);
  on(*r.br, 0, 60, 10000, 1);
  on(*r.br, 0, 61, 65000, 2);
  render(*r.br, 1);
  off(*r.br, 0, 61, 0);  // loud but releasing: stolen before the quiet held voice
  render(*r.br, 1);
  on(*r.br, 0, 70, 60000);
  render(*r.br, 1);
  CHECK(r.a().last_steal().note == 61);
  // Everything left is held: the lowest held note is the very last resort.
  Rig s(1);
  on(*s.br, 0, 36, 60000);
  render(*s.br, 2);
  on(*s.br, 0, 37, 60000);
  render(*s.br, 1);
  CHECK(s.a().steals() == 1 && s.a().last_steal().note == 36);  // nothing else to take: the bass goes
}

void test_no_candidate_exhausted() {
  // Nobody to steal from: the sample-like engine sits exactly at its floor, tone has no floor.
  auto br = std::make_unique<BlockRenderer>(8);
  FakeEngine fm(EngineTypeId::kFm, 2, 70);
  br->bind_engine(fm);
  br->set_channel_engine(1, kFmId);
  auto c = cfg(4);
  add_floor(c, kFmId, 2);
  CHECK(br->arbiter().reserve(c) == RS::kOk);
  on(*br, 1, 60, 60000);
  on(*br, 1, 62, 60000);
  render(*br);
  CHECK(fm.active() == 2);
  on(*br, 0, 70, 60000);  // tone: shared is 0, fm is at its floor
  render(*br);
  CHECK(br->last_status() == EngineStatus::kPoolExhausted && br->arbiter().steals() == 0);
  CHECK(br->arbiter().exhausted() == 1 && br->test_tone().active_voices() == 0 && fm.active() == 2);
  // The engine at its floor can still steal from itself (its own newest, here note 62).
  on(*br, 1, 64, 60000);
  render(*br);
  CHECK(br->arbiter().steals() == 1 && br->arbiter().last_steal().note == 62);
}

void test_unreserved_is_legacy() {
  auto br = std::make_unique<BlockRenderer>(2);
  on(*br, 0, 60, 60000);
  on(*br, 0, 62, 60000);
  on(*br, 0, 64, 60000);
  render(*br);
  CHECK(br->status_count(EngineStatus::kPoolExhausted) == 1 && br->arbiter().steals() == 0);
  CHECK(br->test_tone().active_voices() == 2 && br->arbiter().exhausted() == 1);
}

// --- floors and mixed costs -------------------------------------------------------------------------

void test_mixed_costs_and_floors() {
  auto br = std::make_unique<BlockRenderer>(40);  // tone: cost 1
  FakeEngine fm(EngineTypeId::kFm, 4, 70);        // cost 4
  br->bind_engine(fm);
  br->set_channel_engine(1, kFmId);
  auto c = cfg(40);
  c.fx_cost = 4;
  c.organ_rotary_cost = 4;      // voice budget 32
  add_floor(c, kFmId, 2);       // 8 -> shared 24
  CHECK(br->arbiter().reserve(c) == RS::kOk && br->arbiter().reservation().shared == 24);
  auto& a = br->arbiter();

  for (std::uint32_t i = 0; i < 24; ++i) on(*br, 0, 30 + i, 60000, i % 8);
  render(*br, 2);
  CHECK(br->test_tone().active_voices() == 24 && a.steals() == 0);
  // Two FM voices fit in its reserved floor with no steal at all (floors are guaranteed).
  on(*br, 1, 60, 60000);
  on(*br, 1, 62, 60000);
  render(*br, 2);
  CHECK(fm.active() == 2 && a.steals() == 0);
  CHECK(a.usage().live_cost == 24 + 8 && a.usage().fading_cost == 0);
  CHECK(a.usage(kFmId).live_cost == 8 && a.usage(kToneId).live_cost == 24);
  // A third FM voice (cost 4) needs 4 shared units: it takes four tone voices (cost 1 each).
  on(*br, 1, 64, 60000);
  render(*br, 1);
  CHECK(a.steals() == 4 && a.steals(kToneId) == 4 && a.steals(kFmId) == 0);
  CHECK(fm.active() == 3);
  // The stolen tone voices keep counting until silent: 4 x 1 fading on top of the budget.
  CHECK(a.usage().fading_cost == 4 && a.usage().live_cost == 20 + 12);
  CHECK(a.usage().used() == 36);
  render(*br, 4);  // 4 ms ramp is over
  CHECK(a.usage().fading_cost == 0 && br->test_tone().active_voices() == 20);
  // FM-only squeeze: ten more FM notes take the shared units from the tone voices, then from each other.
  for (std::uint32_t i = 0; i < 10; ++i) {
    on(*br, 1, 70 + i, 60000);
    render(*br, 4);
    CHECK(a.usage().live_cost <= 32 && a.usage(kFmId).live_cost >= 8);
  }
  CHECK(fm.active() >= 7);  // FM now holds (nearly) the whole voice budget: 8 x 4
  CHECK(a.steals(kToneId) >= 20);
  // Tone pressure (louder than FM) now takes FM voices back, but only down to FM's floor of 2 voices.
  br->test_tone().set_param(TestToneEngine::kParamGain, 8.0f);
  const std::uint64_t fm_steals = a.steals(kFmId);
  for (std::uint32_t i = 0; i < 40; ++i) {
    on(*br, 0, 30 + i % 20, 60000);
    render(*br, 3);
    CHECK(a.usage(kFmId).live_cost >= 8 && a.usage().live_cost <= 32);
  }
  CHECK(a.steals(kFmId) > fm_steals && fm.active() >= 2);
}

struct SampleSrc final : ports::SampleSource {
  std::vector<ports::SampleRegionDesc> r;
  std::vector<ports::SamplePcm> p;
  std::vector<float> data;
  ports::InstrumentView instrument() const noexcept override { return {r, p}; }
  void init(std::size_t frames) {
    data.assign(frames, 0.25f);
    ports::SamplePcm pc; pc.frames = frames; pc.left = data.data();
    p.push_back(pc);
  }
  ports::SampleRegionDesc& region(int lo, int hi) {
    ports::SampleRegionDesc d; d.key_lo = static_cast<std::uint8_t>(lo); d.key_hi = static_cast<std::uint8_t>(hi);
    d.key_center = static_cast<std::uint8_t>((lo + hi) / 2);
    r.push_back(d);
    return r.back();
  }
};

void test_sample_floor_256() {
  SampleSrc src;
  src.init(400000);
  src.region(0, 127);
  SampleEngine::Config sc;
  sc.max_voices = 320;
  SampleEngine se(sc);
  se.set_instrument(src);
  const std::uint32_t sc_cost = se.cost_units_per_voice();
  CHECK(sc_cost == 5);
  auto br = std::make_unique<BlockRenderer>(100);
  br->bind_engine(se);
  for (std::uint32_t ch = 2; ch < 16; ++ch) br->set_channel_engine(ch, kSampleId);
  VoiceArbiter::LiveSetConfig c;  // production sample floor minimum: 256
  c.fx_cost = 50;
  c.organ_rotary_cost = 100;
  add_floor(c, kSampleId, 256);
  c.budget = 150 + 256ull * sc_cost + 20;  // 20 shared units
  CHECK(br->arbiter().reserve(c) == RS::kOk && br->arbiter().reservation().shared == 20);
  auto& a = br->arbiter();

  // Tone pressure first: 20 voices fill the shared remainder; more tone notes steal tone.
  for (std::uint32_t i = 0; i < 20; ++i) on(*br, 0, 30 + i, 60000, i);
  render(*br, 2);
  CHECK(br->test_tone().active_voices() == 20 && a.steals() == 0);
  // 256 sample voices (channels 2 and 3 x 128 notes) all start: below the floor nothing is refused.
  for (std::uint32_t k = 0; k < 256; ++k) on(*br, 2 + k / 128, k % 128, 40000, 0);
  render(*br, 2);
  CHECK(se.active_voices() >= 256 && a.usage(kSampleId).live_cost == 256ull * sc_cost);
  CHECK(br->status_count(EngineStatus::kPoolExhausted) == 0);
  // Tone keeps hammering: it can only steal from itself, the sample voices never drop below 256.
  for (std::uint32_t i = 0; i < 100; ++i) {
    on(*br, 0, 30 + i % 40, 60000);
    render(*br, 1);
    CHECK(a.usage(kSampleId).live_cost >= 256ull * sc_cost);
  }
  CHECK(a.steals(kSampleId) == 0 && a.steals(kToneId) > 0);
  // Another sample note beyond the floor wins against tone (needs 5 shared units: takes tone voices).
  const std::uint64_t before = a.steals(kToneId);
  on(*br, 4, 60, 40000);
  render(*br, 1);
  CHECK(a.steals(kToneId) >= before + 5);
  CHECK(br->status_count(EngineStatus::kPoolExhausted) == 0);
  // Once past its floor, samples may steal from each other.
  for (std::uint32_t i = 0; i < 20; ++i) {
    on(*br, 5, i, 40000);
    render(*br, 1);
    CHECK(a.usage(kSampleId).live_cost >= 256ull * sc_cost);
  }
  CHECK(a.steals() == a.steals(kToneId) + a.steals(kSampleId));
}

// A note never steals the voices of its own note-on (all regions of one SFZ note).
void test_own_event_not_stolen() {
  SampleSrc src;
  src.init(100000);
  for (int i = 0; i < 3; ++i) src.region(60, 60);
  SampleEngine::Config sc;
  sc.max_voices = 8;
  SampleEngine se(sc);
  se.set_instrument(src);
  auto br = std::make_unique<BlockRenderer>(4);
  br->bind_engine(se);
  br->set_channel_engine(0, kSampleId);
  br->arbiter().set_min_sample_floor_for_tests(0);
  auto c = cfg(2 * 5);  // two sample voices
  CHECK(br->arbiter().reserve(c) == RS::kOk);
  on(*br, 0, 60, 60000);
  render(*br);
  CHECK(br->last_status() == EngineStatus::kPoolExhausted);
  CHECK(br->arbiter().steals() == 0 && se.active_voices() == 2);
}

// --- ramp ---------------------------------------------------------------------------------------------

void test_ramp_tone_and_sample() {
  // Twin engines: one stolen, one not; the ratio is the fade gain: strictly falling to 0 over the ramp.
  for (int kind = 0; kind < 2; ++kind) {
    Arena arena_a(4096), arena_b(4096);
    TestToneEngine ta(arena_a, 2), tb(arena_b, 2);
    SampleSrc src;
    src.init(100000);
    src.region(0, 127);
    SampleEngine::Config sc;
    sc.max_voices = 2;
    SampleEngine sa(sc), sb(sc);
    sa.set_instrument(src);
    sb.set_instrument(src);
    Engine& a = kind ? static_cast<Engine&>(sa) : static_cast<Engine&>(ta);
    Engine& b = kind ? static_cast<Engine&>(sb) : static_cast<Engine&>(tb);
    CHECK(a.start_voice(0, 69, 60000) == EngineStatus::kOk && b.start_voice(0, 69, 60000) == EngineStatus::kOk);
    float la[512], ra[512], lb[512], rb[512];
    std::fill(la, la + 512, 0.0f); std::fill(ra, ra + 512, 0.0f); std::fill(lb, lb + 512, 0.0f); std::fill(rb, rb + 512, 0.0f);
    a.render(la, ra, 256); b.render(lb, rb, 256);  // warm up identically
    const std::uint32_t ramp = 192;
    CHECK(b.begin_steal_fade(0, ramp));
    CHECK(!b.begin_steal_fade(0, ramp));  // already fading
    std::fill(la, la + 512, 0.0f); std::fill(lb, lb + 512, 0.0f);
    VoiceInfo vi;
    CHECK(b.voice_info(0, vi) && vi.state == VoiceState::kReleasing && vi.note == 69);
    b.render(lb, rb, 100);  // still counted (active) mid-ramp
    CHECK(b.voice_info(0, vi));
    b.render(lb + 100, rb + 100, 412);
    a.render(la, ra, 512);
    CHECK(!b.voice_info(0, vi));  // silent: freed
    CHECK(a.voice_info(0, vi));   // the twin keeps sounding
    float prev = 1.0f;
    bool monotone = true;
    int checked = 0;
    for (std::uint32_t i = 0; i < 512; ++i) {
      if (i >= ramp) {
        monotone = monotone && lb[i] == 0.0f;
        continue;
      }
      if (std::fabs(la[i]) > 0.02f) {
        const float g = lb[i] / la[i];
        const float want = static_cast<float>(ramp - i) / static_cast<float>(ramp);
        monotone = monotone && g <= prev + 1e-5f && std::fabs(g - want) < 1e-3f;
        prev = g;
        ++checked;
      }
    }
    CHECK(monotone && checked > 50);
  }
}

void test_ramp_counts_until_silent() {
  Rig r(2);
  on(*r.br, 0, 60, 60000, 0);
  on(*r.br, 0, 61, 60000, 1);
  render(*r.br, 1);
  on(*r.br, 0, 62, 60000);
  render(*r.br, 1);  // 64 of 192 frames elapsed
  CHECK(r.a().steals() == 1);
  CHECK(r.fm.active() == 3);
  CHECK(r.a().usage().fading_cost == 1 && r.a().usage().live_cost == 2);
  // While the first victim still fades, the next note must take another victim: the fading cost is counted.
  on(*r.br, 0, 63, 60000);
  render(*r.br, 1);
  CHECK(r.a().steals() == 2 && r.a().usage().fading_cost == 2);
  render(*r.br, 3);
  CHECK(r.a().usage().fading_cost == 0 && r.fm.active() == 2);
}

// --- determinism ----------------------------------------------------------------------------------------

struct Run { std::vector<float> audio; std::vector<std::uint32_t> victims; std::uint64_t steals; };
Run script_run(bool use_sample) {
  auto br = std::make_unique<BlockRenderer>(24);
  FakeEngine fm(EngineTypeId::kFm, 3, 80);
  SampleSrc src;
  src.init(30000);
  src.region(0, 127);
  SampleEngine::Config sc;
  sc.max_voices = 80;
  SampleEngine se(sc);
  se.set_instrument(src);
  std::uint32_t id = kFmId;
  if (use_sample) {
    br->bind_engine(se);
    br->arbiter().set_min_sample_floor_for_tests(0);
    id = kSampleId;
  } else {
    br->bind_engine(fm);
  }
  br->set_channel_engine(1, id);
  auto c = cfg(60);
  add_floor(c, id, 4);
  CHECK(br->arbiter().reserve(c) == RS::kOk);
  std::uint32_t x = 12345;
  auto rnd = [&] { x = x * 1664525u + 1013904223u; return x >> 8; };
  Run out;
  float l[kBlockFrames], r[kBlockFrames];
  for (int blk = 0; blk < 600; ++blk) {
    for (int k = 0; k < 3; ++k) {
      const std::uint32_t ch = rnd() % 2, note = 30 + rnd() % 60, vel = 1000 + rnd() % 60000, fr = rnd() % 64;
      if (rnd() % 4 == 0) off(*br, ch, note, fr); else on(*br, ch, note, vel, fr);
    }
    if (blk % 50 == 0) br->submit({make_control_change(0, 0, 64, (blk / 50) % 2 ? 0xFFFFFFFFu : 0u), 5});
    br->process_block(l, r);
    for (std::uint32_t i = 0; i < kBlockFrames; ++i) { out.audio.push_back(l[i]); out.audio.push_back(r[i]); }
    const auto s = br->arbiter().last_steal();
    out.victims.push_back(s.engine_type_id * 1000003u + s.channel * 100003u + s.note * 131u + s.slot);
  }
  out.steals = br->arbiter().steals();
  return out;
}

void test_determinism() {
  for (int sample = 0; sample < 2; ++sample) {
    const Run a = script_run(sample), b = script_run(sample);
    CHECK(a.steals > 10 && a.steals == b.steals);
    CHECK(a.victims == b.victims);
    CHECK(a.audio.size() == b.audio.size() &&
          std::equal(a.audio.begin(), a.audio.end(), b.audio.begin(), [](float x, float y) {
            return std::memcmp(&x, &y, sizeof x) == 0;
          }));
  }
}

// --- pedal, bookkeeping, hardening ----------------------------------------------------------------------

void test_pedal_threshold() {
  auto br = std::make_unique<BlockRenderer>(4);
  auto cc = [&](std::uint32_t group, std::uint32_t ch, std::uint32_t idx, std::uint32_t val) {
    br->submit({make_control_change(group, ch, idx, val), 0});
    render(*br);
  };
  cc(0, 0, 64, 0x7FFFFFFFu);
  CHECK(!br->arbiter().sustain_down(0));  // MIDI 63: up
  cc(0, 0, 64, 0x80000000u);
  CHECK(br->arbiter().sustain_down(0));   // MIDI 64: down
  cc(1, 0, 64, 0u);                       // group 1: ignored, pedal unchanged
  CHECK(br->arbiter().sustain_down(0) && br->ignored_events() == 1);
  cc(0, 0, 7, 0u);                        // other controller: counted as ignored, pedal untouched
  CHECK(br->arbiter().sustain_down(0) && br->ignored_events() == 2);
  cc(0, 0, 64, 0x40000000u);              // MIDI 32: up
  CHECK(!br->arbiter().sustain_down(0));
}

void test_refused_fade() {
  Rig r(3);
  for (std::uint32_t i = 0; i < 3; ++i) on(*r.br, 0, 60 + i, 60000, i);
  render(*r.br, 2);
  r.fm.refuse = 2;  // the natural victim (note 62, newest) refuses to fade
  on(*r.br, 0, 70, 60000);
  render(*r.br, 4);
  CHECK(r.a().steals() == 1 && r.a().last_steal().note == 61);  // next victim, refused one not counted
  CHECK(r.fm.note_alive(0, 62) && !r.fm.note_alive(0, 61) && r.fm.note_alive(0, 70));
  CHECK(r.a().usage().fading_cost == 0);
}

void test_stale_state_reset() {
  Rig r(2);
  on(*r.br, 0, 60, 60000, 0);
  on(*r.br, 0, 61, 60000, 1);
  r.br->submit({make_control_change(0, 0, 64, 0xFFFFFFFFu), 2});
  on(*r.br, 0, 62, 60000, 3);
  render(*r.br, 2);
  CHECK(r.a().steals() == 1 && r.a().sustain_down(0) && r.a().last_steal().valid);
  CHECK(r.a().reserve(cfg(2)) == RS::kOk);
  CHECK(!r.a().sustain_down(0) && r.a().steals() == 0 && r.a().exhausted() == 0 && !r.a().last_steal().valid);
  CHECK(r.a().steals(kFmId) == 0);
  CHECK(r.fm.active() >= 2);  // sounding voices keep sounding
}

void test_note_off_bookkeeping() {
  {  // a note off for a stolen note is benign
    Rig r(2);
    on(*r.br, 0, 60, 60000, 0);
    on(*r.br, 0, 61, 60000, 1);
    render(*r.br, 2);
    on(*r.br, 0, 62, 60000);
    render(*r.br, 4);
    CHECK(!r.fm.note_alive(0, 61));
    off(*r.br, 0, 61);
    render(*r.br);
    CHECK(r.br->status_count(EngineStatus::kVoiceNotFound) == 0 && r.br->stolen_note_offs() == 1);
    off(*r.br, 0, 99);  // a plain unknown note off is still an error
    render(*r.br);
    CHECK(r.br->status_count(EngineStatus::kVoiceNotFound) == 1);
  }
  {  // pedal up for a note that never sounded
    auto br = std::make_unique<BlockRenderer>(1);
    on(*br, 0, 60, 60000);
    on(*br, 0, 62, 60000, 1);  // pool exhausted: never sounds
    render(*br);
    br->submit({make_control_change(0, 0, 64, 0xFFFFFFFFu), 0});
    off(*br, 0, 62);
    render(*br);
    br->submit({make_control_change(0, 0, 64, 0u), 0});
    render(*br);
    CHECK(br->status_count(EngineStatus::kVoiceNotFound) == 0);
  }
  {  // the deferred release goes to the engine that served the note, even if the channel is re-routed
    Rig r(4);
    on(*r.br, 0, 60, 60000);
    render(*r.br);
    r.br->submit({make_control_change(0, 0, 64, 0xFFFFFFFFu), 0});
    off(*r.br, 0, 60, 1);
    render(*r.br);
    r.br->set_channel_engine(0, kToneId);
    r.br->submit({make_control_change(0, 0, 64, 0u), 0});
    render(*r.br, 4);
    CHECK(!r.fm.note_alive(0, 60) && r.br->dropped_deferred_releases() == 0);
  }
  {  // the engine is gone (never bound): the release is dropped and counted, the flag is consumed
    auto br = std::make_unique<BlockRenderer>(4);
    br->set_channel_engine(3, static_cast<std::uint32_t>(EngineTypeId::kVa));
    on(*br, 3, 60, 60000);
    render(*br);
    br->submit({make_control_change(0, 3, 64, 0xFFFFFFFFu), 0});
    off(*br, 3, 60, 1);
    render(*br);
    br->submit({make_control_change(0, 3, 64, 0u), 0});
    render(*br);
    CHECK(br->dropped_deferred_releases() == 1);
    br->submit({make_control_change(0, 3, 64, 0u), 0});
    render(*br);
    CHECK(br->dropped_deferred_releases() == 1);
  }
}

void test_pedal_several_channels() {
  Rig r(8);
  on(*r.br, 0, 60, 60000);
  on(*r.br, 1, 60, 60000);
  on(*r.br, 2, 60, 60000);
  render(*r.br, 2);
  r.br->submit({make_control_change(0, 0, 64, 0xFFFFFFFFu), 0});
  r.br->submit({make_control_change(0, 2, 64, 0xFFFFFFFFu), 0});
  off(*r.br, 0, 60, 1);
  off(*r.br, 1, 60, 1);
  off(*r.br, 2, 60, 1);
  render(*r.br, 4);
  CHECK(r.fm.note_alive(0, 60) && !r.fm.note_alive(1, 60) && r.fm.note_alive(2, 60));
  r.br->submit({make_control_change(0, 0, 64, 0u), 0});
  render(*r.br, 4);
  CHECK(!r.fm.note_alive(0, 60) && r.fm.note_alive(2, 60));
  r.br->submit({make_control_change(0, 2, 64, 0u), 0});
  render(*r.br, 4);
  CHECK(!r.fm.note_alive(2, 60));
}

void test_sample_release_trigger_with_pedal() {
  SampleSrc src;
  src.init(100000);
  src.region(60, 60);
  src.region(60, 60).trigger = ports::SampleTrigger::kRelease;
  SampleEngine::Config sc;
  sc.max_voices = 8;
  SampleEngine se(sc);
  se.set_instrument(src);
  auto br = std::make_unique<BlockRenderer>(4);
  br->bind_engine(se);
  br->set_channel_engine(0, kSampleId);
  on(*br, 0, 60, 60000);
  render(*br, 2);
  CHECK(se.active_voices() == 1);
  br->submit({make_control_change(0, 0, 64, 0xFFFFFFFFu), 0});
  off(*br, 0, 60, 1);
  render(*br, 2);
  CHECK(se.active_voices() == 1);  // attack voice still held by the pedal, no release voice yet
  VoiceInfo vi;
  CHECK(se.voice_info(0, vi) && vi.state == VoiceState::kHeld);
  br->submit({make_control_change(0, 0, 64, 0u), 0});
  render(*br, 1);
  CHECK(se.active_voices() == 2);  // the release-trigger voice starts at pedal up
}

void test_sample_slot_reuse_after_steal() {
  SampleSrc src;
  src.init(200000);
  src.region(0, 127);
  SampleEngine::Config sc;
  sc.max_voices = 4;
  SampleEngine se(sc);
  se.set_instrument(src);
  auto br = std::make_unique<BlockRenderer>(4);
  br->bind_engine(se);
  br->set_channel_engine(0, kSampleId);
  br->arbiter().set_min_sample_floor_for_tests(0);
  CHECK(br->arbiter().reserve(cfg(5)) == RS::kOk);  // one sample voice
  on(*br, 0, 60, 60000);
  render(*br, 2);
  on(*br, 0, 61, 60000);  // steals 60 (slot 0 fades, 61 takes slot 1)
  render(*br, 5);
  CHECK(se.active_voices() == 1);
  on(*br, 0, 62, 60000);  // steals 61; takes slot 0 again (reuse)
  render(*br, 5);
  CHECK(se.active_voices() == 1 && br->arbiter().steals() == 2);
  VoiceInfo vi;
  CHECK(se.voice_info(0, vi) && vi.note == 62 && vi.state == VoiceState::kHeld);
  render(*br, 40);
  CHECK(se.active_voices() == 1);  // the reused voice is not fading
  off(*br, 0, 62);
  render(*br, 100);
  CHECK(se.active_voices() == 0);  // and it responds to the note off
  CHECK(br->status_count(EngineStatus::kVoiceNotFound) == 0);
}

// Stolen mid-ramp, then a note off: the ramp must not change (compare with an un-stopped twin).
void test_note_off_during_ramp() {
  for (int kind = 0; kind < 2; ++kind) {
    Arena arena_a(4096), arena_b(4096);
    TestToneEngine ta(arena_a, 2), tb(arena_b, 2);
    SampleSrc src;
    src.init(100000);
    src.region(0, 127);
    SampleEngine::Config sc;
    sc.max_voices = 2;
    SampleEngine sa(sc), sb(sc);
    sa.set_instrument(src);
    sb.set_instrument(src);
    Engine& a = kind ? static_cast<Engine&>(sa) : static_cast<Engine&>(ta);
    Engine& b = kind ? static_cast<Engine&>(sb) : static_cast<Engine&>(tb);
    CHECK(a.start_voice(0, 69, 60000) == EngineStatus::kOk && b.start_voice(0, 69, 60000) == EngineStatus::kOk);
    float la[300] = {}, ra[300] = {}, lb[300] = {}, rb[300] = {};
    a.render(la, ra, 100); b.render(lb, rb, 100);
    std::fill(la, la + 300, 0.0f); std::fill(ra, ra + 300, 0.0f); std::fill(lb, lb + 300, 0.0f); std::fill(rb, rb + 300, 0.0f);
    CHECK(a.begin_steal_fade(0, 192) && b.begin_steal_fade(0, 192));
    a.render(la, ra, 50); b.render(lb, rb, 50);
    CHECK(b.stop_voice(0, 69) == EngineStatus::kVoiceNotFound);  // fading voices ignore note offs
    a.render(la + 50, ra + 50, 250); b.render(lb + 50, rb + 50, 250);
    CHECK(std::memcmp(la, lb, sizeof la) == 0 && std::memcmp(ra, rb, sizeof ra) == 0);
    VoiceInfo vi;
    CHECK(!a.voice_info(0, vi) && !b.voice_info(0, vi));
  }
}

// Dense stealing on both engines: a sample note below its floor is always granted a voice.
void test_below_floor_always_granted() {
  SampleSrc src;
  src.init(4000);  // short: voices end by themselves, so the sample engine keeps dipping below its floor
  src.region(0, 127);
  SampleEngine::Config sc;
  sc.max_voices = 16 + VoiceArbiter::kFadeHeadroomVoices;
  SampleEngine se(sc);
  se.set_instrument(src);
  auto br = std::make_unique<BlockRenderer>(100);
  br->bind_engine(se);
  for (std::uint32_t ch = 2; ch < 16; ++ch) br->set_channel_engine(ch, kSampleId);
  br->arbiter().set_min_sample_floor_for_tests(0);
  auto c = cfg(16 * 5 + 24);
  add_floor(c, kSampleId, 16);
  CHECK(br->arbiter().reserve(c) == RS::kOk);
  auto& a = br->arbiter();
  std::uint32_t x = 777;
  auto rnd = [&] { x = x * 1664525u + 1013904223u; return x >> 8; };
  float l[kBlockFrames], r[kBlockFrames];
  std::uint32_t below = 0, steals_seen = 0;
  for (int i = 0; i < 20000; ++i) {
    const std::uint32_t op = rnd() % 10;
    const bool sample_note = op < 2;
    if (op < 3) on(*br, sample_note ? 2 + rnd() % 14 : 0, sample_note ? 70 + rnd() % 40 : rnd() % 128, 20000 + rnd() % 40000);
    const bool was_below = a.usage(kSampleId).live_cost < 16ull * 5;
    const auto exhausted_before = br->status_count(EngineStatus::kPoolExhausted);
    br->process_block(l, r);
    if (sample_note && was_below) {
      ++below;
      CHECK(br->status_count(EngineStatus::kPoolExhausted) == exhausted_before);
    }
    if (a.usage().fading_cost > 0) ++steals_seen;
  }
  CHECK(below > 200 && steals_seen > 100 && a.steals() > 100);
}

}  // namespace

int main() {
  test_reservation();
  test_sample_floor_minimum();
  test_full_pool_held_bass();
  test_mixed_velocities();
  test_sustain_pedal();
  test_releasing_first_and_last_resort();
  test_no_candidate_exhausted();
  test_unreserved_is_legacy();
  test_mixed_costs_and_floors();
  test_sample_floor_256();
  test_own_event_not_stolen();
  test_ramp_tone_and_sample();
  test_ramp_counts_until_silent();
  test_determinism();
  test_pedal_threshold();
  test_refused_fade();
  test_stale_state_reset();
  test_note_off_bookkeeping();
  test_pedal_several_channels();
  test_sample_release_trigger_with_pedal();
  test_sample_slot_reuse_after_steal();
  test_note_off_during_ramp();
  test_below_floor_always_granted();
  if (g_fail) {
    std::printf("voice_arbiter_tests: %d FAILED\n", g_fail);
    return 1;
  }
  std::printf("voice_arbiter_tests: ok\n");
  return 0;
}
