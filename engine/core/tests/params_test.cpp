// Story 2.1: generated registry, parameter UMP mapping, last-value table, snapshot adoption and the
// generation lifetime rule (a held note across a swap) in the block renderer.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/snapshot.hpp"

using namespace velora::core;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static void step(BlockRenderer& br, int blocks = 1) {
  float l[kBlockFrames], r[kBlockFrames];
  for (int i = 0; i < blocks; ++i) br.process_block(l, r);
}

static std::unique_ptr<Snapshot> make_snapshot(std::uint64_t rev, std::uint32_t ledger) {
  auto s = std::make_unique<Snapshot>(Snapshot::defaults());
  s->revision = rev;
  s->ledger_index = ledger;
  return s;
}

static void test_registry() {
  using namespace params;
  std::uint32_t prev = 0;
  for (const ParamDef& d : kParams) {
    CHECK(d.id > prev);  // sorted, unique
    prev = d.id;
    CHECK(d.id < 16384);
    CHECK(d.min < d.max && d.def >= d.min && d.def <= d.max);
    CHECK(find(d.id) == &d);
  }
  CHECK(find(99) == nullptr);  // reserved ids are never registered
  CHECK(find(kDrawbar16) && find(kDrawbar16)->cls == Class::kPerformance);
  CHECK(find(kInsertType)->cls == Class::kStructural && find(kMasterEqLowGain)->scope == Scope::kMaster);
  CHECK(find_by_cc(70) == find(kDrawbar16));
  CHECK(find_by_cc(64) == nullptr);
  CHECK(kPartCount == 8 && kInsertSlotCount == 5);
}

static void test_event_mapping() {
  using namespace params;
  for (const ParamDef& d : kParams) {
    if (d.cls != Class::kPerformance) continue;
    for (std::uint32_t part = 0; part < (d.scope == Scope::kPart ? kPartCount : 1u); ++part) {
      for (double v : {d.min, d.max, d.def, (d.min + d.max) / 2}) {
        ParamEvent pe;
        const Ump u = make_param_event(d, part, v);
        CHECK(u.is_assignable_controller() && u.channel() == part && u.group() == 0);
        CHECK(decode_param_event(u, pe) && pe.def == &d && pe.part == part);
        const double expect = d.type == Type::kFloat ? v : std::floor(v + 0.5);
        CHECK(std::fabs(pe.value - expect) <= 1e-4 * (1 + std::fabs(expect)));
      }
    }
  }
  ParamEvent pe;
  CHECK(!decode_param_event(make_note_on(0, 0, 60, 1000), pe));
  Ump bad = make_param_event(*find(kDrawbar16), 0, 1);
  bad.word0 |= (3u << 16);
  CHECK(decode_param_event(bad, pe));
  bad = make_param_event(*find(kDrawbar16), 9, 1);  // part 9 does not exist
  CHECK(!decode_param_event(bad, pe));
  bad = make_param_event(*find(kInsertType), 0, 1);  // structural parameters never travel as UMP
  CHECK(!decode_param_event(bad, pe));
  bad = Ump{make_param_event(*find(kDrawbar16), 0, 1).word0 | 0x7F, 0};  // id 127 is unregistered
  CHECK(!decode_param_event(bad, pe));
  Ump out;
  CHECK(cc_to_param_event(make_control_change(0, 2, 70, 0xFFFFFFFFu), out));
  CHECK(decode_param_event(out, pe) && pe.def->id == kDrawbar16 && pe.part == 2 && pe.value == 8.0f);
  CHECK(!cc_to_param_event(make_control_change(0, 2, 64, 1), out));
  CHECK(!cc_to_param_event(make_control_change(0, 9, 70, 1), out));
}

