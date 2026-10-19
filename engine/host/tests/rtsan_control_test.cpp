// Story 2.1 RT safety: 10,000 performance parameter changes (control thread -> timestamped UMP -> RT)
// and 100 snapshot swaps while the RT side renders blocks with notes starting and ending, all inside a
// nonblocking function so RealtimeSanitizer aborts on any allocation, lock or blocking call on the RT
// path. The control thread (a normal thread) validates, allocates and frees generations meanwhile.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include "velora/core/block_renderer.hpp"
#include "velora/host/control_plane.hpp"
#include "velora/host/midi_ingest.hpp"
#include "velora/host/render_driver.hpp"

using namespace velora;
using namespace velora::host;
namespace p = core::params;

static std::atomic<std::uint64_t> g_clock{1'000'000'000ull};

static void rt_block(RenderDriver& d, core::BlockRenderer& br, float* l, float* r, std::uint32_t i,
                     std::uint64_t t) noexcept VELORA_NONBLOCKING {
  if (i % 3 == 0) br.submit({core::make_note_on(0, i % 8, 36 + (i % 40), 40000), (i * 5) % 64});
  if (i % 3 == 1) br.submit({core::make_note_off(0, (i - 1) % 8, 36 + ((i - 1) % 40)), (i * 7) % 64});
  d.render_block(l, r, t);
}

int main() {
  auto br = std::make_unique<core::BlockRenderer>();
  auto midi_q = std::make_unique<HostEventQueue>();
  auto ctl_q = std::make_unique<HostEventQueue>();
  RenderDriver drv(*br, *midi_q, {}, ctl_q.get());
  ControlPlane plane(br->snapshots(), br->last_values(), *ctl_q, [] { return g_clock.load(std::memory_order_relaxed); });
  std::string err;
  if (!plane.load_initial(ws::v1::State(), err)) {
    std::printf("load_initial: %s\n", err.c_str());
    return 1;
  }

  constexpr int kChanges = 10000, kSwaps = 100;
  std::atomic<bool> control_done{false};
  std::atomic<int> sent{0}, swaps{0}, errors{0};
  std::thread control([&] {
    for (int i = 0; i < kChanges; ++i) {
      // Flow control: the renderer holds at most 256 pending events, so keep fewer than that in flight.
      while (static_cast<std::uint64_t>(i) > br->param_events_applied() + br->param_events_rejected() + 100)
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      for (;;) {
        ws::v1::ClientMessage m;
        auto* c = m.mutable_set_performance_param();
        c->set_param_id(ws::v1::PARAM_DRAWBAR_8);
        c->set_part(static_cast<std::uint32_t>(i % 8));
        c->set_value(static_cast<float>(i % 9));
        bool changed = false;
        const auto rep = plane.handle(m, changed);
        if (!rep.has_error()) break;
        if (rep.error().code() != ws::v1::ERROR_CODE_BUSY) { errors++; break; }
        std::this_thread::sleep_for(std::chrono::microseconds(100));  // RT queue full: let RT drain
      }
      sent++;
      if (i % (kChanges / kSwaps) == 0) {
        ws::v1::ClientMessage m;
        auto* s = m.mutable_set_send_levels();
        s->set_part(static_cast<std::uint32_t>(i % 8));
        s->set_reverb(static_cast<float>(i % 100) / 100.0f);
        bool changed = false;
        const auto rep = plane.handle(m, changed);
        if (rep.has_error() || !changed) errors++;
        else swaps++;
      }
    }
    control_done.store(true);
  });

  float l[core::kBlockFrames], r[core::kBlockFrames];
  std::uint32_t i = 0;
  while (!control_done.load() || i < 200) {
    const std::uint64_t t = g_clock.fetch_add(1'333'333) + 1'333'333;
    rt_block(drv, *br, l, r, i++, t);
    if (i % 4 == 0) std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
  for (int k = 0; k < 20; ++k) {  // drain the tail
    const std::uint64_t t = g_clock.fetch_add(1'333'333) + 1'333'333;
    rt_block(drv, *br, l, r, i++, t);
  }
  control.join();
  for (std::uint32_t k = 0; k < 8; ++k) br->submit({core::make_note_off(0, k, 0), 0});
  for (int k = 0; k < 200; ++k) rt_block(drv, *br, l, r, i++ * 3 + 2, g_clock.fetch_add(1'333'333) + 1'333'333);
  plane.reclaim();

  std::printf("rtsan_control_test: changes=%d swaps=%d applied=%llu rejected=%llu blocks=%u live_generations=%zu\n",
              sent.load(), swaps.load(), (unsigned long long)br->param_events_applied(),
              (unsigned long long)br->param_events_rejected(), i, plane.live_generations());
  const bool ok = errors.load() == 0 && sent.load() == kChanges && swaps.load() == kSwaps &&
                  br->param_events_applied() == static_cast<std::uint64_t>(kChanges) &&
                  br->param_events_rejected() == 0 && plane.revision() == 1u + kSwaps &&
                  br->held_snapshot() == br->snapshots().current();
  return ok ? 0 : 1;
}
