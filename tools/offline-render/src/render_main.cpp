// velora-offline-render --midi in.mid --out out.wav [--engine test-tone|sample] [--voices N]
//                       [--tail-seconds S] [--detune-cents C]
//                       [--engine organ [--drawbars 888000000] [--percussion second|third,fast|slow,soft|normal | off] [--key-click 0..1]
//                     [--vibrato off|v1|v2|v3|c1|c2|c3] [--leakage 0..1] [--rotary off|slow|fast|stop]]
//                       [--part-output P:main|aux] [--send P:REVERB[:DELAY]] [--inserts P:type[@p1/p2/..][!][,...]] [--master-level DB]
//                       [--master-eq LOW:MID:MIDHZ:HIGH[!]] [--master-comp THRESH:RATIO:ATTACK:RELEASE:MAKEUP[!]]   (Story 2.8)
//                       [--reverb room|hall|plate] [--reverb-time S] [--reverb-level 0..1] [--rev-master 0..1]   (reverb, Story 2.10)
//                       [--delay-time MS] [--delay-sync on|off] [--delay-note 1/4|1/8|1/16|1/4d|1/8d|1/16d|1/4t|1/8t|1/16t] [--delay-feedback 0..0.95]
//                       [--delay-level 0..1] [--delay-damping 0..1] [--delay-pingpong 0..1] [--tempo BPM] [--tempo-change SECONDS:BPM]   (delay, Story 2.11)
//                       [--arp P:MODE:RATE:OCTAVES:GATE[:SWING[:LATCH]]]   (arpeggiator, Stories 7.2 / 7.3)
//                       [--line-in in.wav [--line-in-level 0..1]] [--aux-out aux.wav]   (routing, Story 2.7; parts are MIDI channels 0..7)
//                       [--events file.contacts]   (test only; organ engine: contact_k event list, may replace or add to --midi)
//                       [--bundle B --instrument KEY [--seed N] [--gain-db G] [--oracle-block-frames N] [--interpolation hermite|linear]]
//                         [--dev-device-key KEYFILE --wrapped-key BLOB --trust-list FILE]   (Story 6.5, dev builds only: B is a PROTECTED bundle (bundle.vlrp); the content key
//                                                                                              is unwrapped with the dev device key, decrypted on this thread, nothing is written to disk)
//                       [--live-set FILE --library-root DIR [--scene-at SECONDS:INDEX]... [--switch-at SECONDS:LIVESET_FILE]... [--switch-load-blocks N]
//                         [--assign-mode] [--encoder-at SECONDS:ENC:DELTA]... [--seed N] [--tail-seconds S]]
//                         (Live Set mode, Story 4.3: --midi, --out; no other engine option; --switch-at is Story 4.5; --assign-mode / --encoder-at Story 4.10)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "events_file.hpp"
#ifdef VELORA_OFFLINE_DEV_KEYS
#include <sys/resource.h>

#include "velora/format/protected_verify.hpp"
#include "velora/host/dev_key_provider.hpp"
#include "velora/host/protected_head_provider.hpp"
#include "velora/host/trust_list.hpp"
#endif
#include "parse_util.hpp"
#include "render.hpp"
#include "render_live_set.hpp"
#include "velora/core/fm_parameters.hpp"
#include "velora/format/bundle.hpp"
#include "velora/format/sysex_6op.hpp"
#include "velora/host/sample_bundle_loader.hpp"

using namespace velora::offline;

