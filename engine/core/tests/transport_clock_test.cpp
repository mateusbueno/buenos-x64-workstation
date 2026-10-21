// Story 7.1: the single tempo and transport clock. Integer grid, tempo clamp, tempo-change continuity, and a 10-minute sixteenth grid at 120 and
// 97.3 bpm through the BlockRenderer compared with the exact rational position rounded to the nearest frame.
#include <cmath>
#include <cstdint>
#include <limits>
#include <cstdio>
#include <memory>

#include "velora/core/block_renderer.hpp"
#include "velora/core/param_events.hpp"
#include "velora/core/params_gen.hpp"
#include "velora/core/transport_clock.hpp"

using namespace velora::core;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {

void test_basics() {
  CHECK(TransportClock::kUnitsPerTick == 30000);
  CHECK(TransportClock::tenths_from_bpm(120.0f) == 1200 && TransportClock::tenths_from_bpm(97.3f) == 973);
  CHECK(TransportClock::tenths_from_bpm(NAN) == 1200 && TransportClock::tenths_from_bpm(1000.0f) == 3000 && TransportClock::tenths_from_bpm(1.0f) == 200);
  TransportClock c;
  CHECK(c.tempo_tenths() == 1200 && c.position_units() == 0);
  c.set_tempo_tenths(5);
  CHECK(c.tempo_tenths() == 200);
  c.set_tempo_tenths(99999);
  CHECK(c.tempo_tenths() == 3000);
  c.set_tempo_tenths(1200);
  // 120 bpm: a quarter = 24000 frames = 960 ticks; 25 frames per tick
  CHECK(c.ticks_at(24000) == 960 && c.ticks_at(23999) == 959 && c.frame_of_tick(960) == 24000 && c.frame_of_tick(1) == 25);
  c.advance(64);
  CHECK(c.frames() == 64 && c.position_units() == 64 * 1200 && c.ticks_at(0) == 2);
  CHECK(c.frame_of_tick(0) == -64 && c.frame_of_tick(3) == 11);  // past ticks are negative offsets (64 frames = 2.56 ticks)
}

void test_tempo_change_continuity() {
  TransportClock c;
  c.advance(1000);  // 120 bpm
  const std::int64_t before = c.position_units();
  c.set_tempo_tenths(600);
  CHECK(c.position_units() == before);
  c.advance(1000);
  CHECK(c.position_units() == before + 1000 * 600);
}

struct Rig {
  std::unique_ptr<BlockRenderer> br = std::make_unique<BlockRenderer>(32);
  void run(int blocks) {
    float l[kBlockFrames], r[kBlockFrames], al[kBlockFrames], ar[kBlockFrames];
    for (int b = 0; b < blocks; ++b) {
      OutputBuses ob{l, r, al, ar};
      br->process_block(ob);
    }
  }
};

// 10 minutes of blocks; at each block start, every sixteenth (240 ticks) that falls in the block gets the frame the clock reports, which must
// equal the exact rational position rounded to the nearest frame.
void grid_run(float bpm) {
  Rig rig;
  rig.br->submit({make_param_event(*params::find(params::kTempoBpm), 0, bpm), 0});
  rig.run(1);  // applies the event; the clock uses it from the next block
  const std::int64_t tenths = TransportClock::tenths_from_bpm(bpm);
  const std::uint64_t start = rig.br->clock().frames();  // 64: block 0 ran at 120
  const std::int64_t start_units = rig.br->clock().position_units();
  CHECK(start == kBlockFrames && start_units == static_cast<std::int64_t>(kBlockFrames) * 1200);
  const std::int64_t total_blocks = 10 * 60 * 48000 / kBlockFrames;
  std::int64_t next_tick = ((start_units + 240 * 30000 - 1) / (240 * 30000)) * 240;
  std::int64_t checked = 0;
  bool ok = true;
  for (std::int64_t b = 0; b < total_blocks; ++b) {
    rig.run(1);
    // after run, clock advanced; query in terms of the block just rendered via offsets from the new position
    const std::int64_t end_frame = static_cast<std::int64_t>(rig.br->clock().frames());
    while (true) {
      // absolute frame of next_tick: new block start + offset (offset negative while inside the block just rendered)
      const std::int64_t abs_frame = end_frame + rig.br->clock().frame_of_tick(next_tick);
      if (abs_frame >= end_frame) break;
      // exact: units at tempo `tenths` after start_units from frame `start`
      const std::int64_t delta = next_tick * 30000 - start_units;
      const std::int64_t expect = static_cast<std::int64_t>(start) + (2 * delta + tenths) / (2 * tenths);
      if (abs_frame != expect) ok = false;
      // independent oracle (no shared formula with the clock): the musical distance from the start position in beats x frames per beat, in long double
      const long double beats = (static_cast<long double>(next_tick) - static_cast<long double>(start_units) / 30000.0L) / 960.0L;
      const long double exact = beats * 60.0L * 48000.0L / (static_cast<long double>(tenths) / 10.0L);
      if (abs_frame != static_cast<std::int64_t>(start) + static_cast<std::int64_t>(std::floor(exact + 0.5L))) ok = false;
      next_tick += 240;
      ++checked;
    }
  }
  CHECK(ok);
  CHECK(checked > 0);
  std::printf("grid %.1f bpm: %lld sixteenths checked\n", static_cast<double>(bpm), static_cast<long long>(checked));
}

void test_renderer_tempo_change() {
  Rig rig;
  rig.run(2);
  CHECK(rig.br->clock().tempo_tenths() == 1200 && rig.br->clock().position_units() == 2 * 64 * 1200);
  rig.br->submit({make_param_event(*params::find(params::kTempoBpm), 0, 60.0), 10});
  rig.run(1);  // event inside this block: this block still advances at 120
  CHECK(rig.br->clock().position_units() == 3 * 64 * 1200);
  rig.run(1);  // next block uses the new tempo
  CHECK(rig.br->clock().tempo_tenths() == 600 && rig.br->clock().position_units() == 3 * 64 * 1200 + 64 * 600);
}

}  // namespace

int main() {
  test_basics();
  test_tempo_change_continuity();
  test_renderer_tempo_change();
  {  // inf and -inf map to the default tempo like NaN; reset restores the default tempo
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(TransportClock::tenths_from_bpm(inf) == 1200 && TransportClock::tenths_from_bpm(-inf) == 1200);
    TransportClock c;
    c.set_tempo_tenths(600);
    c.advance(64);
    c.reset();
    CHECK(c.tempo_tenths() == 1200 && c.position_units() == 0 && c.frames() == 0);
  }
  grid_run(120.0f);
  grid_run(97.3f);
  if (g_fail) { std::printf("%d failures\n", g_fail); return 1; }
  std::printf("OK\n");
  return 0;
}
