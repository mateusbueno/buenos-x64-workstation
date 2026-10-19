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
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
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
  kCancelled,           // Story 4.5: the load was cancelled (LoadHooks); nothing is left resident
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

// Cooperative cancellation and progress for a long load (Story 4.5). The loader reads samples in chunks of kLoadChunkFrames frames and
// calls both hooks between chunks (and before each sample), on the loading thread. `cancel` (optional): set to true from any thread to stop;
// the load returns kCancelled with everything it made resident already released. `progress` (optional): called with the bytes made resident
// since its previous call (decoded PCM, the same unit as BundleInstrument::resident_bytes(); counted per chunk as it is read, so a sample that then fails
// to decode was counted although it is not resident -- a failed instrument fails the whole load anyway); returning false stops the load like `cancel`.
struct LoadHooks {
  const std::atomic<bool>* cancel = nullptr;
  std::function<bool(std::uint64_t bytes)> progress;
};
inline constexpr std::uint64_t kLoadChunkFrames = 1u << 16;

// Loads the instrument at `index` / the instrument whose key is `key` (exact match, as minted by formatlib).
LoadResult load_instrument_by_index(const velora::format::BundleReader& bundle, std::size_t index, const LoadHooks* hooks = nullptr);
LoadResult load_instrument(const velora::format::BundleReader& bundle, std::string_view key);

// The decoded size, in bytes, that loading the instrument at `index` will make resident, computed from the bundle index only (no sample
// block is read): for every sample with a usable format and at least one frame, frames x channels x 4. Equals BundleInstrument::resident_bytes()
// of a load without failures. 0 for an index out of range or an inconsistent sample range.
std::uint64_t instrument_head_bytes(const velora::format::BundleReader& bundle, std::size_t index);

// Interleaved exchange PCM -> planar float (left then right in `out`, 2 * frames values for stereo).
// False (and `out` unspecified) for bits outside 1..32, a float format that is not 32 bit, an
// unsupported channel count, a size mismatch, or non-finite float data.
bool pcm_to_float(const velora::format::PcmFormat& f, std::span<const std::int32_t> interleaved,
                  std::uint64_t frames, std::vector<float>& out);

// Region model -> port conversion (exposed for tests).
velora::ports::SampleRegionDesc to_port_region(const velora::format::Region& r);

}  // namespace velora::host
