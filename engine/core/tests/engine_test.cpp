#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "velora/core/block_renderer.hpp"
#include "velora/core/spsc_queue.hpp"

using namespace velora::core;

static int g_fail = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    if (!(c)) {                                                          \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
      ++g_fail;                                                          \
    }                                                                    \
  } while (0)

struct Block {
  float l[kBlockFrames];
  float r[kBlockFrames];
};

static Block render(BlockRenderer& br) {
  Block b;
  br.process_block(b.l, b.r);
  return b;
}

static void test_offset_onset() {
  auto br = std::make_unique<BlockRenderer>();
  CHECK(br->submit({make_note_on(0, 0, 69, 0xFFFF), 37}));
  Block b = render(*br);
  bool silent = true;
  for (int i = 0; i < 37; ++i) silent = silent && b.l[i] == 0.0f && b.r[i] == 0.0f;
  CHECK(silent);
  CHECK(b.l[37] != 0.0f && b.r[37] != 0.0f);
  CHECK(br->status_count(EngineStatus::kOk) == 1);
}

static void test_offset_release() {
  const std::uint32_t k = 20;
  auto a = std::make_unique<BlockRenderer>();
  auto b = std::make_unique<BlockRenderer>();
  a->submit({make_note_on(0, 0, 60, 0xFFFF), 0});
  b->submit({make_note_on(0, 0, 60, 0xFFFF), 0});
  render(*a);
  render(*b);  // both held, identical
  b->submit({make_note_off(0, 0, 60), k});
  Block ra = render(*a), rb = render(*b);
  for (std::uint32_t i = 0; i < k; ++i) CHECK(ra.l[i] == rb.l[i]);
  CHECK(ra.l[k] != rb.l[k]);  // release begins exactly at frame k
  // Voice ends after the release time.
  for (int i = 0; i < 3; ++i) render(*b);
  CHECK(b->test_tone().active_voices() == 0);
}

static void test_event_order() {
  auto br = std::make_unique<BlockRenderer>();
  // Submitted out of frame order, plus a same-frame tie (on then off for note 62).
  br->submit({make_note_on(0, 0, 60, 0xFFFF), 50});
  br->submit({make_note_on(0, 0, 62, 0xFFFF), 10});
  br->submit({make_note_off(0, 0, 62), 10});
  Block b = render(*br);
  for (int i = 0; i < 10; ++i) CHECK(b.l[i] == 0.0f);
  CHECK(b.l[50] != 0.0f);
  CHECK(br->test_tone().active_voices() == 1);  // 62: on then off at the same frame (arrival order)
  CHECK(br->status_count(EngineStatus::kOk) == 3);
  CHECK(br->status_count(EngineStatus::kVoiceNotFound) == 0);
  // Reversed tie: off before on -> off finds nothing.
  auto c = std::make_unique<BlockRenderer>();
  c->submit({make_note_off(0, 0, 62), 10});
  c->submit({make_note_on(0, 0, 62, 0xFFFF), 10});
  render(*c);
  CHECK(c->status_count(EngineStatus::kVoiceNotFound) == 1);
}

static void test_event_beyond_block() {
  auto br = std::make_unique<BlockRenderer>();
  br->submit({make_note_on(0, 0, 69, 0xFFFF), 64 + 5});
  Block b0 = render(*br);
  for (int i = 0; i < 64; ++i) CHECK(b0.l[i] == 0.0f);
  CHECK(br->status_count(EngineStatus::kOk) == 0);
  Block b1 = render(*br);
  for (int i = 0; i < 5; ++i) CHECK(b1.l[i] == 0.0f);
  CHECK(b1.l[5] != 0.0f);
  CHECK(br->status_count(EngineStatus::kOk) == 1);
}

