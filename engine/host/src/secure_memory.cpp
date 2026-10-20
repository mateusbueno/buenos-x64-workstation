#define __STDC_WANT_LIB_EXT1__ 1  // memset_s (macOS)
#include "velora/host/secure_memory.hpp"

#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include <string.h>  // explicit_bzero (glibc), memset_s (macOS)

#include <cstring>

namespace velora::host {

void secure_wipe(void* p, std::size_t n) noexcept {
  if (p == nullptr || n == 0) return;
#if defined(__linux__) && defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 25))
  explicit_bzero(p, n);
#elif defined(__APPLE__)
  (void)memset_s(p, n, 0, n);
#else
  volatile std::uint8_t* v = static_cast<volatile std::uint8_t*>(p);  // a volatile store loop the optimiser must keep
  for (std::size_t i = 0; i < n; ++i) v[i] = 0;
#endif
}

std::size_t secure_page_bytes() noexcept {
  const long page = ::sysconf(_SC_PAGESIZE);
  return page > 0 ? static_cast<std::size_t>(page) : 4096;
}

std::size_t memlock_budget_bytes() noexcept {
  struct rlimit rl;
  if (::getrlimit(RLIMIT_MEMLOCK, &rl) != 0) return 0;
  return rl.rlim_cur == RLIM_INFINITY ? static_cast<std::size_t>(-1) : static_cast<std::size_t>(rl.rlim_cur);
}

bool disable_core_dumps() noexcept {
  struct rlimit zero;
  zero.rlim_cur = 0;
  zero.rlim_max = 0;
  if (::setrlimit(RLIMIT_CORE, &zero) != 0) {
    struct rlimit now;  // the hard limit may already be lower than we may set; 0 is the lowest, so a failure here is a real error
    if (::getrlimit(RLIMIT_CORE, &now) != 0) return false;
    return now.rlim_cur == 0;
  }
  return true;
}

std::shared_ptr<SecureKeyHandle> SecureKeyHandle::allocate(const Options& opt, velora::ports::KeyStatus& status) {
  const long page = ::sysconf(_SC_PAGESIZE);
  const std::size_t bytes = page > 0 ? static_cast<std::size_t>(page) : 4096;
  void* mem = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (mem == MAP_FAILED) {
    status = velora::ports::KeyStatus::kInternal;
    return nullptr;
  }
  std::shared_ptr<SecureKeyHandle> h(new SecureKeyHandle());
  h->mem_ = static_cast<std::uint8_t*>(mem);
  h->page_bytes_ = bytes;
  h->opt_ = opt;
  // Excluded from core dumps where the OS can do it per region (before the key is written).
#if defined(MADV_DONTDUMP)
  h->dump_excluded_ = ::madvise(mem, bytes, MADV_DONTDUMP) == 0;
  if (!h->dump_excluded_) {  // Linux: a key page that cannot be kept out of a core dump is refused (RLIMIT_CORE 0 alone is a process setting, not a property of the page)
    h->mem_ = nullptr;
    ::munmap(mem, bytes);
    status = velora::ports::KeyStatus::kMemoryLockFailed;
    return nullptr;
  }
#endif
  h->locked_ = opt.lock_fn ? opt.lock_fn(mem, bytes) : ::mlock(mem, bytes) == 0;
  if (!h->locked_) {  // never keep a key in memory that may be swapped
    h->mem_ = nullptr;  // the destructor must not report a wipe of a key that never was
    ::munmap(mem, bytes);
    status = velora::ports::KeyStatus::kMemoryLockFailed;
    return nullptr;
  }
  std::memset(mem, 0, bytes);
  status = velora::ports::KeyStatus::kOk;
  return h;
}

SecureKeyHandle::~SecureKeyHandle() {
  if (mem_ == nullptr) return;
  secure_wipe(mem_, page_bytes_);
  if (opt_.on_wipe) opt_.on_wipe(WipeEvent{mem_, page_bytes_, locked_, dump_excluded_});
  if (locked_) {
    if (opt_.unlock_fn) opt_.unlock_fn(mem_, page_bytes_);
    else ::munlock(mem_, page_bytes_);
  }
  ::munmap(mem_, page_bytes_);
}

}  // namespace velora::host
