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
// prepared bank (core::SampleBank). The Live Set's master list and tempo become the generation's master / tempo values. The Live Set's
// ASSIGN map is part of the generation (Snapshot::assign, Story 4.10); scenes are not (they are recalled by SceneController, Story 4.4).
//
// Threading: load() / load_file() are control thread only (they allocate and read files). The voice arbiter's reservation is applied with
// VoiceArbiter::reserve(), which by contract runs while the audio callback is not rendering: load a Live Set at start-up, or between renders
// in tests. Story 4.5 splits the load in two halves so a Live Set can be switched while audio renders (see LiveSetSwitcher): stage() does all
// the slow work (resolve, bring heads resident, build the generation and the reservation config) and touches neither the plane nor the
// arbiter, so it may run on a background worker thread; commit_seamless() runs on the control thread, prepares the reservation (validation
// and costs) and publishes the generation with it, and RT commits the reservation when it adopts the generation (Snapshot::reservation).
//
// Heads and streaming (Stories 1.16 / 1.17, board work) are not in this story. The seam is HeadProvider: it turns a resolved instrument
// into a RESIDENT instrument. RamHeadProvider (the Mac path, and the only one here) decodes every sample whole into RAM through
// sample_bundle_loader. A head-and-stream provider will implement the same interface and return the instrument with its heads resident;
// the loader never looks past "resident or not".
#include <array>
#include <atomic>
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
#include "velora/host/edit_port.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/host/scene_controller.hpp"
#include "velora/host/live_set_store.hpp"
#include "velora/host/sample_bundle_loader.hpp"

namespace velora::host {

// Brings a resolved instrument resident. Implementations may be slow (file I/O): control / I-O threads only (never RT). A provider is used by
// ONE thread at a time (the control thread for load(), the switcher's worker for a background load).
class HeadProvider {
 public:
  virtual ~HeadProvider() = default;
  // kOk with `out` set (every sample of the instrument resident), or a typed failure with `out` untouched.
  virtual LoadStatus load(const ResolveResult& resolved, std::shared_ptr<const BundleInstrument>& out) = 0;
  // Story 4.5: the same, cancellable and with progress (see LoadHooks). A cancelled load returns kCancelled with `out` untouched and nothing
  // left resident. The default calls load() after one cancel check and reports the finished size once; slow providers override it and
  // check the hooks between reads.
  virtual LoadStatus load_controlled(const ResolveResult& resolved, std::shared_ptr<const BundleInstrument>& out, const LoadHooks& hooks);
  // Story 4.5: how many bytes load() will make resident, from the bundle index only (no sample data read), so a target that cannot fit
  // the head budget is refused before anything is loaded. The default is instrument_head_bytes().
  virtual std::uint64_t estimate_bytes(const ResolveResult& resolved) const;
};

// The Mac path: whole instruments in RAM (load_instrument_by_index). A sample that fails to decode makes the load fail (kBundleInconsistent):
// a part with holes in its instrument is not "resident". The returned instrument keeps the opened bundle (its file descriptor, the BundleLease)
// alive: the lease is released when the last generation that holds the instrument is freed.
class RamHeadProvider : public HeadProvider {
 public:
  LoadStatus load(const ResolveResult& resolved, std::shared_ptr<const BundleInstrument>& out) override;
  LoadStatus load_controlled(const ResolveResult& resolved, std::shared_ptr<const BundleInstrument>& out, const LoadHooks& hooks) override;
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
  kHeadBudgetExceeded,    // Story 4.5: the target's heads do not fit StageControl::head_budget_bytes (see head_bytes / head_budget): nothing loaded or kept
  kCancelled,             // Story 4.5: StageControl::cancel was set; everything made resident so far was released
};
const char* to_string(LiveSetLoadStatus s);

struct PartLoadReport {
  core::PartState state = core::PartState::kReady;
  // The resolver's verdict for a part that has a timbre and a sample engine (kInvalidId = not attempted).
  ResolveStatus resolve = ResolveStatus::kInvalidId;
  // Story 6.5: the formatlib cause when the resolver's kBundleError came from a bundle (kNone otherwise).
  format::Error resolve_error = format::Error::kNone;
  std::size_t resident_bytes = 0;  // PCM made resident for this part (shared instruments count once, for the first part that loaded them)
};
const char* to_string(core::PartState s);

struct LiveSetLoadResult {
  LiveSetLoadStatus status = LiveSetLoadStatus::kInternal;
  std::vector<PartLoadReport> parts;                 // one per part of the Live Set when it got that far
  liveset::ValidationReport report;                  // kInvalidLiveSet: the issues
  liveset::Status read_status = liveset::Status::kOk;  // kReadFailed
  std::uint32_t failed_part = 0xFFFFFFFFu;           // kInstrumentLoadFailed: the part
  LoadStatus load_status = LoadStatus::kOk;          // kInstrumentLoadFailed: the provider's verdict (Story 6.5: also the typed refusal of a protected library whose signature,
                                                     // trust, hash list or identity fails at resolve: signature_invalid, untrusted_key, protected_downgrade, ...)
  format::Error bundle_error = format::Error::kNone;  // Story 6.5, with kInstrumentLoadFailed from the resolver: the formatlib cause
  core::VoiceArbiter::ReserveStatus reserve_status = core::VoiceArbiter::ReserveStatus::kOk;  // kArbiterRejected
  std::uint64_t revision = 0;                        // kOk: the generation published
  std::size_t resident_bytes = 0;                    // kOk: total PCM resident
  std::uint64_t head_bytes = 0;                      // kHeadBudgetExceeded: the bytes the target needs (the estimate, or what was loaded when the guard tripped)
  std::uint64_t head_budget = 0;                     // kHeadBudgetExceeded: the budget
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
  // Scene recall (Story 4.4). When set, a successful load hands the Live Set (its scenes) and the generation it built to this controller, so
  // scenes can be recalled afterwards; a load in progress makes recalls answer kLoadInProgress. The FX reservation then covers the worst scene
  // (a scene with more insert effects than the base is not refused later for lack of reserved cost). Unset = scenes are ignored, as in 4.3.
  SceneController* scenes = nullptr;
};

// What stage() can be told (Story 4.5). All optional.
struct StageProgress {
  std::uint64_t bytes_done = 0, bytes_total = 0;  // decoded PCM made resident so far / the target's total (shared instruments once)
  std::uint32_t instruments_done = 0, instruments_total = 0;
};
struct StageControl {
  const std::atomic<bool>* cancel = nullptr;  // set from any thread: the stage stops (kCancelled) at the next check and frees what it loaded
  std::uint64_t head_budget_bytes = 0;        // 0 = unlimited; else a target needing more is refused early (kHeadBudgetExceeded) and guarded while loading
  std::function<void(const StageProgress&)> progress;  // on the staging thread, after the sizes are known and then after every chunk of samples
};

// A Live Set made ready for publication: every instrument resident, the generation built, the reservation config computed. Produced by
// stage(), consumed by commit_seamless(). Holds the resident heads until it is committed or destroyed (destroying it frees them).
struct StagedLiveSet {
  liveset::lsp::LiveSet live_set;
  core::Snapshot snapshot;                        // complete except scene_source, reservation, ledger index and revision (set at commit)
  core::VoiceArbiter::LiveSetConfig reservation;  // the Live Set's voice-pool configuration
  LiveSetLoadResult result;                       // status kOk, the per-part reports and resident_bytes
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

