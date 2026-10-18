#pragma once
// Internal: the only translation unit family that touches libFLAC.
#include <cstdint>
#include <vector>

#include "velora/format/bundle.hpp"

namespace velora::format {

// Encodes one block (frames <= kBlockFrames, interleaved) as a self-contained FLAC stream and
// appends it to `out`. Verifies by decoding inside libFLAC.
Error flac_encode_block(const PcmFormat& f, const std::int32_t* interleaved, std::uint32_t frames,
                        std::vector<std::uint8_t>& out);

// Decodes one block coded by flac_encode_block. Rejects anything that is not exactly one frame of
// `frames` frames with the expected channels/bit depth.
Error flac_decode_block(BlockDecoder::Impl& ctx, const std::uint8_t* data, std::size_t size,
                        const PcmFormat& f, std::uint32_t frames, std::int32_t* out);

// Test hook: decode with a throw-away decoder context (same checks as flac_decode_block).
Error flac_decode_standalone(const std::uint8_t* data, std::size_t size, const PcmFormat& f,
                             std::uint32_t frames, std::int32_t* out);

std::vector<std::uint8_t>& scratch_of(BlockDecoder::Impl& ctx);
void count_block(BlockDecoder::Impl& ctx);

}  // namespace velora::format
