// Setlist persistence and commands, Story 4.6. Control thread only; see velora/host/setlist_store.hpp.
#include "velora/host/setlist_store.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <set>
#include <unordered_set>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include "velora/core/params_gen.hpp"

namespace velora::host::setlist {

namespace {

namespace params = ::velora::core::params;

bool ends_with(const std::string& s, const char* suffix) {
  const std::size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

template <class M>
Status serialize_msg(const M& m, std::string& out) {
  out.clear();
  {
    google::protobuf::io::StringOutputStream sos(&out);
    google::protobuf::io::CodedOutputStream cos(&sos);
    cos.SetSerializationDeterministic(true);
    if (!m.SerializeToCodedStream(&cos)) return Status::kIo;
  }
  if (out.size() > kMaxFileBytes) {
    out.clear();
    return Status::kTooLarge;
  }
  return Status::kOk;
}

Status from_live(liveset::Status s) {
  switch (s) {
    case liveset::Status::kOk: return Status::kOk;
    case liveset::Status::kTooLarge: return Status::kTooLarge;
    case liveset::Status::kNotControlThread: return Status::kNotControlThread;
    case liveset::Status::kFaultInjected: return Status::kFaultInjected;
    case liveset::Status::kCommittedNotDurable: return Status::kCommittedNotDurable;
    case liveset::Status::kNotFound: return Status::kNotFound;
    case liveset::Status::kBadId: return Status::kBadId;
    case liveset::Status::kNoSpace: return Status::kNoSpace;
    default: return Status::kIo;
  }
}

}  // namespace

const char* to_string(Status s) {
  switch (s) {
    case Status::kOk: return "ok";
    case Status::kIo: return "io";
    case Status::kTooLarge: return "too_large";
    case Status::kParseError: return "parse_error";
    case Status::kUnsupportedVersion: return "unsupported_version";
    case Status::kInvalid: return "invalid";
    case Status::kNotControlThread: return "not_control_thread";
    case Status::kWriterBusy: return "writer_busy";
    case Status::kFaultInjected: return "fault_injected";
    case Status::kCommittedNotDurable: return "committed_not_durable";
    case Status::kNotFound: return "not_found";
    case Status::kNoSuchSetlist: return "no_such_setlist";
    case Status::kNoSuchTag: return "no_such_tag";
    case Status::kNoSuchLiveSet: return "no_such_live_set";
    case Status::kDuplicateLiveSet: return "duplicate_live_set";
    case Status::kNotInSetlist: return "not_in_setlist";
    case Status::kInvalidName: return "invalid_name";
    case Status::kInvalidOrder: return "invalid_order";
    case Status::kLimitReached: return "limit_reached";
    case Status::kBadId: return "bad_id";
    case Status::kRandomFailed: return "random_failed";
    case Status::kLiveSetInUse: return "live_set_in_use";
    case Status::kNoSpace: return "no_space";
    case Status::kCount: break;
  }
  return "?";
}

// ---- validation ----------------------------------------------------------------------------------------------------
ValidationReport validate(const slp::SetlistLibrary& lib) {
  ValidationReport r;
  auto err = [&](IssueCode c, int setlist = -1, int index = -1) { r.errors.push_back({c, setlist, index}); };
  if (lib.tags_size() > static_cast<int>(kMaxTags)) err(IssueCode::kTooManyTags);
  if (lib.setlists_size() > static_cast<int>(kMaxSetlists)) err(IssueCode::kTooManySetlists);
  std::unordered_set<std::string> tag_ids;
  for (int t = 0; t < lib.tags_size(); ++t) {
    const slp::SetlistTag& tag = lib.tags(t);
    if (!liveset::is_uuid_v4(tag.id())) err(IssueCode::kBadId, t);
    else if (!tag_ids.insert(tag.id()).second) err(IssueCode::kDuplicateId, t);
    if (!liveset::valid_display_name(tag.name())) err(IssueCode::kBadName, t);
  }
  std::unordered_set<std::string> ids;
  for (int i = 0; i < lib.setlists_size(); ++i) {
    const slp::Setlist& sl = lib.setlists(i);
    if (!liveset::is_uuid_v4(sl.id())) err(IssueCode::kBadId, i);
    else if (!ids.insert(sl.id()).second) err(IssueCode::kDuplicateId, i);
    if (!liveset::valid_display_name(sl.name())) err(IssueCode::kBadName, i);
    if (sl.live_set_ids_size() > static_cast<int>(kMaxLiveSetsPerSetlist)) err(IssueCode::kTooManyLiveSets, i);
    std::unordered_set<std::string> seen;
    for (int k = 0; k < sl.live_set_ids_size(); ++k) {
      if (!liveset::is_uuid_v4(sl.live_set_ids(k))) err(IssueCode::kBadLiveSetId, i, k);
      else if (!seen.insert(sl.live_set_ids(k)).second) err(IssueCode::kDuplicateLiveSet, i, k);
    }
    if (sl.tag_ids_size() > static_cast<int>(kMaxTagsPerSetlist)) err(IssueCode::kTooManyTagsOnSetlist, i);
    std::unordered_set<std::string> seen_tags;
    for (int k = 0; k < sl.tag_ids_size(); ++k) {
      if (tag_ids.find(sl.tag_ids(k)) == tag_ids.end()) err(IssueCode::kUnknownTag, i, k);
      else if (!seen_tags.insert(sl.tag_ids(k)).second) err(IssueCode::kDuplicateTag, i, k);
    }
  }
  return r;
}

ValidationReport validate(const slp::AssignDefault& a) {
  ValidationReport r;
  if (a.assign_size() != static_cast<int>(kAssignSlots)) {
    r.errors.push_back({IssueCode::kBadAssignCount, -1, a.assign_size()});
    return r;
  }
  for (int k = 0; k < a.assign_size(); ++k) {
    const std::uint32_t id = a.assign(k);
    if (id == 0) continue;
    const params::ParamDef* d = id <= 0xFFFFu ? params::find(static_cast<std::uint16_t>(id)) : nullptr;
    if (!d || d->cls != params::Class::kPerformance) r.errors.push_back({IssueCode::kBadAssignParam, -1, k});
  }
  return r;
}

// ---- reading and writing -------------------------------------------------------------------------------------------
Status parse_library(std::string_view bytes, std::uint32_t current_version, LoadResult& out) {
  out = LoadResult{};
  if (bytes.size() > kMaxFileBytes) return Status::kTooLarge;
  if (!out.library.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
    out.library.Clear();
    return Status::kParseError;
  }
  out.file_version = out.library.format_version();
  if (out.file_version < min_readable_version(current_version) || out.file_version > current_version) return Status::kUnsupportedVersion;
  if (out.file_version != current_version) {
    out.library.set_format_version(current_version);  // migration stub: no field changed between N-1 and N
    out.migrated = true;
  }
  out.report = validate(out.library);
  return out.report.ok() ? Status::kOk : Status::kInvalid;
}

Status parse_assign(std::string_view bytes, std::uint32_t current_version, AssignResult& out) {
  out = AssignResult{};
  if (bytes.size() > kMaxFileBytes) return Status::kTooLarge;
  if (!out.assign.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
    out.assign.Clear();
    return Status::kParseError;
  }
  out.file_version = out.assign.format_version();
  if (out.file_version < min_readable_version(current_version) || out.file_version > current_version) return Status::kUnsupportedVersion;
  if (out.file_version != current_version) out.assign.set_format_version(current_version);
  out.report = validate(out.assign);
  return out.report.ok() ? Status::kOk : Status::kInvalid;
}

Status serialize(const slp::SetlistLibrary& lib, std::string& out) { return serialize_msg(lib, out); }
Status serialize(const slp::AssignDefault& a, std::string& out) { return serialize_msg(a, out); }

std::array<std::uint32_t, kAssignSlots> factory_default_assign() { return {401, 400, 113, 112, 116, 117, 200, 115}; }

// ---- the store -----------------------------------------------------------------------------------------------------
Status SetlistStore::open(StoreOptions opt, std::unique_ptr<SetlistStore>& out) {
  out.reset();
  struct stat st{};
  if (::stat(opt.dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return Status::kNotFound;
  const int lock = ::open((opt.dir + "/.writer.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
  if (lock < 0) return Status::kIo;
  if (::flock(lock, LOCK_EX | LOCK_NB) != 0) {
    const Status s = errno == EWOULDBLOCK ? Status::kWriterBusy : Status::kIo;
    ::close(lock);
    return s;
  }
  std::unique_ptr<SetlistStore> s(new SetlistStore(std::move(opt)));
  s->lock_fd_ = lock;  // owned from here: the destructor closes it
  s->owner_ = std::this_thread::get_id();

  std::string bytes;
  {
    const liveset::Status lr = liveset::read_regular_file(s->opt_.dir + "/" + kLibraryFile, kMaxFileBytes, bytes);
    if (lr == liveset::Status::kOk) {
      LoadResult res;
      const Status ps = parse_library(bytes, s->opt_.current_version, res);
      if (ps != Status::kOk) return ps;
      s->lib_ = std::move(res.library);
    } else if (lr == liveset::Status::kNotFound) {
      s->lib_.set_format_version(s->opt_.current_version);
    } else {
      return from_live(lr);
    }
  }
  {
    const liveset::Status ar = liveset::read_regular_file(s->opt_.dir + "/" + kAssignFile, kMaxFileBytes, bytes);
    if (ar == liveset::Status::kOk) {
      AssignResult res;
      const Status ps = parse_assign(bytes, s->opt_.current_version, res);
      if (ps != Status::kOk) return ps;
      s->assign_msg_ = std::move(res.assign);
    } else if (ar == liveset::Status::kNotFound) {
      s->assign_msg_.set_format_version(s->opt_.current_version);
      for (std::uint32_t id : factory_default_assign()) s->assign_msg_.add_assign(id);
    } else {
      return from_live(ar);
    }
    for (std::size_t k = 0; k < kAssignSlots; ++k) s->assign_[k] = s->assign_msg_.assign(static_cast<int>(k));
  }
  out = std::move(s);
  return Status::kOk;
}

SetlistStore::~SetlistStore() {
  if (lock_fd_ >= 0) ::close(lock_fd_);
}

liveset::RandomSource& SetlistStore::rng() { return opt_.rng ? *opt_.rng : static_cast<liveset::RandomSource&>(os_rng_); }

std::string SetlistStore::mint_id(const std::function<bool(const std::string&)>& taken) {
  for (int attempt = 0; attempt < liveset::kMaxAdoptAttempts; ++attempt) {
    const std::string id = liveset::mint_uuid_v4(rng());
    if (id.empty()) return std::string();  // fail closed
    if (!taken(id)) return id;
  }
  return std::string();
}

Status SetlistStore::reconcile(std::size_t& removed) {
  removed = 0;
  if (!owner()) return Status::kNotControlThread;
  DIR* d = ::opendir(opt_.dir.c_str());
  if (!d) return Status::kIo;
  std::vector<std::string> doomed;
  while (const dirent* e = ::readdir(d)) {
    const std::string name = e->d_name;
    if (ends_with(name, kLibraryTmpFile) || ends_with(name, kAssignTmpFile)) doomed.push_back(name);
  }
  ::closedir(d);
  Status st = Status::kOk;
  for (const std::string& n : doomed) {
    if (::unlink((opt_.dir + "/" + n).c_str()) == 0) ++removed;
    else st = Status::kIo;
  }
  return st;
}

const slp::Setlist* SetlistStore::find_setlist(const std::string& id) const {
  const int i = index_of_setlist(id);
  return i < 0 ? nullptr : &lib_.setlists(i);
}

int SetlistStore::index_of_setlist(const std::string& id) const {
  for (int i = 0; i < lib_.setlists_size(); ++i)
    if (lib_.setlists(i).id() == id) return i;
  return -1;
}

const slp::SetlistTag* SetlistStore::find_tag(const std::string& id) const {
  for (const slp::SetlistTag& t : lib_.tags())
    if (t.id() == id) return &t;
  return nullptr;
}

Status SetlistStore::commit(slp::SetlistLibrary&& work) {
  if (!owner()) return Status::kNotControlThread;
  work.set_format_version(opt_.current_version);
  if (!validate(work).ok()) return Status::kInvalid;  // a command bug, never a user error: nothing is written
  std::string bytes;
  if (const Status s = serialize(work, bytes); s != Status::kOk) return s;
  mutations_.fetch_add(1, std::memory_order_relaxed);
  const Status ws = from_live(liveset::atomic_replace_file(opt_.dir, kLibraryFile, kLibraryTmpFile, bytes, opt_.on_stage, opt_.inject_dir_sync_failure, opt_.inject_write_errno));
  if (applied(ws)) {
    lib_ = std::move(work);
    ++revision_;
  }
  return ws;
}

Status SetlistStore::create_setlist(const std::string& name, std::string& id) {
  id.clear();
  if (!owner()) return Status::kNotControlThread;
  if (!liveset::valid_display_name(name)) return Status::kInvalidName;
  if (lib_.setlists_size() >= static_cast<int>(kMaxSetlists)) return Status::kLimitReached;
  const std::string nid = mint_id([&](const std::string& c) { return find_setlist(c) != nullptr; });
  if (nid.empty()) return Status::kRandomFailed;
  slp::SetlistLibrary work = lib_;
  slp::Setlist* sl = work.add_setlists();
  sl->set_id(nid);
  sl->set_name(name);
  sl->set_format_version(opt_.current_version);
  const Status s = commit(std::move(work));
  if (applied(s)) id = nid;
  return s;
}

Status SetlistStore::rename_setlist(const std::string& id, const std::string& name) {
  if (!owner()) return Status::kNotControlThread;
  const int i = index_of_setlist(id);
  if (i < 0) return Status::kNoSuchSetlist;
  if (!liveset::valid_display_name(name)) return Status::kInvalidName;
  slp::SetlistLibrary work = lib_;
  work.mutable_setlists(i)->set_name(name);
  return commit(std::move(work));
}

Status SetlistStore::reorder_setlists(const std::vector<std::string>& ids) {
  if (!owner()) return Status::kNotControlThread;
  if (ids.size() != static_cast<std::size_t>(lib_.setlists_size())) return Status::kInvalidOrder;
  std::set<std::string> seen;
  for (const std::string& id : ids)
    if (!seen.insert(id).second || index_of_setlist(id) < 0) return Status::kInvalidOrder;
  slp::SetlistLibrary work = lib_;  // keeps the library's own unknown fields
  work.clear_setlists();
  for (const std::string& id : ids) *work.add_setlists() = lib_.setlists(index_of_setlist(id));  // whole messages: unknown fields travel along
  return commit(std::move(work));
}

Status SetlistStore::delete_setlist(const std::string& id) {
  if (!owner()) return Status::kNotControlThread;
  const int i = index_of_setlist(id);
  if (i < 0) return Status::kNoSuchSetlist;
  slp::SetlistLibrary work = lib_;
  work.mutable_setlists()->DeleteSubrange(i, 1);
  return commit(std::move(work));
}

Status SetlistStore::add_live_set(const std::string& setlist_id, const std::string& live_set_id, std::optional<std::size_t> index) {
  if (!owner()) return Status::kNotControlThread;
  const int i = index_of_setlist(setlist_id);
  if (i < 0) return Status::kNoSuchSetlist;
  if (!liveset::is_uuid_v4(live_set_id)) return Status::kBadId;
  const slp::Setlist& cur = lib_.setlists(i);
  for (const std::string& e : cur.live_set_ids())
    if (e == live_set_id) return Status::kDuplicateLiveSet;
  if (cur.live_set_ids_size() >= static_cast<int>(kMaxLiveSetsPerSetlist)) return Status::kLimitReached;
  const std::size_t n = static_cast<std::size_t>(cur.live_set_ids_size());
  if (index && *index > n) return Status::kInvalidOrder;
  slp::SetlistLibrary work = lib_;
  auto* ids = work.mutable_setlists(i)->mutable_live_set_ids();
  ids->Add(std::string(live_set_id));
  if (index) {
    for (std::size_t k = n; k > *index; --k) ids->SwapElements(static_cast<int>(k), static_cast<int>(k - 1));
  }
  return commit(std::move(work));
}

Status SetlistStore::remove_live_set(const std::string& setlist_id, const std::string& live_set_id) {
  if (!owner()) return Status::kNotControlThread;
  const int i = index_of_setlist(setlist_id);
  if (i < 0) return Status::kNoSuchSetlist;
  const slp::Setlist& cur = lib_.setlists(i);
  int at = -1;
  for (int k = 0; k < cur.live_set_ids_size(); ++k)
    if (cur.live_set_ids(k) == live_set_id) at = k;
  if (at < 0) return Status::kNotInSetlist;
  slp::SetlistLibrary work = lib_;
  work.mutable_setlists(i)->mutable_live_set_ids()->DeleteSubrange(at, 1);
  return commit(std::move(work));
}

Status SetlistStore::reorder_live_sets(const std::string& setlist_id, const std::vector<std::string>& ids) {
  if (!owner()) return Status::kNotControlThread;
  const int i = index_of_setlist(setlist_id);
  if (i < 0) return Status::kNoSuchSetlist;
  const slp::Setlist& cur = lib_.setlists(i);
  if (ids.size() != static_cast<std::size_t>(cur.live_set_ids_size())) return Status::kInvalidOrder;
  std::set<std::string> have(cur.live_set_ids().begin(), cur.live_set_ids().end()), seen;
  for (const std::string& id : ids)
    if (!seen.insert(id).second || have.find(id) == have.end()) return Status::kInvalidOrder;
  slp::SetlistLibrary work = lib_;
  auto* out = work.mutable_setlists(i)->mutable_live_set_ids();
  out->Clear();
  for (const std::string& id : ids) out->Add(std::string(id));
  return commit(std::move(work));
}

Status SetlistStore::create_tag(const std::string& name, std::string& id) {
  id.clear();
  if (!owner()) return Status::kNotControlThread;
  if (!liveset::valid_display_name(name)) return Status::kInvalidName;
  if (lib_.tags_size() >= static_cast<int>(kMaxTags)) return Status::kLimitReached;
  const std::string nid = mint_id([&](const std::string& c) { return find_tag(c) != nullptr; });
  if (nid.empty()) return Status::kRandomFailed;
  slp::SetlistLibrary work = lib_;
  slp::SetlistTag* t = work.add_tags();
  t->set_id(nid);
  t->set_name(name);
  const Status s = commit(std::move(work));
  if (applied(s)) id = nid;
  return s;
}

Status SetlistStore::rename_tag(const std::string& id, const std::string& name) {
  if (!owner()) return Status::kNotControlThread;
  int at = -1;
  for (int t = 0; t < lib_.tags_size(); ++t)
    if (lib_.tags(t).id() == id) at = t;
  if (at < 0) return Status::kNoSuchTag;
  if (!liveset::valid_display_name(name)) return Status::kInvalidName;
  slp::SetlistLibrary work = lib_;
  work.mutable_tags(at)->set_name(name);
  return commit(std::move(work));
}

Status SetlistStore::delete_tag(const std::string& id) {
  if (!owner()) return Status::kNotControlThread;
  int at = -1;
  for (int t = 0; t < lib_.tags_size(); ++t)
    if (lib_.tags(t).id() == id) at = t;
  if (at < 0) return Status::kNoSuchTag;
  slp::SetlistLibrary work = lib_;
  work.mutable_tags()->DeleteSubrange(at, 1);
  for (slp::Setlist& sl : *work.mutable_setlists()) {
    auto* ids = sl.mutable_tag_ids();
    for (int k = ids->size() - 1; k >= 0; --k)
      if (ids->Get(k) == id) ids->DeleteSubrange(k, 1);
  }
  return commit(std::move(work));
}

Status SetlistStore::set_setlist_tags(const std::string& setlist_id, const std::vector<std::string>& tag_ids) {
  if (!owner()) return Status::kNotControlThread;
  const int i = index_of_setlist(setlist_id);
  if (i < 0) return Status::kNoSuchSetlist;
  if (tag_ids.size() > kMaxTagsPerSetlist) return Status::kLimitReached;
  std::set<std::string> seen;
  for (const std::string& t : tag_ids) {
    if (!find_tag(t)) return Status::kNoSuchTag;
    if (!seen.insert(t).second) return Status::kInvalidOrder;  // a tag twice
  }
  slp::SetlistLibrary work = lib_;
  auto* ids = work.mutable_setlists(i)->mutable_tag_ids();
  ids->Clear();
  for (const std::string& t : tag_ids) ids->Add(std::string(t));
  return commit(std::move(work));
}

Status SetlistStore::remove_live_set_everywhere(const std::string& live_set_id, std::size_t& removed_from) {
  removed_from = 0;
  if (!owner()) return Status::kNotControlThread;
  slp::SetlistLibrary work = lib_;
  for (slp::Setlist& sl : *work.mutable_setlists()) {
    auto* ids = sl.mutable_live_set_ids();
    bool hit = false;
    for (int k = ids->size() - 1; k >= 0; --k)
      if (ids->Get(k) == live_set_id) {
        ids->DeleteSubrange(k, 1);
        hit = true;
      }
    if (hit) ++removed_from;
  }
  if (removed_from == 0) return Status::kOk;
  const Status s = commit(std::move(work));
  if (!applied(s)) removed_from = 0;
  return s;
}

Status SetlistStore::set_default_assign(const std::array<std::uint32_t, kAssignSlots>& assign) {
  if (!owner()) return Status::kNotControlThread;
  slp::AssignDefault work = assign_msg_;
  work.set_format_version(opt_.current_version);
  work.clear_assign();
  for (std::uint32_t v : assign) work.add_assign(v);
  if (!validate(work).ok()) return Status::kInvalid;
  std::string bytes;
  if (const Status s = serialize(work, bytes); s != Status::kOk) return s;
  mutations_.fetch_add(1, std::memory_order_relaxed);
  const Status ws = from_live(liveset::atomic_replace_file(opt_.dir, kAssignFile, kAssignTmpFile, bytes, opt_.on_stage, opt_.inject_dir_sync_failure, opt_.inject_write_errno));
  if (applied(ws)) {
    assign_msg_ = std::move(work);
    assign_ = assign;
    ++revision_;
  }
  return ws;
}

Status SetlistStore::create_live_set(liveset::LiveSetStore& live, const std::string& name, std::string& id) {
  id.clear();
  if (!owner()) return Status::kNotControlThread;
  if (!liveset::valid_display_name(name)) return Status::kInvalidName;
  const std::string nid = mint_id([&](const std::string& c) {
    struct stat st{};
    return ::lstat(live.path_for(c).c_str(), &st) == 0;  // any entry under that name, a dangling symlink included, is taken
  });
  if (nid.empty()) return Status::kRandomFailed;
  liveset::lsp::LiveSet ls = liveset::make_default_live_set(nid, name, live.options().part_count);
  liveset::lsp::Scene* scene = ls.add_scenes();  // one scene, all-base: every field absent = the Live Set's own value
  scene->set_name("Scene 1");
  ls.clear_assign();
  for (std::uint32_t v : assign_) ls.add_assign(v);
  const Status s = from_live(live.write(ls));
  if (applied(s)) id = nid;
  return s;
}

Status SetlistStore::delete_live_set(liveset::LiveSetStore& live, const std::string& id, std::size_t* removed_from, const std::string& file_path) {
  if (removed_from) *removed_from = 0;
  if (!owner()) return Status::kNotControlThread;
  if (!liveset::is_uuid_v4(id)) return Status::kBadId;
  std::size_t from = 0;
  if (const Status s = remove_live_set_everywhere(id, from); !applied(s)) return s;
  if (removed_from) *removed_from = from;
  return from_live(live.remove(id, file_path));
}

}  // namespace velora::host::setlist
