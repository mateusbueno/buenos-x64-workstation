#include "velora/librarian/preflight.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <set>

#include "velora/format/bundle.hpp"
#include "velora/format/byte_source.hpp"
#include "velora/format/hash.hpp"
#include "velora/format/instrument_model.hpp"
#include "velora/format/sf2_import.hpp"
#include "velora/format/sfz_import.hpp"
#include "velora/format/sysex_6op.hpp"
#include "velora/librarian/category_suggest.hpp"

namespace velora::librarian {
namespace fs = std::filesystem;
namespace fmt = velora::format;

const char* to_string(RowStatus s) {
  switch (s) {
    case RowStatus::kReady: return "ready";
    case RowStatus::kNeedsCategory: return "needs_category";
    case RowStatus::kSkippedUnsupportedFeatures: return "skipped_unsupported_features";
    case RowStatus::kSkippedDamaged: return "skipped_damaged";
    case RowStatus::kAlreadyImported: return "already_imported";
  }
  return "?";
}

std::uint64_t required_import_bytes(std::uint64_t pcm_total, std::size_t item_count) {
  if (item_count == 0) return 0;
  const std::uint64_t scaled = pcm_total > (~0ull - kImportReserveBytes) / kImportPcmFactor ? (~0ull - kImportReserveBytes) : pcm_total * kImportPcmFactor;
  return scaled + kImportReserveBytes;
}

namespace {

struct Counter {
  std::uint64_t bytes = 0;
};

bool importable(const fs::path& p) {
  std::string e = p.extension().string();
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e == ".sfz" || e == ".sf2" || e == ".syx";
}
bool is_sf2(const fs::path& p) {
  std::string e = p.extension().string();
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e == ".sf2";
}
bool is_syx(const fs::path& p) {
  std::string e = p.extension().string();
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e == ".syx";
}
bool has_wav_ext(const std::string& path) {
  std::string e = fs::path(path).extension().string();
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e == ".wav";
}

bool inside(const fs::path& root, const fs::path& p) {
  const fs::path rel = p.lexically_relative(root);
  return !rel.empty() && *rel.begin() != "..";
}

std::uint64_t file_size_of(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) ? static_cast<std::uint64_t>(st.st_size) : 0;
}

// A pread that counts what it reads.
class CountingFile {
 public:
  explicit CountingFile(const std::string& path, Counter& c) : c_(c) {
    fd_ = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);  // a FIFO or device must never block the scan: it is not a regular file, see below
    struct stat st;
    if (fd_ >= 0 && (::fstat(fd_, &st) != 0 || !S_ISREG(st.st_mode))) {
      ::close(fd_);
      fd_ = -1;
    } else if (fd_ >= 0) {
      size_ = static_cast<std::uint64_t>(st.st_size);
    }
  }
  ~CountingFile() {
    if (fd_ >= 0) ::close(fd_);
  }
  CountingFile(const CountingFile&) = delete;
  CountingFile& operator=(const CountingFile&) = delete;
  bool ok() const { return fd_ >= 0; }
  std::uint64_t size() const { return size_; }
  bool read(std::uint64_t off, void* dst, std::size_t n) {
    if (off > size_ || n > size_ - off) return false;
    auto* p = static_cast<std::uint8_t*>(dst);
    while (n > 0) {
      const ssize_t r = ::pread(fd_, p, n, static_cast<off_t>(off));
      if (r < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      if (r == 0) return false;
      c_.bytes += static_cast<std::uint64_t>(r);
      p += r;
      off += static_cast<std::uint64_t>(r);
      n -= static_cast<std::size_t>(r);
    }
    return true;
  }

 private:
  Counter& c_;
  int fd_ = -1;
  std::uint64_t size_ = 0;
};

// The byte source behind the SF2 parser: the reader's own file source plus a byte counter.
class CountingSrc final : public fmt::ByteSource {
 public:
  CountingSrc(std::shared_ptr<CountingFile> f) : f_(std::move(f)) {}
  std::uint64_t size() const override { return f_->size(); }
  const std::uint8_t* direct(std::uint64_t, std::size_t) const override { return nullptr; }
  fmt::Error read_at(std::uint64_t off, void* dst, std::size_t n) const override {
    if (off > f_->size() || n > f_->size() - off) return fmt::Error::kTruncated;
    return f_->read(off, dst, n) ? fmt::Error::kNone : fmt::Error::kIo;
  }

