// Portable (all-platform) tests: time mapper rule, UMP normalisation, host event queue and the
// shared render path. No audio or MIDI hardware needed.
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/host/block_slicer.hpp"
#include "velora/host/midi_ingest.hpp"
#include "velora/host/render_driver.hpp"
#include "velora/host/time_mapper.hpp"

using namespace velora;
using namespace velora::host;

static int g_fail = 0;
#define CHECK(c)                                                          \
  do {                                                                    \
    if (!(c)) {                                                           \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);            \
      ++g_fail;                                                           \
    }                                                                     \
  } while (0)

static void test_mapper() {
  TimeMapperConfig cfg;  // 48k, 64, 3 ms, horizon 4800
  TimeMapper m(cfg);
  const std::uint64_t bs = 10'000'000'000ull;
  const auto at = [&](std::uint64_t due_off_frames) {  // event whose due time is block start + N frames
    return m.map(bs + frames_to_ns(due_off_frames, 48000) - cfg.delay_ns, bs);
  };
  CHECK(at(0).frame == 0 && at(0).timing == EventTiming::kInBlock);
  CHECK(at(20).frame == 20 && at(20).timing == EventTiming::kInBlock);
  CHECK(at(63).frame == 63 && at(63).timing == EventTiming::kInBlock);
  CHECK(at(64).frame == 64 && at(64).timing == EventTiming::kDeferred);
  CHECK(at(1000).frame == 1000 && at(1000).timing == EventTiming::kDeferred);
  CHECK(at(4799).timing == EventTiming::kDeferred);
  CHECK(at(4800).timing == EventTiming::kBeyondHorizon);
  CHECK(at(10'000'000).timing == EventTiming::kBeyondHorizon);
  // Late: due one ns before the block start.
  auto late = m.map(bs - cfg.delay_ns - 1, bs);
  CHECK(late.timing == EventTiming::kLate && late.frame == 0);
  auto very_late = m.map(0, bs);
  CHECK(very_late.timing == EventTiming::kLate && very_late.frame == 0);
  // Far-future timestamp must not overflow.
  CHECK(m.map(~0ull - cfg.delay_ns, bs).timing == EventTiming::kBeyondHorizon);
  CHECK(m.map(~0ull, bs).timing == EventTiming::kBeyondHorizon);  // event_ns + delay would wrap
  // Degenerate config is sanitised (no divide by zero / underflow).
  TimeMapper bad(TimeMapperConfig{0, 0, 0, 0});
  CHECK(bad.config().sample_rate == 48000 && bad.config().block_frames == 64 &&
        bad.config().horizon_frames > bad.config().block_frames);
  CHECK(bad.map(bs, bs).timing == EventTiming::kInBlock);
  // Monotone: later timestamps never map earlier.
  std::uint32_t prev = 0;
  for (std::uint64_t t = bs - 5'000'000; t < bs + 20'000'000; t += 7919) {
    const auto r = m.map(t, bs);
    CHECK(r.frame >= prev);
    prev = r.frame;
  }
}