static void test_last_value_table() {
  using namespace params;
  BlockRenderer br;
  const ParamDef& d = *find(kDrawbar4);
  CHECK(br.last_values().load(perf_slot(d, 3)) == 0.0f);
  CHECK(br.last_values().load(perf_slot(*find(kDrawbar8), 3)) == 8.0f);  // registry default
  br.submit({make_param_event(d, 3, 7), 10});
  br.submit({make_param_event(d, 4, 2), 20});
  br.submit({make_param_event(*find(kInsertType), 0, 1), 20});  // refused
  step(br);
  CHECK(br.last_values().load(perf_slot(d, 3)) == 7.0f);
  CHECK(br.last_values().load(perf_slot(d, 4)) == 2.0f);
  CHECK(br.last_values().load(perf_slot(d, 0)) == 0.0f);
  CHECK(br.param_events_applied() == 2 && br.param_events_rejected() == 1);
  // A timestamp beyond the block is deferred, and applies when its frame comes.
  br.submit({make_param_event(d, 3, 1), 64 + 5});
  step(br);
  CHECK(br.last_values().load(perf_slot(d, 3)) == 7.0f);
  step(br);
  CHECK(br.last_values().load(perf_slot(d, 3)) == 1.0f);
}

static void test_initial_values_apply_on_generation_change_only() {
  using namespace params;
  BlockRenderer br;
  const ParamDef& d = *find(kDrawbar2);
  const std::size_t slot = perf_slot(d, 1);
  auto s1 = make_snapshot(1, 0);
  s1->perf_init[slot] = 5;
  br.snapshots().publish(s1.get());
  step(br);
  CHECK(br.held_snapshot() == s1.get() && br.last_values().load(slot) == 5.0f);
  br.submit({make_param_event(d, 1, 3), 0});
  step(br);
  CHECK(br.last_values().load(slot) == 3.0f);
  auto s2 = make_snapshot(2, 1);  // initial value unchanged vs generation 1 -> live value kept
  s2->perf_init[slot] = 5;
  br.snapshots().publish(s2.get());
  step(br);
  CHECK(br.held_snapshot() == s2.get() && br.last_values().load(slot) == 3.0f);
  auto s3 = make_snapshot(3, 2);  // changed initial value -> applied on this generation change
  s3->perf_init[slot] = 7;
  br.snapshots().publish(s3.get());
  CHECK(br.last_values().load(slot) == 3.0f);  // not before RT adopts it at a block start
  step(br);
  CHECK(br.last_values().load(slot) == 7.0f);
}

static void test_held_note_keeps_its_generation() {
  BlockRenderer br;
  auto g1 = make_snapshot(1, 0);
  auto g2 = make_snapshot(2, 1);
  auto g3 = make_snapshot(3, 2);
  auto& ex = br.snapshots();
  ex.publish(g1.get());
  step(br);
  br.submit({make_note_on(0, 0, 60, 40000), 0});
  step(br);
  CHECK(ex.refs(0) == 1);
  ex.publish(g2.get());
  step(br, 4);
  CHECK(br.held_snapshot() == g2.get() && ex.rt_held() == g2.get());
  CHECK(ex.refs(0) == 1 && !ex.safe_to_free(g1.get()));  // note still sounding on generation 1
  ex.publish(g3.get());
  step(br, 4);
  CHECK(!ex.safe_to_free(g1.get()));
  CHECK(ex.safe_to_free(g2.get()));  // no voice ever used generation 2
  br.submit({make_note_off(0, 0, 60), 40});
  step(br, 1);
  CHECK(!ex.safe_to_free(g1.get()));  // releasing: the voice is still there
  step(br, 4);                        // 48-frame release is over
  CHECK(ex.refs(0) == 0 && ex.safe_to_free(g1.get()));
  CHECK(!ex.safe_to_free(g3.get()));  // current
}