static velora::core::FmVoiceParameters to_core(const velora::format::FmVoiceParameters& f) {
  velora::core::FmVoiceParameters c;
  for (std::size_t i = 0; i < 6; ++i) {
    c.operators[i].rates = f.operators[i].rates;
    c.operators[i].levels = f.operators[i].levels;
    c.operators[i].kbd_breakpoint = f.operators[i].kbd_breakpoint;
    c.operators[i].left_depth = f.operators[i].left_depth;
    c.operators[i].right_depth = f.operators[i].right_depth;
    c.operators[i].left_curve = f.operators[i].left_curve;
    c.operators[i].right_curve = f.operators[i].right_curve;
    c.operators[i].kbd_rate_scaling = f.operators[i].kbd_rate_scaling;
    c.operators[i].amp_mod_sens = f.operators[i].amp_mod_sens;
    c.operators[i].velocity_sens = f.operators[i].velocity_sens;
    c.operators[i].total_level = f.operators[i].total_level;
    c.operators[i].fixed_freq_mode = f.operators[i].fixed_freq_mode;
    c.operators[i].freq_coarse = f.operators[i].freq_coarse;
    c.operators[i].freq_fine = f.operators[i].freq_fine;
    c.operators[i].detune = f.operators[i].detune;
  }
  c.pitch_env_rates = f.pitch_env_rates;
  c.pitch_env_levels = f.pitch_env_levels;
  c.algorithm = f.algorithm;
  c.feedback = f.feedback;
  c.osc_key_sync = f.osc_key_sync;
  c.lfo_speed = f.lfo_speed;
  c.lfo_delay = f.lfo_delay;
  c.lfo_pitch_mod_depth = f.lfo_pitch_mod_depth;
  c.lfo_amp_mod_depth = f.lfo_amp_mod_depth;
  c.lfo_key_sync = f.lfo_key_sync;
  c.lfo_wave = f.lfo_wave;
  c.lfo_pitch_mod_sens = f.lfo_pitch_mod_sens;
  c.transpose = f.transpose;
  std::memcpy(c.name, f.name, sizeof(c.name));
  c.brightness_offset = f.brightness_offset;
  return c;
}

static int usage() {
  std::fprintf(stderr,
               "usage: velora-offline-render [--midi in.mid] [--events in.contacts (test only, organ engine)] --out out.wav\n"
               "       [--engine test-tone|sample|organ|fm]   (at least one of --midi / --events)\n"
               "       [--voices N] [--tail-seconds S] [--detune-cents C (test only)]\n"
               "       organ engine: [--drawbars 888000000] (nine steps 0..8, 16' to 1')\n"
               "                     [--percussion second|third,fast|slow,soft|normal | off] [--key-click 0..1]\n"
               "                     [--vibrato off|v1|v2|v3|c1|c2|c3] [--leakage 0..1] [--rotary off|slow|fast|stop]\n"
               "       FM engine:    --syx <file.syx> [--voice-slot 1..32] [--negative feedback|algo|gain]\n"
               "       routing (parts = channels 0..7): [--part-output P:main|aux] [--send P:REVERB[:DELAY]] [--inserts P:type[!],...]\n"
               "                     (type: rotary chorus_phaser ep_amp eq compressor, '@p1/p2/..' = normalised params, '!' = bypassed)\n"
               "                     [--master-level DB] [--master-eq LOW_DB:MID_DB:MID_HZ:HIGH_DB[!]]\n"
               "                     [--master-comp THRESHOLD_DB:RATIO:ATTACK_MS:RELEASE_MS:MAKEUP_DB[!]]\n"
               "                     [--reverb room|hall|plate] [--reverb-time 0.2..10] [--reverb-level 0..1] [--rev-master 0..1]\n"
               "                     [--delay-time 1..2000] [--delay-sync on|off] [--delay-note 1/4|1/8|1/16|1/4d|1/8d|1/16d|1/4t|1/8t|1/16t]\n"
               "                     [--delay-feedback 0..0.95] [--delay-level 0..1] [--delay-damping 0..1] [--delay-pingpong 0..1]\n"
               "                     [--tempo 20..300] [--tempo-change SECONDS:BPM] (repeatable)\n"
               "                     [--arp P:MODE:RATE:OCTAVES:GATE] (MODE = up|down|updown|random|as-played, RATE = 1/4|1/8|1/16|1/32|1/4t|1/8t|1/16t|1/32t, OCTAVES = 1..4, GATE = 1..200)\n"
               "                     [--line-in in.wav] [--line-in-level 0..1] [--aux-out aux.wav]\n"
               "       sample engine: --bundle B --instrument KEY [--seed N] [--gain-db G] [--oracle-block-frames N] [--interpolation hermite|linear]\n"
               "       Live Set: --live-set FILE --library-root DIR --midi in.mid --out out.wav [--scene-at SECONDS:INDEX]... [--seed N] [--tail-seconds S]\n"
               "                 [--switch-at SECONDS:LIVESET_FILE]... [--switch-load-blocks N]   (seamless switch to another Live Set; the background load is simulated as N blocks)\n"
               "                 [--assign-mode] [--encoder-at SECONDS:ENC:DELTA]...   (ASSIGN encoder mode from the start; encoder ENC 1..8 = K1..K8 turned by DELTA relative steps, -32768..32767)\n"
               "                 (every note on any channel is fanned out to the Live Set's parts by zone; no other engine option)\n");
  return 2;
}

