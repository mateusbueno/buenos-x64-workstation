#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>

#include "velora/core/rt.hpp"

namespace velora::core {

// Pre-allocated bump arena. The backing block is allocated once in the constructor (non-RT);
// allocate()/reset() never touch the heap or lock, so they are safe on the render path.
class Arena {
 public:
  explicit Arena(std::size_t bytes) : storage_(new std::byte[bytes]), size_(bytes) {}

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  Arena(Arena&&) = delete;
  Arena& operator=(Arena&&) = delete;

  // Returns nullptr when exhausted, or when align is 0 / not a power of two.
  void* allocate(std::size_t bytes, std::size_t align = alignof(std::max_align_t)) noexcept VELORA_NONBLOCKING {
    if (align == 0 || (align & (align - 1)) != 0) return nullptr;
    const auto base = reinterpret_cast<std::uintptr_t>(storage_.get());
    const std::uintptr_t cur = (base + used_ + align - 1) & ~(static_cast<std::uintptr_t>(align) - 1);
    const std::size_t start = static_cast<std::size_t>(cur - base);
    if (start > size_ || bytes > size_ - start) return nullptr;
    used_ = start + bytes;
    return reinterpret_cast<void*>(cur);
  }

  // Default-constructs n objects of trivially-destructible T. nullptr when exhausted.
  template <typename T>
  T* allocate_array(std::size_t n) noexcept VELORA_NONBLOCKING {
    static_assert(std::is_trivially_destructible_v<T>);
    if (n != 0 && n > size_ / sizeof(T)) return nullptr;
    void* p = allocate(sizeof(T) * n, alignof(T));
    if (!p) return nullptr;
    T* t = static_cast<T*>(p);
    for (std::size_t i = 0; i < n; ++i) new (t + i) T();
    return t;
  }

  void reset() noexcept VELORA_NONBLOCKING { used_ = 0; }
  std::size_t used() const noexcept { return used_; }
  std::size_t capacity() const noexcept { return size_; }

 private:
  std::unique_ptr<std::byte[]> storage_;
  std::size_t size_;
  std::size_t used_ = 0;
};

}  // namespace velora::core