static void test_normalise() {
  core::Ump u;
  std::size_t used = 0;
  // MIDI 1.0 note-on ch3 note 60 vel 127
  const std::uint32_t on1[] = {0x2093'3C7Fu};
  CHECK(normalise_packet(on1, 1, used, u) && used == 1 && u.is_note_on() && u.channel() == 3 &&
        u.note() == 60 && u.velocity16() == 65535);
  // MIDI 1.0 note-on vel 0 -> note-off
  const std::uint32_t on0[] = {0x2090'3C00u};
  CHECK(normalise_packet(on0, 1, used, u) && u.is_note_off());
  const std::uint32_t off1[] = {0x2080'3C40u};
  CHECK(normalise_packet(off1, 1, used, u) && u.is_note_off() && u.velocity16() > 0);
  // MIDI 2.0 pass-through
  const core::Ump n2 = core::make_note_on(0, 5, 72, 40000);
  const std::uint32_t w2[] = {n2.word0, n2.word1};
  CHECK(normalise_packet(w2, 2, used, u) && used == 2 && u.word0 == n2.word0 && u.word1 == n2.word1);
  // Control change (type 2 status B), not mapped by the registry (CC 7): passes as the MIDI 2.0 control change (bit-repeated 7 -> 32
  // bits) for the sample engine's CcState (Story 5.2); a registry-mapped one still becomes its parameter event; truncated type-4 packet dropped.
  const std::uint32_t cc[] = {0x20B0'0740u};
  CHECK(normalise_packet(cc, 1, used, u) && used == 1 && u.is_control_change() && u.controller_index() == 7 && u.channel() == 0 &&
        u.controller_value32() == ((0x40u << 25) | (0x40u << 18) | (0x40u << 11) | (0x40u << 4) | (0x40u >> 3)));
  const std::uint32_t cc_mapped[] = {0x20B0'4840u};  // CC 72: drawbar
  CHECK(normalise_packet(cc_mapped, 1, used, u) && u.is_assignable_controller());
  const std::uint32_t cc2_raw[] = {0x40B3'0100u, 0x8000'0000u};  // MIDI 2.0 CC 1 on channel 3
  CHECK(normalise_packet(cc2_raw, 2, used, u) && used == 2 && u.is_control_change() && u.channel() == 3 && u.controller_index() == 1 &&
        u.controller_value32() == 0x8000'0000u);
  const std::uint32_t pc[] = {0x20C0'0700u};  // program change: not a note or a controller
  CHECK(!normalise_packet(pc, 1, used, u) && used == 1);
  const std::uint32_t trunc[] = {n2.word0};
  CHECK(!normalise_packet(trunc, 1, used, u) && used == 1);
  CHECK(scale_velocity_7_to_16(0) == 0 && scale_velocity_7_to_16(127) == 65535);
}

static void test_ingest_and_driver() {
  auto q = std::make_unique<HostEventQueue>();
  MidiIngest ing(*q);
  const core::Ump n = core::make_note_on(0, 0, 69, 50000);
  // Packet stream: note-on (2.0), program change (1.0, ignored), note-off (1.0).
  const std::uint32_t words[] = {n.word0, n.word1, 0x20C0'0740u, 0x2080'4500u};
  const std::uint64_t t0 = 5'000'000'000ull;
  ing.on_ump(words, 4, t0);
  CHECK(ing.counters().notes == 2 && ing.counters().ignored == 1);
  {  // A CC the registry does not map is queued as a controller (Story 5.2), not ignored (own queue: the events below are counted).
    auto cq = std::make_unique<HostEventQueue>();
    MidiIngest cing(*cq);
    const std::uint32_t cc_raw[] = {0x20B0'0140u};
    cing.on_ump(cc_raw, 1, t0);
    CHECK(cing.counters().controllers == 1 && cing.counters().ignored == 0 && cing.counters().notes == 0 && cing.counters().params == 0);
  }
  CHECK(ing.last_stamp_ns() == t0);

  auto br = std::make_unique<core::BlockRenderer>();
  RenderDriver drv(*br, *q);
  float l[64], r[64];
  // Block starts so the note-on is due at frame 10 (and the note-off, same stamp, same frame).
  const std::uint64_t bs = t0 + drv.mapper().config().delay_ns - frames_to_ns(10, 48000);
  drv.render_block(l, r, bs);
  CHECK(drv.counters().events == 2);
  CHECK(drv.counters().last_frame == 10);
  CHECK(drv.counters().late == 0);
  int first = -1;
  for (int i = 0; i < 64 && first < 0; ++i)
    if (l[i] != 0.0f) first = i;
  CHECK(first == -1 || first >= 10);  // note-off at the same frame cuts the voice (silence is fine)

  // Late event -> frame 0 next block, counted.
  ing.on_ump(words, 2, t0);
  drv.render_block(l, r, t0 + 1'000'000'000ull);
  CHECK(drv.counters().late == 1 && drv.counters().last_frame == 0);

  // Far-future event is held (counted once), never crashes, never dropped.
  ing.on_ump(words, 2, t0 + 100'000'000'000ull);
  drv.render_block(l, r, t0 + 1'000'000'000ull);
  drv.render_block(l, r, t0 + 1'001'000'000ull);
  CHECK(drv.counters().beyond_horizon == 1);
  drv.render_block(l, r, t0 + 100'000'000'000ull);  // its time has come
  CHECK(drv.counters().events == 4 && drv.counters().last_frame == 144);

  // Queue full: producer drops and counts, never blocks.
  for (int i = 0; i < 2000; ++i) ing.on_ump(words, 2, t0);
  CHECK(ing.counters().queue_full > 0);
}

// First non-zero left sample index, or -1.
static int first_nonzero(const float* l, int n) {
  for (int i = 0; i < n; ++i)
    if (l[i] != 0.0f) return i;
  return -1;
}

// Note-on only: audio starts exactly at the mapped frame (and MIDI 2.0 velocity 0 stays a note-on).
static void test_exact_start_and_vel0() {
  auto q = std::make_unique<HostEventQueue>();
  MidiIngest ing(*q);
  auto br = std::make_unique<core::BlockRenderer>();
  RenderDriver drv(*br, *q);
  float l[64], r[64];
  const std::uint64_t t0 = 7'000'000'000ull;
  const core::Ump n = core::make_note_on(0, 0, 60, 45000);
  const std::uint32_t w[] = {n.word0, n.word1};
  ing.on_ump(w, 2, t0);
  drv.render_block(l, r, t0 + drv.mapper().config().delay_ns - frames_to_ns(20, 48000));
  CHECK(first_nonzero(l, 64) == 20);

  const core::Ump z = core::make_note_on(0, 0, 61, 0);  // MIDI 2.0 note-on, velocity 0
  const std::uint32_t wz[] = {z.word0, z.word1};
  core::Ump out;
  std::size_t used = 0;
  CHECK(normalise_packet(wz, 2, used, out) && out.is_note_on());
  auto q2 = std::make_unique<HostEventQueue>();
  MidiIngest ing2(*q2);
  ing2.on_ump(wz, 2, t0);
  StampedUmp e;
  CHECK(q2->pop(e) && e.ump.is_note_on());
  CHECK(ing2.last_stamp_ns() == t0);
  ing2.on_ump(wz, 0, t0 + 5);  // nothing queued: stamp must not move
  CHECK(ing2.last_stamp_ns() == t0);
}

// Due ~200 frames out: deferred through the renderer; sound begins at that exact absolute frame.
static void test_deferred_absolute_position() {
  auto q = std::make_unique<HostEventQueue>();
  MidiIngest ing(*q);
  auto br = std::make_unique<core::BlockRenderer>();
  RenderDriver drv(*br, *q);
  const std::uint64_t t0 = 9'000'000'000ull;
  const core::Ump n = core::make_note_on(0, 0, 60, 45000);
  const std::uint32_t w[] = {n.word0, n.word1};
  const std::uint64_t bs0 = t0 + drv.mapper().config().delay_ns - frames_to_ns(200, 48000);
  ing.on_ump(w, 2, t0);
  float l[64], r[64];
  int abs_first = -1;
  for (int b = 0; b < 6; ++b) {
    drv.render_block(l, r, bs0 + frames_to_ns(64ull * b, 48000));
    const int f = first_nonzero(l, 64);
    if (f >= 0 && abs_first < 0) abs_first = b * 64 + f;
  }
  CHECK(drv.counters().deferred == 1);
  CHECK(abs_first == 200);
}

// Note-on beyond the horizon followed by a note-off inside it: the note-off must not overtake.
static void test_no_reorder_beyond_horizon() {
  auto q = std::make_unique<HostEventQueue>();
  MidiIngest ing(*q);
  auto br = std::make_unique<core::BlockRenderer>();
  RenderDriver drv(*br, *q);
  const std::uint64_t bs = 20'000'000'000ull;
  const std::uint64_t on_t = bs + 500'000'000ull;          // 500 ms out: beyond the 100 ms horizon
  const std::uint64_t off_t = on_t + 1'000'000ull;         // 1 ms after the note-on
  const core::Ump on = core::make_note_on(0, 0, 60, 45000);
  const core::Ump off = core::make_note_off(0, 0, 60);
  const std::uint32_t w1[] = {on.word0, on.word1};
  const std::uint32_t w2[] = {off.word0, off.word1};
  ing.on_ump(w1, 2, on_t);
  ing.on_ump(w2, 2, off_t);
  float l[64], r[64];
  drv.render_block(l, r, bs);
  CHECK(drv.counters().events == 0 && drv.counters().beyond_horizon == 1);  // both held, in order
  drv.render_block(l, r, on_t + drv.mapper().config().delay_ns - frames_to_ns(10, 48000));
  CHECK(drv.counters().events == 2);                       // on at frame 10, off 1 ms (48 frames) later
  CHECK(br->test_tone().active_voices() == 1);             // on applied before off: voice is releasing
  for (int b = 0; b < 40; ++b) drv.render_block(l, r, off_t + frames_to_ns(64ull * b, 48000));
  CHECK(br->test_tone().active_voices() == 0);             // released, not stuck
}

// Callback sizes 64/100/512/1: one render_block per 64 output frames, correct starts, continuity.
namespace {
struct CountingSink final : velora::ports::BlockSink {
  std::vector<std::uint64_t> starts;
  std::uint64_t next = 0;  // running sample counter written into the audio
  void render_block(float* l, float* r, std::uint64_t t) noexcept override {
    starts.push_back(t);
    for (int i = 0; i < 64; ++i) l[i] = r[i] = static_cast<float>(next++);
  }
};
}  // namespace

static void test_slicer() {
  for (std::uint32_t cb : {64u, 100u, 512u, 1u}) {
    CountingSink sink;
    BlockSlicer sl;
    const std::uint64_t t_origin = 3'000'000'000ull;
    std::uint64_t played = 0;
    std::vector<float> out;
    const std::uint64_t total = 64 * 40;
    while (played < total) {
      std::vector<float> l(cb), r(cb);
      sl.process(sink, l.data(), r.data(), cb, t_origin + frames_to_ns(played, 48000));
      for (std::uint32_t i = 0; i < cb; ++i) {
        CHECK(l[i] == r[i]);
        out.push_back(l[i]);
      }
      played += cb;
    }
    // Continuity: every output frame is the next sample, no drops or duplicates.
    bool continuous = true;
    for (std::size_t i = 0; i < out.size(); ++i) continuous = continuous && out[i] == static_cast<float>(i);
    CHECK(continuous);
    // Exactly ceil(played/64) blocks; block k starts when its first frame is heard.
    CHECK(sink.starts.size() == (played + 63) / 64);
    bool starts_ok = true;
    for (std::size_t k = 0; k < sink.starts.size(); ++k)
    {  // each term is rounded to a ns separately: allow 1 ns
      const std::uint64_t want = t_origin + frames_to_ns(64 * k, 48000);
      const std::uint64_t d = sink.starts[k] > want ? sink.starts[k] - want : want - sink.starts[k];
      starts_ok = starts_ok && d <= 1;
    }
    CHECK(starts_ok);
    if (!starts_ok || !continuous) std::printf("  (callback size %u)\n", cb);
    sl.reset();
  }
}

// Channel-mode messages are not controllers, and a controller flood never costs a note (Story 5.2).
static void test_controller_flood_and_channel_mode() {
  core::Ump u;
  std::size_t used = 0;
  for (std::uint32_t cc : {120u, 121u, 123u, 127u}) {
    const std::uint32_t w1[] = {0x2000'0000u | (0xBu << 20) | (cc << 8) | 0x7Fu};
    CHECK(!normalise_packet(w1, 1, used, u) && used == 1);
    const std::uint32_t w2[] = {0x4000'0000u | (0xBu << 20) | (cc << 8), 0u};
    CHECK(!normalise_packet(w2, 2, used, u) && used == 2);
  }
  const std::uint32_t cc119[] = {0x2000'0000u | (0xBu << 20) | (119u << 8) | 5u};
  CHECK(normalise_packet(cc119, 1, used, u) && u.is_control_change());  // the last real controller still passes

  auto q = std::make_unique<HostEventQueue>();  // nobody drains it: the worst case
  MidiIngest ing(*q);
  const std::uint32_t cc1[] = {0x2000'0000u | (0xBu << 20) | (1u << 8) | 64u};
  for (int i = 0; i < 100000; ++i) ing.on_ump(cc1, 1, 1);
  const std::uint64_t accepted = ing.counters().controllers.load();
  CHECK(accepted == HostEventQueue::capacity() - kNoteHeadroom);
  CHECK(ing.counters().controllers_dropped.load() == 100000 - accepted);
  CHECK(ing.counters().queue_full.load() == 0);
  // Notes and the pedal still get in, up to the headroom: nothing is lost to the flood.
  for (std::size_t i = 0; i < kNoteHeadroom - 1; ++i) {
    const core::Ump n = (i % 2) ? core::make_note_off(0, 0, 60, 0) : core::make_note_on(0, 0, 60, 40000);
    const std::uint32_t w[] = {n.word0, n.word1};
    ing.on_ump(w, 2, 2);
  }
  const std::uint32_t pedal[] = {0x2000'0000u | (0xBu << 20) | (64u << 8) | 127u};
  ing.on_ump(pedal, 1, 3);
  CHECK(ing.counters().notes.load() == kNoteHeadroom);
  CHECK(ing.counters().queue_full.load() == 0);
  // More controllers while the queue is that full are still shed, not queued.
  ing.on_ump(cc1, 1, 4);
  CHECK(ing.counters().controllers.load() == accepted);
}

int main() {
  test_controller_flood_and_channel_mode();
  test_exact_start_and_vel0();
  test_deferred_absolute_position();
  test_no_reorder_beyond_horizon();
  test_slicer();
  test_mapper();
  test_normalise();
  test_ingest_and_driver();
  if (g_fail == 0) std::printf("host_unit_tests: ok\n");
  return g_fail ? 1 : 0;
}
