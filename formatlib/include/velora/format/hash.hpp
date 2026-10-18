#pragma once
// In-house SHA-256 and SHA-1 (SHA-1 only for UUIDv5, never for integrity) and UUIDv5.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace velora::format {

using Digest256 = std::array<std::uint8_t, 32>;
using Digest160 = std::array<std::uint8_t, 20>;

class Sha256 {
 public:
  Sha256();
  void update(const void* data, std::size_t n);
  void update(std::span<const std::uint8_t> d) { update(d.data(), d.size()); }
  Digest256 finish();  // object must not be reused afterwards without reset()
  void reset();
  static Digest256 hash(std::span<const std::uint8_t> d);

 private:
  void block(const std::uint8_t* p);
  std::uint32_t h_[8];
  std::uint8_t buf_[64];
  std::size_t buf_len_;
  std::uint64_t total_;
};

class Sha1 {
 public:
  Sha1();
  void update(const void* data, std::size_t n);
  Digest160 finish();
  static Digest160 hash(std::span<const std::uint8_t> d);

 private:
  void block(const std::uint8_t* p);
  std::uint32_t h_[5];
  std::uint8_t buf_[64];
  std::size_t buf_len_;
  std::uint64_t total_;
};

using Uuid = std::array<std::uint8_t, 16>;

// RFC 4122 name-based UUID, version 5 (SHA-1).
Uuid uuid_v5(const Uuid& ns, std::span<const std::uint8_t> name);
std::string uuid_to_string(const Uuid& u);
std::string to_hex(std::span<const std::uint8_t> d);

// Fixed namespace for Velora library fingerprints (itself a UUIDv5 of the DNS namespace).
extern const Uuid kLibraryNamespace;
// library_id for user imports: UUIDv5(kLibraryNamespace, source fingerprint bytes).
Uuid library_id_from_fingerprint(std::span<const std::uint8_t> fingerprint);

}  // namespace velora::format
