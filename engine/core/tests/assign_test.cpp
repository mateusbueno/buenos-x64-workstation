// Story 4.10: ASSIGN-mode encoder routing in the RT plane. apply_relative_step (every parameter type, the ends, big steps, no wrap), the reserved
// control events (encoder step, encoder mode, select part), and BlockRenderer routing: a step changes the live value of the parameter mapped to its
// slot in the held generation's ASSIGN map (no pickup), an empty slot does nothing, PART / ALL do nothing yet (counted), part-scope parameters land on the
// selected part, the map follows the generation, malformed events and stale maps are refused and counted. No timing assertions anywhere.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "velora/core/assign_events.hpp"
#include "velora/core/block_renderer.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/snapshot.hpp"

using namespace velora::core;
using namespace velora::core::params;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static void step_blocks(BlockRenderer& br, int blocks = 1) {
  float l[kBlockFrames], r[kBlockFrames];
  for (int i = 0; i < blocks; ++i) br.process_block(l, r);
}

// Publishes generations by hand, as the control thread would.
struct Gens {
  BlockRenderer& br;
  std::vector<std::unique_ptr<Snapshot>> all;
  std::uint64_t rev = 0;
  explicit Gens(BlockRenderer& b) : br(b) {}
  Snapshot* publish(const std::array<std::uint16_t, kAssignSlotCount>& map, bool live_set = false, std::uint64_t program_generation = 0) {
    auto s = std::make_unique<Snapshot>(Snapshot::defaults());
    s->ledger_index = static_cast<std::uint32_t>(all.size() % SnapshotExchange::kLedgerSlots);
    s->revision = ++rev;
    s->assign = map;
    s->live_set = live_set;
    s->program_generation = program_generation;
    br.snapshots().publish(s.get());
    all.push_back(std::move(s));
    step_blocks(br);
    return all.back().get();
  }
};

static float live(const BlockRenderer& br, std::uint16_t id, std::uint32_t part = 0) {
  const ParamDef& d = *find(id);
  return br.last_values().load(perf_slot(d, part));
}
static void turn(BlockRenderer& br, std::uint32_t encoder, std::int32_t delta, std::uint32_t frame = 0) { br.submit({make_encoder_step_event(encoder, delta), frame}); }
static void set_mode(BlockRenderer& br, EncoderMode m) { br.submit({make_encoder_mode_event(m), 0}); }

// ---- the registry flag -------------------------------------------------------------------------------------------------------------------
static void test_assignable_flag() {
  int perf = 0, assignable = 0;
  for (const ParamDef& d : kParams) {
    if (d.cls == Class::kStructural) CHECK(!d.assignable);
    else {
      ++perf;
      assignable += d.assignable ? 1 : 0;
    }
  }
  CHECK(perf > 10 && assignable == perf - 1);  // every performance parameter but the device input
  CHECK(!find(kLineInLevel)->assignable);
  CHECK(find(kDrawbar8)->assignable && find(kSendReverbLevel)->assignable && find(kTempoBpm)->assignable && find(kRotarySpeed)->assignable);
}