  // --- seamless switching (Story 4.5) ---------------------------------------------------------------------------------------------
  // stage(): validates `live_set`, resolves every part's timbre, estimates the heads from bundle index metadata (refusing early when they
  // exceed ctl.head_budget_bytes), brings every instrument resident (cancellable, with progress, guarded against the budget), builds the
  // generation and the reservation configuration. Changes nothing of the engine: no plane, arbiter or scene controller access. Thread-safe
  // against the control and RT threads, so it can run on a worker thread; it must not run concurrently with another stage() or load() on
  // the same loader (the HeadProvider is used by one thread at a time). On success `out` holds the staged Live Set and the status is kOk.
  LiveSetLoadResult stage(const liveset::lsp::LiveSet& live_set, const StageControl& ctl, std::unique_ptr<StagedLiveSet>& out) const;
  // Control thread only, audio MAY be rendering. Prepares the reservation (VoiceArbiter::prepare_reservation: nothing in the arbiter
  // changes), checks the FX budget and the ledger, and publishes the staged generation carrying the reservation; RT applies it when it
  // adopts the generation. All or nothing: on any failure nothing is published and the current generation keeps playing untouched.
  // `staged` is left intact on failure (the caller may retry, e.g. after kBusy) and may be destroyed after a success (the generation shares
  // the banks and effect chains; the staged copy only drops its references).
  LiveSetLoadResult commit_seamless(StagedLiveSet& staged);
  // Id and name of the Live Set whose generation was published last (by load() or commit_seamless()); empty before the first.
  const std::string& loaded_id() const { return loaded_id_; }
  const std::string& loaded_name() const { return loaded_name_; }

  const LiveSetLoaderOptions& options() const { return options_; }
  // Story 4.9: the edit session hears about every published generation (and about to-be-published switches). Control thread; null = none.
  void set_edit_port(LiveSetEditPort* port) { edits_ = port; }

 private:
  LiveSetLoadResult commit_quiesced(StagedLiveSet& staged, SceneController::LoadScope& scope);
  core::BlockRenderer& renderer_;
  ControlPlane& plane_;
  const LibraryResolver& resolver_;
  HeadProvider& heads_;
  const core::SampleEngine* sample_;
  LiveSetLoaderOptions options_;
  LiveSetEditPort* edits_ = nullptr;
  std::string loaded_id_, loaded_name_;
};

}  // namespace velora::host
