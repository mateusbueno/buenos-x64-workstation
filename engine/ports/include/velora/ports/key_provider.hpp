#pragma once
// Key-provider port (Story 6.5, FR30/AR19/AR32): how the engine obtains the 256-bit content key of a protected library. Pure interface, standard library
// only: no cryptography, no OS calls and no formatlib types in this header, so engine/core and engine/ports stay free of crypto dependencies
// (tools/ci/check_core_deps.py).
//
// The engine hands the provider a library id and the wrapped_key_v1 blob that was delivered with the library (tools/keys/KEY-HIERARCHY.md section 6). The
// provider unwraps it with the device key it guards (a dev key on the Mac: engine/host/dev_keys; the OP-TEE trusted application on the board: Story 6.6)
// and returns a ContentKeyHandle. The handle OWNS the key memory: that memory is locked in RAM and excluded from core dumps, it is never copied by the
// engine, and it is zeroized when the last reference is released. The engine attaches the handle (a shared_ptr, refcounted) to the generation that uses the
// library, so the key dies, wiped, when that generation is freed after its last voice ended (AD-23).
//
// Threading: unwrap_content_key() runs on the control thread (or a staging worker). It may be slow and may allocate. It is never called from the audio thread.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace velora::ports {

inline constexpr std::size_t kContentKeyBytes = 32;

// A content key in memory the handle owns. `bytes()` stays valid for as long as any reference to the handle exists.
class ContentKeyHandle {
 public:
  virtual ~ContentKeyHandle() = default;
  virtual std::span<const std::uint8_t, kContentKeyBytes> bytes() const noexcept = 0;
};

// The closed set of reasons an unwrap is refused. None of them carries key material.
enum class KeyStatus : std::uint8_t {
  kOk = 0,
  kInvalidArgument,  // a library id that is not canonical UUID text, or an empty blob
  kMalformedBlob,    // wrong size, magic or algorithm byte
  kWrongDevice,      // the blob was wrapped for another device key (recipient key id mismatch)
  kUnwrapFailed,     // authentication failed: another library, a modified blob, or a forged ephemeral key; or an all-zero shared secret
  kUnavailable,      // the provider has no device key, or the secure world is not reachable
  kMemoryLockFailed, // the key memory could not be locked in RAM (the key is refused rather than kept in swappable memory)
  kInternal,
};
inline const char* to_string(KeyStatus s) {
  switch (s) {
    case KeyStatus::kOk: return "ok";
    case KeyStatus::kInvalidArgument: return "invalid_argument";
    case KeyStatus::kMalformedBlob: return "malformed_blob";
    case KeyStatus::kWrongDevice: return "wrong_device";
    case KeyStatus::kUnwrapFailed: return "unwrap_failed";
    case KeyStatus::kUnavailable: return "unavailable";
    case KeyStatus::kMemoryLockFailed: return "memory_lock_failed";
    case KeyStatus::kInternal: return "internal";
  }
  return "?";
}

struct KeyResult {
  KeyStatus status = KeyStatus::kInternal;
  std::shared_ptr<const ContentKeyHandle> key;  // non-null only with kOk
  bool ok() const { return status == KeyStatus::kOk && key != nullptr; }
};

class KeyProvider {
 public:
  virtual ~KeyProvider() = default;
  // Unwraps `wrapped_blob` (wrapped_key_v1) for `library_id` (the canonical lower-case UUID text; it is bound into the wrap as authenticated data).
  virtual KeyResult unwrap_content_key(std::string_view library_id, std::span<const std::uint8_t> wrapped_blob) = 0;
};

}  // namespace velora::ports