// ---- apply_relative_step ------------------------------------------------------------------------------------------------------------------
static void test_relative_step_rules() {
  // float: 1/128 of the range per step, clamped.
  const ParamDef& rev = *find(kSendReverbLevel);  // 0..1
  CHECK(apply_relative_step(rev, 0.5f, 1) == 0.5f + 1.0f / 128.0f);
  CHECK(apply_relative_step(rev, 0.5f, -1) == 0.5f - 1.0f / 128.0f);
  CHECK(apply_relative_step(rev, 0.5f, 64) == 1.0f);
  CHECK(apply_relative_step(rev, 0.5f, 127) == 1.0f);
  CHECK(apply_relative_step(rev, 0.5f, -127) == 0.0f);
  CHECK(apply_relative_step(rev, 0.0f, -1) == 0.0f && apply_relative_step(rev, 1.0f, 1) == 1.0f);  // saturation at the ends
  CHECK(apply_relative_step(rev, 0.3f, 0) == 0.3f);
  const ParamDef& tempo = *find(kTempoBpm);  // 20..300: 280/128 bpm per step
  CHECK(std::fabs(apply_relative_step(tempo, 120.0f, 10) - (120.0f + 10 * 280.0f / 128.0f)) < 1e-3f);
  // int: +-1 per step, no wrap
  const ParamDef& bar = *find(kDrawbar8);  // 0..8
  CHECK(apply_relative_step(bar, 3.0f, 1) == 4.0f && apply_relative_step(bar, 3.0f, -1) == 2.0f && apply_relative_step(bar, 3.0f, 5) == 8.0f);
  CHECK(apply_relative_step(bar, 8.0f, 1) == 8.0f && apply_relative_step(bar, 0.0f, -1) == 0.0f);
  CHECK(apply_relative_step(bar, 8.0f, 127) == 8.0f && apply_relative_step(bar, 0.0f, -127) == 0.0f && apply_relative_step(bar, 1.0f, 127) == 8.0f);
  CHECK(apply_relative_step(bar, 2.4f, 0) == 2.4f);       // no step: the live value is kept as it is
  CHECK(apply_relative_step(bar, 2.4f, 1) == 3.0f);       // a step first rounds to a whole step
  // enum: +-1 index per step, clamped, no wrap
  const ParamDef& rot = *find(kRotarySpeed);  // stop, slow, fast
  CHECK(apply_relative_step(rot, 1.0f, 1) == 2.0f && apply_relative_step(rot, 1.0f, -1) == 0.0f);
  CHECK(apply_relative_step(rot, 2.0f, 1) == 2.0f && apply_relative_step(rot, 0.0f, -1) == 0.0f && apply_relative_step(rot, 0.0f, 100) == 2.0f);
  const ParamDef& vib = *find(kVibratoChorusType);  // 6 values
  CHECK(apply_relative_step(vib, 5.0f, 1) == 5.0f && apply_relative_step(vib, 4.0f, 3) == 5.0f && apply_relative_step(vib, 2.0f, -9) == 0.0f);
  // bool: positive -> on, negative -> off
  const ParamDef& on = *find(kRotaryOn);
  CHECK(apply_relative_step(on, 0.0f, 1) == 1.0f && apply_relative_step(on, 0.0f, 127) == 1.0f && apply_relative_step(on, 1.0f, 1) == 1.0f);
  CHECK(apply_relative_step(on, 1.0f, -1) == 0.0f && apply_relative_step(on, 1.0f, -127) == 0.0f && apply_relative_step(on, 0.0f, -1) == 0.0f);
  CHECK(apply_relative_step(on, 1.0f, 0) == 1.0f && apply_relative_step(on, 0.0f, 0) == 0.0f);
  // a non-finite or out-of-range live value is repaired, never propagated
  CHECK(apply_relative_step(bar, std::nanf(""), 0) == 8.0f);  // the registry default
  CHECK(apply_relative_step(bar, std::nanf(""), 1) == 8.0f);
  CHECK(apply_relative_step(rev, INFINITY, 64) == 0.5f);  // INFINITY counts as the default of send_reverb_level (0), then 64 steps = half the range
}

