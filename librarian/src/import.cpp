#include "velora/librarian/import.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <dirent.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>

#include "velora/format/bundle.hpp"
#include "velora/format/category.hpp"
#include "velora/format/hash.hpp"
#include "velora/format/sf2_import.hpp"
#include "velora/format/sfz_import.hpp"
#include "velora/format/sysex_6op.hpp"
#include "velora/format/parametric_bundle.hpp"
#include "velora/format/wav_source.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/librarian/category_suggest.hpp"
#include "velora/librarian/library_index.hpp"

namespace velora::librarian {
namespace fs = std::filesystem;
namespace fmt = velora::format;

std::string category_problem(const std::string& category) {
  if (category.empty()) return {};
  if (category.size() > 128) return "category too long";
  for (unsigned char c : category)
    if (c < 0x20 || c == 0x7f) return "invalid category";
  const auto parsed = fmt::parse_category(category);
  if (!parsed) return "unknown category '" + category + "' (use a key such as category.piano or an English name)";
  if (!fmt::is_assignable(*parsed)) return "'none' is not a category an instrument can carry";
  return {};
}

const char* to_string(Stage s) {
  switch (s) {
    case Stage::kStaged: return "staged";
    case Stage::kVerified: return "verified";
    case Stage::kFsynced: return "fsynced";
    case Stage::kRenamed: return "renamed";
    case Stage::kLinkCreated: return "link-created";
    case Stage::kFlipped: return "flipped";
    case Stage::kStageCreated: return "stage-created";
  }
  return "?";
}

bool parse_stage(const std::string& name, Stage& out) {
  for (int i = 0; i <= static_cast<int>(Stage::kStageCreated); ++i)
    if (name == to_string(static_cast<Stage>(i))) {
      out = static_cast<Stage>(i);
      return true;
    }
  return false;
}

const char* to_string(ReasonCode r) {
  switch (r) {
    case ReasonCode::kNone: return "none";
    case ReasonCode::kUnsupportedFeatures: return "unsupported_features";
    case ReasonCode::kDamaged: return "damaged";
  }
  return "?";
}

Status status_for_add_sample_error(fmt::Error e, ReasonCode* reason) {
  if (reason) *reason = ReasonCode::kNone;
  switch (e) {
    case fmt::Error::kInvalidArgument: return Status::kInvalidArgument;
    case fmt::Error::kEncoderFailure: return Status::kIo;
    case fmt::Error::kUnsupportedFormat:
      if (reason) *reason = ReasonCode::kUnsupportedFeatures;
      return Status::kSkipped;
    default:
      if (reason) *reason = ReasonCode::kDamaged;
      return Status::kSkipped;
  }
}

const char* to_string(Status s) {
  switch (s) {
    case Status::kOk: return "ok";
    case Status::kSkipped: return "skipped";
    case Status::kInvalidArgument: return "invalid argument";
    case Status::kVerifyFailed: return "verify failed";
    case Status::kIo: return "I/O error";
    case Status::kCrossDevice: return "staging on another filesystem";
    case Status::kBusy: return "another import is running";
    case Status::kAborted: return "aborted";
    case Status::kDriveRemoved: return "drive removed";
    case Status::kFactoryProtected: return "factory_library_protected";
  }
  return "?";
}

namespace {

struct Fd {
  int fd = -1;
  Fd() = default;
  explicit Fd(int f) : fd(f) {}
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  ~Fd() { if (fd >= 0) ::close(fd); }
  bool ok() const { return fd >= 0; }
};

bool full_sync(int fd) {
#ifdef __APPLE__
  if (::fcntl(fd, F_FULLFSYNC) == 0) return true;  // falls back to fsync on file systems without it
#endif
  return ::fsync(fd) == 0;
}

bool fsync_path(const std::string& path, bool dir) {
  Fd f(::open(path.c_str(), dir ? (O_RDONLY | O_DIRECTORY) : O_RDONLY));
  return f.ok() && full_sync(f.fd);
}

// mkdir; when the directory is new, fsync its parent so the entry survives a crash.
bool mkdir_synced(const std::string& path, const std::string& parent) {
  if (::mkdir(path.c_str(), 0755) == 0) return fsync_path(parent, true);
  return errno == EEXIST;
}

std::string err_text(const std::string& what) { return what + ": " + std::strerror(errno); }

bool is_dir(const std::string& p) {
  struct stat st;
  return ::lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool same_device(const std::string& a, const std::string& b) {
  struct stat sa, sb;
  return ::stat(a.c_str(), &sa) == 0 && ::stat(b.c_str(), &sb) == 0 && sa.st_dev == sb.st_dev;
}

// A problem found while loading a sample.
struct AudioProblem {
  std::string text;      // "ref: path: why"
  ReasonCode code = ReasonCode::kDamaged;
  std::string path;
  std::string ref_file;
  std::size_t ref_line = 0;
};

bool has_wav_extension(const std::string& path) {
  std::string ext = fs::path(path).extension().string();
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext == ".wav";
}

// A sample format outside the supported set (or a non-WAV container such as .flac) is a feature
// the importer does not have; a WAV that cannot be read or is truncated is damaged.
AudioProblem audio_problem(const std::string& path, fmt::Error e, const fmt::SfzSample& s) {
  AudioProblem p;
  p.path = path;
  p.ref_file = s.ref_file;
  p.ref_line = s.ref_line;
  std::string why;
  switch (e) {
    case fmt::Error::kUnsupportedFormat:
      why = "unsupported audio format (need WAV: 16/24-bit PCM or 32-bit float, mono/stereo, "
            "44.1/48/88.2/96 kHz; the importer never resamples or requantizes)";
      p.code = ReasonCode::kUnsupportedFeatures;
      break;
    case fmt::Error::kBadMagic:
      why = "unsupported audio: not a WAV file";
      p.code = has_wav_extension(path) ? ReasonCode::kDamaged : ReasonCode::kUnsupportedFeatures;
      break;
    case fmt::Error::kIo: why = "cannot read sample file"; break;
    default: why = std::string("damaged WAV file (") + fmt::to_string(e) + ")"; break;
  }
  p.text = s.ref_file + ":" + std::to_string(s.ref_line) + ": " + path + ": " + why;
  return p;
}

// A WAV whose data chunk is cut off is damaged, not a shorter sample.
constexpr fmt::WavOptions kStrictWav{true};

std::atomic<std::uint64_t> g_counter{0};

// Frames of `a` and `b` are equal bit for bit.
bool sample_matches(const fmt::BundleReader& r, std::size_t sample, const fmt::WavData& w) {
  const fmt::SampleInfo& s = r.samples()[sample];
  if (!(s.format == w.format) || s.frame_count != w.frame_count) return false;
  const std::size_t ch = s.format.channels;
  std::vector<std::int32_t> blk(std::size_t(fmt::kBlockFrames) * ch);
  fmt::BlockDecoder dec;
  for (std::uint64_t b = 0; b < s.block_count; ++b) {
    auto n = r.read_block(sample, b, blk, dec);
    if (!n) return false;
    if (std::memcmp(blk.data(), w.data.data() + b * fmt::kBlockFrames * ch,
                    std::size_t(n.value()) * ch * sizeof(std::int32_t)) != 0)
      return false;
  }
  return true;
}


// Story 5.8.
bool gone(const ImportOptions& o) { return o.source_gone && o.source_gone(); }

// The source disappeared: nothing of the current timbre is kept. `res` keeps what an earlier checkpoint published.
ImportResult& removed(ImportResult& res, const std::string& where) {
  res.status = Status::kDriveRemoved;
  res.reason_code = ReasonCode::kNone;
  res.offenders.clear();
  res.failing_path.clear();
  res.reasons.push_back("the source drive was removed" + (where.empty() ? std::string() : " (" + where + ")") + "; the timbre in progress was discarded");
  return res;
}

// Errors that mean "the device is gone or failing" (not "this path does not exist").
bool io_errno(int e) { return e == EIO || e == ENXIO || e == ENODEV || e == ESTALE || e == ENOTCONN; }

// The stronger probe for a read that failed with an I/O error (or a parse/verification that failed): list the source folder and read one byte of the source file
// again. true only when the file system itself reports a dead device (EIO, ENXIO, ENODEV, ESTALE...) or the injected probe says gone; a bad file on a healthy
// drive, or a plain missing path, is not removal.
bool confirmed_gone(const ImportOptions& o) {
  if (gone(o)) return true;
  const std::string root = !o.source_root.empty() ? o.source_root : fs::path(o.sfz_path).parent_path().string();
  if (DIR* d = ::opendir(root.empty() ? "." : root.c_str())) {
    errno = 0;
    while (::readdir(d)) {
    }
    const int e = errno;
    ::closedir(d);
    if (e != 0 && io_errno(e)) return true;
  } else if (io_errno(errno)) {
    return true;
  }
  const int fd = ::open(o.sfz_path.c_str(), O_RDONLY);
  if (fd < 0) return io_errno(errno);
  char b;
  const ssize_t n = ::pread(fd, &b, 1, 0);
  const int e = errno;
  ::close(fd);
  return n < 0 && io_errno(e);
}

bool timbre_hook(const ImportOptions& o, const char* point, std::size_t timbre) {  // true = abort
  return o.fault.on_timbre && o.fault.on_timbre(point, timbre) == HookAction::kAbort;
}

}  // namespace

int reconcile_staging(const std::string& root) {
  const fs::path dir = fs::path(root) / host::kLibDirName / kStagingDirName;
  std::error_code ec;
  if (!fs::exists(dir, ec)) return 0;
  int removed = 0;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().filename() == kLockFileName) continue;
    std::error_code rm;
    fs::remove_all(it->path(), rm);
    if (!rm) ++removed;
  }
  return ec ? -1 : removed;
}

