// Story 4.9 RT safety: the control->RT fence under dense traffic. The control thread sends bursts of parameter changes, raises a fence behind them and
// waits for RT's acknowledgement; after every acknowledgement it reads the last-value table and every value of the burst must be there (the fence is ordered
// behind the events it covers, also when the time mapper deferred them to later blocks). Meanwhile RT renders blocks inside a nonblocking function with
// notes starting and ending on 16 channels, the sustain pedal moving, and snapshot swaps arriving; RealtimeSanitizer aborts on any allocation, lock or
// blocking call on that path (the fence acknowledgement is one atomic store, the adopted-revision mark one more per generation change).
// Without the sanitizer it is the same logic.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
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

static void rt_block(RenderDriver& d, core::BlockRenderer& br, float* l, float* r, std::uint32_t i, std::uint64_t t) noexcept VELORA_NONBLOCKING {
  const std::uint32_t ch = (i * 7) % 16, key = 30 + (i * 11) % 70;
  if (i % 2 == 0) br.submit({core::make_note_on(0, ch, key, 20000 + (i % 40000)), (i * 5) % 64});
  if (i % 2 == 1) br.submit({core::make_note_off(0, ((i - 1) * 7) % 16, 30 + ((i - 1) * 11) % 70), (i * 9) % 64});
  if (i % 97 == 0) br.submit({core::make_control_change(0, 0, 64, 0xFFFFFFFFu), 3});
  if (i % 97 == 50) br.submit({core::make_control_change(0, 0, 64, 0u), 3});
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

  constexpr int kRounds = 3000;
  std::atomic<bool> control_done{false};
  std::atomic<int> fences{0}, errors{0}, stale{0}, swaps{0};
  std::thread control([&] {
    std::uint32_t last_seq = 0;
    for (int round = 0; round < kRounds; ++round) {
      std::map<std::pair<std::uint32_t, std::uint32_t>, float> expect;  // (param, part) -> the last value of this burst
      const int burst = 1 + round % 7;
      for (int b = 0; b < burst; ++b) {
        const std::uint32_t id = p::kDrawbar16 + static_cast<std::uint32_t>((round + b) % 9 == 0 ? 0 : (round + b) % 9 - 1);
        const std::uint32_t part = static_cast<std::uint32_t>((round * 3 + b) % 8);
        const float value = static_cast<float>((round + b * 5) % 9);
        for (;;) {
          ws::v1::ClientMessage m;
          auto* c = m.mutable_set_performance_param();
          c->set_param_id(static_cast<ws::v1::ParamId>(id));
          c->set_part(part);
          c->set_value(value);
          bool changed = false;
          const auto rep = plane.handle(m, changed);
          if (!rep.has_error()) break;
          if (rep.error().code() != ws::v1::ERROR_CODE_BUSY) { errors++; break; }
          std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        expect[{id, part}] = value;
      }
      if (round % 40 == 0) {  // snapshot swaps arrive too
        ws::v1::ClientMessage m;
        auto* s = m.mutable_set_send_levels();
        s->set_part(static_cast<std::uint32_t>(round % 8));
        s->set_reverb(static_cast<float>(round % 100) / 100.0f);
        bool changed = false;
        const auto rep = plane.handle(m, changed);
        if (rep.has_error() || !changed) errors++;
        else swaps++;
      }
      std::uint32_t seq = 0;
      while (!plane.raise_fence(seq)) std::this_thread::sleep_for(std::chrono::microseconds(100));
      if (seq != last_seq + 1) errors++;  // sequence numbers are consecutive
      last_seq = seq;
      int spins = 0;
      while (!plane.fence_acked(seq)) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        if (++spins > 400000) { errors++; break; }  // a hang fails instead of blocking
      }
      for (const auto& kv : expect)
        if (plane.live_value(p::perf_slot(*p::find(kv.first.first), kv.first.second)) != kv.second) stale++;  // read AFTER the fence: never stale
      fences++;
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
  control.join();
  for (std::uint32_t k = 0; k < 16; ++k) br->submit({core::make_note_off(0, k, 0), 0});
  for (int k = 0; k < 300; ++k) rt_block(drv, *br, l, r, 1u + 2u * static_cast<std::uint32_t>(k), g_clock.fetch_add(1'333'333) + 1'333'333);
  plane.reclaim();

  std::printf("rtsan_edit_test: fences=%d swaps=%d stale=%d errors=%d blocks=%u adopted=%llu revision=%llu\n", fences.load(), swaps.load(), stale.load(), errors.load(), i,
              (unsigned long long)br->last_values().load_adopted_revision(), (unsigned long long)plane.revision());
  const bool ok = errors.load() == 0 && stale.load() == 0 && fences.load() == kRounds && plane.rt_adopted_current() && br->param_events_rejected() == 0;
  return ok ? 0 : 1;
}
