// sysd config version policy (Story 8.5, FR49, NFR44): N and N-1 load, older / newer are typed refusals that change nothing, unknown members survive
// read-modify-write (semantically: re-serialized, not byte for byte). N = 1 today; the N-1 path runs with N = 2 and 3 like the other formats' tests.
#include <cstdio>
#include <string>

#include "velora/ui/system_config.hpp"

using namespace velora::ui;

static int g_fail = 0;
#define CHECK(c)                                                       \
  do {                                                                 \
    if (!(c)) {                                                        \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);         \
      ++g_fail;                                                        \
    }                                                                  \
  } while (0)

static std::string doc(const std::string& version_member, const std::string& rest = "") {
  return "{" + version_member + "\"theme\": \"studio\"" + rest + "}";
}

int main() {
  // N = 1 (shipping): version 1 loads, 0 is too old, 2 too new, non-integers and non-objects are parse errors.
  {
    SystemConfig c;
    CHECK(c.load(doc("\"schema_version\": 1,")) == ConfigLoadStatus::Ok && c.theme == "studio" && !c.migrated && c.schema_version == 1);
    SystemConfig a;
    CHECK(a.load(doc("")) == ConfigLoadStatus::Ok && a.theme == "studio");  // no version member: read as N
    SystemConfig o;
    CHECK(o.load(doc("\"schema_version\": 0,")) == ConfigLoadStatus::TooOld && o.theme == "stage");  // refused: nothing changed
    CHECK(o.load(doc("\"schema_version\": -3,")) == ConfigLoadStatus::TooOld);
    CHECK(o.load(doc("\"schema_version\": 2,")) == ConfigLoadStatus::TooNew && o.theme == "stage");
    CHECK(o.load(doc("\"schema_version\": 4000000000,")) == ConfigLoadStatus::TooNew);
    CHECK(o.load(doc("\"schema_version\": 1.5,")) == ConfigLoadStatus::ParseError);
    CHECK(o.load(doc("\"schema_version\": \"1\",")) == ConfigLoadStatus::ParseError);
    CHECK(o.load("[1]") == ConfigLoadStatus::ParseError && o.load("{") == ConfigLoadStatus::ParseError && o.load("") == ConfigLoadStatus::ParseError);
    CHECK(o.theme == "stage" && o.unknown_fields.empty());
    CHECK(!o.from_json(doc("\"schema_version\": 2,")) && o.from_json(doc("\"schema_version\": 1,")));
  }
  // N = 2: N-1 (1) loads as migrated and is held as N; 3 is too new; 0 too old.
  {
    SystemConfig c;
    CHECK(c.load(doc("\"schema_version\": 1,"), 2) == ConfigLoadStatus::Ok && c.migrated && c.schema_version == 2);
    CHECK(c.to_json().find("\"schema_version\": 2") != std::string::npos);  // written back as N, never N+1
    SystemConfig n;
    CHECK(n.load(doc("\"schema_version\": 2,"), 2) == ConfigLoadStatus::Ok && !n.migrated);
    SystemConfig bad;
    CHECK(bad.load(doc("\"schema_version\": 3,"), 2) == ConfigLoadStatus::TooNew && bad.theme == "stage");
    CHECK(bad.load(doc("\"schema_version\": 0,"), 2) == ConfigLoadStatus::TooOld);
  }
  // N = 3: version 1 is two majors behind.
  {
    SystemConfig c;
    CHECK(c.load(doc("\"schema_version\": 1,"), 3) == ConfigLoadStatus::TooOld && c.theme == "stage");
    CHECK(c.load(doc("\"schema_version\": 2,"), 3) == ConfigLoadStatus::Ok && c.migrated);
  }
  // Every key to_json() emits is a known key: a new field missing from known_config_key() would come back as an unknown member and be emitted twice.
  {
    SystemConfig c;
    SystemConfig back;
    CHECK(back.load(c.to_json()) == ConfigLoadStatus::Ok && back.unknown_fields.empty());
  }
  // load() into a non-default object merges: keys absent from the file keep the current values.
  {
    SystemConfig c;
    c.language = "es";
    CHECK(c.load(doc("\"schema_version\": 1,")) == ConfigLoadStatus::Ok && c.language == "es" && c.theme == "studio");
  }
  // Unknown members of a newer minor survive read-modify-write, semantically equal (re-serialized) and stable across repeated writes.
  {
    const std::string newer = doc("\"schema_version\": 1,", ",\n \"future_flag\": true, \"future_text\": \"a\\\"b\\n\", \"future_nested\": {\"a\": [1, 2.5, \"x\"], \"b\": null}");
    SystemConfig c;
    CHECK(c.load(newer) == ConfigLoadStatus::Ok && c.unknown_fields.size() == 3);
    c.theme = "stage";  // modify a known field
    const std::string out = c.to_json();
    SystemConfig d;
    CHECK(d.load(out) == ConfigLoadStatus::Ok && d.theme == "stage" && d.unknown_fields == c.unknown_fields);
    CHECK(d.to_json() == out);
    CHECK(out.find("\"future_flag\": true") != std::string::npos && out.find("\"future_nested\": {\"a\":[1,2.5,\"x\"],\"b\":null}") != std::string::npos);
    SystemConfig plain;
    CHECK(plain.to_json().find("future") == std::string::npos);
  }
  std::printf(g_fail ? "config_version_test: %d FAILED\n" : "config_version_test: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
