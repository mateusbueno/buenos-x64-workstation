#include "ssd_qualify.hpp"

#include <fcntl.h>
#include <cerrno>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>

#include "velora/host/head_sizing.hpp"

namespace fs = std::filesystem;

namespace velora::ssd_qualify {

const char* to_string(Status s) {
  switch (s) {
    case Status::kOk: return "ok";
    case Status::kBadDir: return "directory missing or unreadable";
    case Status::kNoFiles: return "no usable files in the directory";
    case Status::kOpenFailed: return "a file could not be opened";
    case Status::kNoCacheRefused: return "the file system refused the no-cache flag";
    case Status::kBadConfig: return "bad configuration";
    case Status::kReadFailed: return "reads failed";
  }
  return "?";
}

std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, double fraction) {
  if (sorted.empty()) return 0;
  if (fraction <= 0.0) return sorted.front();
  const double rank = std::ceil(fraction * static_cast<double>(sorted.size()) - 1e-9);  // the epsilon keeps 0.9999 * 10000 at rank 9999 (not 10000)
  const std::size_t i = static_cast<std::size_t>(std::min<double>(std::max(rank, 1.0), static_cast<double>(sorted.size()))) - 1;
  return sorted[i];
}

namespace {

constexpr std::size_t kMaxFiles = 256;  // files held open at once

struct File {
  int fd = -1;
  std::uint64_t blocks = 0;  // whole blocks in the file
};

struct Closer {
  std::vector<File>& files;
  ~Closer() {
    for (File& f : files)
      if (f.fd >= 0) ::close(f.fd);
  }
};

struct AlignedFree {
  void operator()(void* p) const { std::free(p); }
};

// Opens `path` bypassing the page cache. `mode` is set to what was applied. Returns the fd or -1 with errno (and `refused` when only the no-cache flag failed).
int open_nocache(const std::string& path, std::string& mode, bool allow_cached, bool& refused) {
  refused = false;
#if defined(__linux__)
  int fd = ::open(path.c_str(), O_RDONLY | O_DIRECT | O_CLOEXEC);
  if (fd >= 0) {
    mode = "O_DIRECT";
    return fd;
  }
  if (errno == EINVAL) {  // tmpfs and some network file systems do not support O_DIRECT
    refused = true;
    if (!allow_cached) return -1;
    fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) mode = "none";
    return fd;
  }
  return -1;
#else
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
#if defined(F_NOCACHE)
  if (::fcntl(fd, F_NOCACHE, 1) == 0) {
    mode = "F_NOCACHE";
    return fd;
  }
#endif
  refused = true;
  if (!allow_cached) {
    ::close(fd);
    return -1;
  }
  mode = "none";
  return fd;
#endif
}

}  // namespace

