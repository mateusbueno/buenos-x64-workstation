#include "velora/host/live_set_view.hpp"

#include <algorithm>
#include <unistd.h>

#include <cmath>

#include "live_set.pb.h"
#include "velora/core/engine.hpp"
#include "velora/core/params_gen.hpp"
#include "velora/format/category.hpp"

namespace velora::host {

namespace {

namespace p = core::params;
namespace lsp = ::velora::liveset::v1;

constexpr std::uint32_t kSampleEngine = static_cast<std::uint32_t>(core::EngineTypeId::kSample);
constexpr std::uint32_t kOrganEngine = static_cast<std::uint32_t>(core::EngineTypeId::kOrgan);

float drawbar_value(const lsp::Part& part, std::uint32_t id) {
  const p::ParamDef* d = p::find(id);
  float v = d ? static_cast<float>(d->def) : 0.0f;
  for (const lsp::ParamEntry& e : part.engine().params())
    if (e.param_id() == id) v = e.value();
  return v;
}

}  // namespace

std::string organ_registration_text(const float (&drawbars)[9]) {
  std::string out;
  for (int i = 0; i < 9; ++i) {
    if (i == 2 || i == 6) out.push_back(' ');
    const float v = std::isfinite(drawbars[i]) ? drawbars[i] : 0.0f;
    out.push_back(static_cast<char>('0' + std::clamp(static_cast<int>(std::lround(v)), 0, 8)));
  }
  return out;
}

// The bundle id the library's `current` link names, "" when there is none (a plain read of one link, no scanning).
std::string current_bundle_id(const std::string& root, const std::string& library_id) {
  const std::string link = root + "/" + kLibDirName + "/" + library_id + "/" + kCurrentLinkName;
  char buf[256];
  const ssize_t n = ::readlink(link.c_str(), buf, sizeof buf);
  return n > 0 && static_cast<std::size_t>(n) < sizeof buf ? std::string(buf, static_cast<std::size_t>(n)) : std::string();
}

bool LiveSetViewService::lookup_timbre(const std::string& library_id, const std::string& instrument_key, Resolved& out, std::string& reason) {
  format::Uuid probe;
  const bool valid = parse_library_id(library_id, probe) && !instrument_key.empty() && instrument_key.size() <= liveset::kMaxInstrumentKeyBytes;
  const std::string key = library_id + '\n' + instrument_key;
  if (valid && library_id != kBuiltinLibraryId) {
    const auto it = index_.find(key);
    if (it != index_.end()) {
      const std::string now = current_bundle_id(resolver_.root(), library_id);
      if (!now.empty() && now == it->second->bundle_id) {
        lru_.splice(lru_.begin(), lru_, it->second);  // most recently used first
        out = *it->second;
        ++hits_;
        return true;
      }
      lru_.erase(it->second);
      index_.erase(it);
    }
  }
  ++misses_;
  const ResolveResult rr = resolver_.resolve(library_id, instrument_key);
  if (!rr.ok()) {
    reason = to_string(rr.status);
    return false;
  }
  out = Resolved{};
  out.key = key;
  out.name = rr.instrument_info().name;
  if (!rr.is_builtin && !rr.bundle_dir.empty()) {
    const format::CategoryFileResult cf = format::read_category_file(rr.bundle_dir + "/category");
    if (cf.state == format::CategoryFileState::kValid) out.category = static_cast<std::uint32_t>(cf.category);
    const std::size_t slash = rr.bundle_dir.find_last_of('/');
    out.bundle_id = slash == std::string::npos ? std::string() : rr.bundle_dir.substr(slash + 1);
    if (!out.bundle_id.empty()) {
      lru_.push_front(out);
      index_[key] = lru_.begin();
      while (lru_.size() > kCacheCapacity) {
        index_.erase(lru_.back().key);
        lru_.pop_back();
      }
    }
  }
  return true;
}

LiveSetViewService::LiveSetViewService(ControlPlane& plane, liveset::LiveSetStore& live, const LibraryResolver& resolver, const EditSession* edits, SwitchPort* port)
    : plane_(plane), live_(live), resolver_(resolver), edits_(edits), port_(port) {
  plane_.set_view_handler(this);
}

LiveSetViewService::~LiveSetViewService() { plane_.set_view_handler(nullptr); }

bool LiveSetViewService::run(const ws::v1::ClientMessage& msg, ws::v1::ServerMessage& reply, HandlerFailure& err) {
  auto fail = [&err](ws::v1::ErrorCode code, std::string text, std::map<std::string, std::string> details = {}) {
    err.code = code;
    err.message = std::move(text);
    err.details = std::move(details);
    return false;
  };
  if (msg.body_case() != ws::v1::ClientMessage::kGetLiveSetView) return fail(ws::v1::ERROR_CODE_INVALID_COMMAND, "not a Live Set view command");
  const std::string& id = msg.get_live_set_view().live_set_id();
  if (id.empty()) return fail(ws::v1::ERROR_CODE_INVALID_COMMAND, "GetLiveSetView needs a live_set_id", {{"reason", "empty_id"}});

  // The edited copy when the session holds one, else the stored file. Both are copied out of their owners: nothing here keeps a pointer past the call.
  lsp::LiveSet ls;
  bool have = false;
  if (edits_)
    if (const lsp::LiveSet* w = edits_->working_of(id)) {
      ls = *w;
      have = true;
    }
  if (!have) {
    liveset::LoadResult lr;
    const liveset::Status st = live_.read(id, lr);
    // Only a canonical id is echoed back (an arbitrary client string never is: error details stay small and clean).
    std::map<std::string, std::string> who;
    if (liveset::is_canonical_uuid(id)) who["id"] = id; else who["reason"] = "bad_id";
    if (st == liveset::Status::kNotFound || st == liveset::Status::kBadId) return fail(ws::v1::ERROR_CODE_NO_SUCH_LIVE_SET, "unknown Live Set id", who);
    who["reason"] = liveset::to_string(st);
    if (st != liveset::Status::kOk) return fail(ws::v1::ERROR_CODE_LIVE_SET_LOAD_FAILED, "the Live Set could not be read", who);
    ls = std::move(lr.live_set);
  }

  ws::v1::LiveSetViewReply* out = reply.mutable_live_set_view_reply();
  out->set_live_set_id(id);
  out->set_name(ls.name());
  out->set_dirty(edits_ && edits_->is_dirty(id));
  const std::string sounding = port_ ? port_->sounding_id() : (edits_ ? edits_->current_id() : std::string());
  out->set_sounding(!sounding.empty() && sounding == id);
  out->set_factory(ls.has_factory());
  out->set_scene_count(static_cast<std::uint32_t>(std::min<int>(ls.scenes_size(), static_cast<int>(liveset::kMaxScenes))));
  for (std::uint32_t i = 0; i < liveset::kMaxScenes; ++i) {
    ws::v1::LiveSetViewScene* sc = out->add_scenes();
    sc->set_index(i);
    const bool stored = static_cast<int>(i) < ls.scenes_size();
    sc->set_empty(!stored);
    if (stored) sc->set_name(ls.scenes(static_cast<int>(i)).name());
  }

  const int part_count = std::min(ls.parts_size(), static_cast<int>(plane_.part_cap()));
  for (int i = 0; i < part_count; ++i) {
    const lsp::Part& lp = ls.parts(i);
    ws::v1::LiveSetViewPart* vp = out->add_parts();
    const std::uint32_t et = lp.engine().engine_type_id();
    vp->set_index(static_cast<std::uint32_t>(i));
    vp->set_on(lp.keyboard_on());
    vp->set_engine_type_id(et);
    vp->set_volume(std::isfinite(lp.volume()) ? std::min(std::max(lp.volume(), 0.0f), 1.0f) : 1.0f);
    vp->set_low_key(std::min<std::uint32_t>(lp.low_key(), 127));
    vp->set_high_key(std::min<std::uint32_t>(lp.high_key(), 127));
    const bool sample = et == kSampleEngine || et == 0;
    if (et == kOrganEngine) {
      static constexpr std::uint32_t kIds[9] = {p::kDrawbar16, p::kDrawbar513, p::kDrawbar8, p::kDrawbar4, p::kDrawbar223, p::kDrawbar2, p::kDrawbar135, p::kDrawbar113, p::kDrawbar1};
      float v[9];
      for (int k = 0; k < 9; ++k) v[k] = drawbar_value(lp, kIds[k]);
      vp->set_organ_registration(organ_registration_text(v));
      vp->set_category(static_cast<std::uint32_t>(format::Category::kOrgan));
      continue;
    }
    if (!lp.has_timbre()) {
      vp->set_empty(sample);
      continue;
    }
    Resolved r;
    std::string reason;
    if (!lookup_timbre(lp.timbre().library_id(), lp.timbre().instrument_key(), r, reason)) {
      vp->set_missing_library(sample);  // never a substitute: the name stays empty
      if (sample) vp->set_unavailable_reason(reason);
      continue;
    }
    vp->set_timbre_name(r.name);
    vp->set_category(r.category);
  }
  out->set_part_count(static_cast<std::uint32_t>(out->parts_size()));
  return true;
}

}  // namespace velora::host
