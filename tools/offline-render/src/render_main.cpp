// velora-offline-render --midi in.mid --out out.wav [--engine test-tone|sample] [--voices N]
//                       [--tail-seconds S] [--detune-cents C]
//                       [--engine organ [--drawbars 888000000] [--percussion second|third,fast|slow,soft|normal | off] [--key-click 0..1]
//                     [--vibrato off|v1|v2|v3|c1|c2|c3] [--leakage 0..1] [--rotary off|slow|fast|stop]]
//                       [--events file.contacts]   (test only; organ engine: contact_k event list, may replace or add to --midi)
//                       [--bundle B --instrument KEY [--seed N] [--gain-db G] [--oracle-block-frames N] [--interpolation hermite|linear]]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "events_file.hpp"
#include "parse_util.hpp"
#include "render.hpp"
#include "velora/format/bundle.hpp"
#include "velora/host/sample_bundle_loader.hpp"

using namespace velora::offline;

static int usage() {
  std::fprintf(stderr,
               "usage: velora-offline-render [--midi in.mid] [--events in.contacts (test only, organ engine)] --out out.wav\n"
               "       [--engine test-tone|sample|organ]   (at least one of --midi / --events)\n"
               "       [--voices N] [--tail-seconds S] [--detune-cents C (test only)]\n"
               "       organ engine: [--drawbars 888000000] (nine steps 0..8, 16' to 1')\n"
               "                     [--percussion second|third,fast|slow,soft|normal | off] [--key-click 0..1]\n"
               "                     [--vibrato off|v1|v2|v3|c1|c2|c3] [--leakage 0..1] [--rotary off|slow|fast|stop]\n"
               "       sample engine: --bundle B --instrument KEY [--seed N] [--gain-db G] [--oracle-block-frames N] [--interpolation hermite|linear]\n");
  return 2;
}

