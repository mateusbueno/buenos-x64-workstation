#include "velora/host/migration_runner.hpp"

#include <unistd.h>

namespace velora::host::migration {

const char* to_string(Outcome o) {
  switch (o) {
    case Outcome::kAbsent: return "absent";
    case Outcome::kUpToDate: return "up_to_date";
    case Outcome::kDeferred: return "deferred";
    case Outcome::kMigrated: return "migrated";
    case Outcome::kMigratedNotDurable: return "migrated_not_durable";
    case Outcome::kNotMigratable: return "not_migratable";
    case Outcome::kFailed: return "failed";
    case Outcome::kBadUnit: return "bad_unit";
    case Outcome::kCount: break;
  }
  return "?";
}

std::size_t Report::count(Outcome o) const {
  std::size_t n = 0;
  for (const UnitResult& u : units) n += u.outcome == o ? 1 : 0;
  return n;
}

namespace {
bool plain_name(const std::string& s) { return !s.empty() && s != "." && s != ".." && s.find('/') == std::string::npos && s.find('\0') == std::string::npos; }

// Callbacks belong to the caller: an exception becomes a typed refusal, never propagates out of run().
template <class F>
bool guarded(F&& f, bool& threw) {
  try {
    return f();
  } catch (...) {
    threw = true;
    return false;
  }
}
}  // namespace

Report run(const Options& opt, const std::vector<Unit>& units) {
  Report rep;
  for (const Unit& u : units) {
    UnitResult r;
    r.name = u.name;
    // from >= 1 and to == from + 1 written so it cannot overflow (UINT32_MAX as from_version is refused).
    const bool shape_ok = u.from_version >= 1 && u.to_version >= 2 && u.to_version - 1 == u.from_version;
    if (!plain_name(u.file) || !u.version_of || !u.migrate || u.to_version != opt.current_version || !shape_ok) {
      r.outcome = Outcome::kBadUnit;
      r.detail = u.to_version != opt.current_version ? "to_version is not the running release's version (only N is ever written)"
                                                     : "malformed unit (file name, callbacks or from != to - 1, from >= 1)";
      rep.units.push_back(std::move(r));
      continue;
    }
    const std::string path = opt.dir + "/" + u.file;
    std::string bytes;
    const liveset::Status rs = liveset::read_regular_file(path, u.max_bytes, bytes);  // no symlinks, regular files only, capped
    if (rs == liveset::Status::kNotFound) {
      r.outcome = Outcome::kAbsent;
      rep.units.push_back(std::move(r));
      continue;
    }
    if (rs != liveset::Status::kOk) {
      r.outcome = Outcome::kNotMigratable;
      r.detail = rs == liveset::Status::kTooLarge ? "the file is larger than max_bytes" : "cannot read the file (not a regular file, a symlink, or an I/O error)";
      rep.units.push_back(std::move(r));
      continue;
    }
    bool threw = false;
    std::uint32_t v = 0;
    if (!guarded([&] { return u.version_of(bytes, v); }, threw)) {
      if (threw) {
        r.outcome = Outcome::kFailed;
        r.detail = "version_of threw";
      } else {
        r.outcome = Outcome::kNotMigratable;
        r.detail = "not a readable file of this format";
      }
      rep.units.push_back(std::move(r));
      continue;
    }
    r.version_before = r.version_after = v;
    if (v == u.to_version) {
      r.outcome = Outcome::kUpToDate;
    } else if (v != u.from_version) {
      r.outcome = Outcome::kNotMigratable;
      r.detail = "version is neither N-1 nor N";
    } else {
      if (!rep.slot_probed) {
        rep.slot_probed = true;
        bool probe_threw = false;
        rep.slot_good = opt.slot_good ? guarded([&] { return opt.slot_good(); }, probe_threw) : false;  // a throwing probe is "not good"
      }
      if (!rep.slot_good) {
        r.outcome = Outcome::kDeferred;  // read-only: the N-1 reader serves the data, a rollback finds it byte-identical
      } else {
        std::string out;
        std::uint32_t ov = 0;
        bool migrated_ok = guarded([&] { return u.migrate(bytes, out); }, threw);
        if (!migrated_ok) {
          r.outcome = Outcome::kFailed;
          r.detail = threw ? "the migration threw" : "the migration refused the file";
        } else if (out.size() > u.max_bytes || !guarded([&] { return u.version_of(out, ov); }, threw) || ov != u.to_version) {
          r.outcome = Outcome::kFailed;
          r.detail = threw ? "version_of threw on the output" : "the migration did not produce a readable file of exactly version N";
        } else if (u.validate && !guarded([&] { return u.validate(out); }, threw)) {
          r.outcome = Outcome::kFailed;
          r.detail = threw ? "validate threw" : "the migrated file failed the unit's validation";
        } else {
          // A temp file left by a killed run is stale by definition (single writer): remove it, the exclusive create would refuse it.
          const std::string tmp_name = u.file + ".migrate.tmp";
          ::unlink((opt.dir + "/" + tmp_name).c_str());
          // Lost-update guard: after the temp file is fsynced and before the rename, the source must still be what was migrated.
          bool changed = false, renamed = false;
          auto hook = [&](liveset::Stage st) {
            if (st == liveset::Stage::kFsynced) {
              std::string now;
              if (liveset::read_regular_file(path, u.max_bytes, now) != liveset::Status::kOk || now != bytes) {
                changed = true;
                return liveset::HookAction::kAbort;
              }
            }
            if (st == liveset::Stage::kRenamed) renamed = true;
            return opt.on_stage ? opt.on_stage(st) : liveset::HookAction::kContinue;
          };
          const liveset::Status ws = liveset::atomic_replace_file(opt.dir, u.file, tmp_name, out, hook, opt.inject_dir_sync_failure, opt.inject_write_errno);
          if (ws == liveset::Status::kOk) {
            r.outcome = Outcome::kMigrated;
            r.version_after = ov;
          } else if (ws == liveset::Status::kCommittedNotDurable || (ws == liveset::Status::kFaultInjected && renamed)) {
            r.outcome = Outcome::kMigratedNotDurable;  // the rename happened: the new file IS in place
            r.version_after = ov;
          } else {
            r.outcome = Outcome::kFailed;
            if (changed) {
              ::unlink((opt.dir + "/" + tmp_name).c_str());
              r.detail = "the source file changed during the migration (lost update refused); the file is untouched";
            } else {
              r.detail = std::string("atomic write failed before the rename (the file is untouched): ") + liveset::to_string(ws);
            }
          }
        }
      }
    }
    rep.units.push_back(std::move(r));
  }
  return rep;
}

}  // namespace velora::host::migration