static void test_new_note_binds_current_generation() {
  BlockRenderer br;
  auto g1 = make_snapshot(1, 0);
  auto g2 = make_snapshot(2, 1);
  auto& ex = br.snapshots();
  ex.publish(g1.get());
  step(br);
  ex.publish(g2.get());
  step(br);
  br.submit({make_note_on(0, 0, 62, 40000), 0});
  step(br);
  CHECK(ex.refs(0) == 0 && ex.refs(1) == 1);
  CHECK(ex.safe_to_free(g1.get()));
}

// Hazard clause in isolation: RT holds generation N, no voice references it, a newer one is published but RT
// has not run a block yet. N must not be freeable until RT has moved on.
static void test_hazard_protects_held_generation() {
  BlockRenderer br;
  auto n = make_snapshot(1, 0);
  auto n1 = make_snapshot(2, 1);
  auto& ex = br.snapshots();
  ex.publish(n.get());
  step(br);
  CHECK(ex.rt_held() == n.get() && ex.refs(0) == 0);
  ex.publish(n1.get());
  CHECK(ex.rt_held() == n.get());
  CHECK(!ex.safe_to_free(n.get()));
  step(br);
  CHECK(ex.rt_held() == n1.get() && ex.safe_to_free(n.get()));
}

// Two quick publishes: the previous generation is freed by the control side as soon as RT moves on, and the
// next adoption must not read it (the diff is against an RT-owned copy).
static void test_adoption_never_reads_previous_generation() {
  using namespace params;
  BlockRenderer br;
  const std::size_t slot = perf_slot(*find(kDrawbar2), 1);
  auto g1 = make_snapshot(1, 0);
  g1->perf_init[slot] = 5;
  br.snapshots().publish(g1.get());
  step(br);
  auto g2 = make_snapshot(2, 1);
  g2->perf_init[slot] = 5;
  auto g3 = make_snapshot(3, 2);
  g3->perf_init[slot] = 7;
  br.snapshots().publish(g2.get());
  step(br);
  CHECK(br.snapshots().safe_to_free(g1.get()));
  const std::uint32_t poison = 0xDEADBEEFu;
  std::memcpy(&g1->perf_init[slot], &poison, sizeof poison);  // "freed": must never be read again
  br.snapshots().publish(g3.get());
  br.snapshots().publish(g3.get());
  step(br);
  CHECK(br.last_values().load(slot) == 7.0f);
  auto g4 = make_snapshot(4, 3);  // quick publishes: g4 then g5 before one block
  g4->perf_init[slot] = 2;
  auto g5 = make_snapshot(5, 4);
  g5->perf_init[slot] = 7;  // equals what was applied: live value stays
  br.snapshots().publish(g4.get());
  br.snapshots().publish(g5.get());
  step(br);
  CHECK(br.held_snapshot() == g5.get() && br.last_values().load(slot) == 7.0f);
}

static void test_test_tone_reports_started_notes() {
  BlockRenderer br;
  br.submit({make_note_on(0, 3, 64, 40000), 0});
  br.submit({make_note_on(0, 4, 65, 40000), 0});
  step(br);
  int found = 0, other = 0;
  VoiceInfo vi;
  Engine* e = br.registry().at(0);
  for (std::uint32_t i = 0; i < e->voice_capacity(); ++i) {
    if (!e->voice_info(i, vi)) continue;
    if ((vi.channel == 3 && vi.note == 64) || (vi.channel == 4 && vi.note == 65)) ++found;
    else ++other;
  }
  CHECK(found == 2 && other == 0);
}

int main() {
  test_hazard_protects_held_generation();
  test_adoption_never_reads_previous_generation();
  test_test_tone_reports_started_notes();
  test_registry();
  test_event_mapping();
  test_last_value_table();
  test_initial_values_apply_on_generation_change_only();
  test_held_note_keeps_its_generation();
  test_new_note_binds_current_generation();
  if (g_fail) std::printf("params_tests: %d failure(s)\n", g_fail);
  else std::printf("params_tests: ok\n");
  return g_fail ? 1 : 0;
}
