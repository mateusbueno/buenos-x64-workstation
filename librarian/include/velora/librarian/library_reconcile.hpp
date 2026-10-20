#pragma once
// The startup reconcile pass of the libraries catalog (Story 5.6, AR28). It makes `<data>/lib` and the catalog agree, whatever a crash left behind:
//   1. deletes staging leftovers (`<data>/lib/.staging/*` except the lock) and orphan `.current.tmp.*` links in every library directory;
//   2. removes the catalog rows of every library that has no usable `current` bundle (no link, a dangling link, an unreadable bundle, a bundle that
//      fails verification when it would be (re)cataloged, a bundle with an instrument that has no valid category). Only a DEFINITE absence removes a row:
//      any other error while listing `lib/` or resolving `current` (EACCES, EIO...) is recorded in `errors` and the pass removes NO row; a bundle whose
//      catalog.meta is unreadable, or that cannot be opened, keeps its existing row and is reported in `skipped`;
//   3. creates or refreshes the rows of every library whose `current` bundle is not (or not identically) in the catalog, from the bundle alone
//      (catalog.meta; a bundle that predates it, e.g. an Epic 1 demo bundle or a CLI import, is listed with the instrument names of its manifest
//      and the category of its legacy `category` file; one without a valid category is NOT cataloged and reported in `skipped`).
// The pass is idempotent (a second run changes nothing) and crash safe (every step is a file removal or one catalog transaction; running it again after a
// kill converges). Bundle directories that are not `current` are left alone (leases) EXCEPT those that still carry the checkpoint marker (Story 5.8: a superseded
// checkpoint or a publish that was never flipped; see kCheckpointMarkerName): they are deleted. The marker of a `current` bundle is removed. Called by LibraryService::start() before it serves;
// `rebuild_libraries` drops every library row first (the catalog rebuilt from bundles alone).
#include <functional>
#include <string>
#include <vector>

#include "velora/format/protected_verify.hpp"
#include "velora/librarian/catalog.hpp"

namespace velora::librarian {

struct ReconcileOptions {
  // Test hook: called before each catalog write of the pass ("reconcile.add", "reconcile.update", "reconcile.remove") and after the file cleanup
  // ("reconcile.cleaned"). A hook may _exit() the process.
  std::function<void(const char* point)> hook;
  // Verify (verify_all) a bundle before it is added to or refreshed in the catalog. Default on; a bundle that fails is not cataloged.
  bool verify_new_rows = true;
  // Story 6.4: the trusted content-manifest keys. A library whose `current` bundle is protected (`bundle.vlrp`: a Factory library) is cataloged from its signed manifest and
  // sealed catalog.meta and, with verify_new_rows, by hashing every ciphertext block, exactly like a user bundle; without trusted keys it cannot be examined
  // (its existing row, if any, is kept and the library is reported in `skipped`). Nothing is ever decrypted.
  const velora::format::TrustedKeys* trusted = nullptr;
  // Test hook: an errno (0 = none) to pretend a file system probe failed with ("list_lib", "current" with the path being examined).
  std::function<int(const char* what, const std::string& path)> inject_errno;
};

struct ReconcileReport {
  CatalogStatus status = CatalogStatus::kOk;
  int staging_removed = 0, tmp_links_removed = 0;
  int bundles_pruned = 0;  // Story 5.8: non-current bundle directories that still carry the checkpoint marker (superseded checkpoints, unflipped publishes of a first-time import)
  std::size_t libraries_on_disk = 0;  // libraries with a usable `current` that is cataloged after the pass
  std::size_t rows_removed = 0, rows_added = 0, rows_updated = 0, rows_unchanged = 0;
  std::vector<std::string> skipped;   // "<library_id>: why" for a `current` bundle that was not cataloged
  std::vector<std::string> errors;    // I/O problems; a pass with an error never removes rows (it cannot tell absent from unreadable)
  bool uncertain = false;             // some library could not be judged: its row (if any) is kept
  bool changed() const { return staging_removed || tmp_links_removed || bundles_pruned || rows_removed || rows_added || rows_updated; }
};

CatalogStatus reconcile_libraries(Catalog& catalog, const std::string& data_dir, ReconcileReport& report, const ReconcileOptions& opt = {});
CatalogStatus rebuild_libraries(Catalog& catalog, const std::string& data_dir, ReconcileReport& report, const ReconcileOptions& opt = {});

}  // namespace velora::librarian
