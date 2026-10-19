#pragma once
// formatlib: lossless block-indexed library bundle.
//
// Layout (all integers little-endian, see src/layout.hpp):
//   header (128 B) | manifest | index table | sample blocks
// Every sample is cut into fixed-size blocks of kBlockFrames frames (the last one may be shorter).
// Integer PCM (16/24-bit) blocks are coded with libFLAC; 32-bit float blocks are stored raw.
// The index table maps (sample, block) to a byte range plus a SHA-256 of the stored block bytes, so
// frame k lives in block k / kBlockFrames and is reached by decoding exactly one block.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "velora/format/byte_source.hpp"
#include "velora/format/error.hpp"
#include "velora/format/hash.hpp"
#include "velora/format/instrument_model.hpp"

namespace velora::format {

// Current on-disk format_version (N). Readers accept N and N-1; anything else is rejected.
// Version 2 adds the per-instrument region table (instrument_model.hpp) to the manifest; a
// version 1 bundle reads with empty region tables.
// Minor versions (Story 5.1): the version word in the header and manifest is major | minor << 16.
// Minor 1 appends, after each instrument's region table, an extension table for keyswitches, CC
// crossfades and filters. The writer stamps minor 1 only for a bundle that has such a region, so a
// bundle without them is byte-identical to the Epic 1 format 2 output. Readers accept the current
// minor and the one before it (N and N-1); anything newer is kUnsupportedVersion. The extension
// is NOT ignorable: a reader that does not know minor 1 (any Epic 1 reader) refuses a minor 1
// bundle with kUnsupportedVersion rather than misreading it. Minor 1 requires at least one
// extension record (kCorruptManifest otherwise), so every content has exactly one encoding.
inline constexpr std::uint32_t kFormatVersion = 2;
inline constexpr std::uint32_t kFormatMinor = 1;
inline constexpr std::uint32_t kMinReadableVersion = kFormatVersion - 1;
// Frames per block for every sample (last block of a sample may be shorter). Fixed by format v1.
inline constexpr std::uint32_t kBlockFrames = 4096;

struct PcmFormat {
  std::uint8_t channels = 2;         // 1 or 2
  std::uint8_t bits_per_sample = 16; // 16 or 24 (integer), 32 (float)
  bool is_float = false;             // float requires bits_per_sample == 32
  std::uint32_t sample_rate = 48000; // 44100, 48000, 88200, 96000

  friend bool operator==(const PcmFormat&, const PcmFormat&) = default;
};

// True for the supported set: 16/24-bit int or 32-bit float, mono/stereo, 44.1/48/88.2/96 kHz.
bool is_supported(const PcmFormat& f);

// PCM exchange convention: interleaved std::int32_t, one value per channel per frame.
//  - integer PCM: the sign-extended sample value (16-bit in [-32768,32767], 24-bit in +-2^23).
//  - float32: the IEEE-754 bit pattern (use float_to_bits/bits_to_float); NaN payloads and -0.0
//    are preserved bit for bit.
std::int32_t float_to_bits(float f);
float bits_to_float(std::int32_t b);

// Normalises a relative instrument path into an instrument_key: backslashes become '/', empty and
// "." components are dropped, case is preserved. Absolute paths, ".." components, control
// characters, drive prefixes and an empty result are rejected with kInvalidPath.
Result<std::string> normalize_instrument_key(std::string_view path);

struct SampleInput {
  std::string name;
  PcmFormat format;
  std::uint64_t frame_count = 0;          // 0 is allowed (empty sample, zero blocks)
  std::span<const std::int32_t> data;     // frame_count * channels values
};

struct WriterOptions {
  // Test hook for the version policy; production code leaves it at kFormatVersion. Values in
  // [kMinReadableVersion, kFormatVersion] produce a bundle the reader accepts (same layout);
  // kFormatVersion + 1 produces one the reader must reject; anything else fails finalize().
  std::uint32_t format_version = kFormatVersion;
  // Test hook for the minor policy: 0 makes the writer refuse regions that need the extension
  // table (kInvalidArgument at finalize); 1 (default) stamps minor 1 only when one is present.
  std::uint32_t format_minor = kFormatMinor;
};

class BundleWriter {
 public:
  explicit BundleWriter(WriterOptions opt = {});