 private:
  std::shared_ptr<CountingFile> f_;
};

inline std::uint32_t le16(const std::uint8_t* p) { return static_cast<std::uint32_t>(p[0] | (p[1] << 8)); }
inline std::uint32_t le32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

bool format_supported(const fmt::PcmFormat& f) {
  if (f.channels < 1 || f.channels > 2) return false;
  if (f.sample_rate != 44100 && f.sample_rate != 48000 && f.sample_rate != 88200 && f.sample_rate != 96000) return false;
  if (f.is_float) return f.bits_per_sample == 32;
  return f.bits_per_sample == 16 || f.bits_per_sample == 24;
}

struct WavHead {
  fmt::Error err = fmt::Error::kNone;
  fmt::PcmFormat format;
  std::uint64_t frames = 0;
  std::uint64_t file_size = 0;
};

// The RIFF chunk walk of formatlib's parse_wav (strict: a data chunk longer than the file is truncated, except the 0xFFFFFFFF streaming marker), over
// pread: only chunk headers, the format chunk and nothing of the data are read.
WavHead wav_head(const std::string& path, Counter& cnt) {
  WavHead h;
  CountingFile f(path, cnt);
  if (!f.ok()) {
    h.err = fmt::Error::kIo;
    return h;
  }
  const std::uint64_t fsz = f.size();
  h.file_size = fsz;
  std::uint8_t hdr[12];
  if (fsz < 12 || !f.read(0, hdr, 12)) {
    h.err = fsz < 12 ? fmt::Error::kTruncated : fmt::Error::kIo;
    return h;
  }
  if (std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) {
    h.err = fmt::Error::kBadMagic;
    return h;
  }
  bool have_fmt = false;
  std::uint32_t tag = 0, rate = 0, align = 0, bits = 0, chans = 0;
  std::uint64_t pos = 12;
  while (pos + 8 <= fsz) {
    std::uint8_t ck[8];
    if (!f.read(pos, ck, 8)) {
      h.err = fmt::Error::kIo;
      return h;
    }
    std::uint64_t size = le32(ck + 4);
    const std::uint64_t body = pos + 8;
    if (std::memcmp(ck, "fmt ", 4) == 0) {
      if (size < 16 || body + size > fsz) {
        h.err = fmt::Error::kTruncated;
        return h;
      }
      std::uint8_t fm[40];
      const std::size_t take = size >= 40 ? 40 : 16;
      if (!f.read(body, fm, take)) {
        h.err = fmt::Error::kIo;
        return h;
      }
      tag = le16(fm);
      chans = le16(fm + 2);
      rate = le32(fm + 4);
      align = le16(fm + 12);
      bits = le16(fm + 14);
      if (tag == 0xFFFE) {
        if (size < 40) {
          h.err = fmt::Error::kCorruptHeader;
          return h;
        }
        tag = le16(fm + 24);
      }
      have_fmt = true;
    } else if (std::memcmp(ck, "data", 4) == 0) {
      if (!have_fmt) {
        h.err = fmt::Error::kCorruptHeader;
        return h;
      }
      if (size > fsz - body) {
        if (size != 0xFFFFFFFFu) {
          h.err = fmt::Error::kTruncated;
          return h;
        }
        size = fsz - body;
      }
      h.format.channels = static_cast<std::uint8_t>(chans > 255 ? 255 : chans);
      h.format.bits_per_sample = static_cast<std::uint8_t>(bits > 255 ? 255 : bits);
      h.format.sample_rate = rate;
      h.format.is_float = (tag == 3);
      if ((tag != 1 && tag != 3) || !format_supported(h.format)) {
        h.err = fmt::Error::kUnsupportedFormat;
        return h;
      }
      const std::uint32_t bytes_per = bits / 8;
      if (align != bytes_per * chans) {
        h.err = fmt::Error::kCorruptHeader;
        return h;
      }
      h.frames = (size / bytes_per) / chans;
      return h;
    }
    size += size & 1;
    if (size > fsz - body) {
      h.err = fmt::Error::kTruncated;
      return h;
    }
    pos = body + size;
  }
  h.err = fmt::Error::kTruncated;  // no data chunk
  return h;
}

struct Timbre {
  std::string name, key;
  std::uint64_t size = 0, pcm = 0;
  fmt::Category suggested = fmt::Category::kNone;
};

struct Analysis {
  std::vector<FileDep> deps;
  bool skipped = false;
  RowStatus skip_status = RowStatus::kSkippedDamaged;
  std::string detail;
  bool sf2 = false;
  std::string format = "sfz";
  std::string library_id;
  std::vector<Timbre> timbres;  // skipped: exactly one, with an empty key and pcm 0
};

std::uint64_t pcm_of(const fmt::PcmFormat& f, std::uint64_t frames) {
  return frames * f.channels * (static_cast<std::uint64_t>(f.bits_per_sample) / 8);
}

void add_dep(Analysis& a, const std::string& path) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return;
  FileDep d;
  d.path = path;
  d.size = static_cast<std::uint64_t>(st.st_size);
#ifdef __APPLE__
  d.mtime_ns = static_cast<std::int64_t>(st.st_mtimespec.tv_sec) * 1000000000ll + st.st_mtimespec.tv_nsec;
#else
  d.mtime_ns = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000000000ll + st.st_mtim.tv_nsec;
#endif
  a.deps.push_back(std::move(d));
}

