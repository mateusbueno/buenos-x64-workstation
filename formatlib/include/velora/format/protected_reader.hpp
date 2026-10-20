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

  // Story 6.5: a reader over an already verified view (shared, e.g. the one the library resolver opened) that BORROWS the content key instead of copying it:
  // `key` points at memory the caller owns (the engine's mlocked, non-dumpable key handle) and `key_owner` keeps that memory alive for as long as the reader
  // lives. kWrongContentKey when the key does not match the manifest's key check. The reader never stores a copy of such a key (the cipher context of one
  // block is created and freed per call; OpenSSL cleanses it).
  static Result<ProtectedBundleReader> open_view(std::shared_ptr<const ProtectedBundleView> view, std::span<const std::uint8_t, 32> key,
                                                 std::shared_ptr<const void> key_owner);

  // Test hook (Story 6.5): called, on the calling thread, once for every block any reader decrypts (after the AES-CTR pass). Tests use it to prove WHICH thread
  // decrypts: the loading thread and the I/O edge threads, never the audio thread. nullptr (the default) = no observer. The function must be thread-safe.
  using DecryptObserver = void (*)(std::uint64_t global_block);
  static void set_decrypt_observer(DecryptObserver fn) noexcept;

  ProtectedBundleReader(ProtectedBundleReader&&) noexcept;
  ProtectedBundleReader& operator=(ProtectedBundleReader&&) noexcept;
  ~ProtectedBundleReader();  // wipes its own key copy (none when the key is borrowed)

  const ProtectedBundleView& view() const;
  std::shared_ptr<const ProtectedBundleView> shared_view() const;
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