int main(int argc, char** argv) {
  std::string midi, events_path, out, engine = "test-tone", bundle_path, instrument_key, interp = "hermite";
  bool voices_given = false, sample_opts = false, drawbars_given = false;
  std::string drawbars, percussion;
  bool percussion_given = false, key_click_given = false, vibrato_given = false, leakage_given = false, rotary_given = false;
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
    else if (a == "--events" && val(v)) events_path = v;
    else if (a == "--out" && val(v)) out = v;
    else if (a == "--engine" && val(v)) engine = v;
    else if (a == "--voices" && val(v)) {
      std::uint64_t n = 0;
      if (!parse_uint(v, 4096, n) || n < 1) { std::fprintf(stderr, "error: invalid --voices\n"); return 2; }
      opt.voices = static_cast<std::uint32_t>(n);
      voices_given = true;
    } else if (a == "--drawbars" && val(v)) { drawbars = v; drawbars_given = true;
    } else if (a == "--percussion" && val(v)) { percussion = v; percussion_given = true;
    } else if (a == "--key-click" && val(v)) {
      double k = 0;
      if (!parse_double(v, 0.0, 1.0, k)) { std::fprintf(stderr, "error: invalid --key-click (0..1)\n"); return 2; }
      opt.key_click = static_cast<float>(k);
      key_click_given = true;
    } else if (a == "--vibrato" && val(v)) {
      static const char* const kNames[] = {"off", "v1", "v2", "v3", "c1", "c2", "c3"};
      int found = -1;
      for (int k = 0; k < 7; ++k)
        if (std::strcmp(v, kNames[k]) == 0) found = k;
      if (found < 0) { std::fprintf(stderr, "error: invalid --vibrato (off, v1, v2, v3, c1, c2 or c3)\n"); return 2; }
      opt.vibrato = found;
      vibrato_given = true;
    } else if (a == "--leakage" && val(v)) {
      double k = 0;
      if (!parse_double(v, 0.0, 1.0, k)) { std::fprintf(stderr, "error: invalid --leakage (0..1)\n"); return 2; }
      opt.leakage = static_cast<float>(k);
      leakage_given = true;
    } else if (a == "--rotary" && val(v)) {
      static const char* const kNames[] = {"off", "slow", "fast", "stop"};
      int found = -1;
      for (int k = 0; k < 4; ++k)
        if (std::strcmp(v, kNames[k]) == 0) found = k;
      if (found < 0) { std::fprintf(stderr, "error: invalid --rotary (off, slow, fast or stop)\n"); return 2; }
      opt.rotary = found;
      rotary_given = true;
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
  if ((midi.empty() && events_path.empty()) || out.empty()) return usage();
  if (engine != "test-tone" && engine != "sample" && engine != "organ") {
    std::fprintf(stderr, "error: unknown engine '%s' (test-tone, sample or organ)\n", engine.c_str());
    return 2;
  }
  if (!events_path.empty() && engine != "organ") {
    std::fprintf(stderr, "error: --events needs --engine organ\n");
    return 2;
  }
  if (drawbars_given && engine != "organ") {
    std::fprintf(stderr, "error: --drawbars needs --engine organ\n");
    return 2;
  }
  if ((percussion_given || key_click_given || vibrato_given || leakage_given || rotary_given) && engine != "organ") {
    std::fprintf(stderr, "error: --percussion, --key-click, --vibrato, --leakage and --rotary need --engine organ\n");
    return 2;
  }
  if (engine == "organ") {
    if (percussion_given && percussion != "off") {
      // Comma separated words, each of harmonic (second|third), decay (fast|slow) and volume (soft|normal) at most once;
      // what is not named keeps the registry default (third, fast, soft).
      opt.percussion_on = true;  // (the list is parsed once, from the last --percussion given; defaults first)
      opt.percussion_harmonic = 1; opt.percussion_decay = 0; opt.percussion_volume = 0;
      bool seen[3] = {false, false, false}, good = !percussion.empty();
      std::size_t pos = 0;
      while (good && pos <= percussion.size()) {
        std::size_t comma = percussion.find(',', pos);
        if (comma == std::string::npos) comma = percussion.size();
        const std::string w = percussion.substr(pos, comma - pos);
        int cat = -1, val = 0;
        if (w == "second") { cat = 0; val = 0; } else if (w == "third") { cat = 0; val = 1; }
        else if (w == "fast") { cat = 1; val = 0; } else if (w == "slow") { cat = 1; val = 1; }
        else if (w == "soft") { cat = 2; val = 0; } else if (w == "normal") { cat = 2; val = 1; }
        if (cat < 0 || seen[cat]) { good = false; break; }
        seen[cat] = true;
        (cat == 0 ? opt.percussion_harmonic : cat == 1 ? opt.percussion_decay : opt.percussion_volume) = val;
        pos = comma + 1;
      }
      if (!good) {
        std::fprintf(stderr, "error: invalid --percussion (off, or a comma list of second|third, fast|slow, soft|normal)\n");
        return 2;
      }
    }
    if (drawbars_given) {
      bool good = drawbars.size() == 9;
      for (char c : drawbars) good = good && c >= '0' && c <= '8';
      if (!good) { std::fprintf(stderr, "error: invalid --drawbars (nine digits 0..8, e.g. 888000000)\n"); return 2; }
      opt.drawbars = drawbars;
    }
    opt.engine = EngineKind::kOrgan;
    if (sample_opts || !bundle_path.empty() || !instrument_key.empty()) {
      std::fprintf(stderr, "error: --bundle, --instrument, --seed, --gain-db, --interpolation and --oracle-block-frames need --engine sample\n");
      return 2;
    }
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
  } else if (engine == "organ") {
    // handled above
  } else if (!bundle_path.empty() || !instrument_key.empty() || sample_opts) {
    std::fprintf(stderr, "error: --bundle, --instrument, --seed, --gain-db, --interpolation and --oracle-block-frames need --engine sample\n");
    return 2;
  }
  SmfResult smf;
  if (!midi.empty()) {
    smf = load_smf_file(midi);
    if (!smf.ok) {
      std::fprintf(stderr, "error: %s: %s\n", midi.c_str(), smf.error.c_str());
      return 1;
    }
  } else {
    smf.ok = true;
  }
  if (!events_path.empty()) {
    EventsResult ev = load_events_file(events_path);
    if (!ev.ok) {
      std::fprintf(stderr, "error: %s: %s\n", events_path.c_str(), ev.error.c_str());
      return 1;
    }
    if (ev.contact_count != 0) opt.contact_count = ev.contact_count;
    smf.events = merge_events(smf.events, ev.events);
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