// Exhaustive over every performance parameter: the result stays in range, steps are monotone and deterministic, 0 is identity, a step up and the same step
// down return to the start for whole-step types (when nothing saturates), and the float rule is exactly range/128 per step away from the ends.
static void test_relative_step_exhaustive() {
  std::uint64_t checked = 0;
  for (const ParamDef& d : kParams) {
    if (d.cls != Class::kPerformance) continue;
    std::vector<float> starts = {static_cast<float>(d.min), static_cast<float>(d.max), static_cast<float>(d.def)};
    if (d.type != Type::kFloat) {
      for (double v = d.min; v <= d.max; v += 1.0) starts.push_back(static_cast<float>(v));
    } else {
      for (int k = 1; k < 16; ++k) starts.push_back(static_cast<float>(d.min + (d.max - d.min) * k / 16.0));
    }
    for (float cur : starts) {
      for (std::int32_t delta = -140; delta <= 140; ++delta) {
        const float v = apply_relative_step(d, cur, delta);
        ++checked;
        CHECK(v >= static_cast<float>(d.min) && v <= static_cast<float>(d.max));
        CHECK(v == apply_relative_step(d, cur, delta));  // pure
        if (delta > 0) CHECK(v >= cur);
        if (delta < 0) CHECK(v <= cur);
        if (delta == 0) CHECK(v == cur);
        if (d.type != Type::kFloat) {
          CHECK(v == std::floor(v));
          if (d.type != Type::kBool) {
            const double expect = std::min<double>(d.max, std::max<double>(d.min, cur + delta));
            CHECK(v == static_cast<float>(expect));
            if (cur + delta >= d.min && cur + delta <= d.max) CHECK(apply_relative_step(d, v, -delta) == cur);  // whole steps are reversible
          } else {
            CHECK(v == (delta > 0 ? 1.0f : delta < 0 ? 0.0f : cur));
          }
        } else {
          const double step = (d.max - d.min) / 128.0;
          const double expect = std::min<double>(d.max, std::max<double>(d.min, static_cast<double>(cur) + delta * step));
          CHECK(std::fabs(static_cast<double>(v) - expect) <= 1e-6 * (d.max - d.min));  // a fused multiply-add may differ from the test's own expression in the last bit
        }
      }
    }
  }
  CHECK(checked > 20000);
  // 128 steps sweep a float range from end to end.
  const ParamDef& rev = *find(kSendReverbLevel);
  float v = 0.0f;
  for (int i = 0; i < 128; ++i) v = apply_relative_step(rev, v, 1);
  CHECK(std::fabs(v - 1.0f) < 1e-5f);
  for (int i = 0; i < 200; ++i) v = apply_relative_step(rev, v, 1);
  CHECK(v == 1.0f);
}

// ---- the events ---------------------------------------------------------------------------------------------------------------------------
static void test_event_shapes() {
  for (std::uint32_t enc = 0; enc < kEncoderCount; ++enc) {
    for (std::int32_t delta : {0, 1, -1, 127, -127, 32767, -32768}) {
      const Ump u = make_encoder_step_event(enc, delta);
      std::uint32_t id = 0;
      CHECK(decode_reserved_event_id(u, id) && id == kEncoderStepFirstId + enc);
      CHECK(u.is_assignable_controller() && u.channel() == 15 && u.group() == 0);
      CHECK(static_cast<std::int32_t>(u.word1) == delta);
      ParamEvent pe;
      CHECK(!decode_param_event(u, pe));  // never a parameter
      CHECK(!is_fence_event(u));
    }
  }
  CHECK(static_cast<std::int32_t>(make_encoder_step_event(0, 100000).word1) == 32767);
  CHECK(static_cast<std::int32_t>(make_encoder_step_event(0, -100000).word1) == -32768);
  CHECK(make_encoder_step_event(9, 1).word0 == make_encoder_step_event(1, 1).word0);  // the encoder index is masked to 0..7
  std::uint32_t id = 0;
  CHECK(decode_reserved_event_id(make_encoder_mode_event(EncoderMode::kAssign), id) && id == kEncoderModeEventId);
  CHECK(make_encoder_mode_event(EncoderMode::kAssign).word1 == 2);
  CHECK(decode_reserved_event_id(make_select_part_event(5), id) && id == kSelectPartEventId && make_select_part_event(5).word1 == 5);
  CHECK(decode_reserved_event_id(make_fence_event(7), id) && id == kFenceParamId);
  CHECK(!decode_reserved_event_id(make_param_event(*find(kDrawbar8), 3, 1), id));
  CHECK(!decode_reserved_event_id(make_note_on(0, 15, 60, 1000), id));
  Ump g1 = make_encoder_step_event(0, 1);
  g1.word0 |= 1u << 24;  // group 1
  CHECK(!decode_reserved_event_id(g1, id));
  // The reserved ids never collide with the registry, whatever the registry grows to (the generator refuses ids from kReservedEventFirstId).
  for (const ParamDef& d : kParams) CHECK(d.id < kReservedEventFirstId);
  CHECK(kEncoderStepFirstId + kEncoderCount - 1 < kEncoderModeEventId && kEncoderModeEventId < kSelectPartEventId && kSelectPartEventId < kFenceParamId);
}

