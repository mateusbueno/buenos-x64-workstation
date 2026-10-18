# formatlib

Role: lossless block-indexed library bundle (Story 1.12). Writer and reader for the container, identity minting (`bundle_id`, `library_id`, `instrument_key`), in-house SHA-256/SHA-1/UUIDv5, and a small WAV loader that feeds the writer. Crypto, SF2/SFZ parsing, the librarian and streaming are out of scope.

Dependency rule: may link only libFLAC (found via CMake config, pkg-config or find_library; >= 1.4, target 1.5.0; library only, no flac/metaflac binaries) and the C++ standard library. It must not include anything from `engine/core` (or `engine/host`), and `engine/core` must not depend on it.

Container (target `velora_format`, namespace `velora::format`):

- Layout: 128-byte header, manifest, index table, blocks; little-endian, strictly contiguous, `total_size` in the header makes any truncation a typed error.
- Every sample is cut into blocks of `kBlockFrames` (4096) frames; frame k lives in block `k / 4096`, so a seek decodes exactly one block (`BundleReader::read_block`, per-thread `BlockDecoder`). 16/24-bit PCM is coded per block as a self-contained libFLAC stream (verified by decode at write time); 32-bit float is stored raw. Mono/stereo, 44.1/48/88.2/96 kHz. No resampling or requantization.
- Index entry: offset, size, frames and SHA-256 of the stored block bytes (checked before decoding unless `ReaderOptions::verify_block_hash` is off). The manifest carries per-sample PCM SHA-256, so `verify_all()` is the bit-exact decode verify.
- Identity: `bundle_id` = SHA-256 of the manifest (which embeds the index hash, so it covers all content); `library_id` = vendor-assigned or UUIDv5 of a fingerprint of keys, formats and PCM hashes; `instrument_key` = normalized relative path, minted only by `BundleWriter::add_instrument`. Output is deterministic for a given libFLAC version (encoded frames, hence file bytes and `bundle_id`, may differ across libFLAC versions; PCM hashes, decoded PCM and `library_id` do not). Only the STREAMINFO metadata block is written (no VORBIS_COMMENT/padding). Names are not part of `library_id`.
- Versions: `kFormatVersion` = 1; the reader accepts N and N-1 and rejects anything else with `Error::kUnsupportedVersion`. Zero-length samples are allowed.
- PCM exchange convention: interleaved `int32_t`; float32 is carried as its IEEE bit pattern.

Tests: `format_hash_test`, `format_bundle_test` (round trips, seek, truncation and bit-flip sweeps, corruption, version, identity), `format_wav_test`.
