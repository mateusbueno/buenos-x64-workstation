// velora-null-test --reference r.wav --candidate c.wav [--candidate c2.wav ...]
//                  --tolerance-db X --max-offset-frames N
//   or: --case file.case (key=value: reference, tolerance_db, max_offset_frames,
//   max_length_diff_frames, check_excess, reference_decode; reference path relative to the case file).
//   Explicit flags override the case file.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "nulltest.hpp"
#include "parse_util.hpp"

using namespace velora::offline;

static std::string trim(const std::string& s) {
  const auto a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

static bool load_case(const std::string& path, std::map<std::string, std::string>& kv, std::string& err) {
  std::ifstream in(path);
  if (!in) { err = "cannot open case file: " + path; return false; }
  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos) { err = "bad line in case file: " + line; return false; }
    kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
  }
  return true;
}

static int bad(const char* what) {
  std::fprintf(stderr, "error: invalid value for %s\n", what);
  return 2;
}

static int usage() {
  std::fprintf(stderr,
               "usage: velora-null-test --reference r.wav --candidate c.wav [--candidate ...]\n"
               "       --tolerance-db X --max-offset-frames N [--max-length-diff-frames N] [--check-excess]\n"
               "       [--reference-decode sfizz-s16]  |  --case file.case\n");
  return 2;
}

int main(int argc, char** argv) {
  std::string ref, case_file;
  std::vector<std::string> cands;
  NullParams p;
  bool have_tol = false, have_off = false, have_len = false, have_excess = false;
  std::string ref_decode;
  std::uint64_t n = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--check-excess") { p.check_excess = true; have_excess = true; continue; }
    if (i + 1 >= argc) return usage();
    const char* v = argv[++i];
    char* e = nullptr;
    if (a == "--reference") ref = v;
    else if (a == "--candidate") cands.push_back(v);
    else if (a == "--case") case_file = v;
    else if (a == "--reference-decode") ref_decode = v;
    else if (a == "--tolerance-db") {
      if (!parse_double(v, -400.0, 400.0, p.tolerance_db)) return bad("--tolerance-db");
      have_tol = true;
    } else if (a == "--max-offset-frames") {
      if (!parse_uint(v, 1000000, n)) return bad("--max-offset-frames");
      p.max_offset_frames = static_cast<std::uint32_t>(n);
      have_off = true;
    } else if (a == "--max-length-diff-frames") {
      if (!parse_uint(v, 1000000000ull, p.max_length_diff_frames)) return bad("--max-length-diff-frames");
      have_len = true;
    } else return usage();
  }
  if (!case_file.empty()) {
    std::map<std::string, std::string> kv;
    std::string err;
    if (!load_case(case_file, kv, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 2; }
    for (const auto& e : kv) {
      if (e.first != "reference" && e.first != "tolerance_db" && e.first != "max_offset_frames" &&
          e.first != "max_length_diff_frames" && e.first != "check_excess" && e.first != "reference_decode") {
        std::fprintf(stderr, "error: unknown key '%s' in case file\n", e.first.c_str());
        return 2;
      }
    }
    if (ref.empty() && kv.count("reference")) {
      ref = kv["reference"];
      const auto slash = case_file.find_last_of('/');
      if (!ref.empty() && ref[0] != '/' && slash != std::string::npos) ref = case_file.substr(0, slash + 1) + ref;
    }
    if (!have_tol) {
      if (!kv.count("tolerance_db")) { std::fprintf(stderr, "error: case file lacks tolerance_db\n"); return 2; }
      if (!parse_double(kv["tolerance_db"], -400.0, 400.0, p.tolerance_db)) return bad("tolerance_db");
    }
    if (!have_off && kv.count("max_offset_frames")) {
      if (!parse_uint(kv["max_offset_frames"], 1000000, n)) return bad("max_offset_frames");
      p.max_offset_frames = static_cast<std::uint32_t>(n);
    }
    if (!have_len && kv.count("max_length_diff_frames") &&
        !parse_uint(kv["max_length_diff_frames"], 1000000000ull, p.max_length_diff_frames))
      return bad("max_length_diff_frames");
    if (!have_excess && kv.count("check_excess")) {
      if (kv["check_excess"] != "0" && kv["check_excess"] != "1") return bad("check_excess");
      p.check_excess = kv["check_excess"] == "1";
    }
    if (ref_decode.empty() && kv.count("reference_decode")) ref_decode = kv["reference_decode"];
  } else if (!have_tol) {
    return usage();
  }
  if (ref.empty() || cands.empty()) return usage();
  if (!ref_decode.empty() && ref_decode != "sfizz-s16") return bad("reference_decode");

  WavData rw;
  std::string err;
  if (!read_wav(ref, rw, err)) { std::fprintf(stderr, "error: %s: %s\n", ref.c_str(), err.c_str()); return 1; }
  if (ref_decode == "sfizz-s16" && !undo_sfizz_s16(rw, err)) {
    std::fprintf(stderr, "error: %s: %s\n", ref.c_str(), err.c_str());
    return 1;
  }
  std::printf("reference: %s\n", ref.c_str());
  bool all = true;
  for (const auto& c : cands) {
    WavData cw;
    if (!read_wav(c, cw, err)) {
      std::printf("file: %s\n  result: FAIL (%s)\n", c.c_str(), err.c_str());
      all = false;
      continue;
    }
    const NullResult r = null_test(rw, cw, p);
    std::fputs(format_report(c, r, p).c_str(), stdout);
    all = all && r.pass;
  }
  std::printf("overall: %s\n", all ? "PASS" : "FAIL");
  return all ? 0 : 1;
}