// `root` null = not confined (the space estimate of an import).
Analysis analyze_sfz(const fs::path& file, const fs::path* root, Counter& cnt) {
  Analysis a;
  add_dep(a, file.string());
  const std::string path = file.string();
  const fmt::Category sugg = suggest_category(suggest_input_for_path(path, sfz_metadata_strings(path))).value_or(fmt::Category::kNone);
  const std::uint64_t sfz_size = file_size_of(path);
  cnt.bytes += std::min<std::uint64_t>(sfz_size, 1u << 20);  // sfz_metadata_strings reads at most 1 MiB
  Timbre t;
  t.name = file.stem().string();
  t.key = file.filename().string();  // the importer's instrument_key: the path relative to the file's own directory
  t.suggested = sugg;
  a.library_id = fmt::uuid_to_string(fmt::library_id_from_source(file.generic_string()));
  auto skip = [&](RowStatus st, std::string why) {
    a.skipped = true;
    a.skip_status = st;
    a.detail = std::move(why);
    t.key.clear();
    t.pcm = 0;
    t.size = sfz_size;
    a.timbres.assign(1, t);
    return a;
  };

  fmt::SfzParseResult parsed = fmt::parse_sfz_file(path);
  for (const std::string& inc : parsed.included_files) {
    cnt.bytes += file_size_of(inc);
    if (inc != path) add_dep(a, inc);
  }
  if (!parsed.ok()) {
    bool damaged = false;
    for (const auto& i : parsed.issues)
      if (!fmt::is_unsupported_feature(i.kind)) damaged = true;
    // The text of an issue can quote the content of a file; for a file outside the source root (an #include that leaves it) only the fact is reported.
    const auto& first = parsed.issues.front();
    std::error_code ec0;
    const fs::path where = fs::weakly_canonical(fs::absolute(first.file, ec0), ec0);
    const bool outside = root && !ec0 && !inside(*root, where);
    return skip(damaged ? RowStatus::kSkippedDamaged : RowStatus::kSkippedUnsupportedFeatures,
                outside ? path + ": a file outside the source has a problem (details withheld)" : first.to_string());
  }
  fmt::drop_never_note_triggered(parsed);  // Story 13.2 / 13.7: regions nothing can start (hikey=-1, no controller trigger) are not written
  if (parsed.regions.empty()) return skip(RowStatus::kSkippedDamaged, path + ": no regions in SFZ file");

  std::vector<std::uint64_t> frames(parsed.samples.size(), 0);
  std::uint64_t disk = sfz_size, pcm = 0;
  bool damaged = false, unsupported = false;
  std::string first_damaged, first_unsupported;
  for (std::size_t i = 0; i < parsed.samples.size(); ++i) {
    const auto& s = parsed.samples[i];
    std::error_code ec;
    const fs::path real = fs::canonical(s.path, ec);
    if (ec) {
      damaged = true;
      if (first_damaged.empty()) first_damaged = s.ref_file + ":" + std::to_string(s.ref_line) + ": " + s.path + ": cannot read sample file";
      continue;
    }
    if (root && !inside(*root, real)) {  // a link that leaves the source: nothing outside the volume is read
      damaged = true;
      if (first_damaged.empty()) first_damaged = s.ref_file + ":" + std::to_string(s.ref_line) + ": " + s.path + ": the sample lies outside the source";
      continue;
    }
    const WavHead h = wav_head(real.string(), cnt);
    if (h.err != fmt::Error::kNone) {
      const bool unsup = h.err == fmt::Error::kUnsupportedFormat || (h.err == fmt::Error::kBadMagic && !has_wav_ext(s.path));
      (unsup ? unsupported : damaged) = true;
      std::string& slot = unsup ? first_unsupported : first_damaged;
      if (slot.empty())
        slot = s.ref_file + ":" + std::to_string(s.ref_line) + ": " + s.path + (unsup ? ": unsupported audio format" : std::string(": damaged WAV file (") + fmt::to_string(h.err) + ")");
      continue;
    }
    frames[i] = h.frames;
    add_dep(a, real.string());
    disk += h.file_size;
    pcm += pcm_of(h.format, h.frames);
  }
  if (damaged) return skip(RowStatus::kSkippedDamaged, first_damaged);  // damage beats unsupported, as in the importer
  if (unsupported) return skip(RowStatus::kSkippedUnsupportedFeatures, first_unsupported);
  for (std::size_t i = 0; i < parsed.regions.size(); ++i) {
    fmt::Region r = parsed.regions[i];
    std::string why;
    if (!fmt::resolve_loop_points(r, parsed.loops[i], frames[r.sample], &why))
      return skip(RowStatus::kSkippedDamaged, parsed.region_sources[i].file + ":" + std::to_string(parsed.region_sources[i].line) + ": " + why);
    if (fmt::validate_region(r, frames[r.sample]) != fmt::Error::kNone)
      return skip(RowStatus::kSkippedDamaged, parsed.region_sources[i].file + ":" + std::to_string(parsed.region_sources[i].line) + ": region values do not fit its sample");
  }
  t.size = disk;
  t.pcm = pcm;
  a.timbres.assign(1, t);
  return a;
}

