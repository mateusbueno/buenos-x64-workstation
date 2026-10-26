// WorkerPool tests (Story 1.18): fixed 3-way partitioning, inline vs threaded execution, clamping, pinning hook, clean shutdown.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "velora/core/worker_pool.hpp"

using namespace velora::core;

static int g_fail = 0;
#define CHECK(c)                                                \
  do {                                                          \
    if (!(c)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);  \
      ++g_fail;                                                 \
    }                                                           \
  } while (0)

namespace {

struct Ctx {
  std::array<std::atomic<std::uint32_t>, WorkerPool::kPartitions> runs{};
  std::array<std::uint64_t, WorkerPool::kPartitions> sum{};  // each partition only touches its own element
  std::atomic<std::uint32_t> order_violations{0};
};

void task(void* c, std::uint32_t p) noexcept {
  auto* x = static_cast<Ctx*>(c);
  x->runs[p].fetch_add(1, std::memory_order_relaxed);
  x->sum[p] += (p + 1) * 7u;
}

struct RecordingAffinity final : velora::ports::ThreadAffinity {
  std::atomic<std::uint32_t> calls{0};
  std::atomic<std::uint32_t> cpu_mask{0};
  bool pin_current_thread(std::uint32_t cpu) noexcept override {
    calls.fetch_add(1);
    cpu_mask.fetch_or(1u << cpu);
    return true;
  }
};

// Flush-to-zero control of the calling thread (x86 MXCSR bit 15, ARM64 FPCR bit 24); a no-op (returns false) on other architectures.
bool set_ftz(bool on, std::uint64_t& saved) {
#if defined(__SSE__) || defined(__x86_64__)
  std::uint32_t v = 0;
  asm volatile("stmxcsr %0" : "=m"(v));
  saved = v;
  v = on ? (v | 0x8000u) : (v & ~0x8000u);
  asm volatile("ldmxcsr %0" : : "m"(v));
  return true;
#elif defined(__aarch64__)
  std::uint64_t v = 0;
  asm volatile("mrs %0, fpcr" : "=r"(v));
  saved = v;
  v = on ? (v | (1ull << 24)) : (v & ~(1ull << 24));
  asm volatile("msr fpcr, %0" : : "r"(v));
  return true;
#else
  (void)on;
  (void)saved;
  return false;
#endif
}
void restore_fp(std::uint64_t saved) {
#if defined(__SSE__) || defined(__x86_64__)
  const std::uint32_t v = static_cast<std::uint32_t>(saved);
  asm volatile("ldmxcsr %0" : : "m"(v));
#elif defined(__aarch64__)
  asm volatile("msr fpcr, %0" : : "r"(saved));
#else
  (void)saved;
#endif
}

struct FpCtx {
  std::array<std::uint32_t, WorkerPool::kPartitions> bits{};
};
// A product that is denormal in float: 0 under flush-to-zero, non-zero otherwise.
void denormal_task(void* c, std::uint32_t p) noexcept {
  volatile float a = 1.0e-30f, b = 1.0e-10f;
  const float r = a * b;
  std::memcpy(&static_cast<FpCtx*>(c)->bits[p], &r, sizeof(r));
}

void run_many(WorkerPool& pool, Ctx& c, int n) {
  for (int i = 0; i < n; ++i) pool.run(&task, &c);
}

}  // namespace

