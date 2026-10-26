#include "velora/host/head_memory.hpp"

#include <sys/mman.h>
#include <unistd.h>

namespace velora::host {

namespace {
std::atomic<std::uint64_t> g_alloc{0}, g_free{0}, g_live_bytes{0}, g_locked{0};
}

HeadMemoryStats head_memory_stats() noexcept {
  HeadMemoryStats s;
  const std::uint64_t frees = g_free.load(std::memory_order_relaxed);  // frees first: a concurrent alloc/free pair can then only make live_buffers larger, never negative
  s.allocations = g_alloc.load(std::memory_order_relaxed);
  s.frees = frees;
  s.live_buffers = s.allocations > frees ? s.allocations - frees : 0;
  s.live_bytes = g_live_bytes.load(std::memory_order_relaxed);
  s.locked_buffers = g_locked.load(std::memory_order_relaxed);
  return s;
}

std::size_t head_page_bytes() noexcept {
  const long pg = ::sysconf(_SC_PAGESIZE);
  return pg > 0 ? static_cast<std::size_t>(pg) : 4096;
}

std::size_t head_arena_bytes(std::size_t aligned_payload) noexcept {
  if (aligned_payload == 0) return 0;
  const std::size_t page = head_page_bytes();
  return (aligned_payload + page - 1) / page * page;
}

std::unique_ptr<HeadBuffer> HeadBuffer::allocate(std::size_t bytes, const HeadMemoryOptions& opt, Status& status) {
  const long pg = ::sysconf(_SC_PAGESIZE);
  const std::size_t page = pg > 0 ? static_cast<std::size_t>(pg) : 4096;
  const std::size_t mapped = ((bytes == 0 ? 1 : bytes) + page - 1) / page * page;
  void* mem = ::mmap(nullptr, mapped, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (mem == MAP_FAILED) {
    status = Status::kNoMemory;
    return nullptr;
  }
#if defined(MADV_DONTDUMP)
  (void)::madvise(mem, mapped, MADV_DONTDUMP);  // decoded library content stays out of core files (Linux; macOS has no per-region equivalent)
#endif
  const bool locked = opt.lock_fn ? opt.lock_fn(mem, mapped) : ::mlock(mem, mapped) == 0;
  if (!locked && !opt.allow_unlocked) {
    ::munmap(mem, mapped);
    status = Status::kLockFailed;
    return nullptr;
  }
  // Pre-fault: write one byte in every page (after the lock), so no page of an instrument that is "ready" is ever faulted in by the audio thread.
  volatile std::uint8_t* p = static_cast<volatile std::uint8_t*>(mem);
  for (std::size_t off = 0; off < mapped; off += page) p[off] = 0;

  std::unique_ptr<HeadBuffer> b(new HeadBuffer());
  b->mem_ = mem;
  b->mapped_ = mapped;
  b->locked_ = locked;
  b->prefaulted_ = true;
  if (opt.unlock_fn) b->unlock_fn_ = opt.unlock_fn;
  g_alloc.fetch_add(1, std::memory_order_relaxed);
  g_live_bytes.fetch_add(mapped, std::memory_order_relaxed);
  if (locked) g_locked.fetch_add(1, std::memory_order_relaxed);
  status = Status::kOk;
  return b;
}

HeadBuffer::~HeadBuffer() {
  if (mem_ == nullptr) return;
  if (locked_) {
    if (unlock_fn_) unlock_fn_(mem_, mapped_);
    else ::munlock(mem_, mapped_);
    g_locked.fetch_sub(1, std::memory_order_relaxed);
  }
  ::munmap(mem_, mapped_);
  g_live_bytes.fetch_sub(mapped_, std::memory_order_relaxed);
  g_free.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace velora::host
