#include "velora/core/clock_follower.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/transport_clock.hpp"
#include "velora/core/ump.hpp"

using namespace velora::core;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
      return 1;                                                                     \
    }                                                                               \
  } while (0)

namespace {

int test_dll_tracking_within_2_beats() {
  ClockFollower follower;
  TransportClock clock;
  follower.reset();
  clock.reset();

  // Initial stream at 120.0 BPM (1000 frames per tick)
  const double tau_120 = 1000.0;
  std::uint64_t frame = 0;
  follower.on_start(frame, clock);
  for (std::size_t i = 0; i < 20; ++i) {
    frame = static_cast<std::uint64_t>(std::lround(static_cast<double>(i) * tau_120));
    follower.on_clock_tick(frame, clock);
  }
  CHECK(std::abs(follower.tempo_bpm() - 120.0f) < 0.1f);

  // Step change to 140.0 BPM (120000 / 140 = 857.142857 frames per tick)
  const double target_bpm = 140.0;
  const double tau_140 = 120000.0 / target_bpm;
  const std::uint64_t step_start_frame = frame;

  float prev_bpm = follower.tempo_bpm();
  for (std::size_t k = 1; k <= 48; ++k) {
    frame = step_start_frame + static_cast<std::uint64_t>(std::lround(static_cast<double>(k) * tau_140));
    follower.on_clock_tick(frame, clock);
    const float cur_bpm = follower.tempo_bpm();
    // Critical damping: monotonically approaches target tempo, no overshoot!
    CHECK(cur_bpm >= prev_bpm - 0.05f);
    CHECK(cur_bpm <= target_bpm + 0.05f);  // No overshoot!
    prev_bpm = cur_bpm;
  }

  // Within 2 beats (48 ticks), tracking must settle to within 0.1 BPM of target
  CHECK(std::abs(follower.tempo_bpm() - static_cast<float>(target_bpm)) < 0.1f);
  return 0;
}

int test_step_down_tempo_tracking() {
  ClockFollower follower;
  TransportClock clock;
  follower.reset();
  clock.reset();

  // Initial stream at 130.0 BPM
  const double tau_130 = 120000.0 / 130.0;
  std::uint64_t frame = 0;
  follower.on_start(frame, clock);
  for (std::size_t i = 0; i < 20; ++i) {
    frame = static_cast<std::uint64_t>(std::lround(static_cast<double>(i) * tau_130));
    follower.on_clock_tick(frame, clock);
  }

  // Step down to 90.0 BPM (tau = 1333.333 frames)
  const double target_bpm = 90.0;
  const double tau_90 = 120000.0 / target_bpm;
  const std::uint64_t step_start_frame = frame;

  float prev_bpm = follower.tempo_bpm();
  for (std::size_t k = 1; k <= 48; ++k) {
    frame = step_start_frame + static_cast<std::uint64_t>(std::lround(static_cast<double>(k) * tau_90));
    follower.on_clock_tick(frame, clock);
    const float cur_bpm = follower.tempo_bpm();
    // Critical damping: monotonically decreases, no undershoot below target!
    CHECK(cur_bpm <= prev_bpm + 0.05f);
    CHECK(cur_bpm >= target_bpm - 0.05f);
    prev_bpm = cur_bpm;
  }

  CHECK(std::abs(follower.tempo_bpm() - static_cast<float>(target_bpm)) < 0.1f);
  return 0;
}

int test_clock_lost_thresholds() {
  ClockFollower follower;
  TransportClock clock;
  follower.reset();
  clock.reset();

  // 120 BPM: tau = 1000 frames. 3 ticks = 3000 frames.
  // Minimum timeout is 150 ms (7200 frames at 48 kHz).
  // Expect threshold to be 7200 frames.
  CHECK(follower.timeout_frames() == 7200);

  follower.on_clock_tick(0, clock);
  CHECK(follower.locked());
  CHECK(!follower.clock_lost());

  // Advance by 7199 frames: not lost yet
  follower.process_block(7199, clock);
  CHECK(!follower.clock_lost());

  // Advance by 2 more frames (total 7201 > 7200): must be clock lost!
  follower.process_block(2, clock);
  CHECK(follower.clock_lost());

  // Test at 20.0 BPM: tau = 6000 frames. 3 ticks = 18000 frames > 7200 frames.
  ClockFollower slow_follower;
  TransportClock slow_clock;
  slow_follower.reset();
  slow_clock.reset();
  slow_follower.set_tempo_tenths(200);  // 20.0 BPM
  CHECK(slow_follower.timeout_frames() == 18000);

  slow_follower.on_clock_tick(0, slow_clock);
  slow_follower.process_block(17999, slow_clock);
  CHECK(!slow_follower.clock_lost());

  slow_follower.process_block(2, slow_clock);
  CHECK(slow_follower.clock_lost());

  return 0;
}

int test_start_continue_stop_semantics() {
  ClockFollower follower;
  TransportClock clock;
  follower.reset();
  clock.reset();

  // Start aligns transport clock to tick 0
  follower.on_start(1234, clock);
  CHECK(follower.is_running());
  CHECK(!follower.clock_lost());
  // Frame 1234 of transport clock is tick 0
  CHECK(clock.ticks_at(1234) == 0);

  // Run some ticks at 120 BPM
  for (std::size_t i = 1; i <= 10; ++i) {
    follower.on_clock_tick(1234 + i * 1000, clock);
    clock.advance(1000);
  }
  CHECK(clock.ticks_at(0) > 0);

  // Stop keeps last tempo and does NOT raise clock_lost even after a long timeout
  const float last_tempo = follower.tempo_bpm();
  follower.on_stop(clock.frames(), clock);
  CHECK(!follower.is_running());
  CHECK(!follower.clock_lost());

  // Advance by 20000 frames (> 7200 ms timeout)
  follower.process_block(20000, clock);
  CHECK(!follower.clock_lost());  // Must NOT raise clock lost after Stop!
  CHECK(std::abs(follower.tempo_bpm() - last_tempo) < 0.01f);

  // Position the clock at an unaligned tick (tick 33)
  clock.align_to_tick(33, 0);
  CHECK(clock.ticks_at(0) == 33);

  // Continue aligns grid to next clock tick (tick 40) without raising clock lost
  follower.on_continue(clock.frames(), clock);
  CHECK(follower.is_running());
  CHECK(!follower.clock_lost());
  // Step grid aligned to the next clock tick (40)
  CHECK(clock.ticks_at(0) == 40);

  return 0;
}

int test_unlocked_start_monotonicity() {
  ClockFollower follower;
  TransportClock clock;
  follower.reset();
  clock.reset();

  // Run initial frames before external clock connects
  clock.advance(5000);
  std::int64_t prev_pos = clock.position_units();
  CHECK(prev_pos > 0);

  // Incoming ticks arrive while unlocked (no prior Start message)
  for (std::size_t i = 0; i < 10; ++i) {
    const std::uint64_t tick_f = 5000 + i * 1000;
    while (clock.frames() < tick_f) {
      clock.advance(64);
      follower.process_block(64, clock);
      CHECK(clock.position_units() >= prev_pos);
      prev_pos = clock.position_units();
    }
    follower.on_clock_tick(tick_f, clock);
    CHECK(clock.position_units() >= prev_pos);  // Strictly non-decreasing!
    prev_pos = clock.position_units();
  }

  return 0;
}

int test_transport_clock_negative_units() {
  TransportClock clock;
  clock.reset();
  clock.align_to_tick(0, 32);  // sets units_ = -32 * tenths_
  CHECK(clock.position_units() < 0);
  CHECK(clock.ticks_at(32) == 0);
  CHECK(clock.ticks_at(10) == -1);
  CHECK(clock.ticks_at(0) == -2);  // Proper floor division, not 0!
  return 0;
}

int test_relock_without_double_trigger() {
  ClockFollower follower;
  TransportClock clock;
  follower.reset();
  clock.reset();

  follower.on_start(0, clock);
  for (std::size_t i = 0; i < 20; ++i) {
    follower.on_clock_tick(i * 1000, clock);
    clock.advance(1000);
  }
  CHECK(!follower.clock_lost());

  // Drop clock ticks for 10000 frames (> 7200 timeout)
  for (std::size_t b = 0; b < 160; ++b) {
    clock.advance(64);
    follower.process_block(64, clock);
  }
  CHECK(follower.clock_lost());

  // Clock resumes: verify it clears clock_lost and position NEVER goes backward
  std::int64_t prev_pos = clock.position_units();
  const std::uint64_t resume_frame = clock.frames();
  follower.on_clock_tick(resume_frame, clock);
  CHECK(!follower.clock_lost());
  CHECK(clock.position_units() >= prev_pos);  // Strictly monotonic!

  // Continue running ticks
  for (std::size_t i = 1; i <= 20; ++i) {
    const std::uint64_t tick_f = resume_frame + i * 1000;
    while (clock.frames() < tick_f) {
      clock.advance(64);
      follower.process_block(64, clock);
      CHECK(clock.position_units() >= prev_pos);
      prev_pos = clock.position_units();
    }
    follower.on_clock_tick(tick_f, clock);
    CHECK(clock.position_units() >= prev_pos);
    prev_pos = clock.position_units();
  }

  return 0;
}

int test_block_renderer_integration() {
  BlockRenderer br;
  CHECK(br.clock_source() == ClockSource::kInternal);
  CHECK(!br.last_values().load_clock_lost());

  br.set_clock_source(ClockSource::kDin);
  CHECK(br.clock_source() == ClockSource::kDin);

  float left[64]{}, right[64]{};

  // Submit Start event at frame 0
  CHECK(br.submit({make_start(), 0}));
  br.process_block(left, right);
  CHECK(!br.last_values().load_clock_lost());

  // Submit clock ticks at 120 BPM (~1 tick every ~15.6 blocks = 1000 frames)
  for (std::uint64_t f = 0; f < 30000; f += 1000) {
    // Determine which block `f` falls into
    while (br.clock().frames() + 64 <= f) {
      br.process_block(left, right);
    }
    const std::uint32_t off = static_cast<std::uint32_t>(f - br.clock().frames());
    CHECK(br.submit({make_timing_clock(), off}));
    br.process_block(left, right);
  }

  // Follower must be locked and near 120 BPM
  CHECK(std::abs(br.tempo_bpm() - 120.0f) < 0.5f);
  CHECK(!br.last_values().load_clock_lost());

  // Stop sending clock ticks for > 150 ms (7200 frames = 113 blocks)
  for (std::size_t b = 0; b < 120; ++b) {
    br.process_block(left, right);
  }

  // Must have latched clock lost!
  CHECK(br.clock_follower().clock_lost());
  CHECK(br.last_values().load_clock_lost());

  // Clock resumes
  CHECK(br.submit({make_timing_clock(), 0}));
  br.process_block(left, right);
  CHECK(!br.clock_follower().clock_lost());
  CHECK(!br.last_values().load_clock_lost());

  // Stop event: keeps running without clock lost
  CHECK(br.submit({make_stop(), 0}));
  br.process_block(left, right);
  for (std::size_t b = 0; b < 120; ++b) {
    br.process_block(left, right);
  }
  CHECK(!br.clock_follower().clock_lost());  // Stop must NOT raise clock lost!

  // Continue event
  CHECK(br.submit({make_continue(), 0}));
  br.process_block(left, right);
  CHECK(br.clock_follower().is_running());

  return 0;
}

}  // namespace

int main() {
  if (test_dll_tracking_within_2_beats() != 0) return 1;
  if (test_step_down_tempo_tracking() != 0) return 1;
  if (test_clock_lost_thresholds() != 0) return 1;
  if (test_start_continue_stop_semantics() != 0) return 1;
  if (test_unlocked_start_monotonicity() != 0) return 1;
  if (test_transport_clock_negative_units() != 0) return 1;
  if (test_relock_without_double_trigger() != 0) return 1;
  if (test_block_renderer_integration() != 0) return 1;
  std::printf("PASS clock_follower_tests\n");
  return 0;
}
