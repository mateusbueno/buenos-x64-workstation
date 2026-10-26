#pragma once
// Bundle loader for the sample engine (Story 1.15): decodes every sample of one instrument of a
// formatlib bundle into RAM as planar float and exposes it, with the instrument's region table,
// through the engine/ports sample-source port. Control thread only (it allocates and reads files);
// the audio thread never touches it. By default all samples are fully resident. Story 1.16 adds an opt-in HEAD MODE (SampleLoadOptions::head): only a
// sized head of each sample is decoded, into mlocked, pre-faulted memory; streaming of the rest is Story 1.17.
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
#include "velora/host/head_memory.hpp"
#include "velora/host/head_sizing.hpp"
#include "velora/ports/sample_source.hpp"

namespace velora::host {

enum class LoadStatus : std::uint8_t {
  kOk = 0,
  kInstrumentNotFound,  // instrument_key not in the bundle (nothing is substituted)
  kInstrumentIndex,     // index out of range
  kBundleInconsistent,  // the instrument's sample range lies outside the bundle's sample list
  kCancelled,           // Story 4.5: the load was cancelled (LoadHooks); nothing is left resident
  // Story 6.5 (protected libraries). Typed reasons a protected instrument is refused; none of them carries key material or plaintext.
  kContentIntegrity,     // `content_integrity`: a block failed its hash check or did not decode while the instrument was made resident
  kWrongContentKey,      // `wrong_content_key`: the unwrapped key does not belong to this bundle (its key check fails)
  kSignatureInvalid,     // `signature_invalid`: the manifest signature does not verify (or the hash list / index does not match the signed manifest)
  kUntrustedKey,         // `untrusted_key`: the bundle is signed by a key this engine does not trust
  kKeyUnavailable,       // `key_unavailable`: the key provider could not unwrap the key (another device or library, a modified blob, no provider)
  kProtectedUnsupported, // `protected_unsupported`: the host was built or configured without protected-library support
  kProtectedDowngrade,   // `protected_downgrade`: an unprotected bundle.vlb stands beside the files of a protected bundle
  // Story 1.16 (instrument heads).
  kHeadLockFailed,           // `head_lock_failed`: mlock of a head failed (RLIMIT_MEMLOCK) and SampleLoadOptions::head.memory.allow_unlocked is not set; nothing is left resident
  kHeadProtectedUnsupported, // `head_protected_unsupported`: head mode is on and the bundle is protected (heads are supported for unprotected bundles only; deferred)
};
const char* to_string(LoadStatus s);
// Story 6.5: the typed refusal for a formatlib error met while opening or reading a PROTECTED bundle. kBundleInconsistent for anything that is not one of
// the integrity classes.
LoadStatus load_status_from(velora::format::Error e);

// What the resident loader needs of an opened bundle: its instrument and sample tables and frames decoded from it. The unprotected BundleReader and the
// protected, decrypting reader (engine/host/protected) both come through here, so there is ONE loader and the audio thread cannot tell them apart.
class InstrumentSource {
 public:
  virtual ~InstrumentSource() = default;
  virtual std::span<const velora::format::InstrumentInfo> instruments() const = 0;
  virtual std::span<const velora::format::SampleInfo> samples() const = 0;
  // Frames [first, first + count) of flat sample `sample` as exchange PCM. Runs on the loading thread (not real-time safe).
  virtual velora::format::Error read_frames(std::size_t sample, std::uint64_t first, std::uint64_t count, std::span<std::int32_t> out) const = 0;
  // True for a protected bundle: the loader then gives every sample a block-damage map (ports::SamplePcm::damaged_blocks) for the I/O edge threads to set.
  virtual bool protected_source() const { return false; }
};

// Story 1.16: head mode. Off (the default) = the Story 1.15 behaviour, every sample whole, bit for bit.
struct HeadOptions {
  bool enabled = false;
  HeadSizing sizing;         // p99.99 + margin, the engine block and the interpolation guard (head_sizing.hpp)
  HeadMemoryOptions memory;  // allow_unlocked (dev), test hooks for mlock
};
struct SampleLoadOptions {
  HeadOptions head;
};

class BundleInstrument final : public velora::ports::SampleSource {
 public:
  velora::ports::InstrumentView instrument() const noexcept override { return {regions_, pcm_}; }

  std::size_t sample_count() const { return pcm_.size(); }
  std::size_t region_count() const { return regions_.size(); }
  // Samples whose decode failed: their PCM is left empty so the engine skips (and counts) the
  // regions that use them instead of playing garbage.
  std::size_t failed_samples() const { return failed_samples_; }
  std::size_t resident_bytes() const { return resident_bytes_; }
  // The first formatlib error met while reading a sample (kNone when none). For a protected bundle this is the typed cause of a refused load.
  velora::format::Error first_error() const { return first_error_; }
  // Story 1.16, head mode only. head_mode(): the samples hold heads, not whole samples (a sample whose head reaches its end is whole and has no tail).
  // heads_locked(): every head buffer is mlocked (false only with HeadMemoryOptions::allow_unlocked after a failed mlock). tail_samples(): samples that have a tail.
  bool head_mode() const { return head_mode_; }
  bool heads_locked() const { return head_mode_ && heads_locked_; }
  std::size_t tail_samples() const { return tail_samples_; }
  // Story 6.5: an opaque object the loader's caller hangs on the instrument so that it lives and dies with it -- for a protected instrument the reader that
  // holds the content key. The instrument is what every generation that plays it shares (SampleBank::owner), so whatever is attached here is released,
  // and the key wiped, when the last generation is freed after its last voice ended. Set once, before the instrument is shared.
  void attach_context(std::shared_ptr<const void> ctx) { context_ = std::move(ctx); }
  const std::shared_ptr<const void>& context() const { return context_; }