int main(int argc, char** argv) {
  std::string midi, events_path, out, engine = "test-tone", bundle_path, instrument_key, interp = "hermite";
  std::string syx_path, negative_opt;
  std::uint64_t voice_slot = 1;
  bool voice_slot_given = false;
  bool voices_given = false, sample_opts = false, drawbars_given = false;
  std::string drawbars, percussion;
  bool percussion_given = false, key_click_given = false, vibrato_given = false, leakage_given = false, rotary_given = false;
  RenderOptions opt;
  bool master_eq_seen = false, master_comp_seen = false, reverb_seen = false, reverb_time_seen = false, reverb_level_seen = false, rev_master_seen = false;
  bool delay_seen[7] = {false, false, false, false, false, false, false}, tempo_seen = false;  // each master option may be given once (parse_double also refuses NaN / inf)
  bool arp_seen[8] = {false, false, false, false, false, false, false, false};
  std::string line_in_path, aux_out_path, live_set_path, library_root;
  std::string dev_device_key, wrapped_key_path, trust_list_path;  // Story 6.5 (dev builds)
  bool seed_given = false;
  std::vector<SceneRecallAt> scene_recalls;
  std::vector<SwitchAt> switches;
  std::vector<EncoderAt> encoder_turns;
  bool assign_mode = false;
  std::uint64_t switch_load_blocks = 0;
  bool switch_load_blocks_given = false;
  std::vector<SetlistLoadAt> setlist_loads;
  std::vector<SetlistStepAt> setlist_steps;
  std::string setlist_dir;
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
    } else if (a == "--arp" && val(v)) {  // Story 7.2 / 7.3: P:MODE:RATE:OCTAVES:GATE[:SWING[:LATCH]]
      std::uint32_t part = 0;
      std::string rest;
      bool good = split_part(v, part, rest);
      std::vector<std::string> fields;
      if (good) {
        std::size_t q = 0;
        while (q <= rest.size()) {
          std::size_t colon = rest.find(':', q);
          if (colon == std::string::npos) {
            fields.push_back(rest.substr(q));
            break;
          }
          fields.push_back(rest.substr(q, colon - q));
          q = colon + 1;
        }
        if (fields.size() < 4 || fields.size() > 6) good = false;
      }
      static const char* const kModes[5] = {"up", "down", "updown", "random", "as-played"};
      static const char* const kRates[8] = {"1/4", "1/8", "1/16", "1/32", "1/4t", "1/8t", "1/16t", "1/32t"};
      int mode_idx = -1, rate_idx = -1;
      std::uint64_t octaves = 0;
      double gate = 0.0;
      double swing = 50.0;
      bool latch = false;
      if (good) {
        for (int k = 0; k < 5; ++k)
          if (fields[0] == kModes[k]) mode_idx = k;
        for (int k = 0; k < 8; ++k)
          if (fields[1] == kRates[k]) rate_idx = k;
        good = mode_idx >= 0 && rate_idx >= 0 && parse_uint(fields[2], 4, octaves) && octaves >= 1 &&
               parse_double(fields[3], 1.0, 200.0, gate);
        if (good && fields.size() >= 5) {
          good = parse_double(fields[4], 50.0, 75.0, swing);
        }
        if (good && fields.size() == 6) {
          if (fields[5] == "on") latch = true;
          else if (fields[5] == "off") latch = false;
          else good = false;
        }
      }
      if (!good) {
        std::fprintf(stderr,
                     "error: invalid --arp (P:MODE:RATE:OCTAVES:GATE[:SWING[:LATCH]], P = 0..7, MODE = up|down|updown|random|as-played, "
                     "RATE = 1/4|1/8|1/16|1/32|1/4t|1/8t|1/16t|1/32t, OCTAVES = 1..4, GATE = 1..200, SWING = 50..75, LATCH = on|off)\n");
        return 2;
      }
      if (arp_seen[part]) {
        std::fprintf(stderr, "error: duplicate --arp on part %u\n", part);
        return 2;
      }
      arp_seen[part] = true;
      velora::core::ArpConfig& ac = opt.routing.arp[part];
      ac.enabled = true;
      ac.mode = static_cast<velora::core::ArpMode>(mode_idx);
      ac.rate = static_cast<velora::core::ArpRate>(rate_idx);
      ac.octaves = static_cast<std::uint8_t>(octaves);
      ac.gate_percent = static_cast<float>(gate);
      ac.swing_percent = static_cast<float>(swing);
      ac.latch = latch;
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
    } else if (a == "--scene-at" && val(v)) {
      // SECONDS:INDEX -- Live Set mode: recall scene INDEX at that time (repeatable).
      const std::string sv = v;
      const std::size_t colon = sv.find(':');
      double secs = 0;
      std::uint64_t idx = 0;
      if (colon == std::string::npos || !parse_double(sv.substr(0, colon), 0.0, 3600.0, secs) || !parse_uint(sv.substr(colon + 1), 7, idx)) {
        std::fprintf(stderr, "error: invalid --scene-at '%s' (SECONDS:INDEX with SECONDS 0..3600 and INDEX a whole scene number 0..7)\n", sv.c_str());
        return 2;
      }
      SceneRecallAt rc;
      rc.frame = static_cast<std::uint64_t>(std::llround(secs * 48000.0));
      rc.index = static_cast<std::uint32_t>(idx);
      scene_recalls.push_back(rc);
    } else if (a == "--switch-at" && val(v)) {
      // SECONDS:LIVESET_FILE -- Live Set mode: switch seamlessly to that Live Set at that time (repeatable).
      const std::string sv = v;
      const std::size_t colon = sv.find(':');
      double secs = 0;
      if (colon == std::string::npos || colon + 1 >= sv.size() || !parse_double(sv.substr(0, colon), 0.0, 3600.0, secs)) {
        std::fprintf(stderr, "error: invalid --switch-at '%s' (SECONDS:LIVESET_FILE with SECONDS 0..3600)\n", sv.c_str());
        return 2;
      }
      SwitchAt sw;
      sw.frame = static_cast<std::uint64_t>(std::llround(secs * 48000.0));
      sw.live_set_path = sv.substr(colon + 1);
      switches.push_back(std::move(sw));
    } else if (a == "--encoder-at" && val(v)) {
      // SECONDS:ENC:DELTA -- Live Set mode: encoder ENC (1..8 = K1..K8) turned by DELTA relative steps at that time (repeatable). In ASSIGN mode it changes the
      // parameter mapped to that slot of the Live Set's ASSIGN map.
      const std::string sv = v;
      const std::size_t c1 = sv.find(':');
      const std::size_t c2 = c1 == std::string::npos ? std::string::npos : sv.find(':', c1 + 1);
      double secs = 0;
      std::uint64_t enc = 0;
      std::int64_t delta = 0;
      if (c1 == std::string::npos || c2 == std::string::npos || !parse_double(sv.substr(0, c1), 0.0, 3600.0, secs) ||
          !parse_uint(sv.substr(c1 + 1, c2 - c1 - 1), 8, enc) || enc < 1 || !parse_int(sv.substr(c2 + 1), -32768, 32767, delta)) {
        std::fprintf(stderr, "error: invalid --encoder-at '%s' (SECONDS:ENC:DELTA with SECONDS 0..3600, ENC a whole number 1..8 and DELTA a whole number -32768..32767)\n", sv.c_str());
        return 2;
      }
      EncoderAt ea;
      ea.frame = static_cast<std::uint64_t>(std::llround(secs * 48000.0));
      ea.encoder = static_cast<std::uint32_t>(enc);
      ea.delta = static_cast<std::int32_t>(delta);
      encoder_turns.push_back(ea);
    } else if (a == "--assign-mode") {
      assign_mode = true;
    } else if (a == "--load-setlist-at" && val(v)) {
      const std::string sv = v;
      const std::size_t colon = sv.find(':');
      double secs = 0;
      if (colon == std::string::npos || colon + 1 >= sv.size() || !parse_double(sv.substr(0, colon), 0.0, 3600.0, secs)) {
        std::fprintf(stderr, "error: invalid --load-setlist-at '%s' (SECONDS:SETLIST_ID with SECONDS 0..3600)\n", sv.c_str());
        return 2;
      }
      SetlistLoadAt sl;
      sl.frame = static_cast<std::uint64_t>(std::llround(secs * 48000.0));
      sl.setlist_id = sv.substr(colon + 1);
      setlist_loads.push_back(std::move(sl));
    } else if (a == "--step-at" && val(v)) {
      const std::string sv = v;
      const std::size_t colon = sv.find(':');
      double secs = 0;
      bool next = true;
      if (colon == std::string::npos) {
        if (!parse_double(sv, 0.0, 3600.0, secs)) {
          std::fprintf(stderr, "error: invalid --step-at '%s' (SECONDS[:next|prev])\n", sv.c_str());
          return 2;
        }
      } else {
        if (!parse_double(sv.substr(0, colon), 0.0, 3600.0, secs)) {
          std::fprintf(stderr, "error: invalid --step-at '%s' (SECONDS[:next|prev])\n", sv.c_str());
          return 2;
        }
        const std::string dir = sv.substr(colon + 1);
        if (dir == "prev" || dir == "previous") next = false;
        else if (dir != "next") {
          std::fprintf(stderr, "error: invalid --step-at direction '%s' (next or prev)\n", dir.c_str());
          return 2;
        }
      }
      SetlistStepAt st;
      st.frame = static_cast<std::uint64_t>(std::llround(secs * 48000.0));
      st.next = next;
      setlist_steps.push_back(st);
    } else if (a == "--setlist-dir" && val(v)) {
      setlist_dir = v;
    } else if (a == "--switch-load-blocks" && val(v)) {
      if (!parse_uint(v, 1000000, switch_load_blocks)) {
        std::fprintf(stderr, "error: invalid --switch-load-blocks '%s' (a whole number of 64-frame blocks, 0..1000000)\n", v);
        return 2;
      }
      switch_load_blocks_given = true;
    } else if (a == "--live-set" && val(v)) live_set_path = v;
    else if (a == "--library-root" && val(v)) library_root = v;
    else if (a == "--bundle" && val(v)) bundle_path = v;