Analysis analyze_sf2(const fs::path& file, Counter& cnt) {
  Analysis a;
  a.sf2 = true;
  a.format = "sf2";
  const std::string path = file.string();
  a.library_id = fmt::uuid_to_string(fmt::library_id_from_source(file.generic_string()));
  auto skip = [&](RowStatus st, std::string why) {
    a.skipped = true;
    a.skip_status = st;
    a.detail = std::move(why);
    Timbre t;
    t.name = file.stem().string();
    t.size = file_size_of(path);
    t.suggested = suggest_category(suggest_input_for_path(path, {})).value_or(fmt::Category::kNone);
    a.timbres.assign(1, t);
    return a;
  };
  add_dep(a, path);
  auto cf = std::make_shared<CountingFile>(path, cnt);
  if (!cf->ok()) return skip(RowStatus::kSkippedDamaged, path + ": cannot open");
  fmt::Sf2ParseResult parsed = fmt::parse_sf2(std::make_shared<CountingSrc>(cf));
  if (!parsed.ok()) {
    const bool damaged = parsed.damaged();
    return skip(damaged ? RowStatus::kSkippedDamaged : RowStatus::kSkippedUnsupportedFeatures, path + ": " + parsed.issues.front().message);
  }
  if (parsed.presets.empty()) return skip(RowStatus::kSkippedDamaged, path + ": no regions in SF2 file");
  for (const auto& p : parsed.presets) {
    Timbre t;
    t.name = p.name;
    t.key = p.key;
    for (const auto& s : p.samples) t.pcm += pcm_of(s.format, s.frames);
    t.size = t.pcm;
    t.suggested = suggest_category(suggest_input_for_path(path, {p.name})).value_or(fmt::Category::kNone);
    a.timbres.push_back(std::move(t));
  }
  return a;
}

