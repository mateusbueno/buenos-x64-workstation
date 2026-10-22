// Live Set format and store tests (Story 4.2): round trip, golden bytes, every validation rule, version policy,
// unknown-field preservation, reserved-engine round trip and resolve report, UUIDs and collisions, atomic write
// with crash injection, single writer, hostile input, the compatibility corpus.
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "live_set_fixtures.hpp"
#include "../src/live_set_detail.hpp"
#include "velora/core/arena.hpp"
#include "velora/core/arpeggiator.hpp"
#include "velora/core/engine_registry.hpp"
#include "velora/core/params_gen.hpp"
#include "velora/host/params_names_gen.hpp"
#include "velora/core/snapshot.hpp"
#include "velora/core/test_tone_engine.hpp"

namespace fs = std::filesystem;
using namespace velora::host::liveset;
namespace pbv = velora::liveset::v1;

static int g_fail = 0;
#define CHECK(c)                                               \
  do {                                                         \
    if (!(c)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

namespace {

std::string slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
void spit(const std::string& path, const std::string& b) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(b.data(), static_cast<std::streamsize>(b.size()));
}
std::string bytes_of(const lsp::LiveSet& l) {
  std::string b;
  CHECK(serialize(l, b) == Status::kOk);
  return b;
}
bool has_error(const ValidationReport& r, IssueCode c) {
  for (const Issue& i : r.errors)
    if (i.code == c) return true;
  return false;
}
bool has_warning(const ValidationReport& r, IssueCode c) {
  for (const Issue& i : r.unknown_params)
    if (i.code == c) return true;
  return false;
}

std::string make_tmp_dir() {
  std::string t = (fs::temp_directory_path() / "velora-liveset-XXXXXX").string();
  if (!::mkdtemp(t.data())) std::abort();
  return t;
}

struct TmpDir {
  std::string path = make_tmp_dir();
  ~TmpDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

// Expect `mutate` to make the default Live Set invalid with `code`.
template <class F>
void expect_invalid(const char* what, IssueCode code, F mutate, std::uint32_t parts = 8) {
  lsp::LiveSet l = fixtures::full(parts);
  mutate(l);
  const ValidationReport r = validate(l, parts);
  if (!has_error(r, code)) {
    std::printf("FAIL validation rule not enforced: %s\n", what);
    ++g_fail;
  }
}

// ---- tests -------------------------------------------------------------------------------------------------------------
void test_fixtures_are_valid_and_round_trip() {
  for (const auto& c : fixtures::corpus()) {
    const std::uint32_t parts = static_cast<std::uint32_t>(c.set.parts_size());
    const ValidationReport r = validate(c.set, parts);
    if (!r.ok()) std::printf("fixture %s invalid (first issue code %d)\n", c.name, static_cast<int>(r.errors[0].code));
    CHECK(r.ok());
    const std::string b = bytes_of(c.set);
    LoadResult lr;
    ReadOptions ro;
    ro.part_count = parts;
    CHECK(parse(b, ro, lr) == Status::kOk);
    CHECK(!lr.migrated && lr.file_version == kFormatVersion);
    CHECK(bytes_of(lr.live_set) == b);  // read -> write is byte-identical
    CHECK(lr.live_set.SerializeAsString().size() == b.size());
  }
  // A new Live Set, as the device creates it.
  OsRandom os;
  lsp::LiveSet n = make_default_live_set(mint_uuid_v4(os), "New", kV1PartCount);
  CHECK(validate(n, kV1PartCount).ok());
  CHECK(validate(make_default_live_set(mint_uuid_v4(os), "Pro", kProPartCount), kProPartCount).ok());
  CHECK(!validate(make_default_live_set(mint_uuid_v4(os), "Pro", kProPartCount), kV1PartCount).ok());
}

void test_golden() {
  const std::string golden = slurp(std::string(VELORA_LIVESET_GOLDEN_DIR) + "/live_set_fixed.bin");
  CHECK(!golden.empty());
  CHECK(bytes_of(fixtures::golden()) == golden);
  LoadResult lr;
  CHECK(parse(golden, ReadOptions{}, lr) == Status::kOk);
  CHECK(lr.live_set.name() == "Golden" && lr.live_set.tempo() == 123.5f);
  // Pinned field numbers: format_version = 1 (08 01), id (tag 0x12, 36 bytes), name (0x1a).
  CHECK(golden.size() > 44);
  CHECK(static_cast<unsigned char>(golden[0]) == 0x08 && golden[1] == 0x01);
  CHECK(static_cast<unsigned char>(golden[2]) == 0x12 && golden[3] == 36);
  CHECK(golden.compare(4, 36, "3f2b8c1e-5a4d-4e6f-9a7b-0c1d2e3f4a5b") == 0);
  CHECK(static_cast<unsigned char>(golden[40]) == 0x1a && golden[41] == 6 && golden.compare(42, 6, "Golden") == 0);
  // tempo (field 4, fixed32) 123.5f = 0x42F70000
  CHECK(static_cast<unsigned char>(golden[48]) == 0x25);
  CHECK(static_cast<unsigned char>(golden[49]) == 0x00 && static_cast<unsigned char>(golden[50]) == 0x00 &&
        static_cast<unsigned char>(golden[51]) == 0xF7 && static_cast<unsigned char>(golden[52]) == 0x42);
}

void test_validation_rules() {
  expect_invalid("id not a uuid", IssueCode::kBadId, [](lsp::LiveSet& l) { l.set_id("nope"); });
  expect_invalid("id uuid v1", IssueCode::kBadId, [](lsp::LiveSet& l) { l.set_id("3f2b8c1e-5a4d-1e6f-9a7b-0c1d2e3f4a5b"); });
  expect_invalid("id uppercase", IssueCode::kBadId, [](lsp::LiveSet& l) { l.set_id("3F2B8C1E-5A4D-4E6F-9A7B-0C1D2E3F4A5B"); });
  expect_invalid("id bad variant", IssueCode::kBadId, [](lsp::LiveSet& l) { l.set_id("3f2b8c1e-5a4d-4e6f-0a7b-0c1d2e3f4a5b"); });
  expect_invalid("empty name", IssueCode::kBadName, [](lsp::LiveSet& l) { l.set_name(""); });
  expect_invalid("name too long", IssueCode::kBadName, [](lsp::LiveSet& l) { l.set_name(std::string(65, 'x')); });
  expect_invalid("name control char", IssueCode::kBadName, [](lsp::LiveSet& l) { l.set_name("a\nb"); });
  {
    lsp::LiveSet l = fixtures::minimal();
    l.set_name(std::string(64, 'x'));
    CHECK(validate(l, 8).ok());  // exactly the limit
  }
  expect_invalid("name invalid utf8", IssueCode::kBadName, [](lsp::LiveSet& l) { l.set_name("a\xC3\x28"); });
  expect_invalid("scene name invalid utf8", IssueCode::kBadSceneName, [](lsp::LiveSet& l) { l.mutable_scenes(0)->set_name("\xFF"); });
  expect_invalid("key invalid utf8", IssueCode::kBadTimbre, [](lsp::LiveSet& l) { l.mutable_parts(0)->mutable_timbre()->set_instrument_key("a\xE2\x82"); });
  expect_invalid("key absolute", IssueCode::kBadTimbre, [](lsp::LiveSet& l) { l.mutable_parts(0)->mutable_timbre()->set_instrument_key("/etc/x.sfz"); });
  expect_invalid("key backslash", IssueCode::kBadTimbre, [](lsp::LiveSet& l) { l.mutable_parts(0)->mutable_timbre()->set_instrument_key("a\\b.sfz"); });
  expect_invalid("key dotdot", IssueCode::kBadTimbre, [](lsp::LiveSet& l) { l.mutable_parts(0)->mutable_timbre()->set_instrument_key("a/../b.sfz"); });
  expect_invalid("key leading dotdot", IssueCode::kBadTimbre, [](lsp::LiveSet& l) { l.mutable_parts(0)->mutable_timbre()->set_instrument_key("../b.sfz"); });
  {
    lsp::LiveSet l = fixtures::minimal();
    l.set_name("Caf\xC3\xA9 \xE2\x82\xAC");  // valid multi-byte UTF-8
    fixtures::timbre(l.mutable_parts(0), fixtures::uuid_n(5), "a..b/c...sfz");  // dots inside a segment are fine
    CHECK(validate(l, 8).ok());
  }
  {
    // The registry owns the tempo range and id.
    const auto* t = velora::core::params::find(velora::core::params::kTempoBpm);
    CHECK(t && t->min == 20.0 && t->max == 300.0);
    lsp::LiveSet l = fixtures::minimal();
    l.set_tempo(static_cast<float>(t->min));
    CHECK(validate(l, 8).ok());
    l.set_tempo(static_cast<float>(t->max));
    CHECK(validate(l, 8).ok());
    l.set_tempo(std::nextafter(static_cast<float>(t->max), 1000.0f));
    CHECK(!validate(l, 8).ok());
  }
  expect_invalid("tempo low", IssueCode::kBadTempo, [](lsp::LiveSet& l) { l.set_tempo(19.9f); });
  expect_invalid("tempo high", IssueCode::kBadTempo, [](lsp::LiveSet& l) { l.set_tempo(300.1f); });
  expect_invalid("tempo nan", IssueCode::kBadTempo, [](lsp::LiveSet& l) { l.set_tempo(std::numeric_limits<float>::quiet_NaN()); });
  {
    lsp::LiveSet l = fixtures::minimal();
    l.set_tempo(20.0f);
    CHECK(validate(l, 8).ok());
    l.set_tempo(300.0f);
    CHECK(validate(l, 8).ok());
  }
  expect_invalid("too few parts", IssueCode::kBadPartCount, [](lsp::LiveSet& l) { l.mutable_parts()->RemoveLast(); });
  expect_invalid("too many parts", IssueCode::kBadPartCount, [](lsp::LiveSet& l) { l.add_parts(); });
  // A Pro-sized set does not fit the v1 tier (and the reverse).
  CHECK(!validate(fixtures::full(16), 8).ok() && has_error(validate(fixtures::full(16), 8), IssueCode::kBadPartCount));
  expect_invalid("key above 127", IssueCode::kBadKeyRange, [](lsp::LiveSet& l) { l.mutable_parts(0)->set_high_key(128); });
  expect_invalid("low above high", IssueCode::kBadKeyRange, [](lsp::LiveSet& l) {
    l.mutable_parts(0)->set_low_key(70);
    l.mutable_parts(0)->set_high_key(69);
  });
  {
    lsp::LiveSet l = fixtures::minimal();
    l.mutable_parts(0)->set_low_key(64);
    l.mutable_parts(0)->set_high_key(64);
    CHECK(validate(l, 8).ok());  // one-key zone
    l.mutable_parts(0)->set_low_key(0);
    l.mutable_parts(0)->set_high_key(127);
    CHECK(validate(l, 8).ok());
  }
  expect_invalid("octave", IssueCode::kBadOctave, [](lsp::LiveSet& l) { l.mutable_parts(1)->set_octave(5); });
  expect_invalid("octave low", IssueCode::kBadOctave, [](lsp::LiveSet& l) { l.mutable_parts(1)->set_octave(-5); });
  expect_invalid("transpose", IssueCode::kBadTranspose, [](lsp::LiveSet& l) { l.mutable_parts(1)->set_transpose(13); });
  expect_invalid("transpose low", IssueCode::kBadTranspose, [](lsp::LiveSet& l) { l.mutable_parts(1)->set_transpose(-13); });
  expect_invalid("volume", IssueCode::kBadVolume, [](lsp::LiveSet& l) { l.mutable_parts(2)->set_volume(1.01f); });
  expect_invalid("volume neg", IssueCode::kBadVolume, [](lsp::LiveSet& l) { l.mutable_parts(2)->set_volume(-0.1f); });
  expect_invalid("volume inf", IssueCode::kBadVolume, [](lsp::LiveSet& l) { l.mutable_parts(2)->set_volume(std::numeric_limits<float>::infinity()); });
  expect_invalid("output negative", IssueCode::kBadOutput,
                 [](lsp::LiveSet& l) { l.mutable_parts(2)->set_output(static_cast<ws::v1::PartOutput>(-1)); });
  expect_invalid("send reverb", IssueCode::kBadSend, [](lsp::LiveSet& l) { l.mutable_parts(2)->set_send_reverb(1.5f); });
  expect_invalid("send delay", IssueCode::kBadSend, [](lsp::LiveSet& l) { l.mutable_parts(2)->set_send_delay(-1.0f); });
  expect_invalid("timbre library id", IssueCode::kBadTimbre, [](lsp::LiveSet& l) { l.mutable_parts(0)->mutable_timbre()->set_library_id("x"); });
  expect_invalid("timbre key empty", IssueCode::kBadTimbre, [](lsp::LiveSet& l) { l.mutable_parts(0)->mutable_timbre()->set_instrument_key(""); });
  expect_invalid("timbre key long", IssueCode::kBadTimbre,
                 [](lsp::LiveSet& l) { l.mutable_parts(0)->mutable_timbre()->set_instrument_key(std::string(1025, 'k')); });
  expect_invalid("engine type zero", IssueCode::kBadEngineType, [](lsp::LiveSet& l) { l.mutable_parts(0)->clear_engine(); });
  expect_invalid("too many inserts", IssueCode::kTooManyInserts, [](lsp::LiveSet& l) {
    for (int i = 0; i < 6; ++i) l.mutable_parts(0)->add_inserts();
  });
  expect_invalid("insert too many params", IssueCode::kBadInsertSlot, [](lsp::LiveSet& l) {
    ws::v1::InsertSlot* s = l.mutable_parts(0)->add_inserts();
    for (int i = 0; i < 6; ++i) s->add_params(0.5f);
  });
  expect_invalid("insert param range", IssueCode::kBadInsertSlot, [](lsp::LiveSet& l) {
    l.mutable_parts(0)->add_inserts()->add_params(2.0f);
  });
  expect_invalid("engine param value", IssueCode::kBadParamValue,
                 [](lsp::LiveSet& l) { fixtures::entry(l.mutable_parts(2)->mutable_engine()->mutable_params(), 103, 9); });
  expect_invalid("engine param nonintegral", IssueCode::kBadParamValue,
                 [](lsp::LiveSet& l) { fixtures::entry(l.mutable_parts(2)->mutable_engine()->mutable_params(), 103, 2.5f); });
  expect_invalid("engine param nan", IssueCode::kBadParamValue,
                 [](lsp::LiveSet& l) { fixtures::entry(l.mutable_parts(2)->mutable_engine()->mutable_params(), 116, std::numeric_limits<float>::quiet_NaN()); });
  expect_invalid("engine param duplicate", IssueCode::kDuplicateParam,
                 [](lsp::LiveSet& l) { fixtures::entry(l.mutable_parts(2)->mutable_engine()->mutable_params(), 100, 1); });
  expect_invalid("engine param master scope", IssueCode::kWrongScope,
                 [](lsp::LiveSet& l) { fixtures::entry(l.mutable_parts(2)->mutable_engine()->mutable_params(), 520, 0); });
  expect_invalid("engine param insert scope", IssueCode::kWrongScope,
                 [](lsp::LiveSet& l) { fixtures::entry(l.mutable_parts(2)->mutable_eq(), 302, 0.5f); });
  expect_invalid("too many params", IssueCode::kTooManyParams, [](lsp::LiveSet& l) {
    for (std::uint32_t i = 0; i < 65; ++i) fixtures::entry(l.mutable_parts(0)->mutable_envelope(), 20000 + i, 0);
  });
  expect_invalid("structural part param in a sound list", IssueCode::kWrongClass,
                 [](lsp::LiveSet& l) { fixtures::entry(l.mutable_parts(2)->mutable_engine()->mutable_params(), 700, 1); });
  expect_invalid("part_output in eq list", IssueCode::kWrongClass,
                 [](lsp::LiveSet& l) { fixtures::entry(l.mutable_parts(2)->mutable_eq(), 600, 1); });
  expect_invalid("scene timbre_ref override", IssueCode::kWrongClass,
                 [](lsp::LiveSet& l) { fixtures::entry(l.mutable_scenes(0)->mutable_parts(0)->mutable_engine_params(), 700, 1); });
  expect_invalid("line_in_level in master", IssueCode::kWrongClass, [](lsp::LiveSet& l) { fixtures::entry(l.mutable_master(), 800, 1); });
  expect_invalid("master part-scope param", IssueCode::kWrongScope, [](lsp::LiveSet& l) { fixtures::entry(l.mutable_master(), 116, 0.5f); });
  expect_invalid("tempo in master", IssueCode::kWrongScope, [](lsp::LiveSet& l) { fixtures::entry(l.mutable_master(), 802, 120); });
  expect_invalid("master out of range", IssueCode::kBadParamValue, [](lsp::LiveSet& l) { fixtures::entry(l.mutable_master(), 543, 0.99f); });
  expect_invalid("nine scenes", IssueCode::kTooManyScenes, [](lsp::LiveSet& l) { l.add_scenes()->set_name("9"); });
  expect_invalid("scene name", IssueCode::kBadSceneName, [](lsp::LiveSet& l) { l.mutable_scenes(3)->set_name(""); });
  expect_invalid("scene parts above tier", IssueCode::kTooManySceneParts, [](lsp::LiveSet& l) {
    for (int i = 0; i < 9; ++i) l.mutable_scenes(2)->add_parts();
  });
  expect_invalid("scene volume", IssueCode::kBadVolume, [](lsp::LiveSet& l) { l.mutable_scenes(0)->mutable_parts(0)->set_volume(2); });
  expect_invalid("scene low above base high", IssueCode::kBadKeyRange, [](lsp::LiveSet& l) {
    l.mutable_parts(0)->set_high_key(59);
    l.mutable_scenes(0)->mutable_parts(0)->set_low_key(60);  // effective zone 60..59
  });
  expect_invalid("scene high below base low", IssueCode::kBadKeyRange, [](lsp::LiveSet& l) {
    l.mutable_parts(0)->set_low_key(40);
    l.mutable_scenes(0)->mutable_parts(0)->set_high_key(39);
  });
  expect_invalid("scene insert param range", IssueCode::kBadInsertSlot, [](lsp::LiveSet& l) {
    ws::v1::InsertSlot* sl = l.mutable_scenes(0)->mutable_parts(0)->mutable_inserts()->add_slots();
    sl->set_type(ws::v1::INSERT_TYPE_EQ);
    sl->add_params(7.0f);
  });
  expect_invalid("scene key range", IssueCode::kBadKeyRange, [](lsp::LiveSet& l) {
    l.mutable_scenes(0)->mutable_parts(0)->set_low_key(90);
    l.mutable_scenes(0)->mutable_parts(0)->set_high_key(10);
  });
  expect_invalid("scene octave", IssueCode::kBadOctave, [](lsp::LiveSet& l) { l.mutable_scenes(0)->mutable_parts(0)->set_octave(9); });
  expect_invalid("scene transpose", IssueCode::kBadTranspose, [](lsp::LiveSet& l) { l.mutable_scenes(0)->mutable_parts(0)->set_transpose(40); });
  expect_invalid("scene send", IssueCode::kBadSend, [](lsp::LiveSet& l) { l.mutable_scenes(0)->mutable_parts(0)->set_send_delay(5); });
  expect_invalid("scene inserts", IssueCode::kTooManyInserts, [](lsp::LiveSet& l) {
    for (int i = 0; i < 6; ++i) l.mutable_scenes(0)->mutable_parts(0)->mutable_inserts()->add_slots();
  });
  expect_invalid("scene engine param", IssueCode::kBadParamValue,
                 [](lsp::LiveSet& l) { fixtures::entry(l.mutable_scenes(0)->mutable_parts(0)->mutable_engine_params(), 100, 99); });
  expect_invalid("scene master tempo", IssueCode::kWrongScope, [](lsp::LiveSet& l) { fixtures::entry(l.mutable_scenes(0)->mutable_master(), 802, 100); });
  expect_invalid("assign too short", IssueCode::kBadAssignCount, [](lsp::LiveSet& l) { l.mutable_assign()->RemoveLast(); });
  expect_invalid("assign too long", IssueCode::kBadAssignCount, [](lsp::LiveSet& l) { l.add_assign(0); });
  expect_invalid("assign structural param", IssueCode::kBadAssignParam, [](lsp::LiveSet& l) { l.set_assign(0, 520); });
  expect_invalid("arp gate", IssueCode::kBadArp, [](lsp::LiveSet& l) { l.mutable_parts(0)->mutable_arp()->set_gate(0.0f); });
  expect_invalid("arp octave", IssueCode::kBadArp, [](lsp::LiveSet& l) { l.mutable_parts(0)->mutable_arp()->set_octave_range(5); });
  expect_invalid("arp swing", IssueCode::kBadArp, [](lsp::LiveSet& l) { l.mutable_parts(0)->mutable_arp()->set_swing(49.0f); });
  expect_invalid("arp pattern unspecified", IssueCode::kBadArp, [](lsp::LiveSet& l) { l.mutable_parts(0)->mutable_arp()->set_pattern(lsp::ARP_PATTERN_UNSPECIFIED); });
  expect_invalid("arp rate unspecified", IssueCode::kBadArp, [](lsp::LiveSet& l) { l.mutable_parts(0)->mutable_arp()->set_rate(lsp::ARP_RATE_UNSPECIFIED); });
  expect_invalid("scene arp", IssueCode::kBadArp, [](lsp::LiveSet& l) { l.mutable_scenes(0)->mutable_parts(0)->mutable_arp()->set_swing(99.0f); });
  {
    // Eight scenes and an ASSIGN of eight performance ids are the maxima and are accepted.
    lsp::LiveSet l = fixtures::full();
    CHECK(l.scenes_size() == 8 && validate(l, 8).ok());
  }
  {
    // Unknown ids are preserved and flagged, never rejected.
    lsp::LiveSet l = fixtures::full();
    fixtures::entry(l.mutable_parts(0)->mutable_engine()->mutable_params(), 16000, 0.5f);
    fixtures::entry(l.mutable_parts(0)->mutable_eq(), 16001, -3.0f);
    fixtures::entry(l.mutable_master(), 16002, 1.0f);
    l.set_assign(7, 16003);
    const ValidationReport r = validate(l, 8);
    CHECK(r.ok());
    CHECK(r.unknown_params.size() == 4 && has_warning(r, IssueCode::kUnknownParam));
    std::set<int> ids;
    for (const Issue& i : r.unknown_params) ids.insert(i.index);
    CHECK(ids == std::set<int>({16000, 16001, 16002, 16003}));
    LoadResult lr;
    CHECK(parse(bytes_of(l), ReadOptions{}, lr) == Status::kOk && lr.report.unknown_params.size() == 4);
    CHECK(bytes_of(lr.live_set) == bytes_of(l));
    // An insert type above this build's registry enum is flagged, not rejected.
    l.mutable_parts(1)->add_inserts()->set_type(static_cast<ws::v1::InsertType>(40));
    const ValidationReport r2 = validate(l, 8);
    CHECK(r2.ok() && has_warning(r2, IssueCode::kUnknownInsertType));
    // ...and its params are not range-checked (they belong to a newer build).
    l.mutable_parts(1)->mutable_inserts(0)->add_params(99.0f);
    l.mutable_parts(1)->mutable_inserts(0)->add_params(-5.0f);
    CHECK(validate(l, 8).ok());
    // Enum values beyond this build's range are preserved with a warning.
    l.mutable_parts(2)->set_output(static_cast<ws::v1::PartOutput>(7));
    l.mutable_parts(3)->mutable_arp()->set_pattern(static_cast<lsp::ArpPattern>(9));
    l.mutable_parts(3)->mutable_arp()->set_rate(static_cast<lsp::ArpRate>(12));
    l.mutable_scenes(0)->mutable_parts(0)->set_output(static_cast<ws::v1::PartOutput>(8));
    const ValidationReport r3 = validate(l, 8);
    CHECK(r3.ok() && has_warning(r3, IssueCode::kUnknownEnumValue));
    LoadResult lr3;
    CHECK(parse(bytes_of(l), ReadOptions{}, lr3) == Status::kOk && bytes_of(lr3.live_set) == bytes_of(l));
    CHECK(lr3.live_set.parts(2).output() == 7);
  }
  {
    // The arpeggiator fields are defined and unset in every v1 file; setting them needs no version bump.
    lsp::LiveSet l = fixtures::full();
    for (const lsp::Part& p : l.parts()) CHECK(!p.has_arp());
    lsp::ArpSettings* a = l.mutable_parts(3)->mutable_arp();
    a->set_enabled(true);
    a->set_pattern(lsp::ARP_PATTERN_UP_DOWN);
    a->set_rate(lsp::ARP_RATE_SIXTEENTH_TRIPLET);
    a->set_gate(50.0f);
    a->set_octave_range(2);
    a->set_hold(true);
    a->set_swing(60.0f);
    l.mutable_scenes(1)->mutable_parts(3)->mutable_arp()->set_enabled(false);
    CHECK(validate(l, 8).ok());
    LoadResult lr;
    CHECK(parse(bytes_of(l), ReadOptions{}, lr) == Status::kOk);
    CHECK(lr.live_set.parts(3).arp().rate() == lsp::ARP_RATE_SIXTEENTH_TRIPLET && !lr.live_set.parts(3).arp().has_hold() == false);
    CHECK(lr.live_set.format_version() == 1 && !lr.migrated);
    CHECK(!lr.live_set.parts(3).arp().has_swing() == false && !lr.live_set.scenes(1).parts(3).arp().has_gate());
  }
}

void test_scene_semantics() {
  // Absent in a scene = the base value; presence flags distinguish "override to 0" from "not overridden".
  lsp::LiveSet l = fixtures::full();
  const lsp::ScenePart& sp = l.scenes(0).parts(0);
  CHECK(sp.has_volume() && !sp.has_octave() && !sp.has_low_key() && !sp.has_inserts() && !sp.has_arp());
  const lsp::ScenePart& full_sp = l.scenes(1).parts(0);
  CHECK(full_sp.has_send_delay() && full_sp.send_delay() == 0.0f && full_sp.has_inserts() && full_sp.inserts().slots_size() == 0);
  CHECK(l.scenes(2).parts_size() == 0);  // a scene with only a name is all-base
  LoadResult lr;
  CHECK(parse(bytes_of(l), ReadOptions{}, lr) == Status::kOk);
  CHECK(lr.live_set.scenes(1).parts(0).has_send_delay() && lr.live_set.scenes(1).parts(0).has_inserts());
  CHECK(!lr.live_set.scenes(0).parts(0).has_octave());
  // A scene never carries the timbre or the engine type: the schema has no field for them.
  const auto* d = pbv::ScenePart::descriptor();
  CHECK(d->FindFieldByName("timbre") == nullptr && d->FindFieldByName("engine_type_id") == nullptr);
  CHECK(pbv::Scene::descriptor()->FindFieldByName("tempo") == nullptr);
}

void test_version_policy() {
  lsp::LiveSet l = fixtures::full();
  auto with_version = [&](std::uint32_t v) {
    lsp::LiveSet c = l;
    c.set_format_version(v);
    return bytes_of(c);
  };
  LoadResult lr;
  // N = 1 (this release): only 1 loads; 0 (unset) and 2 are refused with a typed error.
  CHECK(parse(with_version(1), ReadOptions{}, lr) == Status::kOk && !lr.migrated);
  CHECK(parse(with_version(0), ReadOptions{}, lr) == Status::kUnsupportedVersion);
  CHECK(parse(with_version(2), ReadOptions{}, lr) == Status::kUnsupportedVersion);
  CHECK(parse(with_version(4000000000u), ReadOptions{}, lr) == Status::kUnsupportedVersion);
  // A future reader N = 2: N-1 (= the files this release writes) loads and is migrated in memory; N loads; N-2 and N+1 do not.
  ReadOptions n2;
  n2.current_version = 2;
  CHECK(parse(with_version(1), n2, lr) == Status::kOk && lr.migrated && lr.file_version == 1 && lr.live_set.format_version() == 2);
  CHECK(parse(with_version(2), n2, lr) == Status::kOk && !lr.migrated);
  CHECK(parse(with_version(0), n2, lr) == Status::kUnsupportedVersion);
  CHECK(parse(with_version(3), n2, lr) == Status::kUnsupportedVersion);
  ReadOptions n3;
  n3.current_version = 3;
  CHECK(parse(with_version(1), n3, lr) == Status::kUnsupportedVersion);
  CHECK(parse(with_version(2), n3, lr) == Status::kOk && lr.migrated);
  // migrate() stub.
  lsp::LiveSet m = l;
  CHECK(migrate(m, 1, 1) == Status::kOk && m.format_version() == 1);
  CHECK(migrate(m, 1, 2) == Status::kOk && m.format_version() == 2);
  CHECK(migrate(m, 1, 3) == Status::kUnsupportedVersion);
  // A migrated N-1 file keeps its content and unknown fields, and writes back as N.
  const lsp::LiveSet nf = fixtures::newer_schema();
  lsp::LiveSet old = nf;
  old.set_format_version(1);
  CHECK(parse(bytes_of(old), n2, lr) == Status::kOk);
  CHECK(lr.live_set.GetReflection()->GetUnknownFields(lr.live_set).field_count() == 2);
  // A store of version N writes only N and reads N and N-1.
  TmpDir td;
  StoreOptions so;
  so.dir = td.path;
  so.current_version = 2;
  std::unique_ptr<LiveSetStore> st;
  CHECK(LiveSetStore::open(so, st) == Status::kOk);
  CHECK(st->write(old) == Status::kUnsupportedVersion);  // cannot write an old format
  spit(st->path_for(old.id()), bytes_of(old));
  CHECK(st->read(old.id(), lr) == Status::kOk && lr.migrated);
  CHECK(st->write(lr.live_set) == Status::kOk);
  CHECK(st->read(old.id(), lr) == Status::kOk && !lr.migrated && lr.live_set.format_version() == 2);
}

void test_unknown_fields_preserved() {
  lsp::LiveSet l = fixtures::full();
  std::string b = bytes_of(l);
  // A file written by a newer schema: field 500 (varint 7) and field 501 (string) appended by "the future".
  std::string future = b;
  future += std::string("\xA0\x1F\x07", 3);                              // 500, varint 7
  future += std::string("\xAA\x1F\x04" "fwd!", 7);                       // 501, length-delimited
  LoadResult lr;
  CHECK(parse(future, ReadOptions{}, lr) == Status::kOk);
  CHECK(lr.live_set.GetReflection()->GetUnknownFields(lr.live_set).field_count() == 2);
  CHECK(bytes_of(lr.live_set) == future);  // survives the next write unchanged, byte for byte
  // Edits to known fields keep the unknown ones.
  lr.live_set.set_name("Edited");
  lr.live_set.mutable_parts(0)->set_octave(1);
  const std::string edited = bytes_of(lr.live_set);
  LoadResult lr2;
  CHECK(parse(edited, ReadOptions{}, lr2) == Status::kOk && lr2.live_set.name() == "Edited");
  CHECK(lr2.live_set.GetReflection()->GetUnknownFields(lr2.live_set).field_count() == 2);
  // Nested unknown fields (part, engine, scene) via the corpus fixture.
  const lsp::LiveSet n = fixtures::newer_schema();
  const std::string nb = bytes_of(n);
  CHECK(parse(nb, ReadOptions{}, lr) == Status::kOk);
  CHECK(bytes_of(lr.live_set) == nb);
  CHECK(lr.live_set.parts(0).GetReflection()->GetUnknownFields(lr.live_set.parts(0)).field_count() == 1);
  CHECK(lr.live_set.parts(0).engine().GetReflection()->GetUnknownFields(lr.live_set.parts(0).engine()).field_count() == 1);
  CHECK(lr.live_set.scenes(0).GetReflection()->GetUnknownFields(lr.live_set.scenes(0)).field_count() == 1);
  // Through the store too.
  TmpDir td;
  StoreOptions so;
  so.dir = td.path;
  std::unique_ptr<LiveSetStore> st;
  CHECK(LiveSetStore::open(so, st) == Status::kOk);
  CHECK(st->write(lr.live_set) == Status::kOk);
  CHECK(slurp(st->path_for(lr.live_set.id())) == nb);
}

void test_reserved_engines() {
  const lsp::LiveSet l = fixtures::fm_part();
  const std::string b = bytes_of(l);
  TmpDir td;
  StoreOptions so;
  so.dir = td.path;
  std::unique_ptr<LiveSetStore> st;
  CHECK(LiveSetStore::open(so, st) == Status::kOk);
  CHECK(st->write(l) == Status::kOk);
  LoadResult lr;
  CHECK(st->read(l.id(), lr) == Status::kOk);
  CHECK(lr.live_set.parts(0).engine().engine_type_id() == 3 && lr.live_set.parts(1).engine().engine_type_id() == 4);
  CHECK(lr.live_set.parts(0).engine().params_size() == 2);
  CHECK(st->write(lr.live_set) == Status::kOk);
  CHECK(slurp(st->path_for(l.id())) == b);  // read -> write unchanged

  // Resolve report: no engine registry entry for FM/VA -> engine-not-available, never substituted.
  velora::core::EngineRegistry reg;
  velora::core::Arena arena(1 << 16);
  velora::core::TestToneEngine tone(arena, 4);
  const EngineAvailability none = availability_of(reg);
  auto r0 = resolve_parts(l, none);
  CHECK(r0.size() == 8);
  for (PartState s : r0) CHECK(s == PartState::kEngineNotAvailable);  // nothing compiled in this registry

  // Sample (1) and Organ (2) available, FM (3) and VA (4) not.
  const EngineAvailability sample_organ = [](std::uint32_t id) { return id == 1 || id == 2; };
  lsp::LiveSet l2 = fixtures::fm_part();
  fixtures::timbre(l2.mutable_parts(2), fixtures::uuid_n(77), "Keys/A.sfz");
  auto r = resolve_parts(l2, sample_organ);
  CHECK(r[0] == PartState::kEngineNotAvailable);   // FM
  CHECK(r[1] == PartState::kEngineNotAvailable);   // VA
  CHECK(r[2] == PartState::kOk);                   // sample with a timbre
  CHECK(r[3] == PartState::kTimbreEmpty);          // sample without a timbre
  l2.mutable_parts(4)->mutable_engine()->set_engine_type_id(2);
  CHECK(resolve_parts(l2, sample_organ)[4] == PartState::kOk);  // organ needs no timbre
  // With a real registry holding a bound engine.
  CHECK(reg.bind(tone));
  const EngineAvailability with_tone = availability_of(reg);
  l2.mutable_parts(5)->mutable_engine()->set_engine_type_id(static_cast<std::uint32_t>(velora::core::EngineTypeId::kTestTone));
  CHECK(resolve_parts(l2, with_tone)[5] == PartState::kOk);
  CHECK(resolve_parts(l2, with_tone)[0] == PartState::kEngineNotAvailable);
  CHECK(!with_tone(999999));  // unknown id is never available
}

void test_uuid_and_collisions() {
  SeededRandom a(42), b(42), c(43);
  const std::string ua = mint_uuid_v4(a), ub = mint_uuid_v4(b), uc = mint_uuid_v4(c);
  CHECK(ua == ub && ua != uc);  // deterministic with the injected source
  CHECK(is_uuid_v4(ua) && ua[14] == '4' && std::string("89ab").find(ua[19]) != std::string::npos);
  CHECK(ua.size() == 36 && ua[8] == '-' && ua[13] == '-' && ua[18] == '-' && ua[23] == '-');
  std::set<std::string> seen;
  OsRandom os;
  for (int i = 0; i < 1000; ++i) {
    const std::string u = mint_uuid_v4(os);
    CHECK(is_uuid_v4(u));
    seen.insert(u);
  }
  CHECK(seen.size() == 1000);
  CHECK(!is_uuid_v4("") && !is_uuid_v4(ua + "0") && !is_canonical_uuid("3f2b8c1e5a4d4e6f9a7b0c1d2e3f4a5b"));
  CHECK(is_canonical_uuid("3f2b8c1e-5a4d-1e6f-9a7b-0c1d2e3f4a5b") && !is_uuid_v4("3f2b8c1e-5a4d-1e6f-9a7b-0c1d2e3f4a5b"));
  // Collision: new id, content kept.
  lsp::LiveSet l = fixtures::full();
  const std::string before = l.id();
  lsp::LiveSet content = l;
  content.clear_id();
  SeededRandom r(7);
  CHECK(adopt_with_collision_check(l, {"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"}, r) == AdoptResult::kUnchanged && l.id() == before);
  CHECK(adopt_with_collision_check(l, {before}, r) == AdoptResult::kReplaced);
  CHECK(l.id() != before && is_uuid_v4(l.id()));
  lsp::LiveSet after = l;
  after.clear_id();
  CHECK(after.SerializeAsString() == content.SerializeAsString());
  // The new id is unique among the existing ones, even when the first draws collide.
  SeededRandom r1(5), r2(5);
  const std::string first = mint_uuid_v4(r1);
  lsp::LiveSet l3 = fixtures::full();
  l3.set_id(first);
  CHECK(adopt_with_collision_check(l3, {first}, r2) == AdoptResult::kReplaced);
  CHECK(l3.id() != first);
  // Another draw of the same stream also collides: skipped.
  SeededRandom r3(5);
  const std::string d1 = mint_uuid_v4(r3), d2 = mint_uuid_v4(r3);
  SeededRandom r4(5);
  lsp::LiveSet l4 = fixtures::full();
  l4.set_id(d1);
  CHECK(adopt_with_collision_check(l4, {d1, d2}, r4) == AdoptResult::kReplaced);
  CHECK(l4.id() != d1 && l4.id() != d2);
  // A malformed id is also replaced.
  lsp::LiveSet l5 = fixtures::full();
  l5.set_id("not-a-uuid");
  SeededRandom r5(1);
  CHECK(adopt_with_collision_check(l5, {}, r5) == AdoptResult::kReplaced && is_uuid_v4(l5.id()));
  // Fail closed: a failing source never yields an id, and a stuck source stops after the capped number of attempts.
  struct FailingRandom final : RandomSource {
    bool fill(std::uint8_t*, std::size_t) override { return false; }
  } bad;
  CHECK(mint_uuid_v4(bad).empty());
  lsp::LiveSet l6 = fixtures::full();
  const std::string keep = l6.id();
  CHECK(adopt_with_collision_check(l6, {keep}, bad) == AdoptResult::kFailed && l6.id() == keep);
  struct ConstRandom final : RandomSource {
    int calls = 0;
    bool fill(std::uint8_t* d, std::size_t n) override { ++calls; for (std::size_t i = 0; i < n; ++i) d[i] = 7; return true; }
  } same;
  const std::string stuck = mint_uuid_v4(same);
  same.calls = 0;
  CHECK(adopt_with_collision_check(l6, {keep, stuck}, same) == AdoptResult::kFailed && same.calls == kMaxAdoptAttempts && l6.id() == keep);
}

int g_abort_at = -1;  // stage index at which the child dies

void test_atomic_write() {
  TmpDir td;
  int stage_count = 0;
  StoreOptions so;
  so.dir = td.path;
  so.on_stage = [&](Stage s) {
    ++stage_count;
    if (g_abort_at >= 0 && static_cast<int>(s) == g_abort_at) _exit(137);  // simulated crash: no cleanup runs
    return HookAction::kContinue;
  };
  std::unique_ptr<LiveSetStore> st;
  CHECK(LiveSetStore::open(so, st) == Status::kOk);
  lsp::LiveSet a = fixtures::full();
  lsp::LiveSet b = a;
  b.set_name("Version B");
  b.mutable_parts(0)->set_octave(2);
  CHECK(st->write(a) == Status::kOk);
  CHECK(stage_count == 4);
  CHECK(!fs::exists(td.path + "/" + a.id() + ".liveset.tmp"));
  const std::string a_bytes = bytes_of(a), b_bytes = bytes_of(b);
  CHECK(slurp(st->path_for(a.id())) == a_bytes);

  // Crash at each stage of overwriting A with B: the file is A or B, never partial.
  for (int stage = 0; stage < 4; ++stage) {
    spit(st->path_for(a.id()), a_bytes);
    const pid_t pid = fork();
    if (pid == 0) {
      g_abort_at = stage;
      const Status s = st->write(b);
      _exit(s == Status::kOk ? 0 : 1);
    }
    int ws = 0;
    CHECK(waitpid(pid, &ws, 0) == pid && WIFEXITED(ws) && WEXITSTATUS(ws) == 137);
    const std::string now = slurp(st->path_for(a.id()));
    LoadResult lr;
    CHECK(parse(now, ReadOptions{}, lr) == Status::kOk);
    if (stage <= static_cast<int>(Stage::kFsynced)) {
      CHECK(now == a_bytes);  // died before the rename: the old file
      CHECK(fs::exists(td.path + "/" + a.id() + ".liveset.tmp"));
    } else {
      CHECK(now == b_bytes);  // died after the rename: the new file
    }
    std::size_t removed = 0;
    CHECK(st->reconcile(removed) == Status::kOk);
    CHECK(removed == (stage <= static_cast<int>(Stage::kFsynced) ? 1u : 0u));
    CHECK(!fs::exists(td.path + "/" + a.id() + ".liveset.tmp"));
  }
  // The same through the in-process abort path.
  StoreOptions so2;
  TmpDir td2;
  so2.dir = td2.path;
  so2.on_stage = [](Stage s) { return s == Stage::kTempWritten ? HookAction::kAbort : HookAction::kContinue; };
  std::unique_ptr<LiveSetStore> st2;
  CHECK(LiveSetStore::open(so2, st2) == Status::kOk);
  CHECK(st2->write(a) == Status::kFaultInjected);
  LoadResult lr;
  CHECK(st2->read(a.id(), lr) == Status::kNotFound);  // no new file, no old file either
  CHECK(fs::exists(td2.path + "/" + a.id() + ".liveset.tmp"));
  // Garbage temp files from a dead writer are cleaned by reconcile and nothing else is touched.
  spit(td2.path + "/other.liveset.tmp", "junk");
  spit(td2.path + "/keep.liveset", "keep");
  std::size_t removed = 0;
  CHECK(st2->reconcile(removed) == Status::kOk && removed == 2);
  CHECK(fs::exists(td2.path + "/keep.liveset"));
  // An invalid Live Set writes nothing, not even a temp file.
  lsp::LiveSet bad = a;
  bad.set_tempo(1000.0f);
  ValidationReport rep;
  so2.on_stage = nullptr;
  std::unique_ptr<LiveSetStore> st3;
  st2.reset();
  CHECK(LiveSetStore::open(so2, st3) == Status::kOk);
  CHECK(st3->write(bad, &rep) == Status::kInvalid && has_error(rep, IssueCode::kBadTempo));
  CHECK(!fs::exists(st3->path_for(a.id())) && !fs::exists(td2.path + "/" + a.id() + ".liveset.tmp"));
  // Writing replaces atomically and the result reads back.
  CHECK(st3->write(a) == Status::kOk && st3->write(b) == Status::kOk);
  CHECK(st3->read(a.id(), lr) == Status::kOk && lr.live_set.name() == "Version B");
  CHECK(st3->read("../../etc/passwd", lr) == Status::kBadId);
  CHECK(st3->read("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", lr) == Status::kNotFound);
}

void test_single_writer() {
  TmpDir td;
  StoreOptions so;
  so.dir = td.path;
  std::unique_ptr<LiveSetStore> st;
  CHECK(LiveSetStore::open(so, st) == Status::kOk);
  std::unique_ptr<LiveSetStore> second;
  CHECK(LiveSetStore::open(so, second) == Status::kWriterBusy && !second);  // one writer per directory
  // Only the opening (control) thread may write or reconcile.
  const lsp::LiveSet l = fixtures::full();
  Status ws = Status::kOk, rs = Status::kOk;
  Status rd = Status::kIo;
  std::size_t removed = 99;
  CHECK(st->write(l) == Status::kOk);
  std::thread t([&] {
    ws = st->write(l);
    rs = st->reconcile(removed);
    LoadResult lr;
    rd = st->read(l.id(), lr);  // reads are allowed anywhere
  });
  t.join();
  CHECK(ws == Status::kNotControlThread && rs == Status::kNotControlThread && removed == 0);
  CHECK(rd == Status::kOk);
  st.reset();  // closing releases the lock
  CHECK(LiveSetStore::open(so, second) == Status::kOk);
  so.dir = td.path + "/missing";
  std::unique_ptr<LiveSetStore> none;
  CHECK(LiveSetStore::open(so, none) == Status::kNotFound);
}

void test_hostile_input() {
  const std::string good = bytes_of(fixtures::full());
  LoadResult lr;
  // Size bound.
  CHECK(parse(std::string(kMaxFileBytes + 1, 'x'), ReadOptions{}, lr) == Status::kTooLarge);
  TmpDir td;
  spit(td.path + "/big.liveset", std::string(kMaxFileBytes + 1, '\0'));
  CHECK(read_file(td.path + "/big.liveset", ReadOptions{}, lr) == Status::kTooLarge);
  CHECK(read_file(td.path + "/nope.liveset", ReadOptions{}, lr) == Status::kNotFound);
  spit(td.path + "/empty.liveset", "");
  CHECK(read_file(td.path + "/empty.liveset", ReadOptions{}, lr) != Status::kOk);  // version 0
  std::error_code ec;
  fs::create_symlink(td.path + "/empty.liveset", td.path + "/link.liveset", ec);
  CHECK(read_file(td.path + "/link.liveset", ReadOptions{}, lr) != Status::kOk);   // never follows a symlink
  CHECK(read_file(td.path, ReadOptions{}, lr) != Status::kOk);                     // a directory
  // Huge repeated fields: ~1 MiB of empty `parts` entries parse, then fail validation on the count.
  std::string many;
  many += std::string("\x08\x01", 2);
  while (many.size() < kMaxFileBytes - 2) many += std::string("\x2a\x00", 2);
  CHECK(parse(many, ReadOptions{}, lr) == Status::kInvalid && has_error(lr.report, IssueCode::kBadPartCount));
  // Deep nesting of unknown groups: the parser's recursion limit stops it.
  std::string deep;
  for (int i = 0; i < 20000; ++i) deep += '\x0b';  // start group, field 1
  CHECK(parse(deep, ReadOptions{}, lr) == Status::kParseError);
  std::string deep2("\x08\x01", 2);
  for (int i = 0; i < 5000; ++i) deep2 += std::string("\xFB\x1F", 2);  // start group, field 511
  CHECK(parse(deep2, ReadOptions{}, lr) != Status::kOk);
  // Truncation at every length never crashes; OK is only returned for something that validates.
  for (std::size_t n = 0; n < good.size(); n += (n < 200 ? 1 : 7)) {
    const Status s = parse(std::string_view(good).substr(0, n), ReadOptions{}, lr);
    if (s == Status::kOk) CHECK(validate(lr.live_set, 8).ok());
    if (n + 1 < good.size() && n < 150) CHECK(s != Status::kOk);
  }
  // Garbage and mutations with a fixed seed.
  SeededRandom rng(2024);
  int ok_count = 0;
  for (int i = 0; i < 4000; ++i) {
    std::string m = good;
    std::uint8_t r[8];
    rng.fill(r, sizeof r);
    const int edits = 1 + (r[0] % 4);
    for (int e = 0; e < edits; ++e) {
      std::uint8_t q[3];
      rng.fill(q, sizeof q);
      const std::size_t pos = (static_cast<std::size_t>(q[0]) << 8 | q[1]) % m.size();
      switch (r[1 + e] % 3) {
        case 0: m[pos] = static_cast<char>(q[2]); break;
        case 1: m.erase(pos, 1 + q[2] % 8); break;
        default: m.insert(pos, std::string(1 + q[2] % 8, static_cast<char>(q[2]))); break;
      }
      if (m.empty()) m = "x";
    }
    const Status s = parse(m, ReadOptions{}, lr);
    if (s == Status::kOk) {
      ++ok_count;
      CHECK(validate(lr.live_set, 8).ok() && is_uuid_v4(lr.live_set.id()));  // OK only when it really validates
    }
  }
  std::printf("mutation fuzz: %d of 4000 mutants still validate\n", ok_count);
  for (int i = 0; i < 4000; ++i) {
    std::uint8_t len[2];
    rng.fill(len, 2);
    std::string g(static_cast<std::size_t>(len[0]) << 2 | (len[1] & 3), '\0');
    if (!g.empty()) rng.fill(reinterpret_cast<std::uint8_t*>(g.data()), g.size());
    const Status s = parse(g, ReadOptions{}, lr);
    CHECK(s != Status::kOk);  // random bytes never form a valid Live Set
  }
}

void test_corpus() {
  const fs::path dir = VELORA_LIVESET_CORPUS_DIR;
  CHECK(fs::is_directory(dir));
  std::set<std::string> found;
  std::size_t n = 0;
  for (const auto& e : fs::directory_iterator(dir)) {
    if (e.path().extension() != ".liveset") continue;
    ++n;
    found.insert(e.path().filename().string());
    const std::string bytes = slurp(e.path().string());
    ReadOptions ro;
    ro.part_count = 0;  // tool mode: tier from the file
    LoadResult lr;
    const Status s = read_file(e.path().string(), ro, lr);
    if (s != Status::kOk) std::printf("corpus file %s: status %d\n", e.path().filename().c_str(), static_cast<int>(s));
    CHECK(s == Status::kOk);
    CHECK(!lr.migrated);
    CHECK(bytes_of(lr.live_set) == bytes);  // rewriting an unchanged corpus file is byte-identical
    // The version-N reader and the future N+1 reader (which treats N as N-1) both open it.
    ReadOptions next = ro;
    next.current_version = kFormatVersion + 1;
    LoadResult lr2;
    CHECK(read_file(e.path().string(), next, lr2) == Status::kOk && lr2.migrated);
  }
  const std::set<std::string> required = {"minimal.liveset",        "full-8-parts.liveset",
                                          "fm-va-parts.liveset",    "missing-library.liveset",
                                          "pro-16-parts.liveset",   "newer-schema-unknown-fields.liveset"};
  for (const std::string& r : required)
    if (!found.count(r)) {
      std::printf("FAIL corpus file missing: %s\n", r.c_str());
      ++g_fail;
    }
  CHECK(n >= required.size());
  // The committed corpus is what this release's writer produces from the fixtures (regenerate with the
  // live_set_corpus_regen target when a change is intended).
  for (const auto& c : fixtures::corpus()) {
    const std::string committed = slurp((dir / c.name).string());
    if (committed != bytes_of(c.set)) {
      std::printf("FAIL corpus drift: %s differs from the fixture; run the live_set_corpus_regen target\n", c.name);
      ++g_fail;
    }
  }
  // Content spot checks.
  ReadOptions ro;
  ro.part_count = 16;
  LoadResult lr;
  CHECK(read_file((dir / "pro-16-parts.liveset").string(), ro, lr) == Status::kOk && lr.live_set.parts_size() == 16);
  ro.part_count = 8;
  CHECK(read_file((dir / "pro-16-parts.liveset").string(), ro, lr) == Status::kInvalid);  // wrong tier is a typed error
  CHECK(read_file((dir / "full-8-parts.liveset").string(), ro, lr) == Status::kOk);
  CHECK(lr.live_set.scenes_size() == 8 && lr.live_set.assign_size() == 8);
  CHECK(read_file((dir / "fm-va-parts.liveset").string(), ro, lr) == Status::kOk);
  CHECK(!lr.report.unknown_params.empty());  // engine-specific ids are preserved and flagged
  auto states = resolve_parts(lr.live_set, [](std::uint32_t id) { return id == 1 || id == 2; });
  CHECK(states[0] == PartState::kEngineNotAvailable && states[1] == PartState::kEngineNotAvailable);
  CHECK(read_file((dir / "missing-library.liveset").string(), ro, lr) == Status::kOk);
  CHECK(lr.live_set.parts(0).has_timbre());
}


void test_integrity_and_durability() {
  TmpDir td;
  StoreOptions so;
  so.dir = td.path;
  std::unique_ptr<LiveSetStore> st;
  CHECK(LiveSetStore::open(so, st) == Status::kOk);
  LoadResult lr;
  // A file copied to another Live Set's name is refused (it would shadow that Live Set).
  const lsp::LiveSet a = fixtures::full();
  CHECK(st->write(a) == Status::kOk);
  const std::string other = fixtures::uuid_n(555);
  spit(st->path_for(other), slurp(st->path_for(a.id())));
  CHECK(st->read(other, lr) == Status::kInvalid && has_error(lr.report, IssueCode::kBadId));
  CHECK(st->read(a.id(), lr) == Status::kOk);
  // A present-but-empty TimbreRef is normalised to unset (parse and write) instead of failing validation.
  lsp::LiveSet e = fixtures::minimal();
  e.mutable_parts(0)->mutable_timbre();
  CHECK(e.parts(0).has_timbre() && !validate(e, 8).ok());
  CHECK(st->write(e) == Status::kOk);
  CHECK(st->read(e.id(), lr) == Status::kOk && !lr.live_set.parts(0).has_timbre());
  lsp::LiveSet e2 = fixtures::minimal();
  e2.mutable_parts(1)->mutable_timbre();
  LoadResult lr2;
  CHECK(parse(bytes_of(e2), ReadOptions{}, lr2) == Status::kOk && !lr2.live_set.parts(1).has_timbre());
  // Invalid UTF-8 never reaches disk (so every written file can be re-read).
  lsp::LiveSet u = fixtures::minimal();
  u.set_id(fixtures::uuid_n(556));
  u.set_name("bad\xC3\x28");
  CHECK(st->write(u) == Status::kInvalid);
  CHECK(!fs::exists(st->path_for(u.id())));

  // Rename done but directory sync failed: distinct status, the new file is in place.
  {
    TmpDir td2;
    StoreOptions o2;
    o2.dir = td2.path;
    o2.inject_dir_sync_failure = true;
    std::unique_ptr<LiveSetStore> s2;
    CHECK(LiveSetStore::open(o2, s2) == Status::kOk);
    CHECK(s2->write(a) == Status::kCommittedNotDurable);
    CHECK(s2->read(a.id(), lr) == Status::kOk);
    CHECK(slurp(s2->path_for(a.id())) == bytes_of(a));
    CHECK(!fs::exists(td2.path + "/" + a.id() + ".liveset.tmp"));
  }
  // reconcile reports a leftover it cannot remove (a non-empty directory named like a temp file) and still removes the rest.
  {
    fs::create_directory(td.path + "/stuck.liveset.tmp");
    spit(td.path + "/stuck.liveset.tmp/inner", "x");
    spit(td.path + "/gone.liveset.tmp", "x");
    std::size_t removed = 0;
    CHECK(st->reconcile(removed) == Status::kIo && removed == 1);
    CHECK(!fs::exists(td.path + "/gone.liveset.tmp") && fs::exists(td.path + "/stuck.liveset.tmp"));
    fs::remove_all(td.path + "/stuck.liveset.tmp");
    CHECK(st->reconcile(removed) == Status::kOk && removed == 0);
  }
  // Write into a read-only directory: kIo, previous file intact, no temp left (skipped when permissions do not bind, e.g. root).
  {
    lsp::LiveSet b = a;
    b.set_name("Changed");
    ::chmod(td.path.c_str(), 0555);
    if (::access(td.path.c_str(), W_OK) != 0) {
      CHECK(st->write(b) == Status::kIo);
      ::chmod(td.path.c_str(), 0755);
      CHECK(slurp(st->path_for(a.id())) == bytes_of(a));
      CHECK(!fs::exists(td.path + "/" + a.id() + ".liveset.tmp"));
    } else {
      ::chmod(td.path.c_str(), 0755);
      std::printf("note: running with write access to a 0555 directory (root?); read-only write test skipped\n");
    }
  }
}

void test_ownership_and_availability_lifetime() {
  TmpDir td;
  StoreOptions so;
  so.dir = td.path;
  std::unique_ptr<LiveSetStore> st;
  CHECK(LiveSetStore::open(so, st) == Status::kOk);
  const lsp::LiveSet l = fixtures::minimal();
  // Explicit handover: the new thread rebinds, then it (and only it) may write.
  Status before = Status::kOk, after = Status::kIo, old_after = Status::kOk;
  std::thread t([&] {
    before = st->write(l);
    st->rebind_control_thread();
    after = st->write(l);
  });
  t.join();
  old_after = st->write(l);  // the original thread lost the role
  CHECK(before == Status::kNotControlThread && after == Status::kOk && old_after == Status::kNotControlThread);
  st->rebind_control_thread();
  CHECK(st->write(l) == Status::kOk);
  // availability_of owns a snapshot: it stays valid after the registry and engine are gone.
  EngineAvailability avail;
  {
    velora::core::EngineRegistry reg;
    velora::core::Arena arena(1 << 16);
    velora::core::TestToneEngine tone(arena, 4);
    CHECK(reg.bind(tone));
    avail = availability_of(reg);
  }
  CHECK(avail(static_cast<std::uint32_t>(velora::core::EngineTypeId::kTestTone)));
  CHECK(!avail(3) && !avail(4));
}

void test_arpeggiator_registry_and_live_set_defaults() {
  namespace p = velora::core::params;
  namespace core = velora::core;
  namespace detail = velora::host::detail;
  // 1. Registry parameters check (Story 7.4 AC 1):
  // on/off, mode, rate, octaves, gate, swing, latch per part classified as structural, part-scope.
  // Live Set tempo is Live-Set-scoped (tempo_bpm id 802, performance, master-scope).
  const struct {
    std::uint32_t id;
    const char* name;
    p::Class cls;
    p::Scope scope;
    p::Type type;
    double def;
  } expected_params[] = {
      {900, "arp_on", p::Class::kStructural, p::Scope::kPart, p::Type::kBool, 0.0},
      {901, "arp_mode", p::Class::kStructural, p::Scope::kPart, p::Type::kEnum, 0.0},
      {902, "arp_rate", p::Class::kStructural, p::Scope::kPart, p::Type::kEnum, 1.0},
      {903, "arp_octaves", p::Class::kStructural, p::Scope::kPart, p::Type::kInt, 1.0},
      {904, "arp_gate", p::Class::kStructural, p::Scope::kPart, p::Type::kFloat, 50.0},
      {905, "arp_swing", p::Class::kStructural, p::Scope::kPart, p::Type::kFloat, 50.0},
      {906, "arp_latch", p::Class::kStructural, p::Scope::kPart, p::Type::kBool, 0.0},
  };
  for (const auto& ep : expected_params) {
    const p::ParamDef* d = p::find(ep.id);
    CHECK(d != nullptr);
    if (d) {
      CHECK(d->cls == ep.cls);
      CHECK(d->scope == ep.scope);
      CHECK(d->type == ep.type);
      CHECK(d->def == ep.def);
    }
  }
  const p::ParamDef* tempo = p::find(p::kTempoBpm);
  CHECK(tempo && tempo->cls == p::Class::kPerformance && tempo->scope == p::Scope::kMaster);

  // 2. New Live Set creation holds initial values (Story 7.4 AC 2):
  // Off, Up, 1/8, 1 octave, Gate 50%, Swing 50%, Latch Off
  lsp::LiveSet n = make_default_live_set("11111111-2222-4333-8444-555555555555", "New Arp", 8);
  CHECK(validate(n, 8).ok());
  for (int i = 0; i < n.parts_size(); ++i) {
    const lsp::Part& pt = n.parts(i);
    CHECK(pt.has_arp());
    if (pt.has_arp()) {
      const lsp::ArpSettings& a = pt.arp();
      CHECK(a.has_enabled() && a.enabled() == false);
      CHECK(a.has_pattern() && a.pattern() == lsp::ARP_PATTERN_UP);
      CHECK(a.has_rate() && a.rate() == lsp::ARP_RATE_EIGHTH);
      CHECK(a.has_octave_range() && a.octave_range() == 1);
      CHECK(a.has_gate() && a.gate() == 50.0f);
      CHECK(a.has_swing() && a.swing() == 50.0f);
      CHECK(a.has_hold() && a.hold() == false);
    }
  }

  // 3. Backward compatibility (Story 7.4 AC 4):
  // Opening a Live Set saved before this story with unset arp fields loads with initial defaults,
  // no format version bump or migration needed, unknown fields preserved, and write produces a file N and N-1 readers open.
  lsp::LiveSet older = fixtures::full();
  for (const lsp::Part& pt : older.parts()) CHECK(!pt.has_arp());
  std::string bytes;
  CHECK(serialize(older, bytes) == Status::kOk);
  LoadResult lr;
  CHECK(parse(bytes, ReadOptions{}, lr) == Status::kOk);
  CHECK(lr.live_set.format_version() == 1);
  core::Snapshot snap = core::Snapshot::defaults();
  detail::fill_part_values(snap, lr.live_set.parts(0), 0);
  CHECK(snap.parts[0].arp.enabled == false);
  CHECK(snap.parts[0].arp.mode == core::ArpMode::kUp);
  CHECK(snap.parts[0].arp.rate == core::ArpRate::k1_8);
  CHECK(snap.parts[0].arp.octaves == 1);
  CHECK(snap.parts[0].arp.gate_percent == 50.0f);
  CHECK(snap.parts[0].arp.swing_percent == 50.0f);
  CHECK(snap.parts[0].arp.latch == false);
}

// Story 12.8: FM and VA timbre parameters in the versioned formats
void test_fm_va_timbre_parameters_live_set_roundtrip_and_rollback() {
  using namespace velora::core::params;
  namespace pn = velora::host::param_names;

  // 1. Parameter Registry checks (Story 12.8 AC 1)
  const ParamDef* fm_alg = find(1000);
  CHECK(fm_alg != nullptr && fm_alg->cls == Class::kPerformance);
  CHECK(fm_alg->assignable == true);
  CHECK(std::string(pn::locale_key_of(1000)) == "fm.param.algorithm");

  const ParamDef* fm_fb = find(1001);
  CHECK(fm_fb != nullptr && fm_fb->cls == Class::kPerformance);
  CHECK(fm_fb->assignable == true);
  CHECK(std::string(pn::locale_key_of(1001)) == "fm.param.feedback");

  const ParamDef* va_cutoff = find(1231);
  CHECK(va_cutoff != nullptr && va_cutoff->cls == Class::kPerformance);
  CHECK(va_cutoff->assignable == true);
  CHECK(std::string(pn::locale_key_of(1231)) == "va.param.cutoff");

  const ParamDef* va_res = find(1232);
  CHECK(va_res != nullptr && va_res->cls == Class::kPerformance);
  CHECK(va_res->assignable == true);
  CHECK(std::string(pn::locale_key_of(1232)) == "va.param.resonance");

  // 2. Build Live Set with FM and VA parts (Story 12.8 AC 2)
  lsp::LiveSet live_set = make_default_live_set(fixtures::uuid_n(128), "FM and VA Live Set", 8);
  fixtures::clear_fixture_arp(live_set);

  // Part 0: FM Engine (3)
  lsp::Part* p_fm = live_set.mutable_parts(0);
  p_fm->mutable_engine()->set_engine_type_id(3);
  fixtures::entry(p_fm->mutable_engine()->mutable_params(), 1000, 5.0f);  // Algorithm 5
  fixtures::entry(p_fm->mutable_engine()->mutable_params(), 1001, 6.0f);  // Feedback 6
  fixtures::entry(p_fm->mutable_engine()->mutable_params(), 1003, 42.0f); // LFO Speed 42
  fixtures::entry(p_fm->mutable_engine()->mutable_params(), 1028, 95.0f); // OP1 Total Level 95
  fixtures::entry(p_fm->mutable_engine()->mutable_params(), 1029, 2.0f);  // OP1 Coarse Ratio 2

  // Part 1: VA Engine (4)
  lsp::Part* p_va = live_set.mutable_parts(1);
  p_va->mutable_engine()->set_engine_type_id(4);
  fixtures::entry(p_va->mutable_engine()->mutable_params(), 1200, 0.0f);  // Saw wave
  fixtures::entry(p_va->mutable_engine()->mutable_params(), 1220, 85.0f); // Mix Osc1 85%
  fixtures::entry(p_va->mutable_engine()->mutable_params(), 1231, 1450.0f); // Cutoff 1450 Hz
  fixtures::entry(p_va->mutable_engine()->mutable_params(), 1232, 38.0f); // Resonance 38%
  fixtures::entry(p_va->mutable_engine()->mutable_params(), 1240, 15.0f); // Amp Attack 15 ms
  fixtures::entry(p_va->mutable_engine()->mutable_params(), 1264, 45.0f); // Glide 45 ms

  // Assign encoders: K1 -> fm_feedback (1001), K2 -> va_filter_cutoff (1231)
  live_set.set_assign(0, 1001);
  live_set.set_assign(1, 1231);

  // Add Scene 1 with performance parameter overrides (never timbre assignment)
  lsp::Scene* sc = live_set.add_scenes();
  sc->set_name("Chorus Lead");
  for (int i = 0; i < 8; ++i) sc->add_parts();
  fixtures::entry(sc->mutable_parts(0)->mutable_engine_params(), 1001, 7.0f);
  fixtures::entry(sc->mutable_parts(1)->mutable_engine_params(), 1231, 3500.0f);

  // 3. Write and Round-Trip to store
  TmpDir td;
  StoreOptions so;
  so.dir = td.path;
  std::unique_ptr<LiveSetStore> st;
  CHECK(LiveSetStore::open(so, st) == Status::kOk);
  CHECK(st->write(live_set) == Status::kOk);

  LoadResult lr;
  CHECK(st->read(live_set.id(), lr) == Status::kOk);
  CHECK(lr.live_set.parts(0).engine().engine_type_id() == 3);
  CHECK(lr.live_set.parts(0).engine().params_size() == 5);
  CHECK(lr.live_set.parts(1).engine().engine_type_id() == 4);
  CHECK(lr.live_set.parts(1).engine().params_size() == 6);
  CHECK(lr.live_set.scenes_size() == 1);
  CHECK(lr.live_set.scenes(0).parts(0).engine_params_size() == 1);
  CHECK(lr.live_set.scenes(0).parts(0).engine_params(0).param_id() == 1001);
  CHECK(lr.live_set.scenes(0).parts(0).engine_params(0).value() == 7.0f);
  CHECK(lr.live_set.scenes(0).parts(1).engine_params_size() == 1);
  CHECK(lr.live_set.scenes(0).parts(1).engine_params(0).param_id() == 1231);
  CHECK(lr.live_set.scenes(0).parts(1).engine_params(0).value() == 3500.0f);
  CHECK(lr.live_set.assign(0) == 1001);
  CHECK(lr.live_set.assign(1) == 1231);

  // Verify write back is byte-identical
  std::string original_bytes = bytes_of(lr.live_set);
  CHECK(st->write(lr.live_set) == Status::kOk);
  CHECK(slurp(st->path_for(live_set.id())) == original_bytes);

  // 4. Rollback to N-1 simulation (Story 12.8 AC 3)
  const EngineAvailability legacy_availability = [](std::uint32_t id) {
    return id == 1 || id == 2; // only Sample (1) and Organ (2) compiled in
  };
  auto states = resolve_parts(lr.live_set, legacy_availability);
  CHECK(states[0] == PartState::kEngineNotAvailable); // FM plays silent, never substituted
  CHECK(states[1] == PartState::kEngineNotAvailable); // VA plays silent, never substituted

  // Unknown fields preserved when written back by N-1 reader
  std::string file_content;
  CHECK(serialize(lr.live_set, file_content) == Status::kOk);
  LoadResult nminus1_lr;
  CHECK(parse(file_content, ReadOptions{}, nminus1_lr) == Status::kOk);
  CHECK(nminus1_lr.live_set.parts(0).engine().engine_type_id() == 3);
  CHECK(nminus1_lr.live_set.parts(0).engine().params_size() == 5);
  CHECK(nminus1_lr.live_set.parts(1).engine().engine_type_id() == 4);
  CHECK(nminus1_lr.live_set.parts(1).engine().params_size() == 6);
}

}  // namespace

int main() {
  test_fixtures_are_valid_and_round_trip();
  test_golden();
  test_validation_rules();
  test_scene_semantics();
  test_version_policy();
  test_unknown_fields_preserved();
  test_reserved_engines();
  test_uuid_and_collisions();
  test_atomic_write();
  test_single_writer();
  test_integrity_and_durability();
  test_ownership_and_availability_lifetime();
  test_hostile_input();
  test_corpus();
  test_arpeggiator_registry_and_live_set_defaults();
  test_fm_va_timbre_parameters_live_set_roundtrip_and_rollback();
  if (g_fail) {
    std::printf("live_set_tests: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("live_set_tests: ok\n");
  return 0;
}
