#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "velora/ports/key_provider.hpp"
#include "velora/ports/sample_source.hpp"
#include "velora/ports/version.hpp"

namespace ports = velora::ports;

namespace {

// Story 6.5: the key-provider port is a pure interface; this is the smallest implementation (a stub), proving the contract compiles with the standard library alone.
class FixedHandle final : public ports::ContentKeyHandle {
 public:
  std::span<const std::uint8_t, ports::kContentKeyBytes> bytes() const noexcept override { return std::span<const std::uint8_t, ports::kContentKeyBytes>(key_); }

 private:
  std::uint8_t key_[ports::kContentKeyBytes] = {1, 2, 3};
};
class StubProvider final : public ports::KeyProvider {
 public:
  ports::KeyResult unwrap_content_key(std::string_view library_id, std::span<const std::uint8_t> blob) override {
    ports::KeyResult r;
    if (library_id.empty()) r.status = ports::KeyStatus::kInvalidArgument;
    else if (blob.size() != 105) r.status = ports::KeyStatus::kMalformedBlob;
    else {
      r.status = ports::KeyStatus::kOk;
      r.key = std::make_shared<FixedHandle>();
    }
    return r;
  }
};

}  // namespace

int main() {
  if (ports::kPortsApiVersion != 1) return EXIT_FAILURE;
  StubProvider p;
  std::uint8_t blob[105] = {};
  const ports::KeyResult ok = p.unwrap_content_key("lib", blob);
  if (!ok.ok() || ok.key->bytes()[2] != 3) return EXIT_FAILURE;
  if (p.unwrap_content_key("", blob).ok() || p.unwrap_content_key("lib", {}).status != ports::KeyStatus::kMalformedBlob) return EXIT_FAILURE;
  // The closed set of statuses has stable spellings (they travel in logs and typed errors).
  const char* names[] = {"ok", "invalid_argument", "malformed_blob", "wrong_device", "unwrap_failed", "unavailable", "memory_lock_failed", "internal"};
  for (unsigned i = 0; i < 8; ++i)
    if (std::strcmp(ports::to_string(static_cast<ports::KeyStatus>(i)), names[i]) != 0) return EXIT_FAILURE;
  // A sample without a damage map is the Story 1.15 sample: the new fields default to "none".
  const ports::SamplePcm pcm;
  if (pcm.damaged_blocks != nullptr || pcm.damaged_block_frames != 0 || pcm.damaged_block_count != 0) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
