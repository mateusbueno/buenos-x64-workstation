#pragma once
// Live Set load (Story 4.3): turns a validated `velora.liveset.v1.LiveSet` (Story 4.2) into ONE new snapshot generation of the engine --
// every part's timbre resolved through the LibraryResolver (Story 1.14), its instrument brought into RAM, one engine configuration per
// part, the whole Live Set's voice-pool reservation applied -- and swaps it in only when every part's instrument is resident.
//
// All-or-nothing. Publication is the last step. A load that fails at any earlier point (bad file, an instrument that cannot be made
// resident, a reservation the arbiter refuses, an FX cost above the control plane's budget, no free ledger slot) publishes nothing,
// leaves the arbiter reservation as it was, and returns a typed status; the previous generation keeps playing untouched. The only
// outcomes of a part that did not resolve are states of that part, not failures: an unresolved library or instrument leaves the part
// SILENT with the state kMissingLibrary (never a substitute), a reserved engine type with no compiled engine (FM, VA) leaves it silent
// with kEngineNotAvailable, a sample part with no timbre is silent with kEmptyTimbre; an organ part needs no timbre.
//
// What a generation carries (core::Snapshot with live_set = true), per part: zone (low/high key), octave, transpose, keyboard on/off,
// volume, output (Main/Aux), insert chain (instances built here, off the RT thread), send levels, engine type, the sound parameters of
// EngineData / envelope / eq (registry performance ids -> the generation's initial values), the load state and the sample engine's
// prepared bank (core::SampleBank). The Live Set's master list and tempo become the generation's master / tempo values. Scenes and the
// ASSIGN map are NOT loaded here (Stories 4.4 and 4.10).
//
// Threading: control thread only (it allocates and reads files). The voice arbiter's reservation is applied with VoiceArbiter::reserve(),
// which by contract runs while the audio callback is not rendering: load a Live Set at start-up, or between renders in tests. Rendering
// while a load is in flight is for Story 4.5 (background load + seamless handover); until then the swap point is the caller's quiet moment.
//
// Heads and streaming (Stories 1.16 / 1.17, board work) are not in this story. The seam is HeadProvider: it turns a resolved instrument
// into a RESIDENT instrument. RamHeadProvider (the Mac path, and the only one here) decodes every sample whole into RAM through
// sample_bundle_loader. A head-and-stream provider will implement the same interface and return the instrument with its heads resident;
// the loader never looks past "resident or not".
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "velora/core/block_renderer.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/core/snapshot.hpp"
#include "velora/core/voice_arbiter.hpp"
#include "velora/host/control_plane.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/host/live_set_store.hpp"
#include "velora/host/sample_bundle_loader.hpp"

