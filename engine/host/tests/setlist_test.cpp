// Setlist store tests (Story 4.6): round trip and golden bytes, validation, version policy, unknown-field preservation, every command with the
// result checked by re-opening the store, atomicity with crash injection, single writer, new-Live-Set content, cascades, limits, hostile input
// (a deterministic mutation fuzz), the compatibility corpus. No timing assertions.
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "setlist_fixtures.hpp"

namespace fs = std::filesystem;
namespace sl = velora::host::setlist;
namespace ls = velora::host::liveset;
using velora::host::setlist::Status;

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
std::string make_tmp_dir() {
  std::string t = (fs::temp_directory_path() / "velora-setlist-XXXXXX").string();
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
bool has_error(const sl::ValidationReport& r, sl::IssueCode c) {
  for (const sl::Issue& i : r.errors)
    if (i.code == c) return true;
  return false;
}
std::string lib_bytes(const sl::slp::SetlistLibrary& l) {
  std::string b;
  CHECK(sl::serialize(l, b) == Status::kOk);
  return b;
}

struct Rig {
  TmpDir td;
  ls::SeededRandom rng{42};
  std::unique_ptr<sl::SetlistStore> st;
  sl::StoreOptions opt;
  Rig() {
    opt.dir = td.path;
    opt.rng = &rng;
    CHECK(sl::SetlistStore::open(opt, st) == Status::kOk);
  }
  // Re-opens the store from disk (the old one is released first: its writer lock).
  void reopen() {
    st.reset();
    CHECK(sl::SetlistStore::open(opt, st) == Status::kOk);
  }
};

void test_roundtrip_golden_unknown() {
  const std::string golden = slurp(std::string(VELORA_SETLIST_GOLDEN_DIR) + "/setlist_library_fixed.bin");
  CHECK(!golden.empty());
  CHECK(lib_bytes(sfixtures::basic()) == golden);  // deterministic serialisation: the pinned bytes
  sl::LoadResult lr;
  CHECK(sl::parse_library(golden, sl::kFormatVersion, lr) == Status::kOk);
  CHECK(lr.library.setlists_size() == 4 && lr.library.tags_size() == 3 && !lr.migrated);
  CHECK(lib_bytes(lr.library) == golden);  // read -> write is byte for byte

  // Unknown fields from a newer schema survive a rewrite, at every level.
  const std::string newer = lib_bytes(sfixtures::newer_schema());
  CHECK(newer != golden);
  CHECK(sl::parse_library(newer, sl::kFormatVersion, lr) == Status::kOk);
  CHECK(lr.library.GetReflection()->GetUnknownFields(lr.library).field_count() == 2);
  CHECK(lr.library.tags(0).GetReflection()->GetUnknownFields(lr.library.tags(0)).field_count() == 1);
  CHECK(lr.library.setlists(0).GetReflection()->GetUnknownFields(lr.library.setlists(0)).field_count() == 1);
  CHECK(lib_bytes(lr.library) == newer);

  // ... and through the store: a command rewrites the file with the unknown fields still in it.
  TmpDir td;
  spit(td.path + "/" + sl::kLibraryFile, newer);
  std::unique_ptr<sl::SetlistStore> st;
  sl::StoreOptions o;
  o.dir = td.path;
  CHECK(sl::SetlistStore::open(o, st) == Status::kOk);
  CHECK(st->rename_setlist(st->library().setlists(1).id(), "Renamed") == Status::kOk);
  sl::LoadResult after;
  CHECK(sl::parse_library(slurp(td.path + "/" + sl::kLibraryFile), sl::kFormatVersion, after) == Status::kOk);
  CHECK(after.library.GetReflection()->GetUnknownFields(after.library).field_count() == 2);
  CHECK(after.library.tags(0).GetReflection()->GetUnknownFields(after.library.tags(0)).field_count() == 1);
  CHECK(after.library.setlists(0).GetReflection()->GetUnknownFields(after.library.setlists(0)).field_count() == 1);
  CHECK(after.library.setlists(1).name() == "Renamed");
  // ... also a reorder of the setlists (messages move whole).
  std::vector<std::string> ids;
  for (int i = 3; i >= 0; --i) ids.push_back(st->library().setlists(i).id());
  CHECK(st->reorder_setlists(ids) == Status::kOk);
  CHECK(sl::parse_library(slurp(td.path + "/" + sl::kLibraryFile), sl::kFormatVersion, after) == Status::kOk);
  CHECK(after.library.setlists(3).GetReflection()->GetUnknownFields(after.library.setlists(3)).field_count() == 1);
  CHECK(after.library.GetReflection()->GetUnknownFields(after.library).field_count() == 2);
}

void test_version_policy() {
  const std::string v1 = lib_bytes(sfixtures::basic());
  sl::LoadResult lr;
  // A reader at N = 2 accepts a version 1 file (N-1) and migrates it in memory; N = 1 reads its own.
  CHECK(sl::parse_library(v1, 2, lr) == Status::kOk && lr.migrated && lr.file_version == 1 && lr.library.format_version() == 2);
  CHECK(sl::parse_library(v1, 1, lr) == Status::kOk && !lr.migrated);
  CHECK(sl::parse_library(v1, 3, lr) == Status::kUnsupportedVersion);  // N-2
  auto with_version = [&](std::uint32_t v) {
    auto l = sfixtures::basic();
    l.set_format_version(v);
    return lib_bytes(l);
  };
  CHECK(sl::parse_library(with_version(0), 1, lr) == Status::kUnsupportedVersion);  // unset is never valid
  CHECK(sl::parse_library(with_version(2), 1, lr) == Status::kUnsupportedVersion);  // newer than the reader
  CHECK(sl::parse_library(with_version(99), 2, lr) == Status::kUnsupportedVersion);
  sl::AssignResult ar;
  std::string ab;
  CHECK(sl::serialize(sfixtures::assign_factory(), ab) == Status::kOk);
  CHECK(sl::parse_assign(ab, 2, ar) == Status::kOk && ar.file_version == 1);  // N-1
  CHECK(sl::parse_assign(ab, 3, ar) == Status::kUnsupportedVersion);
  // A store at N = 2 opens an N-1 directory and writes N.
  TmpDir td;
  spit(td.path + "/" + sl::kLibraryFile, v1);
  sl::StoreOptions o;
  o.dir = td.path;
  o.current_version = 2;
  std::unique_ptr<sl::SetlistStore> st;
  CHECK(sl::SetlistStore::open(o, st) == Status::kOk);
  std::string id;
  CHECK(st->create_setlist("New", id) == Status::kOk);
  sl::LoadResult after;
  CHECK(sl::parse_library(slurp(td.path + "/" + sl::kLibraryFile), 2, after) == Status::kOk && after.file_version == 2 && !after.migrated);
  // ... and a store at N = 1 refuses that directory now (it is newer), leaving the file alone.
  st.reset();
  const std::string before = slurp(td.path + "/" + sl::kLibraryFile);
  o.current_version = 1;
  CHECK(sl::SetlistStore::open(o, st) == Status::kUnsupportedVersion && !st);
  CHECK(slurp(td.path + "/" + sl::kLibraryFile) == before);
}

void test_validation() {
  using IC = sl::IssueCode;
  auto base = [] { return sfixtures::basic(); };
  CHECK(sl::validate(base()).ok());
  CHECK(sl::validate(sfixtures::empty()).ok());
  auto expect = [&](auto mutate, IC code) {
    auto l = base();
    mutate(l);
    const auto rep = sl::validate(l);
    if (!has_error(rep, code)) std::printf("validation: expected issue %d\n", static_cast<int>(code));
    CHECK(has_error(rep, code));
    std::string bytes;
    // The file path refuses it too, with nothing exposed.
    CHECK(sl::serialize(l, bytes) == Status::kOk);
    sl::LoadResult lr;
    const Status ps = sl::parse_library(bytes, sl::kFormatVersion, lr);
    CHECK(ps == Status::kInvalid || ps == Status::kParseError);  // proto3 refuses malformed UTF-8 while parsing
    if (ps == Status::kInvalid) CHECK(!lr.report.ok());
  };
  expect([](auto& l) { l.mutable_setlists(0)->set_name(""); }, IC::kBadName);
  expect([](auto& l) { l.mutable_setlists(0)->set_name(std::string(65, 'x')); }, IC::kBadName);
  expect([](auto& l) { l.mutable_setlists(0)->set_name("tab\there"); }, IC::kBadName);
  expect([](auto& l) { l.mutable_setlists(0)->set_name(std::string("nul\0x", 5)); }, IC::kBadName);
  expect([](auto& l) { l.mutable_setlists(0)->set_name("del\x7f"); }, IC::kBadName);
  expect([](auto& l) { l.mutable_tags(0)->set_name(""); }, IC::kBadName);
  expect([](auto& l) { l.mutable_tags(0)->set_name(std::string(65, 'y')); }, IC::kBadName);
  expect([](auto& l) { l.mutable_setlists(0)->set_name("\xC0\xAF"); }, IC::kBadName);  // overlong
  expect([](auto& l) { l.mutable_setlists(0)->set_name("\xED\xA0\x80"); }, IC::kBadName);  // surrogate
  expect([](auto& l) { l.mutable_setlists(0)->set_id("not-a-uuid"); }, IC::kBadId);
  expect([](auto& l) { l.mutable_setlists(0)->set_id(l.setlists(0).id().substr(0, 14) + "1" + l.setlists(0).id().substr(15)); }, IC::kBadId);  // not v4
  expect([](auto& l) { l.mutable_tags(1)->set_id("../../etc"); }, IC::kBadId);
  expect([](auto& l) { l.mutable_setlists(1)->set_id(l.setlists(0).id()); }, IC::kDuplicateId);
  expect([](auto& l) { l.mutable_tags(1)->set_id(l.tags(0).id()); }, IC::kDuplicateId);
  expect([](auto& l) { l.mutable_setlists(0)->add_live_set_ids(l.setlists(0).live_set_ids(0)); }, IC::kDuplicateLiveSet);
  expect([](auto& l) { l.mutable_setlists(0)->add_live_set_ids("zzz"); }, IC::kBadLiveSetId);
  expect([](auto& l) { l.mutable_setlists(0)->add_tag_ids(sfixtures::uuid_n(31337)); }, IC::kUnknownTag);
  expect([](auto& l) { l.mutable_setlists(0)->add_tag_ids(l.setlists(0).tag_ids(0)); }, IC::kDuplicateTag);
  expect(
      [](auto& l) {
        while (l.setlists(0).tag_ids_size() < 9) {
          auto* t = l.add_tags();
          t->set_id(sfixtures::uuid_n(8000 + static_cast<std::uint64_t>(l.tags_size())));
          t->set_name("t");
          l.mutable_setlists(0)->add_tag_ids(t->id());
        }
      },
      IC::kTooManyTagsOnSetlist);
  expect(
      [](auto& l) {
        for (int i = 0; l.setlists(0).live_set_ids_size() <= static_cast<int>(sl::kMaxLiveSetsPerSetlist); ++i)
          l.mutable_setlists(0)->add_live_set_ids(sfixtures::uuid_n(20000 + static_cast<std::uint64_t>(i)));
      },
      IC::kTooManyLiveSets);
  expect(
      [](auto& l) {
        for (int i = 0; l.setlists_size() <= static_cast<int>(sl::kMaxSetlists); ++i) sfixtures::add_setlist(l, 30000 + static_cast<std::uint64_t>(i), "s");
      },
      IC::kTooManySetlists);
  expect(
      [](auto& l) {
        for (int i = 0; l.tags_size() <= static_cast<int>(sl::kMaxTags); ++i) sfixtures::add_tag(l, 40000 + static_cast<std::uint64_t>(i), "t");
      },
      IC::kTooManyTags);

  // Default ASSIGN map.
  auto a = sfixtures::assign_factory();
  CHECK(sl::validate(a).ok());
  a.add_assign(0);
  CHECK(has_error(sl::validate(a), IC::kBadAssignCount));
  a = sfixtures::assign_factory();
  a.set_assign(3, 300);  // a structural parameter
  CHECK(has_error(sl::validate(a), IC::kBadAssignParam));
  a.set_assign(3, 60000);  // unknown
  CHECK(has_error(sl::validate(a), IC::kBadAssignParam));
  a.set_assign(3, 0x100000001ull & 0xFFFFFFFFu);
  CHECK(has_error(sl::validate(a), IC::kBadAssignParam));
  a = sfixtures::assign_factory();
  a.mutable_assign()->RemoveLast();
  CHECK(has_error(sl::validate(a), IC::kBadAssignCount));
}

void test_commands_persist() {
  Rig r;
  auto& st = *r.st;
  CHECK(st.library().setlists_size() == 0 && st.library_revision() == 0);
  CHECK(slurp(r.td.path + "/" + sl::kLibraryFile).empty());  // nothing written until the first command
  std::string a, b, c;
  CHECK(st.create_setlist("Alpha", a) == Status::kOk && ls::is_uuid_v4(a));
  CHECK(st.create_setlist("Bravo", b) == Status::kOk);
  CHECK(st.create_setlist("Charlie", c) == Status::kOk);
  CHECK(a != b && b != c && st.library_revision() == 3);
  r.reopen();
  CHECK(r.st->library().setlists_size() == 3 && r.st->library().setlists(1).name() == "Bravo");
  auto& s2 = *r.st;

  CHECK(s2.rename_setlist(b, "Bravo 2") == Status::kOk);
  r.reopen();
  CHECK(r.st->find_setlist(b)->name() == "Bravo 2");

  // Reorder the list: the order is the position.
  CHECK(r.st->reorder_setlists({c, a, b}) == Status::kOk);
  r.reopen();
  CHECK(r.st->library().setlists(0).id() == c && r.st->library().setlists(1).id() == a && r.st->library().setlists(2).id() == b);
  CHECK(r.st->index_of_setlist(a) == 1 && r.st->index_of_setlist("nope") == -1);

  // Live Sets in a setlist: append, insert at an index, remove, reorder.
  const std::string l1 = sfixtures::uuid_n(1), l2 = sfixtures::uuid_n(2), l3 = sfixtures::uuid_n(3), l4 = sfixtures::uuid_n(4);
  CHECK(r.st->add_live_set(a, l1, std::nullopt) == Status::kOk);
  CHECK(r.st->add_live_set(a, l2, std::nullopt) == Status::kOk);
  CHECK(r.st->add_live_set(a, l3, 0) == Status::kOk);  // at the front
  CHECK(r.st->add_live_set(a, l4, 1) == Status::kOk);  // in the middle
  r.reopen();
  {
    const auto& ids = r.st->find_setlist(a)->live_set_ids();
    CHECK(ids.size() == 4 && ids[0] == l3 && ids[1] == l4 && ids[2] == l1 && ids[3] == l2);
  }
  CHECK(r.st->add_live_set(b, l1, std::nullopt) == Status::kOk);  // a Live Set may be in several setlists
  CHECK(r.st->remove_live_set(a, l4) == Status::kOk);
  CHECK(r.st->reorder_live_sets(a, {l2, l1, l3}) == Status::kOk);
  r.reopen();
  {
    const auto& ids = r.st->find_setlist(a)->live_set_ids();
    CHECK(ids.size() == 3 && ids[0] == l2 && ids[1] == l1 && ids[2] == l3);
    CHECK(r.st->find_setlist(b)->live_set_ids_size() == 1);
  }
  // add at the end by index == size
  CHECK(r.st->add_live_set(a, l4, 3) == Status::kOk && r.st->find_setlist(a)->live_set_ids(3) == l4);

  // Tags.
  std::string t1, t2;
  CHECK(r.st->create_tag("Weddings", t1) == Status::kOk && r.st->create_tag("Jazz", t2) == Status::kOk);
  CHECK(r.st->rename_tag(t2, "Jazz club") == Status::kOk);
  CHECK(r.st->set_setlist_tags(a, {t1, t2}) == Status::kOk);
  CHECK(r.st->set_setlist_tags(b, {t2}) == Status::kOk);
  r.reopen();
  CHECK(r.st->library().tags_size() == 2 && r.st->find_tag(t2)->name() == "Jazz club");
  CHECK(r.st->find_setlist(a)->tag_ids_size() == 2 && r.st->find_setlist(b)->tag_ids(0) == t2);
  // Replacing the set removes the old ones.
  CHECK(r.st->set_setlist_tags(a, {t1}) == Status::kOk && r.st->find_setlist(a)->tag_ids_size() == 1);
  CHECK(r.st->set_setlist_tags(a, {t1, t2}) == Status::kOk);
  // Deleting a tag removes it from all setlists.
  CHECK(r.st->delete_tag(t2) == Status::kOk);
  r.reopen();
  CHECK(r.st->find_tag(t2) == nullptr && r.st->library().tags_size() == 1);
  CHECK(r.st->find_setlist(a)->tag_ids_size() == 1 && r.st->find_setlist(a)->tag_ids(0) == t1 && r.st->find_setlist(b)->tag_ids_size() == 0);

  // Deleting a setlist leaves the other setlists (and every Live Set) alone.
  CHECK(r.st->delete_setlist(c) == Status::kOk);
  r.reopen();
  CHECK(r.st->library().setlists_size() == 2 && r.st->find_setlist(c) == nullptr && r.st->find_setlist(b)->live_set_ids_size() == 1);

  // A Live Set removed from every setlist at once.
  std::size_t from = 99;
  CHECK(r.st->remove_live_set_everywhere(l1, from) == Status::kOk && from == 2);
  CHECK(r.st->find_setlist(a)->live_set_ids_size() == 3 && r.st->find_setlist(b)->live_set_ids_size() == 0);
  const std::uint64_t rev = r.st->library_revision();
  const std::string file_before = slurp(r.td.path + "/" + sl::kLibraryFile);
  CHECK(r.st->remove_live_set_everywhere(sfixtures::uuid_n(777), from) == Status::kOk && from == 0);  // in none: no write at all
  CHECK(r.st->library_revision() == rev && slurp(r.td.path + "/" + sl::kLibraryFile) == file_before);
}

void test_refusals_change_nothing() {
  Rig r;
  auto& st = *r.st;
  std::string a, b, t;
  CHECK(st.create_setlist("A", a) == Status::kOk && st.create_setlist("B", b) == Status::kOk && st.create_tag("T", t) == Status::kOk);
  const std::string l1 = sfixtures::uuid_n(1), l2 = sfixtures::uuid_n(2);
  CHECK(st.add_live_set(a, l1, std::nullopt) == Status::kOk && st.add_live_set(a, l2, std::nullopt) == Status::kOk);
  const std::string file = slurp(r.td.path + "/" + sl::kLibraryFile);
  const std::uint64_t rev = st.library_revision();
  std::string id;
  auto same = [&] { return slurp(r.td.path + "/" + sl::kLibraryFile) == file && st.library_revision() == rev; };

  CHECK(st.create_setlist("", id) == Status::kInvalidName && id.empty());
  CHECK(st.create_setlist(std::string(65, 'n'), id) == Status::kInvalidName);
  CHECK(st.create_setlist("bad\nname", id) == Status::kInvalidName);
  CHECK(st.create_setlist("\xFF", id) == Status::kInvalidName);
  CHECK(st.create_setlist(std::string(64, 'n'), id) == Status::kOk);  // exactly 64 bytes is fine
  // (that one was applied: rebuild the baseline)
  CHECK(st.delete_setlist(id) == Status::kOk);
  const std::string file2 = slurp(r.td.path + "/" + sl::kLibraryFile);
  const std::uint64_t rev2 = st.library_revision();
  auto same2 = [&] { return slurp(r.td.path + "/" + sl::kLibraryFile) == file2 && st.library_revision() == rev2; };
  (void)same;
  CHECK(st.rename_setlist("nope", "x") == Status::kNoSuchSetlist);
  CHECK(st.rename_setlist(a, "") == Status::kInvalidName);
  CHECK(st.delete_setlist("nope") == Status::kNoSuchSetlist);
  CHECK(st.reorder_setlists({a}) == Status::kInvalidOrder);            // too short
  CHECK(st.reorder_setlists({a, a}) == Status::kInvalidOrder);         // duplicate
  CHECK(st.reorder_setlists({a, "nope"}) == Status::kInvalidOrder);    // unknown
  CHECK(st.reorder_setlists({a, b, b}) == Status::kInvalidOrder);
  CHECK(st.add_live_set("nope", l1, std::nullopt) == Status::kNoSuchSetlist);
  CHECK(st.add_live_set(a, "x", std::nullopt) == Status::kBadId);
  CHECK(st.add_live_set(a, l1, std::nullopt) == Status::kDuplicateLiveSet);
  CHECK(st.add_live_set(a, sfixtures::uuid_n(9), 3) == Status::kInvalidOrder);  // index past the end
  CHECK(st.remove_live_set(a, sfixtures::uuid_n(9)) == Status::kNotInSetlist);
  CHECK(st.remove_live_set("nope", l1) == Status::kNoSuchSetlist);
  CHECK(st.reorder_live_sets(a, {l1}) == Status::kInvalidOrder);
  CHECK(st.reorder_live_sets(a, {l1, l1}) == Status::kInvalidOrder);
  CHECK(st.reorder_live_sets(a, {l1, sfixtures::uuid_n(9)}) == Status::kInvalidOrder);
  CHECK(st.reorder_live_sets("nope", {}) == Status::kNoSuchSetlist);
  CHECK(st.create_tag("", id) == Status::kInvalidName);
  CHECK(st.rename_tag("nope", "x") == Status::kNoSuchTag);
  CHECK(st.rename_tag(t, "") == Status::kInvalidName);
  CHECK(st.delete_tag("nope") == Status::kNoSuchTag);
  CHECK(st.set_setlist_tags("nope", {}) == Status::kNoSuchSetlist);
  CHECK(st.set_setlist_tags(a, {sfixtures::uuid_n(5)}) == Status::kNoSuchTag);
  CHECK(st.set_setlist_tags(a, {t, t}) == Status::kInvalidOrder);
  CHECK(same2());  // not one refusal wrote anything or moved the revision

  // Limits.
  std::vector<std::string> many;
  for (std::size_t i = 0; i < sl::kMaxTagsPerSetlist; ++i) {
    std::string x;
    CHECK(st.create_tag("t" + std::to_string(i), x) == Status::kOk);
    many.push_back(x);
  }
  CHECK(st.set_setlist_tags(a, many) == Status::kOk);
  many.push_back(t);
  CHECK(st.set_setlist_tags(a, many) == Status::kLimitReached);
  while (static_cast<std::size_t>(st.library().tags_size()) < sl::kMaxTags) {
    std::string x;
    CHECK(st.create_tag("more", x) == Status::kOk);
  }
  CHECK(st.create_tag("one too many", id) == Status::kLimitReached);
  while (static_cast<std::size_t>(st.library().setlists_size()) < sl::kMaxSetlists) CHECK(st.create_setlist("s", id) == Status::kOk);
  CHECK(st.create_setlist("one too many", id) == Status::kLimitReached);
  for (std::size_t i = st.find_setlist(b)->live_set_ids_size(); i < sl::kMaxLiveSetsPerSetlist; ++i)
    CHECK(st.add_live_set(b, sfixtures::uuid_n(100000 + i), std::nullopt) == Status::kOk);
  CHECK(st.add_live_set(b, sfixtures::uuid_n(1), std::nullopt) == Status::kLimitReached);
  // The biggest legal library stays under the file cap and reopens.
  CHECK(slurp(r.td.path + "/" + sl::kLibraryFile).size() < sl::kMaxFileBytes);
  r.reopen();
  CHECK(static_cast<std::size_t>(r.st->library().setlists_size()) == sl::kMaxSetlists);
}

void test_failures_leave_state() {
  // A random source that fails: no id is invented.
  struct Dead : ls::RandomSource {
    bool fill(std::uint8_t*, std::size_t) override { return false; }
  } dead;
  TmpDir td;
  sl::StoreOptions o;
  o.dir = td.path;
  o.rng = &dead;
  std::unique_ptr<sl::SetlistStore> st;
  CHECK(sl::SetlistStore::open(o, st) == Status::kOk);
  std::string id;
  CHECK(st->create_setlist("x", id) == Status::kRandomFailed && id.empty() && st->library().setlists_size() == 0);
  CHECK(st->create_tag("x", id) == Status::kRandomFailed);
  CHECK(!fs::exists(td.path + "/" + sl::kLibraryFile));

  // A directory sync failure: the new file is in place and the state is adopted, reported as committed-not-durable.
  Rig r;
  r.opt.inject_dir_sync_failure = true;
  r.st.reset();
  CHECK(sl::SetlistStore::open(r.opt, r.st) == Status::kOk);
  CHECK(r.st->create_setlist("durable?", id) == Status::kCommittedNotDurable && r.st->library().setlists_size() == 1 && !id.empty());
  r.opt.inject_dir_sync_failure = false;
  r.reopen();
  CHECK(r.st->library().setlists_size() == 1);

  // A fault injected before the rename: nothing adopted, the file as it was.
  Rig q;
  CHECK(q.st->create_setlist("kept", id) == Status::kOk);
  const std::string before = slurp(q.td.path + "/" + sl::kLibraryFile);
  q.st.reset();
  q.opt.on_stage = [](ls::Stage s) { return s == ls::Stage::kFsynced ? ls::HookAction::kAbort : ls::HookAction::kContinue; };
  CHECK(sl::SetlistStore::open(q.opt, q.st) == Status::kOk);
  CHECK(q.st->create_setlist("lost", id) == Status::kFaultInjected && id.empty());
  CHECK(q.st->library().setlists_size() == 1 && q.st->library_revision() == 0 /* revision counts commands since open */);
  CHECK(slurp(q.td.path + "/" + sl::kLibraryFile) == before);
  std::size_t removed = 0;
  CHECK(q.st->reconcile(removed) == Status::kOk && removed == 1);
}

int g_abort_at = -1;

void test_crash_atomicity() {
  Rig r;
  std::string a;
  CHECK(r.st->create_setlist("Before", a) == Status::kOk);
  const std::string old_bytes = slurp(r.td.path + "/" + sl::kLibraryFile);
  // What the file will be after renaming A (computed on a scratch store).
  std::string new_bytes;
  {
    auto lib = r.st->library();
    lib.mutable_setlists(0)->set_name("After");
    new_bytes = lib_bytes(lib);
  }
  r.st.reset();
  int stages = 0;
  r.opt.on_stage = [&](ls::Stage s) {
    ++stages;
    if (g_abort_at >= 0 && static_cast<int>(s) == g_abort_at) _exit(137);  // a simulated crash: no cleanup runs
    return ls::HookAction::kContinue;
  };
  CHECK(sl::SetlistStore::open(r.opt, r.st) == Status::kOk);
  for (int stage = 0; stage < 4; ++stage) {
    spit(r.td.path + "/" + sl::kLibraryFile, old_bytes);
    const pid_t pid = fork();
    if (pid == 0) {
      g_abort_at = stage;
      _exit(r.st->rename_setlist(a, "After") == Status::kOk ? 0 : 1);
    }
    int ws = 0;
    CHECK(waitpid(pid, &ws, 0) == pid && WIFEXITED(ws) && WEXITSTATUS(ws) == 137);
    const std::string now = slurp(r.td.path + "/" + sl::kLibraryFile);
    sl::LoadResult lr;
    CHECK(sl::parse_library(now, sl::kFormatVersion, lr) == Status::kOk);  // never a partial file
    if (stage <= static_cast<int>(ls::Stage::kFsynced)) {
      CHECK(now == old_bytes);
      CHECK(fs::exists(r.td.path + "/" + sl::kLibraryTmpFile));
    } else {
      CHECK(now == new_bytes);
    }
    std::size_t removed = 0;
    CHECK(r.st->reconcile(removed) == Status::kOk);
    CHECK(removed == (stage <= static_cast<int>(ls::Stage::kFsynced) ? 1u : 0u));
    // The parent's in-memory state never saw the child's command, and a fresh store reads whichever file is there.
    std::unique_ptr<sl::SetlistStore> fresh;
    r.st.reset();
    CHECK(sl::SetlistStore::open(r.opt, fresh) == Status::kOk);
    CHECK(fresh->library().setlists(0).name() == (stage <= static_cast<int>(ls::Stage::kFsynced) ? "Before" : "After"));
    fresh.reset();
    CHECK(sl::SetlistStore::open(r.opt, r.st) == Status::kOk);
  }
  // The same for the default ASSIGN map file and the Live Set creation/deletion commands' library part.
  std::array<std::uint32_t, 8> custom = {100, 0, 0, 0, 0, 0, 0, 802};
  const std::string old_assign = slurp(r.td.path + "/" + sl::kAssignFile);  // absent until the first set
  CHECK(old_assign.empty());
  for (int stage = 0; stage < 4; ++stage) {
    const pid_t pid = fork();
    if (pid == 0) {
      g_abort_at = stage;
      _exit(r.st->set_default_assign(custom) == Status::kOk ? 0 : 1);
    }
    int ws = 0;
    CHECK(waitpid(pid, &ws, 0) == pid && WEXITSTATUS(ws) == 137);
    std::unique_ptr<sl::SetlistStore> fresh;
    r.st.reset();
    CHECK(sl::SetlistStore::open(r.opt, fresh) == Status::kOk);  // whatever is on disk reads cleanly
    CHECK(fresh->default_assign() == (stage <= static_cast<int>(ls::Stage::kFsynced) ? sl::factory_default_assign() : custom));
    std::size_t removed = 0;
    CHECK(fresh->reconcile(removed) == Status::kOk);
    fresh.reset();
    std::error_code ec;
    fs::remove(r.td.path + "/" + sl::kAssignFile, ec);
    CHECK(sl::SetlistStore::open(r.opt, r.st) == Status::kOk);
  }
}

void test_single_writer() {
  TmpDir td;
  sl::StoreOptions o;
  o.dir = td.path;
  std::unique_ptr<sl::SetlistStore> a, b;
  CHECK(sl::SetlistStore::open(o, a) == Status::kOk);
  CHECK(sl::SetlistStore::open(o, b) == Status::kWriterBusy && !b);  // even in the same process
  // Commands from any other thread are refused and change nothing.
  std::string id;
  Status s1 = Status::kOk, s2 = Status::kOk, s3 = Status::kOk, s4 = Status::kOk;
  std::size_t removed = 0;
  std::thread t([&] {
    s1 = a->create_setlist("x", id);
    s2 = a->rename_setlist("x", "y");
    s3 = a->set_default_assign(sl::factory_default_assign());
    s4 = a->reconcile(removed);
  });
  t.join();
  CHECK(s1 == Status::kNotControlThread && s2 == Status::kNotControlThread && s3 == Status::kNotControlThread && s4 == Status::kNotControlThread);
  CHECK(a->library().setlists_size() == 0 && !fs::exists(td.path + "/" + sl::kLibraryFile));
  // Explicit handover.
  Status s5 = Status::kIo;
  std::thread t2([&] {
    a->rebind_control_thread();
    s5 = a->create_setlist("from the new owner", id);
  });
  t2.join();
  CHECK(s5 == Status::kOk);
  CHECK(a->create_setlist("old thread", id) == Status::kNotControlThread);  // the first thread no longer owns it
  a->rebind_control_thread();
  CHECK(a->create_setlist("back", id) == Status::kOk);
  // The lock is released with the store.
  a.reset();
  CHECK(sl::SetlistStore::open(o, b) == Status::kOk && b->library().setlists_size() == 2);
  // The directory must exist.
  sl::StoreOptions bad;
  bad.dir = td.path + "/missing";
  std::unique_ptr<sl::SetlistStore> c;
  CHECK(sl::SetlistStore::open(bad, c) == Status::kNotFound);
}

void test_default_assign_and_new_live_set() {
  TmpDir ltd;
  Rig r;
  ls::StoreOptions lo;
  lo.dir = ltd.path;
  std::unique_ptr<ls::LiveSetStore> live;
  CHECK(ls::LiveSetStore::open(lo, live) == ls::Status::kOk);
  auto& st = *r.st;
  // The factory default is valid and is what a fresh install copies.
  CHECK(st.default_assign() == sl::factory_default_assign());
  std::string id1;
  CHECK(st.create_live_set(*live, "First", id1) == Status::kOk && ls::is_uuid_v4(id1));
  ls::LoadResult lr;
  CHECK(live->read(id1, lr) == ls::Status::kOk);  // validates through the Live Set store
  CHECK(lr.live_set.name() == "First" && lr.live_set.parts_size() == 8 && lr.live_set.scenes_size() == 1);
  for (const auto& p : lr.live_set.parts()) {
    CHECK(!p.has_timbre());  // empty parts
    CHECK(p.keyboard_on() && p.low_key() == 0 && p.high_key() == 127);
  }
  CHECK(lr.live_set.assign_size() == 8);
  for (int k = 0; k < 8; ++k) CHECK(lr.live_set.assign(k) == sl::factory_default_assign()[static_cast<std::size_t>(k)]);
  CHECK(lr.live_set.scenes(0).name() == "Scene 1" && lr.live_set.scenes(0).parts_size() == 0);
  CHECK(lr.live_set.tempo() == 120.0f && lr.live_set.format_version() == ls::kFormatVersion);

  // Change the default: existing Live Sets are untouched, new ones copy the new map.
  const std::string first_bytes = slurp(live->path_for(id1));
  const std::array<std::uint32_t, 8> custom = {100, 102, 0, 200, 0, 802, 401, 400};
  CHECK(st.set_default_assign(custom) == Status::kOk && st.default_assign() == custom);
  CHECK(slurp(live->path_for(id1)) == first_bytes);
  std::string id2;
  CHECK(st.create_live_set(*live, "Second", id2) == Status::kOk && id2 != id1);
  CHECK(live->read(id2, lr) == ls::Status::kOk);
  for (int k = 0; k < 8; ++k) CHECK(lr.live_set.assign(k) == custom[static_cast<std::size_t>(k)]);
  // Persistence of the default across a reopen; refusals leave it.
  r.reopen();
  CHECK(r.st->default_assign() == custom);
  std::array<std::uint32_t, 8> bad = custom;
  bad[2] = 300;  // structural
  const std::string assign_file = slurp(r.td.path + "/" + sl::kAssignFile);
  CHECK(r.st->set_default_assign(bad) == Status::kInvalid && r.st->default_assign() == custom);
  CHECK(slurp(r.td.path + "/" + sl::kAssignFile) == assign_file);
  // A damaged default file fails the open and is left untouched.
  spit(r.td.path + "/" + sl::kAssignFile, "garbage\xFF");
  r.st.reset();
  CHECK(sl::SetlistStore::open(r.opt, r.st) == Status::kParseError && !r.st);
  CHECK(slurp(r.td.path + "/" + sl::kAssignFile) == "garbage\xFF");

  // Names are validated before anything is created.
  Rig r2;
  std::string bad_id;
  CHECK(r2.st->create_live_set(*live, "", bad_id) == Status::kInvalidName && bad_id.empty());
  std::size_t files = 0;
  for (const auto& e : fs::directory_iterator(ltd.path))
    if (e.path().extension() == ".liveset") ++files;
  CHECK(files == 2);
  // The Live Set store's single-writer rule applies to creation too.
  Status s = Status::kOk;
  std::string tid;
  std::thread t([&] { s = r2.st->create_live_set(*live, "x", tid); });
  t.join();
  CHECK(s == Status::kNotControlThread);
}

void test_live_set_delete_cascade() {
  TmpDir ltd;
  Rig r;
  ls::StoreOptions lo;
  lo.dir = ltd.path;
  std::unique_ptr<ls::LiveSetStore> live;
  CHECK(ls::LiveSetStore::open(lo, live) == ls::Status::kOk);
  auto& st = *r.st;
  std::string l1, l2, s1, s2;
  CHECK(st.create_live_set(*live, "One", l1) == Status::kOk && st.create_live_set(*live, "Two", l2) == Status::kOk);
  CHECK(st.create_setlist("S1", s1) == Status::kOk && st.create_setlist("S2", s2) == Status::kOk);
  CHECK(st.add_live_set(s1, l1, std::nullopt) == Status::kOk && st.add_live_set(s1, l2, std::nullopt) == Status::kOk);
  CHECK(st.add_live_set(s2, l1, std::nullopt) == Status::kOk);
  // Deleting a setlist never deletes Live Sets.
  CHECK(st.delete_setlist(s2) == Status::kOk);
  CHECK(fs::exists(live->path_for(l1)) && fs::exists(live->path_for(l2)));
  CHECK(st.create_setlist("S2", s2) == Status::kOk && st.add_live_set(s2, l1, std::nullopt) == Status::kOk);
  // Deleting a Live Set removes it from every setlist and removes the file.
  std::size_t from = 0;
  CHECK(st.delete_live_set(*live, l1, &from) == Status::kOk && from == 2);
  CHECK(!fs::exists(live->path_for(l1)) && fs::exists(live->path_for(l2)));
  r.reopen();
  CHECK(r.st->find_setlist(s1)->live_set_ids_size() == 1 && r.st->find_setlist(s1)->live_set_ids(0) == l2);
  CHECK(r.st->find_setlist(s2)->live_set_ids_size() == 0);
  // Deleting it again: the file is gone (kNotFound) and nothing else changes.
  CHECK(r.st->delete_live_set(*live, l1, &from) == Status::kNotFound && from == 0);
  CHECK(r.st->delete_live_set(*live, "junk", &from) == Status::kBadId);
  // A reference to a Live Set whose file vanished is kept (and listed as missing by the controller), and deleting cleans it.
  CHECK(r.st->add_live_set(s1, sfixtures::uuid_n(555), std::nullopt) == Status::kOk);
  r.reopen();
  CHECK(r.st->find_setlist(s1)->live_set_ids_size() == 2);
  CHECK(r.st->delete_live_set(*live, sfixtures::uuid_n(555), &from) == Status::kNotFound && from == 1);
  CHECK(r.st->find_setlist(s1)->live_set_ids_size() == 1);
  // The Live Set store's remove is single-writer too.
  Status s = Status::kOk;
  std::thread t([&] { s = live->remove(l2) == ls::Status::kNotControlThread ? Status::kNotControlThread : Status::kOk; });
  t.join();
  CHECK(s == Status::kNotControlThread && fs::exists(live->path_for(l2)));
}

void test_hostile_files() {
  // A library file that cannot be read fails the open and is never overwritten.
  const std::string good = lib_bytes(sfixtures::basic());
  const std::string cases[] = {"", "\xFF\xFF\xFF\xFF\xFF", "not protobuf at all", std::string(10, '\0'), good.substr(0, good.size() / 2)};
  for (const std::string& c : cases) {
    TmpDir td;
    spit(td.path + "/" + sl::kLibraryFile, c);
    sl::StoreOptions o;
    o.dir = td.path;
    std::unique_ptr<sl::SetlistStore> st;
    const Status s = sl::SetlistStore::open(o, st);
    // The empty file parses as an empty message: format_version 0, unsupported. Nothing parses as OK.
    CHECK(s != Status::kOk && !st);
    CHECK(slurp(td.path + "/" + sl::kLibraryFile) == c);
  }
  {  // too large
    TmpDir td;
    spit(td.path + "/" + sl::kLibraryFile, std::string(sl::kMaxFileBytes + 1, 'a'));
    sl::StoreOptions o;
    o.dir = td.path;
    std::unique_ptr<sl::SetlistStore> st;
    CHECK(sl::SetlistStore::open(o, st) == Status::kTooLarge);
  }
  {  // a symlink is not followed
    TmpDir td;
    spit(td.path + "/real", good);
    ::symlink((td.path + "/real").c_str(), (td.path + "/" + sl::kLibraryFile).c_str());
    sl::StoreOptions o;
    o.dir = td.path;
    std::unique_ptr<sl::SetlistStore> st;
    CHECK(sl::SetlistStore::open(o, st) != Status::kOk);
  }
  {  // a directory where the file should be
    TmpDir td;
    fs::create_directory(td.path + "/" + sl::kLibraryFile);
    sl::StoreOptions o;
    o.dir = td.path;
    std::unique_ptr<sl::SetlistStore> st;
    CHECK(sl::SetlistStore::open(o, st) != Status::kOk);
  }
  // Truncation at every length and a deterministic byte-mutation fuzz never crash; OK is returned only for something that validates.
  sl::LoadResult lr;
  int ok_count = 0;
  for (std::size_t n = 0; n <= good.size(); ++n) {
    const Status s = sl::parse_library(std::string_view(good).substr(0, n), 1, lr);
    if (s == Status::kOk) {
      ++ok_count;
      CHECK(sl::validate(lr.library).ok());
    }
  }
  CHECK(ok_count >= 1);
  ls::SeededRandom rng(2024);
  for (int iter = 0; iter < 4000; ++iter) {
    std::string m = good;
    std::uint8_t r[4];
    rng.fill(r, 4);
    const int edits = 1 + r[0] % 4;
    for (int e = 0; e < edits; ++e) {
      std::uint8_t p[3];
      rng.fill(p, 3);
      const std::size_t at = (static_cast<std::size_t>(p[0]) << 8 | p[1]) % m.size();
      switch (r[1] % 3) {
        case 0: m[at] = static_cast<char>(p[2]); break;
        case 1: m.erase(at, 1 + p[2] % 4); break;
        default: m.insert(at, 1, static_cast<char>(p[2])); break;
      }
      if (m.empty()) m = "x";
    }
    const Status s = sl::parse_library(m, 1, lr);
    if (s == Status::kOk) CHECK(sl::validate(lr.library).ok());
    sl::AssignResult ar;
    (void)sl::parse_assign(m, 1, ar);
  }
  // Pure garbage of many lengths.
  for (int len = 0; len < 600; len += 7) {
    std::string g(static_cast<std::size_t>(len), '\0');
    rng.fill(reinterpret_cast<std::uint8_t*>(g.data()), g.size());
    (void)sl::parse_library(g, 1, lr);
  }
}

void test_corpus() {
  const std::string dir = VELORA_SETLIST_CORPUS_DIR;
  int libs = 0, assigns = 0;
  for (const auto& e : fs::directory_iterator(dir)) {
    const std::string name = e.path().filename().string();
    const std::string bytes = slurp(e.path().string());
    if (e.path().extension() == ".library") {
      ++libs;
      sl::LoadResult lr;
      const Status s = sl::parse_library(bytes, sl::kFormatVersion, lr);
      if (s != Status::kOk) std::printf("corpus file %s: status %d\n", name.c_str(), static_cast<int>(s));
      CHECK(s == Status::kOk);
      CHECK(lib_bytes(lr.library) == bytes);  // byte-stable under read -> write
      // The store opens it too.
      TmpDir td;
      spit(td.path + "/" + sl::kLibraryFile, bytes);
      sl::StoreOptions o;
      o.dir = td.path;
      std::unique_ptr<sl::SetlistStore> st;
      CHECK(sl::SetlistStore::open(o, st) == Status::kOk);
      CHECK(st->library().setlists_size() == lr.library.setlists_size());
    } else if (e.path().extension() == ".pb") {
      ++assigns;
      sl::AssignResult ar;
      CHECK(sl::parse_assign(bytes, sl::kFormatVersion, ar) == Status::kOk);
      std::string again;
      CHECK(sl::serialize(ar.assign, again) == Status::kOk && again == bytes);
    }
  }
  CHECK(libs >= 3 && assigns >= 2);
  for (const auto& c : sfixtures::corpus()) {
    if (slurp(dir + "/" + c.name) != c.bytes) {
      std::printf("FAIL corpus drift: %s differs from the fixture; run the setlist_corpus_regen target\n", c.name);
      ++g_fail;
    }
  }
}

}  // namespace

int main() {
  test_roundtrip_golden_unknown();
  test_version_policy();
  test_validation();
  test_commands_persist();
  test_refusals_change_nothing();
  test_failures_leave_state();
  test_crash_atomicity();
  test_single_writer();
  test_default_assign_and_new_live_set();
  test_live_set_delete_cascade();
  test_hostile_files();
  test_corpus();
  if (g_fail) {
    std::printf("setlist_tests: %d FAILED\n", g_fail);
    return 1;
  }
  std::printf("setlist_tests: ok\n");
  return 0;
}
