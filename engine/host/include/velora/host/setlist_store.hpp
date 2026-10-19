#pragma once
// Setlists, setlist tags and the default ASSIGN map: persistence and commands (Story 4.6, FR82, FR84 partial, FR86, AR30).
//
// The persisted forms are the protobuf messages of proto/setlist.proto (package velora.setlist.v1). The directory `<data>/setlists/` holds
//   setlists.library   ONE SetlistLibrary file: every tag and every setlist, the list order being the setlists' positions. One file, so
//                      every command is a single atomic replace: all or nothing, with no cross-file states to repair after a crash.
//   assign_default.pb  the global default ASSIGN map (8 slots) a new Live Set copies; absent = the built-in factory default
//   .writer.lock       exclusive flock: a second store on the directory (same or another process) gets kWriterBusy
// The write discipline is the Live Set store's (atomic_replace_file: temp file, fsync, rename, directory fsync) and so are the
// single-writer rules: the thread that opens the store is THE control thread; commands from any other thread return kNotControlThread
// (handover is explicit: rebind_control_thread()). Readers (the `library()` accessor included) are for the control thread too: the
// published state is what other threads see.
//
// Rules (also in engine/host/README.md): names are 1..64 bytes of valid UTF-8 without control characters (tag and setlist names need not be
// unique); ids are UUIDv4; a Live Set appears at most once in one setlist but may be in several setlists; a setlist may keep ids of Live
// Sets that no longer exist (the published state flags them as missing); every tag id of a setlist exists; at most kMaxSetlists setlists,
// kMaxTags tags, kMaxLiveSetsPerSetlist Live Sets and kMaxTagsPerSetlist tags per setlist; the file is at most kMaxFileBytes. Version policy:
// N and N-1 load, anything else is kUnsupportedVersion; unknown fields are preserved on re-write.
//
// Commands are atomic: each is applied to a copy, validated, written, and only then adopted in memory (kCommittedNotDurable = renamed but
// the directory fsync failed: the new state IS in place and is adopted; the caller may report a warning).
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "live_set_store.hpp"
#include "setlist.pb.h"

