// Writes the committed fixtures of the Live Set golden (Story 4.3):  live-set-fixture-gen OUT_DIR
//   OUT_DIR/root/lib/<library_id>/<bundle_id>/bundle.vlb + OUT_DIR/root/lib/<library_id>/current -> <bundle_id>   (three tiny libraries)
//   OUT_DIR/layer_split.liveset, wrong_split.liveset, wrong_octave.liveset                                       (the Live Set and two wrong variants)
//   OUT_DIR/scenes.liveset                                                                                       (Story 4.4: the same libraries, an organ part and three scenes)
//   OUT_DIR/switch/switch_a.liveset, switch_b.liveset                                                            (Story 4.5: two Live Sets to switch between, the same libraries)
// Everything is integer arithmetic (no libm), so the files are identical on every platform. Regenerate only on purpose:
//   cmake --build build --target live-set-fixtures   (then review the diff, then re-render the golden: update-live-set-golden)
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "velora/format/bundle.hpp"
#include "velora/host/live_set_store.hpp"

namespace fmt = velora::format;
namespace ls = velora::host::liveset;
namespace lsp = ls::lsp;

static bool mkdir_p(const std::string& p) {
  for (std::size_t i = 1; i <= p.size(); ++i) {
    if (i != p.size() && p[i] != '/') continue;
    if (::mkdir(p.substr(0, i).c_str(), 0755) != 0 && errno != EEXIST) return false;
  }
  return true;
}

struct Voice {
  const char* key;       // instrument key (the file name in the library)
  std::uint32_t index;   // library index (fingerprint)
  std::uint32_t period;  // frames per cycle at the key center
  int shape;             // 0 triangle, 1 square plus triangle, 2 stereo triangles
  std::uint8_t center;
  fmt::AmpEnvelope env;
  float volume_db;
};

// Integer waveforms, 16 bit.
static std::int32_t tri(std::uint32_t phase, std::uint32_t period, std::int32_t amp) {
  const std::uint32_t half = period / 2;
  const std::int32_t t = static_cast<std::int32_t>(phase < half ? phase : period - phase);  // 0..half
  return (t * 2 - static_cast<std::int32_t>(half)) * amp / static_cast<std::int32_t>(half);
}

