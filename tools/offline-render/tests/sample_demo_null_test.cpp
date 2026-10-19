// Env-gated null test of the sample engine against sfizz on REAL libraries (Story 1.15): the Salamander
// Grand Piano and a VSCO2 instrument cannot be committed, so this runs only where they and the oracle exist.
//
//   VELORA_SFZ_DEMO_DIR=~/libs ctest --test-dir build -R sample_demo_null --output-on-failure
//
// Needs: VELORA_SFZ_DEMO_DIR (a directory holding the libraries; every *.sfz below it is tested, at most
// VELORA_DEMO_MAX_SFZ, default 4) and an sfizz_render, found through VELORA_SFIZZ_RENDER or the default
// tools/oracle/.work build (tools/oracle/build-sfizz-render.sh). Without them it exits 77 (ctest: skipped).
// VELORA_DEMO_TOLERANCE_DB (default -60) is the null tolerance. A library using opcodes outside the
// Story 1.13 subset fails at import with the offending file:line, as in librarian_demo_acceptance.
#include <sys/stat.h>
#include <sys/wait.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "velora/librarian/import.hpp"

namespace fs = std::filesystem;

// Command lines are built for /bin/sh inside double quotes: refuse any value that could escape them.
static bool shell_safe(const std::string& v) {
  return !v.empty() && v.find_first_of("\"$`\\\n\r") == std::string::npos;
}

static std::string q(const std::string& s) { return "\"" + s + "\""; }

static int run(const std::string& cmd, std::string& out, const std::string& log) {
  const int rc = std::system((cmd + " > " + q(log) + " 2>&1").c_str());
  std::ifstream in(log);
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return rc == -1 ? -1 : (WIFEXITED(rc) ? WEXITSTATUS(rc) : -1);
}

int main() {
  const char* demo = std::getenv("VELORA_SFZ_DEMO_DIR");
  if (!demo || !*demo) {
    std::printf("skipped: VELORA_SFZ_DEMO_DIR not set\n");
    return 77;
  }
  std::string oracle = std::getenv("VELORA_SFIZZ_RENDER") ? std::getenv("VELORA_SFIZZ_RENDER") : ORACLE_DEFAULT_BIN;
  if (!fs::is_regular_file(oracle)) {
    std::printf("skipped: no sfizz_render (set VELORA_SFIZZ_RENDER or run tools/oracle/build-sfizz-render.sh)\n");
    return 77;
  }
  const char* tol_env = std::getenv("VELORA_DEMO_TOLERANCE_DB");
  const std::string tol = tol_env && *tol_env ? tol_env : "-60";
  char* tend = nullptr;
  const double tol_v = std::strtod(tol.c_str(), &tend);
  if (*tend != '\0' || !(tol_v < 0.0 && tol_v >= -400.0)) {
    std::printf("FAIL: VELORA_DEMO_TOLERANCE_DB must be a negative number of dB\n");
    return 1;
  }
  std::size_t max_sfz = 4;
  if (const char* max_env = std::getenv("VELORA_DEMO_MAX_SFZ")) {
    char* mend = nullptr;
    const unsigned long m = std::strtoul(max_env, &mend, 10);
    if (!*max_env || *mend != '\0' || m == 0) {
      std::printf("FAIL: VELORA_DEMO_MAX_SFZ must be a positive integer\n");
      return 1;
    }
    max_sfz = static_cast<std::size_t>(m);
  }
  if (!shell_safe(oracle) || !shell_safe(RENDER_ORACLE_SH) || !shell_safe(RENDER_EXE) || !shell_safe(NULLTEST_EXE) ||
      !shell_safe(OFFLINE_TEST_DIR) || !shell_safe(SUITE_DIR)) {
    std::printf("FAIL: a path contains characters that are not allowed in shell command lines\n");
    return 1;
  }

  std::vector<fs::path> files;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(demo, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
       it.increment(ec))
    if (it->is_regular_file() && it->path().extension() == ".sfz") files.push_back(it->path());
  std::sort(files.begin(), files.end());
  if (files.empty()) {
    std::printf("skipped: no .sfz below %s\n", demo);
    return 77;
  }
  if (files.size() > max_sfz) files.resize(max_sfz);

  const std::string midi = std::string(SUITE_DIR) + "/midi/demo_probe.mid";
  int failures = 0, n = 0;
  for (const auto& sfz : files) {
    const std::string work = std::string(OFFLINE_TEST_DIR) + "/sample_demo/" + std::to_string(n++);
    std::system(("rm -rf " + q(work) + " && mkdir -p " + q(work + "/root")).c_str());
    std::printf("== %s\n", sfz.c_str());
    if (!shell_safe(sfz.string())) {
      std::printf("  FAIL path contains characters that are not allowed in shell command lines\n");
      ++failures;
      continue;
    }
    velora::librarian::ImportOptions opt;
    opt.sfz_path = sfz.string();
    opt.root = work + "/root";
    const auto imp = velora::librarian::import_sfz(opt);
    if (!imp.ok()) {
      std::printf("  FAIL import:\n");
      for (const auto& r : imp.reasons) std::printf("    %s\n", r.c_str());
      ++failures;
      continue;
    }
    std::string out;
    const std::string ref = work + "/oracle.wav", cand = work + "/engine.wav";
    if (run("VELORA_SFIZZ_RENDER=" + q(oracle) + " " + q(RENDER_ORACLE_SH) + " " + q(sfz.string()) + " " + q(midi) + " " + q(ref) + " 2",
            out, work + "/oracle.log") != 0) {
      std::printf("  FAIL oracle render:\n%s\n", out.c_str());
      ++failures;
      continue;
    }
    const std::string key = sfz.filename().string();  // the librarian default base is the sfz's own directory
    if (run(q(RENDER_EXE) + " --midi " + q(midi) + " --out " + q(cand) + " --engine sample --bundle " +
                q(imp.bundle_dir + "/bundle.vlb") + " --instrument " + q(key) + " --gain-db -7.35 --oracle-block-frames 64 --tail-seconds 2.0",
            out, work + "/render.log") != 0) {
      std::printf("  FAIL engine render:\n%s\n", out.c_str());
      ++failures;
      continue;
    }
    std::printf("  %s", out.c_str());
    const int rc = run(q(NULLTEST_EXE) + " --reference " + q(ref) + " --candidate " + q(cand) + " --tolerance-db " + tol +
                           " --max-offset-frames 0 --check-excess --reference-decode sfizz-s16",
                       out, work + "/null.log");
    std::printf("%s", out.c_str());
    if (rc != 0) ++failures;
  }
  std::printf("%s: %d of %zu libraries failed\n", failures ? "FAIL" : "PASS", failures, files.size());
  return failures ? 1 : 0;
}
