#pragma once
// Bundle loader for the sample engine (Story 1.15): decodes every sample of one instrument of a
// formatlib bundle into RAM as planar float and exposes it, with the instrument's region table,
// through the engine/ports sample-source port. Control thread only (it allocates and reads files);
// the audio thread never touches it. All samples fully resident: heads, locked RAM and streaming
// are Stories 1.16 and 1.17.
//
// Conversion is lossless in the float domain: integer PCM becomes value / 2^(bits-1) (exact in
// float for 16 and 24 bits; 32-bit integer PCM is NOT exact in float, only lossless up to 24 bit), float32
// samples are used bit for bit but a sample containing NaN or infinity is rejected. No resampling, no
// requantization: the engine applies the sample-rate ratio through its pitch ratio.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "velora/format/bundle.hpp"
#include "velora/ports/sample_source.hpp"

namespace velora::host {

enum class LoadStatus : std::uint8_t {
  kOk = 0,
  kInstrumentNotFound,  // instrument_key not in the bundle (nothing is substituted)
  kInstrumentIndex,     // index out of range
  kBundleInconsistent,  // the instrument's sample range lies outside the bundle's sample list
};
const char* to_string(LoadStatus s);

class BundleInstrument final : public velora::ports::SampleSource {
 public:
  velora::ports::InstrumentView instrument() const noexcept override { return {regions_, pcm_}; }

  std::size_t sample_count() const { return pcm_.size(); }
  std::size_t region_count() const { return regions_.size(); }
  // Samples whose decode failed: their PCM is left empty so the engine skips (and counts) the
  // regions that use them instead of playing garbage.
  std::size_t failed_samples() const { return failed_samples_; }
  std::size_t resident_bytes() const { return resident_bytes_; }

 private:
  friend struct BundleInstrumentLoader;
  std::vector<velora::ports::SampleRegionDesc> regions_;
  std::vector<velora::ports::SamplePcm> pcm_;
  std::vector<std::vector<float>> data_;  // one buffer per sample: left then (stereo) right
  std::size_t failed_samples_ = 0;
  std::size_t resident_bytes_ = 0;
};

struct LoadResult {
  LoadStatus status = LoadStatus::kInstrumentIndex;
  std::unique_ptr<BundleInstrument> instrument;  // non-null only with kOk
  bool ok() const { return status == LoadStatus::kOk; }
};

// Loads the instrument at `index` / the instrument whose key is `key` (exact match, as minted by formatlib).
LoadResult load_instrument_by_index(const velora::format::BundleReader& bundle, std::size_t index);
LoadResult load_instrument(const velora::format::BundleReader& bundle, std::string_view key);

// Interleaved exchange PCM -> planar float (left then right in `out`, 2 * frames values for stereo).
// False (and `out` unspecified) for bits outside 1..32, a float format that is not 32 bit, an
// unsupported channel count, a size mismatch, or non-finite float data.
bool pcm_to_float(const velora::format::PcmFormat& f, std::span<const std::int32_t> interleaved,
                  std::uint64_t frames, std::vector<float>& out);

// Region model -> port conversion (exposed for tests).
velora::ports::SampleRegionDesc to_port_region(const velora::format::Region& r);

}  // namespace velora::host