static void test_reserved_and_unknown() {
  auto br = std::make_unique<BlockRenderer>();
  br->set_channel_engine(1, static_cast<std::uint32_t>(EngineTypeId::kFm));
  br->set_channel_engine(2, static_cast<std::uint32_t>(EngineTypeId::kVa));
  br->set_channel_engine(3, 999);
  br->submit({make_note_on(0, 1, 60, 0xFFFF), 0});
  Block b = render(*br);
  for (int i = 0; i < 64; ++i) CHECK(b.l[i] == 0.0f && b.r[i] == 0.0f);
  CHECK(br->last_status() == EngineStatus::kEngineNotAvailable);
  br->submit({make_note_on(0, 2, 60, 0xFFFF), 0});
  b = render(*br);
  CHECK(br->status_count(EngineStatus::kEngineNotAvailable) == 2);
  CHECK(br->test_tone().active_voices() == 0);  // no other engine served it
  br->submit({make_note_on(0, 3, 60, 0xFFFF), 0});
  render(*br);
  CHECK(br->last_status() == EngineStatus::kEngineUnknown);
  CHECK(br->test_tone().active_voices() == 0);

  CHECK(static_cast<std::uint32_t>(EngineTypeId::kSample) == 1);
  CHECK(static_cast<std::uint32_t>(EngineTypeId::kOrgan) == 2);
  CHECK(static_cast<std::uint32_t>(EngineTypeId::kFm) == 3);
  CHECK(static_cast<std::uint32_t>(EngineTypeId::kVa) == 4);
}

static void test_queue_full() {
  SpscQueue<int, 4> q;
  for (int i = 0; i < 4; ++i) CHECK(q.push(i));
  CHECK(!q.push(99));
  int v = -1;
  for (int i = 0; i < 4; ++i) {
    CHECK(q.pop(v));
    CHECK(v == i);  // nothing lost
  }
  CHECK(!q.pop(v));
  CHECK(q.push(7) && q.pop(v) && v == 7);

  auto br = std::make_unique<BlockRenderer>();
  std::size_t accepted = 0;
  for (std::size_t i = 0; i < BlockRenderer::kQueueCapacity + 10; ++i)
    accepted += br->submit({make_note_off(0, 0, 1), 0}) ? 1 : 0;
  CHECK(accepted == BlockRenderer::kQueueCapacity);
}

static void test_pool_exhausted() {
  auto br = std::make_unique<BlockRenderer>(2);
  br->submit({make_note_on(0, 0, 60, 0xFFFF), 0});
  br->submit({make_note_on(0, 0, 62, 0xFFFF), 1});
  br->submit({make_note_on(0, 0, 64, 0xFFFF), 2});
  render(*br);
  CHECK(br->status_count(EngineStatus::kPoolExhausted) == 1);
  CHECK(br->test_tone().active_voices() == 2);
  // The dropped note is the newest (64): releasing it finds nothing.
  br->submit({make_note_off(0, 0, 64), 0});
  render(*br);
  CHECK(br->last_status() == EngineStatus::kVoiceNotFound);
}

static void test_metadata() {
  auto br = std::make_unique<BlockRenderer>();
  CHECK(br->test_tone().cost_units_per_voice() == 1);
  br->submit({make_note_on(0, 0, 60, 0xFFFF), 3});
  render(*br);
  VoiceInfo vi;
  CHECK(br->test_tone().voice_info(0, vi));
  CHECK(vi.start_time == 3 && vi.state == VoiceState::kHeld && vi.level > 0.0f);
  br->submit({make_note_off(0, 0, 60), 40});
  render(*br);
  CHECK(br->test_tone().voice_info(0, vi) && vi.state == VoiceState::kReleasing);
}

static void test_gain_param() {
  auto a = std::make_unique<BlockRenderer>();
  auto b = std::make_unique<BlockRenderer>();
  b->test_tone().set_param(TestToneEngine::kParamGain, 0.5f);
  a->submit({make_note_on(0, 0, 69, 0xFFFF), 0});
  b->submit({make_note_on(0, 0, 69, 0xFFFF), 0});
  Block ra = render(*a), rb = render(*b);
  CHECK(ra.l[30] != 0.0f);
  CHECK(std::fabs(rb.l[30] - 0.5f * ra.l[30]) < 1e-6f);
  // Non-finite gain is rejected.
  b->test_tone().set_param(TestToneEngine::kParamGain, std::nanf(""));
  rb = render(*b);
  CHECK(std::isfinite(rb.l[10]));
}

static void test_ignored_events() {
  auto br = std::make_unique<BlockRenderer>();
  br->submit({Ump{(0x2u << 28) | (0x9u << 20), 0xFFFF0000u}, 0});            // not message type 4
  br->submit({Ump{(0x4u << 28) | (0xBu << 20), 0}, 1});                       // control change
  br->submit({make_note_on(1, 0, 60, 0xFFFF), 2});                            // group 1
  render(*br);
  CHECK(br->ignored_events() == 3);
  CHECK(br->test_tone().active_voices() == 0);
  CHECK(br->status_count(EngineStatus::kOk) == 0);
}