// ---- routing --------------------------------------------------------------------------------------------------------------------------------
static std::array<std::uint16_t, kAssignSlotCount> test_map() {
  // K1 int, K2 float, K3 enum, K4 bool, K5 master float, K6 empty, K7 float, K8 part-scope int
  return {kDrawbar8, kSendReverbLevel, kRotarySpeed, kRotaryOn, kTempoBpm, 0, kLeakage, kDrawbar16};
}

static void test_routing_each_type() {
  BlockRenderer br;
  Gens g(br);
  g.publish(test_map(), /*live_set=*/false);  // plain channel == part addressing: every part has an engine behind it
  set_mode(br, EncoderMode::kAssign);
  step_blocks(br);
  CHECK(br.encoder_state().mode() == EncoderMode::kAssign);
  CHECK(live(br, kDrawbar8) == 8.0f);
  turn(br, 0, -3);  // K1: int, 8 -> 5
  turn(br, 1, 16);  // K2: float, 0 -> 16/128
  turn(br, 2, 1);   // K3: enum slow(1) -> fast(2)
  turn(br, 3, 5);   // K4: bool off -> on
  turn(br, 4, 10);  // K5: master float, tempo 120 -> 120 + 10 * 280 / 128
  step_blocks(br);
  CHECK(live(br, kDrawbar8) == 5.0f);
  CHECK(live(br, kSendReverbLevel) == 0.125f);
  CHECK(live(br, kRotarySpeed) == 2.0f);
  CHECK(live(br, kRotaryOn) == 1.0f);
  CHECK(std::fabs(live(br, kTempoBpm) - (120.0f + 10 * 280.0f / 128.0f)) < 1e-3f);
  CHECK(br.tempo_bpm() > 140.0f);
  // Now the other direction and the ends.
  turn(br, 0, 127);   // saturates at 8
  turn(br, 1, -127);  // saturates at 0
  turn(br, 2, 1);     // already fast: stays
  turn(br, 3, -1);    // off
  step_blocks(br);
  CHECK(live(br, kDrawbar8) == 8.0f && live(br, kSendReverbLevel) == 0.0f && live(br, kRotarySpeed) == 2.0f && live(br, kRotaryOn) == 0.0f);
  const EncoderState::Counters c = br.encoder_state().counters();
  CHECK(c.steps_applied == 9 && c.steps_empty_slot == 0 && c.steps_unrouted == 0 && c.steps_rejected == 0);
  CHECK(br.param_events_applied() >= 9);
}

