// Story 6.9: the signed factory Live Set file (container layout, signature, tamper sweep) and its first-boot install through the single writer (references,
// idempotence, user deletion, user files, newer / older releases, typed errors). The release tool's end-to-end test (tools/factory-release) covers the same file
// made by the real pipeline against installed protected libraries.
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "live_set_fixtures.hpp"
#include "velora/format/protected_writer.hpp"
#include "velora/host/factory_live_set.hpp"
#include "velora/host/live_set_store.hpp"
#include "velora/host/trust_list.hpp"

namespace fs = std::filesystem;
namespace hf = velora::host::factory;
namespace ls = velora::host::liveset;
namespace lsp = velora::liveset::v1;
namespace fmt = velora::format;

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
    char tpl[] = "/tmp/vfl.XXXXXX";
    const char* d = ::mkdtemp(tpl);
    path = d ? d : "";
  }
  ~TmpDir() {
    std::error_code ec;
    if (!path.empty()) fs::remove_all(path, ec);
  }
};

struct Signer {
  fmt::SigningSeed seed;
  fmt::PublicKey pub{};
  fmt::KeyId id{};
  fmt::TrustedKeys trusted;
  explicit Signer(std::uint8_t tag) {
    for (int i = 0; i < 32; ++i) seed.bytes[i] = static_cast<std::uint8_t>(tag * 17 + 5 * i + 3);
    pub = fmt::ed25519_public_from_seed(seed);
    id = fmt::key_id_of_public(pub);
    trusted = fmt::TrustedKeys({fmt::TrustedKey{id, pub}});
  }
};

const std::string kLibA = "00000000-0000-4000-8000-0000000000a1";
const std::string kLibB = "00000000-0000-4000-8000-0000000000b2";

lsp::LiveSet make_ls(std::uint32_t program, const std::string& version, const std::string& name, bool with_timbre = true, const std::string& lib = kLibA,
                     const std::string& key = "piano.sfz") {
  lsp::LiveSet l = ls::make_default_live_set(hf::derive_live_set_id(program), name, 8);
  if (with_timbre) fixtures::timbre(l.mutable_parts(0), lib, key);
  lsp::Part* organ = l.mutable_parts(1);
  organ->mutable_engine()->set_engine_type_id(2);
  fixtures::entry(organ->mutable_engine()->mutable_params(), 100, 8);
  l.mutable_factory()->set_program_id(program);
  l.mutable_factory()->set_release_version(version);
  return l;
}

lsp::FactoryLiveSets make_sets(const std::string& version, std::vector<lsp::LiveSet> sets) {
  lsp::FactoryLiveSets s;
  s.set_format_version(1);
  s.set_release_name("Test release");
  s.set_release_version(version);
  s.set_root("dev");
  for (auto& l : sets) {
    auto* e = s.add_entries();
    e->set_program_id(l.factory().program_id());
    *e->mutable_live_set() = l;
  }
  return s;
}

std::string sign_file(const Signer& sg, const lsp::FactoryLiveSets& s) {
  std::string payload;
  CHECK(hf::serialize_payload(s, payload));
  const auto stmt = hf::signing_statement(sg.id, payload);
  auto sig = fmt::ed25519_sign(sg.seed, stmt);
  CHECK(static_cast<bool>(sig));
  return hf::assemble_container(sg.id, sig.value(), payload);
}

const hf::ReferenceResolver kAllResolve = [](const std::string&, const std::string&) { return true; };

hf::InstallOptions opts(const Signer& sg, hf::ReferenceResolver r = kAllResolve) {
  hf::InstallOptions o;
  o.trusted = &sg.trusted;
  o.resolves = std::move(r);
  return o;
}

std::unique_ptr<ls::LiveSetStore> open_store(const std::string& dir) {
  ls::StoreOptions so;
  so.dir = dir;
  std::unique_ptr<ls::LiveSetStore> st;
  CHECK(ls::LiveSetStore::open(so, st) == ls::Status::kOk);
  return st;
}

