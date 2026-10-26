#pragma once
// Thread pinning port (Story 1.18). The core's voice-render worker pool asks this port once per worker, at
// worker start (never per block), to pin the calling thread to a CPU. The default is a no-op so every host
// (macOS, tests) works unchanged; the Linux implementation uses sched_setaffinity and is compiled only on Linux.
#include <cstdint>

#if defined(__linux__)
#include <sched.h>
#endif

namespace velora::ports {

class ThreadAffinity {
 public:
  virtual ~ThreadAffinity() = default;
  // Pins the CALLING thread to logical CPU `cpu`. True when pinned. Called from the thread itself, outside the audio path.
  virtual bool pin_current_thread(std::uint32_t cpu) noexcept = 0;
};

// Does nothing and reports that nothing was pinned.
class NoopThreadAffinity final : public ThreadAffinity {
 public:
  bool pin_current_thread(std::uint32_t) noexcept override { return false; }
};

#if defined(__linux__)
// sched_setaffinity(0, ...) on the calling thread. Not used by the tests.
class LinuxThreadAffinity final : public ThreadAffinity {
 public:
  bool pin_current_thread(std::uint32_t cpu) noexcept override {
    if (cpu >= CPU_SETSIZE) return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0;
  }
};
#endif

}  // namespace velora::ports
