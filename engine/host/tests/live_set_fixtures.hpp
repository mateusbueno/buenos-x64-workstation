#pragma once
// Fixed Live Sets shared by live_set_test (golden + corpus drift check) and live_set_corpus_gen (regeneration).
// Everything is deterministic: ids come from a seeded random source, no clock, no OS randomness.
#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/unknown_field_set.h>

#include "velora/host/live_set_store.hpp"

namespace fixtures {

namespace ls = velora::host::liveset;
namespace lsp = velora::liveset::v1;

inline std::string uuid_n(std::uint64_t seed) {
  ls::SeededRandom r(seed);
  return ls::mint_uuid_v4(r);
}

inline void entry(google::protobuf::RepeatedPtrField<lsp::ParamEntry>* l, std::uint32_t id, float v) {
  lsp::ParamEntry* e = l->Add();
  e->set_param_id(id);
  e->set_value(v);
}

inline void timbre(lsp::Part* p, const std::string& lib, const std::string& key) {
  p->mutable_timbre()->set_library_id(lib);
  p->mutable_timbre()->set_instrument_key(key);
}

inline void unknown_varint(google::protobuf::Message* m, int field, std::uint64_t v) {
  m->GetReflection()->MutableUnknownFields(m)->AddVarint(field, v);
}

inline void clear_fixture_arp(lsp::LiveSet& l) {
  for (int i = 0; i < l.parts_size(); ++i) l.mutable_parts(i)->clear_arp();
}

// The smallest valid Live Set of the tier.
inline lsp::LiveSet minimal(std::uint32_t parts = 8) {
  lsp::LiveSet l = ls::make_default_live_set(uuid_n(1), "Minimal", parts);
  clear_fixture_arp(l);
  return l;
}

// Literal tables (no float arithmetic: contraction/FMA differs between compilers and would change the golden bytes).
inline constexpr float kSends[17] = {0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.0f, 0.05f, 0.1f, 0.15f, 0.2f, 0.25f, 0.3f, 0.35f, 0.4f, 0.45f, 0.5f, 0.55f};

// A show-sized Live Set: layers, a split, an organ part with drawbars and a rotary insert, sends, 8 sparse and full scenes,
// a full ASSIGN map and master parameters.
inline lsp::LiveSet full(std::uint32_t parts = 8) {
  lsp::LiveSet l = ls::make_default_live_set(uuid_n(2), parts == 8 ? "Full show" : "Pro show", parts);
  clear_fixture_arp(l);
  l.set_tempo(96.5f);
  const std::string lib_a = uuid_n(100), lib_b = uuid_n(101);
  for (std::uint32_t i = 0; i < parts; ++i) {
    lsp::Part* p = l.mutable_parts(static_cast<int>(i));
    p->set_volume(static_cast<float>(64 + 4 * (i % 8)) / 128.0f);  // dyadic: exact on every compiler
    p->set_send_reverb(kSends[i % 5]);
    p->set_send_delay(kSends[(i % 4) + 5]);
    if (i % 2 == 1) p->set_output(ws::v1::PART_OUTPUT_AUX);
    p->set_keyboard_on(i % 7 != 6);
    timbre(p, i % 3 == 0 ? lib_a : lib_b, "Piano/Grand " + std::to_string(i) + ".sfz");
  }
  // Split: part 0 lower half, part 1 upper half, one octave up.
  l.mutable_parts(0)->set_high_key(59);
  l.mutable_parts(1)->set_low_key(60);
  l.mutable_parts(1)->set_octave(1);
  l.mutable_parts(1)->set_transpose(-2);
  // Part 2: the organ (no timbre), drawbars 888000000, rotary insert.
  lsp::Part* organ = l.mutable_parts(2);
  organ->clear_timbre();
  organ->mutable_engine()->set_engine_type_id(2);
  entry(organ->mutable_engine()->mutable_params(), 100, 8);
  entry(organ->mutable_engine()->mutable_params(), 101, 8);
  entry(organ->mutable_engine()->mutable_params(), 102, 8);
  entry(organ->mutable_engine()->mutable_params(), 116, 0.25f);
  entry(organ->mutable_eq(), 117, 0.5f);  // stand-in: the registry has no per-part envelope/EQ ids yet
  entry(organ->mutable_envelope(), 116, 0.75f);
  ws::v1::InsertSlot* s = organ->add_inserts();
  s->set_type(ws::v1::INSERT_TYPE_ROTARY);
  s->add_params(0.5f);
  s->add_params(0.25f);
  ws::v1::InsertSlot* s2 = organ->add_inserts();
  s2->set_type(ws::v1::INSERT_TYPE_EQ);
  s2->set_bypass(true);
  entry(l.mutable_master(), 520, -3.0f);   // master_level
  entry(l.mutable_master(), 531, 3.5f);    // reverb_time
  entry(l.mutable_master(), 541, 1.0f);    // delay_sync
  const std::uint32_t assign[8] = {100, 101, 102, 400, 401, 801, 116, 0};
  for (int k = 0; k < 8; ++k) l.set_assign(k, assign[k]);
  // Scenes: 1 = sparse (one part louder), 2 = full-ish (zones, inserts replaced, sends, engine params), others names only.
  for (int si = 0; si < 8; ++si) {
    lsp::Scene* sc = l.add_scenes();
    sc->set_name("Scene " + std::to_string(si + 1));
    if (si == 0) {
      sc->add_parts()->set_volume(0.9f);
    } else if (si == 1) {
      for (std::uint32_t pi = 0; pi < parts; ++pi) {
        lsp::ScenePart* sp = sc->add_parts();
        sp->set_volume(kSends[pi % 8 + 9]);
        sp->set_keyboard_on(pi % 2 == 0);
        sp->set_octave(-1);
        sp->set_transpose(3);
        sp->set_low_key(24);
        sp->set_high_key(96);
        sp->set_output(ws::v1::PART_OUTPUT_MAIN);
        sp->set_send_reverb(0.7f);
        sp->set_send_delay(0.0f);
        sp->mutable_inserts();  // present and empty = "no inserts in this scene"
      }
      entry(sc->mutable_parts(2)->mutable_engine_params(), 102, 4);
      entry(sc->mutable_parts(2)->mutable_envelope(), 116, 0.1f);
      entry(sc->mutable_master(), 530, 2);  // reverb_type plate
    }
  }
  return l;
}

// A part of a reserved engine type with no compiled engine (FM, VA) next to ordinary parts.
inline lsp::LiveSet fm_part() {
  lsp::LiveSet l = ls::make_default_live_set(uuid_n(3), "FM and VA", 8);
  clear_fixture_arp(l);
  lsp::Part* fm = l.mutable_parts(0);
  fm->mutable_engine()->set_engine_type_id(3);
  entry(fm->mutable_engine()->mutable_params(), 15001, 0.5f);  // engine-specific ids unknown to this build: preserved
  entry(fm->mutable_engine()->mutable_params(), 15002, 3.0f);
  lsp::Part* va = l.mutable_parts(1);
  va->mutable_engine()->set_engine_type_id(4);
  entry(va->mutable_engine()->mutable_params(), 15101, 0.25f);
  return l;
}

// Parts whose libraries are not installed anywhere: the file is still valid, the part just plays silent later.
inline lsp::LiveSet missing_library() {
  lsp::LiveSet l = ls::make_default_live_set(uuid_n(4), "Missing library", 8);
  clear_fixture_arp(l);
  timbre(l.mutable_parts(0), uuid_n(9000), "Strings/Ensemble.sfz");
  timbre(l.mutable_parts(3), uuid_n(9001), "Brass/Section.sfz");
  return l;
}

// What a newer firmware may add: unknown fields at the top level, inside a part, an engine block and a scene.
inline lsp::LiveSet newer_schema() {
  lsp::LiveSet l = full(8);
  l.set_id(uuid_n(5));
  l.set_name("Newer schema");
  unknown_varint(&l, 500, 7);
  l.GetReflection()->MutableUnknownFields(&l)->AddLengthDelimited(501, "from the future");
  unknown_varint(l.mutable_parts(0), 600, 1);
  unknown_varint(l.mutable_parts(0)->mutable_engine(), 601, 2);
  unknown_varint(l.mutable_scenes(0), 602, 3);
  return l;
}

// The set whose serialised bytes are pinned by golden/live_set_fixed.bin.
inline lsp::LiveSet golden() {
  lsp::LiveSet l = ls::make_default_live_set("3f2b8c1e-5a4d-4e6f-9a7b-0c1d2e3f4a5b", "Golden", 8);
  clear_fixture_arp(l);
  l.set_tempo(123.5f);
  timbre(l.mutable_parts(0), "0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d", "Keys/EP.sfz");
  l.mutable_parts(0)->set_high_key(71);
  l.mutable_parts(1)->set_low_key(72);
  l.mutable_parts(1)->set_octave(-1);
  l.mutable_parts(1)->set_transpose(5);
  l.mutable_parts(1)->set_output(ws::v1::PART_OUTPUT_AUX);
  l.mutable_parts(1)->set_send_reverb(0.25f);
  ws::v1::InsertSlot* s = l.mutable_parts(2)->add_inserts();
  s->set_type(ws::v1::INSERT_TYPE_COMPRESSOR);
  s->add_params(0.5f);
  l.mutable_parts(2)->mutable_engine()->set_engine_type_id(2);
  entry(l.mutable_parts(2)->mutable_engine()->mutable_params(), 102, 6);
  lsp::Scene* sc = l.add_scenes();
  sc->set_name("Chorus");
  sc->add_parts()->set_volume(0.75f);
  l.set_assign(0, 100);
  l.set_assign(7, 802);
  entry(l.mutable_master(), 520, -6.0f);
  return l;
}

struct CorpusFile {
  const char* name;
  lsp::LiveSet set;
};

inline std::vector<CorpusFile> corpus() {
  std::vector<CorpusFile> v;
  v.push_back({"minimal.liveset", minimal()});
  v.push_back({"full-8-parts.liveset", full(8)});
  v.push_back({"fm-va-parts.liveset", fm_part()});
  v.push_back({"missing-library.liveset", missing_library()});
  v.push_back({"pro-16-parts.liveset", full(16)});
  v.push_back({"newer-schema-unknown-fields.liveset", newer_schema()});
  return v;
}

}  // namespace fixtures