Status run(const Config& cfg, Report& out, std::string& error) {
  out = Report{};
  if (cfg.align_bytes == 0 || (cfg.align_bytes & (cfg.align_bytes - 1)) != 0 || cfg.block_bytes == 0 || cfg.block_bytes % cfg.align_bytes != 0 || cfg.threads == 0 || cfg.threads > 64 || !(cfg.seconds > 0.0) ||
      !(cfg.rate_mib_s >= 0.0) || cfg.sample_rate == 0 || !(cfg.max_pitch_ratio > 0.0) || !(cfg.margin_ms >= 0.0)) {
    error = "bad configuration: block_bytes must be a non-zero multiple of align_bytes (a power of two), seconds > 0, rate >= 0, sample rate > 0, pitch ratio > 0, margin >= 0";
    return Status::kBadConfig;
  }
  std::error_code ec;
  if (cfg.dir.empty() || !fs::is_directory(cfg.dir, ec)) {
    error = "directory '" + cfg.dir + "' does not exist or is not a directory";
    return Status::kBadDir;
  }
  std::vector<std::string> paths;
  for (fs::recursive_directory_iterator it(cfg.dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (!fs::is_regular_file(it->symlink_status(e2)) || e2) continue;  // symlinks are skipped (not followed)
    const std::uintmax_t size = it->file_size(e2);
    if (e2 || size < cfg.block_bytes) continue;  // a file shorter than one block cannot give an aligned read
    paths.push_back(it->path().string());
  }
  if (ec) {
    error = "cannot read directory '" + cfg.dir + "': " + ec.message();
    return Status::kBadDir;
  }
  if (paths.empty()) {
    error = "no regular file of at least " + std::to_string(cfg.block_bytes) + " bytes under '" + cfg.dir + "'";
    return Status::kNoFiles;
  }
  std::sort(paths.begin(), paths.end());
  if (paths.size() > kMaxFiles) {  // a bounded, seeded random subset: a library may hold hundreds of thousands of files
    std::uint64_t x = cfg.seed ? cfg.seed : 1;
    for (std::size_t i = paths.size() - 1; i > 0; --i) {  // Fisher-Yates with xorshift64*
      x ^= x >> 12;
      x ^= x << 25;
      x ^= x >> 27;
      std::swap(paths[i], paths[static_cast<std::size_t>((x * 0x2545F4914F6CDD1Dull) % (i + 1))]);
    }
    paths.resize(kMaxFiles);
    std::sort(paths.begin(), paths.end());
  }

  std::vector<File> files;
  Closer closer{files};
  std::string mode;
  for (const std::string& p : paths) {
    std::string m;
    bool refused = false;
    const int fd = open_nocache(p, m, cfg.allow_cached, refused);
    if (fd < 0 && !refused && (errno == EACCES || errno == EPERM)) continue;  // not readable by this user: not part of the set
    if (fd < 0) {
      error = std::string(refused ? "the file system refused the no-cache flag for '" : "cannot open '") + p + "'" +
              (refused ? " (use --allow-cached only for development; tmpfs does not support O_DIRECT)" : std::string(": ") + std::strerror(errno));
      return refused ? Status::kNoCacheRefused : Status::kOpenFailed;
    }
    if (mode.empty()) mode = m;
    else if (mode != m) mode = "mixed";
    const off_t len = ::lseek(fd, 0, SEEK_END);
    File f;
    f.fd = fd;
    f.blocks = len > 0 ? static_cast<std::uint64_t>(len) / cfg.block_bytes : 0;
    files.push_back(f);
    if (f.blocks == 0) {
      error = "'" + p + "' shrank below one block";
      return Status::kOpenFailed;
    }
  }

  if (files.empty()) {
    error = "no readable file under '" + cfg.dir + "'";
    return Status::kNoFiles;
  }

  using Clock = std::chrono::steady_clock;
  struct ThreadOut {
    std::vector<std::uint64_t> lat;
    std::uint64_t reads = 0, errors = 0, late = 0;
    bool alloc_failed = false;
  };
  const std::uint32_t nthreads = cfg.threads;
  std::vector<ThreadOut> outs(nthreads);
  // Each thread streams at rate / threads with its own aligned buffer and RNG: the queue depth of the device is `threads` (each read is synchronous).
  const double interval_s = cfg.rate_mib_s > 0.0 ? static_cast<double>(cfg.block_bytes) * nthreads / (cfg.rate_mib_s * 1024.0 * 1024.0) : 0.0;
  const auto t0 = Clock::now();
  const auto deadline = t0 + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(cfg.seconds));
  auto worker = [&](std::uint32_t id) {
    ThreadOut& o = outs[id];
    void* raw = nullptr;
    if (::posix_memalign(&raw, cfg.align_bytes, cfg.block_bytes) != 0) {
      o.alloc_failed = true;
      return;
    }
    std::unique_ptr<void, AlignedFree> buf(raw);
    std::uint64_t rng = (cfg.seed ? cfg.seed : 1) + 0x9E3779B97F4A7C15ull * id;
    auto next = [&rng]() {  // xorshift64*
      rng ^= rng >> 12;
      rng ^= rng << 25;
      rng ^= rng >> 27;
      return rng * 0x2545F4914F6CDD1Dull;
    };
    o.lat.reserve(1u << 16);
    for (;;) {
      const auto now = Clock::now();
      if (now >= deadline) break;
      if (interval_s > 0.0) {
        // Absolute schedule. A read that is slow does not delay the following ones, they are issued as soon as the previous one returns; a read issued more than one
        // interval late is counted in `late_issues`. The latency is measured from the issue of each read, so the queueing a late stream would have suffered
        // (coordinated omission) is NOT in the percentiles: `late_issues` says how much of it there was.
        const auto due = t0 + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(static_cast<double>(o.reads + o.errors) * interval_s));
        if (due > now) {
          if (due >= deadline) break;
          std::this_thread::sleep_until(due);
        } else if (now - due > std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(interval_s))) {
          ++o.late;
        }
      }
      const File& f = files[static_cast<std::size_t>(next() % files.size())];
      const std::uint64_t block = next() % f.blocks;
      const auto a = Clock::now();
      const ssize_t n = ::pread(f.fd, buf.get(), cfg.block_bytes, static_cast<off_t>(block * cfg.block_bytes));
      const auto b = Clock::now();
      if (n != static_cast<ssize_t>(cfg.block_bytes)) {
        ++o.errors;
        continue;
      }
      o.lat.push_back(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()));
      ++o.reads;
    }
  };
  {
    std::vector<std::thread> pool;
    for (std::uint32_t t = 1; t < nthreads; ++t) pool.emplace_back(worker, t);
    worker(0);
    for (std::thread& t : pool) t.join();
  }
  const double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
  std::vector<std::uint64_t> lat;
  std::uint64_t reads = 0, errors = 0, late = 0;
  for (ThreadOut& o : outs) {
    if (o.alloc_failed) {
      error = "cannot allocate the aligned read buffer";
      return Status::kReadFailed;
    }
    lat.insert(lat.end(), o.lat.begin(), o.lat.end());
    reads += o.reads;
    errors += o.errors;
    late += o.late;
  }
  if (reads == 0) {
    error = "no read completed (" + std::to_string(errors) + " failed)";
    return Status::kReadFailed;
  }
  std::sort(lat.begin(), lat.end());
  Report r;
  r.dir = cfg.dir;
  r.no_cache = mode.empty() ? "none" : mode;
  r.block_bytes = cfg.block_bytes;
  r.files = static_cast<std::uint32_t>(files.size());
  r.reads = reads;
  r.errors = errors;
  r.late_issues = late;
  r.threads = nthreads;
  r.bytes = reads * cfg.block_bytes;
  r.seconds = elapsed;
  r.target_mib_s = cfg.rate_mib_s;
  r.achieved_mib_s = elapsed > 0.0 ? static_cast<double>(r.bytes) / (1024.0 * 1024.0) / elapsed : 0.0;
  r.min_ns = lat.front();
  r.median_ns = percentile(lat, 0.5);
  r.p99_ns = percentile(lat, 0.99);
  r.p9999_ns = percentile(lat, 0.9999);
  r.max_ns = lat.back();
  double sum = 0;
  for (std::uint64_t v : lat) sum += static_cast<double>(v);
  r.mean_ns = sum / static_cast<double>(lat.size());
  r.p9999_meaningful = reads >= 100000;
  r.sample_rate = cfg.sample_rate;
  r.max_pitch_ratio = cfg.max_pitch_ratio;
  r.margin_ms = cfg.margin_ms;
  r.recommended_head_ms = static_cast<double>(r.p9999_ns) * 1e-6 + cfg.margin_ms;
  velora::host::HeadSizing s;
  s.p9999_ns = r.p9999_ns;
  s.margin_ns = static_cast<std::uint64_t>(std::llround(cfg.margin_ms * 1e6));
  r.recommended_head_frames = velora::host::head_length(s, cfg.sample_rate, cfg.max_pitch_ratio, ~std::uint64_t{0} >> 2).frames;
  out = std::move(r);
  return Status::kOk;
}