namespace {

// The data root, the library and staging directories and the exclusive lock (protocol step 2).
struct RootLock {
  Fd lock;
  std::string lib_root, staging_root;
};

ImportResult& fail_res(ImportResult& res, Status s, std::string why) {
  res.status = s;
  res.reasons.push_back(std::move(why));
  return res;
}

// Writes `dir/name` atomically (tmp, fsync, rename): the file is either absent/old or complete.
bool write_file_atomic(const std::string& dir, const char* name, const std::string& text) {
  const std::string dst = dir + "/" + name, tmp = dst + ".tmp";
  Fd f(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644));
  return f.ok() && ::write(f.fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()) && full_sync(f.fd) &&
         ::rename(tmp.c_str(), dst.c_str()) == 0;
}

// Story 5.6: the catalog.meta of the bundle being published, from its manifest (via the reopened staged bundle) and the options.
// "" on success, else why the metadata cannot be built (an instrument without a category).
std::string build_catalog_meta(const ImportOptions& opt, const fmt::BundleReader& rd, CatalogMeta& meta) {
  meta = CatalogMeta{};
  std::error_code ec;
  const fs::path abs = fs::weakly_canonical(fs::absolute(opt.sfz_path, ec), ec);
  // Build-only mode (content-pack): the catalog.meta of a vendor pack names the source file, never the packer's directory layout.
  meta.source_path = opt.bundle_sink ? fs::path(opt.sfz_path).filename().string() : (ec ? opt.sfz_path : abs.generic_string());
  meta.source_kind = is_syx_path(opt.sfz_path) ? "sysex" : is_sf2_path(opt.sfz_path) ? "sf2" : "sfz";
  const auto def_cat = fmt::parse_assignable_category(opt.category);
  const bool single = opt.source_timbres ? opt.source_timbres == 1 : rd.instruments().size() == 1;
  for (const auto& ins : rd.instruments()) {
    CatalogMetaInstrument mi;
    mi.key = ins.key;
    mi.name = ins.name.empty() ? ins.key : ins.name;
    if (single && !opt.library_name.empty()) mi.name = opt.library_name;
    std::optional<fmt::Category> cat = def_cat;
    for (const auto& o : opt.instrument_meta) {
      if (o.key != ins.key) continue;
      if (!o.name.empty()) mi.name = o.name;
      if (!o.category.empty()) cat = fmt::parse_assignable_category(o.category);
    }
    if (!cat || !fmt::is_assignable(*cat)) return "instrument '" + ins.key + "' has no category";
    mi.category = *cat;
    meta.instruments.push_back(std::move(mi));
  }
  meta.library_name = !opt.library_name.empty() ? opt.library_name
                      : single                  ? meta.instruments[0].name
                                                : abs.stem().string();
  if (meta.library_name.empty()) meta.library_name = meta.source_path;
  return {};
}

// Argument problems of the Story 5.6 options ("" = fine).
std::string meta_options_problem(const ImportOptions& opt) {
  if (!opt.write_catalog_meta) return {};
  if (opt.category.empty()) return "a category is required when catalog.meta is written";
  if (opt.library_name.size() > 256) return "library name too long";
  for (unsigned char c : opt.library_name)
    if (c < 0x20 || c == 0x7f) return "invalid library name";
  for (const auto& o : opt.instrument_meta) {
    if (o.name.size() > 256) return "instrument name too long";
    for (unsigned char c : o.name)
      if (c < 0x20 || c == 0x7f) return "invalid instrument name";
    if (const std::string why = category_problem(o.category); !why.empty()) return why;
  }
  return {};
}

// Story 6.3, build-only mode (ImportOptions::bundle_sink): the same verification as the staged path (reopen, verify_all, `verify_content` against the
// source), but on the bytes in memory, and the writer goes to the sink. Nothing touches the file system.
ImportResult& finish_in_memory(const ImportOptions& opt, ImportResult& res, const fmt::BundleWriter& writer,
                               const std::function<std::string(const fmt::BundleReader&)>& verify_content) {
  BuiltBundle built;
  built.writer = &writer;
  {  // the unprotected bytes live only inside this scope: zeroed and freed before the sink (which holds the keys) runs
    auto bytes = writer.finalize();
    if (!bytes) return fail_res(res, Status::kIo, std::string("cannot build bundle: ") + fmt::to_string(bytes.error()));
    std::vector<std::uint8_t>& plain = bytes.value();
    auto scrub = [&plain] {
      volatile std::uint8_t* p = plain.data();
      for (std::size_t i = 0; i < plain.size(); ++i) p[i] = 0;
    };
    built.plain_sha256 = fmt::Sha256::hash(plain);
    auto r = fmt::BundleReader::open_memory(plain);
    if (!r) {
      scrub();
      return fail_res(res, Status::kVerifyFailed, std::string("built bundle does not open: ") + fmt::to_string(r.error()));
    }
    const fmt::BundleReader& rd = r.value();
    std::string why;
    if (fmt::Error e = rd.verify_all(); e != fmt::Error::kNone) why = std::string("built bundle failed verification: ") + fmt::to_string(e);
    if (why.empty()) why = verify_content(rd);
    if (why.empty() && opt.write_catalog_meta) {
      CatalogMeta meta;
      why = build_catalog_meta(opt, rd, meta);
      if (why.empty()) built.catalog_meta = catalog_meta_text(meta);
    }
    if (!why.empty()) {
      scrub();
      return fail_res(res, Status::kVerifyFailed, why);
    }
    res.library_id = fmt::uuid_to_string(rd.library_id());
    res.bundle_id = fmt::to_hex(rd.bundle_id());
    scrub();
  }
  built.library_id = res.library_id;
  built.plain_bundle_id = res.bundle_id;
  if (fmt::Error e = opt.bundle_sink(built); e != fmt::Error::kNone)
    return fail_res(res, Status::kIo, std::string("bundle sink failed: ") + fmt::to_string(e));
  res.status = Status::kOk;
  return res;
}

// Story 6.3: the vendor library id option, "" = fine. `out` is set only when an id was given.
std::string library_id_problem(const ImportOptions& opt, fmt::Uuid& out, bool& given) {
  given = !opt.library_id.empty();
  if (!given) return {};
  fmt::Uuid u{};
  if (!fmt::uuid_from_string(opt.library_id, u)) return "library_id '" + opt.library_id + "' is not a canonical UUID";
  if (u == fmt::Uuid{}) return "library_id must not be the nil UUID";
  out = u;
  return {};
}