static void test_arena() {
  Arena a(64);
  CHECK(a.allocate(16, 0) == nullptr);
  CHECK(a.allocate(16, 3) == nullptr);
  CHECK(a.allocate(1000) == nullptr);
  CHECK(a.allocate(~std::size_t{0}) == nullptr);
  CHECK(a.allocate_array<std::uint64_t>(~std::size_t{0} / 4) == nullptr);
  CHECK(a.allocate(32) != nullptr);
  CHECK(a.allocate(64) == nullptr);
  a.reset();
  CHECK(a.used() == 0 && a.allocate(64, 1) != nullptr);
}

static void test_zero_capacity() {
  auto br = std::make_unique<BlockRenderer>(100000);
  CHECK(br->test_tone().voice_capacity() == 0);
  br->submit({make_note_on(0, 0, 60, 0xFFFF), 0});
  Block b = render(*br);
  CHECK(br->last_status() == EngineStatus::kPoolExhausted);
  CHECK(b.l[5] == 0.0f);
}

static void test_deferred_order_and_overflow() {
  {
    auto br = std::make_unique<BlockRenderer>(256);
    for (std::uint32_t i = 0; i < 200; ++i)  // descending frames, all deferred
      br->submit({make_note_on(0, 0, i % 128, 0xFFFF), 64 + (199 - i)});
    for (int i = 0; i < 5; ++i) render(*br);
    CHECK(br->status_count(EngineStatus::kOk) == 200);
    bool ordered = true;
    for (std::uint32_t slot = 0; slot < 200; ++slot) {
      VoiceInfo vi;
      ordered = ordered && br->test_tone().voice_info(slot, vi) && vi.start_time == 64 + slot;
    }
    CHECK(ordered);  // slot order == frame order
  }
  {
    auto br = std::make_unique<BlockRenderer>(256);
    for (std::uint32_t i = 0; i < 300; ++i) br->submit({make_note_on(0, 0, 60, 0xFFFF), 64 + i});
    render(*br);
    CHECK(br->status_count(EngineStatus::kEventOverflow) == 300 - BlockRenderer::kPendingCapacity);
    CHECK(br->submit({make_note_off(0, 0, 60), 0}));  // queue was drained
  }
}

static void test_detune_param() {
  auto run = [](float cents, bool second_note, bool set, Block& b2) {
    auto br = std::make_unique<BlockRenderer>();
    br->submit({make_note_on(0, 0, 60, 0xFFFF), 0});
    render(*br);
    if (set) br->test_tone().set_param(TestToneEngine::kParamDetuneCents, cents);
    if (second_note) br->submit({make_note_on(0, 0, 64, 0xFFFF), 0});
    b2 = render(*br);
  };
  auto same = [](const Block& x, const Block& y) {
    return std::memcmp(x.l, y.l, sizeof x.l) == 0 && std::memcmp(x.r, y.r, sizeof x.r) == 0;
  };
  Block base, early, plain2, det2, big, mx, nan, inf;
  run(0, false, false, base);
  run(1200, false, true, early);  // already-running voice unaffected
  CHECK(same(base, early));
  run(0, true, false, plain2);
  run(1200, true, true, det2);  // later voice detuned
  CHECK(!same(plain2, det2));
  run(1e6f, true, true, big);
  run(4800, true, true, mx);
  CHECK(same(big, mx));  // clamped to +-4800
  run(std::nanf(""), true, true, nan);
  run(INFINITY, true, true, inf);
  CHECK(same(nan, plain2) && same(inf, plain2));  // non-finite ignored
  for (int i = 0; i < 64; ++i) CHECK(std::isfinite(big.l[i]));
}

int main() {
  test_offset_onset();
  test_offset_release();
  test_event_order();
  test_event_beyond_block();
  test_reserved_and_unknown();
  test_queue_full();
  test_pool_exhausted();
  test_metadata();
  test_gain_param();
  test_ignored_events();
  test_arena();
  test_zero_capacity();
  test_deferred_order_and_overflow();
  test_detune_param();
  std::printf(g_fail ? "engine_tests: %d failures\n" : "engine_tests: ok\n", g_fail);
  return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