#ifdef VELORA_OFFLINE_DEV_KEYS  // dev builds only: the option names are not even in the binary of a build without the dev key provider
    else if (a == "--dev-device-key" && val(v)) dev_device_key = v;
    else if (a == "--wrapped-key" && val(v)) wrapped_key_path = v;
    else if (a == "--trust-list" && val(v)) trust_list_path = v;
#endif
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
    } else if (a == "--syx" && val(v)) {
      syx_path = v;
    } else if (a == "--voice-slot" && val(v)) {
      if (!parse_uint(v, 32, voice_slot) || voice_slot < 1) {
        std::fprintf(stderr, "error: invalid --voice-slot (1..32)\n");
        return 2;
      }
      voice_slot_given = true;
    } else if (a == "--negative" && val(v)) {
      negative_opt = v;
    } else return usage();
  }
  if (!scene_recalls.empty() && live_set_path.empty()) {
    std::fprintf(stderr, "error: --scene-at needs --live-set\n");
    return 2;
  }
  if ((!switches.empty() || switch_load_blocks_given) && live_set_path.empty()) {
    std::fprintf(stderr, "error: --switch-at and --switch-load-blocks need --live-set\n");
    return 2;
  }
  if ((!encoder_turns.empty() || assign_mode) && live_set_path.empty()) {
    std::fprintf(stderr, "error: --encoder-at and --assign-mode need --live-set\n");
    return 2;
  }
  if ((!setlist_loads.empty() || !setlist_steps.empty()) && live_set_path.empty()) {
    std::fprintf(stderr, "error: --load-setlist-at and --step-at need --live-set\n");
    return 2;
  }
  if (switch_load_blocks_given && switches.empty()) {
    std::fprintf(stderr, "error: --switch-load-blocks needs --switch-at\n");
    return 2;
  }
  const bool protected_flags = !dev_device_key.empty() || !wrapped_key_path.empty() || !trust_list_path.empty();
  if (protected_flags) {
#ifdef VELORA_OFFLINE_DEV_KEYS
    if (dev_device_key.empty() || trust_list_path.empty()) {
      std::fprintf(stderr, "error: protected bundles need --dev-device-key and --trust-list (and --wrapped-key for --bundle)\n");
      return 2;
    }
    if (live_set_path.empty() && wrapped_key_path.empty()) {
      std::fprintf(stderr, "error: --dev-device-key with --bundle needs --wrapped-key\n");
      return 2;
    }
    if (!live_set_path.empty() && !wrapped_key_path.empty()) {
      std::fprintf(stderr, "error: --wrapped-key goes with --bundle; a Live Set finds wrapped-key.bin in each library\n");
      return 2;
    }
#else
    std::fprintf(stderr, "error: this build has no dev key provider\n");
    return 2;
#endif
  }
