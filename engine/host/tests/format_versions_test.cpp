// Format version policy across the persisted / negotiated forms of engine-host (Story 8.5, FR49, AR29, NFR44): N and N-1 are accepted, older and newer
// are typed refusals with no partial load, unknown fields survive read-modify-write. One section per form; N = 2 (or 3) exercises N-1 where the shipping N is 1.
// The forms owned by other libraries have their own tests (formatlib bundle_test, librarian catalog_test, mcu_frame_test, ui config_version_test); see
// _bmad-output/implementation-artifacts/format-versions-audit-8-5.md.
#include <cstdio>
#include <string>

#include "live_set_fixtures.hpp"
#include "setlist_fixtures.hpp"
#include "velora/host/control_plane.hpp"
#include "velora/host/mcu_frame.hpp"
#include "ws_v1.pb.h"

namespace {
int g_fail = 0;
#define CHECK(c)                                                       \
  do {                                                                 \
    if (!(c)) {                                                        \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);         \
      ++g_fail;                                                        \
    }                                                                  \
  } while (0)

namespace lsn = velora::host::liveset;
namespace sln = velora::host::setlist;

std::size_t unknown_count(const google::protobuf::Message& m) { return m.GetReflection()->GetUnknownFields(m).field_count(); }

void test_live_set() {
  lsn::ReadOptions n2;
  n2.current_version = 2;
  for (std::uint32_t v : {0u, 1u, 2u, 3u, 7u}) {
    velora::liveset::v1::LiveSet l = fixtures::minimal();
    l.set_format_version(v);
    fixtures::unknown_varint(&l, 500, 9);
    std::string bytes;
    CHECK(lsn::serialize(l, bytes) == lsn::Status::kOk);
    lsn::LoadResult lr;
    const lsn::Status s = lsn::parse(bytes, n2, lr);
    if (v == 1 || v == 2) {
      CHECK(s == lsn::Status::kOk && lr.migrated == (v == 1) && lr.live_set.format_version() == 2 && lr.file_version == v);
      // read-modify-write: the unknown field survives, the written version is N
      lr.live_set.set_name("Renamed");
      std::string out;
      CHECK(lsn::serialize(lr.live_set, out) == lsn::Status::kOk);
      lsn::LoadResult again;
      CHECK(lsn::parse(out, n2, again) == lsn::Status::kOk && !again.migrated && unknown_count(again.live_set) == 1 && again.live_set.name() == "Renamed");
    } else {
      CHECK(s == lsn::Status::kUnsupportedVersion);  // typed refusal: callers never adopt `lr` (the stores fail the open and never overwrite the file)
    }
  }
}

void test_setlist() {
  for (std::uint32_t v : {0u, 1u, 2u, 3u}) {
    velora::setlist::v1::SetlistLibrary l = sfixtures::newer_schema();
    l.set_format_version(v);
    std::string bytes;
    CHECK(sln::serialize(l, bytes) == sln::Status::kOk);
    sln::LoadResult lr;
    const sln::Status s = sln::parse_library(bytes, 2, lr);
    if (v == 1 || v == 2) {
      CHECK(s == sln::Status::kOk && lr.migrated == (v == 1) && lr.library.format_version() == 2);
      CHECK(unknown_count(lr.library) == 2 && unknown_count(lr.library.tags(0)) == 1 && unknown_count(lr.library.setlists(0)) == 1);
      std::string out;
      CHECK(sln::serialize(lr.library, out) == sln::Status::kOk);
      sln::LoadResult again;
      CHECK(sln::parse_library(out, 2, again) == sln::Status::kOk && unknown_count(again.library) == 2);
    } else {
      CHECK(s == sln::Status::kUnsupportedVersion);
    }
  }
  for (std::uint32_t v : {0u, 1u, 2u, 3u}) {
    velora::setlist::v1::AssignDefault a = sfixtures::assign_custom();
    a.set_format_version(v);
    fixtures::unknown_varint(&a, 500, 1);
    std::string bytes;
    CHECK(sln::serialize(a, bytes) == sln::Status::kOk);
    sln::AssignResult ar;
    const sln::Status s = sln::parse_assign(bytes, 2, ar);
    if (v == 1 || v == 2) CHECK(s == sln::Status::kOk && unknown_count(ar.assign) == 1);
    else CHECK(s == sln::Status::kUnsupportedVersion);
  }
}

