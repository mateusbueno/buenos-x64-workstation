// ConfigStore tests (Foundation: sysd config service): defaults and first start, validation (every typed error, nothing changes), atomic persist with kill
// injection at every stage (simulated and by real process kills), the load policy (N, N-1, too old, unknown major, corrupt, invalid content), unknown-field
// round trip, N-1 files rewritten as N only through the migration runner, persist failures (ENOSPC, rename/dir sync), revision persistence, single writer.
// No sleeps, no wall clock, temp directories, no root.
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "velora/host/sysd_config_client.hpp"
#include "velora/sysd/config_store.hpp"

namespace fs = std::filesystem;
namespace sd = velora::sysd;
namespace ls = velora::host::liveset;

static int g_fail = 0;
#define CHECK(c)                                               \
  do {                                                         \
    if (!(c)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

namespace {

struct TmpDir {
  std::string path;
  TmpDir() {
    std::string t = (fs::temp_directory_path() / "velora-sysd-XXXXXX").string();
    if (!::mkdtemp(t.data())) std::abort();
    path = t;
  }
  ~TmpDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

std::string slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
void spit(const std::string& p, const std::string& b) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(b.data(), static_cast<std::streamsize>(b.size()));
}
std::string cfg_file(const TmpDir& d) { return d.path + "/" + sd::kFileName; }
bool exists(const std::string& p) { return fs::exists(p); }

std::vector<std::string> g_log;
sd::StoreOptions opts(const TmpDir& d, std::uint32_t n = sd::kSchemaVersion) {
  sd::StoreOptions o;
  o.dir = d.path;
  o.current_version = n;
  o.log = [](const std::string& s) { g_log.push_back(s); };
  return o;
}

ws::v1::ConfigPatch clock_patch(ws::v1::ClockSource c) {
  ws::v1::ConfigPatch p;
  p.set_clock_source(c);
  return p;
}
ws::v1::ConfigPatch pedal_patch(bool sustain, unsigned mn, unsigned mx, ws::v1::PedalPolarity pol = ws::v1::PEDAL_POLARITY_UNSPECIFIED) {
  ws::v1::ConfigPatch p;
  ws::v1::PedalPatch* c = sustain ? p.mutable_sustain() : p.mutable_expression();
  c->set_min(mn);
  c->set_max(mx);
  c->set_polarity(pol);
  return p;
}
ws::v1::ConfigPatch boot_patch(ws::v1::BootIntoMode m, const std::string& id) {
  ws::v1::ConfigPatch p;
  p.mutable_boot_into()->set_mode(m);
  p.mutable_boot_into()->set_default_live_set_id(id);
  return p;
}

// A SysdConfig file at `version` with the given config.
std::string file_bytes(std::uint32_t version, std::uint64_t revision, const ws::v1::Config& c) {
  velora::sysd::v1::SysdConfig f;
  f.set_schema_version(version);
  f.set_revision(revision);
  *f.mutable_config() = c;
  return f.SerializeAsString();
}

bool same_config(const ws::v1::Config& a, const ws::v1::Config& b) { return a.SerializeAsString() == b.SerializeAsString(); }

void test_defaults_first_start() {
  TmpDir d;
  sd::ConfigStore s(opts(d));
  CHECK(s.writer_locked());
  CHECK(s.load_reason() == sd::LoadReason::kMissing);
  CHECK(s.revision() == 0);
  CHECK(same_config(s.config(), velora::host::default_config()));
  CHECK(s.config().clock_source() == ws::v1::CLOCK_SOURCE_INTERNAL);
  CHECK(s.config().velocity_curve() == ws::v1::VELOCITY_CURVE_NORMAL);
  CHECK(s.config().boot_into().mode() == ws::v1::BOOT_INTO_MODE_LAST_SOUND);
  CHECK(s.config().sustain().min() == 0 && s.config().sustain().max() == 0);
  CHECK(!exists(cfg_file(d)));  // the file is created on the first change only
  // A valid change creates it: revision 1, 0644, parses, N.
  ::umask(022);
  const sd::ApplyResult r = s.apply(clock_patch(ws::v1::CLOCK_SOURCE_DIN));
  CHECK(r.status == sd::ApplyStatus::kApplied);
  CHECK(s.revision() == 1 && s.config().clock_source() == ws::v1::CLOCK_SOURCE_DIN);
  struct stat st{};
  CHECK(::stat(cfg_file(d).c_str(), &st) == 0 && (st.st_mode & 0777) == 0644);
  velora::sysd::v1::SysdConfig f;
  CHECK(f.ParseFromString(slurp(cfg_file(d))));
  CHECK(f.schema_version() == sd::kSchemaVersion && f.revision() == 1 && f.config().clock_source() == ws::v1::CLOCK_SOURCE_DIN);
  CHECK(!exists(d.path + "/" + sd::kTmpFileName));
  // Identical change: nothing written, revision unchanged.
  const std::string before = slurp(cfg_file(d));
  CHECK(s.apply(clock_patch(ws::v1::CLOCK_SOURCE_DIN)).status == sd::ApplyStatus::kUnchanged);
  CHECK(s.revision() == 1 && slurp(cfg_file(d)) == before);
}

void test_valid_updates_persist_and_reload() {
  TmpDir d;
  {
    sd::ConfigStore s(opts(d));
    CHECK(s.apply(clock_patch(ws::v1::CLOCK_SOURCE_USB)).changed());
    ws::v1::ConfigPatch p;
    p.set_velocity_curve(ws::v1::VELOCITY_CURVE_HEAVY);
    *p.mutable_sustain() = pedal_patch(true, 120, 3900, ws::v1::PEDAL_POLARITY_NORMALLY_CLOSED).sustain();
    *p.mutable_expression() = pedal_patch(false, 5, 4095).expression();
    *p.mutable_boot_into() = boot_patch(ws::v1::BOOT_INTO_MODE_DEFAULT_LIVE_SET, "b8e2f6a3-5d40-4f9b-8c71-2a3b4c5d6e71").boot_into();
    CHECK(s.apply(p).status == sd::ApplyStatus::kApplied);
    CHECK(s.revision() == 2);
    // A partial update leaves the other fields alone.
    CHECK(s.apply(pedal_patch(true, 0, 0)).status == sd::ApplyStatus::kApplied);  // 0/0 = back to uncalibrated
    CHECK(s.config().sustain().min() == 0 && s.config().sustain().max() == 0);
    CHECK(s.config().expression().max() == 4095 && s.config().velocity_curve() == ws::v1::VELOCITY_CURVE_HEAVY);
  }
  sd::ConfigStore s2(opts(d));  // survives a restart, including the revision
  CHECK(s2.load_reason() == sd::LoadReason::kLoaded);
  CHECK(s2.revision() == 3);
  CHECK(s2.config().clock_source() == ws::v1::CLOCK_SOURCE_USB && s2.config().velocity_curve() == ws::v1::VELOCITY_CURVE_HEAVY);
  CHECK(s2.config().expression().min() == 5 && s2.config().boot_into().default_live_set_id() == "b8e2f6a3-5d40-4f9b-8c71-2a3b4c5d6e71");
  CHECK(s2.apply(clock_patch(ws::v1::CLOCK_SOURCE_INTERNAL)).changed());
  CHECK(s2.revision() == 4);
}

// Field-wise patches: a patch with only some fields never clears the others (Stories 3.11 and 8.9 rely on it).
void test_fieldwise_patch() {
  TmpDir d;
  sd::ConfigStore s(opts(d));
  CHECK(s.apply(pedal_patch(true, 120, 3900, ws::v1::PEDAL_POLARITY_NORMALLY_CLOSED)).changed());
  {  // only the polarity
    ws::v1::ConfigPatch p;
    p.mutable_sustain()->set_polarity(ws::v1::PEDAL_POLARITY_NORMALLY_OPEN);
    CHECK(s.apply(p).status == sd::ApplyStatus::kApplied);
    CHECK(s.config().sustain().min() == 120 && s.config().sustain().max() == 3900 && s.config().sustain().polarity() == ws::v1::PEDAL_POLARITY_NORMALLY_OPEN);
  }
  {  // only max, then only min
    ws::v1::ConfigPatch p;
    p.mutable_sustain()->set_max(4000);
    CHECK(s.apply(p).changed() && s.config().sustain().min() == 120 && s.config().sustain().max() == 4000);
    ws::v1::ConfigPatch q;
    q.mutable_sustain()->set_min(10);
    CHECK(s.apply(q).changed() && s.config().sustain().min() == 10 && s.config().sustain().max() == 4000);
    CHECK(s.config().sustain().polarity() == ws::v1::PEDAL_POLARITY_NORMALLY_OPEN);
    CHECK(s.config().expression().max() == 0);  // the other pedal is untouched
  }
  {  // a partial change that makes the merged result invalid is refused as a whole
    ws::v1::ConfigPatch p;
    p.mutable_sustain()->set_min(4001);
    const std::uint64_t rev = s.revision();
    const sd::ApplyResult r = s.apply(p);
    CHECK(r.status == sd::ApplyStatus::kRejected && r.rejection.code == ws::v1::ERROR_CODE_INVALID_CONFIG && s.revision() == rev && s.config().sustain().min() == 10);
    ws::v1::ConfigPatch q;
    q.mutable_sustain()->set_max(4096);
    CHECK(s.apply(q).rejection.code == ws::v1::ERROR_CODE_OUT_OF_RANGE);
  }
  {  // an empty sub-message is an empty patch
    ws::v1::ConfigPatch p;
    p.mutable_sustain();
    p.mutable_boot_into();
    const sd::ApplyResult r = s.apply(p);
    CHECK(r.status == sd::ApplyStatus::kRejected && r.rejection.details.at("reason") == "empty_patch");
  }
  {  // Boot into: only the id keeps the mode; only the mode keeps the id
    ws::v1::ConfigPatch a;
    a.mutable_boot_into()->set_mode(ws::v1::BOOT_INTO_MODE_DEFAULT_LIVE_SET);
    CHECK(s.apply(a).rejection.details.at("reason") == "missing_live_set_id");  // the merged result needs an id
    ws::v1::ConfigPatch b;
    b.mutable_boot_into()->set_default_live_set_id("b8e2f6a3-5d40-4f9b-8c71-2a3b4c5d6e71");
    CHECK(s.apply(b).changed() && s.config().boot_into().mode() == ws::v1::BOOT_INTO_MODE_LAST_SOUND && s.config().boot_into().default_live_set_id() == "b8e2f6a3-5d40-4f9b-8c71-2a3b4c5d6e71");
    CHECK(s.apply(a).changed() && s.config().boot_into().mode() == ws::v1::BOOT_INTO_MODE_DEFAULT_LIVE_SET && s.config().boot_into().default_live_set_id() == "b8e2f6a3-5d40-4f9b-8c71-2a3b4c5d6e71");
    ws::v1::ConfigPatch c;
    c.mutable_boot_into()->set_mode(ws::v1::BOOT_INTO_MODE_START_OF_SETLIST);
    CHECK(s.apply(c).changed() && s.config().boot_into().default_live_set_id() == "b8e2f6a3-5d40-4f9b-8c71-2a3b4c5d6e71");  // the id stays
    ws::v1::ConfigPatch e;
    e.mutable_boot_into()->set_default_live_set_id("");
    CHECK(s.apply(e).changed() && s.config().boot_into().default_live_set_id().empty());
  }
}

void expect_rejected(const char* what, const ws::v1::ConfigPatch& p, ws::v1::ErrorCode code, const char* reason = nullptr) {
  TmpDir d;
  sd::ConfigStore s(opts(d));
  CHECK(s.apply(clock_patch(ws::v1::CLOCK_SOURCE_DIN)).changed());
  const ws::v1::Config before = s.config();
  const std::uint64_t rev = s.revision();
  const std::string bytes = slurp(cfg_file(d));
  const sd::ApplyResult r = s.apply(p);
  if (r.status != sd::ApplyStatus::kRejected || r.rejection.code != code ||
      (reason && r.rejection.details.count("reason") && r.rejection.details.at("reason") != reason) || (reason && !r.rejection.details.count("reason"))) {
    std::printf("FAIL %s: status %d code %s\n", what, static_cast<int>(r.status), ws::v1::ErrorCode_Name(r.rejection.code).c_str());
    ++g_fail;
  }
  CHECK(same_config(s.config(), before) && s.revision() == rev);  // nothing changed in memory
  CHECK(slurp(cfg_file(d)) == bytes);                            // nor on disk
  CHECK(!exists(d.path + "/" + sd::kTmpFileName));
}

// Story 8.9: a legacy default_live_set_id (not a UUID) stays loadable; with DEFAULT_LIVE_SET it loads as LAST_SOUND (note logged), and patches use the UUID rule.
void test_legacy_default_live_set_id() {
  const std::string uuid = "b8e2f6a3-5d40-4f9b-8c71-2a3b4c5d6e71";
  {  // legacy id, DEFAULT mode: the file loads, as LAST_SOUND, the id is kept, a note is logged
    TmpDir d;
    ws::v1::Config c = velora::host::default_config();
    c.mutable_boot_into()->set_mode(ws::v1::BOOT_INTO_MODE_DEFAULT_LIVE_SET);
    c.mutable_boot_into()->set_default_live_set_id("live-1");
    c.set_clock_source(ws::v1::CLOCK_SOURCE_USB);
    { std::ofstream f(cfg_file(d), std::ios::binary); f << file_bytes(sd::kSchemaVersion, 7, c); }
    g_log.clear();
    sd::ConfigStore s(opts(d));
    CHECK(s.load_reason() == sd::LoadReason::kLoaded);
    CHECK(s.config().boot_into().mode() == ws::v1::BOOT_INTO_MODE_LAST_SOUND && s.config().boot_into().default_live_set_id() == "live-1");
    CHECK(s.config().clock_source() == ws::v1::CLOCK_SOURCE_USB);
    bool noted = false;
    for (const std::string& l : g_log) noted = noted || l.find("loaded as LAST_SOUND") != std::string::npos;
    CHECK(noted);
    // a patch that only changes the mode to DEFAULT cannot use the legacy id
    ws::v1::ConfigPatch m;
    m.mutable_boot_into()->set_mode(ws::v1::BOOT_INTO_MODE_DEFAULT_LIVE_SET);
    CHECK(s.apply(m).status == sd::ApplyStatus::kRejected);
    // another mode with a legacy id kept is fine, and a real id then works with the mode
    ws::v1::ConfigPatch sl;
    sl.mutable_boot_into()->set_mode(ws::v1::BOOT_INTO_MODE_START_OF_SETLIST);
    CHECK(s.apply(sl).changed());
    CHECK(s.apply(boot_patch(ws::v1::BOOT_INTO_MODE_DEFAULT_LIVE_SET, uuid)).changed());
  }
  {  // legacy id with LAST_SOUND: untouched
    TmpDir d;
    ws::v1::Config c = velora::host::default_config();
    c.mutable_boot_into()->set_mode(ws::v1::BOOT_INTO_MODE_LAST_SOUND);
    c.mutable_boot_into()->set_default_live_set_id("live-1");
    { std::ofstream f(cfg_file(d), std::ios::binary); f << file_bytes(sd::kSchemaVersion, 1, c); }
    sd::ConfigStore s(opts(d));
    CHECK(s.load_reason() == sd::LoadReason::kLoaded && s.config().boot_into().default_live_set_id() == "live-1");
  }
}

void test_validation() {
  using E = ws::v1::ErrorCode;
  expect_rejected("empty patch", ws::v1::ConfigPatch{}, ws::v1::ERROR_CODE_INVALID_COMMAND, "empty_patch");
  expect_rejected("clock unspecified", clock_patch(ws::v1::CLOCK_SOURCE_UNSPECIFIED), E::ERROR_CODE_UNKNOWN_ENUM_VALUE);
  {
    ws::v1::ConfigPatch p;
    p.set_clock_source(static_cast<ws::v1::ClockSource>(99));
    expect_rejected("clock out of enum", p, E::ERROR_CODE_UNKNOWN_ENUM_VALUE);
  }
  {
    ws::v1::ConfigPatch p;
    p.set_velocity_curve(static_cast<ws::v1::VelocityCurve>(-1));
    expect_rejected("curve out of enum", p, E::ERROR_CODE_UNKNOWN_ENUM_VALUE);
    p.set_velocity_curve(ws::v1::VELOCITY_CURVE_UNSPECIFIED);
    expect_rejected("curve unspecified", p, E::ERROR_CODE_UNKNOWN_ENUM_VALUE);
  }
  expect_rejected("min == max", pedal_patch(true, 500, 500), E::ERROR_CODE_INVALID_CONFIG, "min_not_below_max");
  expect_rejected("min > max", pedal_patch(false, 900, 100), E::ERROR_CODE_INVALID_CONFIG, "min_not_below_max");
  expect_rejected("max above resolution", pedal_patch(true, 0, 4096), E::ERROR_CODE_OUT_OF_RANGE);
  expect_rejected("min above resolution", pedal_patch(true, 5000, 6000), E::ERROR_CODE_OUT_OF_RANGE);
  // Story 3.11: a calibrated range needs at least 256 counts of travel (255 is refused, 256 is the smallest accepted; 0 / 0 stays "uncalibrated").
  expect_rejected("span 255", pedal_patch(true, 100, 355), E::ERROR_CODE_INVALID_CONFIG, "span_too_small");
  expect_rejected("span 1", pedal_patch(false, 4094, 4095), E::ERROR_CODE_INVALID_CONFIG, "span_too_small");
  expect_rejected("pedal polarity enum", pedal_patch(true, 1, 2, static_cast<ws::v1::PedalPolarity>(7)), E::ERROR_CODE_UNKNOWN_ENUM_VALUE);
  expect_rejected("boot mode unspecified", boot_patch(ws::v1::BOOT_INTO_MODE_UNSPECIFIED, ""), E::ERROR_CODE_UNKNOWN_ENUM_VALUE);
  expect_rejected("boot mode out of enum", boot_patch(static_cast<ws::v1::BootIntoMode>(9), ""), E::ERROR_CODE_UNKNOWN_ENUM_VALUE);
  expect_rejected("default live set without id", boot_patch(ws::v1::BOOT_INTO_MODE_DEFAULT_LIVE_SET, ""), E::ERROR_CODE_INVALID_CONFIG, "missing_live_set_id");
  expect_rejected("id with a slash", boot_patch(ws::v1::BOOT_INTO_MODE_LAST_SOUND, "../etc"), E::ERROR_CODE_INVALID_CONFIG, "bad_live_set_id");
  expect_rejected("id too long", boot_patch(ws::v1::BOOT_INTO_MODE_LAST_SOUND, std::string(65, 'a')), E::ERROR_CODE_INVALID_CONFIG, "bad_live_set_id");
  expect_rejected("legacy id format is refused in a patch", boot_patch(ws::v1::BOOT_INTO_MODE_LAST_SOUND, "live-1"), E::ERROR_CODE_INVALID_CONFIG, "bad_live_set_id");
  expect_rejected("uppercase uuid", boot_patch(ws::v1::BOOT_INTO_MODE_LAST_SOUND, "B8E2F6A3-5D40-4F9B-8C71-2A3B4C5D6E71"), E::ERROR_CODE_INVALID_CONFIG, "bad_live_set_id");
  expect_rejected("id with a space", boot_patch(ws::v1::BOOT_INTO_MODE_DEFAULT_LIVE_SET, "a b"), E::ERROR_CODE_INVALID_CONFIG, "bad_live_set_id");
  {  // one bad field refuses the whole patch: the valid clock change is not applied either
    ws::v1::ConfigPatch p = pedal_patch(true, 10, 5);
    p.set_clock_source(ws::v1::CLOCK_SOURCE_USB);
    expect_rejected("mixed patch", p, E::ERROR_CODE_INVALID_CONFIG);
  }
  {  // a boundary that is valid
    TmpDir d;
    sd::ConfigStore s(opts(d));
    CHECK(s.apply(pedal_patch(true, 0, 256)).status == sd::ApplyStatus::kApplied);
    CHECK(s.apply(pedal_patch(false, 3839, 4095)).status == sd::ApplyStatus::kApplied);
    CHECK(s.apply(boot_patch(ws::v1::BOOT_INTO_MODE_DEFAULT_LIVE_SET, "00000000-0000-4000-8000-000000000001")).status == sd::ApplyStatus::kApplied);
  }
}

// ---- load policy -------------------------------------------------------------------------------------------------------
void test_load_corrupt_and_invalid() {
  {
    TmpDir d;
    spit(cfg_file(d), std::string("\xff\xff\xff not a protobuf", 20));
    g_log.clear();
    sd::ConfigStore s(opts(d));
    CHECK(s.load_reason() == sd::LoadReason::kCorrupt);
    CHECK(same_config(s.config(), velora::host::default_config()) && s.revision() == 0);
    CHECK(exists(cfg_file(d) + ".corrupt") && !exists(cfg_file(d)));  // kept aside
    CHECK(!g_log.empty() && g_log.back().find("corrupt") != std::string::npos);
    CHECK(s.apply(clock_patch(ws::v1::CLOCK_SOURCE_USB)).changed());  // keeps working
    CHECK(exists(cfg_file(d) + ".corrupt"));
  }
  {  // empty file: corrupt (schema_version 0 reads as too_old: either way defaults)
    TmpDir d;
    spit(cfg_file(d), "");
    sd::ConfigStore s(opts(d));
    CHECK(s.load_reason() != sd::LoadReason::kLoaded && same_config(s.config(), velora::host::default_config()));
  }
  {  // oversized
    TmpDir d;
    spit(cfg_file(d), std::string(sd::kMaxFileBytes + 1, 'x'));
    sd::ConfigStore s(opts(d));
    CHECK(s.load_reason() == sd::LoadReason::kTooLarge && same_config(s.config(), velora::host::default_config()));
  }
  {  // parses, but a value is invalid (pedal min >= max, an enum value this release does not know)
    TmpDir d;
    ws::v1::Config c = velora::host::default_config();
    c.mutable_sustain()->set_min(900);
    c.mutable_sustain()->set_max(100);
    spit(cfg_file(d), file_bytes(sd::kSchemaVersion, 7, c));
    sd::ConfigStore s(opts(d));
    CHECK(s.load_reason() == sd::LoadReason::kInvalidContent && same_config(s.config(), velora::host::default_config()));
    CHECK(exists(cfg_file(d) + ".corrupt"));
    TmpDir d2;
    ws::v1::Config c2 = velora::host::default_config();
    c2.set_clock_source(static_cast<ws::v1::ClockSource>(77));
    spit(cfg_file(d2), file_bytes(sd::kSchemaVersion, 7, c2));
    sd::ConfigStore s2(opts(d2));
    CHECK(s2.load_reason() == sd::LoadReason::kInvalidContent && s2.config().clock_source() == ws::v1::CLOCK_SOURCE_INTERNAL);
  }
  {  // Story 3.11: a legacy pedal span of 1..255 counts does not void the file: the pedal becomes 0 / 0 (polarity kept), everything else loads
    TmpDir d;
    ws::v1::Config c = velora::host::default_config();
    c.set_clock_source(ws::v1::CLOCK_SOURCE_USB);
    c.mutable_sustain()->set_min(100);
    c.mutable_sustain()->set_max(200);
    c.mutable_sustain()->set_polarity(ws::v1::PEDAL_POLARITY_NORMALLY_CLOSED);
    c.mutable_expression()->set_min(500);
    c.mutable_expression()->set_max(3000);
    spit(cfg_file(d), file_bytes(sd::kSchemaVersion, 7, c));
    sd::ConfigStore s(opts(d));
    CHECK(s.load_reason() == sd::LoadReason::kLoaded && s.config().clock_source() == ws::v1::CLOCK_SOURCE_USB);
    CHECK(s.config().sustain().min() == 0 && s.config().sustain().max() == 0 && s.config().sustain().polarity() == ws::v1::PEDAL_POLARITY_NORMALLY_CLOSED);
    CHECK(s.config().expression().min() == 500 && s.config().expression().max() == 3000);
    CHECK(!exists(cfg_file(d) + ".corrupt"));
    expect_rejected("legacy span still refused as a patch", pedal_patch(true, 100, 200), ws::v1::ERROR_CODE_INVALID_CONFIG, "span_too_small");
  }
}

void test_load_versions() {
  const ws::v1::Config cur = [] {
    ws::v1::Config c = velora::host::default_config();
    c.set_clock_source(ws::v1::CLOCK_SOURCE_USB);
    return c;
  }();
  {  // N loads
    TmpDir d;
    spit(cfg_file(d), file_bytes(1, 5, cur));
    sd::ConfigStore s(opts(d));
    CHECK(s.load_reason() == sd::LoadReason::kLoaded && s.revision() == 5 && s.config().clock_source() == ws::v1::CLOCK_SOURCE_USB && s.file_version() == 1);
  }
  {  // fields absent from an older writer are filled with the defaults in memory (the file is not touched)
    TmpDir d;
    velora::sysd::v1::SysdConfig f;
    f.set_schema_version(1);
    f.mutable_config()->set_clock_source(ws::v1::CLOCK_SOURCE_DIN);  // nothing else on the wire
    spit(cfg_file(d), f.SerializeAsString());
    const std::string bytes = slurp(cfg_file(d));
    sd::ConfigStore s(opts(d));
    CHECK(s.load_reason() == sd::LoadReason::kLoaded);
    CHECK(s.config().velocity_curve() == ws::v1::VELOCITY_CURVE_NORMAL && s.config().boot_into().mode() == ws::v1::BOOT_INTO_MODE_LAST_SOUND);
    CHECK(slurp(cfg_file(d)) == bytes);
  }
  {  // N = 3 reader: 2 (N-1) loads; 1 (too old) and 4 (too new) give defaults and a typed reason, and the file is untouched on read
    for (const std::uint32_t v : {1u, 2u, 3u, 4u, 0xFFFFFFFFu}) {
      TmpDir d;
      spit(cfg_file(d), file_bytes(v, 9, cur));
      const std::string bytes = slurp(cfg_file(d));
      g_log.clear();
      sd::ConfigStore s(opts(d, 3));
      const bool readable = v == 2 || v == 3;
      CHECK((s.load_reason() == sd::LoadReason::kLoaded) == readable);
      if (!readable) {
        CHECK(s.load_reason() == (v < 2 ? sd::LoadReason::kTooOld : sd::LoadReason::kTooNew));
        CHECK(same_config(s.config(), velora::host::default_config()) && s.revision() == 0);
        CHECK(!g_log.empty() && g_log.back().find(v < 2 ? "too_old" : "too_new") != std::string::npos);
      } else {
        CHECK(s.config().clock_source() == ws::v1::CLOCK_SOURCE_USB && s.revision() == 9);
      }
      CHECK(slurp(cfg_file(d)) == bytes);  // never a write on read
    }
  }
  {  // too_new: the first successful write keeps the unreadable file aside, never loses it
    TmpDir d;
    spit(cfg_file(d), file_bytes(9, 9, cur));
    const std::string bytes = slurp(cfg_file(d));
    sd::ConfigStore s(opts(d));
    CHECK(s.load_reason() == sd::LoadReason::kTooNew);
    CHECK(s.apply(clock_patch(ws::v1::CLOCK_SOURCE_DIN)).status == sd::ApplyStatus::kApplied);
    CHECK(slurp(cfg_file(d) + ".too_new") == bytes);
    velora::sysd::v1::SysdConfig f;
    CHECK(f.ParseFromString(slurp(cfg_file(d))) && f.schema_version() == sd::kSchemaVersion);
  }
  {  // too large: kept aside before the first write, too
    TmpDir d;
    const std::string big(sd::kMaxFileBytes + 10, 'x');
    spit(cfg_file(d), big);
    sd::ConfigStore s(opts(d));
    CHECK(s.load_reason() == sd::LoadReason::kTooLarge);
    CHECK(s.apply(clock_patch(ws::v1::CLOCK_SOURCE_DIN)).changed());
    CHECK(slurp(cfg_file(d) + ".too_large") == big);
  }
}

// The aside never costs the primary file: a failed replace and a crash between the aside and the replace leave it untouched (or fully replaced).
void test_aside_safety() {
  const ws::v1::Config cur = velora::host::default_config();
  {  // failed replace after the aside: primary untouched, one aside, the retry makes no second copy and succeeds
    TmpDir d;
    const std::string original = file_bytes(9, 9, cur);
    spit(cfg_file(d), original);
    bool fail = true;
    sd::StoreOptions o = opts(d);
    o.on_stage = [&](ls::Stage) { return fail ? ls::HookAction::kAbort : ls::HookAction::kContinue; };
    sd::ConfigStore s(o);
    CHECK(s.apply(clock_patch(ws::v1::CLOCK_SOURCE_DIN)).status == sd::ApplyStatus::kRejected);
    CHECK(slurp(cfg_file(d)) == original);
    CHECK(slurp(cfg_file(d) + ".too_new") == original);
    fail = false;
    CHECK(s.apply(clock_patch(ws::v1::CLOCK_SOURCE_DIN)).status == sd::ApplyStatus::kApplied);
    CHECK(slurp(cfg_file(d) + ".too_new") == original && !exists(cfg_file(d) + ".too_new.1"));
    velora::sysd::v1::SysdConfig f;
    CHECK(f.ParseFromString(slurp(cfg_file(d))) && f.schema_version() == sd::kSchemaVersion);
  }
  {  // crash at each stage of the replace: the primary is the original (before the rename) or the new file (after), the aside always holds the original
    const ls::Stage stages[] = {ls::Stage::kTempWritten, ls::Stage::kFsynced, ls::Stage::kRenamed, ls::Stage::kDirSynced};
    for (const ls::Stage at : stages) {
      TmpDir d;
      const std::string original = file_bytes(9, 9, cur);
      spit(cfg_file(d), original);
      {
        sd::StoreOptions o = opts(d);
        o.on_stage = [at](ls::Stage st) { return st == at ? ls::HookAction::kAbort : ls::HookAction::kContinue; };
        sd::ConfigStore s(o);
        CHECK(s.apply(clock_patch(ws::v1::CLOCK_SOURCE_DIN)).status == sd::ApplyStatus::kRejected);
      }
      const bool renamed = at == ls::Stage::kRenamed || at == ls::Stage::kDirSynced;
      CHECK((slurp(cfg_file(d)) == original) == !renamed);
      CHECK(slurp(cfg_file(d) + ".too_new") == original);
      sd::ConfigStore after(opts(d));
      if (renamed) {
        CHECK(after.load_reason() == sd::LoadReason::kLoaded && after.config().clock_source() == ws::v1::CLOCK_SOURCE_DIN);
      } else {
        CHECK(after.load_reason() == sd::LoadReason::kTooNew);
      }
    }
  }
  {  // two corruptions in a row keep both files
    TmpDir d;
    spit(cfg_file(d), std::string("\xff\xff\xff first", 12));
    { sd::ConfigStore s(opts(d)); }
    spit(cfg_file(d), std::string("\xff\xff\xff second", 13));
    { sd::ConfigStore s(opts(d)); }
    CHECK(slurp(cfg_file(d) + ".corrupt") == std::string("\xff\xff\xff first", 12));
    CHECK(slurp(cfg_file(d) + ".corrupt.1") == std::string("\xff\xff\xff second", 13));
    CHECK(!exists(cfg_file(d)));
  }
}

void test_unknown_fields_round_trip() {
  TmpDir d;
  velora::sysd::v1::SysdConfig f;
  f.set_schema_version(1);
  f.set_revision(4);
  *f.mutable_config() = velora::host::default_config();
  // Unknown fields at the file level and inside the Config, as a future release would write.
  f.GetReflection()->MutableUnknownFields(&f)->AddVarint(900, 12345);
  f.mutable_config()->GetReflection()->MutableUnknownFields(f.mutable_config())->AddLengthDelimited(901, "future");
  f.mutable_config()->mutable_sustain()->GetReflection()->MutableUnknownFields(f.mutable_config()->mutable_sustain())->AddVarint(902, 7);
  spit(cfg_file(d), f.SerializeAsString());
  sd::ConfigStore s(opts(d));
  CHECK(s.load_reason() == sd::LoadReason::kLoaded);
  CHECK(s.apply(clock_patch(ws::v1::CLOCK_SOURCE_USB)).status == sd::ApplyStatus::kApplied);
  CHECK(s.apply(pedal_patch(true, 10, 300)).status == sd::ApplyStatus::kApplied);  // replaces min/max of the pedal that holds an unknown field
  velora::sysd::v1::SysdConfig g;
  CHECK(g.ParseFromString(slurp(cfg_file(d))));
  CHECK(g.GetReflection()->GetUnknownFields(g).field_count() == 1 && g.GetReflection()->GetUnknownFields(g).field(0).number() == 900);
  CHECK(g.config().GetReflection()->GetUnknownFields(g.config()).field_count() == 1);
  CHECK(g.config().sustain().GetReflection()->GetUnknownFields(g.config().sustain()).field_count() == 1);
  CHECK(g.config().sustain().min() == 10 && g.config().clock_source() == ws::v1::CLOCK_SOURCE_USB && g.revision() == 6);
}

// An N-1 file: loads read-only; a SetConfig keeps the N-1 stamp; the migration runner rewrites it as N, only once the slot is good.
void test_n_minus_1_and_migration() {
  TmpDir d;
  ws::v1::Config c = velora::host::default_config();
  c.set_velocity_curve(ws::v1::VELOCITY_CURVE_LIGHT);
  spit(cfg_file(d), file_bytes(1, 3, c));  // N = 2 below: this is N-1
  const std::string original = slurp(cfg_file(d));
  bool good = false;
  int probes = 0;
  sd::StoreOptions o = opts(d, 2);
  o.slot_good = [&] {
    ++probes;
    return good;
  };
  sd::ConfigStore s(o);
  CHECK(s.load_reason() == sd::LoadReason::kLoaded && s.file_version() == 1 && s.revision() == 3);
  CHECK(slurp(cfg_file(d)) == original);  // no write on read
  // Before the slot is good: migration deferred, byte-identical, nothing created.
  CHECK(s.migrate() == velora::host::migration::Outcome::kDeferred);
  CHECK(slurp(cfg_file(d)) == original && !exists(d.path + "/" + sd::kTmpFileName));
  // A SetConfig keeps the N-1 stamp.
  CHECK(s.apply(clock_patch(ws::v1::CLOCK_SOURCE_DIN)).status == sd::ApplyStatus::kApplied);
  velora::sysd::v1::SysdConfig f;
  CHECK(f.ParseFromString(slurp(cfg_file(d))) && f.schema_version() == 1 && f.revision() == 4 && f.config().clock_source() == ws::v1::CLOCK_SOURCE_DIN);
  CHECK(s.file_version() == 1);
  // The slot is good: the migration rewrites the stamp as N, nothing else, and is then a no-op.
  good = true;
  CHECK(s.migrate() == velora::host::migration::Outcome::kMigrated);
  velora::sysd::v1::SysdConfig g;
  CHECK(g.ParseFromString(slurp(cfg_file(d))) && g.schema_version() == 2 && g.revision() == 4 && g.config().clock_source() == ws::v1::CLOCK_SOURCE_DIN);
  CHECK(g.config().velocity_curve() == ws::v1::VELOCITY_CURVE_LIGHT);
  CHECK(s.file_version() == 2);
  CHECK(s.migrate() == velora::host::migration::Outcome::kUpToDate);
  CHECK(s.apply(clock_patch(ws::v1::CLOCK_SOURCE_USB)).changed());
  CHECK(f.ParseFromString(slurp(cfg_file(d))) && f.schema_version() == 2);
  // No probe at all = never good (fails closed).
  TmpDir d2;
  spit(cfg_file(d2), file_bytes(1, 1, c));
  sd::ConfigStore s2(opts(d2, 2));
  CHECK(s2.migrate() == velora::host::migration::Outcome::kDeferred);
  // When the slot becomes good, a SetConfig writes N directly.
  TmpDir d3;
  spit(cfg_file(d3), file_bytes(1, 1, c));
  sd::StoreOptions o3 = opts(d3, 2);
  o3.slot_good = [] { return true; };
  sd::ConfigStore s3(o3);
  CHECK(s3.apply(clock_patch(ws::v1::CLOCK_SOURCE_USB)).changed());
  CHECK(f.ParseFromString(slurp(cfg_file(d3))) && f.schema_version() == 2);
}

// ---- persist failures and kills ---------------------------------------------------------------------------------------
void test_persist_failures() {
  TmpDir d;
  sd::StoreOptions o = opts(d);
  o.inject_write_errno = ENOSPC;
  sd::ConfigStore s(o);
  const sd::ApplyResult r = s.apply(clock_patch(ws::v1::CLOCK_SOURCE_DIN));
  CHECK(r.status == sd::ApplyStatus::kRejected && r.rejection.code == ws::v1::ERROR_CODE_STORAGE_FULL);
  CHECK(s.revision() == 0 && s.config().clock_source() == ws::v1::CLOCK_SOURCE_INTERNAL);
  CHECK(!exists(cfg_file(d)) && !exists(d.path + "/" + sd::kTmpFileName));
  // Other I/O failure: STORAGE_FAILED with a reason.
  TmpDir d2;
  sd::StoreOptions o2 = opts(d2);
  o2.inject_write_errno = EIO;
  sd::ConfigStore s2(o2);
  const sd::ApplyResult r2 = s2.apply(clock_patch(ws::v1::CLOCK_SOURCE_DIN));
  CHECK(r2.status == sd::ApplyStatus::kRejected && r2.rejection.code == ws::v1::ERROR_CODE_STORAGE_FAILED && r2.rejection.details.count("reason"));
  CHECK(s2.revision() == 0 && !exists(cfg_file(d2)));
  // The directory does not exist any more (rename fails): rejected, nothing changed.
  TmpDir d3;
  sd::ConfigStore s3(opts(d3));
  fs::remove(d3.path + "/" + sd::kLockFileName);
  fs::remove_all(d3.path);
  const sd::ApplyResult r3 = s3.apply(clock_patch(ws::v1::CLOCK_SOURCE_DIN));
  CHECK(r3.status == sd::ApplyStatus::kRejected && r3.rejection.code == ws::v1::ERROR_CODE_STORAGE_FAILED);
  CHECK(s3.revision() == 0 && s3.config().clock_source() == ws::v1::CLOCK_SOURCE_INTERNAL);
  fs::create_directories(d3.path);
  CHECK(s3.apply(clock_patch(ws::v1::CLOCK_SOURCE_DIN)).changed());  // and it recovers when the disk does
  // Directory sync failure after the rename: applied, not_durable (the documented choice), the new file is in place.
  TmpDir d4;
  sd::StoreOptions o4 = opts(d4);
  o4.inject_dir_sync_failure = true;
  sd::ConfigStore s4(o4);
  const sd::ApplyResult r4 = s4.apply(clock_patch(ws::v1::CLOCK_SOURCE_USB));
  CHECK(r4.status == sd::ApplyStatus::kNotDurable && r4.changed());
  CHECK(s4.revision() == 1 && s4.config().clock_source() == ws::v1::CLOCK_SOURCE_USB);
  velora::sysd::v1::SysdConfig f;
  CHECK(f.ParseFromString(slurp(cfg_file(d4))) && f.config().clock_source() == ws::v1::CLOCK_SOURCE_USB);
}

void test_simulated_kill_every_stage() {
  const ls::Stage stages[] = {ls::Stage::kTempWritten, ls::Stage::kFsynced, ls::Stage::kRenamed, ls::Stage::kDirSynced};
  for (const ls::Stage at : stages) {
    for (const bool preexisting : {false, true}) {
      TmpDir d;
      if (preexisting) {
        sd::ConfigStore s(opts(d));
        CHECK(s.apply(clock_patch(ws::v1::CLOCK_SOURCE_DIN)).changed());
      }
      {
        sd::StoreOptions o = opts(d);
        o.on_stage = [at](ls::Stage st) { return st == at ? ls::HookAction::kAbort : ls::HookAction::kContinue; };
        sd::ConfigStore s(o);
        const sd::ApplyResult r = s.apply(clock_patch(ws::v1::CLOCK_SOURCE_USB));
        // The writer stopped dead: the change is refused and not applied in memory; the process "died" here.
        CHECK(r.status == sd::ApplyStatus::kRejected);
        CHECK(s.config().clock_source() == (preexisting ? ws::v1::CLOCK_SOURCE_DIN : ws::v1::CLOCK_SOURCE_INTERNAL));
      }
      sd::ConfigStore after(opts(d));  // next start: old or new, never corrupt
      const bool renamed = at == ls::Stage::kRenamed || at == ls::Stage::kDirSynced;
      const ws::v1::ClockSource old_clock = preexisting ? ws::v1::CLOCK_SOURCE_DIN : ws::v1::CLOCK_SOURCE_INTERNAL;
      CHECK(after.config().clock_source() == (renamed ? ws::v1::CLOCK_SOURCE_USB : old_clock));
      CHECK(after.revision() == (renamed ? (preexisting ? 2u : 1u) : (preexisting ? 1u : 0u)));
      CHECK(after.load_reason() == ((renamed || preexisting) ? sd::LoadReason::kLoaded : sd::LoadReason::kMissing));
      CHECK(!exists(cfg_file(d) + ".corrupt"));
      // And it keeps working: a stale temp file never blocks the next write.
      CHECK(after.apply(clock_patch(ws::v1::CLOCK_SOURCE_INTERNAL)).changed() || after.config().clock_source() == ws::v1::CLOCK_SOURCE_INTERNAL);
      CHECK(after.config().clock_source() == ws::v1::CLOCK_SOURCE_INTERNAL);
    }
  }
}

void test_real_process_kills() {
  const ls::Stage stages[] = {ls::Stage::kTempWritten, ls::Stage::kFsynced, ls::Stage::kRenamed, ls::Stage::kDirSynced};
  TmpDir d;
  unsigned prev = 0;  // sustain max: every write changes it, so every write reaches the disk layer
  for (int i = 0; i < 24; ++i) {
    const ls::Stage at = stages[i % 4];
    const unsigned next = 400 + static_cast<unsigned>(i);
    const pid_t pid = ::fork();
    if (pid == 0) {
      sd::StoreOptions o;
      o.dir = d.path;
      o.on_stage = [at](ls::Stage st) -> ls::HookAction {
        if (st == at) ::_exit(0);  // killed mid-write
        return ls::HookAction::kContinue;
      };
      sd::ConfigStore s(o);
      s.apply(pedal_patch(true, 1, next));
      ::_exit(1);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    sd::ConfigStore after(opts(d));  // the dead writer's flock is gone with it
    CHECK(after.writer_locked());
    CHECK(after.load_reason() == sd::LoadReason::kLoaded || after.load_reason() == sd::LoadReason::kMissing);
    const unsigned got = after.config().sustain().max();
    CHECK(got == prev || got == next);
    prev = got;
  }
}

void test_single_writer_lock() {
  TmpDir d;
  sd::ConfigStore a(opts(d));
  CHECK(a.writer_locked());
  sd::ConfigStore b(opts(d));  // a second sysd on the same directory
  CHECK(!b.writer_locked());
  CHECK(b.apply(clock_patch(ws::v1::CLOCK_SOURCE_DIN)).status == sd::ApplyStatus::kRejected);
  CHECK(!exists(cfg_file(d)));
  CHECK(a.apply(clock_patch(ws::v1::CLOCK_SOURCE_DIN)).changed());
}

}  // namespace

int main() {
  test_legacy_default_live_set_id();
  test_defaults_first_start();
  test_valid_updates_persist_and_reload();
  test_validation();
  test_fieldwise_patch();
  test_load_corrupt_and_invalid();
  test_load_versions();
  test_unknown_fields_round_trip();
  test_n_minus_1_and_migration();
  test_aside_safety();
  test_persist_failures();
  test_simulated_kill_every_stage();
  test_real_process_kills();
  test_single_writer_lock();
  if (g_fail) {
    std::printf("sysd_config_tests: %d FAILED\n", g_fail);
    return 1;
  }
  std::printf("sysd_config_tests: ok\n");
  return 0;
}