namespace {
std::string esc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') {
      o += '\\';
      o += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char b[8];
      std::snprintf(b, sizeof b, "\\u%04x", c);
      o += b;
    } else {
      o += c;
    }
  }
  return o;
}
}  // namespace

std::string to_json(const Report& r) {
  std::ostringstream o;
  o << std::fixed << std::setprecision(3);
  o << "{\n";
  o << "  \"tool\": \"velora-ssd-qualify\",\n";
  o << "  \"schema\": 1,\n";
  o << "  \"dir\": \"" << esc(r.dir) << "\",\n";
  o << "  \"no_cache\": \"" << r.no_cache << "\",\n";
  o << "  \"files\": " << r.files << ",\n";
  o << "  \"block_bytes\": " << r.block_bytes << ",\n";
  o << "  \"reads\": " << r.reads << ",\n";
  o << "  \"errors\": " << r.errors << ",\n";
  o << "  \"threads\": " << r.threads << ",\n";
  o << "  \"queue_depth\": " << r.threads << ",\n";
  o << "  \"late_issues\": " << r.late_issues << ",\n";
  o << "  \"bytes\": " << r.bytes << ",\n";
  o << "  \"seconds\": " << r.seconds << ",\n";
  o << "  \"target_mib_s\": " << r.target_mib_s << ",\n";
  o << "  \"achieved_mib_s\": " << r.achieved_mib_s << ",\n";
  o << "  \"latency_ns\": {\"min\": " << r.min_ns << ", \"median\": " << r.median_ns << ", \"p99\": " << r.p99_ns << ", \"p99_99\": " << r.p9999_ns
    << ", \"max\": " << r.max_ns << ", \"mean\": " << r.mean_ns << "},\n";
  o << "  \"p99_99_meaningful\": " << (r.p9999_meaningful ? "true" : "false") << ",\n";
  o << "  \"head\": {\"sample_rate\": " << r.sample_rate << ", \"max_pitch_ratio\": " << r.max_pitch_ratio << ", \"margin_ms\": " << r.margin_ms
    << ", \"recommended_ms\": " << r.recommended_head_ms << ", \"recommended_frames\": " << r.recommended_head_frames << "}\n";
  o << "}\n";
  return o.str();
}

