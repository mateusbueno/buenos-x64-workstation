#pragma once
// Story 6.2: the decrypting reader (target velora_format_decrypt). Available ONLY to the engine, offline-render and
// test targets; the desktop build, the librarian and the other verify-only consumers must not link it
// (tools/ci/check_format_no_decrypt.py).
//
// Not real-time safe: opening, read_block and read_frames allocate, do file I/O, hash and run AES. Decrypt runs on
// non-RT threads only (RTSan: never call these from an audio callback); the engine's streaming threads own them.
// Thread-safety: const methods are thread-safe when each thread uses its own BlockDecoder.
#include "velora/format/protected_verify.hpp"

namespace velora::format {

class ProtectedBundleReader {
 public:
  // View verification (see ProtectedBundleView::open_*) plus the content-key check: a key that does not match the
  // manifest's key-check value is kWrongContentKey, never garbage.
  static Result<ProtectedBundleReader> open_memory(std::span<const std::uint8_t> bytes, const TrustedKeys& trusted,
                                                   const ContentKey& key);
  static Result<ProtectedBundleReader> open_source(std::unique_ptr<ByteSource> src, const TrustedKeys& trusted,
                                                   const ContentKey& key);
  static Result<ProtectedBundleReader> open_file(const std::string& path, const TrustedKeys& trusted,
                                                 const ContentKey& key);

  ProtectedBundleReader(ProtectedBundleReader&&) noexcept;
  ProtectedBundleReader& operator=(ProtectedBundleReader&&) noexcept;
  ~ProtectedBundleReader();  // wipes the key copy

  const ProtectedBundleView& view() const;
  const ProtectedInfo& info() const { return view().info(); }
  std::span<const InstrumentInfo> instruments() const { return view().instruments(); }
  std::span<const SampleInfo> samples() const { return view().samples(); }

  // Verifies the ciphertext hash, decrypts and decodes exactly one block. The decoded PCM is bit-identical to the
  // source. Returns the number of frames; `out` is untouched on any error (never plaintext from a bad block).
  Result<std::uint32_t> read_block(std::size_t sample, std::uint64_t block, std::span<std::int32_t> out,
                                   BlockDecoder& dec) const;
  // Frames [first, first+count) of a sample: decrypts and decodes only the covering blocks.
  Error read_frames(std::size_t sample, std::uint64_t first, std::uint64_t count, std::span<std::int32_t> out) const;
  // Decrypts and decodes every block and checks the per-sample PCM hashes of the manifest.
  Error verify_all() const;

 private:
  struct Impl;
  ProtectedBundleReader();
  static Result<ProtectedBundleReader> finish(Result<ProtectedBundleView> v, const ContentKey& key);
  std::unique_ptr<Impl> impl_;
};

}  // namespace velora::format