static void test_no_pickup_and_empty_slot() {
  BlockRenderer br;
  Gens g(br);
  g.publish(test_map(), true);
  set_mode(br, EncoderMode::kAssign);
  // The live value is wherever the player (or a scene) left it; the step applies to it directly, with no pickup or catch-up.
  br.submit({make_param_event(*find(kSendReverbLevel), 0, 0.9), 0});
  br.submit({make_param_event(*find(kDrawbar8), 0, 2), 0});
  step_blocks(br);
  turn(br, 1, 1);
  turn(br, 0, 1);
  step_blocks(br);
  CHECK(std::fabs(live(br, kSendReverbLevel) - (0.9f + 1.0f / 128.0f)) < 1e-6f);
  CHECK(live(br, kDrawbar8) == 3.0f);
  // Empty slot (K6): nothing changes, counted.
  std::array<float, kPerfSlotCount> before{};
  for (std::size_t i = 0; i < kPerfSlotCount; ++i) before[i] = br.last_values().load(i);
  const std::uint64_t applied_before = br.param_events_applied();
  turn(br, 5, 40);
  turn(br, 5, -40);
  step_blocks(br);
  for (std::size_t i = 0; i < kPerfSlotCount; ++i) CHECK(before[i] == br.last_values().load(i));
  CHECK(br.param_events_applied() == applied_before);
  CHECK(br.encoder_state().counters().steps_empty_slot == 2);
  // A generation without any map: every slot is empty.
  g.publish({}, true);
  turn(br, 0, 1);
  step_blocks(br);
  CHECK(live(br, kDrawbar8) == 3.0f && br.encoder_state().counters().steps_empty_slot == 3);
}

static void test_mode_gating() {
  BlockRenderer br;
  Gens g(br);
  g.publish(test_map(), false);
  CHECK(br.encoder_state().mode() == EncoderMode::kPart);  // the default
  turn(br, 0, -1);
  set_mode(br, EncoderMode::kAll);
  step_blocks(br);
  turn(br, 0, -1);
  step_blocks(br);
  CHECK(br.encoder_state().mode() == EncoderMode::kAll);
  CHECK(live(br, kDrawbar8) == 8.0f);  // PART and ALL do nothing yet
  CHECK(br.encoder_state().counters().steps_unrouted == 2 && br.encoder_state().counters().steps_applied == 0);
  set_mode(br, EncoderMode::kAssign);
  turn(br, 0, -1);  // applied in the same block, in order after the mode event
  step_blocks(br);
  CHECK(live(br, kDrawbar8) == 7.0f && br.encoder_state().counters().steps_applied == 1);
  set_mode(br, EncoderMode::kPart);  // leaving ASSIGN stops the routing again
  turn(br, 0, -1);
  step_blocks(br);
  CHECK(live(br, kDrawbar8) == 7.0f && br.encoder_state().counters().steps_unrouted == 3);
}

static void test_selected_part_and_scope() {
  BlockRenderer br;
  Gens g(br);
  g.publish(test_map(), true);
  set_mode(br, EncoderMode::kAssign);
  CHECK(br.encoder_state().selected_part() == 0);
  turn(br, 0, -2);  // K1 drawbar_8 on the selected part (0)
  step_blocks(br);
  CHECK(live(br, kDrawbar8, 0) == 6.0f && live(br, kDrawbar8, 3) == 8.0f);
  br.submit({make_select_part_event(3), 0});
  turn(br, 0, -1);
  turn(br, 4, 1);  // K5 tempo: master scope, whatever the selected part
  step_blocks(br);
  CHECK(br.encoder_state().selected_part() == 3);
  CHECK(live(br, kDrawbar8, 3) == 7.0f && live(br, kDrawbar8, 0) == 6.0f);
  CHECK(live(br, kTempoBpm) > 120.0f);
  // A part with no organ or other engine behind it (every part but the test tone's channels) still has its live value updated; the engine ignores it.
  br.submit({make_select_part_event(7), 0});
  turn(br, 0, -4);
  step_blocks(br);
  CHECK(live(br, kDrawbar8, 7) == 4.0f);
  // A select-part event for a part that does not exist is refused and counted.
  br.submit({make_select_part_event(8), 0});
  br.submit({make_select_part_event(200), 0});
  step_blocks(br);
  CHECK(br.encoder_state().selected_part() == 7 && br.encoder_state().counters().events_rejected == 2);
}

