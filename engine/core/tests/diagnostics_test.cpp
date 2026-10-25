// Story 1.21: in-RAM diagnostics counters and fixed-bin histograms.
#include <cstdint>
#include <cstdio>

#include "velora/core/diagnostics.hpp"

using namespace velora::core;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static std::uint64_t sum_other_than(const Diagnostics& d, std::uint64_t& x, std::uint64_t& u, std::uint64_t& s, std::uint64_t& e) {
  x = d.xruns(); u = d.stream_underruns(); s = d.voice_steals(); e = d.voice_exhausted();
  return x + u + s + e;
}

int main() {
  {  // counters: each moves only itself
    Diagnostics d;
    d.record_xrun();
    CHECK(d.xruns() == 1 && d.stream_underruns() == 0 && d.voice_steals() == 0 && d.voice_exhausted() == 0);
    d.record_stream_underrun();
    d.record_stream_underrun();
    CHECK(d.xruns() == 1 && d.stream_underruns() == 2);
    d.store_voice_totals(5, 2);
    CHECK(d.voice_steals() == 5 && d.voice_exhausted() == 2 && d.xruns() == 1 && d.stream_underruns() == 2);
    std::uint64_t x, u, s, e;
    CHECK(sum_other_than(d, x, u, s, e) == 10);
  }
  {  // render histogram: bins by edge, over-deadline counts an xrun, overflow goes to the last bin
    Diagnostics d;
    const auto& h = d.render_time();
    const auto& ed = h.edges_us();
    d.record_render(40'000, 1'333'333);              // 40 us -> bin 0
    d.record_render(1'000'000, 1'333'333);           // 1000 us -> bin after the 1000 edge
    d.record_render(1'400'000, 1'333'333);           // over deadline: also an xrun
    d.record_render(100'000'000, 1'333'333);         // 100 ms: overflow bin
    CHECK(h.count(0) == 1);
    std::size_t b1000 = 0;
    for (std::size_t i = 0; i < ed.size(); ++i) if (ed[i] == 1000) b1000 = i + 1;
    CHECK(b1000 > 0 && h.count(b1000) == 1);
    CHECK(h.count(Diagnostics::kRenderBins - 1) == 1);
    CHECK(h.total() == 4);
    CHECK(d.xruns() == 2);  // the 1.4 ms and the 100 ms blocks
    d.record_render(1'333'333, 1'333'333);           // exactly on the deadline: not an xrun
    CHECK(d.xruns() == 2);
  }
  {  // latency histogram
    Diagnostics d;
    const auto& h = d.latency();
    d.record_latency_ns(0);
    d.record_latency_ns(249'999);  // still bin 0
    d.record_latency_ns(250'000);  // edge: next bin
    d.record_latency_ns(4'000'000'000ull);
    CHECK(h.count(0) == 2 && h.count(1) == 1);
    CHECK(h.count(Diagnostics::kLatencyBins - 1) == 1 && h.total() == 4);
    CHECK(d.xruns() == 0);
  }
  {  // the edges are strictly increasing
    Diagnostics d;
    for (std::size_t i = 1; i < d.render_time().edges_us().size(); ++i) CHECK(d.render_time().edges_us()[i] > d.render_time().edges_us()[i - 1]);
    for (std::size_t i = 1; i < d.latency().edges_us().size(); ++i) CHECK(d.latency().edges_us()[i] > d.latency().edges_us()[i - 1]);
  }
  if (g_fail) std::printf("diagnostics_tests: %d failure(s)\n", g_fail);
  else std::printf("diagnostics_tests: ok\n");
  return g_fail ? 1 : 0;
}
