#pragma once
// Secret memory for content keys (Story 6.5, FR30/AR19/NFR17). Standard library and POSIX only: no cryptography here, so this lives in velora_host and
// the librarian (which links velora_host) gains nothing that can run a cipher.
//
// SecureKeyHandle is the host's ports::ContentKeyHandle: a key in a page of its own that is
//   * locked in RAM (mlock), so it is never written to swap -- if the lock fails the key is refused (kMemoryLockFailed), not kept in swappable memory;
//   * excluded from core dumps (Linux: madvise(MADV_DONTDUMP) on that page). PR_SET_DUMPABLE is process-wide (it also blocks ptrace and /proc/<pid>/mem
//     for the whole engine) and is NOT used here; instead the host process calls disable_core_dumps() once at start (RLIMIT_CORE 0), which holds on every
//     platform, and the page is also marked DONTDUMP where the OS has it. macOS has no per-region equivalent: there only RLIMIT_CORE protects it;
//   * wiped (secure_wipe: explicit_bzero / memset_s, not optimised away) the moment the last reference is released, before the page is unlocked and unmapped.
// The key is written into the page in place (writable()) by the provider that unwraps it, so no copy of it exists elsewhere in the engine. OpenSSL, which
// the decrypting reader uses, copies the key into the cipher context of the block it is decrypting and cleanses that context when it frees it.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include "velora/ports/key_provider.hpp"

namespace velora::host {

// Overwrites [p, p+n) with zeros in a way the compiler may not remove.
void secure_wipe(void* p, std::size_t n) noexcept;

// The bytes of locked memory this process may still lock (RLIMIT_MEMLOCK soft limit; SIZE_MAX when unlimited, 0 when it cannot be read). Each live content key holds ONE
// page (the unwrap temporaries live in the free part of the same page), the dev provider's device key one more: a host that plays N protected instruments at once
// needs about (N + 2) pages (4 KiB each; 16 KiB pages on some boards). engine-host checks this at start. Docker's default `ulimit -l` is 64 KiB and many Linux distributions
// give 8 MiB; macOS gives 64 KiB to unprivileged processes.
std::size_t memlock_budget_bytes() noexcept;
std::size_t secure_page_bytes() noexcept;

// RLIMIT_CORE = 0 for this process (soft and hard), so no core file can hold key memory or decoded plaintext. True when the limit is now 0. Idempotent.
// Call once in the host's main() before any key is unwrapped.
bool disable_core_dumps() noexcept;

class SecureKeyHandle final : public velora::ports::ContentKeyHandle {
 public:
  // Reported when the page is released, AFTER the wipe and before it is unlocked and unmapped, so a test can look at the very memory the key lived in.
  struct WipeEvent {
    const std::uint8_t* memory;  // the first byte of the key page; valid only during the callback
    std::size_t size;            // bytes wiped (the whole page)
    bool locked;                 // the page was mlocked
    bool dump_excluded;          // MADV_DONTDUMP was applied (Linux)
  };
  using WipeObserver = std::function<void(const WipeEvent&)>;
  struct Options {
    WipeObserver on_wipe;  // test hook (optional)
    // Test hook: replaces mlock (return false = the lock failed). Unset = ::mlock.
    std::function<bool(void* page, std::size_t bytes)> lock_fn;
    // Test hook: replaces munlock.
    std::function<void(void* page, std::size_t bytes)> unlock_fn;
  };

  // A zeroed, locked, dump-excluded page for one key. nullptr with `status` = kMemoryLockFailed (the lock failed) or kInternal (no memory).
  static std::shared_ptr<SecureKeyHandle> allocate(const Options& opt, velora::ports::KeyStatus& status);
  ~SecureKeyHandle() override;
  SecureKeyHandle(const SecureKeyHandle&) = delete;
  SecureKeyHandle& operator=(const SecureKeyHandle&) = delete;

  std::span<const std::uint8_t, velora::ports::kContentKeyBytes> bytes() const noexcept override {
    return std::span<const std::uint8_t, velora::ports::kContentKeyBytes>(mem_, velora::ports::kContentKeyBytes);
  }
  // For the provider that fills the key in place, before the handle is shared. Never call it afterwards.
  std::span<std::uint8_t, velora::ports::kContentKeyBytes> writable() noexcept {
    return std::span<std::uint8_t, velora::ports::kContentKeyBytes>(mem_, velora::ports::kContentKeyBytes);
  }
  // The rest of the key's page, past the 32 key bytes (64 .. page): locked and wiped with the key. The provider keeps its unwrap temporaries (shared secret, wrapping
  // key) there, so one locked page holds everything secret about one unwrap. `n` <= extra_capacity().
  std::span<std::uint8_t> extra(std::size_t offset, std::size_t n) noexcept { return std::span<std::uint8_t>(mem_ + 64 + offset, n); }
  std::size_t extra_capacity() const noexcept { return page_bytes_ > 64 ? page_bytes_ - 64 : 0; }
  const void* page_address() const noexcept { return mem_; }  // tests (/proc/self/smaps VmFlags)
  std::size_t page_size() const noexcept { return page_bytes_; }
  bool locked() const noexcept { return locked_; }
  bool dump_excluded() const noexcept { return dump_excluded_; }

 private:
  SecureKeyHandle() = default;
  std::uint8_t* mem_ = nullptr;
  std::size_t page_bytes_ = 0;
  bool locked_ = false, dump_excluded_ = false;
  Options opt_;
};

}  // namespace velora::host