static void test_map_follows_the_generation() {
  BlockRenderer br;
  Gens g(br);
  g.publish(test_map(), true);
  set_mode(br, EncoderMode::kAssign);
  turn(br, 0, -1);
  step_blocks(br);
  CHECK(live(br, kDrawbar8) == 7.0f);
  // A Live Set switch: the new generation has another map; K1 now drives key_click, the drawbar is left alone.
  std::array<std::uint16_t, kAssignSlotCount> other = test_map();
  other[0] = kKeyClick;
  g.publish(other, true);
  turn(br, 0, 8);
  step_blocks(br);
  CHECK(live(br, kDrawbar8) == 7.0f);
  CHECK(live(br, kKeyClick) == 0.5f + 8.0f / 128.0f);
  // A scene recall is a generation with a new program_generation that copies the map: encoders keep working through it.
  g.publish(other, true, /*program_generation=*/42);
  turn(br, 0, 8);
  step_blocks(br);
  CHECK(live(br, kKeyClick) == 0.5f + 16.0f / 128.0f);
  CHECK(br.encoder_state().mode() == EncoderMode::kAssign);  // the mode flag is RT state, not part of a generation
}

static void test_refusals() {
  BlockRenderer br;
  Gens g(br);
  std::array<std::uint16_t, kAssignSlotCount> m = test_map();
  m[5] = kLineInLevel;  // registered, performance, but not assignable
  m[6] = 9999;          // not in this registry
  m[7] = kInsertType;   // structural
  g.publish(m, true);
  set_mode(br, EncoderMode::kAssign);
  std::array<float, kPerfSlotCount> before{};
  for (std::size_t i = 0; i < kPerfSlotCount; ++i) before[i] = br.last_values().load(i);
  turn(br, 5, 10);
  turn(br, 6, 10);
  turn(br, 7, 10);
  step_blocks(br);
  for (std::size_t i = 0; i < kPerfSlotCount; ++i) CHECK(before[i] == br.last_values().load(i));
  CHECK(br.encoder_state().counters().steps_rejected == 3 && br.encoder_state().counters().steps_applied == 0);
  // Malformed events: a delta outside int16, a mode that does not exist, an unknown reserved id.
  br.submit({Ump{make_encoder_step_event(0, 1).word0, 100000u}, 0});
  br.submit({Ump{make_encoder_step_event(0, 1).word0, 0x80000000u}, 0});
  br.submit({Ump{make_encoder_mode_event(EncoderMode::kAssign).word0, 3u}, 0});
  const std::uint64_t pr = br.param_events_rejected();
  Ump unknown = make_select_part_event(0);
  unknown.word0 = (unknown.word0 & ~0x7F7Fu) | (((0x3FFE >> 7) & 0x7Fu) << 8) | (0x3FFE & 0x7Fu);
  br.submit({unknown, 0});
  step_blocks(br);
  CHECK(br.encoder_state().counters().steps_rejected == 5);
  CHECK(br.encoder_state().counters().events_rejected == 1);
  CHECK(br.param_events_rejected() == pr + 1);
  CHECK(br.encoder_state().mode() == EncoderMode::kAssign);
  CHECK(live(br, kDrawbar8) == 8.0f);
  // The fence still works on the same channel.
  br.submit({make_fence_event(77), 0});
  step_blocks(br);
  CHECK(br.last_values().load_fence_ack() == 77);
}

static void test_timing_and_order_inside_a_block() {
  BlockRenderer br;
  Gens g(br);
  g.publish(test_map(), true);
  set_mode(br, EncoderMode::kAssign);
  // Steps are events like any other: frame order, ties in arrival order, deferral to later blocks.
  turn(br, 0, -1, 10);
  br.submit({make_param_event(*find(kDrawbar8), 0, 6), 20});  // a CC-like set in between
  turn(br, 0, -1, 30);
  turn(br, 0, 3, 64 + 5);  // next block
  step_blocks(br);
  CHECK(live(br, kDrawbar8) == 5.0f);  // 8 -> 7 -> set 6 -> 5
  step_blocks(br);
  CHECK(live(br, kDrawbar8) == 8.0f);
}

