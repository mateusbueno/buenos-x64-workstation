#include "velora/host/control_plane.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

#include "velora/core/param_events.hpp"

namespace velora::host {

namespace p = core::params;
using ws::v1::ErrorCode;

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
  for (core::Snapshot* g : generations_) delete g;
}

bool ControlPlane::commit(const core::Snapshot& work, ControlFail& f) {
  constexpr std::uint32_t kAll = core::SnapshotExchange::kLedgerSlots >= 32
                                     ? 0xFFFFFFFFu
                                     : ((1u << core::SnapshotExchange::kLedgerSlots) - 1);
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
  revision_.store(s->revision, std::memory_order_release);
  reclaim();
  return true;
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

void ControlPlane::fill_state(ws::v1::State* out) const {
  if (current_) fill_from(*current_, out);
}

void ControlPlane::fill_live_values(ws::v1::PerformanceValues* out) const {
  out->Clear();
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
}

ws::v1::ServerMessage ControlPlane::handle(const ws::v1::ClientMessage& msg, bool& changed) {
  changed = false;
  ws::v1::ServerMessage reply;
  reply.set_request_id(msg.request_id());
  Fail f;
  bool ok = false;
  using B = ws::v1::ClientMessage;
  if (!current_) {
    fail(f, ws::v1::ERROR_CODE_INTERNAL, "no snapshot generation loaded");
  } else {
    core::Snapshot work = *current_;
    bool structural = false;
    switch (msg.body_case()) {
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