Analysis analyze_sysex(const fs::path& file, Counter& cnt) {
  Analysis a;
  a.format = "sysex";
  const std::string path = file.string();
  a.library_id = fmt::uuid_to_string(fmt::library_id_from_source(file.generic_string()));
  auto skip = [&](RowStatus st, std::string why) {
    a.skipped = true;
    a.skip_status = st;
    a.detail = std::move(why);
    Timbre t;
    t.name = file.stem().string();
    t.size = file_size_of(path);
    t.suggested = fmt::Category::kSynthPoly;
    a.timbres.assign(1, t);
    return a;
  };
  add_dep(a, path);
  auto cf = std::make_shared<CountingFile>(path, cnt);
  if (!cf->ok()) return skip(RowStatus::kSkippedDamaged, path + ": cannot open");

  std::vector<uint8_t> bytes(cf->size());
  if (!cf->read(0, bytes.data(), bytes.size())) {
    return skip(RowStatus::kSkippedDamaged, path + ": cannot read");
  }

  fmt::SysexParseResult parsed = fmt::parse_sysex_memory(bytes);
  if (!parsed.ok() || parsed.voices.empty()) {
    return skip(RowStatus::kSkippedDamaged, path + ": " + fmt::kSysexDamagedMessage);
  }
  for (const auto& v : parsed.voices) {
    Timbre t;
    t.name = v.name;
    t.key = std::to_string(v.slot);
    t.pcm = 0; // Pure parametric synth timbre, zero PCM bytes
    t.size = bytes.size() / parsed.voices.size();
    t.suggested = fmt::Category::kSynthPoly; // Synth Poly is default
    a.timbres.push_back(std::move(t));
  }
  return a;
}

Analysis analyze_file(const fs::path& file, const fs::path* root, Counter& cnt) {
  if (is_syx(file)) return analyze_sysex(file, cnt);
  return is_sf2(file) ? analyze_sf2(file, cnt) : analyze_sfz(file, root, cnt);
}

class Scan {
 public:
  Scan(const PreflightOptions& o, PreflightResult& r, fs::path root) : opt_(o), res_(r), root_(std::move(root)) {
    max_files_ = (o.max_files == 0 || o.max_files > kPreflightMaxFiles) ? kPreflightMaxFiles : o.max_files;
  }

  void run() {
    std::vector<fs::directory_entry> top;
    if (!list(root_, top)) return;
    // The files at the root form the first library, then one library per top-level folder (sorted by name).
    begin_library(res_.volume_label, {});
    for (const auto& e : top) {
      if (stop()) break;
      std::error_code ec;
      if (!is_dir_entry(e, ec)) entry(e, 0);
    }
    end_library();
    for (const auto& e : top) {
      if (stop()) break;
      std::error_code ec;
      if (!is_dir_entry(e, ec)) continue;
      const std::string name = e.path().filename().string();
      if (name.compare(0, 2, "._") == 0) continue;  // same filter as entry()
      begin_library(name, name);
      walk(e.path(), 1);
      end_library();
    }
  }

