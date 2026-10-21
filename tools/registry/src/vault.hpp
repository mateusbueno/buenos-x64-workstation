#pragma once
// Story 6.8: the content-key vault of the registry. Reads `<vault_dir>/<library_id>/content-key.vault` (the layout `velora-content-pack pack --out DIR` writes),
// opens it with the KEK of the configured root and wraps the content key for one device. The KEK lives in ONE dedicated, page-aligned, mlock()ed mapping together with the
// scratch space for the plaintext content key; nothing else is on that page, so wiping and unlocking it can not disturb anything else. The plaintext content key exists
// ONLY inside wrap() (serialised by a mutex, because the scratch space is shared): never logged, never stored, never returned, wiped before wrap() returns.
#include <array>
#include <memory>
#include <string>
#include <string_view>

#include "util.hpp"

namespace velora::registry {

enum class WrapStatus { kOk, kUnknownLibrary, kVaultUnreadable, kVaultEntryInvalid, kWrapFailed };
const char* to_string(WrapStatus s);

using WrappedBlob = std::array<std::uint8_t, velora::contentpack::kWrappedKeySize>;

class KeyVault {
 public:
  // Loads the KEK from a raw 32-byte file that is not readable by group or others into the locked page. If the page can not be locked in RAM (RLIMIT_MEMLOCK) this
  // FAILS, unless `allow_no_mlock` (development and tests only). nullptr and `err` on failure; `err` never holds key bytes.
  static std::unique_ptr<KeyVault> create(const std::string& vault_dir, const std::string& kek_file, bool allow_no_mlock, std::string* err);
  ~KeyVault();
  KeyVault(const KeyVault&) = delete;
  KeyVault& operator=(const KeyVault&) = delete;

  // `library_id` must be a canonical lower-case UUID (checked here too: it becomes a path component).
  WrapStatus wrap(std::string_view library_id, const Key32& device_public_key, WrappedBlob& out) const;
  std::string kek_id_hex() const;  // the public id of the KEK (first 8 bytes of a hash), for the startup line
  bool locked() const;             // true when the KEK page is mlock()ed

 private:
  KeyVault() = default;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace velora::registry