namespace velora::host::setlist {

namespace slp = ::velora::setlist::v1;

inline constexpr std::uint32_t kFormatVersion = 1;
inline constexpr std::size_t kMaxFileBytes = 1u << 20;
inline constexpr std::size_t kMaxSetlists = 100;
inline constexpr std::size_t kMaxTags = 64;
inline constexpr std::size_t kMaxLiveSetsPerSetlist = 256;
inline constexpr std::size_t kMaxTagsPerSetlist = 8;
inline constexpr std::size_t kAssignSlots = liveset::kAssignSlots;
inline constexpr const char* kLibraryFile = "setlists.library";
inline constexpr const char* kLibraryTmpFile = "setlists.library.tmp";
inline constexpr const char* kAssignFile = "assign_default.pb";
inline constexpr const char* kAssignTmpFile = "assign_default.pb.tmp";

enum class Status : std::uint8_t {
  kOk = 0,
  kIo,
  kTooLarge,
  kParseError,
  kUnsupportedVersion,
  kInvalid,               // parsed or built, but a validation rule failed (see the report); nothing written
  kNotControlThread,
  kWriterBusy,
  kFaultInjected,
  kCommittedNotDurable,   // the new file is in place, the directory fsync failed
  kNotFound,              // open: the directory does not exist
  // command refusals (nothing changed)
  kNoSuchSetlist,
  kNoSuchTag,
  kNoSuchLiveSet,
  kDuplicateLiveSet,      // the Live Set is already in the setlist
  kNotInSetlist,
  kInvalidName,
  kInvalidOrder,          // not a permutation / index past the end
  kLimitReached,
  kBadId,                 // a malformed id
  kRandomFailed,          // no randomness for a new id (fails closed)
  kLiveSetInUse,          // delete_live_set: the caller refused (reserved for the controller)
  kCount
};
const char* to_string(Status s);
// True when the status means "the command took effect" (kOk or kCommittedNotDurable).
constexpr bool applied(Status s) { return s == Status::kOk || s == Status::kCommittedNotDurable; }

enum class IssueCode : std::uint8_t {
  kBadVersion = 0,
  kBadId,                  // a setlist or tag id that is not a UUIDv4
  kDuplicateId,            // two setlists (or two tags) with one id
  kBadName,
  kBadLiveSetId,           // a Live Set id that is not a UUIDv4
  kDuplicateLiveSet,       // a Live Set twice in one setlist
  kUnknownTag,             // a setlist names a tag that does not exist
  kDuplicateTag,           // a tag twice on one setlist
  kTooManySetlists,
  kTooManyTags,
  kTooManyLiveSets,
  kTooManyTagsOnSetlist,
  kBadAssignCount,         // the default ASSIGN map does not have exactly 8 slots
  kBadAssignParam,         // a slot that is not 0 or a registry performance parameter
  kCount
};
struct Issue {
  IssueCode code = IssueCode::kBadVersion;
  int setlist = -1;   // setlist index (or tag index for tag issues, see code)
  int index = -1;     // entry index within the setlist
  friend bool operator==(const Issue&, const Issue&) = default;
};
struct ValidationReport {
  std::vector<Issue> errors;
  bool ok() const { return errors.empty(); }
};

ValidationReport validate(const slp::SetlistLibrary& lib);
ValidationReport validate(const slp::AssignDefault& a);

struct LoadResult {
  slp::SetlistLibrary library;
  ValidationReport report;
  std::uint32_t file_version = 0;
  bool migrated = false;
};
struct AssignResult {
  slp::AssignDefault assign;
  ValidationReport report;
  std::uint32_t file_version = 0;
};
constexpr std::uint32_t min_readable_version(std::uint32_t current) { return current > 1 ? current - 1 : 1; }

// Parse + version policy (N, N-1; N-1 is migrated in memory by a stub that only moves format_version) + validation.
Status parse_library(std::string_view bytes, std::uint32_t current_version, LoadResult& out);
Status parse_assign(std::string_view bytes, std::uint32_t current_version, AssignResult& out);
// Deterministic serialisation (fields in number order, unknown fields last); kTooLarge above kMaxFileBytes.
Status serialize(const slp::SetlistLibrary& lib, std::string& out);
Status serialize(const slp::AssignDefault& a, std::string& out);

// The built-in default ASSIGN map: K1..K8 -> send_delay_level (401, the DELAY function), send_reverb_level (400), percussion_volume (113),
// percussion_decay (112), key_click (116), leakage (117), rotary_speed (200), vibrato_chorus_type (115): performance parameters of the
// current registry, to be replaced by the knob functions (brightness, attack, release, EQ) as those parameters appear.
std::array<std::uint32_t, kAssignSlots> factory_default_assign();

struct StoreOptions {
  std::string dir;                                  // `<data>/setlists/`, an existing directory on the journaled partition
  std::uint32_t current_version = kFormatVersion;   // N; tests set N=2 to exercise the N-1 path
  liveset::RandomSource* rng = nullptr;             // nullptr = the operating system's (OsRandom); tests inject SeededRandom
  std::function<liveset::HookAction(liveset::Stage)> on_stage;  // fault injection, as the Live Set store
  bool inject_dir_sync_failure = false;             // test only
};

class SetlistStore {
 public:
  // Takes the directory's exclusive writer lock, records the calling thread as THE control thread and loads the library and the default
  // ASSIGN map (absent files = empty library / factory default). A file that exists but cannot be read (parse error, unsupported
  // version, invalid) fails the open with that status and is NEVER overwritten.
  static Status open(StoreOptions opt, std::unique_ptr<SetlistStore>& out);
  ~SetlistStore();
  SetlistStore(const SetlistStore&) = delete;
  SetlistStore& operator=(const SetlistStore&) = delete;

  void rebind_control_thread() { owner_.store(std::this_thread::get_id()); }
  // Removes leftover `*.tmp` files of an interrupted write. Control thread only.
  Status reconcile(std::size_t& removed);

