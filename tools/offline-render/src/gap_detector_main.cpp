#include <cstdio>
#include <cstdlib>
#include <string>

#include "gap_detector.hpp"
#include "parse_util.hpp"
#include "wav.hpp"

using namespace velora::offline;

static int usage() {
  std::fprintf(stderr,
               "usage: velora-gap-detector --baseline BASE.wav --candidate CAND.wav [options]\n"
               "  options:\n"
               "    --switch-time SECONDS       Time in seconds where switch occurred (default: 0)\n"
               "    --max-drop-db DB            Max allowed drop in envelope below baseline (default: 6.0 dB)\n"
               "    --min-active-db DB          Baseline RMS floor to inspect (default: -60.0 dBFS)\n"
               "    --check-until-time SECONDS  Stop checking after this time (default: end of audio)\n");
  return 2;
}

int main(int argc, char** argv) {
  std::string base_path, cand_path;
  GapDetectorOptions opt;
  double switch_sec = 0.0;
  double check_until_sec = 0.0;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto val = [&](std::string& out) -> bool {
      if (i + 1 < argc) { out = argv[++i]; return true; }
      return false;
    };
    std::string v;
    if (a == "--baseline" && val(v)) base_path = v;
    else if (a == "--candidate" && val(v)) cand_path = v;
    else if (a == "--switch-time" && val(v)) {
      if (!parse_double(v, 0.0, 3600.0, switch_sec)) return usage();
    } else if (a == "--check-until-time" && val(v)) {
      if (!parse_double(v, 0.0, 3600.0, check_until_sec)) return usage();
    } else if (a == "--max-drop-db" && val(v)) {
      double d = 0;
      if (!parse_double(v, 0.01, 60.0, d)) return usage();
      opt.max_allowed_drop_db = static_cast<float>(d);
    } else if (a == "--min-active-db" && val(v)) {
      double d = 0;
      if (!parse_double(v, -140.0, 0.0, d)) return usage();
      opt.min_active_db = static_cast<float>(d);
    } else {
      return usage();
    }
  }

  if (base_path.empty() || cand_path.empty()) return usage();

  WavData baseline, candidate;
  std::string err;
  if (!read_wav(base_path, baseline, err)) {
    std::fprintf(stderr, "error reading baseline WAV '%s': %s\n", base_path.c_str(), err.c_str());
    return 1;
  }
  if (!read_wav(cand_path, candidate, err)) {
    std::fprintf(stderr, "error reading candidate WAV '%s': %s\n", cand_path.c_str(), err.c_str());
    return 1;
  }

  opt.switch_frame = static_cast<std::uint64_t>(std::llround(switch_sec * baseline.sample_rate));
  if (check_until_sec > 0.0) {
    opt.check_until_frame = static_cast<std::uint64_t>(std::llround(check_until_sec * baseline.sample_rate));
  }

  const GapReport rep = detect_gaps_and_truncation(baseline, candidate, opt);
  if (rep.pass) {
    std::printf("PASS: %s\n", rep.message.c_str());
    return 0;
  } else {
    std::fprintf(stderr, "FAIL: %s\n", rep.message.c_str());
    return 1;
  }
}