  // Vendor-assigned library_id. When not called, library_id is derived (UUIDv5 of the fingerprint
  // of instrument keys and, per sample, format, frame count and PCM SHA-256, in instrument-major
  // order). Display names (instrument and sample) are not part of the identity. The fingerprint
  // depends only on PCM, never on the libFLAC version.
  void set_library_id(const Uuid& id);

  // Library identity from a stable source description (Story 1.13: the imported SFZ's canonical
  // path) instead of the content fingerprint: library_id = UUIDv5 of SHA-256("velora.source.v1"
  // || 0x00 || source_id). Re-importing changed content of the same source keeps the library_id
  // and yields a new bundle_id. Ignored when set_library_id() was called.
  // An empty source_id is kInvalidArgument (and leaves the writer unchanged).
  Error set_source_id(std::string_view source_id);

  // Adds an instrument. Its instrument_key is minted here from `relative_path` (see
  // normalize_instrument_key); callers cannot supply a key directly. Returns the instrument index.
  Result<std::size_t> add_instrument(std::string_view name, std::string_view relative_path);

  // Appends a region to the instrument's region table (format v2; regions with keyswitches, CC
  // crossfades or a filter go to the format 2.1 extension table). The region's `sample` must
  // already have been added to this instrument (regions are validated against its length with
  // validate_region). kInvalidArgument otherwise.
  Error add_region(std::size_t instrument, const Region& region);

  // Encodes immediately; `input.data` is not retained. Verifies the encoding by decoding.
  Error add_sample(std::size_t instrument, const SampleInput& input);

  // Deterministic for a given libFLAC version: identical calls produce identical bytes. Encoded
  // FLAC frames may differ between libFLAC versions (bundle_id and bytes with them); decoded PCM,
  // PCM hashes and library_id never do. Only the STREAMINFO metadata block is emitted.
  Result<std::vector<std::uint8_t>> finalize() const;
  Error finalize_to_file(const std::string& path) const;

 private:
  struct SampleRec {
    std::string name;
    PcmFormat format;
    std::uint64_t frames = 0;
    std::uint64_t first_block = 0;  // global block ordinal
    std::uint32_t block_count = 0;
    Digest256 pcm_sha256{};
  };
  struct BlockRec {
    std::uint64_t data_offset = 0;  // within the block region
    std::uint32_t size = 0;
    std::uint32_t frames = 0;
    Digest256 sha256{};
  };
  struct InstrumentRec {
    std::string name, key;
    std::vector<SampleRec> samples;
    std::vector<Region> regions;
  };
  WriterOptions opt_;
  bool has_library_id_ = false;
  Uuid library_id_{};
  bool has_source_id_ = false;
  std::string source_id_;
  std::vector<InstrumentRec> instruments_;
  std::vector<BlockRec> blocks_;
  std::vector<std::uint8_t> block_data_;
};

struct SampleInfo {
  std::string name;
  PcmFormat format;
  std::uint64_t frame_count = 0;
  std::uint32_t block_count = 0;
  std::uint64_t first_block = 0;  // ordinal into the bundle index table
  std::size_t instrument = 0;
  Digest256 pcm_sha256{};
};

struct InstrumentInfo {
  std::string name;
  std::string key;
  std::size_t first_sample = 0;  // index into the bundle's flat sample list
  std::size_t sample_count = 0;
  std::vector<Region> regions;   // empty for format v1 bundles
};

struct ReaderOptions {
  // Check each block's SHA-256 against the index before decoding it.
  bool verify_block_hash = true;
};

// Per-thread decode context. Owns the libFLAC decoder and a scratch buffer for file-backed
// bundles, so BundleReader::read_block (const, thread-safe) can run on any I/O thread with one
// BlockDecoder per thread. Allocation: only libFLAC's internal buffers and the scratch vector
// grow, and only on the first block (or a larger one); steady state does not allocate.
class BlockDecoder {
 public:
  struct Impl;
  BlockDecoder();
  ~BlockDecoder();
  BlockDecoder(BlockDecoder&&) noexcept;
  BlockDecoder& operator=(BlockDecoder&&) noexcept;
  BlockDecoder(const BlockDecoder&) = delete;
  BlockDecoder& operator=(const BlockDecoder&) = delete;
  std::uint64_t blocks_decoded() const;  // diagnostics/tests

