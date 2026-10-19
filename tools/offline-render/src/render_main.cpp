// velora-offline-render --midi in.mid --out out.wav [--engine test-tone|sample] [--voices N]
//                       [--tail-seconds S] [--detune-cents C]
//                       [--bundle B --instrument KEY [--seed N] [--gain-db G] [--oracle-block-frames N] [--interpolation hermite|linear]]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "parse_util.hpp"
#include "render.hpp"
#include "velora/format/bundle.hpp"
#include "velora/host/sample_bundle_loader.hpp"

using namespace velora::offline;

static int usage() {
  std::fprintf(stderr,
               "usage: velora-offline-render --midi in.mid --out out.wav [--engine test-tone|sample]\n"
               "       [--voices N] [--tail-seconds S] [--detune-cents C (test only)]\n"
               "       sample engine: --bundle B --instrument KEY [--seed N] [--gain-db G] [--oracle-block-frames N] [--interpolation hermite|linear]\n");
  return 2;
}

int main(int argc, char** argv) {
  std::string midi, out, engine = "test-tone", bundle_path, instrument_key, interp = "hermite";
  bool voices_given = false, sample_opts = false;
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
      voices_given = true;
    } else if (a == "--bundle" && val(v)) bundle_path = v;
    else if (a == "--instrument" && val(v)) instrument_key = v;
    else if (a == "--interpolation" && val(v)) { interp = v; sample_opts = true; }
    else if (a == "--oracle-block-frames" && val(v)) {
      sample_opts = true;
      std::uint64_t n = 0;
      if (!parse_uint(v, 8192, n)) { std::fprintf(stderr, "error: invalid --oracle-block-frames\n"); return 2; }
      opt.oracle_block_frames = static_cast<std::uint32_t>(n);
    } else if (a == "--gain-db" && val(v)) {
      sample_opts = true;
      double g = 0;
      if (!parse_double(v, -144.0, 48.0, g)) { std::fprintf(stderr, "error: invalid --gain-db\n"); return 2; }
      opt.gain_db = static_cast<float>(g);
    } else if (a == "--seed" && val(v)) {
      sample_opts = true;
      std::uint64_t n = 0;
      if (!parse_uint(v, UINT64_MAX, n)) { std::fprintf(stderr, "error: invalid --seed\n"); return 2; }
      opt.seed = n;
    } else if (a == "--tail-seconds" && val(v)) {
      if (!parse_double(v, 0.0, 600.0, opt.tail_seconds)) { std::fprintf(stderr, "error: invalid --tail-seconds\n"); return 2; }
    } else if (a == "--detune-cents" && val(v)) {
      double c = 0;
      if (!parse_double(v, -4800.0, 4800.0, c)) { std::fprintf(stderr, "error: invalid --detune-cents\n"); return 2; }
      opt.detune_cents = static_cast<float>(c);
    } else return usage();
  }
  if (midi.empty() || out.empty()) return usage();
  if (engine != "test-tone" && engine != "sample") {
    std::fprintf(stderr, "error: unknown engine '%s' (test-tone or sample)\n", engine.c_str());
    return 2;
  }
  if (interp != "hermite" && interp != "linear") {
    std::fprintf(stderr, "error: invalid --interpolation (hermite or linear)\n");
    return 2;
  }
  opt.interpolation = interp == "linear" ? velora::core::SampleInterpolation::kLinear
                                         : velora::core::SampleInterpolation::kHermite3;
  std::unique_ptr<velora::host::BundleInstrument> instrument;
  std::unique_ptr<velora::format::BundleReader> reader;
  if (engine == "sample") {
    if (bundle_path.empty() || instrument_key.empty()) {
      std::fprintf(stderr, "error: --engine sample needs --bundle and --instrument\n");
      return 2;
    }
    if (!voices_given) opt.voices = 64;
    auto opened = velora::format::BundleReader::open_file(bundle_path);
    if (!opened.ok()) {
      std::fprintf(stderr, "error: %s: cannot open bundle (%s)\n", bundle_path.c_str(),
                   velora::format::to_string(opened.error()));
      return 1;
    }
    reader = std::make_unique<velora::format::BundleReader>(opened.take());
    auto loaded = velora::host::load_instrument(*reader, instrument_key);
    if (!loaded.ok()) {
      std::fprintf(stderr, "error: %s: instrument '%s': %s\n", bundle_path.c_str(), instrument_key.c_str(),
                   velora::host::to_string(loaded.status));
      return 1;
    }
    instrument = std::move(loaded.instrument);
    opt.engine = EngineKind::kSample;
    opt.instrument = instrument.get();
  } else if (!bundle_path.empty() || !instrument_key.empty() || sample_opts) {
    std::fprintf(stderr, "error: --bundle, --instrument, --seed, --gain-db, --interpolation and --oracle-block-frames need --engine sample\n");
    return 2;
  }
  SmfResult smf = load_smf_file(midi);
  if (!smf.ok) {
    std::fprintf(stderr, "error: %s: %s\n", midi.c_str(), smf.error.c_str());
    return 1;
  }
  WavData wav;
  std::string err;
  SampleRenderStats stats;
  if (!render_events(smf.events, opt, wav, err, &stats)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }
  if (engine == "sample" && stats.unplayable_regions >= instrument->region_count()) {
    std::fprintf(stderr, "error: instrument '%s' has no playable region (%zu regions, all lack usable sample data or have invalid parameters); nothing written\n",
                 instrument_key.c_str(), instrument->region_count());
    return 1;
  }
  if (!write_wav(out, wav, err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }
  if (engine == "sample") {
    std::printf("sample engine: %zu samples resident (%zu bytes, %zu failed), %u unplayable regions, "
                "%llu unplayable starts skipped, %llu pool exhausted\n",
                instrument->sample_count(), instrument->resident_bytes(), instrument->failed_samples(),
                stats.unplayable_regions, static_cast<unsigned long long>(stats.skipped_unplayable_starts),
                static_cast<unsigned long long>(stats.pool_exhausted));
  }
  std::printf("rendered %zu events -> %s (%llu frames, 48000 Hz, 2 ch, float32)\n", smf.events.size(),
              out.c_str(), static_cast<unsigned long long>(wav.frames()));
  return 0;
}
