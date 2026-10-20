// Story 5.8 tests: checkpointed publishing of a multi-timbre import, drive removal, Import again and the CLI/service flags.
//
// Importer level (import_file, no service): checkpoint boundaries (interval 1, interval N with a remainder, an exact multiple, the byte threshold with injected
// sizes, no checkpoint when the library is already published), immutability of every published bundle, drive removal before anything finished / after a
// checkpoint / halfway through the first timbre / in an SFZ, Import again (same library_id and instrument_key, a changed timbre replaced in a new bundle, the
// resolver the Live Set loader uses resolves it unchanged) and removal during Import again (the old bundle stays current and intact).
// Service level (a real socket client): the same through ImportRequest with catalog rows observed at every checkpoint, the DRIVE_REMOVED terminal state with
// its counts, earlier libraries untouched, the built-in stat probe (the source folder disappears), Import again through ImportAction IMPORT.
// CLI: --checkpoint-timbres / --checkpoint-bytes, the test CLI's SIGKILL and source-gone environment hooks.
// Env-gated (VELORA_TEST_DISK_IMAGE=1, macOS): a hdiutil disk image detached in the middle of an import; skips cleanly otherwise.
//
// No timing assertion anywhere; waits are bounded polls for an event. Fixtures are generated (import_fixtures.hpp).
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>

#include "import_fixtures.hpp"
#include "lib_test_util.hpp"
#include "velora/format/bundle.hpp"
#include "velora/format/byte_source.hpp"
#include "velora/format/hash.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/librarian/import.hpp"
#include "velora/librarian/library_index.hpp"
#include "velora/librarian/library_reconcile.hpp"
#include "velora/librarian/library_service.hpp"

using namespace velora::librarian;
namespace fs = std::filesystem;
namespace fmt = velora::format;
using ltest::Client;
using ltest::TempDir;

