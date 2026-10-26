#pragma once
// velora-ssd-qualify (Story 1.16): measures the read latency a sample stream would see on the real SSD and turns it into a recommended instrument head length.
//
// It reads random, aligned blocks from the files of a directory with the operating system's page cache bypassed (O_DIRECT on Linux, F_NOCACHE on macOS; aligned
// buffers), paced at a configured streaming rate for a number of seconds, and reports min / median / p99 / p99.99 / max latency as JSON. The recommended head
// length is the one engine/host/head_sizing.hpp computes from p99.99 + a margin at the given sample rate and highest pitch ratio.
//
// The default run is short (seconds) so that it can be tested anywhere. The qualification run of the board (1 h, 256 voices' worth of streaming, SSD at least 90 %
// full) is the documented procedure in tools/ssd-qualify/README.md; it is NOT run in CI and its result is dev-board work (a short run's p99.99 is not a measurement of
// the board: the report says how many reads the percentile rests on).
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace velora::ssd_qualify {

struct Config {
  std::string dir;                       // directory whose regular files (recursively) are read
  double seconds = 2.0;                  // how long to read
  std::uint32_t block_bytes = 128 * 1024;  // one read; a multiple of align_bytes
  std::uint32_t align_bytes = 4096;      // buffer, offset and length alignment (O_DIRECT needs the device's logical block size at most this)
  double rate_mib_s = 64.0;              // aggregate streaming rate to pace the reads at; 0 = as fast as possible
  std::uint32_t threads = 1;             // concurrent readers = the device queue depth (each read is synchronous); the rate is shared between them
  std::uint64_t seed = 1;
  bool allow_cached = false;             // dev/test: continue (and say so in the report) when the file system refuses the no-cache flag (tmpfs)
  // Head recommendation inputs.
  std::uint32_t sample_rate = 48000;
  double max_pitch_ratio = 2.0;
  double margin_ms = 1.0;
};

struct Report {
  std::string dir;
  std::string no_cache;  // "O_DIRECT", "F_NOCACHE" or "none" (allow_cached after a refusal)
  std::uint32_t block_bytes = 0, files = 0;
  std::uint64_t reads = 0, errors = 0, bytes = 0, late_issues = 0;
  std::uint32_t threads = 1;
  double seconds = 0, achieved_mib_s = 0, target_mib_s = 0;
  // Latencies in nanoseconds (per read, excluding the pacing sleep).
  std::uint64_t min_ns = 0, median_ns = 0, p99_ns = 0, p9999_ns = 0, max_ns = 0;
  double mean_ns = 0;
  bool p9999_meaningful = false;  // at least 100000 reads: below that p99.99 is just the max
  std::uint32_t sample_rate = 0;
  double max_pitch_ratio = 0, margin_ms = 0;
  std::uint64_t recommended_head_frames = 0;
  double recommended_head_ms = 0;  // p99.99 + margin
};

enum class Status : std::uint8_t { kOk = 0, kBadDir, kNoFiles, kOpenFailed, kNoCacheRefused, kBadConfig, kReadFailed };
const char* to_string(Status s);

// The value at `fraction` (0..1) of the sorted latencies (nearest rank: ceil(fraction * n)), 0 for an empty set.
std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, double fraction);

// Runs the measurement. On failure `error` says why (a clear message, e.g. naming the directory).
Status run(const Config& cfg, Report& out, std::string& error);

std::string to_json(const Report& r);

// The command line (args after the program name). Returns the process exit code: 0 ok, 2 usage / bad configuration, 3 a measurement error. JSON goes to `out`
// (or the file given by --out), diagnostics to `err`.
int cli_main(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

}  // namespace velora::ssd_qualify