  // Story 6.5. Protected instruments only (has_damage_maps()): the per-sample block-damage flags the audio thread reads. Any non-real-time thread (the I/O
  // edge threads) may mark a block damaged; the flag is sticky until the instrument is reloaded. `sample` is the instrument-local index (as in instrument().samples).
  bool has_damage_maps() const { return !damage_.empty(); }
  // True only on the 0 -> 1 transition (this call marked it), so a block is reported once however often it is found bad.
  bool mark_block_damaged(std::size_t sample, std::uint64_t block) const noexcept;
  bool block_damaged(std::size_t sample, std::uint64_t block) const noexcept;
  std::uint64_t damaged_blocks() const noexcept;  // total flagged blocks

 private:
  friend struct BundleInstrumentLoader;
  std::vector<std::unique_ptr<std::atomic<std::uint8_t>[]>> damage_;  // one map per sample (empty vector: an unprotected instrument)
  std::vector<std::uint32_t> damage_count_;
  std::shared_ptr<const void> context_;
  velora::format::Error first_error_ = velora::format::Error::kNone;
  std::vector<velora::ports::SampleRegionDesc> regions_;
  std::vector<velora::ports::SamplePcm> pcm_;
  std::vector<std::vector<float>> data_;  // one buffer per sample: left then (stereo) right
  std::size_t failed_samples_ = 0;
  std::size_t resident_bytes_ = 0;
  bool head_mode_ = false, heads_locked_ = true;
  std::size_t tail_samples_ = 0;
  std::unique_ptr<HeadBuffer> head_arena_;  // head mode: ONE locked, pre-faulted arena holding every sample's head (64 byte aligned); munlocked and freed with the instrument
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
// Story 1.16: the same with load options (head mode). A head-mode load decodes only each sample's head into locked, pre-faulted memory, on the calling thread; the
// instrument is returned only when EVERY head is resident (kOk), otherwise nothing is left resident. Typed refusals: kHeadLockFailed, kHeadProtectedUnsupported.
LoadResult load_instrument_by_index(const InstrumentSource& source, std::size_t index, const LoadHooks* hooks, const SampleLoadOptions& options);
LoadResult load_instrument_by_index(const velora::format::BundleReader& bundle, std::size_t index, const LoadHooks* hooks, const SampleLoadOptions& options);
// The same over any InstrumentSource (Story 6.5: protected bundles). A sample whose frames cannot be read stays empty and is counted in failed_samples();
// first_error() then says why.
LoadResult load_instrument_by_index(const InstrumentSource& source, std::size_t index, const LoadHooks* hooks = nullptr);
LoadResult load_instrument(const velora::format::BundleReader& bundle, std::string_view key);

// The decoded size, in bytes, that loading the instrument at `index` will make resident, computed from the bundle index only (no sample
// block is read): for every sample with a usable format and at least one frame, frames x channels x 4. Equals BundleInstrument::resident_bytes()
// of a load without failures. 0 for an index out of range or an inconsistent sample range.
std::uint64_t instrument_head_bytes(const velora::format::BundleReader& bundle, std::size_t index);
std::uint64_t instrument_head_bytes(std::span<const velora::format::InstrumentInfo> instruments, std::span<const velora::format::SampleInfo> samples, std::size_t index);
// Story 1.16: with head mode on, the bytes of the HEADS the loader will make resident (sized per sample from the instrument's region table, see head_sizing.hpp);
// equals BundleInstrument::resident_bytes() of that load: the size of the one locked arena (heads 64 byte aligned, the arena page rounded), i.e. what is really mapped.
// With head mode off it is the function above.
std::uint64_t instrument_head_bytes(std::span<const velora::format::InstrumentInfo> instruments, std::span<const velora::format::SampleInfo> samples, std::size_t index, const HeadOptions& head);
// The head plan itself (frames per instrument-local sample; 0 frames for a sample the loader leaves empty). Empty for an index out of range.
std::vector<HeadLength> plan_heads(std::span<const velora::format::InstrumentInfo> instruments, std::span<const velora::format::SampleInfo> samples, std::size_t index, const HeadSizing& sizing);

// Interleaved exchange PCM -> planar float (left then right in `out`, 2 * frames values for stereo).
// False (and `out` unspecified) for bits outside 1..32, a float format that is not 32 bit, an
// unsupported channel count, a size mismatch, or non-finite float data.
bool pcm_to_float(const velora::format::PcmFormat& f, std::span<const std::int32_t> interleaved,
                  std::uint64_t frames, std::vector<float>& out);

// Region model -> port conversion (exposed for tests).
velora::ports::SampleRegionDesc to_port_region(const velora::format::Region& r);

}  // namespace velora::host
