// velora-offline-render --midi in.mid --out out.wav [--engine test-tone|sample] [--voices N]
//                       [--tail-seconds S] [--detune-cents C]
//                       [--engine organ [--drawbars 888000000] [--percussion second|third,fast|slow,soft|normal | off] [--key-click 0..1]
//                     [--vibrato off|v1|v2|v3|c1|c2|c3] [--leakage 0..1] [--rotary off|slow|fast|stop]]
//                       [--part-output P:main|aux] [--send P:REVERB[:DELAY]] [--inserts P:type[@p1/p2/..][!][,...]] [--master-level DB]
//                       [--master-eq LOW:MID:MIDHZ:HIGH[!]] [--master-comp THRESH:RATIO:ATTACK:RELEASE:MAKEUP[!]]   (Story 2.8)
//                       [--reverb room|hall|plate] [--reverb-time S] [--reverb-level 0..1] [--rev-master 0..1]   (reverb, Story 2.10)
//                       [--delay-time MS] [--delay-sync on|off] [--delay-note 1/4|1/8|1/16|1/4d|1/8d|1/16d|1/4t|1/8t|1/16t] [--delay-feedback 0..0.95]
//                       [--delay-level 0..1] [--delay-damping 0..1] [--delay-pingpong 0..1] [--tempo BPM] [--tempo-change SECONDS:BPM]   (delay, Story 2.11)
//                       [--line-in in.wav [--line-in-level 0..1]] [--aux-out aux.wav]   (routing, Story 2.7; parts are MIDI channels 0..7)
//                       [--events file.contacts]   (test only; organ engine: contact_k event list, may replace or add to --midi)
//                       [--bundle B --instrument KEY [--seed N] [--gain-db G] [--oracle-block-frames N] [--interpolation hermite|linear]]
//                       [--live-set FILE --library-root DIR [--seed N] [--tail-seconds S]]   (Live Set mode, Story 4.3: --midi, --out; no other engine option)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "events_file.hpp"
#include "parse_util.hpp"
#include "render.hpp"
#include "render_live_set.hpp"
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
               "       routing (parts = channels 0..7): [--part-output P:main|aux] [--send P:REVERB[:DELAY]] [--inserts P:type[!],...]\n"
               "                     (type: rotary chorus_phaser ep_amp eq compressor, '@p1/p2/..' = normalised params, '!' = bypassed)\n"
               "                     [--master-level DB] [--master-eq LOW_DB:MID_DB:MID_HZ:HIGH_DB[!]]\n"
               "                     [--master-comp THRESHOLD_DB:RATIO:ATTACK_MS:RELEASE_MS:MAKEUP_DB[!]]\n"
               "                     [--reverb room|hall|plate] [--reverb-time 0.2..10] [--reverb-level 0..1] [--rev-master 0..1]\n"
               "                     [--delay-time 1..2000] [--delay-sync on|off] [--delay-note 1/4|1/8|1/16|1/4d|1/8d|1/16d|1/4t|1/8t|1/16t]\n"
               "                     [--delay-feedback 0..0.95] [--delay-level 0..1] [--delay-damping 0..1] [--delay-pingpong 0..1]\n"
               "                     [--tempo 20..300] [--tempo-change SECONDS:BPM] (repeatable)\n"
               "                     [--line-in in.wav] [--line-in-level 0..1] [--aux-out aux.wav]\n"
               "       sample engine: --bundle B --instrument KEY [--seed N] [--gain-db G] [--oracle-block-frames N] [--interpolation hermite|linear]\n"
               "       Live Set: --live-set FILE --library-root DIR --midi in.mid --out out.wav [--seed N] [--tail-seconds S]\n"
               "                 (every note on any channel is fanned out to the Live Set's parts by zone; no other engine option)\n");
  return 2;
}

