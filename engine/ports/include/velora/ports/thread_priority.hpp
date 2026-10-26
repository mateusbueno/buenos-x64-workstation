#pragma once
// Thread priority port (Story 1.17). The I/O edge threads that stream sample tails from the SSD must run BELOW the audio thread: they are never real time and must
// not be able to preempt the audio callback. The host's StreamManager asks this port once per I/O thread, at thread start (never per read), to apply the role's
// priority to the calling thread. The default is a no-op so every host (tests, CI) works unchanged.
//
// Implementations live in engine/host (host/thread_priority.hpp): macOS maps the I/O role to a user-initiated QoS class (the CoreAudio thread is a real-time thread and
// stays above it); Linux (board, documented) puts the audio threads on SCHED_FIFO at priority P and the I/O threads on SCHED_FIFO at P - 1 or, without CAP_SYS_NICE,
// on the normal class with a nice value, on cores isolated from the audio cores (the isolation itself is dev-board work).
#include <cstdint>

namespace velora::ports {

enum class ThreadRole : std::uint8_t { kAudio = 0, kIoEdge = 1 };

class ThreadPriority {
 public:
  virtual ~ThreadPriority() = default;
  // Applies the priority of `role` to the CALLING thread. True when the priority was applied. Called from the thread itself, outside the audio path.
  virtual bool apply_current_thread(ThreadRole role) noexcept = 0;
};

class NoopThreadPriority final : public ThreadPriority {
 public:
  bool apply_current_thread(ThreadRole) noexcept override { return false; }
};

}  // namespace velora::ports
