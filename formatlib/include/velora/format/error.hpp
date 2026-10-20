#pragma once
// Typed errors for formatlib. No exceptions are used on the read path.
#include <cassert>
#include <cstdint>
#include <optional>
#include <utility>

namespace velora::format {

enum class [[nodiscard]] Error : std::uint8_t {
  kNone = 0,
  kInvalidArgument,     // caller passed inconsistent or out-of-range data
  kUnsupportedFormat,   // sample format outside the supported set
  kInvalidPath,         // instrument path cannot be normalised
  kDuplicateKey,        // two instruments normalise to the same key
  kIo,                  // file open/read/write failure
  kBadMagic,            // not a bundle
  kTruncated,           // file/blocks shorter than the header says
  kUnsupportedVersion,  // format_version outside [N-1, N]
  kCorruptHeader,
  kCorruptManifest,
  kCorruptIndex,
  kCorruptBlock,        // FLAC decoder rejected the block or produced wrong shape
  kBlockHashMismatch,   // block bytes do not match the index hash
  kPcmHashMismatch,     // decoded sample does not match the manifest PCM hash
  kOutOfRange,          // sample/block/frame index outside the bundle
  kBufferTooSmall,      // caller output buffer too small
  kEncoderFailure,
  // Story 6.2 (protected bundles)
  kNotProtected,        // a protected-bundle API was given an unprotected bundle
  kUntrustedKey,        // the manifest signer's key id is not in the trusted-key list
  kSignatureInvalid,    // Ed25519 signature over the manifest statement does not verify
  kHashListMismatch,    // hash-list root in the manifest does not match the index block hashes
  kWrongContentKey,     // the content key does not pass the manifest's key-check value
  // Story 6.4
  kCatalogMetaInvalid,  // catalog.meta.sig is malformed (size, magic, algorithm), or the seal is not for this catalog.meta
};

const char* to_string(Error e);

// Minimal value-or-error type (std::expected is C++23).
template <class T>
class [[nodiscard]] Result {
 public:
  Result(T v) : value_(std::move(v)), error_(Error::kNone) {}  // NOLINT: implicit by design
  Result(Error e) : error_(e) {                                 // NOLINT: implicit by design
    assert(e != Error::kNone && "Result(Error::kNone) is a programming error");
  }
  bool ok() const { return error_ == Error::kNone; }
  explicit operator bool() const { return ok(); }
  Error error() const { return error_; }
  T& value() { return *value_; }
  const T& value() const { return *value_; }
  T take() { return std::move(*value_); }

 private:
  std::optional<T> value_;
  Error error_;
};

}  // namespace velora::format
