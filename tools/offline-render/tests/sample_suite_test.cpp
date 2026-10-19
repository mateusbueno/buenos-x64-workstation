// Null test of the sample engine against committed sfizz renders (Story 1.15), through the shipped tools:
//   librarian import (in process)  ->  velora-offline-render --engine sample  ->  velora-null-test --case
// Usage: sample_suite_test <case> <hermite|linear> [--negative gain|interp|strip]
//   no flag     the render must pass the case's null test (tolerance in golden/<case>.case)
//   --negative  a deliberately wrong render (1 dB gain error / the other interpolation / the Story 5.2 opcodes
//               stripped from the SFZ: keyswitches, CC crossfades and filters) must FAIL the same null test
//               on residual, proving the test can fail
//   cli-errors  as the case name: exercises the sample-engine argument errors of velora-offline-render
// Needs no sfizz: the goldens are committed. Regenerating them needs tools/oracle (see its README).
#include <sys/stat.h>
#include <sys/wait.h>

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

#include "velora/librarian/import.hpp"

#ifndef SUITE_DIR
#error "SUITE_DIR must point at tools/offline-render/tests/sample_suite"
#endif

static int g_fail = 0;
#define CHECK(c)                                                       \
  do {                                                                 \
    if (!(c)) {                                                        \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);         \
      ++g_fail;                                                        \
    }                                                                  \
  } while (0)

static std::string q(const std::string& s) { return "\"" + s + "\""; }

