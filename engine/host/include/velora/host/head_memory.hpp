#pragma once
// Locked, pre-faulted memory for instrument heads (Story 1.16). POSIX only; allocation, mlock and the page touch run on the LOADING thread, never on the audio
// thread (which only reads the memory). engine/core never sees any of this: it is handed plain pointers through the sample-source port.
//
// A buffer is an anonymous mapping, mlock'ed, then written once per page so every page is resident before the instrument becomes playable. When mlock fails
// (RLIMIT_MEMLOCK too small for the head budget: raise `ulimit -l`, on the board LimitMEMLOCK=infinity in the engine service) the buffer is refused unless
// `allow_unlocked` is set (a development option), in which case it is still pre-faulted but reported as locked = false. Destroying the buffer munlocks and unmaps it.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace velora::host {

struct HeadMemoryOptions {
  bool allow_unlocked = false;  // dev only: keep an unlocked (but pre-faulted) head when mlock fails, and record it
  // Test hooks: replace mlock / munlock (return false = the lock failed). Unset = ::mlock / ::munlock.
  std::function<bool(void* p, std::size_t bytes)> lock_fn;
  std::function<void(void* p, std::size_t bytes)> unlock_fn;
};

// Process-wide counters, so a test can prove that every head that was allocated is freed (no leak).
struct HeadMemoryStats {
  std::uint64_t allocations = 0, frees = 0;      // buffers created / destroyed
  std::uint64_t live_buffers = 0, live_bytes = 0;  // mapped now (page rounded)
  std::uint64_t locked_buffers = 0;              // live buffers whose mlock succeeded
};
HeadMemoryStats head_memory_stats() noexcept;

// Arena layout of an instrument's heads: ONE buffer per instrument, each sample's head at a 64 byte aligned offset inside it; only the arena is page rounded and
// mlocked (one mapping and one mlock per instrument, not per sample). head_align() rounds a head's bytes up to 64; head_arena_bytes() is the mapped size of an arena
// holding `aligned_payload` bytes (0 stays 0). The head budget counts the arena size, so the estimate equals the real mapped size.
inline constexpr std::size_t kHeadAlignBytes = 64;
inline constexpr std::size_t head_align(std::size_t bytes) noexcept { return (bytes + kHeadAlignBytes - 1) / kHeadAlignBytes * kHeadAlignBytes; }
std::size_t head_page_bytes() noexcept;
std::size_t head_arena_bytes(std::size_t aligned_payload) noexcept;

class HeadBuffer {
 public:
  enum class Status : std::uint8_t { kOk = 0, kLockFailed, kNoMemory };
  // `bytes` rounded up to whole pages (at least one). nullptr with `status` set on failure; nothing stays mapped or locked then.
  static std::unique_ptr<HeadBuffer> allocate(std::size_t bytes, const HeadMemoryOptions& opt, Status& status);
  ~HeadBuffer();
  HeadBuffer(const HeadBuffer&) = delete;
  HeadBuffer& operator=(const HeadBuffer&) = delete;

  float* floats() noexcept { return static_cast<float*>(mem_); }
  const float* floats() const noexcept { return static_cast<const float*>(mem_); }
  std::size_t mapped_bytes() const noexcept { return mapped_; }
  bool locked() const noexcept { return locked_; }
  bool prefaulted() const noexcept { return prefaulted_; }

 private:
  HeadBuffer() = default;
  void* mem_ = nullptr;
  std::size_t mapped_ = 0;
  bool locked_ = false, prefaulted_ = false;
  std::function<void(void*, std::size_t)> unlock_fn_;
};

}  // namespace velora::host
