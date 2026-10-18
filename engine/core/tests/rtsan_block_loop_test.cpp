// 10,000 blocks of note on/off traffic through the block loop inside a nonblocking function.
// RTSan aborts the process on any malloc/lock/blocking call in the render path.
#include <cstdint>
#include <cstdio>
#include <memory>

#include "velora/core/block_renderer.hpp"

using namespace velora::core;

static void run_block(BlockRenderer& br, float* l, float* r, std::uint32_t i) [[clang::nonblocking]] {
  const std::uint32_t note = 36 + (i % 48);
  br.submit({make_note_on(0, 0, note, 40000), (i * 7) % 200});  // some deferred past the block
  br.submit({make_note_off(0, 0, 36 + ((i + 20) % 48)), (i * 13) % 200});
  br.submit({make_note_on(0, 1, note, 40000), (i * 3) % 64});   // channel 1 -> FM (not compiled)
  br.submit({make_note_on(1, 0, note, 40000), 5});              // group 1 -> ignored
  if (i % 100 == 0)
    for (std::uint32_t k = 0; k < 40; ++k) br.submit({make_note_on(0, 0, 20 + k, 40000), k});  // pool-full burst
  br.process_block(l, r);
}

int main() {
  auto br = std::make_unique<BlockRenderer>();
  br->set_channel_engine(1, static_cast<std::uint32_t>(EngineTypeId::kFm));
  float l[kBlockFrames], r[kBlockFrames];
  for (std::uint32_t i = 0; i < 10000; ++i) run_block(*br, l, r, i);
  const auto ok = br->status_count(EngineStatus::kOk);
  const auto na = br->status_count(EngineStatus::kEngineNotAvailable);
  const auto full = br->status_count(EngineStatus::kPoolExhausted);
  const auto ign = br->ignored_events();
  std::printf("rtsan_block_loop_test: ok=%llu not_available=%llu pool_exhausted=%llu ignored=%llu\n",
              (unsigned long long)ok, (unsigned long long)na, (unsigned long long)full, (unsigned long long)ign);
  return (ok > 0 && na > 0 && full > 0 && ign > 0) ? 0 : 1;
}