void test_ws_handshake() {
  using velora::host::ProtocolCheck;
  using velora::host::check_protocol_version;
  using velora::host::min_protocol_version;
  CHECK(min_protocol_version(1) == 1 && min_protocol_version(2) == 1 && min_protocol_version(3) == 2);
  // N = 1 (shipping): only 1.
  CHECK(check_protocol_version(1, 1) == ProtocolCheck::kOk);
  CHECK(check_protocol_version(0, 1) == ProtocolCheck::kTooOld);  // PROTOCOL_VERSION_UNSPECIFIED
  CHECK(check_protocol_version(2, 1) == ProtocolCheck::kTooNew);
  // N = 2: 1 (N-1) and 2; 0 too old; 3 too new.
  CHECK(check_protocol_version(1, 2) == ProtocolCheck::kOk && check_protocol_version(2, 2) == ProtocolCheck::kOk);
  CHECK(check_protocol_version(0, 2) == ProtocolCheck::kTooOld && check_protocol_version(3, 2) == ProtocolCheck::kTooNew);
  // N = 3: 1 is two behind.
  CHECK(check_protocol_version(1, 3) == ProtocolCheck::kTooOld && check_protocol_version(2, 3) == ProtocolCheck::kOk);
  CHECK(check_protocol_version(0xFFFFFFFFu, 3) == ProtocolCheck::kTooNew);
  CHECK(velora::host::ControlPlane::kProtocolVersion == static_cast<std::uint32_t>(::ws::v1::PROTOCOL_VERSION_1));
  // Unknown fields of a newer minor survive a parse / serialise round trip (protobuf keeps them) and never break the handshake.
  ::ws::v1::ClientMessage m;
  m.mutable_hello()->set_version(1);
  m.GetReflection()->MutableUnknownFields(&m)->AddVarint(900, 5);
  m.mutable_hello()->GetReflection()->MutableUnknownFields(m.mutable_hello())->AddLengthDelimited(901, "later");
  std::string bytes;
  CHECK(m.SerializeToString(&bytes));
  ::ws::v1::ClientMessage back;
  CHECK(back.ParseFromString(bytes) && back.hello().version() == 1 && unknown_count(back) == 1 && unknown_count(back.hello()) == 1);
  std::string again;
  CHECK(back.SerializeToString(&again) && again == bytes);
}

void test_mcu_protocol() {
  namespace mcu = velora::host::mcu;
  // The decoder of a release at 2.1 accepts 1.x (N-1) and 2.x, rejects major 0 and 3.x (typed kVersionRejected, nothing delivered).
  mcu::StreamDecoder d(0x21);
  std::vector<mcu::Frame> frames;
  std::vector<mcu::EventInfo> events;
  std::uint16_t seq = 0;
  for (std::uint8_t ver : {std::uint8_t{0x10}, std::uint8_t{0x2F}, std::uint8_t{0x0F}, std::uint8_t{0x30}, std::uint8_t{0x21}}) {
    auto f = mcu::make_frame(mcu::Button{1, true}, seq++, 0, ver);
    CHECK(f.has_value());
    auto bytes = mcu::encode(*f);
    CHECK(bytes.has_value());
    d.feed(bytes->data(), bytes->size(), &frames, &events);
  }
  CHECK(frames.size() == 3 && frames[0].version == 0x10 && frames[1].version == 0x2F && frames[2].version == 0x21);
  CHECK(d.counters().version_mismatches == 2);
}

}  // namespace

int main() {
  test_live_set();
  test_setlist();
  test_ws_handshake();
  test_mcu_protocol();
  std::printf(g_fail ? "format_versions_test: %d FAILED\n" : "format_versions_test: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