// A storm of steps and direction changes is deterministic: two renderers fed the same events agree bit for bit; large steps saturate cleanly.
static void test_storm_determinism() {
  auto run = [](std::vector<float>& out) {
    BlockRenderer br;
    Gens g(br);
    g.publish(test_map(), true);
    set_mode(br, EncoderMode::kAssign);
    std::uint64_t rng = 99;
    for (int i = 0; i < 4000; ++i) {
      rng = rng * 6364136223846793005ull + 1442695040888963407ull;
      const std::uint32_t r = static_cast<std::uint32_t>(rng >> 33);
      const std::int32_t delta = static_cast<std::int32_t>(r % 255) - 127;
      turn(br, r % 8, delta, (r >> 8) % 64);
      if (i % 50 == 0) br.submit({make_select_part_event((r >> 4) % 8), 0});
      if (i % 3 == 0) step_blocks(br);
    }
    step_blocks(br, 2);
    for (std::size_t s = 0; s < kPerfSlotCount; ++s) out.push_back(br.last_values().load(s));
    const EncoderState::Counters c = br.encoder_state().counters();
    CHECK(c.steps_applied + c.steps_empty_slot + c.steps_no_part == 4000);
    for (const ParamDef& d : kParams) {
      if (d.cls != Class::kPerformance) continue;
      for (std::uint32_t p = 0; p < (d.scope == Scope::kPart ? kPartCount : 1u); ++p) {
        const float v = br.last_values().load(perf_slot(d, p));
        CHECK(v >= static_cast<float>(d.min) && v <= static_cast<float>(d.max));
      }
    }
  };
  std::vector<float> a, b;
  run(a);
  run(b);
  CHECK(a == b);
}

// A step in ASSIGN mode before any generation exists: no crash, an empty slot.
static void test_no_generation_yet() {
  BlockRenderer br;
  CHECK(br.held_snapshot() == nullptr);
  set_mode(br, EncoderMode::kAssign);
  turn(br, 0, 5);
  step_blocks(br);
  CHECK(br.held_snapshot() == nullptr);
  const EncoderState::Counters c = br.encoder_state().counters();
  CHECK(c.steps_empty_slot == 1 && c.steps_applied == 0 && c.steps_rejected == 0 && c.steps_no_part == 0);
}

// The single assignability rule, on the registry and on synthetic definitions the registry cannot hold (a bad last-value index or scope never reaches
// perf_slot(), a whole-step range a float cannot hold exactly is not assignable, an id is never narrowed to 16 bits).
static void test_assignability_rule() {
  CHECK(find_assignable(kDrawbar8) == find(kDrawbar8) && find_assignable(kTempoBpm) && find_assignable(kRotarySpeed));
  CHECK(!find_assignable(kLineInLevel) && !find_assignable(kInsertType) && !find_assignable(0) && !find_assignable(9999));
  CHECK(!find_assignable(0x10000u + kDrawbar8) && !find_assignable(0x10001u) && !find_assignable(0xFFFFFFFFu));
  ParamDef d = *find(kDrawbar8);
  CHECK(is_assignable(d));
  ParamDef x = d;
  x.perf_index = -1;
  CHECK(!is_assignable(x));
  x = d;
  x.scope = Scope::kInsert;
  CHECK(!is_assignable(x));
  x = d;
  x.cls = Class::kStructural;
  CHECK(!is_assignable(x));
  x = d;
  x.assignable = false;
  CHECK(!is_assignable(x));
  x = d;
  x.type = Type::kUint;
  x.min = 0;
  x.max = 16777216.0;
  CHECK(is_assignable(x));
  x.max = 16777217.0;
  CHECK(!is_assignable(x));
  x.type = Type::kFloat;  // a float range is not limited
  CHECK(is_assignable(x));
  // Whole steps near the top of the largest exact range: +1 does not vanish, the clamp holds.
  ParamDef big = d;
  big.type = Type::kUint;
  big.min = 0;
  big.max = 16777216.0;
  CHECK(apply_relative_step(big, 16777214.0f, 1) == 16777215.0f && apply_relative_step(big, 16777215.0f, 1) == 16777216.0f);
  CHECK(apply_relative_step(big, 16777216.0f, 5) == 16777216.0f && apply_relative_step(big, 3.0f, -100) == 0.0f);
}