namespace velora::host {

// Brings a resolved instrument resident. Implementations may be slow (file I/O): control / I-O threads only.
class HeadProvider {
 public:
  virtual ~HeadProvider() = default;
  // kOk with `out` set (every sample of the instrument resident), or a typed failure with `out` untouched.
  virtual LoadStatus load(const ResolveResult& resolved, std::shared_ptr<const BundleInstrument>& out) = 0;
};

// The Mac path: whole instruments in RAM (load_instrument_by_index). A sample that fails to decode makes the load fail (kBundleInconsistent):
// a part with holes in its instrument is not "resident".
class RamHeadProvider final : public HeadProvider {
 public:
  LoadStatus load(const ResolveResult& resolved, std::shared_ptr<const BundleInstrument>& out) override;
};

enum class LiveSetLoadStatus : std::uint8_t {
  kOk = 0,
  kReadFailed,            // load_file(): the file could not be read or parsed (see read_status)
  kInvalidLiveSet,        // the Live Set violates the format rules for this tier (see report)
  kPartCountUnsupported,  // the tier's part count is valid but this build's engine carries fewer parts (core::params::kPartCount)
  kInstrumentLoadFailed,  // a resolved instrument could not be made resident (see failed_part, load_status): nothing published
  kArbiterRejected,       // the voice pool refused the Live Set's reservation (see reserve_status): nothing published
  kFxBudgetExceeded,      // the effects cost more than the control plane's FX budget: nothing published
  kNotQuiesced,           // the audio callback is rendering (LiveSetLoaderOptions::is_rendering): nothing was touched
  kBusy,                  // no free snapshot ledger slot (too many generations still referenced by sounding voices): nothing published
  kInternal,
};
const char* to_string(LiveSetLoadStatus s);

struct PartLoadReport {
  core::PartState state = core::PartState::kReady;
  // The resolver's verdict for a part that has a timbre and a sample engine (kInvalidId = not attempted).
  ResolveStatus resolve = ResolveStatus::kInvalidId;
  std::size_t resident_bytes = 0;  // PCM made resident for this part (shared instruments count once, for the first part that loaded them)
};
const char* to_string(core::PartState s);

struct LiveSetLoadResult {
  LiveSetLoadStatus status = LiveSetLoadStatus::kInternal;
  std::vector<PartLoadReport> parts;                 // one per part of the Live Set when it got that far
  liveset::ValidationReport report;                  // kInvalidLiveSet: the issues
  liveset::Status read_status = liveset::Status::kOk;  // kReadFailed
  std::uint32_t failed_part = 0xFFFFFFFFu;           // kInstrumentLoadFailed: the part
  LoadStatus load_status = LoadStatus::kOk;          // kInstrumentLoadFailed: the provider's verdict
  core::VoiceArbiter::ReserveStatus reserve_status = core::VoiceArbiter::ReserveStatus::kOk;  // kArbiterRejected
  std::uint64_t revision = 0;                        // kOk: the generation published
  std::size_t resident_bytes = 0;                    // kOk: total PCM resident
  std::string detail;
  bool ok() const { return status == LiveSetLoadStatus::kOk; }
};

struct LiveSetLoaderOptions {
  std::uint32_t part_count = liveset::kV1PartCount;  // the tier's part count (8 on v1, 16 on Pro)
  std::uint64_t arbiter_budget = 1u << 20;           // cost units of the whole voice pool (calibrated on the board: Story 1.18)
  std::uint64_t fx_flat = 0;                         // extra flat FX reservation on top of the Live Set's own effect instances
  std::uint32_t sample_floor_voices = core::VoiceArbiter::kSampleFloorVoices;
  std::uint32_t steal_ramp_frames = core::VoiceArbiter::kStealRampFrames;
  // Quiescence guard. The reservation (VoiceArbiter::reserve()) must not run while the audio callback renders. When set, the loader asks this
  // before it touches anything and refuses with kNotQuiesced when it returns true. engine-host answers "the audio stream is running";
  // the offline renderer renders on the calling thread, so it is never rendering during a load. Unset = the caller guarantees quiescence.
  std::function<bool()> is_rendering;
};

class LiveSetLoader {
 public:
  // `sample` (optional): the sample engine bound to `renderer`, whose configuration the per-part banks are prepared for. Without it a
  // Live Set's sample parts are silent with kEngineNotAvailable.
  LiveSetLoader(core::BlockRenderer& renderer, ControlPlane& plane, const LibraryResolver& resolver, HeadProvider& heads,
                const core::SampleEngine* sample, LiveSetLoaderOptions options = {});

  // Loads an already parsed Live Set: validated here against options.part_count (the caller need not).
  LiveSetLoadResult load(const liveset::lsp::LiveSet& live_set);
  // Reads, validates and loads the Live Set file at `path`.
  LiveSetLoadResult load_file(const std::string& path);

  const LiveSetLoaderOptions& options() const { return options_; }

 private:
  core::BlockRenderer& renderer_;
  ControlPlane& plane_;
  const LibraryResolver& resolver_;
  HeadProvider& heads_;
  const core::SampleEngine* sample_;
  LiveSetLoaderOptions options_;
};

}  // namespace velora::host