static std::string install(const std::string& root, const Voice& v) {
  const std::string fp = "velora-liveset-golden/" + std::to_string(v.index);
  const fmt::Uuid lib = fmt::library_id_from_fingerprint({reinterpret_cast<const std::uint8_t*>(fp.data()), fp.size()});
  fmt::BundleWriter w;
  w.set_library_id(lib);
  auto ins = w.add_instrument(v.key, v.key);
  if (!ins.ok()) std::exit(1);
  const std::uint32_t frames = v.period * 8;
  const bool stereo = v.shape == 2;
  std::vector<std::int32_t> pcm(static_cast<std::size_t>(frames) * (stereo ? 2 : 1));
  for (std::uint32_t i = 0; i < frames; ++i) {
    const std::uint32_t ph = i % v.period;
    if (v.shape == 0) {
      pcm[i] = tri(ph, v.period, 14000);
    } else if (v.shape == 1) {
      pcm[i] = ((ph < v.period / 2) ? 6000 : -6000) + tri(ph, v.period, 5000);
    } else {
      pcm[2 * i] = tri(ph, v.period, 9000);
      pcm[2 * i + 1] = tri((ph + v.period / 4) % v.period, v.period, 9000);
    }
  }
  if (w.add_sample(ins.value(), {"s", {static_cast<std::uint8_t>(stereo ? 2 : 1), 16, false, 48000}, frames, pcm}) != fmt::Error::kNone) std::exit(1);
  fmt::Region r;
  r.sample = 0;
  r.key_lo = 0;
  r.key_hi = 127;
  r.key_center = v.center;
  r.loop_mode = fmt::LoopMode::kLoopContinuous;
  r.ampeg = v.env;
  r.volume_db = v.volume_db;
  if (w.add_region(ins.value(), r) != fmt::Error::kNone) std::exit(1);
  auto bytes = w.finalize();
  if (!bytes.ok()) std::exit(1);
  auto probe = fmt::BundleReader::open_memory(bytes.value());
  if (!probe.ok()) std::exit(1);
  const std::string lib_id = fmt::uuid_to_string(lib), bundle_id = fmt::to_hex(probe.value().bundle_id());
  const std::string dir = root + "/lib/" + lib_id + "/" + bundle_id;
  if (!mkdir_p(dir)) std::exit(1);
  std::ofstream f(dir + "/bundle.vlb", std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(bytes.value().data()), static_cast<std::streamsize>(bytes.value().size()));
  f.close();
  const std::string link = root + "/lib/" + lib_id + "/current";
  ::unlink(link.c_str());
  if (::symlink(bundle_id.c_str(), link.c_str()) != 0) std::exit(1);
  std::printf("library %s %s (%s)\n", lib_id.c_str(), v.key, bundle_id.c_str());
  return lib_id;
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: live-set-fixture-gen OUT_DIR\n");
    return 2;
  }
  const std::string out = argv[1];
  const std::string root = out + "/root";
  //                            key                  idx period shape center  delay start attack  hold decay  sustain release     dB
  const std::string bass = install(root, {"bass/electric.sfz", 1, 480, 0, 36, {0, 0, 0.005f, 0, 0.6f, 60, 0.25f}, 0.0f});
  const std::string piano = install(root, {"piano/grand.sfz", 2, 400, 1, 60, {0, 0, 0.002f, 0, 1.2f, 25, 0.35f}, 0.0f});
  const std::string pad = install(root, {"pad/strings.sfz", 3, 960, 2, 60, {0, 0, 0.25f, 0, 0.5f, 90, 0.8f}, -4.0f});

  // The Live Set and two deliberately wrong variants: the golden must be sensitive to the zone boundary and to the octave of a part.
  struct Variant {
    const char* file;
    std::uint32_t bass_high;   // the top of the bass zone (47 = B2); the piano starts one key above
    int pad_octave;
  };
  const Variant variants[] = {{"layer_split.liveset", 47, -1}, {"wrong_split.liveset", 48, -1}, {"wrong_octave.liveset", 47, 0}};
  for (const Variant& var : variants) {
    lsp::LiveSet s = ls::make_default_live_set("6b0d2c8e-31f4-4a57-9c1e-7d5a40b2e9f3", "Layer and split", 8);
    s.set_tempo(120.0f);
    auto timbre = [&](int p, const std::string& lib, const char* key) {
      s.mutable_parts(p)->mutable_timbre()->set_library_id(lib);
      s.mutable_parts(p)->mutable_timbre()->set_instrument_key(key);
    };
    // 0: bass, the lower zone up to B2 (47).  1: piano from C3 (48).  2: a pad layered under the whole keyboard, an octave down, on the Aux bus.
    // 3: the piano doubled an octave up above C5 (72), quietly.  4: a pad with the keyboard off (silent).  5: a library that is not installed
    // (silent).  6: an FM part (no engine compiled in: silent).  7: a sample part with no timbre (silent).
    timbre(0, bass, "bass/electric.sfz");
    s.mutable_parts(0)->set_high_key(var.bass_high);
    s.mutable_parts(0)->set_volume(0.9f);
    timbre(1, piano, "piano/grand.sfz");
    s.mutable_parts(1)->set_low_key(var.bass_high + 1);
    s.mutable_parts(1)->set_volume(0.8f);
    s.mutable_parts(1)->set_send_reverb(0.25f);
    timbre(2, pad, "pad/strings.sfz");
    s.mutable_parts(2)->set_octave(var.pad_octave);
    s.mutable_parts(2)->set_volume(0.5f);
    s.mutable_parts(2)->set_output(ws::v1::PART_OUTPUT_AUX);
    timbre(3, piano, "piano/grand.sfz");
    s.mutable_parts(3)->set_low_key(72);
    s.mutable_parts(3)->set_octave(1);
    s.mutable_parts(3)->set_transpose(0);
    s.mutable_parts(3)->set_volume(0.25f);
    timbre(4, pad, "pad/strings.sfz");
    s.mutable_parts(4)->set_keyboard_on(false);
    timbre(5, "7c1f0e9a-2b3d-4c5e-8f60-a1b2c3d4e5f6", "ghost/missing.sfz");
    s.mutable_parts(6)->mutable_engine()->set_engine_type_id(3);
    if (!ls::validate(s, 8).ok()) {
      std::fprintf(stderr, "the fixture Live Set is not valid\n");
      return 1;
    }
    std::string bytes;
    if (ls::serialize(s, bytes) != ls::Status::kOk) return 1;
    std::ofstream f(out + "/" + var.file, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  // Story 4.4: scenes.liveset. Bass / piano split at B2 / C3, a pad (an octave down, Aux) and an organ, with three scenes that move parameters
  // only: verse (the pad joins on the low keys), chorus (the pad on the whole keyboard and the organ come in) and outro (the piano alone).
  // The base has the pad and the organ keyboards off, so the scenes decide who plays.
  {
    lsp::LiveSet s = ls::make_default_live_set("c24a7e51-9d0b-4f3e-8a62-5b1e0d7f3c94", "Scenes show", 8);
    s.set_tempo(120.0f);
    auto timbre = [&](int p, const std::string& lib, const char* key) {
      s.mutable_parts(p)->mutable_timbre()->set_library_id(lib);
      s.mutable_parts(p)->mutable_timbre()->set_instrument_key(key);
    };
    timbre(0, bass, "bass/electric.sfz");
    s.mutable_parts(0)->set_high_key(47);
    s.mutable_parts(0)->set_volume(0.9f);
    timbre(1, piano, "piano/grand.sfz");
    s.mutable_parts(1)->set_low_key(48);
    s.mutable_parts(1)->set_volume(0.8f);
    s.mutable_parts(1)->set_send_reverb(0.25f);
    timbre(2, pad, "pad/strings.sfz");
    s.mutable_parts(2)->set_octave(-1);
    s.mutable_parts(2)->set_volume(0.5f);
    s.mutable_parts(2)->set_output(ws::v1::PART_OUTPUT_AUX);
    s.mutable_parts(2)->set_keyboard_on(false);
    lsp::Part* organ = s.mutable_parts(3);  // part 3: the organ, drawbars 8 8 0 (16' 8'), no keyboard until the chorus
    organ->mutable_engine()->set_engine_type_id(2);
    organ->set_volume(0.5f);
    organ->set_keyboard_on(false);
    auto add_param = [](google::protobuf::RepeatedPtrField<lsp::ParamEntry>* l, std::uint32_t id, float v) {
      lsp::ParamEntry* e = l->Add();
      e->set_param_id(id);
      e->set_value(v);
    };
    add_param(organ->mutable_engine()->mutable_params(), 100, 8);  // drawbar_16
    add_param(organ->mutable_engine()->mutable_params(), 102, 6);  // drawbar_8
    // verse: the pad joins on the low keys (up to B3), the piano a little back.
    lsp::Scene* verse = s.add_scenes();
    verse->set_name("Verse");
    verse->add_parts();
    verse->mutable_parts(0)->set_volume(0.8f);
    lsp::ScenePart* v1 = verse->add_parts();
    v1->set_volume(0.6f);
    lsp::ScenePart* v2 = verse->add_parts();
    v2->set_keyboard_on(true);
    v2->set_high_key(59);
    // chorus: the pad on the whole keyboard, the organ comes in with a brighter registration, more reverb on the piano.
    lsp::Scene* chorus = s.add_scenes();
    chorus->set_name("Chorus");
    chorus->add_parts();
    lsp::ScenePart* c1 = chorus->add_parts();
    c1->set_send_reverb(0.4f);
    lsp::ScenePart* c2 = chorus->add_parts();
    c2->set_keyboard_on(true);
    c2->set_volume(0.7f);
    lsp::ScenePart* c3 = chorus->add_parts();
    c3->set_keyboard_on(true);
    add_param(c3->mutable_engine_params(), 102, 8);  // drawbar_8
    add_param(c3->mutable_engine_params(), 104, 5);  // drawbar_2_2_3
    // outro: the piano alone, softer; the pad and organ keyboards off, the bass off.
    lsp::Scene* outro = s.add_scenes();
    outro->set_name("Outro");
    lsp::ScenePart* o0 = outro->add_parts();
    o0->set_keyboard_on(false);
    lsp::ScenePart* o1 = outro->add_parts();
    o1->set_volume(0.55f);
    o1->set_send_reverb(0.15f);
    if (!ls::validate(s, 8).ok()) {
      std::fprintf(stderr, "the scenes fixture Live Set is not valid\n");
      return 1;
    }
    std::string bytes;
    if (ls::serialize(s, bytes) != ls::Status::kOk) return 1;
    std::ofstream f(out + "/scenes.liveset", std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  // Story 4.5: two Live Sets for the seamless switch golden (switch_a -> switch_b). A: bass / piano split at B2 / C3 with a long-release pad layered
  // an octave down on the Aux bus. B: the pad on the whole keyboard (Main), the piano an octave down above C3 with reverb, the bass an octave up
  // below C4 and an organ above C4 -- different zones, octaves, outputs and engines, and B reserves the pool for an organ where A does not.
  {
    if (!mkdir_p(out + "/switch")) return 1;
    auto make = [&](const char* id, const char* name) {
      lsp::LiveSet s = ls::make_default_live_set(id, name, 8);
      s.set_tempo(120.0f);
      return s;
    };
    auto timbre = [](lsp::LiveSet& s, int p, const std::string& lib, const char* key) {
      s.mutable_parts(p)->mutable_timbre()->set_library_id(lib);
      s.mutable_parts(p)->mutable_timbre()->set_instrument_key(key);
    };
    lsp::LiveSet a = make("a5d1e7b2-4c3f-4e8a-9b60-1f2c3d4e5f60", "Switch A");
    timbre(a, 0, bass, "bass/electric.sfz");
    a.mutable_parts(0)->set_high_key(47);
    a.mutable_parts(0)->set_volume(0.9f);
    timbre(a, 1, piano, "piano/grand.sfz");
    a.mutable_parts(1)->set_low_key(48);
    a.mutable_parts(1)->set_volume(0.8f);
    a.mutable_parts(1)->set_send_reverb(0.25f);
    timbre(a, 2, pad, "pad/strings.sfz");
    a.mutable_parts(2)->set_octave(-1);
    a.mutable_parts(2)->set_volume(0.5f);
    a.mutable_parts(2)->set_output(ws::v1::PART_OUTPUT_AUX);
    lsp::LiveSet b = make("b8e2f6a3-5d40-4f9b-8c71-2a3b4c5d6e71", "Switch B");
    timbre(b, 0, pad, "pad/strings.sfz");
    b.mutable_parts(0)->set_volume(0.6f);
    timbre(b, 1, piano, "piano/grand.sfz");
    b.mutable_parts(1)->set_low_key(48);
    b.mutable_parts(1)->set_octave(-1);
    b.mutable_parts(1)->set_volume(0.7f);
    b.mutable_parts(1)->set_send_reverb(0.4f);
    timbre(b, 2, bass, "bass/electric.sfz");
    b.mutable_parts(2)->set_high_key(59);
    b.mutable_parts(2)->set_octave(1);
    b.mutable_parts(2)->set_volume(0.8f);
    lsp::Part* organ = b.mutable_parts(3);
    organ->mutable_engine()->set_engine_type_id(2);
    organ->set_low_key(60);
    organ->set_volume(0.4f);
    auto add_param = [](google::protobuf::RepeatedPtrField<lsp::ParamEntry>* l, std::uint32_t id, float v) {
      lsp::ParamEntry* e = l->Add();
      e->set_param_id(id);
      e->set_value(v);
    };
    add_param(organ->mutable_engine()->mutable_params(), 100, 8);  // drawbar_16
    add_param(organ->mutable_engine()->mutable_params(), 102, 6);  // drawbar_8
    for (lsp::LiveSet* s : {&a, &b}) {
      if (!ls::validate(*s, 8).ok()) {
        std::fprintf(stderr, "a switch fixture Live Set is not valid\n");
        return 1;
      }
    }
    const std::pair<const char*, lsp::LiveSet*> files[] = {{"switch_a.liveset", &a}, {"switch_b.liveset", &b}};
    for (const auto& fl : files) {
      std::string bytes;
      if (ls::serialize(*fl.second, bytes) != ls::Status::kOk) return 1;
      std::ofstream f(out + "/switch/" + fl.first, std::ios::binary | std::ios::trunc);
      f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
  }
  return 0;
}