 private:
  bool stop() {
    if (halt_) return true;  // a hard bound (files, rows, entries) was reached; a folder cut at the depth cap only marks `truncated`
    if (opt_.cancel && opt_.cancel->load()) {
      res_.cancelled = true;
      return true;
    }
    return res_.cancelled;
  }
  static bool is_dir_entry(const fs::directory_entry& e, std::error_code& ec) {
    const fs::file_status st = e.symlink_status(ec);
    return !ec && fs::is_directory(st);  // a link to a directory is a symlink, not a directory: it is skipped by entry()
  }
  bool list(const fs::path& dir, std::vector<fs::directory_entry>& out) {
    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::none, ec), end;
    if (ec) {
      ++res_.unreadable_dirs;
      return false;
    }
    for (; it != end; it.increment(ec)) {
      if (ec) {
        ++res_.unreadable_dirs;
        break;
      }
      if (++res_.entries_visited > kPreflightMaxEntries) {
        res_.truncated = halt_ = true;
        break;
      }
      out.push_back(*it);
    }
    std::sort(out.begin(), out.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) { return a.path().filename().native() < b.path().filename().native(); });
    return true;
  }
  void begin_library(const std::string& name, const std::string& folder) {
    PreflightLibrary l;
    l.name = name;
    l.folder = folder;
    l.first_row = res_.rows.size();
    res_.libraries.push_back(std::move(l));
  }
  void end_library() {
    PreflightLibrary& l = res_.libraries.back();
    l.row_count = res_.rows.size() - l.first_row;
    if (l.row_count == 0) res_.libraries.pop_back();
  }
  void walk(const fs::path& dir, std::size_t depth) {
    if (depth > kPreflightMaxDepth) {
      res_.truncated = true;
      return;
    }
    std::vector<fs::directory_entry> ents;
    if (!list(dir, ents)) return;
    for (const auto& e : ents) {
      if (stop()) return;
      entry(e, depth);
    }
  }
  void entry(const fs::directory_entry& e, std::size_t depth) {
    const std::string fname = e.path().filename().string();
    if (fname.compare(0, 2, "._") == 0) return;  // macOS AppleDouble resource fork, not a file of the user
    std::error_code ec;
    const fs::file_status ls = e.symlink_status(ec);
    if (ec) return;
    if (fs::is_directory(ls)) {
      walk(e.path(), depth + 1);
      return;
    }
    fs::path target = e.path();
    if (fs::is_symlink(ls)) {
      if (!importable(e.path())) {
        // A link that is not an importable file: a link to a directory is never followed.
        std::error_code e2;
        if (fs::is_directory(fs::status(e.path(), e2))) ++res_.symlinks_skipped;
        return;
      }
      target = fs::canonical(e.path(), ec);
      if (ec || !fs::is_regular_file(target, ec) || !inside(root_, target)) {
        ++res_.symlinks_skipped;
        return;
      }
      // A link to a file inside the source is an alias: the file is listed once, under its own name, so its bytes are counted once.
      ++res_.aliases_skipped;
      return;
    } else if (!fs::is_regular_file(ls) || !importable(e.path())) {
      return;
    }
    if (res_.files_scanned >= max_files_ || res_.rows.size() >= kPreflightMaxRows) {
      res_.truncated = halt_ = true;
      return;
    }
    if (!seen_real_.insert(target.string()).second) {  // an alias of a file already listed: once, so its bytes are not counted twice
      ++res_.aliases_skipped;
      return;
    }
    ++res_.files_scanned;
    add_file(e.path(), target);
  }
  void add_file(const fs::path& shown, const fs::path& real) {
    Counter cnt;
    Analysis a = analyze_file(real, &root_, cnt);
    res_.bytes_read += cnt.bytes;
    res_.deps[real.string()] = std::move(a.deps);
    const std::string rel = shown.lexically_relative(root_).generic_string();
    for (Timbre& t : a.timbres) {
      PreflightRow r;
      r.path = real.string();
      r.rel_path = rel;
      r.name = t.name;
      r.format = a.format;
      r.library_id = a.library_id;
      r.instrument_key = t.key;
      r.size_bytes = t.size;
      r.pcm_bytes = a.skipped ? 0 : t.pcm;
      r.suggested = t.suggested;
      r.status = a.skipped ? a.skip_status : (t.suggested == fmt::Category::kNone ? RowStatus::kNeedsCategory : RowStatus::kReady);
      r.library = res_.libraries.size() - 1;
      r.detail = a.skipped ? a.detail : std::string();
      res_.rows.push_back(std::move(r));
    }
    if (opt_.on_file) opt_.on_file(res_.files_scanned);
  }

  const PreflightOptions& opt_;
  PreflightResult& res_;
  fs::path root_;
  std::set<std::string> seen_real_;
  bool halt_ = false;
  std::size_t max_files_ = kPreflightMaxFiles;
};

}  // namespace

PreflightResult preflight_scan(const PreflightOptions& opt) {
  const auto t0 = std::chrono::steady_clock::now();
  PreflightResult res;
  std::error_code ec;
  const fs::path root = fs::canonical(fs::path(opt.source), ec);
  if (opt.source.empty() || ec || !fs::is_directory(root, ec)) {
    res.error = "the source is not a readable folder: " + opt.source;
    return res;
  }
  res.source = root.string();
  res.volume_label = opt.volume_label.empty() ? root.filename().string() : opt.volume_label;
  if (res.volume_label.empty()) res.volume_label = "volume";
  Scan(opt, res, root).run();
  res.ok = true;
  res.duration_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
  return res;
}

ImportEstimate estimate_import(const std::string& file) {
  ImportEstimate e;
  Counter cnt;
  std::error_code ec;
  const fs::path real = fs::canonical(fs::path(file), ec);
  if (ec) {
    e.skipped = true;
    return e;
  }
  Analysis a = analyze_file(real, nullptr, cnt);
  e.bytes_read = cnt.bytes;
  e.skipped = a.skipped;
  e.deps = std::move(a.deps);
  if (!a.skipped)
    for (const Timbre& t : a.timbres) e.pcm_bytes += t.pcm;
  return e;
}

}  // namespace velora::librarian