 private:
  friend class BundleReader;
  std::unique_ptr<Impl> impl_;
};

class BundleReader {
 public:
  // Memory is borrowed and must outlive the reader.
  static Result<BundleReader> open_memory(std::span<const std::uint8_t> bytes,
                                          ReaderOptions opt = {});
  // Reads header, manifest and index from a caller-supplied source (e.g. one that goes through a
  // counting file-system port); blocks are read on demand through the same source.
  static Result<BundleReader> open_source(std::unique_ptr<ByteSource> src, ReaderOptions opt = {});
  // Reads header, manifest and index; blocks are read on demand (pread).
  static Result<BundleReader> open_file(const std::string& path, ReaderOptions opt = {});

  BundleReader(BundleReader&&) noexcept;
  BundleReader& operator=(BundleReader&&) noexcept;
  ~BundleReader();

  std::uint32_t format_version() const { return version_; }  // major
  std::uint32_t format_minor() const { return minor_; }       // 0 = Epic 1 layout, 1 = with extensions
  const Digest256& bundle_id() const { return bundle_id_; }  // SHA-256 of the manifest bytes
  const Uuid& library_id() const { return library_id_; }
  std::span<const InstrumentInfo> instruments() const { return instruments_; }
  std::span<const SampleInfo> samples() const { return samples_; }
  std::uint64_t total_blocks() const { return index_.size(); }

  // Decodes one block into `out` (interleaved int32, see PCM convention). Returns the number of
  // frames in the block. `out` must hold at least frames_in_block * channels values.
  // Thread-safe (const) as long as each thread uses its own BlockDecoder.
  Result<std::uint32_t> read_block(std::size_t sample, std::uint64_t block,
                                   std::span<std::int32_t> out, BlockDecoder& dec) const;
  Result<std::uint32_t> read_block(std::size_t sample, std::uint64_t block,
                                   std::span<std::int32_t> out) const;

  // Convenience: frames [first, first+count) into `out` (count*channels values), decoding each
  // covering block once.
  Error read_frames(std::size_t sample, std::uint64_t first, std::uint64_t count,
                    std::span<std::int32_t> out) const;

  // Decodes every block of every sample and checks block hashes and per-sample PCM hashes.
  Error verify_all() const;

 private:
  struct IndexEntry {
    std::uint64_t offset = 0;
    std::uint32_t size = 0;
    std::uint32_t frames = 0;
    Digest256 sha256{};
  };
  BundleReader();
  Result<std::uint32_t> read_block_impl(std::size_t sample, std::uint64_t block,
                                        std::span<std::int32_t> out, BlockDecoder& dec,
                                        bool verify_hash) const;
  static Result<BundleReader> open(std::unique_ptr<ByteSource> src, ReaderOptions opt);

  std::unique_ptr<ByteSource> src_;
  ReaderOptions opt_;
  std::uint32_t version_ = 0;
  std::uint32_t minor_ = 0;
  Digest256 bundle_id_{};
  Uuid library_id_{};
  std::vector<InstrumentInfo> instruments_;
  std::vector<SampleInfo> samples_;
  std::vector<IndexEntry> index_;
};

}  // namespace velora::format