std::string slurp(const std::string& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::size_t liveset_files(const std::string& dir) {
  std::size_t n = 0;
  for (const auto& e : fs::directory_iterator(dir))
    if (e.path().extension() == ".liveset") ++n;
  return n;
}

// ---- container ----------------------------------------------------------------------------------------------------------------------------------

void test_ids_and_versions() {
  // The id is a function of the program id alone, UUIDv4-shaped, and pinned (a change would orphan every installed factory Live Set).
  CHECK(hf::derive_live_set_id(1) == hf::derive_live_set_id(1));
  CHECK(hf::derive_live_set_id(1) != hf::derive_live_set_id(2));
  for (std::uint32_t p : {1u, 2u, 7u, 100u, 4294967295u}) CHECK(ls::is_uuid_v4(hf::derive_live_set_id(p)));
  CHECK(hf::derive_live_set_id(1) == "942040ec-e6ba-4187-9883-9c5646779e79" && hf::derive_live_set_id(2) == "aed472df-8201-431e-bed9-b377754e2497" && hf::derive_live_set_id(100) == "8b97746e-bf3f-4fe5-ae82-60a80ea0939d");  // computed independently with Python hashlib
  CHECK(hf::valid_release_version("1.0.0") && hf::valid_release_version("0.12.345") && hf::valid_release_version("10.0.1"));
  for (const char* bad : {"", "1", "1.0", "1.0.0.0", "01.0.0", "1.00.0", "1.0.0-rc1", "a.b.c", "1..0", " 1.0.0", "1.0.0 ", "1.0.-1", "1234567890.0.0"})
    CHECK(!hf::valid_release_version(bad));
  CHECK(hf::compare_release_versions("1.0.0", "1.0.0") == 0);
  CHECK(hf::compare_release_versions("1.0.0", "1.0.1") < 0 && hf::compare_release_versions("1.1.0", "1.0.9") > 0 && hf::compare_release_versions("2.0.0", "10.0.0") < 0);
}

void test_container_layout() {
  Signer sg(1);
  const lsp::FactoryLiveSets sets = make_sets("1.2.3", {make_ls(1, "1.2.3", "One"), make_ls(2, "1.2.3", "Two")});
  const std::string f = sign_file(sg, sets);
  std::string payload;
  CHECK(hf::serialize_payload(sets, payload));
  // The documented layout, byte for byte.
  CHECK(f.size() == hf::kHeaderBytes + payload.size() && hf::kHeaderBytes == 84);
  CHECK(std::memcmp(f.data(), "VFLS", 4) == 0);
  CHECK(static_cast<std::uint8_t>(f[4]) == 1 && f[5] == 0 && f[6] == 0 && f[7] == 0);
  CHECK(std::memcmp(f.data() + 8, sg.id.data(), 8) == 0);
  const std::uint32_t n = static_cast<std::uint8_t>(f[80]) | static_cast<std::uint8_t>(f[81]) << 8 | static_cast<std::uint8_t>(f[82]) << 16 | static_cast<std::uint32_t>(static_cast<std::uint8_t>(f[83])) << 24;
  CHECK(n == payload.size());
  CHECK(f.compare(84, std::string::npos, payload) == 0);
  // The signature covers "velora/factory-live-sets/v1" 0x00 || key id || payload length LE || payload.
  std::vector<std::uint8_t> msg;
  const std::string dom = "velora/factory-live-sets/v1";
  msg.insert(msg.end(), dom.begin(), dom.end());
  msg.push_back(0);
  msg.insert(msg.end(), sg.id.begin(), sg.id.end());
  for (int i = 0; i < 4; ++i) msg.push_back(static_cast<std::uint8_t>((n >> (8 * i)) & 0xff));
  msg.insert(msg.end(), payload.begin(), payload.end());
  CHECK(msg == hf::signing_statement(sg.id, payload));
  fmt::Signature sig{};
  std::memcpy(sig.data(), f.data() + 16, 64);
  CHECK(fmt::ed25519_verify_strict(sg.pub, msg, sig));
  // Deterministic: the same inputs give the same bytes (Ed25519 is deterministic, the serialisation too).
  CHECK(sign_file(sg, sets) == f);
  // Verifies and round-trips.
  lsp::FactoryLiveSets back;
  fmt::KeyId who{};
  CHECK(hf::verify_container(f, sg.trusted, back, &who) == hf::ContainerStatus::kOk && who == sg.id);
  CHECK(back.entries_size() == 2 && back.release_version() == "1.2.3" && back.entries(1).live_set().name() == "Two");
}

void test_container_tamper_sweep() {
  Signer sg(1), other(2);
  const std::string f = sign_file(sg, make_sets("1.0.0", {make_ls(1, "1.0.0", "One")}));
  lsp::FactoryLiveSets out;
  // Every single-bit flip of every byte is refused (a flip in the key id is another / unknown signer; in the signature or payload an invalid signature or payload).
  std::size_t checked = 0;
  for (std::size_t i = 0; i < f.size(); ++i) {
    for (int bit : {0, 5}) {
      std::string t = f;
      t[i] = static_cast<char>(t[i] ^ (1 << bit));
      const hf::ContainerStatus s = hf::verify_container(t, sg.trusted, out);
      CHECK(s != hf::ContainerStatus::kOk);
      if (i < 4) CHECK(s == hf::ContainerStatus::kBadMagic);
      else if (i < 8) CHECK(s == hf::ContainerStatus::kUnsupportedVersion);
      else if (i < 16) CHECK(s == hf::ContainerStatus::kUntrustedKey);
      else if (i < 80) CHECK(s == hf::ContainerStatus::kSignatureInvalid);
      else if (i < 84) CHECK(s == hf::ContainerStatus::kTruncated || s == hf::ContainerStatus::kBadLength || s == hf::ContainerStatus::kTooLarge);
      else CHECK(s == hf::ContainerStatus::kSignatureInvalid);
      ++checked;
    }
  }
  CHECK(checked == f.size() * 2);
  CHECK(hf::verify_container(f.substr(0, 83), sg.trusted, out) == hf::ContainerStatus::kTruncated);
  CHECK(hf::verify_container(f.substr(0, f.size() - 1), sg.trusted, out) == hf::ContainerStatus::kTruncated);
  CHECK(hf::verify_container(f + "x", sg.trusted, out) == hf::ContainerStatus::kBadLength);
  CHECK(hf::verify_container("", sg.trusted, out) == hf::ContainerStatus::kTruncated);
  CHECK(hf::verify_container(f, other.trusted, out) == hf::ContainerStatus::kUntrustedKey);
  CHECK(hf::verify_container(f, fmt::TrustedKeys(), out) == hf::ContainerStatus::kUntrustedKey);
  // A declared length above the limit is refused before anything is read.
  std::string big = f;
  big[83] = static_cast<char>(0x7f);
  CHECK(hf::verify_container(big, sg.trusted, out) == hf::ContainerStatus::kTooLarge);
  // A file signed over garbage that is not a FactoryLiveSets message: valid signature, bad payload.
  {
    const std::string junk = "\xff\xff\xff\xff not a protobuf";
    const auto stmt = hf::signing_statement(sg.id, junk);
    auto sig = fmt::ed25519_sign(sg.seed, stmt);
    CHECK(hf::verify_container(hf::assemble_container(sg.id, sig.value(), junk), sg.trusted, out) == hf::ContainerStatus::kBadPayload);
  }
  // The same payload signed under another domain (a Live Set signature or a release index signature) is not accepted.
  {
    std::string payload;
    CHECK(hf::serialize_payload(make_sets("1.0.0", {make_ls(1, "1.0.0", "One")}), payload));
    std::vector<std::uint8_t> wrong(payload.begin(), payload.end());
    auto sig = fmt::ed25519_sign(sg.seed, wrong);
    CHECK(hf::verify_container(hf::assemble_container(sg.id, sig.value(), payload), sg.trusted, out) == hf::ContainerStatus::kSignatureInvalid);
  }
}

// ---- install ------------------------------------------------------------------------------------------------------------------------------------

void test_install_happy_and_idempotent() {
  TmpDir t;
  Signer sg(1);
  auto st = open_store(t.path);
  const std::string f = sign_file(sg, make_sets("1.0.0", {make_ls(1, "1.0.0", "Grand"), make_ls(2, "1.0.0", "Organ Gospel", false)}));
  hf::InstallResult r = hf::install_factory_live_sets(*st, f, opts(sg));
  CHECK(r.ok() && r.imported == std::vector<std::uint32_t>({1, 2}) && r.updated.empty() && r.unchanged.empty() && r.conflicts.empty());
  CHECK(r.release_version == "1.0.0" && r.release_name == "Test release");
  CHECK(liveset_files(t.path) == 2);
  CHECK(fs::exists(t.path + "/" + hf::kStateFileName));
  // The files are the signed Live Sets, marked factory, readable through the store like any Live Set.
  ls::LoadResult lr;
  CHECK(st->read(hf::derive_live_set_id(1), lr) == ls::Status::kOk && hf::is_factory(lr.live_set) && lr.live_set.name() == "Grand" && lr.live_set.factory().program_id() == 1 &&
        lr.live_set.factory().release_version() == "1.0.0" && lr.live_set.parts(0).timbre().library_id() == kLibA);
  CHECK(st->read(hf::derive_live_set_id(2), lr) == ls::Status::kOk && lr.live_set.parts(1).engine().engine_type_id() == 2 && !lr.live_set.parts(0).has_timbre());
  // Second run: nothing to do, no file touched, no write attempted.
  const std::uint64_t m = st->mutations();
  const std::string state = slurp(t.path + "/" + hf::kStateFileName);
  const std::string bytes1 = slurp(st->path_for(hf::derive_live_set_id(1)));
  r = hf::install_factory_live_sets(*st, f, opts(sg));
  CHECK(r.ok() && r.imported.empty() && r.updated.empty() && r.unchanged == std::vector<std::uint32_t>({1, 2}) && !r.changed());
  CHECK(st->mutations() == m && slurp(t.path + "/" + hf::kStateFileName) == state && slurp(st->path_for(hf::derive_live_set_id(1))) == bytes1);
  // The install goes through the single writer: a second store on the directory is refused.
  ls::StoreOptions so;
  so.dir = t.path;
  std::unique_ptr<ls::LiveSetStore> second;
  CHECK(ls::LiveSetStore::open(so, second) == ls::Status::kWriterBusy);
}

void test_install_user_delete_and_user_file() {
  TmpDir t;
  Signer sg(1);
  auto st = open_store(t.path);
  const std::string f = sign_file(sg, make_sets("1.0.0", {make_ls(1, "1.0.0", "One"), make_ls(2, "1.0.0", "Two"), make_ls(3, "1.0.0", "Three")}));
  CHECK(hf::install_factory_live_sets(*st, f, opts(sg)).ok());
  // The user deletes program 2: it is not brought back, by the same release or a newer one.
  CHECK(st->remove(hf::derive_live_set_id(2)) == ls::Status::kOk);
  hf::InstallResult r = hf::install_factory_live_sets(*st, f, opts(sg));
  CHECK(r.ok() && r.deleted_by_user == std::vector<std::uint32_t>({2}) && r.imported.empty() && r.unchanged == std::vector<std::uint32_t>({1, 3}));
  CHECK(!fs::exists(st->path_for(hf::derive_live_set_id(2))));
  const std::string f2 = sign_file(sg, make_sets("1.1.0", {make_ls(1, "1.1.0", "One v2"), make_ls(2, "1.1.0", "Two v2"), make_ls(3, "1.1.0", "Three v2"), make_ls(4, "1.1.0", "Four")}));
  r = hf::install_factory_live_sets(*st, f2, opts(sg));
  CHECK(r.ok() && r.updated == std::vector<std::uint32_t>({1, 3}) && r.imported == std::vector<std::uint32_t>({4}) && r.deleted_by_user == std::vector<std::uint32_t>({2}));
  ls::LoadResult lr;
  CHECK(st->read(hf::derive_live_set_id(1), lr) == ls::Status::kOk && lr.live_set.name() == "One v2" && lr.live_set.factory().release_version() == "1.1.0");
  CHECK(st->read(hf::derive_live_set_id(2), lr) == ls::Status::kNotFound);
  // A file with the derived id that is NOT ours (no factory mark) is never overwritten, and its program id is not recorded.
  TmpDir t2;
  auto st2 = open_store(t2.path);
  lsp::LiveSet mine = ls::make_default_live_set(hf::derive_live_set_id(5), "My own", 8);
  CHECK(st2->write(mine) == ls::Status::kOk);
  const std::string before = slurp(st2->path_for(mine.id()));
  r = hf::install_factory_live_sets(*st2, sign_file(sg, make_sets("1.0.0", {make_ls(5, "1.0.0", "Factory five"), make_ls(6, "1.0.0", "Six")})), opts(sg));
  CHECK(r.ok() && r.conflicts == std::vector<std::uint32_t>({5}) && r.imported == std::vector<std::uint32_t>({6}));
  CHECK(slurp(st2->path_for(mine.id())) == before);
  CHECK(slurp(t2.path + "/" + hf::kStateFileName).find("program 5") == std::string::npos);
}

void test_install_release_order() {
  TmpDir t;
  Signer sg(1);
  auto st = open_store(t.path);
  CHECK(hf::install_factory_live_sets(*st, sign_file(sg, make_sets("2.0.0", {make_ls(1, "2.0.0", "One")})), opts(sg)).ok());
  const std::uint64_t m = st->mutations();
  const hf::InstallResult r = hf::install_factory_live_sets(*st, sign_file(sg, make_sets("1.9.9", {make_ls(1, "1.9.9", "Old one"), make_ls(2, "1.9.9", "Old two")})), opts(sg));
  CHECK(r.status == hf::FactoryStatus::kDowngrade && st->mutations() == m && liveset_files(t.path) == 1);
  // The same release again is not a downgrade, and an equal release does not rewrite.
  CHECK(hf::install_factory_live_sets(*st, sign_file(sg, make_sets("2.0.0", {make_ls(1, "2.0.0", "One")})), opts(sg)).unchanged.size() == 1);
  CHECK(st->mutations() == m);
}

void test_install_references() {
  TmpDir t;
  Signer sg(1);
  auto st = open_store(t.path);
  lsp::LiveSet a = make_ls(1, "1.0.0", "A", true, kLibA, "piano.sfz");
  lsp::LiveSet b = make_ls(2, "1.0.0", "B", true, kLibB, "strings.sfz");
  fixtures::timbre(b.mutable_parts(3), kLibA, "gone.sfz");
  const std::string f = sign_file(sg, make_sets("1.0.0", {a, b}));
  const hf::ReferenceResolver only_piano = [](const std::string& lib, const std::string& key) { return lib == kLibA && key == "piano.sfz"; };
  const hf::InstallResult r = hf::install_factory_live_sets(*st, f, opts(sg, only_piano));
  CHECK(r.status == hf::FactoryStatus::kUnresolvedReferences && r.missing.size() == 2);
  if (r.missing.size() == 2) {
    CHECK(r.missing[0].program_id == 2 && r.missing[0].part == 0 && r.missing[0].library_id == kLibB && r.missing[0].instrument_key == "strings.sfz");
    CHECK(r.missing[1].program_id == 2 && r.missing[1].part == 3 && r.missing[1].library_id == kLibA && r.missing[1].instrument_key == "gone.sfz");
  }
  CHECK(r.detail.find(kLibB) != std::string::npos && r.detail.find("gone.sfz") != std::string::npos);
  CHECK(liveset_files(t.path) == 0 && !fs::exists(t.path + "/" + hf::kStateFileName));  // nothing written, not even for the resolvable program
  // Engine-only Live Sets reference no library and always install.
  CHECK(hf::install_factory_live_sets(*st, sign_file(sg, make_sets("1.0.0", {make_ls(9, "1.0.0", "Organ only", false)})), opts(sg, [](const std::string&, const std::string&) { return false; })).ok());
}

void test_install_rejections() {
  TmpDir t;
  Signer sg(1), other(2);
  auto st = open_store(t.path);
  auto expect = [&](const std::string& file, hf::FactoryStatus want, const Signer& s = Signer(1)) {
    const hf::InstallResult r = hf::install_factory_live_sets(*st, file, opts(s));
    CHECK(r.status == want);
    CHECK(liveset_files(t.path) == 0 && !fs::exists(t.path + "/" + hf::kStateFileName));  // nothing written for any rejection
    if (r.status != want) std::printf("   got %s: %s (wanted %s)\n", hf::to_string(r.status), r.detail.c_str(), hf::to_string(want));
  };
  const lsp::FactoryLiveSets good = make_sets("1.0.0", {make_ls(1, "1.0.0", "One")});
  expect(sign_file(sg, good), hf::FactoryStatus::kUntrustedKey, other);
  { std::string f = sign_file(sg, good); f[100] = static_cast<char>(f[100] ^ 1); expect(f, hf::FactoryStatus::kSignatureInvalid, sg); }
  expect("garbage", hf::FactoryStatus::kBadContainer, sg);
  { lsp::FactoryLiveSets s = good; s.set_format_version(2); expect(sign_file(sg, s), hf::FactoryStatus::kBadRelease, sg); }
  { lsp::FactoryLiveSets s = good; s.set_release_version("1.0"); expect(sign_file(sg, s), hf::FactoryStatus::kBadRelease, sg); }
  { lsp::FactoryLiveSets s = good; s.set_release_name(""); expect(sign_file(sg, s), hf::FactoryStatus::kBadRelease, sg); }
  { lsp::FactoryLiveSets s = good; s.set_root("staging"); expect(sign_file(sg, s), hf::FactoryStatus::kBadRelease, sg); }
  { lsp::FactoryLiveSets s = good; s.clear_entries(); expect(sign_file(sg, s), hf::FactoryStatus::kBadRelease, sg); }
  { lsp::FactoryLiveSets s = make_sets("1.0.0", {make_ls(1, "1.0.0", "One"), make_ls(2, "1.0.0", "Two")}); s.mutable_entries(1)->set_program_id(1); expect(sign_file(sg, s), hf::FactoryStatus::kDuplicateProgramId, sg); }
  { lsp::FactoryLiveSets s = good; s.mutable_entries(0)->set_program_id(0); expect(sign_file(sg, s), hf::FactoryStatus::kDuplicateProgramId, sg); }
  { lsp::FactoryLiveSets s = good; s.mutable_entries(0)->mutable_live_set()->set_id(fixtures::uuid_n(5)); expect(sign_file(sg, s), hf::FactoryStatus::kInvalidLiveSet, sg); }  // not the derived id
  { lsp::FactoryLiveSets s = good; s.mutable_entries(0)->mutable_live_set()->clear_factory(); expect(sign_file(sg, s), hf::FactoryStatus::kInvalidLiveSet, sg); }
  { lsp::FactoryLiveSets s = good; s.mutable_entries(0)->mutable_live_set()->mutable_factory()->set_release_version("0.9.0"); expect(sign_file(sg, s), hf::FactoryStatus::kInvalidLiveSet, sg); }
  { lsp::FactoryLiveSets s = good; s.mutable_entries(0)->mutable_live_set()->mutable_factory()->set_program_id(2); expect(sign_file(sg, s), hf::FactoryStatus::kInvalidLiveSet, sg); }
  { lsp::FactoryLiveSets s = good; s.mutable_entries(0)->mutable_live_set()->set_tempo(1000.0f); expect(sign_file(sg, s), hf::FactoryStatus::kInvalidLiveSet, sg); }
  { lsp::FactoryLiveSets s = good; s.mutable_entries(0)->mutable_live_set()->mutable_parts()->RemoveLast(); expect(sign_file(sg, s), hf::FactoryStatus::kInvalidLiveSet, sg); }
  { lsp::FactoryLiveSets s = good; s.mutable_entries(0)->mutable_live_set()->set_format_version(7); expect(sign_file(sg, s), hf::FactoryStatus::kInvalidLiveSet, sg); }
  // Options: no trusted keys / no resolver is a refusal, not a pass.
  {
    hf::InstallOptions none;
    CHECK(hf::install_factory_live_sets(*st, sign_file(sg, good), none).status == hf::FactoryStatus::kBadContainer);
  }
  // A missing file is an I/O error, a directory is not read.
  CHECK(hf::install_factory_live_sets_file(*st, t.path + "/nope.vfls", opts(sg)).status == hf::FactoryStatus::kIo);
  CHECK(hf::install_factory_live_sets_file(*st, t.path, opts(sg)).status == hf::FactoryStatus::kIo);
  // A damaged state file no longer stops the install: see test_state_rules.
}

fmt::TrustedKeys trusted_for_root(const Signer& sg, const std::string& root) {
  fmt::TrustedKey k{sg.id, sg.pub, root};
  return fmt::TrustedKeys({k});
}

void test_root_binding() {
  Signer sg(1);
  lsp::FactoryLiveSets dev = make_sets("1.0.0", {make_ls(1, "1.0.0", "One")});  // root "dev"
  lsp::FactoryLiveSets prod = dev;
  prod.set_root("production");
  const std::string fdev = sign_file(sg, dev), fprod = sign_file(sg, prod);
  const fmt::TrustedKeys dev_trust = trusted_for_root(sg, "dev"), any_trust = trusted_for_root(sg, "");
  lsp::FactoryLiveSets out;
  CHECK(hf::verify_container(fdev, dev_trust, out) == hf::ContainerStatus::kOk);
  CHECK(hf::verify_container(fprod, dev_trust, out) == hf::ContainerStatus::kRootMismatch);  // a dev key claiming production
  CHECK(hf::verify_container(fprod, any_trust, out) == hf::ContainerStatus::kOk);            // a trust without a root binds nothing
  CHECK(hf::verify_container(fprod, trusted_for_root(sg, "production"), out) == hf::ContainerStatus::kOk);
  TmpDir t;
  auto st = open_store(t.path);
  hf::InstallOptions o;
  o.trusted = &dev_trust;
  o.resolves = kAllResolve;
  const hf::InstallResult r = hf::install_factory_live_sets(*st, fprod, o);
  CHECK(r.status == hf::FactoryStatus::kRootMismatch && liveset_files(t.path) == 0 && !fs::exists(t.path + "/" + hf::kStateFileName));
  CHECK(hf::install_factory_live_sets(*st, fdev, o).ok());
  // A trust list read from a file keeps the root column.
  {
    std::ofstream(t.path + "/x.pub-list") << "production content-manifest " << fmt::to_hex(sg.id) << " " << fmt::to_hex(sg.pub) << "\n";
    fmt::TrustedKeys tk;
    std::string err;
    CHECK(velora::host::load_trust_list_file(t.path + "/x.pub-list", tk, err) && tk.find(sg.id) && tk.find(sg.id)->root == "production");
  }
}

void test_state_rules() {
  Signer sg(1);
  // Per root: another root's state is not a floor and nothing is deleted.
  {
    TmpDir t;
    auto st = open_store(t.path);
    CHECK(hf::install_factory_live_sets(*st, sign_file(sg, make_sets("2.0.0", {make_ls(1, "2.0.0", "One")})), opts(sg)).ok());
    lsp::FactoryLiveSets prod = make_sets("1.0.0", {make_ls(2, "1.0.0", "Two")});
    prod.set_root("production");
    const hf::InstallResult r = hf::install_factory_live_sets(*st, sign_file(sg, prod), opts(sg));
    CHECK(r.ok() && r.imported == std::vector<std::uint32_t>({2}) && liveset_files(t.path) == 2);
    CHECK(slurp(t.path + "/" + hf::kStateFileName).find("root production") != std::string::npos);
    // ... and going back to dev at 2.0.0 is unchanged for program 1 (its file is still there).
    CHECK(hf::install_factory_live_sets(*st, sign_file(sg, make_sets("2.0.0", {make_ls(1, "2.0.0", "One")})), opts(sg)).unchanged.size() == 1);
  }
  // Unknown `key value` lines of a newer writer are ignored; the known ones still count.
  {
    TmpDir t;
    auto st = open_store(t.path);
    const std::string f = sign_file(sg, make_sets("1.0.0", {make_ls(1, "1.0.0", "One")}));
    CHECK(hf::install_factory_live_sets(*st, f, opts(sg)).ok());
    std::string state = slurp(t.path + "/" + hf::kStateFileName);
    fs::remove(t.path + "/" + hf::kStateFileName);
    std::ofstream(t.path + "/" + hf::kStateFileName) << state << "future-field some value\n";
    CHECK(st->remove(hf::derive_live_set_id(1)) == ls::Status::kOk);
    const hf::InstallResult r = hf::install_factory_live_sets(*st, f, opts(sg));
    CHECK(r.ok() && r.deleted_by_user == std::vector<std::uint32_t>({1}) && r.imported.empty());  // the state was read, not rebuilt
  }
  // A damaged state file is rebuilt from the factory marks in the store (nothing is refused, nothing is lost).
  {
    TmpDir t;
    auto st = open_store(t.path);
    const std::string f = sign_file(sg, make_sets("1.0.0", {make_ls(1, "1.0.0", "One"), make_ls(2, "1.0.0", "Two")}));
    CHECK(hf::install_factory_live_sets(*st, f, opts(sg)).ok());
    fs::remove(t.path + "/" + hf::kStateFileName);
    std::ofstream(t.path + "/" + hf::kStateFileName) << "not a state file\n";
    const hf::InstallResult r = hf::install_factory_live_sets(*st, f, opts(sg));
    CHECK(r.ok() && r.unchanged == std::vector<std::uint32_t>({1, 2}) && r.imported.empty());
    CHECK(slurp(t.path + "/" + hf::kStateFileName).find("program 2") != std::string::npos);  // rewritten properly
  }
  // A missing state file: the downgrade floor is the highest factory mark in the store.
  {
    TmpDir t;
    auto st = open_store(t.path);
    CHECK(hf::install_factory_live_sets(*st, sign_file(sg, make_sets("2.0.0", {make_ls(1, "2.0.0", "One")})), opts(sg)).ok());
    fs::remove(t.path + "/" + hf::kStateFileName);
    const std::uint64_t m = st->mutations();
    CHECK(hf::install_factory_live_sets(*st, sign_file(sg, make_sets("1.9.0", {make_ls(1, "1.9.0", "Old")})), opts(sg)).status == hf::FactoryStatus::kDowngrade && st->mutations() == m);
  }
  // A failure half way records the ids actually written: the rerun does not take them for deletions, and completes.
  {
    TmpDir t;
    auto st = open_store(t.path);
    fs::create_directories(t.path + "/" + hf::derive_live_set_id(2) + ".liveset");  // program 2's file cannot be written (a directory is in the way)
    const std::string f = sign_file(sg, make_sets("1.0.0", {make_ls(1, "1.0.0", "One"), make_ls(2, "1.0.0", "Two")}));
    const hf::InstallResult bad = hf::install_factory_live_sets(*st, f, opts(sg));
    CHECK(bad.status == hf::FactoryStatus::kStore && bad.program_id == 2 && fs::exists(st->path_for(hf::derive_live_set_id(1))));
    const std::string state = slurp(t.path + "/" + hf::kStateFileName);
    CHECK(state.find("program 1") != std::string::npos && state.find("program 2") == std::string::npos);
    fs::remove_all(t.path + "/" + hf::derive_live_set_id(2) + ".liveset");
    const hf::InstallResult r = hf::install_factory_live_sets(*st, f, opts(sg));
    CHECK(r.ok() && r.unchanged == std::vector<std::uint32_t>({1}) && r.imported == std::vector<std::uint32_t>({2}));
  }
  // A damaged factory file at a listed program id is repaired, not taken for a user's conflict.
  {
    TmpDir t;
    auto st = open_store(t.path);
    const std::string f = sign_file(sg, make_sets("1.0.0", {make_ls(1, "1.0.0", "One")}));
    CHECK(hf::install_factory_live_sets(*st, f, opts(sg)).ok());
    { std::ofstream(st->path_for(hf::derive_live_set_id(1)), std::ios::binary | std::ios::trunc) << "garbage, not a Live Set"; }
    const hf::InstallResult r = hf::install_factory_live_sets(*st, f, opts(sg));
    CHECK(r.ok() && r.updated == std::vector<std::uint32_t>({1}) && r.conflicts.empty());
    ls::LoadResult lr;
    CHECK(st->read(hf::derive_live_set_id(1), lr) == ls::Status::kOk && hf::is_factory(lr.live_set));
  }
}

void test_install_crash_recovery() {
  // The state file is written last: a crash after the Live Sets and before the state is repaired by running the install again (the files are 'unchanged', the state is written).
  TmpDir t;
  Signer sg(1);
  auto st = open_store(t.path);
  const std::string f = sign_file(sg, make_sets("1.0.0", {make_ls(1, "1.0.0", "One"), make_ls(2, "1.0.0", "Two")}));
  CHECK(hf::install_factory_live_sets(*st, f, opts(sg)).ok());
  fs::remove(t.path + "/" + hf::kStateFileName);  // the crash
  const hf::InstallResult r = hf::install_factory_live_sets(*st, f, opts(sg));
  CHECK(r.ok() && r.unchanged == std::vector<std::uint32_t>({1, 2}) && r.imported.empty() && fs::exists(t.path + "/" + hf::kStateFileName));
  // A write that fails on the way (full disk) reports kStore and a rerun completes the job.
  TmpDir t2;
  auto st2 = open_store(t2.path);
  st2->set_inject_write_errno(ENOSPC);
  const hf::InstallResult bad = hf::install_factory_live_sets(*st2, f, opts(sg));
  CHECK(bad.status == hf::FactoryStatus::kStore && liveset_files(t2.path) == 0 && !fs::exists(t2.path + "/" + hf::kStateFileName));
  st2->set_inject_write_errno(0);
  CHECK(hf::install_factory_live_sets(*st2, f, opts(sg)).imported.size() == 2);
}

void test_install_control_thread() {
  TmpDir t;
  Signer sg(1);
  auto st = open_store(t.path);
  const std::string f = sign_file(sg, make_sets("1.0.0", {make_ls(1, "1.0.0", "One")}));
  hf::InstallResult r;
  std::thread other([&] { r = hf::install_factory_live_sets(*st, f, opts(sg)); });
  other.join();
  CHECK(r.status == hf::FactoryStatus::kStore && liveset_files(t.path) == 0);  // only the control thread writes
  CHECK(hf::install_factory_live_sets(*st, f, opts(sg)).ok());
}

void test_factory_mark_roundtrip() {
  // The mark is an additive optional field: a Live Set without it serialises exactly as before, one with it survives read -> write unchanged.
  lsp::LiveSet plain = ls::make_default_live_set(fixtures::uuid_n(77), "Plain", 8);
  std::string a;
  CHECK(ls::serialize(plain, a) == ls::Status::kOk);
  lsp::LiveSet marked = plain;
  marked.mutable_factory()->set_program_id(3);
  marked.mutable_factory()->set_release_version("1.0.0");
  std::string b;
  CHECK(ls::serialize(marked, b) == ls::Status::kOk && b != a && b.size() > a.size());
  ls::LoadResult lr;
  CHECK(ls::parse(b, {}, lr) == ls::Status::kOk && lr.live_set.has_factory() && lr.live_set.factory().program_id() == 3);
  std::string c;
  CHECK(ls::serialize(lr.live_set, c) == ls::Status::kOk && c == b);
  CHECK(!hf::is_factory(plain) && hf::is_factory(marked));
}

}  // namespace

int main() {
  test_ids_and_versions();
  test_container_layout();
  test_container_tamper_sweep();
  test_install_happy_and_idempotent();
  test_install_user_delete_and_user_file();
  test_install_release_order();
  test_install_references();
  test_install_rejections();
  test_install_crash_recovery();
  test_root_binding();
  test_state_rules();
  test_install_control_thread();
  test_factory_mark_roundtrip();
  if (g_fail == 0) std::printf("factory_live_set_tests: ok\n");
  return g_fail == 0 ? 0 : 1;
}