// Channel 15 below the reserved ids is not a control event: it is refused as a parameter event (part 15 does not exist), counted exactly once.
static void test_low_ids_on_channel_15() {
  BlockRenderer br;
  Gens g(br);
  g.publish(test_map(), false);
  std::uint32_t id = 0;
  Ump low = make_param_event(*find(kDrawbar8), 0, 1);
  low.word0 |= 15u << 16;
  CHECK(!decode_reserved_event_id(low, id));
  Ump master = make_param_event(*find(kTempoBpm), 0, 200);
  master.word0 |= 15u << 16;
  CHECK(!decode_reserved_event_id(master, id));
  const std::uint64_t before = br.param_events_rejected();
  br.submit({low, 0});
  br.submit({master, 0});
  step_blocks(br);
  CHECK(br.param_events_rejected() == before + 2);
  CHECK(live(br, kDrawbar8, 0) == 8.0f && live(br, kTempoBpm) == 120.0f);
  const EncoderState::Counters c = br.encoder_state().counters();
  CHECK(c.steps_rejected == 0 && c.events_rejected == 0);
  // The lowest reserved id and one below it.
  Ump edge = make_select_part_event(0);
  edge.word0 = (edge.word0 & ~0x7F7Fu) | (((kReservedEventFirstId >> 7) & 0x7Fu) << 8) | (kReservedEventFirstId & 0x7Fu);
  CHECK(decode_reserved_event_id(edge, id) && id == kReservedEventFirstId);
  edge.word0 = (edge.word0 & ~0x7F7Fu) | (((kReservedEventFirstId - 1) >> 7 & 0x7Fu) << 8) | ((kReservedEventFirstId - 1) & 0x7Fu);
  CHECK(!decode_reserved_event_id(edge, id));
}

// A part-scope step whose part has no engine (a Live Set generation with a silent part) still moves the live value but is counted apart.
static void test_step_on_a_part_without_engine() {
  BlockRenderer br;
  Gens g(br);
  g.publish(test_map(), /*live_set=*/true);  // default parts: engine type 0, nothing behind any part
  set_mode(br, EncoderMode::kAssign);
  turn(br, 0, -2);  // part scope
  turn(br, 4, 1);   // master scope: no part needed
  step_blocks(br);
  CHECK(live(br, kDrawbar8, 0) == 6.0f && live(br, kTempoBpm) > 120.0f);
  const EncoderState::Counters c = br.encoder_state().counters();
  CHECK(c.steps_no_part == 1 && c.steps_applied == 1 && c.steps_empty_slot == 0);
}

int main() {
  test_assignable_flag();
  test_relative_step_rules();
  test_relative_step_exhaustive();
  test_event_shapes();
  test_routing_each_type();
  test_no_pickup_and_empty_slot();
  test_mode_gating();
  test_selected_part_and_scope();
  test_map_follows_the_generation();
  test_refusals();
  test_timing_and_order_inside_a_block();
  test_storm_determinism();
  test_no_generation_yet();
  test_assignability_rule();
  test_low_ids_on_channel_15();
  test_step_on_a_part_without_engine();
  if (g_fail) std::printf("assign_tests: %d failure(s)\n", g_fail);
  else std::printf("assign_tests: ok\n");
  return g_fail ? 1 : 0;
}
