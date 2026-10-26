#pragma once
// WorkerPool (Story 1.18): a fixed-partition fork/join pool for the parallel voice render.
//
// The work of a block is split into kPartitions (3) partitions. run() executes task(ctx, p) for p = 0..2 and returns when all
// three are done (the barrier). With 1 execution thread the three partitions run inline on the caller in order 0, 1, 2; with 3
// threads each partition runs on its own worker. The partition set is the same either way, so a task whose partitions write
// disjoint state is bit-identical across both modes.
//
// Real-time contract: run() uses atomics only (no mutex, no condition variable, no allocation, no syscall). Partitions are CLAIMED
// through one atomic word (generation << 32 | claimed count): a worker claims the next unclaimed partition and runs it, so which
// thread runs a partition never matters (partitions write disjoint state). After a bounded spin the caller claims and runs any
// partition no worker has picked up yet, so a worker that was preempted or never got scheduled cannot stall the audio thread
// on work nobody started (a partition a worker already claimed is still waited for). Idle workers poll with a CPU pause, then
// std::this_thread::yield() between polls so an oversubscribed host is not starved. The caller's floating-point control state
// (x86 MXCSR / ARM FPCR: flush-to-zero, denormals-are-zero, rounding) is copied to the executing worker for every partition, so
// inline and threaded runs compute bit-identically. Threads are created and pinned (through the ports::ThreadAffinity hook, once
// per worker at start) in start(), which is setup-time and never throws.
// start(), stop() and run() must all be called from one thread (the audio / setup thread); stop() is never concurrent with run().
#include <atomic>
#include <cstdint>
#include <thread>

#include "velora/core/rt.hpp"
#include "velora/ports/thread_affinity.hpp"

namespace velora::core {

class WorkerPool {
 public:
  static constexpr std::uint32_t kPartitions = 3;
  using Task = void (*)(void* ctx, std::uint32_t partition) noexcept;

  // kOk: the requested count (1 or 3) is in effect (and every requested pin succeeded). kClamped: the request was not 1 or 3 and was
  // clamped (0..2 -> 1, above 3 -> 3). kBadCpu: a cpu index is outside the hardware range; nothing was started (the pool runs inline).
  // kPinFailed: the workers run but at least one pin request was refused (see pinned_workers()). kStartFailed: a worker thread could
  // not be created; the pool was rolled back to inline execution.
  enum class StartResult : std::uint8_t { kOk, kClamped, kBadCpu, kPinFailed, kStartFailed };

  WorkerPool() = default;
  ~WorkerPool() { stop(); }
  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;

  // Setup-time. (Re)starts the pool with `threads` execution threads. `affinity` (may be null = no pinning) is asked once per worker
  // to pin it to cpus[worker] (`cpus` may be null: then CPU = worker index); the cpu indices are validated against
  // std::thread::hardware_concurrency() when that is known. Returns how the request was resolved; threads() is the effective count.
  // Returns after every worker has attempted its pin. Does not throw.
  StartResult start(std::uint32_t threads, ports::ThreadAffinity* affinity = nullptr, const std::uint32_t* cpus = nullptr) noexcept;
  // Joins the workers (no hang: they poll the stop flag). Safe to call twice; the pool then runs inline (1 thread).
  void stop() noexcept;

  std::uint32_t threads() const noexcept { return threads_; }
  // Number of workers that managed to pin themselves (0 with the no-op port).
  std::uint32_t pinned_workers() const noexcept { return pinned_.load(std::memory_order_relaxed); }

  // Runs the three partitions and waits for all of them. Real-time safe.
  void run(Task task, void* ctx) noexcept VELORA_NONBLOCKING;

 private:
  void worker_main(ports::ThreadAffinity* affinity, std::uint32_t cpu) noexcept;
  // Claims the next unclaimed partition of the current run; false when all three are claimed. On success task_/ctx_/fp_ are valid.
  bool claim(std::uint32_t& partition) noexcept;
  void execute(std::uint32_t partition) noexcept;

  static constexpr std::size_t kLine = 64;
  std::uint32_t threads_ = 1;
  std::thread workers_[kPartitions];
  // Published by run() before the state store (release), read by a claimer after its successful claim (acquire).
  Task task_ = nullptr;
  void* ctx_ = nullptr;
  std::uint64_t fp_ = 0;
  alignas(kLine) std::atomic<std::uint64_t> state_{kExhausted};  // generation << 32 | partitions claimed so far
  alignas(kLine) std::atomic<std::uint32_t> done_{0};
  alignas(kLine) std::atomic<bool> stop_{false};
  std::atomic<std::uint32_t> pinned_{0};
  std::atomic<std::uint32_t> pin_attempted_{0};
  static constexpr std::uint64_t kExhausted = kPartitions;
};

}  // namespace velora::core
