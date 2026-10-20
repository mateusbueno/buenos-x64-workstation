#pragma once
// The DEV key provider (Story 6.5, target velora_host_dev_keys): a ports::KeyProvider that unwraps wrapped_key_v1 blobs with a developer's device private key
// on the Mac or a Linux dev machine. The board's provider is the OP-TEE trusted application (Story 6.6) behind the same port; the engine cannot tell them apart.
//
// DEV ONLY. The device private key is never embedded and never defaulted: the host passes the PATH of a `device.x25519.priv` written by
// `tools/keys/velora_keys.py generate` (raw 32-byte unclamped X25519 scalar, outside the repository). The target is built only with VELORA_DEV_KEY_PROVIDER
// (default ON for development configurations; CMake refuses it in a Release / MinSizeRel / VELORA_PRODUCTION configuration), so a shipped image has no
// code that reads a device key from a file.
//
// wrapped_key_v1, exactly as tools/keys/KEY-HIERARCHY.md section 6 (the pinned vectors are checked by the tests):
//   blob (105 B) = "VWK1" | alg 0x01 | recipient key id[8] | ephemeral X25519 public[32] | GCM nonce[12] | ciphertext[32] | tag[16]
//   shared = X25519(device_private, ephemeral_public)  (all-zero refused)
//   wrapping_key = HKDF-SHA256(IKM = shared, salt = ephemeral_public || device_public, info = "velora/wrapped_key_v1/" || library_id, L = 32)
//   content key = AES-256-GCM-open(wrapping_key, nonce, ct, AAD = blob[0:45] || library_id, tag)
// The device private key lives in a SecureKeyHandle (mlocked, non-dumpable, wiped on release) like every content key; the unwrapped key is decrypted straight
// into its own SecureKeyHandle page, so it exists nowhere else. The short-lived shared secret and wrapping key sit in the free part of that same locked page and are wiped
// before unwrap returns, on every path. OpenSSL copies the key material into its own contexts (EVP_PKEY, the GCM context) for the duration of the call and
// cleanses them when it frees them.
//
// Threading: unwrap_content_key is thread-safe (no mutable state); it blocks for a few microseconds of crypto and a handful of syscalls.
#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "velora/host/secure_memory.hpp"
#include "velora/ports/key_provider.hpp"

namespace velora::host {

class DevKeyProvider final : public velora::ports::KeyProvider {
 public:
  struct Options {
    // Hooks for the key pages (tests: fail the lock). `key_memory.on_wipe` observes the wipe of every CONTENT key this provider returns.
    SecureKeyHandle::Options key_memory;
    // The library id must be the canonical lower-case UUID text (kInvalidArgument otherwise). Tests of the pinned vector with a free-form id switch it off.
    bool require_uuid_library_id = true;
    SecureKeyHandle::WipeObserver device_key_on_wipe;  // observes the wipe of the device private key when the provider is destroyed (tests)
  };

  // Reads the 32-byte raw X25519 private scalar from `path`. nullptr with `err` set when the file cannot be read or is not exactly 32 bytes.
  static std::unique_ptr<DevKeyProvider> from_file(const std::string& path, const Options& opt, std::string& err);
  // From bytes the caller holds (tests with the pinned vector). The caller wipes its copy.
  static std::unique_ptr<DevKeyProvider> from_bytes(std::span<const std::uint8_t, 32> device_private, const Options& opt, std::string& err);
  ~DevKeyProvider() override;

  velora::ports::KeyResult unwrap_content_key(std::string_view library_id, std::span<const std::uint8_t> wrapped_blob) override;

  // The device's public half and its key id (first 8 bytes of SHA-256(public)): what a vendor wraps keys to.
  const std::array<std::uint8_t, 32>& device_public() const { return device_public_; }
  const std::array<std::uint8_t, 8>& device_key_id() const { return device_key_id_; }

 private:
  DevKeyProvider() = default;
  std::shared_ptr<SecureKeyHandle> device_private_;
  std::array<std::uint8_t, 32> device_public_{};
  std::array<std::uint8_t, 8> device_key_id_{};
  Options opt_;
};

inline constexpr std::size_t kWrappedKeyBytes = 105;

}  // namespace velora::host
