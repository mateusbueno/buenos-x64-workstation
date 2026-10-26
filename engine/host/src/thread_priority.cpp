#include "velora/host/thread_priority.hpp"

#include <pthread.h>

#if defined(__APPLE__)
#include <pthread/qos.h>
#elif defined(__linux__)
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace velora::host {

bool PosixThreadPriority::apply_current_thread(velora::ports::ThreadRole role) noexcept {
#if defined(__APPLE__)
  if (role != velora::ports::ThreadRole::kIoEdge) return false;  // CoreAudio owns the audio thread
  return pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0) == 0;
#elif defined(__linux__)
  const int audio = opt_.audio_priority;
  const int prio = role == velora::ports::ThreadRole::kAudio ? audio : audio - opt_.io_below;
  sched_param sp{};
  sp.sched_priority = prio < 1 ? 1 : prio;
  if (sched_setscheduler(0, SCHED_FIFO, &sp) == 0) return true;
  if (role == velora::ports::ThreadRole::kIoEdge) {
    const pid_t tid = static_cast<pid_t>(::syscall(SYS_gettid));
    (void)setpriority(PRIO_PROCESS, static_cast<id_t>(tid), opt_.io_nice);  // fallback: normal class, lower priority (reported as not applied)
  }
  return false;
#else
  (void)role;
  return false;
#endif
}

}  // namespace velora::host