#ifdef VELORA_OFFLINE_DEV_KEYS
  // Story 6.5: the dev key provider, the trusted keys and the protected head provider. The process writes no core file (the content key and the decoded PCM are in its memory).
  std::unique_ptr<velora::host::DevKeyProvider> dev_keys;
  std::shared_ptr<const velora::format::TrustedKeys> trusted;
  std::unique_ptr<velora::host::ProtectedHeadProvider> protected_heads;
  velora::host::DamageRegistry damage;
  if (protected_flags) {
    if (!velora::host::disable_core_dumps()) {
      std::fprintf(stderr, "error: cannot disable core dumps (RLIMIT_CORE); refusing to hold content keys\n");
      return 1;
    }
    struct rlimit core_rl;
    ::getrlimit(RLIMIT_CORE, &core_rl);
    std::printf("core_limit=%llu/%llu\n", (unsigned long long)core_rl.rlim_cur, (unsigned long long)core_rl.rlim_max);
    std::string kerr;
    velora::format::TrustedKeys tk;
    if (!velora::host::load_trust_list_file(trust_list_path, tk, kerr)) {
      std::fprintf(stderr, "error: %s\n", kerr.c_str());
      return 1;
    }
    trusted = std::make_shared<const velora::format::TrustedKeys>(std::move(tk));
    dev_keys = velora::host::DevKeyProvider::from_file(dev_device_key, {}, kerr);
    if (!dev_keys) {
      std::fprintf(stderr, "error: %s\n", kerr.c_str());
      return 1;
    }
    protected_heads = std::make_unique<velora::host::ProtectedHeadProvider>(velora::host::make_posix_fs(), *dev_keys, &damage);
  }