// false: `res` holds the failure.
bool lock_root(const ImportOptions& opt, ImportResult& res, RootLock& rl) {
  auto fail = [&](Status s, std::string why) { fail_res(res, s, std::move(why)); return false; };
  rl.lib_root = (fs::path(opt.root) / host::kLibDirName).string();
  rl.staging_root = rl.lib_root + "/" + kStagingDirName;
  if (!mkdir_synced(rl.lib_root, opt.root) || !mkdir_synced(rl.staging_root, rl.lib_root) || !is_dir(rl.staging_root))
    return fail(Status::kIo, err_text("cannot create " + rl.staging_root));
  rl.lock.fd = ::open((rl.staging_root + "/" + kLockFileName).c_str(), O_RDWR | O_CREAT, 0644);
  if (!rl.lock.ok()) return fail(Status::kIo, err_text("cannot open lock file"));
  if (::flock(rl.lock.fd, LOCK_EX | LOCK_NB) != 0)
    return fail(errno == EWOULDBLOCK ? Status::kBusy : Status::kIo, err_text("cannot lock " + rl.staging_root));
  if (opt.fault.pretend_cross_device || !same_device(rl.staging_root, rl.lib_root))
    return fail(Status::kCrossDevice,
                "the staging directory " + rl.staging_root + " is not on the destination filesystem");
  const int rc = reconcile_staging(opt.root);
  if (rc < 0) return fail(Status::kIo, "cannot clean up leftovers in " + rl.staging_root);
  if (rc > 0) res.reconciled = static_cast<std::size_t>(rc);
  return true;
}

// Protocol steps 4-8 for a bundle whose content is complete in `writer`: stage, verify (reopen,
// verify_all, then `verify_content` against the source), fsync, rename, flip `current`.
ImportResult& stage_and_publish(const ImportOptions& opt, ImportResult& res, RootLock& rl, fmt::BundleWriter& writer,
                                const std::function<std::string(const fmt::BundleReader&)>& verify_content) {
  auto fail = [&](Status s, std::string why) -> ImportResult& { return fail_res(res, s, std::move(why)); };
  std::error_code ec;
  const std::string& lib_root = rl.lib_root;
  const std::string& staging_root = rl.staging_root;
#define VELORA_HOOK(stage)                                                         \
  do {                                                                             \
    if (opt.fault.on_stage && opt.fault.on_stage(stage) == HookAction::kAbort) {   \
      res.status = Status::kAborted;                                               \
      return res;                                                                  \
    }                                                                              \
  } while (0)

  // 4. Stage.
  const std::string stage_dir = staging_root + "/" +
      std::to_string(::getpid()) + "-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
      std::to_string(g_counter.fetch_add(1));
  if (::mkdir(stage_dir.c_str(), 0755) != 0) return fail(Status::kIo, err_text("mkdir " + stage_dir));
  const std::string staged = stage_dir + "/" + host::kBundleFileName;
  auto cleanup = [&]() { std::error_code e; fs::remove_all(stage_dir, e); };
  VELORA_HOOK(Stage::kStageCreated);
  // Category metadata is part of the staged directory, hence of the atomic publish.
  auto write_category = [&](const std::string& dir) -> bool {
    // Validated by the importers: the stable key of an assignable category (never "none"), see category.hpp.
    const auto parsed = fmt::parse_assignable_category(opt.category);
    const std::string text = parsed ? fmt::category_file_text(*parsed) : std::string();
    return !text.empty() && write_file_atomic(dir, kCategoryFileName, text);
  };
  std::string meta_text;  // Story 5.6: catalog.meta, built from the reopened staged bundle during verification
  if (!opt.category.empty() && !write_category(stage_dir)) {
    cleanup();
    return fail(Status::kIo, err_text("write category"));
  }
  if (opt.mark_checkpoint && !write_file_atomic(stage_dir, kCheckpointMarkerName, "1\n")) {
    cleanup();
    return fail(Status::kIo, err_text("write checkpoint marker"));
  }
  if (fmt::Error e = writer.finalize_to_file(staged); e != fmt::Error::kNone) {
    cleanup();
    return fail(Status::kIo, std::string("cannot write bundle: ") + fmt::to_string(e));
  }
  if (!fsync_path(stage_dir, true)) {
    cleanup();
    return fail(Status::kIo, err_text("fsync " + stage_dir));
  }
  VELORA_HOOK(Stage::kStaged);

  // 5. Verify bit-exact from the staged bytes.
  ++opt.fault.rounds_seen;
  if (opt.fault.corrupt_staged_before_verify && (opt.fault.corrupt_round == 0 || opt.fault.rounds_seen == opt.fault.corrupt_round)) {
    Fd f(::open(staged.c_str(), O_RDWR));
    struct stat st;
    std::uint8_t b = 0;
    if (!f.ok() || ::fstat(f.fd, &st) != 0 || st.st_size < 2) { cleanup(); return fail(Status::kIo, "fault injection failed"); }
    const off_t pos = st.st_size - 2;  // inside the last block of the last sample
    if (::pread(f.fd, &b, 1, pos) != 1) { cleanup(); return fail(Status::kIo, "fault injection failed"); }
    b ^= 0x01;
    if (::pwrite(f.fd, &b, 1, pos) != 1) { cleanup(); return fail(Status::kIo, "fault injection failed"); }
    full_sync(f.fd);
  }
  auto verify = [&]() -> std::string {
    auto r = fmt::BundleReader::open_file(staged);
    if (!r) return std::string("staged bundle does not open: ") + fmt::to_string(r.error());
    const fmt::BundleReader& rd = r.value();
    if (fmt::Error e = rd.verify_all(); e != fmt::Error::kNone)
      return std::string("staged bundle failed verification: ") + fmt::to_string(e);
    if (std::string why = verify_content(rd); !why.empty()) return why;
    if (opt.write_catalog_meta) {
      CatalogMeta meta;
      if (std::string why = build_catalog_meta(opt, rd, meta); !why.empty()) return why;
      meta_text = catalog_meta_text(meta);
    }
    res.library_id = fmt::uuid_to_string(rd.library_id());
    res.bundle_id = fmt::to_hex(rd.bundle_id());
    return {};
  };
  if (std::string why = verify(); !why.empty()) {
    cleanup();
    return fail(Status::kVerifyFailed, why);
  }
  if (opt.write_catalog_meta && !write_file_atomic(stage_dir, kCatalogMetaFileName, meta_text)) {
    cleanup();
    return fail(Status::kIo, err_text("write catalog.meta"));
  }
  VELORA_HOOK(Stage::kVerified);

  // 6. Make the staged files durable.
  if (!fsync_path(staged, false) || !fsync_path(stage_dir, true)) {
    cleanup();
    return fail(Status::kIo, err_text("fsync staged bundle"));
  }
  VELORA_HOOK(Stage::kFsynced);

  // 7. Publish: rename the staging directory to <library_id>/<bundle_id>.
  const std::string lib_dir = lib_root + "/" + res.library_id;
  res.bundle_dir = lib_dir + "/" + res.bundle_id;
  // Story 6.4: an installed Factory library (any of its bundles is protected: bundle.vlrp) is never replaced or extended by a user import.
  if (factory_library_installed(opt.root, res.library_id)) {
    cleanup();
    return fail(Status::kFactoryProtected, "library " + res.library_id + " is a factory library installed from a protected bundle; a user import cannot replace it");
  }
  if (::mkdir(lib_dir.c_str(), 0755) != 0 && errno != EEXIST) {
    cleanup();
    return fail(Status::kIo, err_text("mkdir " + lib_dir));
  }
  if (!is_dir(lib_dir)) { cleanup(); return fail(Status::kIo, lib_dir + " is not a directory"); }
  // Stale `.current.tmp.*` links of a crashed flip, in this library's directory only (the lock
  // makes this safe).
  for (fs::directory_iterator it(lib_dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string n = it->path().filename().string();
    if (n.rfind(".current.tmp", 0) == 0) { std::error_code rm; fs::remove(it->path(), rm); ++res.reconciled; }
  }
  if (is_dir(res.bundle_dir)) {
    // Identical content was imported before. It must be intact; otherwise do not touch it.
    auto existing = fmt::BundleReader::open_file(res.bundle_dir + "/" + host::kBundleFileName);
    if (!existing || existing.value().verify_all() != fmt::Error::kNone ||
        fmt::to_hex(existing.value().bundle_id()) != res.bundle_id) {
      cleanup();
      return fail(Status::kIo, res.bundle_dir + " exists but is damaged; remove it and import again");
    }
    cleanup();
    // Identical bundle already there: refresh its category (still before the flip).
    if (!opt.category.empty() && (!write_category(res.bundle_dir) || !fsync_path(res.bundle_dir, true)))
      return fail(Status::kIo, err_text("write category"));
    if (opt.write_catalog_meta && (!write_file_atomic(res.bundle_dir, kCatalogMetaFileName, meta_text) || !fsync_path(res.bundle_dir, true)))
      return fail(Status::kIo, err_text("write catalog.meta"));
  } else {
    if (::rename(stage_dir.c_str(), res.bundle_dir.c_str()) != 0) {
      const bool xdev = errno == EXDEV;
      const std::string why = err_text("rename to " + res.bundle_dir);
      cleanup();
      return fail(xdev ? Status::kCrossDevice : Status::kIo, why);
    }
    res.bundle_created = true;
    if (!fsync_path(lib_dir, true) || !fsync_path(lib_root, true))
      return fail(Status::kIo, err_text("fsync " + lib_dir));
  }
  VELORA_HOOK(Stage::kRenamed);

  // 8. Flip `current` (symlink to a temporary name, then rename over it).
  const std::string link = lib_dir + "/" + host::kCurrentLinkName;
  char cur[PATH_MAX];
  const ssize_t n = ::readlink(link.c_str(), cur, sizeof cur - 1);
  const bool already = n > 0 && std::string(cur, static_cast<std::size_t>(n)) == res.bundle_id;
  if (!already) {
    const std::string tmp = lib_dir + "/.current.tmp." + std::to_string(::getpid());
    ::unlink(tmp.c_str());
    if (::symlink(res.bundle_id.c_str(), tmp.c_str()) != 0) return fail(Status::kIo, err_text("symlink"));
    VELORA_HOOK(Stage::kLinkCreated);
    if (::rename(tmp.c_str(), link.c_str()) != 0) {
      const std::string why = err_text("rename current");
      ::unlink(tmp.c_str());
      return fail(Status::kIo, why);
    }
    if (!fsync_path(lib_dir, true)) return fail(Status::kIo, err_text("fsync " + lib_dir));
    res.current_flipped = true;
  }
  VELORA_HOOK(Stage::kFlipped);

  res.status = Status::kOk;
  return res;
#undef VELORA_HOOK
}

}  // namespace

