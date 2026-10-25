#pragma once
// Migration runner (Story 8.5, FR49, FR69, AR29, NFR44, AD-12): irreversible format migrations only AFTER the A/B slot is marked good.
//
// The rule it enforces. A release that finds data written by the previous release (format version N-1) reads it through its own N-1 reader and keeps
// it read-only. The data is rewritten as version N only once the running slot has been marked good (RAUC `mark-good`, dev-board work: this module takes
// the answer through the injected SlotProbe and knows nothing about RAUC). Until then a rollback to the previous firmware finds every byte untouched,
// because nothing was written; after it, the migration is committed and the previous release is never booted again. Version N is what is written, never N+1.
//
// A migration unit migrates ONE file of the data root from version `from` to `to` (to == the running release's N, from == N-1). The write is the data
// root's atomic replace (temp file, fsync, rename, directory fsync: liveset::atomic_replace_file), so a kill at any point leaves the old file or the new
// one, never a partial one; a failing migration (the transform refuses, the output has the wrong version, the disk is full) leaves the old file intact.
// Readers stay N / N-1 (min_readable_version): the data written as N opens on this release and, as its N-1, on the next one.
//
// Probe contract (what the dev-board work wires): `slot_good()` is true only when the init system reports the booted slot as good (committed by the
// update client after the boot health checks passed, i.e. the point of no return of the update). It must be cheap and must never block a performance:
// the caller (the librarian at startup after reconcile, or the control thread at an idle-safe moment, never during playing) runs the runner once per boot
// and again after the mark-good event. A probe that cannot tell answers false: the migration waits.
//
// Temp file: `<file>.migrate.tmp`. A killed run may leave it; the next run that is about to write removes it first (single writer), so it never blocks a
// migration. Lost update: after the temp file is fsynced and just before the rename the source is re-read; if it changed since it was migrated the run
// returns kFailed and the file is untouched. Callbacks that throw are caught (kFailed); a throwing probe means "not good".
// Control thread / librarian only: allocates and does file I/O.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "live_set_store.hpp"

namespace velora::host::migration {

using SlotProbe = std::function<bool()>;

struct Unit {
  std::string name;    // for logs and the report
  std::string file;    // a plain file name inside Options::dir (no separators)
  std::uint32_t from_version = 0;  // N-1
  std::uint32_t to_version = 0;    // N: must equal Options::current_version
  std::size_t max_bytes = liveset::kMaxFileBytes;
  // The format's own version reader: false = the bytes are not a readable file of this format. Never mutates anything.
  std::function<bool(std::string_view bytes, std::uint32_t& version)> version_of;
  // The migration: `in` is a file at from_version, `out` must be a file at to_version. false = refuse (the file stays as it is).
  std::function<bool(std::string_view in, std::string& out)> migrate;
  // Optional: a last check of the migrated bytes (beyond "readable, exactly version N") before anything is written. false = kFailed, file untouched.
  std::function<bool(std::string_view out)> validate;
};

struct Options {
  std::string dir;                        // the data directory holding the unit files, on the journaled partition
  std::uint32_t current_version = 0;      // N of the running release (every unit must migrate to exactly this)
  SlotProbe slot_good;                    // empty = never good (fails closed)
  std::function<liveset::HookAction(liveset::Stage)> on_stage;  // fault injection, as the stores
  bool inject_dir_sync_failure = false;   // test only
  int inject_write_errno = 0;             // test only
};

enum class Outcome : std::uint8_t {
  kAbsent = 0,         // no such file: nothing to migrate
  kUpToDate,           // the file is already at to_version
  kDeferred,           // needs migration, the slot is not good: read-only, nothing written
  kMigrated,           // rewritten as to_version, durable
  kMigratedNotDurable, // the rename happened (new file IS in place) but the directory fsync failed or the run stopped after the rename
  kNotMigratable,      // the version is neither from nor to (too old / too new / unreadable): left untouched
  kFailed,             // the migration or the write failed BEFORE the rename (callback refused or threw, validation, lost update, ENOSPC): the old file is intact
  kBadUnit,            // the unit is malformed (to_version != current_version, from != to - 1, missing callbacks, bad file name): nothing touched
  kCount
};
const char* to_string(Outcome o);

struct UnitResult {
  std::string name;
  Outcome outcome = Outcome::kAbsent;
  std::uint32_t version_before = 0;  // 0 when absent / unreadable
  std::uint32_t version_after = 0;   // version of the file now on disk (0 when absent / unreadable)
  std::string detail;                // closed, human readable reason for kFailed / kBadUnit / kNotMigratable
};

struct Report {
  std::vector<UnitResult> units;
  bool slot_probed = false;  // the probe was asked (only when some unit needed it)
  bool slot_good = false;
  std::size_t count(Outcome o) const;
  bool any_pending() const { return count(Outcome::kDeferred) > 0; }
};

// Runs every unit in order. Units are independent (one failing does not stop the next). The probe is asked at most once, and only when a unit has a file
// at from_version. Before the slot is good no file is created, written, renamed or removed (reads only).
Report run(const Options& opt, const std::vector<Unit>& units);

}  // namespace velora::host::migration
