#include "velora/host/live_set_loader.hpp"

#include <algorithm>
#include <cmath>
#include <map>

#include "velora/core/organ_engine.hpp"
#include "velora/core/params_gen.hpp"

namespace velora::host {

namespace p = core::params;
namespace lsp = liveset::lsp;

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

LoadStatus RamHeadProvider::load(const ResolveResult& resolved, std::shared_ptr<const BundleInstrument>& out) {
  if (!resolved.ok() || !resolved.bundle) return LoadStatus::kInstrumentNotFound;
  LoadResult r = load_instrument_by_index(*resolved.bundle, resolved.instrument);
  if (!r.ok()) return r.status;
  if (r.instrument->failed_samples() != 0) return LoadStatus::kBundleInconsistent;  // holes in the instrument: not resident
  out = std::shared_ptr<const BundleInstrument>(std::move(r.instrument));
  return LoadStatus::kOk;
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

// A scalar the registry knows: non-finite takes the default, anything else is clamped to the registry range.
float sane(const p::ParamDef& d, float v) {
  if (!std::isfinite(v)) return static_cast<float>(d.def);
  return static_cast<float>(std::min<double>(std::max<double>(v, d.min), d.max));
}
float sane_volume(float v) { return std::isfinite(v) ? std::min(std::max(v, 0.0f), 1.0f) : 1.0f; }

// Applies one ParamEntry list to the generation: a registry performance parameter becomes the initial value of its slot (for `part` when
// part-scoped); structural master parameters go to the master array; ids this build does not know are preserved in the file and ignored here.
void apply_entries(core::Snapshot& s, const google::protobuf::RepeatedPtrField<lsp::ParamEntry>& list, std::uint32_t part, bool master_list) {
  for (const lsp::ParamEntry& e : list) {
    const p::ParamDef* d = p::find(e.param_id());
    if (!d || !std::isfinite(e.value())) continue;
    const float value = sane(*d, e.value());
    if (d->cls == p::Class::kPerformance) {
      if (master_list ? d->scope != p::Scope::kMaster : d->scope != p::Scope::kPart) continue;
      s.perf_init[p::perf_slot(*d, d->scope == p::Scope::kPart ? part : 0)] = value;
    } else if (master_list && d->scope == p::Scope::kMaster && d->master_index >= 0) {
      s.master[static_cast<std::size_t>(d->master_index)] = value;
    }
  }
}

void fill_inserts(core::PartCfg& pc, const google::protobuf::RepeatedPtrField<ws::v1::InsertSlot>& slots) {
  pc.inserts = {};
  const int n = slots.size() < static_cast<int>(p::kInsertSlotCount) ? slots.size() : static_cast<int>(p::kInsertSlotCount);
  pc.insert_count = static_cast<std::uint8_t>(n);
  for (int i = 0; i < n; ++i) {
    const ws::v1::InsertSlot& in = slots[i];
    core::InsertSlotCfg& dst = pc.inserts[static_cast<std::size_t>(i)];
    for (std::uint32_t k = 0; k < p::kInsertParamCount; ++k) {  // missing params take the registry default
      const p::ParamDef* d = p::find(p::kInsertParam1 + k);
      dst.params[k] = d ? static_cast<float>(d->def) : 0.5f;
    }
    // A type above this build's enum comes from a newer build: it is kept in the file, here the slot is empty ("none").
    dst.type = in.type() >= 0 && static_cast<std::uint32_t>(in.type()) < p::kInsertTypeCount ? static_cast<std::uint8_t>(in.type()) : 0;
    dst.bypass = in.bypass() ? 1 : 0;
    for (int k = 0; k < in.params_size() && k < static_cast<int>(p::kInsertParamCount); ++k) {
      const p::ParamDef* d = p::find(p::kInsertParam1 + static_cast<std::uint32_t>(k));
      if (d && std::isfinite(in.params(k))) dst.params[static_cast<std::size_t>(k)] = in.params(k);
    }
  }
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
  using core::EngineTypeId;
  if (options_.is_rendering && options_.is_rendering()) return failure(LiveSetLoadStatus::kNotQuiesced);
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

  // 2. Resolve and make resident, part by part. Nothing here touches the engine.
  const auto available = liveset::availability_of(renderer_.registry());
  const bool sample_bound = sample_ != nullptr && available(static_cast<std::uint32_t>(EngineTypeId::kSample));
  core::Snapshot snap = core::Snapshot::defaults();
  snap.live_set = true;
  LiveSetLoadResult res;
  res.parts.resize(static_cast<std::size_t>(ls.parts_size()));
  struct Resident {
    std::shared_ptr<const BundleInstrument> instrument;
    bool counted = false;
  };
  std::map<std::string, Resident> cache;  // (library_id, instrument_key) -> the one resident copy shared by the parts that use it
  std::uint32_t sample_parts = 0, organ_parts = 0;

  for (int i = 0; i < ls.parts_size(); ++i) {
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
      const ResolveResult rr = resolver_.resolve(lp.timbre().library_id(), lp.timbre().instrument_key());
      rep.resolve = rr.status;
      if (!rr.ok()) {
        rep.state = pc.state = core::PartState::kMissingLibrary;  // unresolved library or instrument: silent, never a substitute
        continue;
      }
      const std::string key = lp.timbre().library_id() + '\n' + lp.timbre().instrument_key();
      Resident& slot = cache[key];
      if (!slot.instrument) {
        const LoadStatus ls_ = heads_.load(rr, slot.instrument);
        if (ls_ != LoadStatus::kOk || !slot.instrument) {
          cache.erase(key);
          LiveSetLoadResult f = failure(LiveSetLoadStatus::kInstrumentLoadFailed,
                                        "part " + std::to_string(i) + ": " + (ls_ == LoadStatus::kOk ? "no instrument" : to_string(ls_)));
          f.failed_part = pi;
          f.load_status = ls_ == LoadStatus::kOk ? LoadStatus::kInstrumentNotFound : ls_;
          f.parts = std::move(res.parts);
          return f;  // nothing was published, nothing was reserved
        }
      }
      if (!slot.counted) {
        slot.counted = true;
        rep.resident_bytes = slot.instrument->resident_bytes();
        res.resident_bytes += rep.resident_bytes;
      }
      pc.sample_bank = sample_->prepare(*slot.instrument, slot.instrument);
      ++sample_parts;
    } else if (et == static_cast<std::uint32_t>(EngineTypeId::kOrgan)) {
      ++organ_parts;  // the organ needs no timbre
    }
    rep.state = pc.state = core::PartState::kReady;
  }

  // 3. The Live Set's master settings and tempo.
  apply_entries(snap, ls.master(), 0, true);
  snap.perf_init[p::perf_slot(*p::find(p::kTempoBpm), 0)] = sane(*p::find(p::kTempoBpm), ls.tempo());
  core::build_chains(snap);

  // 4. The reservation of the whole Live Set: FX first, the organ + rotary next, then the floors.
  core::VoiceArbiter::LiveSetConfig cfg;
  cfg.budget = options_.arbiter_budget;
  cfg.fx_cost = options_.fx_flat;
  cfg.steal_ramp_frames = options_.steal_ramp_frames;
  if (!core::fill_fx_costs(snap, cfg)) {
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

  // 5. Everything that can refuse, refuses before anything changes: the control plane's FX budget and ledger first, then the arbiter
  //    (atomic by itself). Only then is the generation published.
  switch (plane_.can_publish(snap)) {
    case ControlPlane::PublishStatus::kOk: break;
    case ControlPlane::PublishStatus::kFxBudgetExceeded: {
      LiveSetLoadResult f = failure(LiveSetLoadStatus::kFxBudgetExceeded);
      f.parts = std::move(res.parts);
      return f;
    }
    case ControlPlane::PublishStatus::kBusy: {
      LiveSetLoadResult f = failure(LiveSetLoadStatus::kBusy);
      f.parts = std::move(res.parts);
      return f;
    }
    case ControlPlane::PublishStatus::kInternal: return failure(LiveSetLoadStatus::kInternal);
  }
  // What is reserved now, exactly, so that a failure after the reservation puts it back.
  const bool had_reservation = renderer_.arbiter().reserved();
  const core::VoiceArbiter::LiveSetConfig previous = had_reservation ? renderer_.arbiter().config() : core::VoiceArbiter::LiveSetConfig{};
  const core::VoiceArbiter::ReserveStatus rs = renderer_.arbiter().reserve(cfg);
  if (rs != core::VoiceArbiter::ReserveStatus::kOk) {
    LiveSetLoadResult f = failure(LiveSetLoadStatus::kArbiterRejected, "reserve status " + std::to_string(static_cast<int>(rs)));
    f.reserve_status = rs;
    f.parts = std::move(res.parts);
    return f;
  }
  std::string detail;
  const ControlPlane::PublishStatus ps = plane_.publish_snapshot(snap, detail);
  if (ps != ControlPlane::PublishStatus::kOk) {  // not expected after can_publish(): put the reservation back
    if (had_reservation) (void)renderer_.arbiter().reserve(previous);
    else renderer_.arbiter().clear_reservation();
    LiveSetLoadResult f = failure(ps == ControlPlane::PublishStatus::kFxBudgetExceeded ? LiveSetLoadStatus::kFxBudgetExceeded
                                  : ps == ControlPlane::PublishStatus::kBusy             ? LiveSetLoadStatus::kBusy
                                                                                          : LiveSetLoadStatus::kInternal,
                                  detail);
    f.parts = std::move(res.parts);
    return f;
  }
  res.status = LiveSetLoadStatus::kOk;
  res.revision = plane_.revision();
  return res;
}

}  // namespace velora::host
