// The host render path (queue drain -> time mapping -> BlockRenderer) inside a nonblocking
// function: RTSan aborts on any malloc/lock/blocking call.
#include <cstdio>
#include <memory>

#include "velora/core/block_renderer.hpp"
#include "velora/host/midi_ingest.hpp"
#include "velora/host/render_driver.hpp"

using namespace velora;
using namespace velora::host;

static void run_block(RenderDriver& d, MidiIngest& in, float* l, float* r, std::uint32_t i,
                      std::uint64_t bs, std::uint64_t delay) noexcept [[clang::nonblocking]] {
  const std::uint32_t words[] = {core::make_note_on(0, 0, 36 + (i % 48), 40000).word0, 40000u << 16,
                                 0x2080'0000u | ((36 + ((i + 20) % 48)) << 8)};
  // Due 0, 24, 48 or 72 frames into the block: three in-block, one deferred.
  in.on_ump(words, 3, bs - delay + (i % 4) * 500'000ull);
  d.render_block(l, r, bs);
}

int main() {
  auto q = std::make_unique<HostEventQueue>();
  auto br = std::make_unique<core::BlockRenderer>();
  RenderDriver drv(*br, *q);
  MidiIngest ing(*q);
  float l[64], r[64];
  std::uint64_t bs = 1'000'000'000ull;
  for (std::uint32_t i = 0; i < 10000; ++i) {
    run_block(drv, ing, l, r, i, bs, drv.mapper().config().delay_ns);
    bs += frames_to_ns(64, 48000);
  }
  std::printf("rtsan_host_render_test: events=%llu late=%llu\n",
              (unsigned long long)drv.counters().events.load(),
              (unsigned long long)drv.counters().late.load());
  const auto& c = drv.counters();
  // Story 1.21: the diagnostics were updated inside the nonblocking path (atomics only): every block timed, every on-time event's latency recorded.
  const auto& dg = br->last_values().diagnostics();
  // (a block is 1.33 ms of budget; unsanitized, the stub render stays far below it. Under RTSan the sanitizer overhead may exceed it, so only count there.)
#if !defined(__has_feature) || !__has_feature(realtime_sanitizer)
  if (dg.xruns() != 0) return 1;
#endif
  const std::uint64_t xr = dg.xruns();
  drv.record_xrun();
  if (dg.render_time().total() != c.blocks.load() || dg.latency().total() == 0 || dg.latency().total() > c.events || dg.xruns() != xr + 1) return 1;
  return (c.events > 0 && c.deferred > 0 && c.deferred < c.events && c.late == 0 &&
          c.renderer_rejected == 0 && ing.counters().queue_full == 0)
             ? 0
             : 1;
}