#endif
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
    lo.scene_recalls = scene_recalls;
    lo.switches = switches;
    lo.switch_load_blocks = static_cast<std::uint32_t>(switch_load_blocks);
    lo.encoders = encoder_turns;
    lo.assign_mode = assign_mode;
    lo.setlist_dir = setlist_dir;
    lo.setlist_loads = setlist_loads;
    lo.setlist_steps = setlist_steps;
#ifdef VELORA_OFFLINE_DEV_KEYS
    if (protected_flags) {
      lo.heads = protected_heads.get();
      lo.protected_trust = trusted;
    }
#endif
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
    if (!scene_recalls.empty()) std::printf("live set: %llu scene recall(s)\n", static_cast<unsigned long long>(lrep.scenes_recalled));
    if (!switches.empty()) std::printf("live set: %llu live set switch(es)\n", static_cast<unsigned long long>(lrep.switches_published));
    if (!encoder_turns.empty())
      std::printf("live set: %llu encoder step(s) applied, %llu on empty slots, %llu outside ASSIGN mode, %llu refused\n", static_cast<unsigned long long>(lrep.encoder_steps_applied),
                  static_cast<unsigned long long>(lrep.encoder_steps_empty), static_cast<unsigned long long>(lrep.encoder_steps_unrouted),
                  static_cast<unsigned long long>(lrep.encoder_steps_rejected));
    std::printf("rendered %zu events -> %s (%llu frames, 48000 Hz, 2 ch, float32)\n", lsmf.events.size(), out.c_str(),
                static_cast<unsigned long long>(lwav.frames()));
    return 0;
  }
  if ((midi.empty() && events_path.empty()) || out.empty()) return usage();
  if (engine != "test-tone" && engine != "sample" && engine != "organ" && engine != "fm") {
    std::fprintf(stderr, "error: unknown engine '%s' (test-tone, sample, organ or fm)\n", engine.c_str());
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
  if (engine == "fm") {
    if (syx_path.empty()) {
      std::fprintf(stderr, "error: --engine fm needs --syx\n");
      return 2;
    }
    if (drawbars_given || percussion_given || key_click_given || vibrato_given || leakage_given || rotary_given) {
      std::fprintf(stderr, "error: organ options cannot be used with --engine fm\n");
      return 2;
    }
    if (sample_opts || !bundle_path.empty() || !instrument_key.empty()) {
      std::fprintf(stderr, "error: sample options cannot be used with --engine fm\n");
      return 2;
    }
    auto syx_res = velora::format::parse_sysex_file(syx_path);
    if (!syx_res.ok()) {
      std::fprintf(stderr, "error: %s\n", syx_res.error_message.c_str());
      return 1;
    }
    if (syx_res.voices.empty()) {
      std::fprintf(stderr, "error: no voices found in SysEx file\n");
      return 1;
    }
    if (voice_slot > syx_res.voices.size()) {
      std::fprintf(stderr, "error: voice slot %llu out of range (1..%zu)\n", (unsigned long long)voice_slot, syx_res.voices.size());
      return 2;
    }
    const auto& vdump = syx_res.voices[voice_slot - 1];
    opt.fm_params = to_core(vdump.parameters);
    if (!negative_opt.empty()) {
      if (negative_opt == "feedback") {
        opt.fm_params.feedback = (opt.fm_params.feedback == 0) ? 7 : 0;
      } else if (negative_opt == "algo") {
        opt.fm_params.algorithm = (opt.fm_params.algorithm == 1) ? 32 : 1;
      } else if (negative_opt == "gain") {
        opt.fm_params.operators[0].total_level = (opt.fm_params.operators[0].total_level >= 20) ? opt.fm_params.operators[0].total_level - 20 : 99;
      } else {
        std::fprintf(stderr, "error: unknown --negative option '%s' (feedback, algo or gain)\n", negative_opt.c_str());
        return 2;
      }
    }
    opt.engine = EngineKind::kFm;
  } else {
    if (!syx_path.empty() || voice_slot_given || !negative_opt.empty()) {
      std::fprintf(stderr, "error: --syx, --voice-slot and --negative need --engine fm\n");
      return 2;
    }
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
  std::shared_ptr<const velora::host::BundleInstrument> protected_instrument;  // Story 6.5: from a protected bundle (it owns the content key until it dies)
  const velora::host::BundleInstrument* inst = nullptr;
  std::unique_ptr<velora::format::BundleReader> reader;
  if (engine == "sample") {
    if (bundle_path.empty() || instrument_key.empty()) {
      std::fprintf(stderr, "error: --engine sample needs --bundle and --instrument\n");
      return 2;
    }
    if (!voices_given) opt.voices = 64;
    if (protected_flags) {
#ifdef VELORA_OFFLINE_DEV_KEYS
      auto view = velora::format::ProtectedBundleView::open_file(bundle_path, *trusted);
      if (!view.ok()) {
        std::fprintf(stderr, "error: %s: cannot open protected bundle (%s)\n", bundle_path.c_str(), velora::format::to_string(view.error()));
        return 1;
      }
      auto shared_view = std::make_shared<const velora::format::ProtectedBundleView>(view.take());
      std::size_t index = shared_view->instruments().size();
      for (std::size_t i = 0; i < shared_view->instruments().size(); ++i)
        if (shared_view->instruments()[i].key == instrument_key) index = i;
      if (index == shared_view->instruments().size()) {
        std::fprintf(stderr, "error: %s: instrument '%s': %s\n", bundle_path.c_str(), instrument_key.c_str(), velora::host::to_string(velora::host::LoadStatus::kInstrumentNotFound));
        return 1;
      }
      std::vector<std::uint8_t> blob;
      {
        std::FILE* f = std::fopen(wrapped_key_path.c_str(), "rb");
        if (!f) {
          std::fprintf(stderr, "error: %s: cannot be read\n", wrapped_key_path.c_str());
          return 1;
        }
        std::uint8_t buf[512];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && blob.size() < 4096) blob.insert(blob.end(), buf, buf + n);
        std::fclose(f);
      }
      const std::string lib_text = velora::format::uuid_to_string(shared_view->info().library_id);
      const velora::host::LoadStatus st = protected_heads->load_with_blob(shared_view, lib_text, index, blob, protected_instrument);
      if (st != velora::host::LoadStatus::kOk) {
        std::fprintf(stderr, "error: %s: instrument '%s': %s\n", bundle_path.c_str(), instrument_key.c_str(), velora::host::to_string(st));
        return 1;
      }
      inst = protected_instrument.get();
#endif
    } else {
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
      inst = instrument.get();
    }
    opt.engine = EngineKind::kSample;
    opt.instrument = inst;
  } else if (engine == "organ" || engine == "fm") {
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
  if (engine == "sample" && stats.unplayable_regions >= inst->region_count()) {
    std::fprintf(stderr, "error: instrument '%s' has no playable region (%zu regions, all lack usable sample data or have invalid parameters); nothing written\n",
                 instrument_key.c_str(), inst->region_count());
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
                inst->sample_count(), inst->resident_bytes(), inst->failed_samples(),
                stats.unplayable_regions, static_cast<unsigned long long>(stats.skipped_unplayable_starts),
                static_cast<unsigned long long>(stats.pool_exhausted));
  }
  std::printf("rendered %zu events -> %s (%llu frames, 48000 Hz, 2 ch, float32)\n", smf.events.size(),
              out.c_str(), static_cast<unsigned long long>(wav.frames()));
  return 0;
}