namespace {
bool parse_double(const std::string& s, double& v) {
  char* end = nullptr;
  v = std::strtod(s.c_str(), &end);
  return end != s.c_str() && *end == '\0' && std::isfinite(v);
}
bool parse_u64(const std::string& s, std::uint64_t& v) {
  if (s.empty() || s[0] == '-') return false;
  char* end = nullptr;
  v = std::strtoull(s.c_str(), &end, 10);
  return *end == '\0';
}
void usage(std::ostream& err) {
  err << "usage: velora-ssd-qualify --dir <directory> [--seconds N] [--rate-mib-s N] [--block-kib N] [--threads N] [--sample-rate HZ] [--max-pitch-ratio R]\n"
         "                          [--margin-ms N] [--seed N] [--allow-cached] [--out report.json]\n"
         "Reads random aligned blocks of the files under <directory> with the page cache bypassed and prints a JSON latency report and the recommended head length.\n";
}
}  // namespace

int cli_main(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  Config cfg;
  std::string out_path;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto value = [&](std::string& v) {
      if (i + 1 >= args.size()) {
        err << "error: " << a << " needs a value\n";
        return false;
      }
      v = args[++i];
      return true;
    };
    std::string v;
    double d = 0;
    std::uint64_t u = 0;
    if (a == "--help" || a == "-h") {
      usage(out);
      return 0;
    } else if (a == "--allow-cached") {
      cfg.allow_cached = true;
    } else if (a == "--dir") {
      if (!value(cfg.dir)) return 2;
    } else if (a == "--out") {
      if (!value(out_path)) return 2;
    } else if (a == "--seconds" || a == "--rate-mib-s" || a == "--max-pitch-ratio" || a == "--margin-ms") {
      if (!value(v)) return 2;
      if (!parse_double(v, d)) {
        err << "error: " << a << " expects a number, got '" << v << "'\n";
        return 2;
      }
      (a == "--seconds" ? cfg.seconds : a == "--rate-mib-s" ? cfg.rate_mib_s : a == "--max-pitch-ratio" ? cfg.max_pitch_ratio : cfg.margin_ms) = d;
    } else if (a == "--block-kib" || a == "--sample-rate" || a == "--seed" || a == "--threads" || a == "--queue-depth") {
      if (!value(v)) return 2;
      if (!parse_u64(v, u)) {
        err << "error: " << a << " expects a non-negative integer, got '" << v << "'\n";
        return 2;
      }
      if (a == "--block-kib") {
        if (u < 1 || u > 65536) {  // 1 KiB .. 64 MiB, checked before the 32 bit byte count is formed
          err << "error: --block-kib must be between 1 and 65536\n";
          return 2;
        }
        cfg.block_bytes = static_cast<std::uint32_t>(u * 1024);
      } else if (a == "--sample-rate") {
        if (u < 1 || u > 768000) {
          err << "error: --sample-rate must be between 1 and 768000\n";
          return 2;
        }
        cfg.sample_rate = static_cast<std::uint32_t>(u);
      } else if (a == "--threads" || a == "--queue-depth") {
        if (u < 1 || u > 64) {
          err << "error: " << a << " must be between 1 and 64\n";
          return 2;
        }
        cfg.threads = static_cast<std::uint32_t>(u);
      } else {
        cfg.seed = u;
      }
    } else {
      err << "error: unknown argument '" << a << "'\n";
      usage(err);
      return 2;
    }
  }
  if (cfg.dir.empty()) {
    err << "error: --dir is required\n";
    usage(err);
    return 2;
  }
  Report rep;
  std::string error;
  const Status st = run(cfg, rep, error);
  if (st != Status::kOk) {
    err << "error: " << error << "\n";
    return st == Status::kBadConfig ? 2 : 3;
  }
  const std::string json = to_json(rep);
  if (!out_path.empty()) {
    std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
    f << json;
    if (!f) {
      err << "error: cannot write '" << out_path << "'\n";
      return 3;
    }
  } else {
    out << json;
  }
  if (!rep.p9999_meaningful) err << "note: " << rep.reads << " reads: p99.99 rests on fewer than 100000 reads and is not a qualification figure (run for hours on the board)\n";
  return 0;
}

}  // namespace velora::ssd_qualify
