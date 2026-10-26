#pragma once
// Thread priority adapter (Story 1.17) for the I/O edge threads of the stream manager, behind ports::ThreadPriority.
//
//   macOS (the Mac prototype): the I/O role maps to the user-initiated QoS class; the CoreAudio render thread is a real-time thread and always runs above it. The audio role is a
//     no-op here (CoreAudio owns its thread).
//   Linux (the board; compiled everywhere, exercised only on the board): the audio threads run SCHED_FIFO at `audio_priority` and the I/O edge threads SCHED_FIFO at
//     `audio_priority - io_below`, so they can never preempt the audio threads; without CAP_SYS_NICE / an RLIMIT_RTPRIO the I/O role falls back to the normal class with
//     `io_nice` (and the call reports false: the priority of a fallback is not the real-time one). Running them on cores isolated from the audio cores (isolcpus, affinity) is
//     dev-board work (ports::ThreadAffinity), as is the measurement of the result.
#include <cstdint>

#include "velora/ports/thread_priority.hpp"

namespace velora::host {

struct PosixThreadPriorityOptions {
  int audio_priority = 80;  // Linux SCHED_FIFO priority of the audio threads
  int io_below = 10;        // the I/O threads are this much lower
  int io_nice = 5;          // the fallback nice value of the I/O role
};

class PosixThreadPriority final : public velora::ports::ThreadPriority {
 public:
  PosixThreadPriority() = default;
  explicit PosixThreadPriority(PosixThreadPriorityOptions o) : opt_(o) {}
  bool apply_current_thread(velora::ports::ThreadRole role) noexcept override;

 private:
  PosixThreadPriorityOptions opt_;
};

}  // namespace velora::host
