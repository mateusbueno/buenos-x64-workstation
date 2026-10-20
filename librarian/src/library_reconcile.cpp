#include "velora/librarian/library_reconcile.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <climits>
#include <filesystem>
#include <map>
#include <set>

#include "velora/format/bundle.hpp"
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
  if (::stat((dir / host::kBundleFileName).c_str(), &bs) != 0) {
    if (errno == ENOENT || errno == ENOTDIR) return Cur::kAbsent;
    why = dir.string() + ": " + std::strerror(errno);
    return Cur::kError;
  }
  if (!S_ISREG(bs.st_mode)) return Cur::kAbsent;
  dir_out = dir.string();
  return Cur::kFound;
}

}  // namespace

CatalogStatus reconcile_libraries(Catalog& cat, const std::string& data_dir, ReconcileReport& rep, const ReconcileOptions& opt) {
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
        case Cur::kFound: disk.push_back({d.filename().string(), cur}); break;
        case Cur::kAbsent: break;
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
    DescribeResult dr = describe_bundle(d.bundle_dir);
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
      auto rd = fmt::BundleReader::open_file(d.bundle_dir + "/" + host::kBundleFileName);
      const fmt::Error e = rd ? rd.value().verify_all() : rd.error();
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

CatalogStatus rebuild_libraries(Catalog& cat, const std::string& data_dir, ReconcileReport& rep, const ReconcileOptions& opt) {
  std::vector<LibraryRecord> rows;
  CatalogStatus st = cat.list_libraries(rows, false);
  if (st != CatalogStatus::kOk) {
    rep = ReconcileReport{};
    rep.status = st;
    return st;
  }
  std::size_t dropped = 0;
  for (const auto& r : rows) {
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
