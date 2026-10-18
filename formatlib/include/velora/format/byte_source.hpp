#pragma once
// Random-access byte source behind BundleReader. Implement it to back a reader with something
// other than memory or a plain file (BundleReader::open_source).
#include <cstddef>
#include <cstdint>

#include "velora/format/error.hpp"

namespace velora::format {

class ByteSource {
 public:
  virtual ~ByteSource() = default;
  virtual std::uint64_t size() const = 0;
  // Pointer to [off, off+n) when the bytes are directly addressable, else nullptr.
  virtual const std::uint8_t* direct(std::uint64_t off, std::size_t n) const = 0;
  // Reads exactly n bytes at off; Error::kTruncated when the range exceeds size().
  virtual Error read_at(std::uint64_t off, void* dst, std::size_t n) const = 0;
};

}  // namespace velora::format
