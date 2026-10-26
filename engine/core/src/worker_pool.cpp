#include "velora/core/worker_pool.hpp"

namespace velora::core {

namespace {
inline void cpu_pause() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  asm volatile("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
  asm volatile("yield" ::: "memory");
#else
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}
constexpr std::uint32_t kIdleSpins = 4000;
constexpr std::uint32_t kCallerSpins = 2000;  // before the caller helps with partitions nobody has claimed

// The floating-point control state that changes arithmetic results: x86 MXCSR (FTZ, DAZ, rounding), ARM64 FPCR (FZ, rounding, ...).
// No-op elsewhere.
inline std::uint64_t fp_get() noexcept {
#if defined(__SSE__) || defined(__x86_64__)
  std::uint32_t v = 0;
  asm volatile("stmxcsr %0" : "=m"(v));
  return v;
#elif defined(__aarch64__)
  std::uint64_t v = 0;
  asm volatile("mrs %0, fpcr" : "=r"(v));
  return v;
#else
  return 0;
#endif
}
inline void fp_set(std::uint64_t v) noexcept {
#if defined(__SSE__) || defined(__x86_64__)
  const std::uint32_t m = static_cast<std::uint32_t>(v);
  asm volatile("ldmxcsr %0" : : "m"(m));
#elif defined(__aarch64__)
  asm volatile("msr fpcr, %0" : : "r"(v));
#else
  (void)v;
#endif
}
}  // namespace

WorkerPool::StartResult WorkerPool::start(std::uint32_t threads, ports::ThreadAffinity* affinity, const std::uint32_t* cpus) noexcept {
  stop();
  StartResult res = StartResult::kOk;
  if (threads != 1 && threads != kPartitions) {
    res = StartResult::kClamped;
    threads = threads > kPartitions ? kPartitions : 1;
  }
  if (threads == 1) {
    threads_ = 1;
    return res;
  }
  std::uint32_t cpu[kPartitions];
  const std::uint32_t hw = std::thread::hardware_concurrency();
  for (std::uint32_t p = 0; p < kPartitions; ++p) {
    cpu[p] = cpus ? cpus[p] : p;
    if (affinity && hw != 0 && cpu[p] >= hw) return StartResult::kBadCpu;  // nothing started: inline execution
  }
  stop_.store(false, std::memory_order_relaxed);
  done_.store(0, std::memory_order_relaxed);
  pinned_.store(0, std::memory_order_relaxed);
  pin_attempted_.store(0, std::memory_order_relaxed);
  state_.store(kExhausted, std::memory_order_release);  // nothing to claim until run()
  std::uint32_t started = 0;
  try {
    for (; started < kPartitions; ++started) {
      const std::uint32_t c = cpu[started];
      workers_[started] = std::thread([this, affinity, c] { worker_main(affinity, c); });
    }
  } catch (...) {
    stop_.store(true, std::memory_order_release);  // roll back: the workers that did start exit, the rest never existed
    for (std::uint32_t i = 0; i < started; ++i)
      if (workers_[i].joinable()) workers_[i].join();
    threads_ = 1;
    return StartResult::kStartFailed;
  }
  threads_ = kPartitions;
  while (pin_attempted_.load(std::memory_order_acquire) != kPartitions) std::this_thread::yield();  // setup-time wait for the pin attempts
  if (affinity && pinned_.load(std::memory_order_relaxed) != kPartitions && res == StartResult::kOk) res = StartResult::kPinFailed;
  return res;
}

void WorkerPool::stop() noexcept {
  stop_.store(true, std::memory_order_release);  // workers check it only while idle: a claimed partition always completes
  for (auto& w : workers_)
    if (w.joinable()) w.join();
  threads_ = 1;
}

bool WorkerPool::claim(std::uint32_t& partition) noexcept {
  std::uint64_t s = state_.load(std::memory_order_acquire);
  for (;;) {
    const std::uint32_t n = static_cast<std::uint32_t>(s & 0xFFFFFFFFu);
    if (n >= kPartitions) return false;
    if (state_.compare_exchange_weak(s, s + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
      partition = n;
      return true;
    }
  }
}

void WorkerPool::execute(std::uint32_t partition) noexcept {
  // task_, ctx_ and fp_ were written before the release store of state_ that this thread's successful claim read.
  const std::uint64_t want = fp_;
  if (fp_get() != want) fp_set(want);
  task_(ctx_, partition);
  done_.fetch_add(1, std::memory_order_acq_rel);
}

void WorkerPool::worker_main(ports::ThreadAffinity* affinity, std::uint32_t cpu) noexcept {
  if (affinity && affinity->pin_current_thread(cpu)) pinned_.fetch_add(1, std::memory_order_relaxed);
  pin_attempted_.fetch_add(1, std::memory_order_release);
  std::uint32_t spins = 0;
  for (;;) {
    std::uint32_t p;
    if (claim(p)) {
      execute(p);
      spins = 0;
      continue;
    }
    if (stop_.load(std::memory_order_acquire)) return;  // only when idle: never abandons a claimed partition
    if (++spins < kIdleSpins) cpu_pause();
    else std::this_thread::yield();  // idle wait on the worker thread only; the audio thread never yields
  }
}

void WorkerPool::run(Task task, void* ctx) noexcept {
  if (threads_ == 1) {
    for (std::uint32_t p = 0; p < kPartitions; ++p) task(ctx, p);
    return;
  }
  task_ = task;
  ctx_ = ctx;
  fp_ = fp_get();
  done_.store(0, std::memory_order_relaxed);
  const std::uint64_t gen = (state_.load(std::memory_order_relaxed) >> 32) + 1;
  state_.store(gen << 32, std::memory_order_release);  // publishes task_, ctx_, fp_, done_ = 0; 0 partitions claimed
  for (std::uint32_t i = 0; i < kCallerSpins && done_.load(std::memory_order_acquire) != kPartitions; ++i) cpu_pause();
  std::uint32_t p;
  while (claim(p)) {  // help: run whatever no worker has picked up (the caller's own FP state is the published one)
    task(ctx, p);
    done_.fetch_add(1, std::memory_order_acq_rel);
  }
  while (done_.load(std::memory_order_acquire) != kPartitions) cpu_pause();  // only partitions a worker is already running
}

}  // namespace velora::core
