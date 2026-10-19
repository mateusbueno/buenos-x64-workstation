#pragma once

#include <array>
#include <atomic>
#include <cstddef>

#include "velora/core/rt.hpp"

namespace velora::core {

// Fixed-capacity lock-free single-producer/single-consumer ring. No allocation after
// construction. Capacity must be a power of two; usable slots == Capacity.
template <typename T, std::size_t Capacity>
class SpscQueue {
  static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
  static_assert(std::atomic<std::size_t>::is_always_lock_free);

 public:
  // Producer side. Returns false when full (nothing already queued is lost).
  bool push(const T& v) noexcept VELORA_NONBLOCKING {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    if (head - tail == Capacity) return false;
    buf_[head & (Capacity - 1)] = v;
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  // Consumer side. Returns false when empty.
  bool pop(T& out) noexcept VELORA_NONBLOCKING {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t head = head_.load(std::memory_order_acquire);
    if (head == tail) return false;
    out = buf_[tail & (Capacity - 1)];
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  // Producer side: how many elements are queued right now (exact for the producer, who owns `head`; the consumer may only have made it smaller).
  std::size_t size() const noexcept {
    return head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_acquire);
  }

  static constexpr std::size_t capacity() noexcept { return Capacity; }

 private:
  std::array<T, Capacity> buf_{};
  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
};

}  // namespace velora::core