int main(int argc, char** argv) {
  std::string midi, events_path, out, engine = "test-tone", bundle_path, instrument_key, interp = "hermite";
  bool voices_given = false, sample_opts = false, drawbars_given = false;
  std::string drawbars, percussion;
  bool percussion_given = false, key_click_given = false, vibrato_given = false, leakage_given = false, rotary_given = false;
  RenderOptions opt;
  bool master_eq_seen = false, master_comp_seen = false, reverb_seen = false, reverb_time_seen = false, reverb_level_seen = false, rev_master_seen = false;
  bool delay_seen[7] = {false, false, false, false, false, false, false}, tempo_seen = false;  // each master option may be given once (parse_double also refuses NaN / inf)
  std::string line_in_path, aux_out_path, live_set_path, library_root;
  bool seed_given = false;
  WavData line_in_wav, aux_wav;
  // "P:rest" -> part index 0..7 and the text after the colon.
  auto split_part = [](const std::string& arg, std::uint32_t& part, std::string& rest) {
    const std::size_t colon = arg.find(':');
    std::uint64_t p = 0;
    if (colon == std::string::npos || !parse_uint(arg.substr(0, colon), 7, p)) return false;
    part = static_cast<std::uint32_t>(p);
    rest = arg.substr(colon + 1);
    return true;
  };
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
    } else if (a == "--part-output" && val(v)) {
      std::uint32_t part = 0;
      std::string rest;
      if (!split_part(v, part, rest) || (rest != "main" && rest != "aux")) {
        std::fprintf(stderr, "error: invalid --part-output (P:main or P:aux, P = 0..7)\n");
        return 2;
      }
      opt.routing.output[part] = rest == "aux" ? 1 : 0;
      opt.routing.enabled = true;
    } else if (a == "--send" && val(v)) {
      std::uint32_t part = 0;
      std::string rest;
      double rev = 0, dly = 0;
      bool good = split_part(v, part, rest);
      if (good) {
        const std::size_t colon = rest.find(':');
        good = parse_double(rest.substr(0, colon), 0.0, 1.0, rev) &&
               (colon == std::string::npos || parse_double(rest.substr(colon + 1), 0.0, 1.0, dly));
      }
      if (!good) { std::fprintf(stderr, "error: invalid --send (P:REVERB or P:REVERB:DELAY, levels 0..1)\n"); return 2; }
      opt.routing.send_reverb[part] = static_cast<float>(rev);
      opt.routing.send_delay[part] = static_cast<float>(dly);
      opt.routing.enabled = true;
    } else if (a == "--inserts" && val(v)) {
      std::uint32_t part = 0;
      std::string rest;
      bool good = split_part(v, part, rest);
      std::vector<RoutingOptions::Insert> list;
      std::size_t pos = 0;
      while (good && !rest.empty() && pos <= rest.size()) {
        std::size_t comma = rest.find(',', pos);
        if (comma == std::string::npos) comma = rest.size();
        std::string w = rest.substr(pos, comma - pos);
        RoutingOptions::Insert in;
        if (!w.empty() && w.back() == '!') { in.bypass = true; w.pop_back(); }
        static const char* const kTypes[] = {"none", "rotary", "chorus_phaser", "ep_amp", "eq", "compressor"};
        const std::size_t at = w.find('@');  // type@p1/p2/..: normalised insert params 1..5 (0..1), missing ones keep the defaults
        if (at != std::string::npos) {
          std::string pl = w.substr(at + 1);
          w.resize(at);
          std::size_t q = 0;
          std::size_t n = 0;
          while (good && q <= pl.size()) {
            std::size_t slash = pl.find('/', q);
            if (slash == std::string::npos) slash = pl.size();
            double pv = 0;
            if (n >= in.params.size() || !parse_double(pl.substr(q, slash - q), 0.0, 1.0, pv)) { good = false; break; }
            in.params[n++] = static_cast<float>(pv);
            q = slash + 1;
          }
          in.has_params = true;
          if (!good) break;
        }
        int t = -1;
        for (int k = 0; k < 6; ++k)
          if (w == kTypes[k]) t = k;
        if (t < 0 || list.size() >= 5) { good = false; break; }
        in.type = static_cast<std::uint8_t>(t);
        list.push_back(in);
        pos = comma + 1;
      }
      if (!good) {
        std::fprintf(stderr, "error: invalid --inserts (P:type[@p1/p2/..][!],... with at most 5 of none rotary chorus_phaser ep_amp eq compressor, params 0..1)\n");
        return 2;
      }
      opt.routing.inserts[part] = list;
      opt.routing.enabled = true;
    } else if (a == "--master-level" && val(v)) {
      double db = 0;
      if (!parse_double(v, -60.0, 12.0, db)) { std::fprintf(stderr, "error: invalid --master-level (-60..12 dB)\n"); return 2; }
      opt.routing.master_level_db = static_cast<float>(db);
      opt.routing.enabled = true;
    } else if (a == "--reverb" && val(v)) {
      if (reverb_seen) { std::fprintf(stderr, "error: duplicate --reverb\n"); return 2; }
      reverb_seen = true;  // Story 2.10: reverb type room|hall|plate
      static const char* const kTypes[3] = {"room", "hall", "plate"};
      int found = -1;
      for (int k = 0; k < 3; ++k)
        if (std::strcmp(v, kTypes[k]) == 0) found = k;
      if (found < 0) { std::fprintf(stderr, "error: invalid --reverb (room, hall or plate)\n"); return 2; }
      opt.routing.reverb_type = static_cast<std::uint8_t>(found);
      opt.routing.enabled = true;
    } else if (a == "--reverb-time" && val(v)) {
      if (reverb_time_seen) { std::fprintf(stderr, "error: duplicate --reverb-time\n"); return 2; }
      reverb_time_seen = true;
      double t = 0;
      if (!parse_double(v, 0.2, 10.0, t)) { std::fprintf(stderr, "error: invalid --reverb-time (0.2..10 seconds)\n"); return 2; }
      opt.routing.reverb_time_s = static_cast<float>(t);
      opt.routing.enabled = true;
    } else if (a == "--reverb-level" && val(v)) {
      if (reverb_level_seen) { std::fprintf(stderr, "error: duplicate --reverb-level\n"); return 2; }
      reverb_level_seen = true;
      double lv = 0;
      if (!parse_double(v, 0.0, 1.0, lv)) { std::fprintf(stderr, "error: invalid --reverb-level (0..1)\n"); return 2; }
      opt.routing.reverb_level = static_cast<float>(lv);
      opt.routing.enabled = true;
    } else if (a == "--rev-master" && val(v)) {
      if (rev_master_seen) { std::fprintf(stderr, "error: duplicate --rev-master\n"); return 2; }
      rev_master_seen = true;
      double lv = 0;
      if (!parse_double(v, 0.0, 1.0, lv)) { std::fprintf(stderr, "error: invalid --rev-master (0..1)\n"); return 2; }
      opt.routing.reverb_master = static_cast<float>(lv);
      opt.routing.enabled = true;
    } else if ((a == "--delay-time" || a == "--delay-feedback" || a == "--delay-level" || a == "--delay-damping" || a == "--delay-pingpong") && val(v)) {
      // Story 2.11: the delay settings of the snapshot the routing options publish (each given once; the ranges are the registry's)
      struct Opt { const char* name; double lo, hi; float RoutingOptions::*field; int idx; };
      static const Opt kOpts[] = {{"--delay-time", 1.0, 2000.0, &RoutingOptions::delay_time_ms, 0}, {"--delay-feedback", 0.0, 0.95, &RoutingOptions::delay_feedback, 1},
                                  {"--delay-level", 0.0, 1.0, &RoutingOptions::delay_level, 2}, {"--delay-damping", 0.0, 1.0, &RoutingOptions::delay_damping, 3},
                                  {"--delay-pingpong", 0.0, 1.0, &RoutingOptions::delay_pingpong, 4}};
      for (const Opt& o : kOpts) {
        if (a != o.name) continue;
        if (delay_seen[o.idx]) { std::fprintf(stderr, "error: duplicate %s\n", o.name); return 2; }
        delay_seen[o.idx] = true;
        double x = 0;
        if (!parse_double(v, o.lo, o.hi, x)) { std::fprintf(stderr, "error: invalid %s (%g..%g)\n", o.name, o.lo, o.hi); return 2; }
        opt.routing.*(o.field) = static_cast<float>(x);
      }
      opt.routing.enabled = true;
    } else if (a == "--delay-sync" && val(v)) {
      if (delay_seen[5]) { std::fprintf(stderr, "error: duplicate --delay-sync\n"); return 2; }
      delay_seen[5] = true;
      if (std::strcmp(v, "on") != 0 && std::strcmp(v, "off") != 0) { std::fprintf(stderr, "error: invalid --delay-sync (on or off)\n"); return 2; }
      opt.routing.delay_sync = std::strcmp(v, "on") == 0;
      opt.routing.enabled = true;
    } else if (a == "--delay-note" && val(v)) {
      if (delay_seen[6]) { std::fprintf(stderr, "error: duplicate --delay-note\n"); return 2; }
      delay_seen[6] = true;
      static const char* const kNotes[9] = {"1/4", "1/8", "1/16", "1/4d", "1/8d", "1/16d", "1/4t", "1/8t", "1/16t"};  // the registry enum delay_note, in order
      int found = -1;
      for (int k = 0; k < 9; ++k)
        if (std::strcmp(v, kNotes[k]) == 0) found = k;
      if (found < 0) { std::fprintf(stderr, "error: invalid --delay-note (1/4 1/8 1/16, d = dotted, t = triplet)\n"); return 2; }
      opt.routing.delay_note = static_cast<std::uint8_t>(found);
      opt.routing.enabled = true;
    } else if (a == "--tempo" && val(v)) {
      if (tempo_seen) { std::fprintf(stderr, "error: duplicate --tempo\n"); return 2; }
      tempo_seen = true;
      double bpm = 0;
      if (!parse_double(v, 20.0, 300.0, bpm)) { std::fprintf(stderr, "error: invalid --tempo (20..300 bpm)\n"); return 2; }
      opt.routing.tempo_bpm = static_cast<float>(bpm);
      opt.routing.enabled = true;
    } else if (a == "--tempo-change" && val(v)) {  // SECONDS:BPM, repeatable
      const char* colon = std::strchr(v, ':');
      double sec = 0, bpm = 0;
      if (!colon || colon == v) { std::fprintf(stderr, "error: invalid --tempo-change (SECONDS:BPM)\n"); return 2; }
      const std::string left(v, static_cast<std::size_t>(colon - v));
      if (!parse_double(left.c_str(), 0.0, 3600.0, sec) || !parse_double(colon + 1, 20.0, 300.0, bpm)) {
        std::fprintf(stderr, "error: invalid --tempo-change (SECONDS 0..3600 : BPM 20..300)\n");
        return 2;
      }
      opt.routing.tempo_changes.emplace_back(sec, static_cast<float>(bpm));
      opt.routing.enabled = true;
    } else if (a == "--master-eq" && val(v)) {  // LOW_DB:MID_DB:MID_HZ:HIGH_DB[!]   (! = bypassed)
      if (master_eq_seen) { std::fprintf(stderr, "error: duplicate --master-eq\n"); return 2; }
      master_eq_seen = true;
      std::string w = v;
      RoutingOptions::MasterEq me;
      if (!w.empty() && w.back() == '!') { me.bypass = true; w.pop_back(); }
      double x[4] = {0, 0, 0, 0};
      std::size_t q = 0;
      bool good = true;
      for (int k = 0; k < 4 && good; ++k) {
        std::size_t colon = w.find(':', q);
        if (colon == std::string::npos) colon = w.size();
        if ((k < 3 && colon == w.size()) || (k == 3 && colon != w.size())) { good = false; break; }
        static const double lo[4] = {-15, -15, 200, -15}, hi[4] = {15, 15, 8000, 15};
        good = parse_double(w.substr(q, colon - q), lo[k], hi[k], x[k]);
        q = colon + 1;
      }
      if (!good) {
        std::fprintf(stderr, "error: invalid --master-eq (LOW_DB:MID_DB:MID_HZ:HIGH_DB[!], gains -15..15, mid 200..8000 Hz)\n");
        return 2;
      }
      me.low_db = static_cast<float>(x[0]);
      me.mid_db = static_cast<float>(x[1]);
      me.mid_hz = static_cast<float>(x[2]);
      me.high_db = static_cast<float>(x[3]);
      opt.routing.master_eq = me;
      opt.routing.enabled = true;
    } else if (a == "--master-comp" && val(v)) {  // THRESHOLD_DB:RATIO:ATTACK_MS:RELEASE_MS:MAKEUP_DB[!]
      if (master_comp_seen) { std::fprintf(stderr, "error: duplicate --master-comp\n"); return 2; }
      master_comp_seen = true;
      std::string w = v;
      RoutingOptions::MasterComp mc;
      if (!w.empty() && w.back() == '!') { mc.bypass = true; w.pop_back(); }
      double x[5] = {0, 0, 0, 0, 0};
      std::size_t q = 0;
      bool good = true;
      static const double lo[5] = {-60, 1, 0.1, 10, 0}, hi[5] = {0, 20, 100, 1000, 24};
      for (int k = 0; k < 5 && good; ++k) {
        std::size_t colon = w.find(':', q);
        if (colon == std::string::npos) colon = w.size();
        if ((k < 4 && colon == w.size()) || (k == 4 && colon != w.size())) { good = false; break; }
        good = parse_double(w.substr(q, colon - q), lo[k], hi[k], x[k]);
        q = colon + 1;
      }
      if (!good) {
        std::fprintf(stderr, "error: invalid --master-comp (THRESHOLD_DB:RATIO:ATTACK_MS:RELEASE_MS:MAKEUP_DB[!], -60..0, 1..20, 0.1..100, 10..1000, 0..24)\n");
        return 2;
      }
      mc.threshold_db = static_cast<float>(x[0]);
      mc.ratio = static_cast<float>(x[1]);
      mc.attack_ms = static_cast<float>(x[2]);
      mc.release_ms = static_cast<float>(x[3]);
      mc.makeup_db = static_cast<float>(x[4]);
      opt.routing.master_comp = mc;
      opt.routing.enabled = true;
    } else if (a == "--line-in" && val(v)) {
      line_in_path = v;
      opt.routing.enabled = true;
    } else if (a == "--line-in-level" && val(v)) {
      double lv = 0;
      if (!parse_double(v, 0.0, 1.0, lv)) { std::fprintf(stderr, "error: invalid --line-in-level (0..1)\n"); return 2; }
      opt.routing.line_in_level = static_cast<float>(lv);
      opt.routing.enabled = true;
    } else if (a == "--aux-out" && val(v)) {
      aux_out_path = v;
      opt.routing.enabled = true;
    } else if (a == "--live-set" && val(v)) live_set_path = v;
    else if (a == "--library-root" && val(v)) library_root = v;
    else if (a == "--bundle" && val(v)) bundle_path = v;
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
      seed_given = true;
    } else if (a == "--tail-seconds" && val(v)) {
      if (!parse_double(v, 0.0, 600.0, opt.tail_seconds)) { std::fprintf(stderr, "error: invalid --tail-seconds\n"); return 2; }
    } else if (a == "--detune-cents" && val(v)) {
      double c = 0;
      if (!parse_double(v, -4800.0, 4800.0, c)) { std::fprintf(stderr, "error: invalid --detune-cents\n"); return 2; }
      opt.detune_cents = static_cast<float>(c);
    } else return usage();
  }
  if (!live_set_path.empty() || !library_root.empty()) {
    // Live Set mode (Story 4.3): the Live Set decides the engines, the routing and the instruments.
    if (live_set_path.empty() || library_root.empty() || midi.empty() || out.empty()) return usage();
    if (engine != "test-tone" || !events_path.empty() || !bundle_path.empty() || !instrument_key.empty() || drawbars_given || percussion_given ||
        key_click_given || vibrato_given || leakage_given || rotary_given || voices_given || opt.routing.enabled ||
        interp != "hermite" || opt.gain_db != 0.0f || opt.oracle_block_frames != 0 ||
        opt.detune_cents != 0.0f) {
      std::fprintf(stderr, "error: --live-set cannot be combined with engine, instrument, routing or organ options (only --seed and --tail-seconds)\n");
      return 2;
    }
    SmfResult lsmf = load_smf_file(midi);
    if (!lsmf.ok) {
      std::fprintf(stderr, "error: %s: %s\n", midi.c_str(), lsmf.error.c_str());
      return 1;
    }
    LiveSetRenderOptions lo;
    lo.live_set_path = live_set_path;
    lo.library_root = library_root;
    lo.tail_seconds = opt.tail_seconds;
    if (seed_given) lo.seed = opt.seed;
    WavData lwav;
    LiveSetRenderReport lrep;
    std::string lerr;
    if (!render_live_set(lsmf.events, lo, lwav, lerr, &lrep)) {
      std::fprintf(stderr, "error: %s\n", lerr.c_str());
      return 1;
    }
    if (!write_wav(out, lwav, lerr)) {
      std::fprintf(stderr, "error: %s\n", lerr.c_str());
      return 1;
    }
    static const char* const kStates[] = {"ready", "empty timbre", "missing library", "engine not available"};
    for (std::size_t i = 0; i < lrep.part_states.size(); ++i)
      std::printf("part %zu: %s\n", i, kStates[static_cast<std::size_t>(lrep.part_states[i])]);
    std::printf("live set: %llu pool exhausted, %llu notes dropped out of range, %llu keys no part took\n",
                static_cast<unsigned long long>(lrep.pool_exhausted), static_cast<unsigned long long>(lrep.dropped_out_of_range),
                static_cast<unsigned long long>(lrep.unzoned_keys));
    std::printf("rendered %zu events -> %s (%llu frames, 48000 Hz, 2 ch, float32)\n", lsmf.events.size(), out.c_str(),
                static_cast<unsigned long long>(lwav.frames()));
    return 0;
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
  if (!line_in_path.empty()) {
    if (!read_wav(line_in_path, line_in_wav, err)) {
      std::fprintf(stderr, "error: %s: %s\n", line_in_path.c_str(), err.c_str());
      return 1;
    }
    opt.routing.line_in = &line_in_wav;
  }
  if (!aux_out_path.empty()) opt.aux_out = &aux_wav;
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
  if (!aux_out_path.empty() && !write_wav(aux_out_path, aux_wav, err)) {
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