int main() {
  // inline (1 thread): partitions run in order on the caller
  {
    WorkerPool pool;
    CHECK(pool.threads() == 1);
    CHECK(pool.start(1) == WorkerPool::StartResult::kOk);
    CHECK(pool.threads() == 1);
    Ctx c;
    run_many(pool, c, 1000);
    for (auto& r : c.runs) CHECK(r.load() == 1000);
    CHECK(c.sum[0] == 7000 && c.sum[1] == 14000 && c.sum[2] == 21000);
  }
  // 3 threads: every partition runs exactly once per run(), and run() returns only after all are done (the barrier)
  {
    WorkerPool pool;
    CHECK(pool.start(3) == WorkerPool::StartResult::kOk);
    CHECK(pool.threads() == 3);
    Ctx c;
    for (int i = 0; i < 20000; ++i) {
      pool.run(&task, &c);
      for (auto& r : c.runs) {
        if (r.load(std::memory_order_relaxed) != static_cast<std::uint32_t>(i + 1)) ++c.order_violations;
      }
    }
    CHECK(c.order_violations.load() == 0);
    CHECK(c.sum[0] == 7ull * 20000 && c.sum[1] == 14ull * 20000 && c.sum[2] == 21ull * 20000);
    pool.stop();
    pool.stop();  // idempotent
    CHECK(pool.threads() == 1);
    Ctx c2;
    run_many(pool, c2, 10);  // after stop the pool runs inline
    for (auto& r : c2.runs) CHECK(r.load() == 10);
  }
  // clamping: 0..2 -> 1, above 3 -> 3
  {
    WorkerPool pool;
    CHECK(pool.start(0) == WorkerPool::StartResult::kClamped && pool.threads() == 1);
    CHECK(pool.start(2) == WorkerPool::StartResult::kClamped && pool.threads() == 1);
    CHECK(pool.start(7) == WorkerPool::StartResult::kClamped && pool.threads() == 3);
    Ctx c;
    run_many(pool, c, 100);
    for (auto& r : c.runs) CHECK(r.load() == 100);
    CHECK(pool.start(3) == WorkerPool::StartResult::kOk && pool.threads() == 3);  // restart while running
    run_many(pool, c, 100);
    for (auto& r : c.runs) CHECK(r.load() == 200);
  }
  // pinning hook: asked once per worker at start with cpus[worker], never per block; failures and bad cpus are reported
  {
    const unsigned hw = std::thread::hardware_concurrency();
    const std::uint32_t cpus[3] = {0, hw >= 2 ? 1u : 0u, 0};
    RecordingAffinity aff;
    WorkerPool pool;
    CHECK(pool.start(3, &aff, cpus) == WorkerPool::StartResult::kOk);
    Ctx c;
    run_many(pool, c, 500);
    pool.stop();
    CHECK(aff.calls.load() == 3);
    CHECK(aff.cpu_mask.load() == (1u | (hw >= 2 ? 2u : 0u)));
    CHECK(pool.pinned_workers() == 3);
    RecordingAffinity aff1;
    pool.start(1, &aff1);  // inline: no threads, nothing pinned
    run_many(pool, c, 5);
    CHECK(aff1.calls.load() == 0);
    velora::ports::NoopThreadAffinity noop;  // the port refuses: the pool runs, and says so
    CHECK(pool.start(3, &noop) == WorkerPool::StartResult::kPinFailed);
    CHECK(pool.threads() == 3);
    run_many(pool, c, 5);
    pool.stop();
    CHECK(pool.pinned_workers() == 0);
    CHECK(pool.start(3) == WorkerPool::StartResult::kOk);  // no affinity requested: nothing to fail
    pool.stop();
    if (hw != 0) {  // a cpu index outside the hardware range: nothing started
      const std::uint32_t bad[3] = {0, 1, hw + 5};
      RecordingAffinity aff2;
      CHECK(pool.start(3, &aff2, bad) == WorkerPool::StartResult::kBadCpu);
      CHECK(pool.threads() == 1 && aff2.calls.load() == 0);
      Ctx c3;
      run_many(pool, c3, 3);  // still usable, inline
      for (auto& r : c3.runs) CHECK(r.load() == 3);
    }
  }
  // the caller's floating-point control state (flush-to-zero) reaches the workers: inline and threaded runs are bit-identical
  {
    WorkerPool inl, thr;
    inl.start(1);
    thr.start(3);
    std::uint64_t saved = 0;
    if (set_ftz(true, saved)) {
      FpCtx a, b;
      for (int i = 0; i < 200; ++i) {  // many runs: different workers take the partitions
        inl.run(&denormal_task, &a);
        thr.run(&denormal_task, &b);
        CHECK(a.bits == b.bits);
        CHECK(a.bits[0] == 0 && a.bits[1] == 0 && a.bits[2] == 0);  // FTZ was honoured everywhere
      }
      restore_fp(saved);
      FpCtx c, d;
      inl.run(&denormal_task, &c);  // FTZ off again: the denormal survives, in both
      thr.run(&denormal_task, &d);
      CHECK(c.bits == d.bits);
      CHECK(c.bits[0] != 0);
    } else {
      std::printf("worker_pool_tests: FTZ control not available on this architecture, FP propagation not tested\n");
    }
  }
  // shutdown with workers idle (never ran) and destruction while running: joins, no hang
  {
    WorkerPool pool;
    pool.start(3);
  }
  {
    auto pool = std::make_unique<WorkerPool>();
    pool->start(3);
    Ctx c;
    run_many(*pool, c, 10);
    pool.reset();
  }
  std::printf(g_fail ? "worker_pool_tests: FAILED (%d)\n" : "worker_pool_tests: ok\n", g_fail);
  return g_fail ? 1 : 0;
}