ImportResult import_sfz(const ImportOptions& opt) {
  ImportResult res;
  auto fail = [&](Status s, std::string why) -> ImportResult& {
    res.status = s;
    res.reasons.push_back(std::move(why));
    return res;
  };

  if (opt.sfz_path.empty() || (opt.root.empty() && !opt.bundle_sink))
    return fail(Status::kInvalidArgument, "an SFZ file and a data root are required");
  if (const std::string why = category_problem(opt.category); !why.empty()) return fail(Status::kInvalidArgument, why);
  if (const std::string why = meta_options_problem(opt); !why.empty()) return fail(Status::kInvalidArgument, why);
  fmt::Uuid vendor_id{};
  bool has_vendor_id = false;
  if (const std::string why = library_id_problem(opt, vendor_id, has_vendor_id); !why.empty()) return fail(Status::kInvalidArgument, why);
  // Suggestion only (Story 5.5): never stored; the caller decides the category.
  res.suggested_category =
      suggest_category(suggest_input_for_path(opt.sfz_path, sfz_metadata_strings(opt.sfz_path))).value_or(fmt::Category::kNone);

  if (!opt.bundle_sink) {
    struct stat st;
    if (::stat(opt.root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
      return fail(Status::kInvalidArgument, "the data root " + opt.root + " is not an existing directory");
  }

  std::error_code ec;
  const fs::path sfz_abs = fs::weakly_canonical(fs::absolute(opt.sfz_path, ec), ec);
  if (ec) return fail(Status::kIo, "cannot resolve " + opt.sfz_path);
  const fs::path base = opt.base_dir.empty() ? sfz_abs.parent_path()
                                              : fs::weakly_canonical(fs::absolute(opt.base_dir, ec), ec);
  const fs::path rel = sfz_abs.lexically_relative(base);
  if (rel.empty() || *rel.begin() == "..")
    return fail(Status::kInvalidArgument, "the SFZ file is not inside the base directory");

  // 1. Parse (no audio, no filesystem writes).
  res.timbres_total = 1;  // an SFZ file is one timbre
  if (timbre_hook(opt, "timbre-start", 0)) return fail(Status::kAborted, "stopped at timbre-start");
  const std::string lib_root = opt.base_dir.empty() ? std::string{} : base.string();
  fmt::SfzParseResult parsed = fmt::parse_sfz_file(opt.sfz_path, lib_root);
  res.source_region_count = parsed.source_region_count;
  for (const auto& a : parsed.allowlisted) res.allowlisted.push_back({a.opcode, a.file, a.line});
  for (const auto& w : parsed.warnings) res.warnings.push_back({"ignored_opcode", w.opcode, w.file, w.line, w.message, {}});
  for (const auto& v : parsed.ignored_values) res.ignored_values.push_back({v.opcode, v.file, v.line});
  if (!parsed.ok() && confirmed_gone(opt)) return removed(res, "reading " + opt.sfz_path);
  if (!parsed.ok()) {
    res.status = Status::kSkipped;
    bool damaged = false;
    for (const auto& i : parsed.issues) {
      res.reasons.push_back(i.to_string());
      if (fmt::is_unsupported_feature(i.kind)) {
        res.offenders.push_back({i.opcode, i.file, i.line, i.message});
      } else {
        damaged = true;
        if (res.failing_path.empty()) res.failing_path = i.path;
      }
    }
    // A file with any damage is damaged: the list of unsupported constructs may be incomplete.
    res.reason_code = damaged ? ReasonCode::kDamaged : ReasonCode::kUnsupportedFeatures;
    return res;
  }
  // Story 13.2 / 13.7: a hikey=-1 region without an on_locc/on_hicc trigger can never start a voice and is not written; one with a trigger is kept.
  const std::size_t never_note = fmt::drop_never_note_triggered(parsed);
  if (parsed.regions.empty()) {
    res.status = Status::kSkipped;
    res.reason_code = ReasonCode::kDamaged;
    res.failing_path = opt.sfz_path;
    res.reasons.push_back(opt.sfz_path + ": no regions in SFZ file" +
                          (never_note ? " that a note can trigger (hikey=-1 regions without an on_locc/on_hicc trigger are never played)" : ""));
    return res;
  }

  // Story 6.4: refused early, before any sample is read or encoded.
  if (!opt.bundle_sink && !opt.root.empty() &&
      factory_library_installed(opt.root, fmt::uuid_to_string(has_vendor_id ? vendor_id : fmt::library_id_from_source(sfz_abs.generic_string()))))
    return fail(Status::kFactoryProtected, "the library of " + opt.sfz_path + " is a factory library installed from a protected bundle; a user import cannot replace it");

  // An override for an instrument this file does not have is refused before anything is written.
  for (const auto& o : opt.instrument_meta)
    if (o.key != rel.generic_string())
      return fail(Status::kInvalidArgument, "unknown_instrument_key: '" + o.key + "' is not an instrument of " + opt.sfz_path);

  // 2. Directories and the exclusive lock.
  RootLock rl;
  if (!opt.bundle_sink && !lock_root(opt, res, rl)) return res;

  // 3. Build the bundle: load each sample once, encode, drop it again.
  fmt::BundleWriter writer;
  if (writer.set_source_id(sfz_abs.generic_string()) != fmt::Error::kNone)
    return fail(Status::kInvalidArgument, "empty source id");
  if (has_vendor_id) writer.set_library_id(vendor_id);
  auto inst = writer.add_instrument(sfz_abs.stem().string(), rel.generic_string());
  if (!inst) return fail(Status::kInvalidArgument, std::string("instrument key: ") + fmt::to_string(inst.error()));
  std::vector<AudioProblem> audio_problems;
  Status infra_status = Status::kIo;
  std::string infra_problem;  // add_sample failed for a reason that is not about the WAV
  std::vector<std::uint64_t> frames(parsed.samples.size(), 0);
  for (std::size_t i = 0; i < parsed.samples.size(); ++i) {
    const auto& s = parsed.samples[i];
    auto w = fmt::load_wav_file(s.path, kStrictWav);
    // A read that failed because the drive is gone is not a damaged sample. An I/O error is confirmed by the stronger probe first (a bad file on a healthy drive
    // stays damaged); a short read (truncated) is damaged unless the probe says the source is gone.
    if (gone(opt) || (!w && w.error() == fmt::Error::kIo && confirmed_gone(opt))) return removed(res, "reading " + s.path);
    if (!w) {
      audio_problems.push_back(audio_problem(s.path, w.error(), s));
      continue;
    }
    frames[i] = w.value().frame_count;
    if (fmt::Error e = writer.add_sample(inst.value(), w.value().as_input(s.name)); e != fmt::Error::kNone)
    {
      ReasonCode rc;
      const Status st = status_for_add_sample_error(e, &rc);
      if (st == Status::kSkipped) {
        audio_problems.push_back(audio_problem(s.path, e, parsed.samples[i]));
      } else if (infra_problem.empty()) {
        infra_status = st;
        infra_problem = s.path + ": cannot add sample: " + fmt::to_string(e);
      }
    }
  }
  if (!infra_problem.empty()) return fail(infra_status, infra_problem);
  if (!audio_problems.empty()) {
    res.status = Status::kSkipped;
    bool damaged = false;
    for (const auto& p : audio_problems) {
      res.reasons.push_back(p.text);
      if (p.code == ReasonCode::kUnsupportedFeatures) {
        res.offenders.push_back({"sample", p.ref_file, p.ref_line, p.text});
      } else {
        damaged = true;
        if (res.failing_path.empty()) res.failing_path = p.path;
      }
    }
    res.reason_code = damaged ? ReasonCode::kDamaged : ReasonCode::kUnsupportedFeatures;
    return res;
  }
  std::vector<std::string> region_problems;
  std::vector<fmt::Region> regions = parsed.regions;  // with loop defaults resolved
  std::size_t first_bad_region = 0;
  for (std::size_t i = 0; i < regions.size(); ++i) {
    auto& r = regions[i];
    const std::string where = parsed.region_sources[i].file + ":" + std::to_string(parsed.region_sources[i].line) + ": ";
    std::string why;
    auto note = [&](std::string text) {
      if (region_problems.empty()) first_bad_region = i;
      region_problems.push_back(std::move(text));
    };
    if (!fmt::resolve_loop_points(r, parsed.loops[i], frames[r.sample], &why))
      note(where + why + " (sample '" + parsed.samples[r.sample].name + "')");
    else if (fmt::validate_region(r, frames[r.sample]) != fmt::Error::kNone ||
             writer.add_region(inst.value(), r) != fmt::Error::kNone)
      note(where + "region values do not fit sample '" + parsed.samples[r.sample].name +
                                "' (offset or loop points beyond its length)");
  }
  if (!region_problems.empty()) {
    res.status = Status::kSkipped;
    res.reason_code = ReasonCode::kDamaged;
    res.failing_path = parsed.region_sources[first_bad_region].file;
    res.reasons = std::move(region_problems);
    return res;
  }
  // Story 13.5: the CC defaults and <curve> tables the regions use (empty for every library that has none: the bundle keeps its bytes).
  if (writer.set_instrument_cc_setup(inst.value(), parsed.cc_setup) != fmt::Error::kNone)
    return fail(Status::kInvalidArgument, opt.sfz_path + ": the CC setup (set_cc / <curve>) is not valid");
  res.sample_count = parsed.samples.size();
  res.region_count = regions.size();
  if (timbre_hook(opt, "timbre-done", 0)) return fail(Status::kAborted, "stopped at timbre-done");

  // 4-8. Stage, verify, publish, flip.
  auto verify_content = [&](const fmt::BundleReader& rd) -> std::string {
    if (rd.instruments().size() != 1 || rd.samples().size() != parsed.samples.size())
      return "staged bundle has the wrong instrument/sample count";
    if (rd.instruments()[0].regions != regions)
      return "staged region table differs from the parsed region table";
    if (rd.instruments()[0].cc_setup != parsed.cc_setup) return "staged CC setup differs from the parsed one";
    for (std::size_t i = 0; i < parsed.samples.size(); ++i) {
      auto w = fmt::load_wav_file(parsed.samples[i].path, kStrictWav);  // fresh decode of the source
      if (!w || !sample_matches(rd, i, w.value()))
        return "sample '" + parsed.samples[i].name + "' is not bit-identical to its source";
    }
    return {};
  };
  if (opt.bundle_sink) {
    finish_in_memory(opt, res, writer, verify_content);
    if (res.status == Status::kOk) res.timbres_published = 1;
    return res;
  }
  stage_and_publish(opt, res, rl, writer, verify_content);
  if ((res.status == Status::kVerifyFailed || res.status == Status::kIo) && !res.bundle_created && confirmed_gone(opt)) {
    res.reasons.clear();  // the staging directory is already removed by the failed publish
    res.library_id.clear();
    res.bundle_id.clear();
    res.bundle_dir.clear();
    return removed(res, "verifying " + opt.sfz_path);
  }
  if (res.status == Status::kOk) res.timbres_published = 1;
  return res;
}


bool is_sf2_path(const std::string& path) {
  std::string ext = fs::path(path).extension().string();
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext == ".sf2";
}

bool is_syx_path(const std::string& path) {
  std::string ext = fs::path(path).extension().string();
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext == ".syx";
}

ImportResult import_sysex(const ImportOptions& opt) {
  ImportResult res;
  auto fail = [&](Status s, std::string why) -> ImportResult& { return fail_res(res, s, std::move(why)); };
  if (opt.sfz_path.empty() || (opt.root.empty() && !opt.bundle_sink))
    return fail(Status::kInvalidArgument, "a SysEx file and a data root are required");
  if (const std::string why = category_problem(opt.category); !why.empty()) return fail(Status::kInvalidArgument, why);
  if (const std::string why = meta_options_problem(opt); !why.empty()) return fail(Status::kInvalidArgument, why);
  fmt::Uuid vendor_id{};
  bool has_vendor_id = false;
  if (const std::string why = library_id_problem(opt, vendor_id, has_vendor_id); !why.empty()) return fail(Status::kInvalidArgument, why);
  if (!opt.bundle_sink) {
    struct stat st;
    if (::stat(opt.root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
      return fail(Status::kInvalidArgument, "the data root " + opt.root + " is not an existing directory");
  }

  // 1. Parse using formatlib 6-OP parser
  fmt::SysexParseResult parsed = fmt::parse_sysex_file(opt.sfz_path);
  res.suggested_category = fmt::Category::kSynthPoly;
  res.timbres_total = parsed.voices.empty() ? 1 : parsed.voices.size();

  if (!parsed.ok() || parsed.voices.empty()) {
    res.status = Status::kSkipped;
    res.reason_code = ReasonCode::kDamaged;
    res.failing_path = opt.sfz_path;
    res.reasons.push_back(opt.sfz_path + ": " + fmt::kSysexDamagedMessage);
    return res;
  }

  std::error_code ec;
  const fs::path abs = fs::weakly_canonical(fs::absolute(opt.sfz_path, ec), ec);
  if (ec) return fail(Status::kIo, "cannot resolve " + opt.sfz_path);

  const fmt::Uuid will_be = has_vendor_id ? vendor_id : fmt::library_id_from_source(abs.generic_string());
  if (!opt.bundle_sink && !opt.root.empty() && factory_library_installed(opt.root, fmt::uuid_to_string(will_be))) {
    return fail(Status::kFactoryProtected, "the library of " + opt.sfz_path + " is a factory library installed from a protected bundle; a user import cannot replace it");
  }

  // Determine category
  fmt::Category cat = fmt::Category::kSynthPoly;
  if (!opt.category.empty()) {
    auto parsed_cat = fmt::parse_assignable_category(opt.category);
    if (parsed_cat && fmt::is_assignable(*parsed_cat)) cat = *parsed_cat;
  }

  std::string lib_name = !opt.library_name.empty() ? opt.library_name : abs.stem().string();

  auto bundle_res = fmt::ParametricBundle::from_sysex(parsed, abs.generic_string(), lib_name, cat);
  if (!bundle_res) return fail(Status::kInvalidArgument, "failed to create parametric bundle");
  fmt::ParametricBundle bundle = bundle_res.take();
  if (has_vendor_id) bundle.set_library_id(vendor_id);

  // Apply per-instrument overrides if any
  for (const auto& o : opt.instrument_meta) {
    for (auto& t : bundle.timbres()) {
      if (t.key == o.key) {
        if (!o.name.empty()) t.name = o.name;
        if (!o.category.empty()) {
          auto c = fmt::parse_assignable_category(o.category);
          if (c && fmt::is_assignable(*c)) t.category = *c;
        }
      }
    }
  }

  std::vector<uint8_t> bundle_bytes = bundle.serialize();
  fmt::Digest256 bundle_id = fmt::Sha256::hash(bundle_bytes);
  std::string bundle_id_str = fmt::to_hex(bundle_id);
  std::string lib_id_str = fmt::uuid_to_string(bundle.library_id());

  res.library_id = lib_id_str;
  res.bundle_id = bundle_id_str;
  res.timbres_total = bundle.timbres().size();
  res.timbres_published = bundle.timbres().size();
  res.preset_count = bundle.timbres().size();

  if (opt.bundle_sink) {
    res.status = Status::kOk;
    return res;
  }

  RootLock rl;
  if (!lock_root(opt, res, rl)) return res;

  std::string cur_link = rl.lib_root + "/" + lib_id_str + "/" + host::kCurrentLinkName;
  char buf[PATH_MAX];
  const ssize_t cur_len = ::readlink(cur_link.c_str(), buf, sizeof buf - 1);  // readlink does not NUL-terminate
  const bool had_current = cur_len > 0;
  std::string old_bundle_id = had_current ? std::string(buf, static_cast<std::size_t>(cur_len)) : "";

  std::string staging_dir = rl.lib_root + "/.staging/syx-" + bundle_id_str.substr(0, 8) + "-" + std::to_string(::getpid());
  std::filesystem::create_directories(staging_dir, ec);
  if (ec) return fail(Status::kIo, "cannot create staging dir " + staging_dir);

  std::string staged_bundle = staging_dir + "/" + host::kBundleFileName;
  {
    std::ofstream f(staged_bundle, std::ios::binary);
    if (!f.is_open()) return fail(Status::kIo, "cannot write staged bundle");
    f.write(reinterpret_cast<const char*>(bundle_bytes.data()), bundle_bytes.size());
    if (!f.good()) return fail(Status::kIo, "write error on staged bundle");
  }

  CatalogMeta cm;
  cm.format_version = kCatalogMetaFormatVersion;
  cm.library_name = lib_name;
  cm.source_kind = "sysex";
  cm.source_path = abs.generic_string();
  for (const auto& t : bundle.timbres()) {
    CatalogMetaInstrument mi;
    mi.key = t.key;
    mi.name = t.name;
    mi.category = t.category;
    cm.instruments.push_back(std::move(mi));
  }
  std::string meta_txt = catalog_meta_text(cm);
  {
    std::ofstream f(staging_dir + "/" + kCatalogMetaFileName);
    f << meta_txt;
  }

  fsync_path(staged_bundle, false);
  fsync_path(staging_dir, true);

  std::string lib_dest_dir = rl.lib_root + "/" + lib_id_str;
  std::filesystem::create_directories(lib_dest_dir, ec);
  std::string final_bundle_dir = lib_dest_dir + "/" + bundle_id_str;
  if (std::filesystem::exists(final_bundle_dir)) {
    std::filesystem::remove_all(final_bundle_dir, ec);
  }
  std::filesystem::rename(staging_dir, final_bundle_dir, ec);
  if (ec) return fail(Status::kIo, "cannot move staged bundle to " + final_bundle_dir);
  fsync_path(lib_dest_dir, true);

  std::string symlink_tmp = lib_dest_dir + "/.current.tmp." + std::to_string(::getpid());
  ::unlink(symlink_tmp.c_str());
  if (::symlink(bundle_id_str.c_str(), symlink_tmp.c_str()) != 0) {
    return fail(Status::kIo, "symlink failed");
  }
  if (::rename(symlink_tmp.c_str(), cur_link.c_str()) != 0) {
    ::unlink(symlink_tmp.c_str());
    return fail(Status::kIo, "cannot flip current symlink");
  }
  fsync_path(lib_dest_dir, true);

  res.status = Status::kOk;
  res.bundle_created = true;
  res.current_flipped = true;
  res.bundle_dir = final_bundle_dir;
  if (had_current && old_bundle_id == bundle_id_str) {
    res.bundle_created = false;
    res.current_flipped = false;
  }
  return res;
}

ImportResult import_file(const ImportOptions& opt) {
  if (is_syx_path(opt.sfz_path)) return import_sysex(opt);
  return is_sf2_path(opt.sfz_path) ? import_sf2(opt) : import_sfz(opt);
}

ImportResult import_sf2(const ImportOptions& opt) {
  ImportResult res;
  res.sf2 = true;
  auto fail = [&](Status s, std::string why) -> ImportResult& { return fail_res(res, s, std::move(why)); };
  if (opt.sfz_path.empty() || (opt.root.empty() && !opt.bundle_sink))
    return fail(Status::kInvalidArgument, "an SF2 file and a data root are required");
  if (const std::string why = category_problem(opt.category); !why.empty()) return fail(Status::kInvalidArgument, why);
  if (const std::string why = meta_options_problem(opt); !why.empty()) return fail(Status::kInvalidArgument, why);
  fmt::Uuid vendor_id{};
  bool has_vendor_id = false;
  if (const std::string why = library_id_problem(opt, vendor_id, has_vendor_id); !why.empty()) return fail(Status::kInvalidArgument, why);
  if (!opt.bundle_sink) {
    struct stat st;
    if (::stat(opt.root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
      return fail(Status::kInvalidArgument, "the data root " + opt.root + " is not an existing directory");
  }

  // 1. Parse (no filesystem writes). Damaged beats unsupported; nothing of a skipped file is written.
  // The suggestion from the name and folders is there even when the file turns out damaged (as for SFZ); a
  // successful parse refines it with the preset names below.
  res.suggested_category = suggest_category(suggest_input_for_path(opt.sfz_path, {})).value_or(fmt::Category::kNone);
  auto parse_source = [&]() -> fmt::Sf2ParseResult {
    if (!opt.fault.open_sf2) return fmt::parse_sf2_file(opt.sfz_path);
    auto src = opt.fault.open_sf2(opt.sfz_path);
    return src ? fmt::parse_sf2(src) : fmt::parse_sf2_file(opt.sfz_path);
  };
  fmt::Sf2ParseResult parsed = parse_source();
  auto skip = [&](ReasonCode rc, std::string why) -> ImportResult& {
    res.status = Status::kSkipped;
    res.reason_code = rc;
    if (rc == ReasonCode::kDamaged) res.failing_path = opt.sfz_path;
    res.reasons.push_back(opt.sfz_path + ": " + std::move(why));
    return res;
  };
  res.timbres_total = parsed.presets.empty() ? 1 : parsed.presets.size();
  if (parsed.ok() && !parsed.presets.empty() && opt.on_timbres) opt.on_timbres(parsed.presets.size());
  if (!parsed.ok() && confirmed_gone(opt)) return removed(res, "reading " + opt.sfz_path);
  if (!parsed.ok()) {
    res.status = Status::kSkipped;
    const bool damaged = parsed.damaged();
    for (const auto& i : parsed.issues) {
      res.reasons.push_back(opt.sfz_path + ": " + i.message);
      if (i.kind == fmt::Sf2IssueKind::kUnsupported) res.offenders.push_back({i.item, opt.sfz_path, 0, i.message});
    }
    if (damaged) res.offenders.clear();
    res.reason_code = damaged ? ReasonCode::kDamaged : ReasonCode::kUnsupportedFeatures;
    if (damaged) res.failing_path = opt.sfz_path;
    return res;
  }
  for (const auto& w : parsed.warnings)
    res.warnings.push_back({w.code, w.item, opt.sfz_path, 0, w.message, w.preset});
  {
    // SF2: the preset names are the metadata (bank names are not in the model). Suggestion only.
    std::vector<std::string> names;
    for (const auto& pr : parsed.presets) names.push_back(pr.name);
    res.suggested_category = suggest_category(suggest_input_for_path(opt.sfz_path, std::move(names))).value_or(fmt::Category::kNone);
  }
  res.preset_count = parsed.presets.size();
  res.ignored_generators = parsed.stats.ignored_generators;
  res.ignored_modulators = parsed.stats.ignored_modulators;
  res.source_region_count = parsed.stats.zones;

  std::error_code ec;
  const fs::path abs = fs::weakly_canonical(fs::absolute(opt.sfz_path, ec), ec);
  if (ec) return fail(Status::kIo, "cannot resolve " + opt.sfz_path);
  if (!opt.bundle_sink && !opt.root.empty() &&
      factory_library_installed(opt.root, fmt::uuid_to_string(has_vendor_id ? vendor_id : fmt::library_id_from_source(abs.generic_string()))))
    return fail(Status::kFactoryProtected, "the library of " + opt.sfz_path + " is a factory library installed from a protected bundle; a user import cannot replace it");

  for (const auto& o : opt.instrument_meta) {
    bool known = false;
    for (const auto& p : parsed.presets) known |= p.key == o.key;
    if (!known) return fail(Status::kInvalidArgument, "unknown_instrument_key: '" + o.key + "' is not a preset of " + opt.sfz_path);
  }

  // 2. Directories and the exclusive lock.
  RootLock rl;
  if (!opt.bundle_sink && !lock_root(opt, res, rl)) return res;

  // 3. Build the bundle: one instrument per preset (timbre); each sample read once, encoded, dropped again. Story 5.8: every `every` finished timbres
  // (or `every_bytes` of PCM) the timbres finished so far are published as a bundle of their own (a checkpoint); see import.hpp.
  const std::size_t n_presets = parsed.presets.size();
  res.timbres_total = n_presets;
  const std::size_t every = opt.checkpoint_timbres ? opt.checkpoint_timbres : kDefaultCheckpointTimbres;
  const std::uint64_t every_bytes = opt.checkpoint_bytes ? opt.checkpoint_bytes : kDefaultCheckpointBytes;
  // A library that already has a `current` bundle (Import again, or what a crashed run published) is replaced only by a complete, verified bundle: no
  // checkpoints, and a removal leaves the old bundle current.
  bool has_previous = opt.bundle_sink != nullptr;  // build-only mode (Story 6.3): the whole bundle or nothing, no checkpoints
  if (!opt.bundle_sink) {
    // The library the bundle will be published as: the vendor id when one was given, else the path-derived one.
    const fmt::Uuid will_be = has_vendor_id ? vendor_id : fmt::library_id_from_source(abs.generic_string());
    const std::string cur = rl.lib_root + "/" + fmt::uuid_to_string(will_be) + "/" + host::kCurrentLinkName;
    char buf[PATH_MAX];
    has_previous = ::readlink(cur.c_str(), buf, sizeof buf - 1) > 0;
  }
  fmt::BundleWriter writer;
  if (writer.set_source_id(abs.generic_string()) != fmt::Error::kNone)
    return fail(Status::kInvalidArgument, "empty source id");
  if (has_vendor_id) writer.set_library_id(vendor_id);
  std::vector<std::int32_t> pcm;
  std::size_t samples = 0, regions = 0;       // of the finished timbres
  std::size_t finished = 0, pub_count = 0;    // timbres in the writer / in the last published bundle
  std::size_t pend = 0;
  std::uint64_t pend_bytes = 0;
  std::string prev_file;                      // bundle.vlb of the last published bundle of this run
  bool any_created = false, any_flipped = false, is_removed = false;

  // What a bundle of the first `n` timbres must hold: the first `carried` against the previous bundle of this run (it was verified against the source
  // when it was published), the others against a fresh parse of the source, or - when the source is gone - against what was parsed and converted.
  auto verify_for = [&](std::size_t n, std::size_t carried, bool against_source, std::string prev) {
    return [&, n, carried, against_source, prev](const fmt::BundleReader& rd) -> std::string {
      if (rd.instruments().size() != n) return "staged bundle has the wrong instrument count";
      if (carried > 0) {
        auto pr = fmt::BundleReader::open_file(prev);
        if (!pr || pr.value().instruments().size() != carried) return "the previous checkpoint bundle does not open or has another instrument count";
        for (std::size_t i = 0; i < carried; ++i) {
          const auto& a = rd.instruments()[i];
          const auto& b = pr.value().instruments()[i];
          if (a.key != b.key || a.name != b.name || a.regions != b.regions || a.sample_count != b.sample_count)
            return "finished timbre " + a.key + " differs from the previous checkpoint";
          for (std::size_t k = 0; k < a.sample_count; ++k) {
            const auto& sa = rd.samples()[a.first_sample + k];
            const auto& sb = pr.value().samples()[b.first_sample + k];
            if (!(sa.format == sb.format) || sa.frame_count != sb.frame_count || sa.pcm_sha256 != sb.pcm_sha256)
              return "sample '" + sa.name + "' of " + a.key + " differs from the previous checkpoint";
          }
        }
      }
      std::optional<fmt::Sf2ParseResult> fresh;
      if (against_source) {
        fresh.emplace(parse_source());
        if (!fresh->ok() || fresh->presets.size() != parsed.presets.size()) return "the SF2 file changed while it was imported";
      }
      std::vector<std::int32_t> want, got;
      for (std::size_t i = carried; i < n; ++i) {
        const auto& pp = parsed.presets[i];
        const auto& ins = rd.instruments()[i];
        if (ins.key != pp.key || ins.name != pp.name) return "staged instrument " + pp.key + " has the wrong key or name";
        if (ins.regions != pp.regions) return "staged region table of " + pp.key + " differs from the parsed zones";
        if (ins.sample_count != pp.samples.size()) return "staged instrument " + pp.key + " has the wrong sample count";
        for (std::size_t k = 0; k < pp.samples.size(); ++k) {
          const auto& si = rd.samples()[ins.first_sample + k];
          if (!(si.format == pp.samples[k].format) || si.frame_count != pp.samples[k].frames)
            return "sample '" + pp.samples[k].name + "' of " + pp.key + " has the wrong format or length";
        }
        if (!fresh) continue;
        const auto& fp = fresh->presets[i];
        if (ins.key != fp.key || ins.name != fp.name) return "staged instrument " + fp.key + " has the wrong key or name";
        if (ins.regions != fp.regions || fp.regions != pp.regions) return "staged region table of " + fp.key + " differs from the parsed zones";
        for (std::size_t k = 0; k < fp.samples.size(); ++k) {
          const auto& si = rd.samples()[ins.first_sample + k];
          if (!(si.format == fp.samples[k].format) || si.frame_count != fp.samples[k].frames)
            return "sample '" + fp.samples[k].name + "' of " + fp.key + " has the wrong format or length";
          if (fresh->samples->read(fp.samples[k], want) != fmt::Error::kNone) return "cannot re-read sample '" + fp.samples[k].name + "'";
          got.assign(want.size(), 0);
          if (rd.read_frames(ins.first_sample + k, 0, want.size(), got) != fmt::Error::kNone || got != want)
            return "sample '" + fp.samples[k].name + "' of " + fp.key + " is not bit-identical to its source";
        }
      }
      return {};
    };
  };
  // Publishes the first `finished` timbres as one bundle (the whole protocol, from staging to the flip of `current`).
  // `intermediate`: more bundles will follow (a checkpoint). Every bundle of a first-time import carries the checkpoint marker until the last one is current.
  ImportOptions ropt = opt;
  ropt.source_timbres = n_presets;
  bool prev_intermediate = false;
  auto round = [&](bool against_source) -> ImportResult {
    ImportResult r = res;
    r.bundle_created = r.current_flipped = false;
    r.sample_count = samples;
    r.region_count = regions;
    ropt.mark_checkpoint = !has_previous;
    stage_and_publish(ropt, r, rl, writer, verify_for(finished, pub_count, against_source, prev_file));
    return r;
  };
  // A bundle is published and current. `last`: no more bundles follow in this run - its marker goes; otherwise the previous marked bundle is superseded and removed
  // (a lease is an open file descriptor of the engine, which survives the unlink; the new bundle is current before the old one goes).
  auto merge = [&](const ImportResult& r, bool last) {
    const std::string old_dir = prev_intermediate ? fs::path(prev_file).parent_path().string() : std::string();
    res = r;
    any_created |= r.bundle_created;
    any_flipped |= r.current_flipped;
    pub_count = finished;
    prev_file = r.bundle_dir + "/" + host::kBundleFileName;
    res.timbres_published = finished;
    if (!old_dir.empty() && old_dir != r.bundle_dir) {
      std::error_code rm;
      fs::remove_all(old_dir, rm);
    }
    if (last) {
      ::unlink((r.bundle_dir + "/" + kCheckpointMarkerName).c_str());
      fsync_path(r.bundle_dir, true);
    }
    prev_intermediate = !last && !has_previous;
  };
  auto source_failure = [&](const ImportResult& r) {
    return (r.status == Status::kVerifyFailed || r.status == Status::kIo) && !r.bundle_created && confirmed_gone(opt);
  };

  for (std::size_t pi = 0; pi < n_presets && !is_removed; ++pi) {
    const auto& p = parsed.presets[pi];
    if (timbre_hook(opt, "timbre-start", pi)) return fail(Status::kAborted, "stopped at timbre-start");
    if (gone(opt)) {
      is_removed = true;
      break;
    }
    auto inst = writer.add_instrument(p.name, p.key);
    if (!inst) return fail(Status::kInvalidArgument, "instrument key " + p.key + ": " + fmt::to_string(inst.error()));
    std::size_t t_samples = 0;
    std::uint64_t t_bytes = 0;
    for (const auto& s : p.samples) {
      const fmt::Error re = parsed.samples->read(s, pcm);
      // A parsed file whose sample data cannot be read (EIO, ENXIO, a short read of a pulled drive) or a probe that says the source is gone: removal.
      if (gone(opt) || (re == fmt::Error::kIo && confirmed_gone(opt))) {
        is_removed = true;
        break;
      }
      if (re == fmt::Error::kIo)  // an I/O error on a drive that still answers: this item fails (nothing of it is kept), the job goes on
        return fail(Status::kIo, "preset " + p.key + ": sample '" + s.name + "': I/O error while reading the source");
      if (re != fmt::Error::kNone)
        return skip(ReasonCode::kDamaged, "preset " + p.key + ": sample '" + s.name + "': cannot read its data (" + fmt::to_string(re) + ")");
      fmt::SampleInput in;
      in.name = s.name;
      in.format = s.format;
      in.frame_count = s.frames;
      in.data = pcm;
      if (fmt::Error e = writer.add_sample(inst.value(), in); e != fmt::Error::kNone) {
        ReasonCode rc;
        const Status st = status_for_add_sample_error(e, &rc);
        if (st == Status::kSkipped) return skip(rc, "preset " + p.key + ": sample '" + s.name + "': " + fmt::to_string(e));
        return fail(st, "preset " + p.key + ": cannot add sample '" + s.name + "': " + fmt::to_string(e));
      }
      ++t_samples;
      t_bytes += s.frames * s.format.channels * (s.format.bits_per_sample / 8u);
    }
    if (is_removed) {
      // The timbre in progress is dropped whole (its instrument and every sample block already encoded for it).
      if (writer.discard_last_instrument() != fmt::Error::kNone) return fail(Status::kIo, "cannot discard the timbre in progress");
      break;
    }
    for (const auto& r : p.regions) {
      if (writer.add_region(inst.value(), r) != fmt::Error::kNone)
        return skip(ReasonCode::kDamaged, "preset " + p.key + ": a region does not fit the instrument model");
    }
    regions += p.regions.size();
    samples += t_samples;
    res.converted_bytes += t_bytes;
    ++finished;
    if (timbre_hook(opt, "timbre-done", pi)) return fail(Status::kAborted, "stopped at timbre-done");
    ++pend;
    pend_bytes += opt.checkpoint_size_fn ? opt.checkpoint_size_fn(pi, t_bytes) : t_bytes;
    if (!has_previous && finished < n_presets && regions > 0 && (pend >= every || pend_bytes >= every_bytes)) {
      ImportResult r = round(true);
      if (r.status == Status::kOk) {
        merge(r, false);
        ++res.checkpoints;
        pend = 0;
        pend_bytes = 0;
        if (opt.on_checkpoint) opt.on_checkpoint(res);
        if (timbre_hook(opt, "checkpoint-published", finished)) return fail(Status::kAborted, "stopped at checkpoint-published");
      } else if (source_failure(r)) {
        is_removed = true;  // the staging directory is already removed; the finished timbres are published below without the source
      } else {
        return r;  // earlier checkpoints stay published
      }
    }
  }

  auto finish_removed = [&]() -> ImportResult {
    if (!has_previous && finished > pub_count && regions > 0) {
      ImportResult r = round(false);
      if (r.status == Status::kOk) merge(r, true);
      else if (r.status == Status::kAborted) return r;
    }
    res.bundle_created = any_created;
    res.current_flipped = any_flipped;
    res.sample_count = samples;
    res.region_count = regions;
    removed(res, "reading " + opt.sfz_path);
    return res;
  };
  if (is_removed) return finish_removed();
  if (regions == 0) return skip(ReasonCode::kDamaged, "no regions in SF2 file");
  res.sample_count = samples;
  res.region_count = regions;

  // Story 6.3: build-only mode verifies in memory against a fresh parse of the source and hands the writer to the sink.
  if (opt.bundle_sink) {
    ropt.source_timbres = n_presets;
    finish_in_memory(ropt, res, writer, verify_for(finished, 0, true, std::string()));
    if (res.status == Status::kOk) res.timbres_published = finished;
    return res;
  }

  // 4-8. Stage, verify against a fresh parse of the source, publish, flip.
  ImportResult last = round(true);
  if (last.status == Status::kOk) {
    merge(last, true);
    res.bundle_created = any_created;
    res.current_flipped = any_flipped;
    return res;
  }
  if (source_failure(last)) return finish_removed();
  return last;
}

CliWriterGuard::CliWriterGuard(const std::string& root) {
  const std::string lib = (fs::path(root) / host::kLibDirName).string();
  const std::string path = lib + "/" + kServiceLockFileName;
  // Always created and always held shared for the whole run: a service that starts during this import then cannot take its exclusive lock, and so cannot
  // reconcile (delete `.staging`) under the running import. A root that does not exist is left to the importer's own error.
  if (::mkdir(lib.c_str(), 0755) != 0 && errno != EEXIST) {
    if (errno != ENOENT && errno != ENOTDIR) {
      ok_ = false;
      err_ = err_text("cannot create " + lib);
    }
    return;
  }
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd_ < 0) {
    ok_ = false;
    err_ = err_text("cannot open " + path);
    return;
  }
  if (::flock(fd_, LOCK_SH | LOCK_NB) != 0) {
    ok_ = false;
    err_ = errno == EWOULDBLOCK
               ? "the librarian service is running on " + root + " (it owns " + lib +
                     " and the catalog): import through the Library API (ImportRequest), or stop the service first"
               : err_text("cannot lock " + path);
    ::close(fd_);
    fd_ = -1;
  }
}

CliWriterGuard::~CliWriterGuard() {
  if (fd_ >= 0) ::close(fd_);
}

}  // namespace velora::librarian
