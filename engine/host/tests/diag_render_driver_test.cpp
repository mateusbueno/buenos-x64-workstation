// Story 1.21: RenderDriver diagnostics with a deterministic clock: event-to-onset latency recorded by drain(), late / unstamped events not recorded,
// a render over its deadline counts an xrun.
#include <cstdio>
#include <memory>

#include "velora/core/block_renderer.hpp"
#include "velora/host/midi_ingest.hpp"
#include "velora/host/render_driver.hpp"

using namespace velora;
using namespace velora::host;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static void note(MidiIngest& in, std::uint32_t n, std::uint64_t host_ns) {
  const std::uint32_t words[] = {core::make_note_on(0, 0, n, 40000).word0, 40000u << 16};
  in.on_ump(words, 2, host_ns);
}

int main() {
  float l[64], r[64];
  {  // exact latency and bin
    auto q = std::make_unique<HostEventQueue>();
    auto br = std::make_unique<core::BlockRenderer>();
    RenderDriver drv(*br, *q);
    MidiIngest in(*q);
    const std::uint64_t block_start = 10'000'000;
    // due = host + 3 ms delay = block_start + 10 frames -> frame 10; onset - host = 3000 us = bin [3000, 4000)
    note(in, 60, block_start - 3'000'000 + frames_to_ns(10, 48000));
    drv.render_block(l, r, block_start);
    const auto& h = br->last_values().diagnostics().latency();
    CHECK(drv.counters().last_frame.load() == 10 && drv.counters().events.load() == 1);
    CHECK(h.total() == 1);
    std::size_t bin = 0;
    for (std::size_t i = 0; i < h.edges_us().size(); ++i) if (h.edges_us()[i] <= 3000) bin = i + 1;
    CHECK(h.edges_us()[bin - 1] == 3000 && h.count(bin) == 1);
    // a late event (stamped long before the block) records nothing; neither does an unstamped one (host_ns == 0)
    note(in, 61, 1'000'000);
    note(in, 62, 0);
    drv.render_block(l, r, block_start + 1'333'333);
    CHECK(drv.counters().late.load() >= 1 && h.total() == 1);
    CHECK(br->last_values().diagnostics().render_time().total() == 2);
  }
  {  // a render over its deadline counts an xrun; a generous deadline does not
    auto q = std::make_unique<HostEventQueue>();
    auto br = std::make_unique<core::BlockRenderer>();
    TimeMapperConfig tiny;
    tiny.sample_rate = 4'000'000'000u;  // deadline 64 frames = 16 ns: any real render exceeds it
    RenderDriver slow(*br, *q, tiny);
    slow.render_block(l, r, 1'000'000);
    CHECK(br->last_values().diagnostics().xruns() == 1);
    auto br2 = std::make_unique<core::BlockRenderer>();
    TimeMapperConfig slack;
    slack.sample_rate = 1000;  // deadline 64 ms
    RenderDriver ok(*br2, *q, slack);
    ok.render_block(l, r, 1'000'000);
    CHECK(br2->last_values().diagnostics().xruns() == 0);
  }
  if (g_fail) std::printf("diag_render_driver_test: %d failure(s)\n", g_fail);
  else std::printf("diag_render_driver_test: ok\n");
  return g_fail ? 1 : 0;
}
