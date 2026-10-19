#include "velora/host/live_set_loader.hpp"

#include <algorithm>
#include <cmath>
#include <map>

#include "velora/core/organ_engine.hpp"
#include "velora/core/params_gen.hpp"
#include "live_set_detail.hpp"
#include "velora/host/scene_controller.hpp"

namespace velora::host {

namespace p = core::params;
namespace lsp = liveset::lsp;
using detail::apply_entries;
using detail::fill_inserts;
using detail::sane;
using detail::sane_volume;

const char* to_string(LiveSetLoadStatus s) {
  switch (s) {
    case LiveSetLoadStatus::kOk: return "ok";
    case LiveSetLoadStatus::kReadFailed: return "read failed";
    case LiveSetLoadStatus::kInvalidLiveSet: return "invalid live set";
    case LiveSetLoadStatus::kPartCountUnsupported: return "part count not supported by this build";
    case LiveSetLoadStatus::kInstrumentLoadFailed: return "instrument load failed";
    case LiveSetLoadStatus::kArbiterRejected: return "voice pool reservation rejected";
    case LiveSetLoadStatus::kFxBudgetExceeded: return "fx budget exceeded";
    case LiveSetLoadStatus::kNotQuiesced: return "audio is rendering";
    case LiveSetLoadStatus::kBusy: return "snapshot ledger busy";
    case LiveSetLoadStatus::kInternal: return "internal error";
    case LiveSetLoadStatus::kHeadBudgetExceeded: return "head budget exceeded";
    case LiveSetLoadStatus::kCancelled: return "cancelled";
  }
  return "?";
}

const char* to_string(core::PartState s) {
  switch (s) {
    case core::PartState::kReady: return "ready";
    case core::PartState::kEmptyTimbre: return "empty timbre";
    case core::PartState::kMissingLibrary: return "missing library";
    case core::PartState::kEngineNotAvailable: return "engine not available";
  }
  return "?";
}

LoadStatus HeadProvider::load_controlled(const ResolveResult& resolved, std::shared_ptr<const BundleInstrument>& out, const LoadHooks& hooks) {
  if (hooks.cancel && hooks.cancel->load(std::memory_order_acquire)) return LoadStatus::kCancelled;
  std::shared_ptr<const BundleInstrument> tmp;
  const LoadStatus st = load(resolved, tmp);
  if (st != LoadStatus::kOk) return st;
  if (tmp && hooks.progress && !hooks.progress(tmp->resident_bytes())) return LoadStatus::kCancelled;  // the instrument dies here
  out = std::move(tmp);
  return LoadStatus::kOk;
}

std::uint64_t HeadProvider::estimate_bytes(const ResolveResult& resolved) const {
  return resolved.ok() && resolved.bundle ? instrument_head_bytes(*resolved.bundle, resolved.instrument) : 0;
}

namespace {
// The instrument keeps the opened bundle (the BundleLease: its file descriptor) until the last generation holding it is freed.
LoadStatus ram_load(const ResolveResult& resolved, std::shared_ptr<const BundleInstrument>& out, const LoadHooks* hooks) {
  if (!resolved.ok() || !resolved.bundle) return LoadStatus::kInstrumentNotFound;
  LoadResult r = load_instrument_by_index(*resolved.bundle, resolved.instrument, hooks);
  if (!r.ok()) return r.status;
  if (r.instrument->failed_samples() != 0) return LoadStatus::kBundleInconsistent;  // holes in the instrument: not resident
  std::shared_ptr<const format::BundleReader> lease = resolved.bundle;
  const BundleInstrument* raw = r.instrument.release();
  out = std::shared_ptr<const BundleInstrument>(raw, [lease = std::move(lease)](const BundleInstrument* p) { delete p; });
  return LoadStatus::kOk;
}
}  // namespace

LoadStatus RamHeadProvider::load(const ResolveResult& resolved, std::shared_ptr<const BundleInstrument>& out) { return ram_load(resolved, out, nullptr); }

LoadStatus RamHeadProvider::load_controlled(const ResolveResult& resolved, std::shared_ptr<const BundleInstrument>& out, const LoadHooks& hooks) {
  return ram_load(resolved, out, &hooks);
}

LiveSetLoader::LiveSetLoader(core::BlockRenderer& renderer, ControlPlane& plane, const LibraryResolver& resolver, HeadProvider& heads,
                             const core::SampleEngine* sample, LiveSetLoaderOptions options)
    : renderer_(renderer), plane_(plane), resolver_(resolver), heads_(heads), sample_(sample), options_(options) {}

namespace {

LiveSetLoadResult failure(LiveSetLoadStatus s, std::string detail = {}) {
  LiveSetLoadResult r;
  r.status = s;
  r.detail = std::move(detail);
  return r;
}

}  // namespace

LiveSetLoadResult LiveSetLoader::load_file(const std::string& path) {
  liveset::ReadOptions ro;
  ro.part_count = options_.part_count;
  liveset::LoadResult lr;
  const liveset::Status st = liveset::read_file(path, ro, lr);
  if (st == liveset::Status::kInvalid) {
    LiveSetLoadResult r = failure(LiveSetLoadStatus::kInvalidLiveSet, path);
    r.report = std::move(lr.report);
    return r;
  }
  if (st != liveset::Status::kOk) {
    LiveSetLoadResult r = failure(LiveSetLoadStatus::kReadFailed, path);
    r.read_status = st;
    return r;
  }
  return load(lr.live_set);
}

LiveSetLoadResult LiveSetLoader::load(const lsp::LiveSet& ls) {
  SceneController::LoadScope scene_scope(options_.scenes);  // recalls answer "load in progress" until this load ends
  if (options_.is_rendering && options_.is_rendering()) return failure(LiveSetLoadStatus::kNotQuiesced);
  std::unique_ptr<StagedLiveSet> staged;
  LiveSetLoadResult r = stage(ls, StageControl{}, staged);
  if (!r.ok()) return r;
  return commit_quiesced(*staged, scene_scope);
}

LiveSetLoadResult LiveSetLoader::stage(const lsp::LiveSet& ls, const StageControl& ctl, std::unique_ptr<StagedLiveSet>& out) const {
  using core::EngineTypeId;
  const auto cancelled = [&ctl] { return ctl.cancel != nullptr && ctl.cancel->load(std::memory_order_acquire); };
  out.reset();
  if (cancelled()) return failure(LiveSetLoadStatus::kCancelled);
  // 1. The format rules for this tier. A Pro-sized (16 part) file fails here on a v1 tier ("exactly the tier's part count").
  liveset::ValidationReport report = liveset::validate(ls, options_.part_count);
  if (!report.ok()) {
    LiveSetLoadResult r = failure(LiveSetLoadStatus::kInvalidLiveSet);
    r.report = std::move(report);
    return r;
  }
  if (static_cast<std::uint32_t>(ls.parts_size()) > p::kPartCount)
    return failure(LiveSetLoadStatus::kPartCountUnsupported,
                   std::to_string(ls.parts_size()) + " parts, this build's engine carries " + std::to_string(p::kPartCount));

  // 2. Resolve part by part (phase A), then make the distinct instruments resident (phase B), then prepare the banks (phase C). Nothing
  //    here touches the engine, the plane or the arbiter.
  const auto available = liveset::availability_of(renderer_.registry());
  const bool sample_bound = sample_ != nullptr && available(static_cast<std::uint32_t>(EngineTypeId::kSample));
  auto staged = std::make_unique<StagedLiveSet>();
  core::Snapshot& snap = staged->snapshot;
  snap = core::Snapshot::defaults();
  snap.live_set = true;
  LiveSetLoadResult& res = staged->result;
  res.parts.resize(static_cast<std::size_t>(ls.parts_size()));
  struct Unique {  // one (library, instrument) shared by every part that uses it
    ResolveResult resolved;
    std::uint32_t first_part = 0;
    std::uint64_t estimate = 0;
    std::shared_ptr<const BundleInstrument> instrument;
  };
  std::vector<Unique> uniques;
  std::map<std::string, std::size_t> unique_of;               // key -> index into `uniques`
  std::vector<int> part_unique(static_cast<std::size_t>(ls.parts_size()), -1);  // the unique a sample part loads, -1 = none
  std::uint32_t sample_parts = 0, organ_parts = 0;

  for (int i = 0; i < ls.parts_size(); ++i) {
    if (cancelled()) return failure(LiveSetLoadStatus::kCancelled);
    const lsp::Part& lp = ls.parts(i);
    const auto pi = static_cast<std::uint32_t>(i);
    core::PartCfg& pc = snap.parts[pi];
    PartLoadReport& rep = res.parts[pi];
    const std::uint32_t et = lp.engine().engine_type_id();
    pc.engine_type = et;
    pc.low_key = static_cast<std::uint8_t>(lp.low_key());
    pc.high_key = static_cast<std::uint8_t>(lp.high_key());
    pc.octave = static_cast<std::int8_t>(lp.octave());
    pc.transpose = static_cast<std::int8_t>(lp.transpose());
    pc.keyboard_on = lp.keyboard_on() ? 1 : 0;
    pc.volume = sane_volume(lp.volume());
    pc.output = lp.output() == ws::v1::PART_OUTPUT_AUX ? static_cast<std::uint8_t>(p::kPartOutputAux) : static_cast<std::uint8_t>(p::kPartOutputMain);
    fill_inserts(pc, lp.inserts());
    snap.perf_init[p::perf_slot(*p::find(p::kSendReverbLevel), pi)] = sane(*p::find(p::kSendReverbLevel), lp.send_reverb());
    snap.perf_init[p::perf_slot(*p::find(p::kSendDelayLevel), pi)] = sane(*p::find(p::kSendDelayLevel), lp.send_delay());
    apply_entries(snap, lp.engine().params(), pi, false);
    apply_entries(snap, lp.envelope(), pi, false);
    apply_entries(snap, lp.eq(), pi, false);

    if (!available(et)) {
      rep.state = pc.state = core::PartState::kEngineNotAvailable;  // FM / VA / unknown: silent, never another engine
      continue;
    }
    if (et == static_cast<std::uint32_t>(EngineTypeId::kSample)) {
      if (!sample_bound) {
        rep.state = pc.state = core::PartState::kEngineNotAvailable;
        continue;
      }
      if (!lp.has_timbre()) {
        rep.state = pc.state = core::PartState::kEmptyTimbre;
        continue;
      }
      ResolveResult rr = resolver_.resolve(lp.timbre().library_id(), lp.timbre().instrument_key());
      rep.resolve = rr.status;
      if (!rr.ok()) {
        rep.state = pc.state = core::PartState::kMissingLibrary;  // unresolved library or instrument: silent, never a substitute
        continue;
      }
      const std::string key = lp.timbre().library_id() + '\n' + lp.timbre().instrument_key();
      auto it = unique_of.find(key);
      if (it == unique_of.end()) {
        it = unique_of.emplace(key, uniques.size()).first;
        Unique u;
        u.resolved = std::move(rr);
        u.first_part = pi;
        uniques.push_back(std::move(u));
      }
      part_unique[pi] = static_cast<int>(it->second);
      rep.state = core::PartState::kReady;  // resolved: ready once its instrument is resident (a failure reports the parts as the old load() did)
      ++sample_parts;
    } else if (et == static_cast<std::uint32_t>(EngineTypeId::kOrgan)) {
      ++organ_parts;  // the organ needs no timbre
      rep.state = pc.state = core::PartState::kReady;
    } else {
      rep.state = pc.state = core::PartState::kReady;
    }
  }

  // Phase B. The size of the target comes from the bundle indexes before a single sample is read, so a target that cannot fit the head
  // budget is refused early; the same budget guards the loading itself (a provider that was off by a few samples still cannot overshoot).
  std::uint64_t total_bytes = 0;
  for (Unique& u : uniques) {
    u.estimate = heads_.estimate_bytes(u.resolved);
    total_bytes += u.estimate;
  }
  if (cancelled()) return failure(LiveSetLoadStatus::kCancelled);
  if (ctl.head_budget_bytes != 0 && total_bytes > ctl.head_budget_bytes) {
    LiveSetLoadResult f = failure(LiveSetLoadStatus::kHeadBudgetExceeded, "needs " + std::to_string(total_bytes) + " bytes of heads, budget " + std::to_string(ctl.head_budget_bytes));
    f.head_bytes = total_bytes;
    f.head_budget = ctl.head_budget_bytes;
    f.parts = std::move(res.parts);
    return f;
  }
  std::uint64_t loaded_bytes = 0;
  std::uint32_t instruments_done = 0;
  bool budget_tripped = false;
  const auto report_progress = [&] {
    if (!ctl.progress) return;
    StageProgress sp;
    sp.bytes_done = loaded_bytes;
    // An estimate of 0 (a provider that cannot size from the index) or one that undercounts must not leave the progress at 0 or past 1.
    sp.bytes_total = std::max(total_bytes, loaded_bytes);
    sp.instruments_done = instruments_done;
    sp.instruments_total = static_cast<std::uint32_t>(uniques.size());
    ctl.progress(sp);
  };
  LoadHooks hooks;
  hooks.cancel = ctl.cancel;
  hooks.progress = [&](std::uint64_t bytes) {
    loaded_bytes += bytes;
    if (ctl.head_budget_bytes != 0 && loaded_bytes > ctl.head_budget_bytes) {
      budget_tripped = true;
      return false;
    }
    report_progress();
    return !cancelled();
  };
  report_progress();
  for (Unique& u : uniques) {
    if (cancelled()) return failure(LiveSetLoadStatus::kCancelled);
    const LoadStatus st = heads_.load_controlled(u.resolved, u.instrument, hooks);
    if (st == LoadStatus::kCancelled || (st == LoadStatus::kOk && budget_tripped)) {
      if (budget_tripped) {
        LiveSetLoadResult f = failure(LiveSetLoadStatus::kHeadBudgetExceeded, "the heads outgrew the budget of " + std::to_string(ctl.head_budget_bytes) + " bytes while loading");
        f.head_bytes = loaded_bytes;
        f.head_budget = ctl.head_budget_bytes;
        f.parts = std::move(res.parts);
        return f;  // `uniques` (the partial heads) are released on return
      }
      return failure(LiveSetLoadStatus::kCancelled);
    }
    if (st != LoadStatus::kOk || !u.instrument) {
      LiveSetLoadResult f = failure(LiveSetLoadStatus::kInstrumentLoadFailed,
                                    "part " + std::to_string(u.first_part) + ": " + (st == LoadStatus::kOk ? "no instrument" : to_string(st)));
      f.failed_part = u.first_part;
      f.load_status = st == LoadStatus::kOk ? LoadStatus::kInstrumentNotFound : st;
      f.parts = std::move(res.parts);
      return f;  // nothing was published, nothing was reserved
    }
    ++instruments_done;
    res.parts[u.first_part].resident_bytes = u.instrument->resident_bytes();  // shared instruments count once, for the first part
    res.resident_bytes += u.instrument->resident_bytes();
    report_progress();
  }

  // Phase C: each sample part gets its own bank over the shared resident instrument.
  if (cancelled()) return failure(LiveSetLoadStatus::kCancelled);
  for (int i = 0; i < ls.parts_size(); ++i) {
    if (part_unique[static_cast<std::size_t>(i)] < 0) continue;
    const Unique& u = uniques[static_cast<std::size_t>(part_unique[static_cast<std::size_t>(i)])];
    core::PartCfg& pc = snap.parts[static_cast<std::size_t>(i)];
    pc.sample_bank = sample_->prepare(*u.instrument, u.instrument);
    res.parts[static_cast<std::size_t>(i)].state = pc.state = core::PartState::kReady;
  }

  // 3. The Live Set's master settings and tempo.
  apply_entries(snap, ls.master(), 0, true);
  snap.perf_init[p::perf_slot(*p::find(p::kTempoBpm), 0)] = sane(*p::find(p::kTempoBpm), ls.tempo());
  core::build_chains(snap);
  snap.scene_count = static_cast<std::uint32_t>(ls.scenes_size());
  snap.scene_index = -1;  // a freshly loaded Live Set has no scene recalled

  // 4. The reservation of the whole Live Set: FX first, the organ + rotary next, then the floors.
  core::VoiceArbiter::LiveSetConfig& cfg = staged->reservation;
  cfg = core::VoiceArbiter::LiveSetConfig{};
  cfg.budget = options_.arbiter_budget;
  cfg.fx_cost = options_.fx_flat;
  cfg.steal_ramp_frames = options_.steal_ramp_frames;
  // The effects to reserve for: the base, or -- with a scene controller -- the scene that costs the most, so no scene is later refused for
  // lack of reserved cost (the plane's own budget check still applies at recall time).
  core::Snapshot worst = snap;
  if (options_.scenes) {
    std::uint64_t worst_cost = core::fx_cost_units(snap);
    for (int sc = 0; sc < ls.scenes_size(); ++sc) {
      core::Snapshot trial = snap;
      apply_scene(ls.scenes(sc), snap, trial);
      for (core::PartCfg& pc : trial.parts) pc.chain.reset();  // cost from the configured insert types
      const std::uint64_t c = core::fx_cost_units(trial);
      if (c > worst_cost) {
        worst_cost = c;
        worst = std::move(trial);
      }
    }
  }
  if (!core::fill_fx_costs(worst, cfg)) {
    LiveSetLoadResult f = failure(LiveSetLoadStatus::kArbiterRejected, "too many effect instances");
    f.reserve_status = core::VoiceArbiter::ReserveStatus::kFxExceedsBudget;
    f.parts = std::move(res.parts);
    return f;
  }
  if (organ_parts > 0) cfg.organ_rotary_cost = core::OrganEngine::reservation_cost_units(organ_parts);
  cfg.waive_organ_reservation = organ_parts == 0;
  if (sample_parts > 0 && available(static_cast<std::uint32_t>(EngineTypeId::kSample))) {
    cfg.floors[0] = {static_cast<std::uint32_t>(EngineTypeId::kSample), options_.sample_floor_voices};
    cfg.floor_count = 1;
  }
  cfg.waive_sample_floor = sample_parts == 0;  // no sample part: the floor is zero

  staged->live_set = ls;
  res.status = LiveSetLoadStatus::kOk;
  LiveSetLoadResult ret = res;
  out = std::move(staged);
  return ret;
}

namespace {

LiveSetLoadResult publish_failure(ControlPlane::PublishStatus ps, std::string detail, std::vector<PartLoadReport> parts) {
  LiveSetLoadResult f = failure(ps == ControlPlane::PublishStatus::kFxBudgetExceeded ? LiveSetLoadStatus::kFxBudgetExceeded
                                : ps == ControlPlane::PublishStatus::kBusy             ? LiveSetLoadStatus::kBusy
                                                                                        : LiveSetLoadStatus::kInternal,
                                std::move(detail));
  f.parts = std::move(parts);
  return f;
}

}  // namespace

LiveSetLoadResult LiveSetLoader::commit_quiesced(StagedLiveSet& staged, SceneController::LoadScope& scope) {
  core::Snapshot& snap = staged.snapshot;
  snap.scene_source = scope.token();
  // Everything that can refuse, refuses before anything changes: the control plane's FX budget and ledger first, then the arbiter
  // (atomic by itself). Only then is the generation published.
  const ControlPlane::PublishStatus can = plane_.can_publish(snap);
  if (can != ControlPlane::PublishStatus::kOk) return publish_failure(can, {}, std::move(staged.result.parts));
  // What is reserved now, exactly, so that a failure after the reservation puts it back.
  const bool had_reservation = renderer_.arbiter().reserved();
  const core::VoiceArbiter::LiveSetConfig previous = had_reservation ? renderer_.arbiter().config() : core::VoiceArbiter::LiveSetConfig{};
  const core::VoiceArbiter::ReserveStatus rs = renderer_.arbiter().reserve(staged.reservation);
  if (rs != core::VoiceArbiter::ReserveStatus::kOk) {
    LiveSetLoadResult f = failure(LiveSetLoadStatus::kArbiterRejected, "reserve status " + std::to_string(static_cast<int>(rs)));
    f.reserve_status = rs;
    f.parts = std::move(staged.result.parts);
    return f;
  }
  std::string detail;
  const ControlPlane::PublishStatus ps = plane_.publish_snapshot(snap, detail);
  if (ps != ControlPlane::PublishStatus::kOk) {  // not expected after can_publish(): put the reservation back
    if (had_reservation) (void)renderer_.arbiter().reserve(previous);
    else renderer_.arbiter().clear_reservation();
    return publish_failure(ps, detail, std::move(staged.result.parts));
  }
  scope.commit(staged.live_set, snap);
  loaded_id_ = staged.live_set.id();
  loaded_name_ = staged.live_set.name();
  LiveSetLoadResult res = std::move(staged.result);
  res.status = LiveSetLoadStatus::kOk;
  res.revision = plane_.revision();
  return res;
}

LiveSetLoadResult LiveSetLoader::commit_seamless(StagedLiveSet& staged) {
  SceneController::LoadScope scope(options_.scenes);  // recalls answer "load in progress" only for this (control thread) commit
  core::Snapshot& snap = staged.snapshot;
  snap.scene_source = scope.token();
  // Validate and compute the reservation now, on this thread; RT only copies it when it adopts the generation. Nothing in the arbiter changes.
  core::VoiceArbiter::PreparedReservation prep;
  const core::VoiceArbiter::ReserveStatus rs = renderer_.arbiter().prepare_reservation(staged.reservation, prep);
  if (rs != core::VoiceArbiter::ReserveStatus::kOk) {
    LiveSetLoadResult f = failure(LiveSetLoadStatus::kArbiterRejected, "reserve status " + std::to_string(static_cast<int>(rs)));
    f.reserve_status = rs;
    f.parts = staged.result.parts;
    return f;
  }
  snap.reservation = prep;
  snap.reservation_epoch = plane_.next_reservation_epoch();
  const ControlPlane::PublishStatus can = plane_.can_publish(snap);
  if (can != ControlPlane::PublishStatus::kOk) return publish_failure(can, {}, staged.result.parts);
  std::string detail;
  const ControlPlane::PublishStatus ps = plane_.publish_snapshot(snap, detail);
  if (ps != ControlPlane::PublishStatus::kOk) return publish_failure(ps, detail, staged.result.parts);
  scope.commit(staged.live_set, snap);
  loaded_id_ = staged.live_set.id();
  loaded_name_ = staged.live_set.name();
  LiveSetLoadResult res = staged.result;
  res.status = LiveSetLoadStatus::kOk;
  res.revision = plane_.revision();
  return res;
}

}  // namespace velora::host