// Runs a shell command, captures stdout+stderr into `out`, returns the exit status (-1 on failure).
static int run(const std::string& cmd, std::string& out, const std::string& log) {
  const int rc = std::system((cmd + " > " + q(log) + " 2>&1").c_str());
  std::ifstream in(log);
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  if (rc == -1) return -1;
  return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

// Value of the counter printed as "<N> <label>" (e.g. "0 unplayable regions"); -1 when absent.
static long counter(const std::string& out, const std::string& label) {
  const std::size_t at = out.find(" " + label);
  if (at == std::string::npos) return -1;
  std::size_t b = at;
  while (b > 0 && out[b - 1] >= '0' && out[b - 1] <= '9') --b;
  if (b == at) return -1;
  return std::strtol(out.substr(b, at - b).c_str(), nullptr, 10);
}

// The SFZ text without any Story 5.2 opcode (sw_*, xfin_*/xfout_*, xf_cccurve, fil_type, cutoff, resonance): opcodes are the
// whitespace separated key=value words, headers and comments stay as they are.
static std::string strip_story52(const std::string& sfz, int& stripped) {
  auto drop = [](const std::string& w) {
    const std::string key = w.substr(0, w.find('='));
    return w.find('=') != std::string::npos &&
           (key.rfind("sw_", 0) == 0 || key.rfind("xfin_", 0) == 0 || key.rfind("xfout_", 0) == 0 || key == "xf_cccurve" ||
            key == "fil_type" || key == "cutoff" || key == "resonance");
  };
  std::string out;
  std::istringstream in(sfz);
  std::string line;
  stripped = 0;
  while (std::getline(in, line)) {
    if (line.rfind("//", 0) == 0) {  // comments stay as they are
      out += line + "\n";
      continue;
    }
    // Split into words and the whitespace between them, keep every separator, drop the dropped words with the blanks that led to them.
    std::size_t i = 0;
    while (i < line.size()) {
      const std::size_t ws = i;
      while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
      const std::size_t wb = i;
      while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
      const std::string blanks = line.substr(ws, wb - ws), word = line.substr(wb, i - wb);
      if (!word.empty() && drop(word)) {
        ++stripped;
        continue;
      }
      out += blanks + word;
    }
    out += "\n";
  }
  return out;
}

static void mkdirs(const std::string& p) {
  std::string cur;
  for (std::size_t i = 0; i <= p.size(); ++i) {
    if (i == p.size() || p[i] == '/') {
      if (!cur.empty()) ::mkdir(cur.c_str(), 0755);
    }
    if (i < p.size()) cur += p[i];
  }
}

static int cli_errors(const std::string& work) {
  std::string out;
  const std::string exe = q(RENDER_EXE);
  const std::string mid = q(std::string(SUITE_DIR) + "/midi/velocity_layers.mid");
  const std::string log = work + "/cli.log";
  CHECK(run(exe + " --midi " + mid + " --out " + q(work + "/x.wav") + " --engine sample", out, log) == 2);
  CHECK(out.find("needs --bundle and --instrument") != std::string::npos);
  CHECK(run(exe + " --midi " + mid + " --out " + q(work + "/x.wav") + " --bundle b.vlb --instrument k", out, log) == 2);
  CHECK(out.find("need --engine sample") != std::string::npos);
  CHECK(run(exe + " --midi " + mid + " --out " + q(work + "/x.wav") + " --engine sample --bundle " +
                q(work + "/nonexistent.vlb") + " --instrument k", out, log) == 1);
  CHECK(out.find("cannot open bundle") != std::string::npos);
  CHECK(run(exe + " --midi " + mid + " --out " + q(work + "/x.wav") + " --seed 3", out, log) == 2);  // sample-only option
  CHECK(out.find("need --engine sample") != std::string::npos);
  CHECK(run(exe + " --midi " + mid + " --out " + q(work + "/x.wav") + " --gain-db -3", out, log) == 2);
  CHECK(run(exe + " --midi " + mid + " --out " + q(work + "/x.wav") + " --engine sample --bundle b --instrument k --interpolation cubic",
            out, log) == 2);
  CHECK(out.find("invalid --interpolation") != std::string::npos);

  // A bundle that exists but not the instrument: nothing is substituted.
  velora::librarian::ImportOptions opt;
  opt.sfz_path = std::string(SUITE_DIR) + "/sfz/velocity_layers.sfz";
  opt.base_dir = std::string(SUITE_DIR) + "/sfz";
  opt.root = work + "/root";
  mkdirs(opt.root);
  const auto res = velora::librarian::import_sfz(opt);
  CHECK(res.ok());
  if (res.ok()) {
    CHECK(run(exe + " --midi " + mid + " --out " + q(work + "/x.wav") + " --engine sample --bundle " +
                  q(res.bundle_dir + "/bundle.vlb") + " --instrument NoSuch.sfz", out, log) == 1);
    CHECK(out.find("instrument not found") != std::string::npos);
  }
  return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  const std::string name = argv[1];
  const std::string work = std::string(OFFLINE_TEST_DIR) + "/sample_suite/" + name + (argc > 3 ? "_neg" : "");
  std::string out;
  std::system(("rm -rf " + q(work)).c_str());
  mkdirs(work);
  if (name == "cli-errors") return cli_errors(work);
  if (argc < 3) return 2;
  const std::string interp = argv[2];
  std::string negative;
  if (argc > 3) {
    if (argc != 5 || std::string(argv[3]) != "--negative" ||
        (std::string(argv[4]) != "gain" && std::string(argv[4]) != "interp" && std::string(argv[4]) != "strip")) {
      std::fprintf(stderr, "usage: sample_suite_test <case> <hermite|linear> [--negative gain|interp|strip]\n");
      return 2;
    }
    negative = argv[4];
  }

  // 1. Import the SFZ with the librarian into a scratch data root.
  velora::librarian::ImportOptions opt;
  opt.sfz_path = std::string(SUITE_DIR) + "/sfz/" + name + ".sfz";
  opt.base_dir = std::string(SUITE_DIR) + "/sfz";
  opt.root = work + "/root";
  mkdirs(opt.root);
  if (negative == "strip") {  // the same instrument without keyswitches, crossfades and filters (samples still resolve through base_dir)
    std::ifstream in(opt.sfz_path);
    int stripped = 0;
    std::string text = strip_story52(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), stripped);
    if (stripped == 0) {  // a negative that strips nothing would fail (or pass) for the wrong reason
      std::printf("FAIL: --negative strip removed no Story 5.2 opcode from %s\n", opt.sfz_path.c_str());
      return 2;
    }
    mkdirs(work + "/stripped");
    // The importer keeps samples inside the SFZ's directory: the stripped file gets its own copy of the samples.
    if (std::system(("cp -R " + q(std::string(SUITE_DIR) + "/sfz/samples") + " " + q(work + "/stripped/")).c_str()) != 0) {
      std::printf("FAIL: cannot copy the samples for the stripped instrument\n");
      return 2;
    }
    opt.sfz_path = work + "/stripped/" + name + ".sfz";
    opt.base_dir = work + "/stripped";
    std::ofstream(opt.sfz_path) << text;
  }
  const auto res = velora::librarian::import_sfz(opt);
  if (!res.ok()) {
    std::printf("import failed:\n");
    for (const auto& r : res.reasons) std::printf("  %s\n", r.c_str());
    return 1;
  }

  // 2. Render the committed MIDI through the sample engine. sfizz applies a -7.35 dB default master
  //    volume (Default::globalVolume); the engine's gain parameter compensates. The block-stall
  //    emulation mirrors an sfizz 1.2.3 defect (see engine/core/README.md).
  std::string gain = "-7.35", use_interp = interp;
  if (negative == "gain") gain = "-6.35";
  if (negative == "interp") use_interp = interp == "linear" ? "hermite" : "linear";
  const std::string cand = work + "/candidate.wav";
  const std::string render = q(RENDER_EXE) + " --midi " + q(std::string(SUITE_DIR) + "/midi/" + name + ".mid") +
                             " --out " + q(cand) + " --engine sample --bundle " + q(res.bundle_dir + "/bundle.vlb") +
                             " --instrument " + q(name + ".sfz") + " --interpolation " + use_interp +
                             " --gain-db " + gain + " --oracle-block-frames 64 --tail-seconds 1.0";
  int rc = run(render, out, work + "/render.log");
  std::printf("%s", out.c_str());
  CHECK(rc == 0);
  CHECK(counter(out, "unplayable regions") == 0);
  CHECK(counter(out, "failed)") == 0);
  CHECK(counter(out, "pool exhausted") == 0);

  // 3. Null test against the committed sfizz render with the case's tolerance.
  rc = run(q(NULLTEST_EXE) + " --case " + q(std::string(SUITE_DIR) + "/golden/" + name + ".case") + " --candidate " + q(cand),
           out, work + "/null.log");
  std::printf("%s", out.c_str());
  if (negative.empty()) {
    CHECK(rc == 0);
    CHECK(out.find("overall: PASS") != std::string::npos);
  } else {
    CHECK(rc == 1);
    CHECK(out.find("result: FAIL (residual above tolerance)") != std::string::npos);
  }
  return g_fail ? 1 : 0;
}
