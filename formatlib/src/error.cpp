#include "velora/format/error.hpp"

namespace velora::format {
const char* to_string(Error e) {
  switch (e) {
    case Error::kNone: return "none";
    case Error::kInvalidArgument: return "invalid argument";
    case Error::kUnsupportedFormat: return "unsupported sample format";
    case Error::kInvalidPath: return "invalid instrument path";
    case Error::kDuplicateKey: return "duplicate instrument key";
    case Error::kIo: return "I/O error";
    case Error::kBadMagic: return "not a bundle";
    case Error::kTruncated: return "truncated bundle";
    case Error::kUnsupportedVersion: return "unsupported format_version";
    case Error::kCorruptHeader: return "corrupt header";
    case Error::kCorruptManifest: return "corrupt manifest";
    case Error::kCorruptIndex: return "corrupt index";
    case Error::kCorruptBlock: return "corrupt block";
    case Error::kBlockHashMismatch: return "block_hash_mismatch";
    case Error::kPcmHashMismatch: return "pcm hash mismatch";
    case Error::kOutOfRange: return "out of range";
    case Error::kBufferTooSmall: return "buffer too small";
    case Error::kEncoderFailure: return "encoder failure";
    case Error::kNotProtected: return "not a protected bundle";
    case Error::kUntrustedKey: return "untrusted_key";
    case Error::kSignatureInvalid: return "signature_invalid";
    case Error::kHashListMismatch: return "hash list root mismatch";
    case Error::kWrongContentKey: return "wrong_content_key";
    case Error::kCatalogMetaInvalid: return "catalog_meta_invalid";
  }
  return "unknown";
}
}  // namespace velora::format
