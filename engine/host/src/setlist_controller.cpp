// Setlist commands, loaded setlist and LIVE SET stepping, Story 4.6. Control thread only; see velora/host/setlist_controller.hpp.
#include "velora/host/setlist_controller.hpp"

#include <algorithm>
#include <sys/stat.h>

#include <unordered_map>

namespace velora::host {

namespace sl = ::velora::host::setlist;
namespace slp = ::velora::setlist::v1;
using ws::v1::ClientMessage;
using B = ws::v1::ClientMessage;

namespace {

unsigned char fold(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return (u >= 'a' && u <= 'z') ? static_cast<unsigned char>(u - 'a' + 'A') : u;
}

bool fail(HandlerFailure& e, ws::v1::ErrorCode code, std::string msg, std::map<std::string, std::string> details = {}) {
  e.code = code;
  e.message = std::move(msg);
  e.details = std::move(details);
  return false;
}

// Maps a refused store command to the closed ws.v1 error set.
bool fail_status(HandlerFailure& e, sl::Status s, const std::string& id = {}, const char* limit_name = "") {
  std::map<std::string, std::string> d;
  if (!id.empty()) d["id"] = id;
  switch (s) {
    case sl::Status::kNoSuchSetlist: return fail(e, ws::v1::ERROR_CODE_NO_SUCH_SETLIST, "no such setlist", d);
    case sl::Status::kNoSuchTag: return fail(e, ws::v1::ERROR_CODE_NO_SUCH_TAG, "no such tag", d);
    case sl::Status::kNoSuchLiveSet:
    case sl::Status::kBadId: return fail(e, ws::v1::ERROR_CODE_NO_SUCH_LIVE_SET, "no such Live Set", d);
    case sl::Status::kDuplicateLiveSet:
      return fail(e, ws::v1::ERROR_CODE_DUPLICATE_LIVE_SET_IN_SETLIST, "the Live Set is already in the setlist", d);
    case sl::Status::kNotInSetlist: return fail(e, ws::v1::ERROR_CODE_LIVE_SET_NOT_IN_SETLIST, "the Live Set is not in the setlist", d);
    case sl::Status::kInvalidName:
      return fail(e, ws::v1::ERROR_CODE_INVALID_NAME, "invalid name (1..64 bytes of UTF-8, no control characters)", d);
    case sl::Status::kInvalidOrder:
      return fail(e, ws::v1::ERROR_CODE_INVALID_ORDER, "not a permutation of the current ids, or an index past the end", d);
    case sl::Status::kLimitReached:
      d["limit_name"] = limit_name;
      d["limit"] = std::to_string(std::string(limit_name) == "setlists"                ? sl::kMaxSetlists
                                  : std::string(limit_name) == "tags"                   ? sl::kMaxTags
                                  : std::string(limit_name) == "live_sets_per_setlist" ? sl::kMaxLiveSetsPerSetlist
                                                                                        : sl::kMaxTagsPerSetlist);
      return fail(e, ws::v1::ERROR_CODE_LIMIT_REACHED, "limit reached", d);
    case sl::Status::kLiveSetInUse: return fail(e, ws::v1::ERROR_CODE_LIVE_SET_IN_USE, "the Live Set sounds or is loading", d);
    default:
      d["reason"] = sl::to_string(s);
      return fail(e, ws::v1::ERROR_CODE_STORAGE_FAILED, "the change could not be stored; nothing changed", d);
  }
}

}  // namespace

bool live_set_name_less(const CatalogEntry& a, const CatalogEntry& b) {
  const std::size_t n = std::min(a.name.size(), b.name.size());
  for (std::size_t i = 0; i < n; ++i) {
    const unsigned char x = fold(a.name[i]), y = fold(b.name[i]);
    if (x != y) return x < y;
  }
  if (a.name.size() != b.name.size()) return a.name.size() < b.name.size();
  return a.id < b.id;
}

const char* to_string(StepResult r) {
  switch (r) {
    case StepResult::kMoved: return "moved";
    case StepResult::kEndOfList: return "end_of_setlist";
    case StepResult::kStartOfList: return "start_of_setlist";
    case StepResult::kEmptyList: return "empty";
    case StepResult::kLoadFailed: return "load_failed";
  }
  return "?";
}

SetlistController::SetlistController(ControlPlane& plane, sl::SetlistStore& store, liveset::LiveSetStore& live, SwitchPort& port)
    : plane_(plane), store_(store), live_(live), port_(port) {
  plane_.set_setlist_handler(this);
  last_seen_ = port_.effective_id();
}

SetlistController::~SetlistController() { plane_.set_setlist_handler(nullptr); }

const CatalogEntry* SetlistController::find_catalog(const std::string& id) const {
  for (const CatalogEntry& e : port_.catalog())
    if (e.id == id) return &e;
  return nullptr;
}

bool SetlistController::known_live_set(const std::string& id, CatalogEntry* out) {
  if (!liveset::is_uuid_v4(id)) return false;
  if (const CatalogEntry* e = find_catalog(id)) {
    if (out) *out = *e;
    return true;
  }
  liveset::LoadResult lr;  // a Live Set written by someone else since the catalog was made: adopt it when it is valid
  if (live_.read(id, lr) != liveset::Status::kOk) return false;
  CatalogEntry e{id, lr.live_set.name(), live_.path_for(id)};
  port_.add_to_catalog(e);
  if (out) *out = e;
  return true;
}

std::vector<CatalogEntry> SetlistController::all_sorted() const {
  std::vector<CatalogEntry> v = port_.catalog();
  std::sort(v.begin(), v.end(), live_set_name_less);
  return v;
}

// Follows the current Live Set: a change clears the end / start flags and, when the new one is in the loaded setlist, moves the position to it.
void SetlistController::sync() const {
  const std::string cur = port_.effective_id();
  if (cur == last_seen_) return;
  last_seen_ = cur;
  end_ = start_ = false;
  ++seq_;
  if (loaded_.empty()) return;
  const slp::Setlist* s = store_.find_setlist(loaded_);
  if (!s) return;
  for (int i = 0; i < s->live_set_ids_size(); ++i)
    if (s->live_set_ids(i) == cur) {
      position_ = i;
      anchor_ = cur;
      return;
    }
}

void SetlistController::reanchor() {
  const slp::Setlist* s = loaded_.empty() ? nullptr : store_.find_setlist(loaded_);
  if (!s) {
    position_ = -1;
    anchor_.clear();
    return;
  }
  if (anchor_.empty()) return;
  for (int i = 0; i < s->live_set_ids_size(); ++i)
    if (s->live_set_ids(i) == anchor_) {
      position_ = i;
      ++seq_;
      return;
    }
  // The anchor Live Set left the setlist: stay on its predecessor so "next" lands on what took its place.
  position_ = std::max(-1, std::min(position_ - 1, s->live_set_ids_size() - 1));
  anchor_ = position_ >= 0 ? s->live_set_ids(position_) : std::string();
  ++seq_;
}

int SetlistController::last_setlist_position() const {
  sync();
  return loaded_.empty() ? -1 : position_;
}
bool SetlistController::end_of_setlist() const {
  sync();
  return end_;
}
bool SetlistController::start_of_setlist() const {
  sync();
  return start_;
}

bool SetlistController::tick() {
  sync();
  const bool changed = seq_ != reported_seq_;
  reported_seq_ = seq_;
  return changed;
}

sl::Status SetlistController::load_setlist(const std::string& id) {
  sync();
  const slp::Setlist* s = store_.find_setlist(id);
  if (!s) return sl::Status::kNoSuchSetlist;
  if (loaded_ == id) return sl::Status::kOk;  // already the loaded one: nothing changes
  loaded_ = id;
  position_ = -1;
  anchor_.clear();
  end_ = start_ = false;
  ++seq_;
  // Nothing is loaded into the engine. If the Live Set that sounds happens to be in the new setlist, the position is its index.
  const std::string cur = port_.effective_id();
  for (int i = 0; i < s->live_set_ids_size(); ++i)
    if (s->live_set_ids(i) == cur) {
      position_ = i;
      anchor_ = cur;
    }
  return sl::Status::kOk;
}

void SetlistController::unload_setlist() {
  sync();
  if (loaded_.empty()) return;
  loaded_.clear();
  position_ = -1;
  anchor_.clear();
  end_ = start_ = false;
  ++seq_;
}

StepOutcome SetlistController::jump_target(const std::string& id, StepOutcome o) {
  o.live_set_id = id;
  CatalogEntry entry;
  if (!known_live_set(id, &entry)) {
    liveset::LoadResult lr;
    const liveset::Status rs = live_.read(id, lr);
    o.result = StepResult::kLoadFailed;
    if (rs == liveset::Status::kNotFound) o.reason = "missing";
    else if (rs == liveset::Status::kIo || rs == liveset::Status::kTooLarge || rs == liveset::Status::kOk) o.reason = "read_failed";
    else o.reason = "invalid_live_set";
    return o;
  }
  o.live_set_name = entry.name;
  // A file that has become unreadable or invalid since the catalog was made is refused here, before anything moves. The catalog's own path is
  // read (the Live Set directory may hold files that are not named <id>.liveset; those still load and step).
  liveset::LoadResult lr;
  liveset::ReadOptions ro;
  ro.part_count = live_.options().part_count;
  ro.current_version = live_.options().current_version;
  const liveset::Status rs = entry.path.empty() ? live_.read(id, lr) : liveset::read_file(entry.path, ro, lr);
  if (rs != liveset::Status::kOk) {
    o.result = StepResult::kLoadFailed;
    if (rs == liveset::Status::kNotFound) o.reason = "missing";
    else if (rs == liveset::Status::kIo || rs == liveset::Status::kTooLarge) o.reason = "read_failed";
    else o.reason = "invalid_live_set";
    return o;
  }
  o.jump = port_.jump_to(id);
  if (o.jump.result == SwitchOutcome::Result::kUnknownLiveSet) {
    o.result = StepResult::kLoadFailed;
    o.reason = "missing";
    return o;
  }
  // The position follows the request only when the switcher really took it: its current Live Set must now be the target.
  if (port_.effective_id() != id) {
    o.result = StepResult::kLoadFailed;
    o.reason = "switch_not_applied";
    return o;
  }
  o.result = StepResult::kMoved;
  return o;
}

StepOutcome SetlistController::step_in(bool forward, const slp::Setlist& s) {
  StepOutcome o;
  o.in_setlist = true;
  const int n = s.live_set_ids_size();
  if (n == 0) {
    o.result = StepResult::kEmptyList;
    return o;
  }
  const int target = forward ? position_ + 1 : position_ - 1;
  if (target >= n || target < 0) {
    const bool was = forward ? end_ : start_;
    o.result = forward ? StepResult::kEndOfList : StepResult::kStartOfList;
    o.position = position_;
    (forward ? end_ : start_) = true;
    if (!was) ++seq_;
    return o;
  }
  const std::string id = s.live_set_ids(target);
  o = jump_target(id, o);
  o.position = target;
  if (o.result == StepResult::kMoved) {
    position_ = target;
    anchor_ = id;
    last_seen_ = port_.effective_id();  // == id (checked in jump_target)
    end_ = start_ = false;
    ++seq_;
  }
  return o;
}

StepOutcome SetlistController::step_all(bool forward) {
  StepOutcome o;
  o.in_setlist = false;
  const std::vector<CatalogEntry> all = all_sorted();
  if (all.empty()) {
    o.result = StepResult::kEmptyList;
    return o;
  }
  int cur = -1;
  const std::string cid = port_.effective_id();
  for (std::size_t i = 0; i < all.size(); ++i)
    if (all[i].id == cid) cur = static_cast<int>(i);
  const int n = static_cast<int>(all.size());
  int target;
  if (cur < 0) target = forward ? 0 : n - 1;
  else target = forward ? cur + 1 : cur - 1;
  if (target >= n || target < 0) {
    const bool was = forward ? end_ : start_;
    o.result = forward ? StepResult::kEndOfList : StepResult::kStartOfList;
    o.position = cur;
    (forward ? end_ : start_) = true;
    if (!was) ++seq_;
    return o;
  }
  o = jump_target(all[static_cast<std::size_t>(target)].id, o);
  o.position = target;
  if (o.result == StepResult::kMoved) {
    last_seen_ = port_.effective_id();
    end_ = start_ = false;
    ++seq_;
  }
  return o;
}

StepOutcome SetlistController::step(bool forward) {
  sync();
  if (!loaded_.empty()) {
    if (const slp::Setlist* s = store_.find_setlist(loaded_)) return step_in(forward, *s);
    unload_setlist();  // the loaded setlist no longer exists
  }
  return step_all(forward);
}

void SetlistController::fill_status_into(ws::v1::SetlistStatus* s) const {
  sync();
  s->set_available(true);
  const slp::Setlist* cur = loaded_.empty() ? nullptr : store_.find_setlist(loaded_);
  if (cur) {
    s->set_loaded_setlist_id(cur->id());
    s->set_loaded_setlist_name(cur->name());
    s->set_loaded_setlist_size(static_cast<std::uint32_t>(cur->live_set_ids_size()));
  }
  s->set_last_setlist_position(cur ? position_ : -1);
  s->set_current_live_set_id(port_.sounding_id());
  s->set_end_of_setlist(end_);
  s->set_start_of_setlist(start_);
  s->set_setlist_count(static_cast<std::uint32_t>(store_.library().setlists_size()));
  s->set_status_seq(seq_);
  s->set_library_revision(store_.library_revision());
  s->set_not_durable_commits(static_cast<std::uint32_t>(not_durable_));
}

void SetlistController::fill_status(ws::v1::State* out) const { fill_status_into(out->mutable_setlists()); }

bool SetlistController::run(const ClientMessage& msg, ws::v1::ServerMessage& reply, HandlerFailure& err, bool& bump_revision, bool& push_state) {
  bump_revision = false;
  push_state = false;
  sync();
  const std::uint64_t seq_before = seq_;
  bool ok = false;

  bool not_durable = false;  // this command was committed with a failed directory sync
  auto applied_ok = [&](sl::Status s, const std::string& id = {}, const char* limit_name = "") {
    if (!sl::applied(s)) return fail_status(err, s, id, limit_name);
    if (s == sl::Status::kCommittedNotDurable) {
      ++not_durable_;
      not_durable = true;
    }
    bump_revision = true;
    ++seq_;
    return true;
  };

  switch (msg.body_case()) {
    case B::kCreateSetlist: {
      std::string id;
      ok = applied_ok(store_.create_setlist(msg.create_setlist().name(), id), {}, "setlists");
      if (ok) {
        reply.mutable_created_reply()->set_id(id);
        reply.mutable_created_reply()->set_name(msg.create_setlist().name());
      }
      break;
    }
    case B::kRenameSetlist:
      ok = applied_ok(store_.rename_setlist(msg.rename_setlist().id(), msg.rename_setlist().name()), msg.rename_setlist().id());
      break;
    case B::kReorderSetlists: {
      std::vector<std::string> ids(msg.reorder_setlists().ids().begin(), msg.reorder_setlists().ids().end());
      ok = applied_ok(store_.reorder_setlists(ids));
      break;
    }
    case B::kDeleteSetlist: {
      const std::string id = msg.delete_setlist().id();
      ok = applied_ok(store_.delete_setlist(id), id);
      if (ok && id == loaded_) {  // the loaded setlist is gone; the sound is not touched
        loaded_.clear();
        position_ = -1;
        anchor_.clear();
        end_ = start_ = false;
      }
      break;
    }
    case B::kAddLiveSetToSetlist: {
      const auto& m = msg.add_live_set_to_setlist();
      if (!store_.find_setlist(m.setlist_id())) {
        ok = fail_status(err, sl::Status::kNoSuchSetlist, m.setlist_id());
        break;
      }
      if (!known_live_set(m.live_set_id(), nullptr)) {
        ok = fail_status(err, sl::Status::kNoSuchLiveSet, m.live_set_id());
        break;
      }
      std::optional<std::size_t> index;
      if (m.has_index()) index = m.index();
      ok = applied_ok(store_.add_live_set(m.setlist_id(), m.live_set_id(), index), m.setlist_id(), "live_sets_per_setlist");
      if (ok && m.setlist_id() == loaded_) reanchor();
      break;
    }
    case B::kRemoveLiveSetFromSetlist: {
      const auto& m = msg.remove_live_set_from_setlist();
      ok = applied_ok(store_.remove_live_set(m.setlist_id(), m.live_set_id()), m.setlist_id());
      if (ok && m.setlist_id() == loaded_) reanchor();
      break;
    }
    case B::kReorderLiveSetsInSetlist: {
      const auto& m = msg.reorder_live_sets_in_setlist();
      std::vector<std::string> ids(m.live_set_ids().begin(), m.live_set_ids().end());
      ok = applied_ok(store_.reorder_live_sets(m.setlist_id(), ids), m.setlist_id());
      if (ok && m.setlist_id() == loaded_) reanchor();
      break;
    }
    case B::kCreateLiveSet: {
      std::string id;
      ok = applied_ok(store_.create_live_set(live_, msg.create_live_set().name(), id));
      if (ok) {
        port_.add_to_catalog(CatalogEntry{id, msg.create_live_set().name(), live_.path_for(id)});
        reply.mutable_created_reply()->set_id(id);
        reply.mutable_created_reply()->set_name(msg.create_live_set().name());
      }
      break;
    }
    case B::kDeleteLiveSet: {
      const std::string id = msg.delete_live_set().id();
      if (!liveset::is_uuid_v4(id)) {
        ok = fail_status(err, sl::Status::kNoSuchLiveSet, id);
        break;
      }
      if (id == port_.effective_id() || id == port_.sounding_id()) {
        ok = fail_status(err, sl::Status::kLiveSetInUse, id);
        break;
      }
      const CatalogEntry* listed = find_catalog(id);
      const std::string file = listed && listed->path != live_.path_for(id) ? listed->path : std::string();  // not <id>.liveset: delete by the catalog's path
      std::size_t from = 0;
      const sl::Status s = store_.delete_live_set(live_, id, &from, file);
      if (sl::applied(s)) {
        ok = applied_ok(s, id);
        port_.remove_from_catalog(id);
        reanchor();
      } else if (s == sl::Status::kNotFound && (from > 0 || listed)) {
        // The file was already gone: the command still cleans the references (a "missing" entry) and the catalog.
        ok = applied_ok(sl::Status::kOk);
        port_.remove_from_catalog(id);
        reanchor();
      } else if (from > 0) {
        // Partial: the setlists no longer list it, but the file could not be removed. The Live Set stays in the catalog (its file exists); the
        // change is published and reported as a storage failure with the partial result.
        fail_status(err, s == sl::Status::kBadId ? sl::Status::kIo : s, id);
        if (s == sl::Status::kBadId) err.details["reason"] = "path_outside_live_set_dir";
        err.details["partial"] = "references_removed_file_kept";
        err.details["removed_from_setlists"] = std::to_string(from);
        bump_revision = true;
        ++seq_;
        reanchor();
        ok = false;
      } else if (s == sl::Status::kBadId) {  // the catalog's path is not a .liveset file of the Live Set directory: refused, nothing changed
        ok = fail(err, ws::v1::ERROR_CODE_STORAGE_FAILED, "the Live Set file is outside the Live Set directory; nothing changed",
                  {{"id", id}, {"reason", "path_outside_live_set_dir"}});
      } else {
        ok = fail_status(err, s == sl::Status::kNotFound ? sl::Status::kNoSuchLiveSet : s, id);
      }
      break;
    }
    case B::kCreateTag: {
      std::string id;
      ok = applied_ok(store_.create_tag(msg.create_tag().name(), id), {}, "tags");
      if (ok) {
        reply.mutable_created_reply()->set_id(id);
        reply.mutable_created_reply()->set_name(msg.create_tag().name());
      }
      break;
    }
    case B::kRenameTag:
      ok = applied_ok(store_.rename_tag(msg.rename_tag().id(), msg.rename_tag().name()), msg.rename_tag().id());
      break;
    case B::kDeleteTag:
      ok = applied_ok(store_.delete_tag(msg.delete_tag().id()), msg.delete_tag().id());
      break;
    case B::kSetSetlistTags: {
      const auto& m = msg.set_setlist_tags();
      std::vector<std::string> ids(m.tag_ids().begin(), m.tag_ids().end());
      const sl::Status s = store_.set_setlist_tags(m.setlist_id(), ids);
      ok = applied_ok(s, s == sl::Status::kNoSuchTag ? std::string() : m.setlist_id(), "tags_per_setlist");
      break;
    }
    case B::kLoadSetlist: {
      const sl::Status s = load_setlist(msg.load_setlist().id());
      if (s != sl::Status::kOk) {
        ok = fail_status(err, s, msg.load_setlist().id());
        break;
      }
      const slp::Setlist* x = store_.find_setlist(loaded_);
      ws::v1::LoadSetlistReply* r = reply.mutable_load_setlist_reply();
      r->set_setlist_id(loaded_);
      r->set_position(position_);
      r->set_live_set_count(x ? static_cast<std::uint32_t>(x->live_set_ids_size()) : 0);
      bump_revision = true;
      ok = true;
      break;
    }
    case B::kUnloadSetlist:
      unload_setlist();
      bump_revision = true;
      ok = true;
      break;
    case B::kNextLiveSet:
    case B::kPreviousLiveSet: {
      const StepOutcome o = step(msg.body_case() == B::kNextLiveSet);
      std::map<std::string, std::string> d;
      d["scope"] = o.in_setlist ? "setlist" : "all_live_sets";
      d["loaded_setlist_id"] = loaded_;
      switch (o.result) {
        case StepResult::kMoved: {
          ws::v1::StepReply* r = reply.mutable_step_reply();
          r->set_live_set_id(o.live_set_id);
          r->set_live_set_name(o.live_set_name);
          r->set_position(o.position);
          r->set_in_setlist(o.in_setlist);
          switch (o.jump.result) {
            case SwitchOutcome::Result::kAccepted: r->set_jump_result(ws::v1::JUMP_RESULT_ACCEPTED); break;
            case SwitchOutcome::Result::kAlreadyLoaded: r->set_jump_result(ws::v1::JUMP_RESULT_ALREADY_LOADED); break;
            case SwitchOutcome::Result::kAlreadyLoading: r->set_jump_result(ws::v1::JUMP_RESULT_ALREADY_LOADING); break;
            case SwitchOutcome::Result::kUnknownLiveSet: break;  // never kMoved (step() turns it into kLoadFailed)
          }
          r->set_superseded_previous(o.jump.superseded_previous);
          bump_revision = true;
          ok = true;
          break;
        }
        case StepResult::kEndOfList:
          ok = fail(err, ws::v1::ERROR_CODE_END_OF_SETLIST, "end of setlist", d);
          break;
        case StepResult::kStartOfList:
          ok = fail(err, ws::v1::ERROR_CODE_START_OF_SETLIST, "start of setlist", d);
          break;
        case StepResult::kEmptyList:
          ok = fail(err, ws::v1::ERROR_CODE_EMPTY_SETLIST, "there is no Live Set to step to", d);
          break;
        case StepResult::kLoadFailed:
          d["id"] = o.live_set_id;
          d["reason"] = o.reason;
          ok = fail(err, ws::v1::ERROR_CODE_LIVE_SET_LOAD_FAILED, "the next Live Set cannot be loaded; the position did not move", d);
          break;
      }
      break;
    }
    case B::kGetSetlists: {
      const std::string& detail = msg.get_setlists().setlist_id();
      const slp::Setlist* dsl = nullptr;
      if (!detail.empty()) {
        dsl = store_.find_setlist(detail);
        if (!dsl) {
          ok = fail_status(err, sl::Status::kNoSuchSetlist, detail);
          break;
        }
      }
      // "Missing" means no usable file: a catalog entry whose file is gone, or an id that is neither listed nor a valid Live Set file on disk.
      std::unordered_map<std::string, bool> exists;
      auto present = [&](const std::string& id) {
        auto it = exists.find(id);
        if (it != exists.end()) return it->second;
        bool v;
        if (const CatalogEntry* e = find_catalog(id)) {
          struct stat st{};
          v = e->path.empty() || ::stat(e->path.c_str(), &st) == 0;
        } else {
          v = known_live_set(id, nullptr);
        }
        exists.emplace(id, v);
        return v;
      };
      ws::v1::SetlistsReply* r = reply.mutable_setlists_reply();
      for (const slp::Setlist& s : store_.library().setlists()) {
        ws::v1::SetlistSummary* x = r->add_setlists();
        x->set_id(s.id());
        x->set_name(s.name());
        for (const std::string& t : s.tag_ids()) x->add_tag_ids(t);
        x->set_live_set_count(static_cast<std::uint32_t>(s.live_set_ids_size()));
        std::uint32_t missing = 0;
        for (const std::string& id : s.live_set_ids())
          if (!present(id)) ++missing;
        x->set_missing_count(missing);
        x->set_loaded(s.id() == loaded_);
      }
      for (const slp::SetlistTag& t : store_.library().tags()) {
        ws::v1::SetlistTagInfo* x = r->add_tags();
        x->set_id(t.id());
        x->set_name(t.name());
      }
      fill_status_into(r->mutable_status());
      if (dsl) {
        r->set_detail_setlist_id(dsl->id());
        for (const std::string& id : dsl->live_set_ids()) {
          ws::v1::SetlistEntry* x = r->add_entries();
          x->set_live_set_id(id);
          x->set_missing(!present(id));  // (a valid file found on disk is adopted into the catalog by present())
          const CatalogEntry* e = find_catalog(id);
          if (e) x->set_name(e->name);
        }
      }
      ok = true;
      break;
    }
    default:
      ok = fail(err, ws::v1::ERROR_CODE_INVALID_COMMAND, "not a setlist command");
      break;
  }
  if (ok && not_durable) {
    if (reply.has_created_reply()) reply.mutable_created_reply()->set_not_durable(true);
    else if (reply.body_case() == ws::v1::ServerMessage::BODY_NOT_SET) reply.mutable_ack()->set_not_durable(true);
  }
  // A visible change that does not move the revision (the end / start flags latching on a refused step) still reaches subscribers.
  push_state = seq_ != seq_before;
  return ok;
}

}  // namespace velora::host
