#include "velora/librarian/import.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>

#include "velora/format/bundle.hpp"
#include "velora/format/sf2_import.hpp"
#include "velora/format/sfz_import.hpp"
#include "velora/format/wav_source.hpp"
#include "velora/host/library_resolver.hpp"

namespace velora::librarian {
namespace fs = std::filesystem;
namespace fmt = velora::format;

const char* to_string(Stage s) {
  switch (s) {
    case Stage::kStaged: return "staged";
    case Stage::kVerified: return "verified";
    case Stage::kFsynced: return "fsynced";
    case Stage::kRenamed: return "renamed";
    case Stage::kLinkCreated: return "link-created";
    case Stage::kFlipped: return "flipped";
  }
  return "?";
}

bool parse_stage(const std::string& name, Stage& out) {
  for (int i = 0; i <= static_cast<int>(Stage::kFlipped); ++i)
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
  // Category metadata is part of the staged directory, hence of the atomic publish.
  auto write_category = [&](const std::string& dir) -> bool {
    const std::string cat = dir + "/" + kCategoryFileName, tmp = cat + ".tmp";
    Fd f(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644));
    const std::string text = opt.category + "\n";
    return f.ok() && ::write(f.fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()) &&
           full_sync(f.fd) && ::rename(tmp.c_str(), cat.c_str()) == 0;
  };
  if (!opt.category.empty() && !write_category(stage_dir)) {
    cleanup();
    return fail(Status::kIo, err_text("write category"));
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
  if (opt.fault.corrupt_staged_before_verify) {
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
    res.library_id = fmt::uuid_to_string(rd.library_id());
    res.bundle_id = fmt::to_hex(rd.bundle_id());
    return {};
  };
  if (std::string why = verify(); !why.empty()) {
    cleanup();
    return fail(Status::kVerifyFailed, why);
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

  if (opt.sfz_path.empty() || opt.root.empty())
    return fail(Status::kInvalidArgument, "an SFZ file and a data root are required");
  if (!opt.category.empty()) {
    for (unsigned char c : opt.category)
      if (c < 0x20 || c == 0x7f || c == '/') return fail(Status::kInvalidArgument, "invalid category");
    if (opt.category.size() > 128) return fail(Status::kInvalidArgument, "category too long");
  }

  {
    struct stat st;
    if (::stat(opt.root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
      return fail(Status::kInvalidArgument, "the data root " + opt.root + " is not an existing directory");
  }

  // 1. Parse (no audio, no filesystem writes).
  fmt::SfzParseResult parsed = fmt::parse_sfz_file(opt.sfz_path);
  res.source_region_count = parsed.source_region_count;
  for (const auto& a : parsed.allowlisted) res.allowlisted.push_back({a.opcode, a.file, a.line});
  for (const auto& w : parsed.warnings) res.warnings.push_back({"ignored_opcode", w.opcode, w.file, w.line, w.message, {}});
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
  if (parsed.regions.empty()) {
    res.status = Status::kSkipped;
    res.reason_code = ReasonCode::kDamaged;
    res.failing_path = opt.sfz_path;
    res.reasons.push_back(opt.sfz_path + ": no regions in SFZ file");
    return res;
  }

  std::error_code ec;
  const fs::path sfz_abs = fs::weakly_canonical(fs::absolute(opt.sfz_path, ec), ec);
  if (ec) return fail(Status::kIo, "cannot resolve " + opt.sfz_path);
  const fs::path base = opt.base_dir.empty() ? sfz_abs.parent_path()
                                              : fs::weakly_canonical(fs::absolute(opt.base_dir, ec), ec);
  const fs::path rel = sfz_abs.lexically_relative(base);
  if (rel.empty() || *rel.begin() == "..")
    return fail(Status::kInvalidArgument, "the SFZ file is not inside the base directory");

  // 2. Directories and the exclusive lock.
  RootLock rl;
  if (!lock_root(opt, res, rl)) return res;

  // 3. Build the bundle: load each sample once, encode, drop it again.
  fmt::BundleWriter writer;
  if (writer.set_source_id(sfz_abs.generic_string()) != fmt::Error::kNone)
    return fail(Status::kInvalidArgument, "empty source id");
  auto inst = writer.add_instrument(sfz_abs.stem().string(), rel.generic_string());
  if (!inst) return fail(Status::kInvalidArgument, std::string("instrument key: ") + fmt::to_string(inst.error()));
  std::vector<AudioProblem> audio_problems;
  Status infra_status = Status::kIo;
  std::string infra_problem;  // add_sample failed for a reason that is not about the WAV
  std::vector<std::uint64_t> frames(parsed.samples.size(), 0);
  for (std::size_t i = 0; i < parsed.samples.size(); ++i) {
    const auto& s = parsed.samples[i];
    auto w = fmt::load_wav_file(s.path, kStrictWav);
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
  res.sample_count = parsed.samples.size();
  res.region_count = regions.size();

  // 4-8. Stage, verify, publish, flip.
  auto verify_content = [&](const fmt::BundleReader& rd) -> std::string {
    if (rd.instruments().size() != 1 || rd.samples().size() != parsed.samples.size())
      return "staged bundle has the wrong instrument/sample count";
    if (rd.instruments()[0].regions != regions)
      return "staged region table differs from the parsed region table";
    for (std::size_t i = 0; i < parsed.samples.size(); ++i) {
      auto w = fmt::load_wav_file(parsed.samples[i].path, kStrictWav);  // fresh decode of the source
      if (!w || !sample_matches(rd, i, w.value()))
        return "sample '" + parsed.samples[i].name + "' is not bit-identical to its source";
    }
    return {};
  };
  return stage_and_publish(opt, res, rl, writer, verify_content);
}


bool is_sf2_path(const std::string& path) {
  std::string ext = fs::path(path).extension().string();
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext == ".sf2";
}

ImportResult import_file(const ImportOptions& opt) { return is_sf2_path(opt.sfz_path) ? import_sf2(opt) : import_sfz(opt); }

ImportResult import_sf2(const ImportOptions& opt) {
  ImportResult res;
  res.sf2 = true;
  auto fail = [&](Status s, std::string why) -> ImportResult& { return fail_res(res, s, std::move(why)); };
  if (opt.sfz_path.empty() || opt.root.empty())
    return fail(Status::kInvalidArgument, "an SF2 file and a data root are required");
  if (!opt.category.empty()) {
    for (unsigned char c : opt.category)
      if (c < 0x20 || c == 0x7f || c == '/') return fail(Status::kInvalidArgument, "invalid category");
    if (opt.category.size() > 128) return fail(Status::kInvalidArgument, "category too long");
  }
  {
    struct stat st;
    if (::stat(opt.root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
      return fail(Status::kInvalidArgument, "the data root " + opt.root + " is not an existing directory");
  }

  // 1. Parse (no filesystem writes). Damaged beats unsupported; nothing of a skipped file is written.
  fmt::Sf2ParseResult parsed = fmt::parse_sf2_file(opt.sfz_path);
  auto skip = [&](ReasonCode rc, std::string why) -> ImportResult& {
    res.status = Status::kSkipped;
    res.reason_code = rc;
    if (rc == ReasonCode::kDamaged) res.failing_path = opt.sfz_path;
    res.reasons.push_back(opt.sfz_path + ": " + std::move(why));
    return res;
  };
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
  res.preset_count = parsed.presets.size();
  res.ignored_generators = parsed.stats.ignored_generators;
  res.ignored_modulators = parsed.stats.ignored_modulators;
  res.source_region_count = parsed.stats.zones;

  std::error_code ec;
  const fs::path abs = fs::weakly_canonical(fs::absolute(opt.sfz_path, ec), ec);
  if (ec) return fail(Status::kIo, "cannot resolve " + opt.sfz_path);

  // 2. Directories and the exclusive lock.
  RootLock rl;
  if (!lock_root(opt, res, rl)) return res;

  // 3. Build the bundle: one instrument per preset; each sample read once, encoded, dropped again.
  fmt::BundleWriter writer;
  if (writer.set_source_id(abs.generic_string()) != fmt::Error::kNone)
    return fail(Status::kInvalidArgument, "empty source id");
  std::vector<std::int32_t> pcm;
  std::size_t samples = 0, regions = 0;
  for (const auto& p : parsed.presets) {
    auto inst = writer.add_instrument(p.name, p.key);
    if (!inst) return fail(Status::kInvalidArgument, "instrument key " + p.key + ": " + fmt::to_string(inst.error()));
    for (const auto& s : p.samples) {
      if (fmt::Error e = parsed.samples->read(s, pcm); e != fmt::Error::kNone)
        return skip(ReasonCode::kDamaged, "preset " + p.key + ": sample '" + s.name + "': cannot read its data (" + fmt::to_string(e) + ")");
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
      ++samples;
    }
    for (const auto& r : p.regions) {
      if (writer.add_region(inst.value(), r) != fmt::Error::kNone)
        return skip(ReasonCode::kDamaged, "preset " + p.key + ": a region does not fit the instrument model");
      ++regions;
    }
  }
  if (regions == 0) return skip(ReasonCode::kDamaged, "no regions in SF2 file");
  res.sample_count = samples;
  res.region_count = regions;

  // 4-8. Stage, verify against a fresh parse of the source, publish, flip.
  auto verify_content = [&](const fmt::BundleReader& rd) -> std::string {
    fmt::Sf2ParseResult fresh = fmt::parse_sf2_file(opt.sfz_path);
    if (!fresh.ok() || fresh.presets.size() != parsed.presets.size()) return "the SF2 file changed while it was imported";
    if (rd.instruments().size() != fresh.presets.size()) return "staged bundle has the wrong instrument count";
    std::vector<std::int32_t> want, got;
    for (std::size_t i = 0; i < fresh.presets.size(); ++i) {
      const auto& fp = fresh.presets[i];
      const auto& ins = rd.instruments()[i];
      if (ins.key != fp.key || ins.name != fp.name) return "staged instrument " + fp.key + " has the wrong key or name";
      if (ins.regions != fp.regions || fp.regions != parsed.presets[i].regions)
        return "staged region table of " + fp.key + " differs from the parsed zones";
      if (ins.sample_count != fp.samples.size()) return "staged instrument " + fp.key + " has the wrong sample count";
      for (std::size_t k = 0; k < fp.samples.size(); ++k) {
        const auto& si = rd.samples()[ins.first_sample + k];
        if (!(si.format == fp.samples[k].format) || si.frame_count != fp.samples[k].frames)
          return "sample '" + fp.samples[k].name + "' of " + fp.key + " has the wrong format or length";
        if (fresh.samples->read(fp.samples[k], want) != fmt::Error::kNone) return "cannot re-read sample '" + fp.samples[k].name + "'";
        got.assign(want.size(), 0);
        if (rd.read_frames(ins.first_sample + k, 0, want.size(), got) != fmt::Error::kNone || got != want)
          return "sample '" + fp.samples[k].name + "' of " + fp.key + " is not bit-identical to its source";
      }
    }
    return {};
  };
  return stage_and_publish(opt, res, rl, writer, verify_content);
}

}  // namespace velora::librarian
