// velora-ssd-qualify (Story 1.16): percentiles, a short run on a temporary directory (JSON report with the percentiles and the recommended head length), and the
// clear errors for a missing / empty directory and bad arguments. The 1 h board run is not done here.
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ssd_qualify.hpp"

namespace fs = std::filesystem;
using namespace velora::ssd_qualify;

static int g_fail = 0;
#define CHECK(c)                                               \
  do {                                                         \
    if (!(c)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

struct TmpDir {
  std::string path;
  TmpDir() {
    path = (fs::temp_directory_path() / "velora-ssdq-XXXXXX").string();
    if (!mkdtemp(path.data())) std::exit(1);
  }
  ~TmpDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

static void write_file(const std::string& path, std::size_t bytes) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  std::string chunk(4096, 'x');
  for (std::size_t n = 0; n < bytes; n += chunk.size()) f.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
}

static double json_number(const std::string& j, const std::string& key) {
  const std::size_t p = j.find("\"" + key + "\":");
  if (p == std::string::npos) return -1.0;
  return std::strtod(j.c_str() + p + key.size() + 3, nullptr);
}

static void test_percentile() {
  std::vector<std::uint64_t> v;
  for (std::uint64_t i = 1; i <= 10000; ++i) v.push_back(i);
  CHECK(percentile(v, 0.0) == 1 && percentile(v, 0.5) == 5000 && percentile(v, 0.99) == 9900 && percentile(v, 0.9999) == 9999 && percentile(v, 1.0) == 10000);
  CHECK(percentile({}, 0.5) == 0 && percentile({7}, 0.9999) == 7);
}

static void test_short_run_report() {
  TmpDir tmp;
  for (int i = 0; i < 3; ++i) write_file(tmp.path + "/f" + std::to_string(i) + ".bin", 1 << 20);
  write_file(tmp.path + "/tiny.bin", 100);  // shorter than a block: ignored
  Config cfg;
  cfg.dir = tmp.path;
  cfg.seconds = 0.4;
  cfg.rate_mib_s = 200;     // paced
  cfg.allow_cached = true;  // the temp directory may be tmpfs (no O_DIRECT)
  Report r;
  std::string err;
  CHECK(run(cfg, r, err) == Status::kOk);
  CHECK(r.files == 3 && r.reads > 0 && r.errors == 0 && r.bytes == r.reads * cfg.block_bytes);
  CHECK(r.min_ns <= r.median_ns && r.median_ns <= r.p99_ns && r.p99_ns <= r.p9999_ns && r.p9999_ns <= r.max_ns && r.min_ns > 0);
  CHECK(!r.p9999_meaningful);  // a 0.4 s run is not a qualification
  CHECK(r.no_cache == "O_DIRECT" || r.no_cache == "F_NOCACHE" || r.no_cache == "none");
  CHECK(r.achieved_mib_s <= 250.0);  // paced: the achieved rate does not exceed the target by much
  // Recommended head = p99.99 + 1 ms at 48 kHz and pitch ratio 2, through the same sizing as the loader.
  const double expect_ms = static_cast<double>(r.p9999_ns) * 1e-6 + 1.0;
  CHECK(std::fabs(r.recommended_head_ms - expect_ms) < 1e-9);
  CHECK(r.recommended_head_frames >= static_cast<std::uint64_t>(std::floor(expect_ms * 1e-3 * 48000.0 * 2.0)));
  const std::string j = to_json(r);
  for (const char* k : {"\"tool\": \"velora-ssd-qualify\"", "\"latency_ns\"", "\"min\"", "\"median\"", "\"p99\"", "\"p99_99\"", "\"max\"", "\"recommended_ms\"",
                        "\"recommended_frames\"", "\"no_cache\""})
    CHECK(j.find(k) != std::string::npos);
  CHECK(json_number(j, "reads") == static_cast<double>(r.reads));
  CHECK(json_number(j, "p99_99") == static_cast<double>(r.p9999_ns));
  CHECK(j.front() == '{' && j.find_last_of('}') != std::string::npos);
}

// Threads share the rate; symlinks and short files are not part of the set.
static void test_threads_and_symlinks() {
  TmpDir tmp;
  write_file(tmp.path + "/real.bin", 1 << 20);
  std::error_code ec;
  fs::create_symlink(tmp.path + "/real.bin", tmp.path + "/link.bin", ec);
  Config cfg;
  cfg.dir = tmp.path;
  cfg.seconds = 0.3;
  cfg.threads = 3;
  cfg.rate_mib_s = 0;
  cfg.allow_cached = true;
  Report r;
  std::string err;
  CHECK(run(cfg, r, err) == Status::kOk);
  CHECK(r.files == 1 && r.threads == 3 && r.reads > 0 && r.errors == 0);
  const std::string j = to_json(r);
  CHECK(j.find("\"queue_depth\": 3") != std::string::npos && j.find("\"late_issues\"") != std::string::npos);
  cfg.threads = 0;
  CHECK(run(cfg, r, err) == Status::kBadConfig);
}

static void test_cli() {
  TmpDir tmp;
  write_file(tmp.path + "/a.bin", 1 << 20);
  const std::string report = tmp.path + "/report.json";
  {
    std::ostringstream out, err;
    CHECK(cli_main({"--dir", tmp.path + "/nonexistent", "--seconds", "0.1"}, out, err) == 3);
    CHECK(err.str().find("does not exist") != std::string::npos && out.str().empty());
  }
  {
    TmpDir empty;
    std::ostringstream out, err;
    CHECK(cli_main({"--dir", empty.path, "--seconds", "0.1"}, out, err) == 3);
    CHECK(err.str().find("no regular file") != std::string::npos);
  }
  {
    std::ostringstream out, err;
    CHECK(cli_main({}, out, err) == 2 && err.str().find("--dir is required") != std::string::npos);
    std::ostringstream o2, e2;
    CHECK(cli_main({"--dir", tmp.path, "--seconds", "abc"}, o2, e2) == 2);
    std::ostringstream o3, e3;
    CHECK(cli_main({"--dir", tmp.path, "--bogus"}, o3, e3) == 2);
    std::ostringstream o5, e5;
    CHECK(cli_main({"--dir", tmp.path, "--block-kib", "99999999999"}, o5, e5) == 2 && e5.str().find("--block-kib") != std::string::npos);  // range-checked before any cast
    std::ostringstream o6, e6;
    CHECK(cli_main({"--dir", tmp.path, "--sample-rate", "4294967297"}, o6, e6) == 2);
    std::ostringstream o4, e4;
    CHECK(cli_main({"--dir", tmp.path, "--block-kib", "3"}, o4, e4) == 2);  // 3 KiB is not a multiple of the 4 KiB alignment
  }
  {
    std::ostringstream out, err;
    CHECK(cli_main({"--dir", tmp.path, "--seconds", "0.2", "--rate-mib-s", "100", "--allow-cached", "--margin-ms", "2", "--out", report}, out, err) == 0);
    std::ifstream f(report);
    std::stringstream ss;
    ss << f.rdbuf();
    CHECK(ss.str().find("\"p99_99\"") != std::string::npos && json_number(ss.str(), "margin_ms") == 2.0);
  }
}

int main() {
  test_percentile();
  test_short_run_report();
  test_threads_and_symlinks();
  test_cli();
  if (g_fail) {
    std::printf("ssd_qualify_test: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("ssd_qualify_test: ok\n");
  return 0;
}
