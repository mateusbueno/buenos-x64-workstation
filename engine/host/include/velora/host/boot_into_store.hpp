#pragma once
// Journaled Boot-into pointer (Story 8.8, NFR15, FR64, AD-18).
//
// The unit remembers the Live Set and scene the player was on. The pointer {setlist_id, live_set_id, scene_index, schema_version} lives in memory on
// the control thread, is marked dirty at every change, and is written to `<dir>/boot_into.vbp` only at idle-safe moments (BootIntoTracker) with the
// store-wide atomic discipline (liveset::atomic_replace_file: temp file, fsync, rename, directory fsync). The file is at most kMaxFileBytes (4 KiB).
//
// On-disk form (text, one `key=value` per line, closed set of keys; the last line is a CRC-32 of everything before it):
//   velora-boot-into
//   schema_version=1
//   setlist_id=<id or empty>
//   live_set_id=<id>
//   scene_index=<decimal>
//   crc32=<8 hex digits>
// A missing, oversized, corrupt (bad CRC, bad field, trailing bytes) or unknown-schema file is "no pointer": the caller falls back to the Default
// Live Set target (Story 8.9 resolves the boot target; nothing here reads a setting). Control thread only; never from RT.
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "velora/host/live_set_store.hpp"

namespace velora::host::bootinto {

constexpr std::uint32_t kSchemaVersion = 1;
constexpr std::size_t kMaxFileBytes = 4096;
constexpr std::size_t kMaxIdBytes = 64;
inline constexpr const char* kFileName = "boot_into.vbp";
inline constexpr const char* kTmpFileName = "boot_into.vbp.tmp";

struct Pointer {
  std::string setlist_id;  // empty = no setlist
  std::string live_set_id;
  std::uint32_t scene_index = 0;
  std::uint32_t schema_version = kSchemaVersion;
  friend bool operator==(const Pointer&, const Pointer&) = default;
};

// An id is empty or 1..kMaxIdBytes characters of [A-Za-z0-9_-]. A pointer to write needs a non-empty live_set_id.
bool valid_id(const std::string& id);
bool valid_pointer(const Pointer& p);

std::string serialize(const Pointer& p);
// False on anything but a well-formed current-schema file.
bool parse(const std::string& bytes, Pointer& out);

enum class LoadStatus : std::uint8_t { kOk = 0, kNoPointer };
// Reads `<dir>/boot_into.vbp`. kNoPointer for a missing, unreadable, corrupt or unknown-schema file (`out` is then left default).
LoadStatus load(const std::string& dir, Pointer& out);

struct StoreOptions {
  std::string dir;  // an existing directory on the journaled data partition; the temp file lives in it too
  // Test hook, as the Live Set store's: called after each stage of a write; kAbort stops dead (kFaultInjected), or the hook may _exit().
  std::function<liveset::HookAction(liveset::Stage)> on_stage;
  bool inject_dir_sync_failure = false;
  int inject_write_errno = 0;
};

class BootIntoStore {
 public:
  // Records the calling thread as THE control thread and loads the stored pointer (if any).
  explicit BootIntoStore(StoreOptions opt);
  // Handover to the thread that will serve commands (the control server's thread).
  void rebind_control_thread() noexcept { owner_ = std::this_thread::get_id(); }

  bool has_stored() const noexcept { return has_stored_; }
  const Pointer& stored() const noexcept { return stored_; }

  // Writes `p` atomically unless it equals the stored pointer (then nothing is written, kOk, wrote=false). The stored copy changes only when the
  // write is complete and durable. kCommittedNotDurable / kFaultInjected / kIo / kNoSpace: the caller keeps the pointer dirty and retries.
  liveset::Status write(const Pointer& p, bool* wrote = nullptr);
  std::uint64_t writes() const noexcept { return writes_; }  // files actually written (attempts that reached the disk layer)

  const StoreOptions& options() const { return opt_; }

 private:
  StoreOptions opt_;
  std::thread::id owner_;
  Pointer stored_;
  bool has_stored_ = false;
  std::uint64_t writes_ = 0;
};

// Idle-safe policy. All inputs are injected, so tests drive it with a fake clock.
struct TrackerProbes {
  std::function<std::uint64_t()> now_ns;
  std::function<std::uint32_t()> held_notes;  // physical keys held; unset = 0
  std::function<bool()> pedal_down;           // unset = false
  std::function<bool()> import_verifying;     // unset = false
};

class BootIntoTracker {
 public:
  static constexpr std::uint64_t kIdleNs = 2'000'000'000ull;    // no note held and no pedal down for this long
  static constexpr std::uint64_t kMaxWaitNs = 30'000'000'000ull;  // or this long since the pointer first became dirty (later changes do not restart it)
  static constexpr std::uint64_t kRetryNs = 1'000'000'000ull;     // pause after a failed write

  BootIntoTracker(BootIntoStore& store, TrackerProbes probes);

  // The current position changed: dirty in memory, no file access. Ignored when invalid (no Live Set) or identical to what is already pending.
  void mark_dirty(const Pointer& p);
  // Polled from the control tick with the current position: marks dirty when it differs from the last one seen.
  void observe(const Pointer& p);

  // What the control tick knows about where the player is. Only a settled position (a Live Set sounds, no load in progress, a scene is current) counts.
  struct Position {
    std::string live_set_id;  // LiveSetSwitcher::sounding_id()
    std::int32_t scene = -1;  // SceneController::current_scene()
    std::string setlist_id;   // SetlistController::loaded_setlist_id()
    bool loading = false;     // LiveSetSwitcher::loading()
  };
  // The whole per-tick body: binds the store to the calling (control) thread on the first call, observes `pos` and runs tick(). True when a file was written.
  bool update(const Position& pos);

  // Control tick (<= 20 ms). Writes at most once when idle-safe; true when a file was written.
  bool tick();

  bool dirty() const noexcept { return dirty_; }
  const Pointer& pending() const noexcept { return pending_; }

 private:
  BootIntoStore& store_;
  TrackerProbes probes_;
  Pointer pending_, observed_;
  bool dirty_ = false, have_observed_ = false, bound_ = false;
  std::uint64_t retry_at_ns_ = 0;  // after a failed write: no new attempt before this time
  std::uint64_t change_ns_ = 0;       // when the pending pointer last changed
  std::uint64_t quiet_since_ns_ = 0;  // the last tick that saw a note held or the pedal down (or the start)
};

}  // namespace velora::host::bootinto
