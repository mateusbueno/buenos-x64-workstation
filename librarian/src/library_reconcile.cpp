#include "velora/librarian/library_reconcile.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <climits>
#include <filesystem>
#include <map>
#include <chrono>
#include <set>

#include "velora/format/bundle.hpp"
#include "velora/format/protected_verify.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/librarian/import.hpp"
#include "velora/librarian/library_index.hpp"

namespace velora::librarian {
namespace fs = std::filesystem;
namespace fmt = velora::format;

namespace {

bool is_bundle_id(const std::string& s) {
  return s.size() == 64 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

enum class Cur { kFound, kAbsent, kError };

// Resolves `current`: kAbsent only for a definite "not there" (ENOENT, not a link, malformed or dangling target); any other errno is kError.
Cur current_bundle_dir(const fs::path& lib_dir, const ReconcileOptions& opt, std::string& dir_out, std::string& why) {
  const std::string link = (lib_dir / host::kCurrentLinkName).string();
  if (opt.inject_errno)
    if (const int e = opt.inject_errno("current", link); e != 0) {
      why = link + ": " + std::strerror(e);
      return e == ENOENT ? Cur::kAbsent : Cur::kError;
    }
  struct stat st;
  if (::lstat(link.c_str(), &st) != 0) {
    if (errno == ENOENT) return Cur::kAbsent;
    why = link + ": " + std::strerror(errno);
    return Cur::kError;
  }
  if (!S_ISLNK(st.st_mode)) return Cur::kAbsent;
  char buf[PATH_MAX];
  const ssize_t n = ::readlink(link.c_str(), buf, sizeof buf - 1);
  if (n < 0) {
    why = link + ": " + std::strerror(errno);
    return Cur::kError;
  }
  if (n == 0) return Cur::kAbsent;
  const std::string target(buf, static_cast<std::size_t>(n));
  if (!is_bundle_id(target)) return Cur::kAbsent;
  const fs::path dir = lib_dir / target;
  struct stat bs;
  int rc = ::stat((dir / host::kBundleFileName).c_str(), &bs);
  if (rc != 0 && (errno == ENOENT || errno == ENOTDIR)) rc = ::stat((dir / kProtectedBundleFileName).c_str(), &bs);  // Story 6.4: a protected bundle
  if (rc != 0) {
    if (errno == ENOENT || errno == ENOTDIR) return Cur::kAbsent;
    why = dir.string() + ": " + std::strerror(errno);
    return Cur::kError;
  }
  if (!S_ISREG(bs.st_mode)) return Cur::kAbsent;
  dir_out = dir.string();
  return Cur::kFound;
}

// Story 5.8: deletes the bundle directories of a library that carry the checkpoint marker and are not `current`, and clears the marker of the current one.
void prune_marked(const fs::path& lib_dir, const std::string& current_id, ReconcileReport& rep) {
  std::error_code ec;
  for (fs::directory_iterator it(lib_dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string n = it->path().filename().string();
    if (!is_bundle_id(n)) continue;
    const fs::path marker = it->path() / kCheckpointMarkerName;
    std::error_code e2;
    if (!fs::exists(marker, e2)) continue;
    if (n == current_id) {
      fs::remove(marker, e2);
      continue;
    }
    std::error_code rm;
    fs::remove_all(it->path(), rm);
    if (!rm) ++rep.bundles_pruned;
  }
}

}  // namespace

std::string reconcile_summary(const ReconcileReport& r) {
  return "reconcile summary: staging_deleted=" + std::to_string(r.staging_removed) + " tmp_links_deleted=" + std::to_string(r.tmp_links_removed) +
         " catalog_rows_removed=" + std::to_string(r.rows_removed) + " catalog_rows_recreated=" + std::to_string(r.rows_added) +
         " catalog_rows_updated=" + std::to_string(r.rows_updated) + " catalog_rows_unchanged=" + std::to_string(r.rows_unchanged) +
         " gc_bundles_deleted=" + std::to_string(r.bundles_pruned) + " not_cataloged=" + std::to_string(r.skipped.size()) +
         " errors=" + std::to_string(r.errors.size()) + " libraries=" + std::to_string(r.libraries_on_disk) + " elapsed_ms=" + std::to_string(r.elapsed_us / 1000);
}

namespace {
CatalogStatus reconcile_impl(Catalog& cat, const std::string& data_dir, ReconcileReport& rep, const ReconcileOptions& opt);
}

CatalogStatus reconcile_libraries(Catalog& cat, const std::string& data_dir, ReconcileReport& rep, const ReconcileOptions& opt) {
  const auto t0 = std::chrono::steady_clock::now();
  const CatalogStatus st = reconcile_impl(cat, data_dir, rep, opt);
  rep.elapsed_us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
  return st;
}

namespace {
CatalogStatus reconcile_impl(Catalog& cat, const std::string& data_dir, ReconcileReport& rep, const ReconcileOptions& opt) {
  rep = ReconcileReport{};
  auto hook = [&](const char* p) {
    if (opt.hook) opt.hook(p);
  };
  const fs::path lib_root = fs::path(data_dir) / host::kLibDirName;
  std::error_code ec;

  // 1. Leftovers of crashed imports.
  bool lib_exists = false;
  {
    const int inj = opt.inject_errno ? opt.inject_errno("list_lib", lib_root.string()) : 0;
    ec.clear();
    lib_exists = inj == 0 && fs::exists(lib_root, ec);
    if (inj != 0 && inj != ENOENT) ec = std::make_error_code(static_cast<std::errc>(inj));
    if (ec) {
      rep.errors.push_back("cannot examine " + lib_root.string() + ": " + ec.message());
      rep.uncertain = true;
      lib_exists = false;
    }
  }
  if (lib_exists) {
    const int n = reconcile_staging(data_dir);
    if (n < 0) rep.errors.push_back("cannot clean " + (lib_root / kStagingDirName).string());
    else rep.staging_removed = n;
  }

  // 2. What is on disk: libraries with a usable `current`.
  struct OnDisk {
    std::string library_id, bundle_dir;
  };
  std::vector<OnDisk> disk;
  std::set<std::string> keep;  // libraries that could not be judged (I/O error, unreadable bundle or metadata): their rows stay
  bool listed = true;
  if (lib_exists) {
    std::vector<fs::path> dirs;
    for (fs::directory_iterator it(lib_root, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code e2;
      if (!it->is_directory(e2) || it->is_symlink(e2)) continue;
      format::Uuid u;
      if (!host::parse_library_id(it->path().filename().string(), u)) continue;
      dirs.push_back(it->path());
    }
    if (ec) {
      listed = false;
      rep.errors.push_back("cannot list " + lib_root.string() + ": " + ec.message());
    }
    std::sort(dirs.begin(), dirs.end());
    for (const fs::path& d : dirs) {
      std::error_code e2;
      for (fs::directory_iterator it(d, e2), end; !e2 && it != end; it.increment(e2)) {
        if (it->path().filename().string().rfind(".current.tmp", 0) != 0) continue;
        std::error_code rm;
        fs::remove(it->path(), rm);
        if (!rm) ++rep.tmp_links_removed;
      }
      std::string cur, why;
      switch (current_bundle_dir(d, opt, cur, why)) {
        case Cur::kFound:
          disk.push_back({d.filename().string(), cur});
          prune_marked(d, fs::path(cur).filename().string(), rep);
          break;
        case Cur::kAbsent:
          prune_marked(d, std::string(), rep);
          break;
        case Cur::kError:
          rep.errors.push_back(why);
          rep.uncertain = true;
          keep.insert(d.filename().string());  // cannot tell: whatever row it has stays
          break;
      }
    }
  }
  hook("reconcile.cleaned");

  // 3. The catalog as it is.
  std::vector<LibraryRecord> rows;
  rep.status = cat.list_libraries(rows, /*with_instruments=*/false);
  if (rep.status != CatalogStatus::kOk) return rep.status;
  std::set<std::string> catalog_ids;
  for (const auto& r : rows) catalog_ids.insert(r.library_id);

  // 4. Add or refresh the rows of every library on disk; remember which are valid.
  std::set<std::string> valid;
  for (const OnDisk& d : disk) {
    DescribeOptions dopt;
    dopt.trusted = opt.trusted;
    DescribeResult dr = describe_bundle(d.bundle_dir, dopt);
    if (dr.status != DescribeStatus::kOk) {
      rep.skipped.push_back(d.library_id + ": " + dr.why);
      if (dr.status == DescribeStatus::kUnreadable || dr.status == DescribeStatus::kMetaUnreadable) keep.insert(d.library_id);  // not a definite "no": keep the row
      continue;
    }
    LibraryRecord have;
    bool found = false;
    rep.status = cat.get_library(d.library_id, have, found);
    if (rep.status != CatalogStatus::kOk) return rep.status;
    if (found && same_content(have, dr.record)) {
      valid.insert(d.library_id);
      ++rep.rows_unchanged;
      continue;
    }
    if (opt.verify_new_rows) {
      fmt::Error e = fmt::Error::kNone;
      try {
        if (dr.record.kind == LibraryKind::kFactory) {  // ciphertext block hashes only; nothing is decrypted
          auto pv = fmt::ProtectedBundleView::open_file(d.bundle_dir + "/" + kProtectedBundleFileName, *opt.trusted);
          e = pv ? pv.value().verify_all_blocks() : pv.error();
        } else {
          auto rd = fmt::BundleReader::open_file(d.bundle_dir + "/" + host::kBundleFileName);
          e = rd ? rd.value().verify_all() : rd.error();
        }
      } catch (const std::exception&) {  // out of memory while examining: not verified, not cataloged
        e = fmt::Error::kIo;
      }
      if (e != fmt::Error::kNone) {
        rep.skipped.push_back(d.library_id + ": the bundle fails verification (" + fmt::to_string(e) + ")");
        continue;
      }
    }
    hook(found ? "reconcile.update" : "reconcile.add");
    bool changed = false;
    rep.status = cat.publish_library(dr.record, changed);
    if (rep.status != CatalogStatus::kOk) return rep.status;
    valid.insert(d.library_id);
    if (found) ++rep.rows_updated;
    else ++rep.rows_added;
  }

  // 5. Rows without a usable `current` go (not when the listing was incomplete: absent and unreadable cannot be told apart then).
  if (listed && rep.errors.empty()) {
    for (const std::string& id : catalog_ids) {
      if (valid.count(id) || keep.count(id)) continue;
      hook("reconcile.remove");
      bool removed = false;
      rep.status = cat.remove_library(id, removed);
      if (rep.status != CatalogStatus::kOk) return rep.status;
      if (removed) ++rep.rows_removed;
    }
  }
  rep.libraries_on_disk = valid.size();
  if (!keep.empty()) rep.uncertain = true;
  rep.status = CatalogStatus::kOk;
  return rep.status;
}
}  // namespace

CatalogStatus rebuild_libraries(Catalog& cat, const std::string& data_dir, ReconcileReport& rep, const ReconcileOptions& opt) {
  std::vector<LibraryRecord> rows;
  CatalogStatus st = cat.list_libraries(rows, false);
  if (st != CatalogStatus::kOk) {
    rep = ReconcileReport{};
    rep.status = st;
    return st;
  }
  std::size_t dropped = 0;
  const bool can_examine_factory = opt.trusted && opt.trusted->size() > 0;
  for (const auto& r : rows) {
    // Story 6.4: a Factory row is dropped only when it can be rebuilt from its bundle, i.e. when there are trusted keys to examine a protected bundle with.
    // Without them the pass would list nothing for it and the library would vanish from the catalog.
    if (r.kind == LibraryKind::kFactory && !can_examine_factory) continue;
    bool removed = false;
    st = cat.remove_library(r.library_id, removed);
    if (st != CatalogStatus::kOk) {
      rep = ReconcileReport{};
      rep.status = st;
      return st;
    }
    if (removed) ++dropped;
  }
  st = reconcile_libraries(cat, data_dir, rep, opt);
  rep.rows_removed += dropped;
  return st;
}

}  // namespace velora::librarian