namespace {

constexpr unsigned kPiano = 1;

// --- importer-level helpers ---------------------------------------------------------------------------------------------------------------------

struct Published {
  std::size_t timbres = 0;
  std::string bundle_id, bundle_dir, bytes;  // the bytes of bundle.vlb at the moment of the checkpoint
  std::string current;                        // what `current` named at that moment
  bool verified = false;                      // bundle.vlb verified at that moment
  std::size_t dirs_now = 0;                   // bundle directories of the library at that moment (a superseded checkpoint is already gone)
  std::vector<std::string> keys;
  std::vector<fmt::Digest256> hashes;         // first sample of each instrument
};

struct Run {
  ImportResult r;
  std::vector<Published> cps;
  std::vector<std::string> points;  // timbre points in order, "name:index"
};

std::string current_of(const std::string& root, const std::string& lib) {
  std::error_code ec;
  const fs::path l = fs::path(root) / "lib" / lib / "current";
  return fs::is_symlink(l, ec) ? fs::read_symlink(l, ec).string() : std::string();
}

std::string lib_id_of(const fs::path& sf2) {
  return fmt::uuid_to_string(fmt::library_id_from_source(fs::weakly_canonical(sf2).generic_string()));
}

std::vector<std::string> keys_of(const std::string& bundle_file) {
  std::vector<std::string> k;
  auto rd = fmt::BundleReader::open_file(bundle_file);
  if (rd.ok())
    for (const auto& i : rd.value().instruments()) k.push_back(i.key);
  return k;
}

bool bundle_verifies(const std::string& bundle_file) {
  auto rd = fmt::BundleReader::open_file(bundle_file);
  return rd.ok() && rd.value().verify_all() == fmt::Error::kNone;
}

bool staging_clean(const std::string& root) {
  std::set<std::string> n;
  std::error_code ec;
  for (fs::directory_iterator it(fs::path(root) / "lib" / ".staging", ec), end; !ec && it != end; it.increment(ec)) n.insert(it->path().filename().string());
  return n.empty() || n == std::set<std::string>{".lock"};
}

std::size_t bundle_dirs(const std::string& root, const std::string& lib) {
  std::size_t n = 0;
  std::error_code ec;
  for (fs::directory_iterator it(fs::path(root) / "lib" / lib, ec), end; !ec && it != end; it.increment(ec))
    if (it->path().filename().string().size() == 64) ++n;
  return n;
}

// Runs import_file on `file` into `root`. `tweak` adjusts the options; `gone_at` > 0 makes the source vanish at the n-th "timbre-start".
Run run_import(const fs::path& file, const std::string& root, std::uint32_t interval, std::uint64_t bytes = 0, int gone_at = 0,
               const std::function<void(ImportOptions&)>& tweak = {}) {
  Run run;
  auto starts = std::make_shared<int>(0);
  auto gone = std::make_shared<bool>(false);
  ImportOptions o;
  o.sfz_path = file.string();
  o.root = root;
  o.category = "piano";
  o.checkpoint_timbres = interval;
  o.checkpoint_bytes = bytes;
  o.source_gone = [gone] { return *gone; };
  o.fault.on_timbre = [&run, starts, gone, gone_at](const char* p, std::size_t t) {
    run.points.push_back(std::string(p) + ":" + std::to_string(t));
    if (std::string(p) == "timbre-start" && ++*starts == gone_at) *gone = true;
    return HookAction::kContinue;
  };
  o.on_checkpoint = [&run, root](const ImportResult& r) {
    Published p;
    p.timbres = r.timbres_published;
    p.bundle_id = r.bundle_id;
    p.bundle_dir = r.bundle_dir;
    p.bytes = ltest::slurp(r.bundle_dir + "/bundle.vlb");
    p.current = current_of(root, r.library_id);
    p.verified = bundle_verifies(r.bundle_dir + "/bundle.vlb");
    p.dirs_now = bundle_dirs(root, r.library_id);
    if (auto rd = fmt::BundleReader::open_file(r.bundle_dir + "/bundle.vlb"); rd.ok())
      for (const auto& i : rd.value().instruments()) {
        p.keys.push_back(i.key);
        p.hashes.push_back(rd.value().samples()[i.first_sample].pcm_sha256);
      }
    run.cps.push_back(std::move(p));
  };
  if (tweak) tweak(o);
  run.r = import_file(o);
  return run;
}

std::string clean_bundle_id(const fs::path& file, std::size_t* instruments = nullptr) {
  TempDir t;
  fs::create_directories(t.sub("r"));
  const Run r = run_import(file, t.sub("r"), 0);
  if (instruments) *instruments = keys_of(r.r.bundle_dir + "/bundle.vlb").size();
  return r.r.bundle_id;
}

// --- tests: checkpoint boundaries ---------------------------------------------------------------------------------------------------------------

// Every checkpoint is a verified immutable bundle of the first k timbres that `current` names at once; the final bundle equals a clean import.
void check_boundaries(const std::string& label, unsigned presets, std::uint32_t interval, std::uint64_t bytes, const std::function<std::uint64_t(std::size_t, std::uint64_t)>& size_fn,
                      const std::vector<std::size_t>& want) {
  TempDir t;
  fs::create_directories(t.sub("src"));
  fs::create_directories(t.sub("root"));
  const fs::path sf2 = ifx::write_many_sf2(fs::path(t.sub("src")) / "many.sf2", presets);
  Run run = run_import(sf2, t.sub("root"), interval, bytes, 0, [&](ImportOptions& o) { o.checkpoint_size_fn = size_fn; });
  const char* L = label.c_str();
  if (!run.r.ok()) std::printf("%s: import failed: %s\n", L, run.r.reasons.empty() ? "?" : run.r.reasons[0].c_str());
  CHECK(run.r.ok());
  std::vector<std::size_t> got;
  for (const auto& c : run.cps) got.push_back(c.timbres);
  if (got != want) {
    std::printf("%s: checkpoints at", L);
    for (auto g : got) std::printf(" %zu", g);
    std::printf(" want");
    for (auto g : want) std::printf(" %zu", g);
    std::printf("\n");
  }
  CHECK(got == want);
  CHECK(run.r.checkpoints == want.size() && run.r.timbres_total == presets && run.r.timbres_published == presets);
  const std::string lib = run.r.library_id;
  const std::string final_file = run.r.bundle_dir + "/bundle.vlb";
  CHECK(keys_of(final_file).size() == presets && bundle_verifies(final_file));
  CHECK(current_of(t.sub("root"), lib) == run.r.bundle_id);
  CHECK(run.r.bundle_created && run.r.current_flipped);
  // Superseded checkpoints are deleted once the next bundle is current: only the final bundle is left, without the checkpoint marker.
  CHECK(bundle_dirs(t.sub("root"), lib) == 1 && !fs::exists(run.r.bundle_dir + "/" + kCheckpointMarkerName));
  CHECK(staging_clean(t.sub("root")));
  // The final state equals a clean import of the same source (same bundle id: same bytes).
  CHECK(run.r.bundle_id == clean_bundle_id(sf2));
  // Checkpoint k, as seen when it was published: `current` named it, it was the only bundle directory, it verified, it held the first k timbres (the same samples the
  // final bundle holds), and it is gone now.
  auto final_rd = fmt::BundleReader::open_file(final_file);
  for (const auto& c : run.cps) {
    CHECK(c.current == c.bundle_id && c.dirs_now == 1 && c.verified && c.keys.size() == c.timbres);
    CHECK(!fs::exists(c.bundle_dir));
    CHECK(final_rd.ok());
    if (!final_rd.ok()) continue;
    for (std::size_t i = 0; i < c.keys.size() && i < final_rd.value().instruments().size(); ++i) {
      const auto& b = final_rd.value().instruments()[i];
      CHECK(c.keys[i] == b.key && c.hashes[i] == final_rd.value().samples()[b.first_sample].pcm_sha256);
    }
  }
}

void test_checkpoint_boundaries() {
  check_boundaries("interval 1", 7, 1, 0, {}, {1, 2, 3, 4, 5, 6});
  check_boundaries("interval 3, remainder 1", 7, 3, 0, {}, {3, 6});
  check_boundaries("interval 5, exact multiple", 10, 5, 0, {}, {5});  // the last timbre ends the file: no checkpoint of its own
  check_boundaries("interval 5, 11 timbres", 11, 5, 0, {}, {5, 10});
  check_boundaries("default interval (10) with 6 timbres", 6, 0, 0, {}, {});
  check_boundaries("default interval (10) with 12 timbres", 12, 0, 0, {}, {10});
  check_boundaries("interval larger than the file", 4, 100, 0, {}, {});
  // The byte threshold: injected sizes, default 20 GiB, timbres of 10 GiB each: a checkpoint every second timbre (the count of 10 is never reached).
  const auto ten_gib = [](std::size_t, std::uint64_t) -> std::uint64_t { return 10ull << 30; };
  check_boundaries("20 GiB threshold, 10 GiB timbres", 12, 0, 0, ten_gib, {2, 4, 6, 8, 10});
  // Sizes differ per timbre: 15 + 6 GiB reach 20 GiB at timbre 2; then five of 1 GiB and 14 GiB each: 5 + 14 = 19 GiB at timbre 8 is still below, timbre 9 passes
  // (33 GiB), and 14 + 14 at timbre 11.
  const auto mixed = [](std::size_t i, std::uint64_t) -> std::uint64_t { return i == 0 ? 15ull << 30 : i == 1 ? 6ull << 30 : i < 7 ? 1ull << 30 : 14ull << 30; };
  check_boundaries("mixed sizes", 12, 0, 0, mixed, {2, 9, 11});
  // An explicit byte limit with real (tiny) sizes: every timbre exceeds it.
  check_boundaries("1 byte threshold", 4, 100, 1, {}, {1, 2, 3});
  // The count wins when it comes first.
  check_boundaries("count before bytes", 6, 2, 0, [](std::size_t, std::uint64_t) -> std::uint64_t { return 1; }, {2, 4});
}

// A library that already has a `current` bundle is never checkpointed (Import again / a crashed run's partial bundle): the old one stays until the new one is whole.
void test_no_checkpoint_when_published() {
  TempDir t;
  fs::create_directories(t.sub("src"));
  fs::create_directories(t.sub("root"));
  const fs::path sf2 = ifx::write_many_sf2(fs::path(t.sub("src")) / "m.sf2", 6);
  Run a = run_import(sf2, t.sub("root"), 1);
  CHECK(a.r.ok() && a.cps.size() == 5);
  ifx::write_many_sf2(sf2, 6, 3);  // timbre 3 changed
  Run b = run_import(sf2, t.sub("root"), 1);
  CHECK(b.r.ok() && b.cps.empty() && b.r.checkpoints == 0 && b.r.bundle_id != a.r.bundle_id);
  CHECK(current_of(t.sub("root"), b.r.library_id) == b.r.bundle_id);
  // Identical source again: unchanged, nothing new.
  Run c = run_import(sf2, t.sub("root"), 1);
  CHECK(c.r.ok() && !c.r.bundle_created && !c.r.current_flipped && c.r.bundle_id == b.r.bundle_id && c.cps.empty());
}

// --- tests: drive removal -------------------------------------------------------------------------------------------------------------------------

// Removal during the first timbre (before anything finished): nothing is published, the library does not exist, no staging leftovers.
void test_removal_before_first_timbre_finishes() {
  TempDir t;
  fs::create_directories(t.sub("src"));
  fs::create_directories(t.sub("root"));
  const fs::path sf2 = ifx::write_many_sf2(fs::path(t.sub("src")) / "m.sf2", 6);
  const Run run = run_import(sf2, t.sub("root"), 2, 0, 1);
  CHECK(run.r.status == Status::kDriveRemoved && run.r.timbres_published == 0 && run.r.timbres_total == 6 && run.cps.empty());
  CHECK(!run.r.reasons.empty());
  CHECK(!fs::exists(fs::path(t.sub("root")) / "lib" / lib_id_of(sf2)));
  CHECK(staging_clean(t.sub("root")));
  CHECK(run.r.bundle_id.empty() && !run.r.bundle_created && !run.r.current_flipped);
}

// Removal after a checkpoint, in the middle of the 4th timbre (the probe flips at its start, the first sample read notices): the checkpoint stays, the three
// finished timbres are published as the last bundle, the 4th is discarded whole.
void test_removal_after_checkpoint() {
  TempDir t;
  fs::create_directories(t.sub("src"));
  fs::create_directories(t.sub("root"));
  const fs::path sf2 = ifx::write_many_sf2(fs::path(t.sub("src")) / "m.sf2", 6);
  const Run run = run_import(sf2, t.sub("root"), 2, 0, 4);
  CHECK(run.r.status == Status::kDriveRemoved && run.r.timbres_total == 6 && run.r.timbres_published == 3 && run.r.checkpoints == 1);
  CHECK(run.cps.size() == 1 && run.cps[0].timbres == 2);
  const std::string lib = lib_id_of(sf2);
  CHECK(!run.r.bundle_id.empty() && current_of(t.sub("root"), lib) == run.r.bundle_id);
  const std::string f = run.r.bundle_dir + "/bundle.vlb";
  CHECK((keys_of(f) == std::vector<std::string>{"0:0", "0:1", "0:2"}) && bundle_verifies(f));
  CHECK(bundle_dirs(t.sub("root"), lib) == 1 && !fs::exists(run.cps[0].bundle_dir));  // the checkpoint of 2 was superseded by the partial of 3
  CHECK(!fs::exists(run.r.bundle_dir + "/" + kCheckpointMarkerName));
  CHECK(staging_clean(t.sub("root")));
  // The 3 published timbres are the first 3 of a clean import, sample for sample.
  TempDir c;
  fs::create_directories(c.sub("r"));
  const Run clean = run_import(sf2, c.sub("r"), 0);
  auto a = fmt::BundleReader::open_file(f), b = fmt::BundleReader::open_file(clean.r.bundle_dir + "/bundle.vlb");
  CHECK(a.ok() && b.ok());
  if (a.ok() && b.ok())
    for (std::size_t i = 0; i < 3; ++i)
      CHECK(a.value().samples()[a.value().instruments()[i].first_sample].pcm_sha256 == b.value().samples()[b.value().instruments()[i].first_sample].pcm_sha256);
  // The next run (the drive is back) completes the library and equals the clean import.
  const Run again = run_import(sf2, t.sub("root"), 2);
  CHECK(again.r.ok() && again.r.bundle_id == clean.r.bundle_id && again.cps.empty() && current_of(t.sub("root"), lib) == clean.r.bundle_id);
}

// Removal with finished timbres but before the first checkpoint interval: they are published (the story: "publishes the timbres finished so far").
void test_removal_before_first_checkpoint_publishes_finished() {
  TempDir t;
  fs::create_directories(t.sub("src"));
  fs::create_directories(t.sub("root"));
  const fs::path sf2 = ifx::write_many_sf2(fs::path(t.sub("src")) / "m.sf2", 6);
  const Run run = run_import(sf2, t.sub("root"), 0, 0, 3);  // default interval 10: no checkpoint is due
  CHECK(run.r.status == Status::kDriveRemoved && run.r.timbres_published == 2 && run.r.checkpoints == 0 && run.cps.empty());
  CHECK(keys_of(run.r.bundle_dir + "/bundle.vlb") == (std::vector<std::string>{"0:0", "0:1"}));
  CHECK(current_of(t.sub("root"), lib_id_of(sf2)) == run.r.bundle_id && staging_clean(t.sub("root")));
}

// Removal during Import again: the old bundle stays current and intact, nothing new is published.
void test_removal_during_import_again() {
  TempDir t;
  fs::create_directories(t.sub("src"));
  fs::create_directories(t.sub("root"));
  const fs::path sf2 = ifx::write_many_sf2(fs::path(t.sub("src")) / "m.sf2", 6);
  const Run first = run_import(sf2, t.sub("root"), 2);
  CHECK(first.r.ok());
  const std::string lib = first.r.library_id, old_bytes = ltest::slurp(first.r.bundle_dir + "/bundle.vlb");
  const std::size_t dirs = bundle_dirs(t.sub("root"), lib);
  ifx::write_many_sf2(sf2, 6, 2);  // timbre 2 changed on the source
  const Run run = run_import(sf2, t.sub("root"), 2, 0, 4);
  CHECK(run.r.status == Status::kDriveRemoved && run.r.timbres_published == 0 && run.cps.empty());
  CHECK(current_of(t.sub("root"), lib) == first.r.bundle_id && ltest::slurp(first.r.bundle_dir + "/bundle.vlb") == old_bytes);
  CHECK(bundle_dirs(t.sub("root"), lib) == dirs && bundle_verifies(first.r.bundle_dir + "/bundle.vlb") && staging_clean(t.sub("root")));
  CHECK(keys_of(first.r.bundle_dir + "/bundle.vlb").size() == 6);
}

// An SFZ is one timbre: the probe flips at its start, the first sample read notices; nothing at all is written, an earlier library is untouched.
void test_removal_sfz() {
  TempDir t;
  fs::create_directories(t.sub("src"));
  fs::create_directories(t.sub("root"));
  const fs::path a = ifx::write_sfz(t.sub("src"), "A"), b = ifx::write_sfz(t.sub("src"), "B");
  const Run ok = run_import(a, t.sub("root"), 0);
  CHECK(ok.r.ok() && ok.r.timbres_total == 1 && ok.r.timbres_published == 1);
  const std::string a_bytes = ltest::slurp(ok.r.bundle_dir + "/bundle.vlb");
  const Run run = run_import(b, t.sub("root"), 0, 0, 1);
  CHECK(run.r.status == Status::kDriveRemoved && run.r.timbres_published == 0 && run.r.timbres_total == 1);
  CHECK(!fs::exists(fs::path(t.sub("root")) / "lib" / run.r.library_id) || run.r.library_id.empty());
  CHECK(staging_clean(t.sub("root")) && current_of(t.sub("root"), ok.r.library_id) == ok.r.bundle_id);
  CHECK(ltest::slurp(ok.r.bundle_dir + "/bundle.vlb") == a_bytes);
  // A parse that fails because the probe says the source is gone is removal, not `damaged`.
  const fs::path missing = fs::path(t.sub("src")) / "Gone.sfz";
  const Run absent = run_import(missing, t.sub("root"), 0, 0, 0, [](ImportOptions& o) { o.source_gone = [] { return true; }; });
  CHECK(absent.r.status == Status::kDriveRemoved);
  const Run damaged = run_import(missing, t.sub("root"), 0);
  CHECK(damaged.r.status == Status::kSkipped && damaged.r.reason_code == ReasonCode::kDamaged);  // the same file with a live drive stays "damaged"
}

// A stop (cancel) between timbres keeps what the checkpoints published.
void test_abort_between_timbres_keeps_checkpoints() {
  TempDir t;
  fs::create_directories(t.sub("src"));
  fs::create_directories(t.sub("root"));
  const fs::path sf2 = ifx::write_many_sf2(fs::path(t.sub("src")) / "m.sf2", 6);
  auto starts = std::make_shared<int>(0);
  const Run run = run_import(sf2, t.sub("root"), 2, 0, 0, [&](ImportOptions& o) {
    auto prev = o.fault.on_timbre;
    o.fault.on_timbre = [prev, starts](const char* p, std::size_t t) {
      prev(p, t);
      return std::string(p) == "timbre-start" && ++*starts == 5 ? HookAction::kAbort : HookAction::kContinue;
    };
  });
  CHECK(run.r.status == Status::kAborted && run.r.timbres_published == 4 && run.cps.size() == 2);
  CHECK(keys_of(run.r.bundle_dir + "/bundle.vlb").size() == 4 && current_of(t.sub("root"), lib_id_of(sf2)) == run.r.bundle_id);
  CHECK(staging_clean(t.sub("root")));
}

// --- tests: Import again ---------------------------------------------------------------------------------------------------------------------------

// Import again of a changed SoundFont: the library_id and every instrument_key stay, `current` points at a new bundle holding the re-converted timbre, the other
// timbres are bit-identical, the old bundle stays (a lease the engine may hold), and the resolver (what the Live Set loader calls for a part's timbre) answers
// the same (library_id, instrument_key) with the new data, without any edit of the Live Set.
void test_import_again_replaces_in_place() {
  TempDir t;
  fs::create_directories(t.sub("src"));
  fs::create_directories(t.sub("root"));
  const fs::path sf2 = ifx::write_many_sf2(fs::path(t.sub("src")) / "m.sf2", 5);
  const Run first = run_import(sf2, t.sub("root"), 0);
  CHECK(first.r.ok());
  velora::host::LibraryResolver resolver(velora::host::make_posix_fs(), t.sub("root"));
  const auto before = resolver.resolve(first.r.library_id, "0:2");
  CHECK(before.ok());
  const auto before_hash = before.ok() ? before.bundle->samples()[before.instrument_info().first_sample].pcm_sha256 : fmt::Digest256{};
  ifx::write_many_sf2(sf2, 5, 3);  // timbre "0:2" is the third preset
  const Run again = run_import(sf2, t.sub("root"), 0);
  CHECK(again.r.ok() && again.r.library_id == first.r.library_id && again.r.bundle_id != first.r.bundle_id);
  CHECK(again.r.bundle_created && again.r.current_flipped && current_of(t.sub("root"), again.r.library_id) == again.r.bundle_id);
  CHECK(bundle_dirs(t.sub("root"), again.r.library_id) == 2 && bundle_verifies(first.r.bundle_dir + "/bundle.vlb"));
  CHECK(keys_of(again.r.bundle_dir + "/bundle.vlb") == keys_of(first.r.bundle_dir + "/bundle.vlb"));
  const auto after = resolver.resolve(first.r.library_id, "0:2");  // the SAME reference a Live Set holds
  CHECK(after.ok());
  if (after.ok() && before.ok()) {
    CHECK(after.bundle->bundle_id() != before.bundle->bundle_id());
    CHECK(after.bundle->samples()[after.instrument_info().first_sample].pcm_sha256 != before_hash);  // re-converted
    CHECK(before.bundle->samples()[before.instrument_info().first_sample].pcm_sha256 == before_hash);  // the held lease still reads the old data
  }
  // Every other timbre is retained bit for bit.
  auto a = fmt::BundleReader::open_file(first.r.bundle_dir + "/bundle.vlb"), b = fmt::BundleReader::open_file(again.r.bundle_dir + "/bundle.vlb");
  CHECK(a.ok() && b.ok());
  if (a.ok() && b.ok())
    for (std::size_t i = 0; i < 5; ++i) {
      const auto& x = a.value().samples()[a.value().instruments()[i].first_sample];
      const auto& y = b.value().samples()[b.value().instruments()[i].first_sample];
      CHECK((i == 2) == (x.pcm_sha256 != y.pcm_sha256));
    }
  for (const char* k : {"0:0", "0:1", "0:3", "0:4"}) CHECK(resolver.resolve(first.r.library_id, k).ok());
}

// --- service-level tests ---------------------------------------------------------------------------------------------------------------------------

bool terminal(ws::v1::ImportState s) {
  return s == ws::v1::IMPORT_STATE_PUBLISHED || s == ws::v1::IMPORT_STATE_FAILED || s == ws::v1::IMPORT_STATE_CANCELLED ||
         s == ws::v1::IMPORT_STATE_DRIVE_REMOVED;
}

struct Item {
  std::string path;
  unsigned cat = kPiano;
  ws::v1::ImportAction action = ws::v1::IMPORT_ACTION_DEFAULT;
};

std::uint64_t submit(Client& c, const std::string& source, const std::vector<Item>& items) {
  ws::v1::LibraryClientMessage m;
  auto* r = m.mutable_import_request();
  r->set_source(source);
  for (const Item& i : items) {
    auto* it = r->add_items();
    it->set_path(i.path);
    it->set_category(i.cat);
    it->set_action(i.action);
  }
  auto a = c.call(m);
  return a && a->has_import_accepted() ? a->import_accepted().job_id() : 0;
}

std::vector<ws::v1::ImportProgress> collect(Client& c, std::uint64_t job, int timeout_ms = 60000) {
  std::vector<ws::v1::ImportProgress> ev;
  std::deque<ws::v1::LibraryServerMessage> keep;
  auto take = [&](const ws::v1::LibraryServerMessage& m) {
    if (m.has_import_progress() && m.import_progress().job_id() == job) ev.push_back(m.import_progress());
    else keep.push_back(m);
  };
  for (const auto& m : c.pushes) take(m);
  c.pushes.clear();
  while (ev.empty() || !terminal(ev.back().state())) {
    auto r = c.recv(timeout_ms);
    if (!r) break;
    take(*r);
  }
  for (auto& m : keep) c.pushes.push_back(std::move(m));
  return ev;
}

std::optional<ws::v1::LibraryServerMessage> list_libs(Client& c) {
  ws::v1::LibraryClientMessage m;
  m.mutable_list_libraries();
  return c.call(m);
}
std::optional<ws::v1::LibraryServerMessage> list_inst(Client& c, const std::string& id) {
  ws::v1::LibraryClientMessage m;
  m.mutable_list_instruments()->set_library_id(id);
  return c.call(m);
}

struct Svc {
  TempDir td;
  LibraryServiceOptions o;
  std::unique_ptr<LibraryService> s;
  std::string err;
  Catalog::FaultHook fault_hook;  // catalog fault injection, installed before start
  explicit Svc(const std::function<void(LibraryServiceOptions&)>& cfg = {}) {
    o.data_dir = td.sub("data");
    fs::create_directories(o.data_dir);
    o.socket_path = td.sub("s.sock");
    if (cfg) cfg(o);
  }
  bool start() {
    s = std::make_unique<LibraryService>(o);
    if (fault_hook) s->set_catalog_fault_hook(fault_hook);
    return s->start(err);
  }
  bool connect(Client& c) { return c.connect(o.socket_path) && c.handshake("checkpoint-test"); }
  std::string data() const { return o.data_dir; }
};

struct LibRow {
  std::string id, bundle;
  std::uint32_t instruments = 0;
};
std::map<std::string, LibRow> libs_of(Client& c) {
  std::map<std::string, LibRow> m;
  auto r = list_libs(c);
  if (r && r->has_library_list())
    for (const auto& l : r->library_list().libraries()) m[l.source_path()] = {l.library_id(), l.bundle_id(), l.instrument_count()};
  return m;
}

// Catalog rows follow every checkpoint, the events count them, and the final bundle equals a clean import.
void test_service_checkpoints() {
  struct Seen {
    std::size_t timbres;
    std::uint32_t listed;
    std::string bundle;
  };
  std::vector<Seen> seen;
  Client probe;
  std::string many_path;
  Svc svc([&](LibraryServiceOptions& o) {
    o.checkpoint_timbres = 2;
    o.import_timbre_hook = [&](const char* p, std::uint64_t, std::size_t n) {
      if (std::string(p) != "checkpoint-published") return;
      auto libs = libs_of(probe);  // on the worker thread, through its own connection: the catalog row of this checkpoint is already there
      const auto it = libs.find(many_path);
      seen.push_back({n, it == libs.end() ? 0u : it->second.instruments, it == libs.end() ? std::string() : it->second.bundle});
    };
  });
  CHECK(svc.start());
  TempDir src;
  const fs::path sf2 = ifx::write_many_sf2(fs::path(src.path) / "many.sf2", 7);
  many_path = fs::canonical(sf2).string();
  Client c;
  CHECK(svc.connect(c) && probe.connect(svc.o.socket_path) && probe.handshake("probe"));
  const auto job = submit(c, sf2.string(), {{""}});
  CHECK(job != 0);
  const auto ev = collect(c, job);
  CHECK(!ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_PUBLISHED);
  if (!ev.empty()) {
    const auto& last = ev.back();
    CHECK(last.checkpoints() == 3 && last.timbres_total() == 7 && last.timbres_imported() == 7 && last.items_imported() == 1 && last.items_total() == 1);
    bool item_seen = false;
    for (const auto& e : ev)
      if (e.has_item()) {
        item_seen = true;
        CHECK(e.item().timbres() == 7 && e.item().timbres_total() == 7 && e.item().status() == ws::v1::IMPORT_ITEM_IMPORTED);
      }
    CHECK(item_seen);
    std::uint64_t seq = 0;
    std::uint32_t prev_t = 0, cps = 0;
    for (const auto& e : ev) {  // monotone counters, one event per checkpoint without an item result
      CHECK(e.event_seq() == ++seq && e.timbres_imported() >= prev_t && e.checkpoints() >= cps);
      if (e.checkpoints() > cps) CHECK(!e.has_item() && e.state() == ws::v1::IMPORT_STATE_RUNNING && e.timbres_imported() == 2 * e.checkpoints());
      prev_t = e.timbres_imported();
      cps = e.checkpoints();
    }
  }
  CHECK(seen.size() == 3);
  for (std::size_t i = 0; i < seen.size(); ++i) CHECK(seen[i].timbres == 2 * (i + 1) && seen[i].listed == seen[i].timbres && !seen[i].bundle.empty());
  CHECK(seen.size() == 3 && seen[0].bundle != seen[1].bundle && seen[1].bundle != seen[2].bundle);
  auto libs = libs_of(c);
  CHECK(libs.size() == 1 && libs[many_path].instruments == 7 && libs[many_path].bundle == clean_bundle_id(sf2));
}

// The built-in probe (the source folder is no longer there) and the injected one end the job DRIVE_REMOVED with the counts; the library imported before is
// untouched; the items after the removed one are not processed.
void test_service_drive_removed(bool injected) {
  std::atomic<bool> gone{false};
  std::atomic<int> starts{0};
  fs::path moved, folder;
  Svc svc([&](LibraryServiceOptions& o) {
    o.checkpoint_timbres = 2;
    if (injected) o.source_gone = [&](const std::string&) { return gone.load(); };
    o.import_timbre_hook = [&](const char* p, std::uint64_t, std::size_t) {
      // Calls of timbre-start: A.sfz = #1, many.sf2 = #2..#7, C.sfz = #8. The 5th is the 4th timbre of the SoundFont.
      if (std::string(p) != "timbre-start" || ++starts != 5) return;
      if (injected) gone = true;
      else fs::rename(folder, moved);  // the volume vanishes: its path is no longer there
    };
  });
  CHECK(svc.start());
  TempDir src;
  folder = fs::path(src.path) / "vol";
  moved = fs::path(src.path) / "vol.gone";
  fs::create_directories(folder);
  const fs::path a = ifx::write_sfz(folder, "A"), c_sfz = ifx::write_sfz(folder, "C");
  const fs::path many = ifx::write_many_sf2(folder / "many.sf2", 6);
  Client c;
  CHECK(svc.connect(c));
  const std::string folder_s = fs::canonical(folder).string();
  const std::string a_key = folder_s + "/A.sfz", m_key = folder_s + "/many.sf2", c_key = folder_s + "/C.sfz";  // the catalog keeps the source path of the import
  const auto job = submit(c, folder_s, {{a.filename().string()}, {many.filename().string()}, {c_sfz.filename().string()}});
  CHECK(job != 0);
  const auto ev = collect(c, job);
  CHECK(!ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_DRIVE_REMOVED);
  if (!ev.empty()) {
    const auto& l = ev.back();
    CHECK(l.items_total() == 3 && l.items_done() == 3 && l.items_imported() == 1 && l.items_failed() == 0 && l.items_cancelled() == 0 && l.items_skipped() == 1);
    CHECK(l.timbres_total() == 8 && l.timbres_imported() == 4 && l.checkpoints() == 1 && l.current_item().empty());
    bool saw_item = false, saw_skipped = false;
    for (const auto& e : ev) {
      if (e.has_item() && e.item().status() == ws::v1::IMPORT_ITEM_DRIVE_REMOVED) {
        saw_item = true;
        CHECK(e.item().timbres() == 3 && e.item().timbres_total() == 6 && e.item().instruments() == 3 && e.item().partial());
        CHECK(e.bytes_done() < e.bytes_total());  // only what was converted of the removed item counts
      }
      if (e.has_item() && e.item().status() == ws::v1::IMPORT_ITEM_SKIPPED && e.item().reason() == ws::v1::IMPORT_REASON_DRIVE_REMOVED) {
        saw_skipped = true;  // C.sfz: not attempted
        CHECK(e.item().path() == c_key && e.item().timbres() == 0 && !e.item().partial());
      }
    }
    CHECK(saw_item && saw_skipped);
  }
  // Terminal state is sticky and answered to a status request.
  ws::v1::LibraryClientMessage sm;
  sm.mutable_import_status()->set_job_id(job);
  auto st = c.call(sm);
  CHECK(st && st->has_import_progress() && st->import_progress().state() == ws::v1::IMPORT_STATE_DRIVE_REMOVED);
  // Catalog: A is complete, the SoundFont has its 3 finished timbres, C is not there. Back on the disk: A's bundle is the one a clean import makes.
  auto libs = libs_of(c);
  CHECK(libs.size() == 2 && libs.count(a_key) == 1 && libs.count(m_key) == 1 && libs.count(c_key) == 0);
  CHECK(libs[m_key].instruments == 3);
  auto im = list_inst(c, libs[m_key].id);
  CHECK(im && im->has_instrument_list() && im->instrument_list().total() == 3);
  const std::string lib_dir = svc.data() + "/lib";
  CHECK(staging_clean(svc.data()));
  CHECK(bundle_verifies(lib_dir + "/" + libs[m_key].id + "/" + libs[m_key].bundle + "/bundle.vlb"));
  CHECK(keys_of(lib_dir + "/" + libs[m_key].id + "/" + libs[m_key].bundle + "/bundle.vlb").size() == 3);
  CHECK(bundle_verifies(lib_dir + "/" + libs[a_key].id + "/" + libs[a_key].bundle + "/bundle.vlb"));
  CHECK(current_of(svc.data(), libs[a_key].id) == libs[a_key].bundle);
  if (injected) CHECK(libs[a_key].bundle == clean_bundle_id(a));  // an earlier library is exactly what a clean import makes (the id depends on the path)
}

// Removal before the first timbre finished: nothing at all of that library; the job still ends DRIVE_REMOVED with 0 imported.
void test_service_removed_before_anything() {
  std::atomic<bool> gone{false};
  Svc svc([&](LibraryServiceOptions& o) {
    o.checkpoint_timbres = 2;
    o.source_gone = [&](const std::string&) { return gone.load(); };
    o.import_timbre_hook = [&](const char* p, std::uint64_t, std::size_t t) {
      if (std::string(p) == "timbre-start" && t == 0) gone = true;
    };
  });
  CHECK(svc.start());
  TempDir src;
  const fs::path many = ifx::write_many_sf2(fs::path(src.path) / "many.sf2", 5);
  Client c;
  CHECK(svc.connect(c));
  const auto job = submit(c, many.string(), {{""}});
  const auto ev = collect(c, job);
  CHECK(!ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_DRIVE_REMOVED);
  if (!ev.empty()) {
    const auto& l = ev.back();
    CHECK(l.items_imported() == 0 && l.timbres_imported() == 0 && l.timbres_total() == 5 && l.checkpoints() == 0 && l.items_done() == 1);
    bool item_seen = false;
    for (const auto& e : ev)
      if (e.has_item()) {
        item_seen = true;
        CHECK(e.item().status() == ws::v1::IMPORT_ITEM_DRIVE_REMOVED && e.item().timbres() == 0 && e.item().instruments() == 0);
      }
    CHECK(item_seen);
  }
  CHECK(libs_of(c).empty());
  std::set<std::string> top;
  for (fs::directory_iterator it(svc.data() + "/lib"), end; it != end; ++it) top.insert(it->path().filename().string());
  top.erase(kServiceLockFileName);
  CHECK(top.empty() || top == std::set<std::string>{".staging"});
  CHECK(staging_clean(svc.data()));
}

// Import again through ImportAction IMPORT: the same library_id and instrument keys, a new current bundle with the re-converted timbre; removal during a second
// Import again leaves that bundle current and the catalog row untouched.
void test_service_import_again() {
  std::atomic<bool> gone{false};
  std::atomic<int> starts{0}, arm{0};
  Svc svc([&](LibraryServiceOptions& o) {
    o.checkpoint_timbres = 2;
    o.source_gone = [&](const std::string&) { return gone.load(); };
    o.import_timbre_hook = [&](const char* p, std::uint64_t, std::size_t) {
      if (std::string(p) == "timbre-start" && arm.load() && ++starts == 3) gone = true;
    };
  });
  CHECK(svc.start());
  TempDir src;
  const fs::path sf2 = ifx::write_many_sf2(fs::path(src.path) / "m.sf2", 5);
  Client c;
  CHECK(svc.connect(c));
  auto j1 = submit(c, sf2.string(), {{""}});
  CHECK(!collect(c, j1).empty());
  const std::string key = fs::canonical(sf2).string();
  auto libs1 = libs_of(c);
  CHECK(libs1.size() == 1 && libs1[key].instruments == 5);
  const LibRow first = libs1[key];
  // Unchanged source + IMPORT: the same bundle, reported unchanged.
  auto j2 = submit(c, sf2.string(), {{"", kPiano, ws::v1::IMPORT_ACTION_IMPORT}});
  auto e2 = collect(c, j2);
  CHECK(!e2.empty() && e2.back().state() == ws::v1::IMPORT_STATE_PUBLISHED && e2.back().items_unchanged() == 1 && e2.back().checkpoints() == 0);
  CHECK(libs_of(c)[key].bundle == first.bundle);
  // Changed source + IMPORT: replaced in place.
  ifx::write_many_sf2(sf2, 5, 4);
  auto j3 = submit(c, sf2.string(), {{"", kPiano, ws::v1::IMPORT_ACTION_IMPORT}});
  auto e3 = collect(c, j3);
  CHECK(!e3.empty() && e3.back().state() == ws::v1::IMPORT_STATE_PUBLISHED && e3.back().items_imported() == 1 && e3.back().checkpoints() == 0);
  auto libs3 = libs_of(c);
  CHECK(libs3[key].id == first.id && libs3[key].bundle != first.bundle && libs3[key].instruments == 5);
  CHECK(current_of(svc.data(), first.id) == libs3[key].bundle);
  velora::host::LibraryResolver resolver(velora::host::make_posix_fs(), svc.data());
  for (const char* k : {"0:0", "0:1", "0:2", "0:3", "0:4"}) CHECK(resolver.resolve(first.id, k).ok());
  // Removal during the next Import again: the current bundle (libs3) stays, the row is untouched, the job is DRIVE_REMOVED with 0 imported from the library.
  ifx::write_many_sf2(sf2, 5, 1);
  arm = 1;
  auto j4 = submit(c, sf2.string(), {{"", kPiano, ws::v1::IMPORT_ACTION_IMPORT}});
  auto e4 = collect(c, j4);
  CHECK(!e4.empty() && e4.back().state() == ws::v1::IMPORT_STATE_DRIVE_REMOVED && e4.back().timbres_imported() == 0 && e4.back().checkpoints() == 0);
  auto libs4 = libs_of(c);
  CHECK(libs4[key].bundle == libs3[key].bundle && libs4[key].instruments == 5 && current_of(svc.data(), first.id) == libs3[key].bundle);
  CHECK(bundle_verifies(svc.data() + "/lib/" + first.id + "/" + libs3[key].bundle + "/bundle.vlb") && staging_clean(svc.data()));
}

// --- CLI -----------------------------------------------------------------------------------------------------------------------------------------

int run_shell(const std::string& cmd, std::string* out = nullptr, std::string* err = nullptr) {
  TempDir t;
  // The shell's own stderr goes to the file too, so its "Killed: 9" notice of a SIGKILLed child does not clutter the test log.
  const std::string full = "exec 2>" + t.sub("e.txt") + "; " + cmd + " > " + t.sub("o.txt");
  const int rc = std::system(full.c_str());
  if (out) *out = ltest::slurp(t.sub("o.txt"));
  if (err) *err = ltest::slurp(t.sub("e.txt"));
  if (WIFSIGNALED(rc)) return 128 + WTERMSIG(rc);
  return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

void test_cli() {
  TempDir t;
  fs::create_directories(t.sub("src"));
  const fs::path sf2 = ifx::write_many_sf2(fs::path(t.sub("src")) / "m.sf2", 7);
  const std::string lib = lib_id_of(sf2);
  std::string out, err;
  const std::string cli = LIBRARIAN_EXE, tcli = LIBRARIAN_TEST_EXE;
  // The flag: checkpoints happen (more bundle directories than the one final bundle), the result equals the default import.
  fs::create_directories(t.sub("r1"));
  CHECK(run_shell(cli + " import " + sf2.string() + " --root " + t.sub("r1") + " --checkpoint-timbres 2", &out, &err) == 0);
  CHECK(bundle_dirs(t.sub("r1"), lib) == 1);  // checkpoints at 2, 4, 6 were each superseded; the final bundle is left
  fs::create_directories(t.sub("r2"));
  CHECK(run_shell(cli + " import " + sf2.string() + " --root " + t.sub("r2"), &out, &err) == 0);
  CHECK(bundle_dirs(t.sub("r2"), lib) == 1 && current_of(t.sub("r1"), lib) == current_of(t.sub("r2"), lib));
  fs::create_directories(t.sub("r3"));
  CHECK(run_shell(cli + " import " + sf2.string() + " --root " + t.sub("r3") + " --checkpoint-bytes 1", &out, &err) == 0);
  CHECK(bundle_dirs(t.sub("r3"), lib) == 1 && current_of(t.sub("r3"), lib) == current_of(t.sub("r2"), lib));  // every timbre exceeds one byte
  // Bad values are usage errors and write nothing.
  for (const char* bad : {"--checkpoint-timbres 0", "--checkpoint-timbres x", "--checkpoint-timbres -3", "--checkpoint-bytes 0", "--checkpoint-bytes 1k", "--checkpoint-timbres 99999999999"}) {
    fs::create_directories(t.sub("r4"));
    CHECK(run_shell(cli + " import " + sf2.string() + " --root " + t.sub("r4") + " " + bad, &out, &err) == 2);
    CHECK(!fs::exists(fs::path(t.sub("r4")) / "lib" / lib));
    fs::remove_all(t.sub("r4"));
  }
  // The test CLI: SIGKILL at the n-th arrival at a point (a real kill of a real process), and the source vanishing.
  fs::create_directories(t.sub("r5"));
  CHECK(run_shell("VELORA_LIBRARIAN_KILL=checkpoint-published@2 " + tcli + " import " + sf2.string() + " --root " + t.sub("r5") + " --checkpoint-timbres 2") == 128 + SIGKILL);
  CHECK(keys_of(fs::path(t.sub("r5")) / "lib" / lib / current_of(t.sub("r5"), lib) / "bundle.vlb").size() == 4);
  CHECK(bundle_dirs(t.sub("r5"), lib) == 1);  // the checkpoint of 2 was already superseded when the process died
  fs::create_directories(t.sub("r6"));
  CHECK(run_shell("VELORA_LIBRARIAN_SOURCE_GONE=timbre-start@4 " + tcli + " import " + sf2.string() + " --root " + t.sub("r6") + " --checkpoint-timbres 2", &out, &err) == 1);
  CHECK(err.find("drive removed") != std::string::npos);
  CHECK(keys_of(fs::path(t.sub("r6")) / "lib" / lib / current_of(t.sub("r6"), lib) / "bundle.vlb").size() == 3 && staging_clean(t.sub("r6")));
  // The batch stops at the removed file: the next file is not attempted.
  fs::create_directories(t.sub("src2"));
  ifx::write_many_sf2(fs::path(t.sub("src2")) / "a.sf2", 4);
  ifx::write_sfz(t.sub("src2"), "zz");
  fs::create_directories(t.sub("r7"));
  CHECK(run_shell("VELORA_LIBRARIAN_SOURCE_GONE=timbre-start@2 " + tcli + " import " + t.sub("src2") + " --root " + t.sub("r7"), &out, &err) == 1);
  CHECK(out.find("zz.sfz") == std::string::npos && err.find("zz.sfz") == std::string::npos);
}


// --- Story 5.8 review round ----------------------------------------------------------------------------------------------------------------------

// A byte source over the bytes of a file that fails every read with `mode` (1 = kIo, 2 = kTruncated) once armed.
class FaultSrc final : public fmt::ByteSource {
 public:
  FaultSrc(std::string bytes, std::shared_ptr<std::atomic<int>> mode) : b_(std::move(bytes)), mode_(std::move(mode)) {}
  std::uint64_t size() const override { return b_.size(); }
  const std::uint8_t* direct(std::uint64_t, std::size_t) const override { return nullptr; }
  fmt::Error read_at(std::uint64_t off, void* dst, std::size_t n) const override {
    if (mode_->load() == 1) return fmt::Error::kIo;
    if (mode_->load() == 2) return fmt::Error::kTruncated;
    if (off > b_.size() || n > b_.size() - off) return fmt::Error::kTruncated;
    std::memcpy(dst, b_.data() + off, n);
    return fmt::Error::kNone;
  }

 private:
  std::string b_;
  std::shared_ptr<std::atomic<int>> mode_;
};

std::function<std::shared_ptr<const fmt::ByteSource>(const std::string&)> fault_opener(const std::shared_ptr<std::atomic<int>>& mode) {
  return [mode](const std::string& path) -> std::shared_ptr<const fmt::ByteSource> { return std::make_shared<FaultSrc>(ltest::slurp(path), mode); };
}

// Removal in the middle of a timbre that has 3 samples: the probe is polled once at the start of a timbre and once after each sample read, so the 11th look is the
// 2nd sample of timbre 3. The timbres before it are published, the half-read one leaves nothing (not even in the staging area), and the result is exactly the bundle of
// a SoundFont that only ever had those two timbres.
void test_removal_in_the_second_sample_of_a_timbre() {
  TempDir t;
  fs::create_directories(t.sub("src"));
  fs::create_directories(t.sub("root"));
  fs::create_directories(t.sub("root2"));
  const fs::path sf2 = ifx::write_many_sf2(fs::path(t.sub("src")) / "m.sf2", 5, 0, 2200, 3);
  const Run run = run_import(sf2, t.sub("root"), 0, 0, 0, [](ImportOptions& o) {
    auto looks = std::make_shared<int>(0);
    o.source_gone = [looks] { return ++*looks >= 11; };
  });
  CHECK(run.r.status == Status::kDriveRemoved && run.r.timbres_published == 2 && run.r.timbres_total == 5);
  CHECK(keys_of(run.r.bundle_dir + "/bundle.vlb") == (std::vector<std::string>{"0:0", "0:1"}) && bundle_verifies(run.r.bundle_dir + "/bundle.vlb"));
  CHECK(staging_clean(t.sub("root")) && bundle_dirs(t.sub("root"), run.r.library_id) == 1);
  ifx::write_many_sf2(sf2, 2, 0, 2200, 3);  // the same file with only the first two timbres
  const Run prefix = run_import(sf2, t.sub("root2"), 0);
  CHECK(prefix.r.ok() && prefix.r.bundle_id == run.r.bundle_id);
}

// I/O errors and short reads of the source. A read error on a drive that still answers (the stronger probe: folder listing and a re-read of the file) fails ONLY that
// item; the injected probe turns the same error into a drive removal. A short read is `damaged` unless the probe says the source is gone.
void test_io_error_classification() {
  struct Case {
    int mode;        // 1 kIo, 2 kTruncated
    bool probe;      // the injected probe says gone once the fault is armed
    Status want;
  };
  const Case cases[] = {{1, false, Status::kIo}, {1, true, Status::kDriveRemoved}, {2, false, Status::kSkipped}, {2, true, Status::kDriveRemoved}};
  for (const Case& c : cases) {
    TempDir t;
    fs::create_directories(t.sub("src"));
    fs::create_directories(t.sub("root"));
    const fs::path sf2 = ifx::write_many_sf2(fs::path(t.sub("src")) / "m.sf2", 5);
    auto mode = std::make_shared<std::atomic<int>>(0);
    auto calls = std::make_shared<int>(-1);  // < 0: not armed
    const Run run = run_import(sf2, t.sub("root"), 2, 0, 0, [&](ImportOptions& o) {
      o.fault.open_sf2 = fault_opener(mode);
      auto prev = o.fault.on_timbre;
      auto starts = std::make_shared<int>(0);
      o.fault.on_timbre = [=](const char* p, std::size_t i) {
        prev(p, i);
        if (std::string(p) == "timbre-start" && ++*starts == 3) {  // the third timbre: the first two are checkpointed
          mode->store(c.mode);
          *calls = 0;
        }
        return HookAction::kContinue;
      };
      // Look 1 is the start of the timbre, look 2 follows the failing read.
      const int yes_at = c.mode == 1 ? 3 : 2;  // an I/O error is confirmed by the stronger probe (look 3); a short read only by the plain one (look 2)
      o.source_gone = [=] { return c.probe && *calls >= 0 && ++*calls >= yes_at; };
    });
    if (run.r.status != c.want) std::printf("case mode %d probe %d: status %s\n", c.mode, int(c.probe), to_string(run.r.status));
    CHECK(run.r.status == c.want);
    CHECK(run.r.timbres_published == 2 && run.r.checkpoints == 1 && keys_of(run.r.bundle_dir + "/bundle.vlb").size() == 2);  // the checkpoint stays
    CHECK(current_of(t.sub("root"), lib_id_of(sf2)) == run.r.bundle_id && staging_clean(t.sub("root")));
    if (c.want == Status::kSkipped) CHECK(run.r.reason_code == ReasonCode::kDamaged);
    if (c.want == Status::kIo) CHECK(!run.r.reasons.empty() && run.r.reasons.back().find("I/O error") != std::string::npos);
  }
  // At service level an unconfirmed I/O error fails that item and the job goes on with the next one.
  std::atomic<int> starts{0};
  auto mode = std::make_shared<std::atomic<int>>(0);
  Svc svc([&](LibraryServiceOptions& o) {
    o.checkpoint_timbres = 2;
    o.import_timbre_hook = [&](const char* p, std::uint64_t, std::size_t) {
      if (std::string(p) == "timbre-start" && ++starts == 3) mode->store(1);
    };
    o.import_options_hook = [&](std::uint64_t, std::size_t item, ImportOptions& io) {
      if (item == 0) io.fault.open_sf2 = fault_opener(mode);
    };
  });
  CHECK(svc.start());
  TempDir src;
  const fs::path many = ifx::write_many_sf2(fs::path(src.path) / "many.sf2", 5), b = ifx::write_sfz(src.path, "B");
  Client c;
  CHECK(svc.connect(c));
  const auto job = submit(c, src.path, {{many.filename().string()}, {b.filename().string()}});
  const auto ev = collect(c, job);
  CHECK(!ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_PUBLISHED && ev.back().items_failed() == 1 && ev.back().items_imported() == 1);
  bool seen = false;
  for (const auto& e : ev)
    if (e.has_item() && e.item().status() == ws::v1::IMPORT_ITEM_FAILED) {
      seen = true;
      CHECK(e.item().timbres() == 2 && e.item().partial());  // the checkpoint's two timbres stay
    }
  CHECK(seen);
  mode->store(0);
  CHECK(libs_of(c).size() == 2);
}

// The partial-library contract: an item that ends cancelled / skipped (damaged) / failed (verification) AFTER a checkpoint keeps that checkpoint's timbres published and
// cataloged and says so (`timbres` and `partial`).
void test_partial_library_contract() {
  struct Case {
    const char* name;
    ws::v1::ImportItemStatus status;
    ws::v1::ImportState state;
  };
  for (int which = 0; which < 3; ++which) {
    std::atomic<int> starts{0};
    auto mode = std::make_shared<std::atomic<int>>(0);
    Client probe;
    Svc svc;
    svc.o.checkpoint_timbres = 2;
    svc.o.import_timbre_hook = [&](const char* p, std::uint64_t job, std::size_t) {
      if (std::string(p) != "timbre-start" || ++starts != 4) return;  // the 4th timbre: the checkpoint of 2 exists
      if (which == 0) {
        ws::v1::LibraryClientMessage m;
        m.mutable_cancel_import()->set_job_id(job);
        probe.call(m);
      } else if (which == 1) {
        mode->store(2);  // a short read: damaged
      }
    };
    svc.o.import_options_hook = [&](std::uint64_t, std::size_t, ImportOptions& io) {
      if (which == 1) io.fault.open_sf2 = fault_opener(mode);
      if (which == 2) {
        io.fault.corrupt_staged_before_verify = true;
        io.fault.corrupt_round = 2;  // the second checkpoint (timbres 1..4) fails its verification
      }
    };
    CHECK(svc.start());
    TempDir src;
    const fs::path many = ifx::write_many_sf2(fs::path(src.path) / "many.sf2", 7);
    Client c;
    CHECK(svc.connect(c) && probe.connect(svc.o.socket_path) && probe.handshake("probe"));
    const auto job = submit(c, many.string(), {{""}});
    const auto ev = collect(c, job);
    const ws::v1::ImportItemResult* item = nullptr;
    for (const auto& e : ev)
      if (e.has_item()) item = &e.item();
    CHECK(!ev.empty() && item != nullptr);
    if (!item || ev.empty()) continue;
    const std::size_t want_timbres = which == 2 ? 2 : 2;  // the first checkpoint (2) in every case
    CHECK(item->timbres() == want_timbres && item->partial());
    if (which == 0) CHECK(item->status() == ws::v1::IMPORT_ITEM_CANCELLED && ev.back().state() == ws::v1::IMPORT_STATE_CANCELLED);
    if (which == 1) CHECK(item->status() == ws::v1::IMPORT_ITEM_SKIPPED && item->reason() == ws::v1::IMPORT_REASON_DAMAGED && ev.back().state() == ws::v1::IMPORT_STATE_FAILED);
    if (which == 2) CHECK(item->status() == ws::v1::IMPORT_ITEM_FAILED && ev.back().state() == ws::v1::IMPORT_STATE_FAILED);
    CHECK(ev.back().timbres_imported() == 2 && ev.back().checkpoints() == 1);
    auto libs = libs_of(c);
    CHECK(libs.size() == 1 && libs.begin()->second.instruments == 2);  // the prefix is cataloged and playable
    if (!libs.empty()) {
      CHECK(bundle_verifies(svc.data() + "/lib/" + libs.begin()->second.id + "/" + libs.begin()->second.bundle + "/bundle.vlb"));
      CHECK(bundle_dirs(svc.data(), libs.begin()->second.id) == 1);
    }
    CHECK(staging_clean(svc.data()));
  }
}

// A checkpoint whose catalog write fails is reported in the next progress event (`checkpoint_error`), retried by the next checkpoint, and the library ends complete.
void test_checkpoint_catalog_failure_is_reported_and_retried() {
  Svc svc;
  svc.o.checkpoint_timbres = 2;
  int writes = 0;
  svc.fault_hook = [&](const char* point) { return std::string(point) == "library.before_commit" && ++writes == 1; };  // the first library write fails
  CHECK(svc.start());
  TempDir src;
  const fs::path many = ifx::write_many_sf2(fs::path(src.path) / "many.sf2", 7);
  Client c;
  CHECK(svc.connect(c));
  const auto ev = collect(c, submit(c, many.string(), {{""}}));
  CHECK(!ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_PUBLISHED && ev.back().checkpoint_error().empty());
  bool reported = false;
  std::uint32_t counted_at_error = 99;
  for (const auto& e : ev)
    if (!e.checkpoint_error().empty()) {
      reported = true;
      counted_at_error = e.timbres_imported();
    }
  CHECK(reported && counted_at_error == 0);  // not counted as imported while its row is missing
  CHECK(ev.back().timbres_imported() == 7);
  auto libs = libs_of(c);
  CHECK(libs.size() == 1 && libs.begin()->second.instruments == 7);
}

// A one-timbre checkpoint of a multi-timbre file is named like the final bundle (the file stem), not after its only instrument.
void test_single_timbre_checkpoint_keeps_the_library_name() {
  std::vector<std::string> names;
  Client probe;
  Svc svc;
  svc.o.checkpoint_timbres = 1;
  svc.o.import_timbre_hook = [&](const char* p, std::uint64_t, std::size_t) {
    if (std::string(p) != "checkpoint-published") return;
    auto r = list_libs(probe);
    if (r && r->has_library_list() && r->library_list().libraries_size() == 1) names.push_back(r->library_list().libraries(0).name());
  };
  CHECK(svc.start());
  TempDir src;
  const fs::path many = ifx::write_many_sf2(fs::path(src.path) / "many.sf2", 3);
  Client c;
  CHECK(svc.connect(c) && probe.connect(svc.o.socket_path) && probe.handshake("probe"));
  collect(c, submit(c, many.string(), {{""}}));
  CHECK(names.size() == 2 && names[0] == "many" && names[1] == "many");
  auto r = list_libs(c);
  CHECK(r && r->library_list().libraries_size() == 1 && r->library_list().libraries(0).name() == "many");
}

// A client that did not list `import.checkpoints.v1` never sees DRIVE_REMOVED: FAILED with `message` = drive_removed, and the item FAILED.
void test_old_client_sees_failed() {
  std::atomic<int> starts{0};
  Svc svc([&](LibraryServiceOptions& o) {
    o.checkpoint_timbres = 2;
    auto gone = std::make_shared<std::atomic<bool>>(false);
    o.source_gone = [gone](const std::string&) { return gone->load(); };
    o.import_timbre_hook = [&starts, gone](const char* p, std::uint64_t, std::size_t) {
      if (std::string(p) == "timbre-start" && ++starts == 4) gone->store(true);
    };
  });
  CHECK(svc.start());
  TempDir src;
  const fs::path many = ifx::write_many_sf2(fs::path(src.path) / "many.sf2", 6);
  Client c;
  CHECK(c.connect(svc.o.socket_path));
  ws::v1::LibraryClientMessage hello;
  hello.mutable_hello()->set_version(1);  // no features: an old client
  auto ack = c.call(hello);
  CHECK(ack && ack->has_hello_ack());
  const auto job = submit(c, many.string(), {{""}});
  const auto ev = collect(c, job);
  CHECK(!ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_FAILED && ev.back().message() == "drive_removed");
  for (const auto& e : ev) {
    CHECK(e.state() != ws::v1::IMPORT_STATE_DRIVE_REMOVED);
    if (e.has_item()) CHECK(e.item().status() != ws::v1::IMPORT_ITEM_DRIVE_REMOVED && e.item().reason() != ws::v1::IMPORT_REASON_DRIVE_REMOVED);
  }
  ws::v1::LibraryClientMessage sm;
  sm.mutable_import_status()->set_job_id(job);
  auto st = c.call(sm);
  CHECK(st && st->has_import_progress() && st->import_progress().state() == ws::v1::IMPORT_STATE_FAILED);
  Client modern;
  CHECK(svc.connect(modern));
  ws::v1::LibraryClientMessage sm2;
  sm2.mutable_import_status()->set_job_id(job);
  auto st2 = modern.call(sm2);
  CHECK(st2 && st2->has_import_progress() && st2->import_progress().state() == ws::v1::IMPORT_STATE_DRIVE_REMOVED);
}

// Removal twice (the retry meets the removal again), then a preflight of the volume (the published timbres are `already imported` with the default action Skip, the rest
// is ready) and Import again of the file, which completes the library without checkpoints. This is the user's way back after a pulled drive.
void test_removal_retry_preflight_and_remainder() {
  std::atomic<int> starts{0}, arm_at{4};
  auto gone = std::make_shared<std::atomic<bool>>(false);
  Svc svc([&](LibraryServiceOptions& o) {
    o.checkpoint_timbres = 2;
    o.source_gone = [gone](const std::string&) { return gone->load(); };
    o.import_timbre_hook = [&, gone](const char* p, std::uint64_t, std::size_t) {
      if (std::string(p) == "timbre-start" && ++starts == arm_at.load()) gone->store(true);
    };
  });
  CHECK(svc.start());
  TempDir src;
  fs::create_directories(fs::path(src.path) / "vol");
  const fs::path many = ifx::write_many_sf2(fs::path(src.path) / "vol" / "many.sf2", 6);
  const std::string vol = fs::canonical(fs::path(src.path) / "vol").string(), key = vol + "/many.sf2";
  Client c;
  CHECK(svc.connect(c));
  auto ev = collect(c, submit(c, vol, {{"many.sf2"}}));
  CHECK(!ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_DRIVE_REMOVED && ev.back().timbres_imported() == 3);
  const LibRow first = libs_of(c)[key];
  CHECK(first.instruments == 3);
  // The drive is back, the retry is pulled again (a library that is already published is not checkpointed: nothing new is published, the 3 timbres stay).
  gone->store(false);
  starts = 0;
  arm_at = 3;
  ev = collect(c, submit(c, vol, {{"many.sf2"}}));
  CHECK(!ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_DRIVE_REMOVED && ev.back().timbres_imported() == 0 && ev.back().checkpoints() == 0);
  const LibRow second = libs_of(c)[key];
  CHECK(second.bundle == first.bundle && second.instruments == 3 && current_of(svc.data(), first.id) == first.bundle);
  // Preflight: the published timbres are already imported (default Skip), the others are not.
  gone->store(false);
  arm_at = -1;
  ws::v1::LibraryClientMessage pm;
  pm.mutable_preflight_request()->set_source(vol);
  pm.mutable_preflight_request()->set_volume_label("VOL");
  auto pd = c.call(pm, 30000);
  CHECK(pd && pd->has_preflight_done());
  if (pd && pd->has_preflight_done()) {
    ws::v1::LibraryClientMessage lm;
    lm.mutable_list_preflight_rows()->set_preflight_id(pd->preflight_done().preflight_id());
    auto rows = c.call(lm, 30000);
    CHECK(rows && rows->has_preflight_rows() && rows->preflight_rows().rows_size() == 6);
    if (rows && rows->has_preflight_rows())
      for (const auto& r : rows->preflight_rows().rows()) {
        const bool published = r.instrument_key() == "0:0" || r.instrument_key() == "0:1" || r.instrument_key() == "0:2";
        CHECK(published == (r.status() == ws::v1::PREFLIGHT_STATUS_ALREADY_IMPORTED));
        CHECK(published == (r.action() == ws::v1::IMPORT_ACTION_SKIP));
      }
  }
  // Import again completes the library; the result is the clean import.
  ev = collect(c, submit(c, vol, {{"many.sf2", kPiano, ws::v1::IMPORT_ACTION_IMPORT}}));
  CHECK(!ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_PUBLISHED && ev.back().checkpoints() == 0);
  const LibRow done = libs_of(c)[key];
  CHECK(done.instruments == 6 && done.bundle == clean_bundle_id(many) && current_of(svc.data(), first.id) == done.bundle);
  CHECK(bundle_dirs(svc.data(), first.id) == 2);  // the partial bundle of the first run stays (a lease), the complete one is current
}

// The shipped binaries: the real librarian-service takes --checkpoint-timbres / --checkpoint-bytes, and rejects bad values; the shipped librarian has no kill hook.
int spawn_service(const std::string& data, const std::string& sock, const std::vector<std::string>& extra, bool wait_exit, pid_t* pid_out) {
  const pid_t pid = ::fork();
  if (pid == 0) {
    std::vector<std::string> a = {"librarian-service", "--data", data, "--socket", sock};
    a.insert(a.end(), extra.begin(), extra.end());
    std::vector<char*> argv;
    for (auto& x : a) argv.push_back(x.data());
    argv.push_back(nullptr);
    const int dn = ::open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      ::dup2(dn, 1);
      ::dup2(dn, 2);
    }
    ::execv(LIBRARIAN_SERVICE_EXE, argv.data());
    ::_exit(127);
  }
  *pid_out = pid;
  if (!wait_exit) return 0;
  int st = 0;
  ::waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

void test_service_binary_flags() {
  TempDir t;
  fs::create_directories(t.sub("data"));
  for (const char* bad : {"0", "x", "-1", "99999999999"}) {
    pid_t pid = 0;
    CHECK(spawn_service(t.sub("data"), t.sub("s.sock"), {"--checkpoint-timbres", bad}, true, &pid) == 2);
  }
  for (const char* bad : {"0", "x", "-1"}) {
    pid_t pid = 0;
    CHECK(spawn_service(t.sub("data"), t.sub("s.sock"), {"--checkpoint-bytes", bad}, true, &pid) == 2);
  }
  const fs::path many = ifx::write_many_sf2(fs::path(t.sub("data")).parent_path() / (fs::path(t.path).filename().string() + "_m") / "m.sf2", 7);
  struct Cleanup {
    fs::path p;
    ~Cleanup() { std::error_code ec; fs::remove_all(p, ec); }
  } cleanup{many.parent_path()};
  for (int variant = 0; variant < 2; ++variant) {
    const std::string data = t.sub("d" + std::to_string(variant)), sock = t.sub("v" + std::to_string(variant) + ".sock");
    fs::create_directories(data);
    pid_t pid = 0;
    spawn_service(data, sock, variant == 0 ? std::vector<std::string>{"--checkpoint-timbres", "2"} : std::vector<std::string>{"--checkpoint-bytes", "1"}, false, &pid);
    Client c;
    CHECK(ltest::wait_until([&] { return c.connect(sock); }, 20000) && c.handshake("flags"));
    const auto ev = collect(c, submit(c, many.string(), {{""}}));
    CHECK(!ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_PUBLISHED);
    if (!ev.empty()) CHECK(ev.back().checkpoints() == (variant == 0 ? 3u : 6u) && ev.back().timbres_imported() == 7);  // 2 timbres: 2,4,6; 1 byte: after each of the first 6
    ::kill(pid, SIGTERM);
    int st = 0;
    ::waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  }
}

void test_shipped_binary_has_no_kill_hook() {
  TempDir t;
  fs::create_directories(t.sub("src"));
  fs::create_directories(t.sub("root"));
  const fs::path sf2 = ifx::write_many_sf2(fs::path(t.sub("src")) / "m.sf2", 4);
  const std::string cli = LIBRARIAN_EXE;
  CHECK(run_shell("VELORA_LIBRARIAN_KILL=timbre-start@1 VELORA_LIBRARIAN_SOURCE_GONE=timbre-start@2 " + cli + " import " + sf2.string() + " --root " + t.sub("root")) == 0);
  CHECK(keys_of(fs::path(t.sub("root")) / "lib" / lib_id_of(sf2) / current_of(t.sub("root"), lib_id_of(sf2)) / "bundle.vlb").size() == 4);
}

// Reconcile after a kill removes what the checkpoint protocol left behind, and only that.
void test_reconcile_prunes_checkpoint_leftovers() {
  TempDir t;
  fs::create_directories(t.sub("src"));
  fs::create_directories(t.sub("root"));
  const fs::path sf2 = ifx::write_many_sf2(fs::path(t.sub("src")) / "m.sf2", 6);
  const std::string lib = lib_id_of(sf2);
  // A kill at `renamed` of the 2nd checkpoint: the 1st checkpoint is current, the 2nd bundle is renamed in place but not current.
  std::string out, err;
  CHECK(run_shell(std::string("VELORA_LIBRARIAN_KILL=renamed@2 ") + LIBRARIAN_TEST_EXE + " import " + sf2.string() + " --root " + t.sub("root") + " --category piano --checkpoint-timbres 2") ==
        128 + SIGKILL);
  CHECK(bundle_dirs(t.sub("root"), lib) == 2);
  // An unrelated old bundle (no marker: a lease of an earlier Import again) must survive.
  ifx::write_many_sf2(sf2, 6, 1);
  const Run second = run_import(sf2, t.sub("root"), 0);
  CHECK(second.r.ok());
  const std::size_t before = bundle_dirs(t.sub("root"), lib);
  CHECK(before == 3);  // checkpoint, orphan, complete
  std::unique_ptr<Catalog> cat;
  CHECK(Catalog::open(t.sub("root"), cat) == CatalogStatus::kOk);
  ReconcileReport rep;
  CHECK(reconcile_libraries(*cat, t.sub("root"), rep) == CatalogStatus::kOk && rep.errors.empty());
  CHECK(rep.bundles_pruned == 2 && bundle_dirs(t.sub("root"), lib) == 1 && current_of(t.sub("root"), lib) == second.r.bundle_id);
  ReconcileReport again;
  CHECK(reconcile_libraries(*cat, t.sub("root"), again) == CatalogStatus::kOk && !again.changed());
}

// --- env-gated: a real disk image detached in the middle of an import (macOS, no root) ----------------------------------------------------------------

int test_disk_image() {
#ifdef __APPLE__
  const char* gate = std::getenv("VELORA_TEST_DISK_IMAGE");
  if (!gate || std::string(gate) != "1") {
    std::printf("disk image test: skipped (set VELORA_TEST_DISK_IMAGE=1 to detach a hdiutil image mid-import)\n");
    return 0;
  }
  if (run_shell("command -v hdiutil") != 0) {
    std::printf("disk image test: skipped (no hdiutil)\n");
    return 0;
  }
  TempDir t;
  const std::string dmg = t.sub("vol.dmg"), mnt = t.sub("mnt");
  fs::create_directories(mnt);
  if (run_shell("hdiutil create -size 24m -fs HFS+ -volname VLRIMG -ov " + dmg) != 0 ||
      run_shell("hdiutil attach -nobrowse -noverify -mountpoint " + mnt + " " + dmg) != 0) {
    std::printf("disk image test: skipped (hdiutil could not create or attach an image here)\n");
    return 0;
  }
  struct Detach {
    std::string mnt;
    ~Detach() { run_shell("hdiutil detach -force " + mnt + " > /dev/null 2>&1"); }
  } guard{mnt};
  const fs::path a = ifx::write_sfz(mnt, "A"), c_sfz = ifx::write_sfz(mnt, "C");
  const fs::path many = ifx::write_many_sf2(fs::path(mnt) / "many.sf2", 8, 0, 20000);
  std::atomic<int> starts{0};
  Svc svc([&](LibraryServiceOptions& o) {
    o.checkpoint_timbres = 2;
    o.import_timbre_hook = [&](const char* p, std::uint64_t, std::size_t) {
      if (std::string(p) == "timbre-start" && ++starts == 6) run_shell("hdiutil detach -force " + mnt);  // the 5th timbre of the SoundFont
    };
  });
  CHECK(svc.start());
  Client c;
  CHECK(svc.connect(c));
  const auto job = submit(c, fs::canonical(mnt).string(), {{"A.sfz"}, {"many.sf2"}, {"C.sfz"}});
  CHECK(job != 0);
  const auto ev = collect(c, job);
  CHECK(!ev.empty() && ev.back().state() == ws::v1::IMPORT_STATE_DRIVE_REMOVED);
  if (!ev.empty()) CHECK(ev.back().items_imported() == 1 && ev.back().timbres_imported() == 1 + 4 && ev.back().timbres_total() == 10);
  CHECK(staging_clean(svc.data()));
  auto libs = libs_of(c);
  CHECK(libs.size() == 2);
  for (const auto& kv : libs) CHECK(bundle_verifies(svc.data() + "/lib/" + kv.second.id + "/" + kv.second.bundle + "/bundle.vlb"));
  std::printf("disk image test: done\n");
#endif
  return 0;
}

}  // namespace

int main() {
  ::signal(SIGPIPE, SIG_IGN);
  test_checkpoint_boundaries();
  test_no_checkpoint_when_published();
  test_removal_before_first_timbre_finishes();
  test_removal_after_checkpoint();
  test_removal_before_first_checkpoint_publishes_finished();
  test_removal_during_import_again();
  test_removal_sfz();
  test_abort_between_timbres_keeps_checkpoints();
  test_import_again_replaces_in_place();
  test_service_checkpoints();
  test_service_drive_removed(true);
  test_service_drive_removed(false);
  test_service_removed_before_anything();
  test_service_import_again();
  test_cli();
  test_removal_in_the_second_sample_of_a_timbre();
  test_io_error_classification();
  test_partial_library_contract();
  test_checkpoint_catalog_failure_is_reported_and_retried();
  test_single_timbre_checkpoint_keeps_the_library_name();
  test_old_client_sees_failed();
  test_removal_retry_preflight_and_remainder();
  test_service_binary_flags();
  test_shipped_binary_has_no_kill_hook();
  test_reconcile_prunes_checkpoint_leftovers();
  test_disk_image();
  std::printf(g_fail ? "librarian_checkpoint_test: %d FAILED\n" : "librarian_checkpoint_test: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
