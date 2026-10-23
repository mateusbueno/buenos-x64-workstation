#include "velora/host/control_plane.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

#include "velora/core/assign_events.hpp"
#include "velora/core/param_events.hpp"

namespace velora::host {

namespace p = core::params;
using ws::v1::ErrorCode;

const char* to_string(SceneResult r) {
  switch (r) {
    case SceneResult::kOk: return "ok";
    case SceneResult::kAtFirst: return "at first scene";
    case SceneResult::kAtLast: return "at last scene";
    case SceneResult::kNoSuchScene: return "no such scene";
    case SceneResult::kNoLiveSet: return "no live set loaded";
    case SceneResult::kLoadInProgress: return "live set load in progress";
    case SceneResult::kBusy: return "snapshot ledger busy";
    case SceneResult::kFxBudgetExceeded: return "fx budget exceeded";
    case SceneResult::kInternal: return "internal error";
  }
  return "?";
}

struct ControlFail {
  ErrorCode code = ws::v1::ERROR_CODE_UNSPECIFIED;
  std::string message;
  std::map<std::string, std::string> details;
};

namespace {

using Fail = ControlFail;

std::string num(double v) {
  char b[48];
  std::snprintf(b, sizeof b, "%g", v);
  return b;
}

bool fail(Fail& f, ErrorCode c, std::string m) {
  f.code = c;
  f.message = std::move(m);
  return false;
}

bool check_part(std::uint32_t part, Fail& f) {
  if (part < p::kPartCount) return true;
  f.details["part"] = std::to_string(part);
  f.details["max_part"] = std::to_string(p::kPartCount - 1);
  return fail(f, ws::v1::ERROR_CODE_INVALID_PART, "part index out of range");
}

bool check_value(const p::ParamDef& d, double v, Fail& f) {
  f.details["param_id"] = std::to_string(d.id);
  if (!std::isfinite(v) || v < d.min || v > d.max) {
    f.details["min"] = num(d.min);
    f.details["max"] = num(d.max);
    return fail(f, ws::v1::ERROR_CODE_OUT_OF_RANGE, "value outside the parameter range");
  }
  if (d.type != p::Type::kFloat && v != std::floor(v))
    return fail(f, ws::v1::ERROR_CODE_OUT_OF_RANGE, "parameter takes whole steps only");
  f.details.erase("param_id");
  return true;
}

const p::ParamDef* lookup(int id, Fail& f) {
  const p::ParamDef* d = id > 0 ? p::find(static_cast<std::uint32_t>(id)) : nullptr;
  if (!d) {
    f.details["param_id"] = std::to_string(id);
    fail(f, ws::v1::ERROR_CODE_UNKNOWN_PARAM, "unknown parameter id");
  }
  return d;
}

bool check_enum(std::uint32_t id, int v, std::uint32_t count, const char* what, Fail& f) {
  if (v >= 0 && static_cast<std::uint32_t>(v) < count) return true;
  f.details["param_id"] = std::to_string(id);
  f.details["value"] = std::to_string(v);
  return fail(f, ws::v1::ERROR_CODE_UNKNOWN_ENUM_VALUE, std::string("unknown ") + what + " value");
}

bool same_inserts(const core::PartCfg& a, const core::PartCfg& b) {
  if (a.insert_count != b.insert_count) return false;
  for (std::uint32_t i = 0; i < a.insert_count; ++i) {
    const core::InsertSlotCfg &x = a.inserts[i], &y = b.inserts[i];
    if (x.type != y.type || x.bypass != y.bypass || x.params != y.params) return false;
  }
  return true;
}

bool apply_insert_chain(core::Snapshot& s, std::uint32_t part,
                        const google::protobuf::RepeatedPtrField<ws::v1::InsertSlot>& slots, Fail& f) {
  if (!check_part(part, f)) return false;
  if (slots.size() > static_cast<int>(p::kInsertSlotCount)) {
    f.details["max_slots"] = std::to_string(p::kInsertSlotCount);
    return fail(f, ws::v1::ERROR_CODE_INVALID_SLOT, "too many insert slots");
  }
  core::PartCfg& pc = s.parts[part];
  core::PartCfg next = pc;
  next.inserts = {};
  next.insert_count = static_cast<std::uint8_t>(slots.size());
  for (int i = 0; i < slots.size(); ++i) {
    const ws::v1::InsertSlot& in = slots[i];
    if (!check_enum(p::kInsertType, in.type(), p::kInsertTypeCount, "insert type", f)) return false;
    if (in.params_size() > static_cast<int>(p::kInsertParamCount)) {
      f.details["slot"] = std::to_string(i);
      return fail(f, ws::v1::ERROR_CODE_INVALID_SLOT, "too many insert parameters");
    }
    core::InsertSlotCfg& dst = next.inserts[static_cast<std::size_t>(i)];
    for (std::uint32_t k = 0; k < p::kInsertParamCount; ++k) {  // missing params take the registry default
      const p::ParamDef* d = p::find(p::kInsertParam1 + k);
      if (!d) return fail(f, ws::v1::ERROR_CODE_INTERNAL, "registry gap: insert_param ids are not contiguous");
      dst.params[k] = static_cast<float>(d->def);
    }
    dst.type = static_cast<std::uint8_t>(in.type());
    dst.bypass = in.bypass() ? 1 : 0;
    for (int k = 0; k < in.params_size(); ++k) {
      const p::ParamDef* dp = p::find(p::kInsertParam1 + static_cast<std::uint32_t>(k));
      if (!dp) return fail(f, ws::v1::ERROR_CODE_INTERNAL, "registry gap: insert_param ids are not contiguous");
      const p::ParamDef& d = *dp;
      if (!check_value(d, in.params(k), f)) {
        f.details["slot"] = std::to_string(i);
        return false;
      }
      dst.params[static_cast<std::size_t>(k)] = in.params(k);
    }
  }
  if (pc.chain && same_inserts(pc, next)) {
    next.chain = pc.chain;  // nothing changed: the generation keeps the very same effect instances (RT does not cross-fade)
  } else {
    next.chain = core::build_chain(next);  // pre-allocated here, off the RT thread; they travel with the snapshot
  }
  pc = next;
  return true;
}

bool apply_send(core::Snapshot& s, std::uint32_t part, std::uint16_t id, float v, Fail& f) {
  const p::ParamDef& d = *p::find(id);
  if (!check_value(d, v, f)) return false;
  s.perf_init[p::perf_slot(d, part)] = v;
  return true;
}

bool apply_master(core::Snapshot& s, const google::protobuf::RepeatedPtrField<ws::v1::ParamValue>& values, Fail& f,
                  bool reject_duplicates = false) {
  core::Snapshot next = s;
  std::array<bool, p::kMasterParams + 1> seen{};
  for (const ws::v1::ParamValue& pv : values) {
    const p::ParamDef* d = lookup(pv.param_id(), f);
    if (!d) return false;
    if (reject_duplicates && d->scope == p::Scope::kMaster && d->master_index >= 0) {
      bool& was = seen[static_cast<std::size_t>(d->master_index)];
      if (was) {
        f.details["param_id"] = std::to_string(d->id);
        return fail(f, ws::v1::ERROR_CODE_INVALID_COMMAND, "master parameter listed twice");
      }
      was = true;
    }
    if (d->cls != p::Class::kStructural) {
      f.details["param_id"] = std::to_string(d->id);
      return fail(f, ws::v1::ERROR_CODE_WRONG_CLASS, "performance parameter sent as a structural setting");
    }
    if (d->scope != p::Scope::kMaster) {
      f.details["param_id"] = std::to_string(d->id);
      return fail(f, ws::v1::ERROR_CODE_WRONG_SCOPE, "not a master parameter");
    }
    if (!check_value(*d, pv.value(), f)) return false;
    next.master[static_cast<std::size_t>(d->master_index)] = pv.value();
  }
  s = next;
  return true;
}

bool apply_part_output(core::Snapshot& s, std::uint32_t part, int output, Fail& f) {
  if (!check_part(part, f)) return false;
  if (!check_enum(p::kPartOutput, output, p::kPartOutputCount, "part output", f)) return false;
  s.parts[part].output = static_cast<std::uint8_t>(output);
  return true;
}

// The dev configuration / any State, applied on top of the registry defaults.
bool apply_config(core::Snapshot& s, const ws::v1::State& cfg, Fail& f) {
  std::array<bool, p::kPerfSlotCount> init_seen{};
  for (const ws::v1::ParamValue& pv : cfg.performance_init()) {
    const p::ParamDef* d = lookup(pv.param_id(), f);
    if (!d) return false;
    if (d->cls != p::Class::kPerformance)
      return fail(f, ws::v1::ERROR_CODE_WRONG_CLASS, "performance_init holds a structural parameter: " + std::to_string(d->id));
    std::uint32_t part = 0;
    if (d->scope == p::Scope::kPart) {
      if (!pv.has_part()) {
        f.details["param_id"] = std::to_string(d->id);
        return fail(f, ws::v1::ERROR_CODE_INVALID_PART, "performance_init entry for a part-scope parameter needs an explicit part");
      }
      part = pv.part();
      if (!check_part(part, f)) return false;
    }
    if (!check_value(*d, pv.value(), f)) return false;
    const std::size_t slot = p::perf_slot(*d, part);
    if (init_seen[slot]) {
      f.details["param_id"] = std::to_string(d->id);
      f.details["part"] = std::to_string(part);
      return fail(f, ws::v1::ERROR_CODE_INVALID_COMMAND, "performance_init lists the same parameter and part twice");
    }
    init_seen[slot] = true;
    s.perf_init[slot] = pv.value();
  }
  std::array<bool, p::kPartCount> seen{};
  for (const ws::v1::PartState& ps : cfg.parts()) {
    if (!check_part(ps.part(), f)) return false;
    if (seen[ps.part()]) {
      f.details["part"] = std::to_string(ps.part());
      return fail(f, ws::v1::ERROR_CODE_INVALID_COMMAND, "part listed twice");
    }
    seen[ps.part()] = true;
    s.parts[ps.part()].timbre_ref = ps.timbre_ref();
    if (!apply_part_output(s, ps.part(), ps.output(), f)) return false;
    if (!apply_insert_chain(s, ps.part(), ps.inserts(), f)) return false;
    if (ps.has_send_reverb() && !apply_send(s, ps.part(), p::kSendReverbLevel, ps.send_reverb(), f)) return false;
    if (ps.has_send_delay() && !apply_send(s, ps.part(), p::kSendDelayLevel, ps.send_delay(), f)) return false;
  }
  return apply_master(s, cfg.master(), f, /*reject_duplicates=*/true);
}

ws::v1::ParamId pid(std::uint32_t id) { return static_cast<ws::v1::ParamId>(id); }

void fill_from(const core::Snapshot& s, ws::v1::State* out) {
  out->Clear();
  out->set_revision(s.revision);
  out->set_live_set_loaded(s.live_set);
  out->set_current_scene(s.live_set ? s.scene_index : -1);
  out->set_scene_count(s.live_set ? s.scene_count : 0);
  for (std::uint32_t part = 0; part < p::kPartCount; ++part) {
    const core::PartCfg& pc = s.parts[part];
    ws::v1::PartState* ps = out->add_parts();
    ps->set_part(part);
    ps->set_timbre_ref(pc.timbre_ref);
    ps->set_output(static_cast<ws::v1::PartOutput>(pc.output));
    for (std::uint32_t i = 0; i < pc.insert_count; ++i) {
      ws::v1::InsertSlot* in = ps->add_inserts();
      in->set_type(static_cast<ws::v1::InsertType>(pc.inserts[i].type));
      in->set_bypass(pc.inserts[i].bypass != 0);
      for (float v : pc.inserts[i].params) in->add_params(v);
    }
    ps->set_send_reverb(s.perf_init[p::perf_slot(*p::find(p::kSendReverbLevel), part)]);
    ps->set_send_delay(s.perf_init[p::perf_slot(*p::find(p::kSendDelayLevel), part)]);
  }
  for (const p::ParamDef& d : p::kParams) {
    if (d.cls == p::Class::kStructural && d.scope == p::Scope::kMaster) {
      ws::v1::ParamValue* pv = out->add_master();
      pv->set_param_id(pid(d.id));
      pv->set_value(s.master[static_cast<std::size_t>(d.master_index)]);
    } else if (d.cls == p::Class::kPerformance) {
      const std::uint32_t n = d.scope == p::Scope::kPart ? p::kPartCount : 1;
      for (std::uint32_t part = 0; part < n; ++part) {
        ws::v1::ParamValue* pv = out->add_performance_init();
        pv->set_param_id(pid(d.id));
        pv->set_part(part);
        pv->set_value(s.perf_init[p::perf_slot(d, part)]);
      }
    }
  }
}

}  // namespace

ControlPlane::ControlPlane(core::SnapshotExchange& exchange, const core::LastValueTable& live,
                           ControlEventQueue& rt_events, Clock now_ns)
    : exchange_(exchange), live_(live), rt_events_(rt_events), clock_(std::move(now_ns)) {}

ControlPlane::~ControlPlane() {
  if (damage_) damage_->set_listener({});  // an edge thread must not call into a plane that is gone
  for (core::Snapshot* g : generations_) delete g;
}

bool ControlPlane::commit(const core::Snapshot& work, ControlFail& f) {
  constexpr std::uint32_t kAll = core::SnapshotExchange::kLedgerSlots >= 32
                                     ? 0xFFFFFFFFu
                                     : ((1u << core::SnapshotExchange::kLedgerSlots) - 1);
  const std::uint64_t cost = core::fx_cost_units(work);
  const std::uint64_t budget = fx_budget();  // read once
  // Only a command that raises the cost is refused: lowering the budget below the current cost never blocks removing effects
  // or changing anything else that does not add cost.
  if (cost > budget && (!current_ || cost > fx_cost_.load(std::memory_order_acquire))) {
    f.details["fx_cost"] = std::to_string(cost);
    f.details["fx_budget"] = std::to_string(budget);
    return fail(f, ws::v1::ERROR_CODE_FX_BUDGET_EXCEEDED, "the effects would cost more than the FX budget");
  }
  if (ledger_used_ == kAll) reclaim();
  if (ledger_used_ == kAll) {
    f.details["live_generations"] = std::to_string(generations_.size());
    return fail(f, ws::v1::ERROR_CODE_BUSY, "too many snapshot generations are still referenced by sounding voices");
  }
  std::uint32_t idx = 0;
  while (ledger_used_ & (1u << idx)) ++idx;
  auto* s = new core::Snapshot(work);
  s->magic = core::Snapshot::kMagic;
  s->ledger_index = idx;
  s->revision = revision_.load(std::memory_order_relaxed) + 1;
  generations_.push_back(s);
  ledger_used_ |= 1u << idx;
  live_count_.store(generations_.size(), std::memory_order_release);
  exchange_.publish(s);  // the one atomic store RT sees
  current_ = s;
  fx_cost_.store(cost, std::memory_order_release);
  revision_.store(s->revision, std::memory_order_release);
  reclaim();
  if (fader_handler_) fader_handler_->on_snapshot_published(*s);
  return true;
}

bool ControlPlane::raise_fence(std::uint32_t& seq) {
  const std::uint32_t next = fence_seq_ + 1;
  if (!rt_events_.push(StampedUmp{core::make_fence_event(next), clock_()})) return false;
  fence_seq_ = next;
  seq = next;
  return true;
}

bool ControlPlane::push_performance_value(const core::params::ParamDef& d, std::uint32_t part, double value) {
  return rt_events_.push(StampedUmp{core::make_param_event(d, part, value), clock_()});
}

ControlPlane::PublishStatus ControlPlane::can_publish(const core::Snapshot& work) {
  const std::uint64_t cost = core::fx_cost_units(work);
  if (cost > fx_budget() && (!current_ || cost > fx_cost_.load(std::memory_order_acquire))) return PublishStatus::kFxBudgetExceeded;
  constexpr std::uint32_t kAll = core::SnapshotExchange::kLedgerSlots >= 32 ? 0xFFFFFFFFu : ((1u << core::SnapshotExchange::kLedgerSlots) - 1);
  if (ledger_used_ == kAll) reclaim();
  return ledger_used_ == kAll ? PublishStatus::kBusy : PublishStatus::kOk;
}

ControlPlane::PublishStatus ControlPlane::publish_snapshot(const core::Snapshot& work, std::string& detail) {
  if (fault_set_) {
    fault_set_ = false;
    detail = "injected publish fault";
    return fault_;
  }
  Fail f;
  if (commit(work, f)) return PublishStatus::kOk;
  detail = f.message;
  for (const auto& kv : f.details) detail += " [" + kv.first + "=" + kv.second + "]";
  if (f.code == ws::v1::ERROR_CODE_FX_BUDGET_EXCEEDED) return PublishStatus::kFxBudgetExceeded;
  if (f.code == ws::v1::ERROR_CODE_BUSY) return PublishStatus::kBusy;
  return PublishStatus::kInternal;
}

std::size_t ControlPlane::reclaim() {
  std::size_t n = 0;
  for (std::size_t i = 0; i < generations_.size();) {
    core::Snapshot* g = generations_[i];
    if (g != current_ && exchange_.safe_to_free(g)) {
      ledger_used_ &= ~(1u << g->ledger_index);
      g->magic = 0;  // poison: a stale reader (a bug) would see it
      delete g;
      generations_.erase(generations_.begin() + static_cast<std::ptrdiff_t>(i));
      ++n;
    } else {
      ++i;
    }
  }
  if (n) {
    freed_.fetch_add(n, std::memory_order_release);
    live_count_.store(generations_.size(), std::memory_order_release);
  }
  return n;
}

bool ControlPlane::load_initial(const ws::v1::State& config, std::string& err) {
  if (current_) {
    err = "an initial generation is already loaded";
    return false;
  }
  core::Snapshot work = core::Snapshot::defaults();
  Fail f;
  if (!apply_config(work, config, f) || !commit(work, f)) {
    err = f.message;
    for (const auto& kv : f.details) err += " [" + kv.first + "=" + kv.second + "]";
    return false;
  }
  return true;
}

// Story 4.10: the encoder mode / selected part as commanded and as RT applied them, the ASSIGN map of the Live Set that sounds, the counters.
void ControlPlane::fill_encoders(ws::v1::State* out) const {
  ws::v1::EncoderStatus* e = out->mutable_encoders();
  e->set_mode(enc_mode_req_);
  e->set_selected_part(sel_part_req_);
  const core::EncoderState& rt = live_.encoder();
  e->set_rt_mode(static_cast<ws::v1::EncoderMode>(std::min(static_cast<int>(rt.mode()), 2) + 1));  // clamped: the enum cast never leaves PART..ASSIGN
  e->set_rt_selected_part(rt.selected_part());
  const core::EncoderState::Counters c = rt.counters();
  e->set_steps_applied(c.steps_applied);
  e->set_steps_empty_slot(c.steps_empty_slot);
  e->set_steps_unrouted(c.steps_unrouted);
  e->set_steps_rejected(c.steps_rejected);
  e->set_events_rejected(c.events_rejected);
  e->set_steps_no_part(c.steps_no_part);
  if (current_ && current_->live_set)
    for (std::size_t k = 0; k < core::kAssignSlotCount; ++k) e->add_assign(current_->assign[k]);
}

// Story 6.5: the RT content_integrity counter and the libraries the edge threads reported damaged.
void ControlPlane::fill_content(ws::v1::State* out) const {
  ws::v1::ContentStatus* c = out->mutable_content();
  c->set_content_integrity(live_.load_content_integrity());
  if (!damage_) return;
  const std::vector<std::string> ids = damage_->damaged_libraries();
  c->set_damaged_library_count(static_cast<std::uint32_t>(ids.size()));
  for (std::size_t i = 0; i < ids.size() && i < 16; ++i) c->add_damaged_library_ids(ids[i]);
  c->set_damage_reports(damage_->reports());
  c->set_io_errors(damage_->io_errors());
}

void ControlPlane::set_damage_registry(DamageRegistry* registry) {
  if (damage_) damage_->set_listener({});
  damage_ = registry;
  if (damage_) damage_->set_listener([this] { bump_revision(); });
}

void ControlPlane::fill_state(ws::v1::State* out) const {
  if (current_) fill_from(*current_, out);
  fill_encoders(out);
  fill_content(out);
  if (switch_handler_) switch_handler_->fill_status(out);
  if (setlist_handler_) setlist_handler_->fill_status(out);
  if (edit_handler_) edit_handler_->fill_status(out);
  if (fader_handler_) fader_handler_->fill_status(out);
}

void ControlPlane::fill_live_values(ws::v1::PerformanceValues* out) const {
  out->Clear();
  out->set_current_scene(current_ && current_->live_set ? current_->scene_index : -1);
  for (const p::ParamDef& d : p::kParams) {
    if (d.cls != p::Class::kPerformance) continue;
    const std::uint32_t n = d.scope == p::Scope::kPart ? p::kPartCount : 1;
    for (std::uint32_t part = 0; part < n; ++part) {
      ws::v1::ParamValue* pv = out->add_values();
      pv->set_param_id(pid(d.id));
      pv->set_part(part);
      pv->set_value(live_.load(p::perf_slot(d, part)));
    }
  }
  // Compressor gain reduction (Story 2.8): the insert slots that hold a compressor in the current generation, then the master ones.
  if (current_) {
    for (std::uint32_t part = 0; part < p::kPartCount; ++part) {
      const core::PartCfg& pc = current_->parts[part];
      for (std::uint32_t slot = 0; slot < pc.insert_count && slot < p::kInsertSlotCount; ++slot) {
        if (pc.inserts[slot].type != p::kInsertTypeCompressor) continue;
        ws::v1::GainReduction* g = out->add_gain_reductions();
        g->set_scope(ws::v1::GAIN_REDUCTION_SCOPE_INSERT);
        g->set_part(part);
        g->set_slot(slot);
        g->set_gain_reduction_db(live_.load_gain_reduction(core::LastValueTable::gr_insert_slot(part, slot)));
      }
    }
  }
  for (std::uint32_t bus = 0; bus < core::kOutputBusCount; ++bus) {
    ws::v1::GainReduction* g = out->add_gain_reductions();
    g->set_scope(bus == 0 ? ws::v1::GAIN_REDUCTION_SCOPE_MASTER_MAIN : ws::v1::GAIN_REDUCTION_SCOPE_MASTER_AUX);
    g->set_gain_reduction_db(live_.load_gain_reduction(core::LastValueTable::gr_master_slot(bus)));
  }
}

ws::v1::ServerMessage ControlPlane::handle(const ws::v1::ClientMessage& msg, bool& changed) {
  changed = false;
  ws::v1::ServerMessage reply;
  reply.set_request_id(msg.request_id());
  Fail f;
  bool ok = false;
  using B = ws::v1::ClientMessage;
  const bool scene_cmd = msg.body_case() == B::kRecallScene || msg.body_case() == B::kNextScene || msg.body_case() == B::kPreviousScene;
  const bool switch_cmd = msg.body_case() == B::kJumpToLiveSet || msg.body_case() == B::kCancelLoad;
  const bool setlist_cmd = msg.body_case() >= B::kCreateSetlist && msg.body_case() <= B::kGetSetlists;
  const bool edit_cmd = msg.body_case() == B::kSetPartTimbre || msg.body_case() == B::kWriteLiveSet || msg.body_case() == B::kSetAssignSlot ||
                        msg.body_case() == B::kClearAssignSlot;
  const bool fader_cmd = msg.body_case() == B::kSetFaderMode || msg.body_case() == B::kSelectOrganPart || msg.body_case() == B::kMoveFader;
  if (edit_cmd) {
    // Edits and Write (Story 4.9): the EditSession owns the work and answers with typed errors.
    if (!edit_handler_) {
      fail(f, ws::v1::ERROR_CODE_INVALID_COMMAND, "edits are not available: the engine was started without a Live Set directory");
    } else {
      HandlerFailure hf;
      bool bump = false, push = false;
      ok = edit_handler_->run(msg, reply, hf, bump, push);
      if (!ok) {
        f.code = hf.code;
        f.message = hf.message;
        f.details = hf.details;
      }
      if (bump) bump_revision();
      changed = push || bump;
    }
  } else if (setlist_cmd) {
    // Setlists, tags, Live Set creation and stepping (Story 4.6): the SetlistController owns the work and answers with typed errors.
    if (!setlist_handler_) {
      fail(f, ws::v1::ERROR_CODE_INVALID_COMMAND, "setlists are not available: the engine was started without a data directory");
    } else {
      HandlerFailure hf;
      bool bump = false, push = false;
      ok = setlist_handler_->run(msg, reply, hf, bump, push);
      if (!ok) {
        f.code = hf.code;
        f.message = hf.message;
        f.details = hf.details;
      }
      if (bump) bump_revision();  // also for a partial failure that changed the state (DeleteLiveSet)
      changed = push || bump;
    }
  } else if (switch_cmd) {
    // Live Set switching (Story 4.5): the switcher owns the work; a jump returns at once, the load runs on its worker.
    if (!switch_handler_ || !current_) {
      fail(f, ws::v1::ERROR_CODE_NO_LIVE_SET, "no Live Set catalog is available");
    } else if (msg.body_case() == B::kCancelLoad) {
      const SwitchOutcome o = switch_handler_->cancel();
      reply.mutable_cancel_load_reply()->set_cancelled(o.cancelled);
      ok = true;
      changed = o.cancelled;  // the loading status changed: subscribers get a State push
    } else {
      const SwitchOutcome o = switch_handler_->jump(msg.jump_to_live_set().id());
      if (o.result == SwitchOutcome::Result::kUnknownLiveSet) {
        f.details["id"] = msg.jump_to_live_set().id();
        fail(f, ws::v1::ERROR_CODE_NO_SUCH_LIVE_SET, "unknown Live Set id");
      } else {
        ws::v1::JumpReply* jr = reply.mutable_jump_reply();
        jr->set_result(o.result == SwitchOutcome::Result::kAccepted ? ws::v1::JUMP_RESULT_ACCEPTED
                       : o.result == SwitchOutcome::Result::kAlreadyLoaded ? ws::v1::JUMP_RESULT_ALREADY_LOADED
                                                                            : ws::v1::JUMP_RESULT_ALREADY_LOADING);
        jr->set_target_id(o.target_id);
        jr->set_target_name(o.target_name);
        jr->set_superseded_previous(o.superseded_previous);
        ok = true;
        changed = o.result == SwitchOutcome::Result::kAccepted || o.superseded_previous;  // a no-op pushes nothing
      }
    }
  } else if (scene_cmd) {
    // Scene commands (Story 4.4) never need a copy of the snapshot here: the handler builds its own.
    const SceneHandler::Op op = msg.body_case() == B::kRecallScene ? SceneHandler::Op::kRecall
                                : msg.body_case() == B::kNextScene ? SceneHandler::Op::kNext
                                                                     : SceneHandler::Op::kPrevious;
    const std::uint32_t index = op == SceneHandler::Op::kRecall ? msg.recall_scene().index() : 0;
    SceneOutcome o;
    o.result = SceneResult::kNoLiveSet;
    if (scene_handler_ && current_) o = scene_handler_->run(op, index);
    if (o.ok()) {
      ws::v1::SceneReply* sr = reply.mutable_scene_reply();
      sr->set_result(o.result == SceneResult::kOk ? ws::v1::SCENE_STEP_RESULT_OK
                     : o.result == SceneResult::kAtFirst ? ws::v1::SCENE_STEP_RESULT_AT_FIRST
                                                         : ws::v1::SCENE_STEP_RESULT_AT_LAST);
      sr->set_current_scene(o.scene);
      ok = true;
      changed = o.published;
    } else {
      f.message = to_string(o.result);
      f.details["result"] = to_string(o.result);
      switch (o.result) {
        case SceneResult::kNoSuchScene:
          f.code = ws::v1::ERROR_CODE_NO_SUCH_SCENE;
          f.details["index"] = std::to_string(index);
          f.details["scene_count"] = std::to_string(current_ && current_->live_set ? current_->scene_count : 0);
          break;
        case SceneResult::kNoLiveSet: f.code = ws::v1::ERROR_CODE_NO_LIVE_SET; break;
        case SceneResult::kLoadInProgress:
        case SceneResult::kBusy:
          f.code = ws::v1::ERROR_CODE_BUSY;
          f.details["reason"] = o.busy_reason;
          break;
        case SceneResult::kFxBudgetExceeded:
          f.code = ws::v1::ERROR_CODE_FX_BUDGET_EXCEEDED;
          f.details["fx_cost"] = std::to_string(fx_cost());
          f.details["fx_budget"] = std::to_string(fx_budget());
          break;
        default: f.code = ws::v1::ERROR_CODE_INTERNAL; break;
      }
    }
  } else if (fader_cmd) {
    // Fader commands (Story 4.12): the FaderController owns the work and answers with typed errors.
    if (!fader_handler_) {
      fail(f, ws::v1::ERROR_CODE_INVALID_COMMAND, "fader controller is not available");
    } else {
      HandlerFailure hf;
      bool bump = false, push = false;
      ok = fader_handler_->run(msg, reply, hf, bump, push);
      if (!ok) {
        f.code = hf.code;
        f.message = hf.message;
        f.details = hf.details;
      }
      if (bump) bump_revision();
      changed = push || bump;
    }
  } else if (!current_) {
    fail(f, ws::v1::ERROR_CODE_INTERNAL, "no snapshot generation loaded");
  } else {
    core::Snapshot work = *current_;
    bool structural = false;
    std::string blocked;
    const bool structural_msg = msg.body_case() == B::kSetInsertChain || msg.body_case() == B::kSetSendLevels || msg.body_case() == B::kSetPartOutput ||
                                msg.body_case() == B::kSetMasterParams || msg.body_case() == B::kSetPartVolume;
    const bool was_blocked = structural_msg && edit_handler_ && edit_handler_->structural_blocked(blocked);
    if (was_blocked) {  // refused like any other command: the shared tail below builds the error reply and counts the rejection
      f.details["reason"] = blocked;
      fail(f, ws::v1::ERROR_CODE_BUSY, "the Live Set is being rebuilt from its edited state; try again in a moment");
    }
    if (msg.body_case() == B::kGetState && edit_handler_) edit_handler_->refresh();
    if (!was_blocked) switch (msg.body_case()) {
      case B::kSetInsertChain:
        ok = apply_insert_chain(work, msg.set_insert_chain().part(), msg.set_insert_chain().slots(), f);
        structural = true;
        break;
      case B::kSetSendLevels: {
        const auto& m = msg.set_send_levels();
        ok = check_part(m.part(), f) && apply_send(work, m.part(), p::kSendReverbLevel, m.reverb(), f) &&
             apply_send(work, m.part(), p::kSendDelayLevel, m.delay(), f);
        structural = true;
        break;
      }
      case B::kSetPartOutput:
        ok = apply_part_output(work, msg.set_part_output().part(), msg.set_part_output().output(), f);
        structural = true;
        break;
      case B::kSetMasterParams:
        if (msg.set_master_params().values_size() == 0) {
          fail(f, ws::v1::ERROR_CODE_INVALID_COMMAND, "SetMasterParams carries no values");
        } else {
          ok = apply_master(work, msg.set_master_params().values(), f);
        }
        structural = true;
        break;
      case B::kSetPartVolume: {
        const auto& m = msg.set_part_volume();
        ok = check_part(m.part(), f);
        if (ok && (!std::isfinite(m.volume()) || m.volume() < 0.0f || m.volume() > 1.0f)) {
          f.details["part"] = std::to_string(m.part());
          f.details["min"] = "0";
          f.details["max"] = "1";
          ok = fail(f, ws::v1::ERROR_CODE_OUT_OF_RANGE, "part volume outside 0..1");
        }
        if (ok) work.parts[m.part()].volume = m.volume();
        structural = true;
        break;
      }
      case B::kSetTimbreRef:
        ok = check_part(msg.set_timbre_ref().part(), f);
        if (ok) work.parts[msg.set_timbre_ref().part()].timbre_ref = msg.set_timbre_ref().timbre_ref();
        structural = true;
        break;
      case B::kSetPerformanceParam: {
        const auto& m = msg.set_performance_param();
        const p::ParamDef* d = lookup(m.param_id(), f);
        if (!d) break;
        if (d->cls != p::Class::kPerformance) {
          f.details["param_id"] = std::to_string(d->id);
          fail(f, ws::v1::ERROR_CODE_WRONG_CLASS, "structural parameter sent as a performance change");
          break;
        }
        std::uint32_t part = 0;
        if (d->scope == p::Scope::kPart) {
          part = m.part();
          if (!check_part(part, f)) break;
        }
        if (!check_value(*d, m.value(), f)) break;
        const core::Ump u = core::make_param_event(*d, part, m.value());
        if (!rt_events_.push(StampedUmp{u, clock_()})) {
          fail(f, ws::v1::ERROR_CODE_BUSY, "the real-time event queue is full");
          break;
        }
        ok = true;
        break;
      }
      case B::kGetState:
        fill_state(reply.mutable_state());
        ok = true;
        break;
      case B::kGetPerformanceValues:
        fill_live_values(reply.mutable_performance_values());
        ok = true;
        break;
      case B::kSubscribe:
        ok = true;
        break;
      case B::kRecallScene:
      case B::kNextScene:
      case B::kPreviousScene:
      case B::kJumpToLiveSet:
      case B::kCancelLoad:
        break;  // handled before the snapshot is copied
      case B::kCreateSetlist:
      case B::kRenameSetlist:
      case B::kReorderSetlists:
      case B::kDeleteSetlist:
      case B::kAddLiveSetToSetlist:
      case B::kRemoveLiveSetFromSetlist:
      case B::kReorderLiveSetsInSetlist:
      case B::kCreateLiveSet:
      case B::kDeleteLiveSet:
      case B::kCreateTag:
      case B::kRenameTag:
      case B::kDeleteTag:
      case B::kSetSetlistTags:
      case B::kLoadSetlist:
      case B::kUnloadSetlist:
      case B::kNextLiveSet:
      case B::kPreviousLiveSet:
      case B::kGetSetlists:
        break;  // handled before the snapshot is copied
      case B::kSetPartTimbre:
      case B::kWriteLiveSet:
      case B::kSetAssignSlot:
      case B::kClearAssignSlot:
      case B::kSetFaderMode:
      case B::kSelectOrganPart:
      case B::kMoveFader:
        break;  // handled before the snapshot is copied
      case B::kSetEncoderMode: {  // Story 4.10: a UMP on the control lane, in order with the encoder steps; the plane remembers what it commanded
        const ws::v1::EncoderMode m = msg.set_encoder_mode().mode();
        if (m < ws::v1::ENCODER_MODE_PART || m > ws::v1::ENCODER_MODE_ASSIGN) {
          f.details["mode"] = std::to_string(static_cast<int>(m));
          fail(f, ws::v1::ERROR_CODE_INVALID_COMMAND, "SetEncoderMode needs PART, ALL or ASSIGN");
          break;
        }
        if (!rt_events_.push(StampedUmp{core::make_encoder_mode_event(static_cast<core::EncoderMode>(static_cast<int>(m) - 1)), clock_()})) {
          fail(f, ws::v1::ERROR_CODE_BUSY, "the real-time event queue is full");
          break;
        }
        ok = true;
        if (m != enc_mode_req_) {
          enc_mode_req_ = m;
          bump_revision();
          changed = true;
        } else if (static_cast<int>(live_.encoder().mode()) + 1 != static_cast<int>(m)) {
          changed = true;  // a repeated command while RT still shows another mode: publish the State again
        }
        break;
      }
      case B::kSelectPart: {
        const std::uint32_t part = msg.select_part().part();
        if (!check_part(part, f)) break;
        if (!rt_events_.push(StampedUmp{core::make_select_part_event(part), clock_()})) {
          fail(f, ws::v1::ERROR_CODE_BUSY, "the real-time event queue is full");
          break;
        }
        ok = true;
        if (part != sel_part_req_) {
          sel_part_req_ = part;
          bump_revision();
          changed = true;
        } else if (live_.encoder().selected_part() != part) {
          changed = true;
        }
        break;
      }
      case B::kHello:
        fail(f, ws::v1::ERROR_CODE_INVALID_COMMAND, "handshake already done on this connection");
        break;
      case B::BODY_NOT_SET:
        fail(f, ws::v1::ERROR_CODE_INVALID_COMMAND, "no command set");
        break;
    }
    if (ok && structural) {
      ok = commit(work, f);
      changed = ok;
      if (ok && edit_handler_ && structural_msg) edit_handler_->on_structural(msg);  // Story 4.9: record the edit against the Live Set
    }
  }
  if (!ok) {
    rejected_.fetch_add(1, std::memory_order_relaxed);
    reply.clear_state();
    reply.clear_performance_values();
    ws::v1::Error* e = reply.mutable_error();
    e->set_code(f.code);
    e->set_message(f.message);
    for (const auto& kv : f.details) (*e->mutable_details())[kv.first] = kv.second;
  } else if (reply.body_case() == ws::v1::ServerMessage::BODY_NOT_SET) {
    reply.mutable_ack();
  }
  reply.set_revision(revision());
  if (ok && reply.has_state()) reply.mutable_state()->set_revision(revision());
  return reply;
}

}  // namespace velora::host