  // ---- state (control thread) ----------------------------------------------------------------------------------------------------
  const slp::SetlistLibrary& library() const { return lib_; }
  std::uint64_t library_revision() const { return revision_; }  // increases with every applied command
  const std::array<std::uint32_t, kAssignSlots>& default_assign() const { return assign_; }
  const slp::Setlist* find_setlist(const std::string& id) const;
  int index_of_setlist(const std::string& id) const;  // -1 when absent
  const slp::SetlistTag* find_tag(const std::string& id) const;

  // ---- commands (control thread; all or nothing; the new id of a create goes to `id`) -------------------------------------------------
  Status create_setlist(const std::string& name, std::string& id);
  Status rename_setlist(const std::string& id, const std::string& name);
  Status reorder_setlists(const std::vector<std::string>& ids);   // a permutation of all current setlist ids
  Status delete_setlist(const std::string& id);                    // never touches a Live Set
  // The caller (SetlistController) checks that the Live Set exists; the store checks duplicates, the limit and `index` (nullopt = append).
  Status add_live_set(const std::string& setlist_id, const std::string& live_set_id, std::optional<std::size_t> index);
  Status remove_live_set(const std::string& setlist_id, const std::string& live_set_id);
  Status reorder_live_sets(const std::string& setlist_id, const std::vector<std::string>& ids);
  Status create_tag(const std::string& name, std::string& id);
  Status rename_tag(const std::string& id, const std::string& name);
  Status delete_tag(const std::string& id);                        // removes it from every setlist
  Status set_setlist_tags(const std::string& setlist_id, const std::vector<std::string>& tag_ids);
  // Removes a Live Set id from every setlist (one write; no write when it is in none: kOk and `removed_from` = 0).
  Status remove_live_set_everywhere(const std::string& live_set_id, std::size_t& removed_from);

  // The global default ASSIGN map: validated (8 slots, 0 or a performance param), written, adopted. Story 4.10 puts a command on it.
  Status set_default_assign(const std::array<std::uint32_t, kAssignSlots>& assign);

  // Creates a Live Set in `live`: id = fresh UUIDv4 (unique among the files of `live`), the tier's number of empty Sample parts, ONE scene
  // (named "Scene 1", all-base), the ASSIGN map copied from the current default. Written by `live` (so its single-writer and atomic rules apply).
  Status create_live_set(liveset::LiveSetStore& live, const std::string& name, std::string& id);
  // Removes the Live Set from every setlist FIRST (one write), then deletes its file. A crash in between leaves a Live Set file that no
  // setlist lists (it is still in All Live Sets). kNotFound (as the Live Set store's) when there is no such file; the setlists are cleaned anyway.
  // `removed_from` (optional) gets the number of setlists that listed it. `file_path` (optional) is the catalog's path when the file is not named
  // `<id>.liveset`. Partial failure: when the file cannot be removed (kIo) the setlists are ALREADY cleaned (`removed_from` > 0 says so) and the Live
  // Set stays in All Live Sets; the caller reports it.
  Status delete_live_set(liveset::LiveSetStore& live, const std::string& id, std::size_t* removed_from = nullptr, const std::string& file_path = {});

  const StoreOptions& options() const { return opt_; }

 private:
  explicit SetlistStore(StoreOptions opt) : opt_(std::move(opt)) {}
  bool owner() const { return std::this_thread::get_id() == owner_.load(); }
  Status commit(slp::SetlistLibrary&& work);
  std::string mint_id(const std::function<bool(const std::string&)>& taken);
  liveset::RandomSource& rng();

  StoreOptions opt_;
  int lock_fd_ = -1;
  std::atomic<std::thread::id> owner_;
  slp::SetlistLibrary lib_;
  std::array<std::uint32_t, kAssignSlots> assign_{};
  slp::AssignDefault assign_msg_;  // the message as read (unknown fields kept) / as last written
  std::uint64_t revision_ = 0;
  liveset::OsRandom os_rng_;
};

}  // namespace velora::host::setlist
