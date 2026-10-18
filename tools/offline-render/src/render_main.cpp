// velora-offline-render --midi in.mid --out out.wav [--engine test-tone] [--voices N]
//                       [--tail-seconds S] [--detune-cents C]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "parse_util.hpp"
#include "render.hpp"

using namespace velora::offline;

static int usage() {
  std::fprintf(stderr,
               "usage: velora-offline-render --midi in.mid --out out.wav [--engine test-tone]\n"
               "       [--voices N] [--tail-seconds S] [--detune-cents C (test only)]\n");
  return 2;
}

int main(int argc, char** argv) {
  std::string midi, out, engine = "test-tone";
  RenderOptions opt;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto val = [&](const char*& v) {
      if (i + 1 >= argc) return false;
      v = argv[++i];
      return true;
    };
    const char* v = nullptr;
    if (a == "--midi" && val(v)) midi = v;
    else if (a == "--out" && val(v)) out = v;
    else if (a == "--engine" && val(v)) engine = v;
    else if (a == "--voices" && val(v)) {
      std::uint64_t n = 0;
      if (!parse_uint(v, 4096, n) || n < 1) { std::fprintf(stderr, "error: invalid --voices\n"); return 2; }
      opt.voices = static_cast<std::uint32_t>(n);
    } else if (a == "--tail-seconds" && val(v)) {
      if (!parse_double(v, 0.0, 600.0, opt.tail_seconds)) { std::fprintf(stderr, "error: invalid --tail-seconds\n"); return 2; }
    } else if (a == "--detune-cents" && val(v)) {
      double c = 0;
      if (!parse_double(v, -4800.0, 4800.0, c)) { std::fprintf(stderr, "error: invalid --detune-cents\n"); return 2; }
      opt.detune_cents = static_cast<float>(c);
    } else return usage();
  }
  if (midi.empty() || out.empty()) return usage();
  if (engine != "test-tone") {
    std::fprintf(stderr, "error: unknown engine '%s' (only test-tone is available)\n", engine.c_str());
    return 2;
  }
  SmfResult smf = load_smf_file(midi);
  if (!smf.ok) {
    std::fprintf(stderr, "error: %s: %s\n", midi.c_str(), smf.error.c_str());
    return 1;
  }
  WavData wav;
  std::string err;
  if (!render_events(smf.events, opt, wav, err) || !write_wav(out, wav, err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }
  std::printf("rendered %zu events -> %s (%llu frames, 48000 Hz, 2 ch, float32)\n", smf.events.size(),
              out.c_str(), static_cast<unsigned long long>(wav.frames()));
  return 0;
}
