// Story 6.9: the factory release pipeline, end to end on a deterministic SYNTHETIC factory source tree (the founder's content, Story 11.17, is not delivered yet).
//   * the release description (schema, every refusal with its typed code);
//   * build: layout, the index (every file with SHA-256 and size, libraries, program id table), the signature checked independently, vaults only in registry-vault/;
//   * reproducibility: a rebuild from the same sources, keys and vaults is byte-identical (every file, the index, the signed Live Sets); other keys / a fresh content key are not;
//   * refusals: vendor-id conflicts and unregistered ids through the register, duplicate program ids, missing categories, unresolved references, root mismatch, existing output;
//   * verify: a clean release, a tamper sweep (every kind of change with its exact reason), a re-signed index that still hides a damaged bundle (found by --bundles);
//   * the CLI and its exit codes;
//   * (dev key provider builds) the release installed on the host librarian and played by the engine host components: every factory instrument plays, every factory Live Set
//     loads without a missing library, each instrument shows its category, the first-boot import is idempotent and a missing library is a typed refusal.
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "crypto.hpp"
#include "lib_test_util.hpp"
#include "release.hpp"
#include "synthetic_factory.hpp"
#include "velora/format/category.hpp"
#include "velora/format/json_lite.hpp"
#include "velora/format/protected_verify.hpp"
#include "velora/format/protected_writer.hpp"
#include "velora/host/factory_live_set.hpp"
#include "velora/librarian/install.hpp"
#include "velora/librarian/library_service.hpp"
#ifdef FR_HAS_DEV_KEYS
#include "velora/core/block_renderer.hpp"
#include "velora/core/organ_engine.hpp"
#include "velora/core/sample_engine.hpp"
#include "velora/host/control_plane.hpp"
#include "velora/host/damage_registry.hpp"
#include "velora/host/dev_key_provider.hpp"
#include "velora/host/fs_port.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/host/live_set_loader.hpp"
#include "velora/host/protected_head_provider.hpp"
#endif

#ifndef FR_EXE
#error "FR_EXE must name velora-factory-release"
#endif

namespace fs = std::filesystem;
namespace fmt = velora::format;
namespace cp = velora::contentpack;
namespace fr = velora::factoryrelease;
namespace hf = velora::host::factory;
namespace lib = velora::librarian;
using ltest::TempDir;

namespace {

using Bytes = std::vector<std::uint8_t>;

// CHECK of lib_test_util.hpp is used; a failure code check:
#define CHECK_CODE(f, want)                                                                                                                         \
  do {                                                                                                                                              \
    const fr::Failure _f = (f);                                                                                                                     \
    if (_f.code != (want)) {                                                                                                                        \
      std::printf("FAIL %s:%d: %s -> %s '%s' (want %s)\n", __FILE__, __LINE__, #f, fr::to_string(_f.code), _f.message.c_str(), fr::to_string(want)); \
      ++g_fail;                                                                                                                                     \
    }                                                                                                                                               \
  } while (0)

std::string hex(std::span<const std::uint8_t> d) { return fmt::to_hex(d); }

Bytes read_all(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
std::string read_text(const fs::path& p) {
  const Bytes b = read_all(p);
  return std::string(b.begin(), b.end());
}
void put(const fs::path& p, const std::string& bytes, mode_t mode = 0644) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary | std::ios::trunc).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  ::chmod(p.c_str(), mode);
}
void put_bytes(const fs::path& p, std::span<const std::uint8_t> b, mode_t mode = 0644) { put(p, std::string(b.begin(), b.end()), mode); }

// Every regular file under `dir` (relative path -> bytes).
std::map<std::string, Bytes> tree(const fs::path& dir) {
  std::map<std::string, Bytes> m;
  for (const auto& e : fs::recursive_directory_iterator(dir))
    if (e.is_regular_file()) m[e.path().lexically_relative(dir).generic_string()] = read_all(e.path());
  return m;
}

int run_cmd(const std::string& cmd, std::string* out = nullptr) {
  static int n = 0;
  const std::string log = "/tmp/vl_fr_" + std::to_string(::getpid()) + "_" + std::to_string(++n) + ".log";
  const int rc = std::system((cmd + " > '" + log + "' 2>&1").c_str());
  if (out) *out = ltest::slurp(log);
  std::remove(log.c_str());
  return rc == -1 ? -1 : (WIFEXITED(rc) ? WEXITSTATUS(rc) : -1);
}

// ---- keys ------------------------------------------------------------------------------------------------------------------------------------------

struct Keys {
  cp::Key32 seed{}, kek{}, dev_priv{}, dev_pub{};
  fmt::SigningSeed sg;
  fmt::PublicKey pub{};
  fmt::KeyId id{};
  fmt::TrustedKeys trusted;
};

Keys make_keys(const fs::path& dir, const char* root = "dev") {
  Keys k;
  cp::random_bytes(k.seed);
  cp::random_bytes(k.kek);
  cp::random_bytes(k.dev_priv);
  k.sg.bytes = k.seed;
  k.pub = fmt::ed25519_public_from_seed(k.sg);
  k.id = fmt::key_id_of_public(k.pub);
  k.dev_pub = cp::x25519_public(k.dev_priv);
  k.trusted = fmt::TrustedKeys({fmt::TrustedKey{k.id, k.pub}});
  put_bytes(dir / "content-manifest.ed25519.seed", k.seed, 0600);
  put_bytes(dir / "content-manifest.ed25519.pub", k.pub, 0644);
  put_bytes(dir / "kek.aes256", k.kek, 0600);
  put_bytes(dir / "device.x25519.priv", k.dev_priv, 0600);
  put_bytes(dir / "device.x25519.pub", k.dev_pub, 0644);
  put(dir / "content-manifest.json", std::string("{\"root\": \"") + root + "\", \"dev_only\": false, \"key_id\": \"" + hex(k.id) + "\"}\n");
  put(dir / "kek.json", std::string("{\"root\": \"") + root + "\", \"dev_only\": false, \"key_id\": \"" + hex(cp::kek_id(k.kek)) + "\"}\n");
  return k;
}

// ---- helpers ---------------------------------------------------------------------------------------------------------------------------------------

struct Env {
  TempDir t;
  fs::path work, src, keys_dir;
  Keys keys;
  Env() {
    work = t.path;
    src = work / "src";
    keys_dir = work / "keys";
    synth::write_sources(src);
    keys = make_keys(keys_dir);
  }
  fr::ReleaseDescription desc(const synth::Options& o = {}) const {
    fr::ReleaseDescription d;
    CHECK_CODE(fr::parse_release_description(synth::description(o), src.string(), d), fr::Code::kOk);
    return d;
  }
  fr::BuildRequest request(const fr::ReleaseDescription& d, const fs::path& out, const std::string& reuse = "") const {
    fr::BuildRequest r;
    r.desc = d;
    r.out_dir = out.string();
    r.kek = &keys.kek;
    r.signing_seed = &keys.sg;
    r.reuse_vaults_dir = reuse;
    r.register_path = (work / "vendor-ids").string();
    r.date = "2026-10-20";
    return r;
  }
  fr::VerifyOptions vopts(bool bundles = false) const {
    fr::VerifyOptions v;
    v.trusted = &keys.trusted;
    v.check_bundles = bundles;
    return v;
  }
};

fs::path new_dir(const fs::path& p) {
  fs::create_directories(p);
  return p;
}

std::set<std::string> problem_codes(const fr::VerifyReport& r) {
  std::set<std::string> s;
  for (const auto& p : r.problems) s.insert(p.code);
  return s;
}

// Verify and return the codes.
std::set<std::string> verify_codes(const Env& e, const fs::path& dir, bool bundles = false, fr::Failure* f = nullptr) {
  fr::VerifyReport rep;
  const fr::Failure r = fr::verify_release(dir.string(), e.vopts(bundles), rep);
  if (f) *f = r;
  return problem_codes(rep);
}

// A release copy to damage.
fs::path copy_release(const fs::path& from, const fs::path& to) {
  fs::remove_all(to);
  fs::copy(from, to, fs::copy_options::recursive);
  return to;
}

void flip(const fs::path& p, std::size_t at, std::uint8_t mask = 0x10) {
  Bytes b = read_all(p);
  b.at(at) = static_cast<std::uint8_t>(b.at(at) ^ mask);
  put_bytes(p, b);
}

// Writes `idx` as the release index of `rel_dir` and signs it again with the test key: an attacker who owns the signing key, or a buggy tool.
void write_signed_index(const Env& e, const fs::path& rel_dir, const std::string& idx) {
  put(rel_dir / fr::kIndexFileName, idx);
  const fmt::Digest256 h = fmt::Sha256::hash(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(idx.data()), idx.size()));
  std::vector<std::uint8_t> msg;
  msg.insert(msg.end(), fr::kIndexSigDomain, fr::kIndexSigDomain + std::strlen(fr::kIndexSigDomain));
  msg.push_back(0);
  msg.insert(msg.end(), e.keys.id.begin(), e.keys.id.end());
  msg.insert(msg.end(), h.begin(), h.end());
  auto sig = fmt::ed25519_sign(e.keys.sg, msg);
  CHECK(static_cast<bool>(sig));
  std::string sf = "VRIS";
  sf.push_back(1);
  sf.push_back(1);
  sf.push_back(0);
  sf.push_back(0);
  sf.append(reinterpret_cast<const char*>(e.keys.id.data()), 8);
  sf.append(reinterpret_cast<const char*>(sig.value().data()), 64);
  put(rel_dir / fr::kIndexSigFileName, sf);
}

// Replaces the first `from` in the index text and re-signs.
void edit_index(const Env& e, const fs::path& rel_dir, const std::string& from, const std::string& to) {
  std::string idx = read_text(rel_dir / fr::kIndexFileName);
  const std::size_t at = idx.find(from);
  CHECK(at != std::string::npos);
  if (at != std::string::npos) idx.replace(at, from.size(), to);
  write_signed_index(e, rel_dir, idx);
}

// Rewrites the index entry of `path` (size, sha256) and signs the index again.
void reindex(const Env& e, const fs::path& rel_dir, const std::string& path) {
  std::string idx = read_text(rel_dir / fr::kIndexFileName);
  const std::size_t at = idx.find("{\"path\": \"" + path + "\"");
  CHECK(at != std::string::npos);
  const std::size_t end = idx.find('}', at);
  std::uint64_t size = 0;
  const std::string sha = fr::sha256_file_hex((rel_dir / path).string(), &size);
  idx.replace(at, end - at + 1, "{\"path\": \"" + path + "\", \"size\": " + std::to_string(size) + ", \"sha256\": \"" + sha + "\"}");
  write_signed_index(e, rel_dir, idx);
}

// ---- tests -----------------------------------------------------------------------------------------------------------------------------------------

void test_description() {
  Env e;
  fr::ReleaseDescription d = e.desc();
  CHECK(d.name == "Velora Factory Library" && d.version == "1.0.0" && d.root == cp::Root::kDev && d.libraries.size() == 4 && d.live_sets.size() == 4);
  CHECK(d.live_set_file == hf::kDefaultFileName && d.libraries[3].pack.instruments.size() == 2 && d.libraries[0].pack.localized_names.at("pt-BR") == "Piano de Fabrica");
  CHECK(d.live_sets[1].parts[0].engine_type == 2 && !d.live_sets[1].parts[0].has_timbre && d.live_sets[1].parts[0].params.size() == 3);
  CHECK(d.live_sets[3].parts[1].output_aux && d.live_sets[3].parts[1].octave == 1 && d.live_sets[3].parts[0].high_key == 59);
  // A description without Live Sets is fine.
  { synth::Options o; o.with_live_sets = false; fr::ReleaseDescription x = e.desc(o); CHECK(x.live_sets.empty() && x.live_set_file.empty()); }

  auto parse = [&](const std::string& text) {
    fr::ReleaseDescription x;
    return fr::parse_release_description(text, e.src.string(), x);
  };
  auto replace = [](std::string s, const std::string& a, const std::string& b) {
    const std::size_t at = s.find(a);
    if (at == std::string::npos) std::printf("(test bug: '%s' not found)\n", a.c_str());
    return s.replace(at, a.size(), b);
  };
  const std::string good = synth::description();
  CHECK_CODE(parse(good), fr::Code::kOk);
  CHECK_CODE(parse("not json"), fr::Code::kBadDescription);
  CHECK_CODE(parse("[]"), fr::Code::kBadDescription);
  CHECK_CODE(parse(replace(good, "\"schema_version\": 1", "\"schema_version\": 2")), fr::Code::kBadDescription);
  CHECK_CODE(parse(replace(good, "\"root\": \"dev\"", "\"root\": \"staging\"")), fr::Code::kBadDescription);
  CHECK_CODE(parse(replace(good, "\"version\": \"1.0.0\"", "\"version\": \"1.0\"")), fr::Code::kBadDescription);
  CHECK_CODE(parse(replace(good, "\"version\": \"1.0.0\"", "\"version\": \"v1.0.0\"")), fr::Code::kBadDescription);
  CHECK_CODE(parse(replace(good, "\"name\": \"Velora Factory Library\", ", "")), fr::Code::kBadDescription);
  CHECK_CODE(parse(replace(good, "\"schema_version\": 1,", "\"schema_version\": 1, \"extra\": 1,")), fr::Code::kBadDescription);
  CHECK_CODE(parse(replace(good, "\"libraries\"", "\"libraries\": [], \"libraries\"")), fr::Code::kBadDescription);  // duplicate field
  CHECK_CODE(parse(replace(good, "\"revision\": 1}", "\"revision\": 0}")), fr::Code::kBadDescription);
  CHECK_CODE(parse(replace(good, "\"revision\": 1}", "\"revision\": 1, \"colour\": 3}")), fr::Code::kBadDescription);
  // Duplicate library id / name.
  CHECK_CODE(parse(replace(good, synth::kPipeId, synth::kPianoId)), fr::Code::kDuplicateLibrary);
  CHECK_CODE(parse(replace(good, "Velora Factory Pipes", "  velora  FACTORY piano ")), fr::Code::kDuplicateLibrary);
  // Missing category: left out, or one that is not assignable (none).
  { synth::Options o; o.omit_piano_category = true; CHECK_CODE(parse(synth::description(o)), fr::Code::kMissingCategory); }
  { synth::Options o; o.piano_category = "none"; CHECK_CODE(parse(synth::description(o)), fr::Code::kMissingCategory); }
  { synth::Options o; o.piano_category = "bagpipes"; CHECK_CODE(parse(synth::description(o)), fr::Code::kMissingCategory); }
  CHECK_CODE(parse(replace(good, "\"category\": \"epiano\"", "\"category\": \"none\"")), fr::Code::kMissingCategory);
  // Program ids.
  CHECK_CODE(parse(replace(good, "\"program_id\": 2", "\"program_id\": 1")), fr::Code::kDuplicateProgramId);
  CHECK_CODE(parse(replace(good, "\"program_id\": 2", "\"program_id\": 0")), fr::Code::kDuplicateProgramId);
  CHECK_CODE(parse(replace(good, "\"program_id\": 2", "\"program_id\": -3")), fr::Code::kDuplicateProgramId);
  // References: only to the release's own libraries; an organ part has no timbre; fields are checked.
  { synth::Options o; o.live_set_extra = ",{\"program_id\": 9, \"name\": \"X\", \"parts\": [{\"part\": 0, \"timbre\": {\"library_id\": \"00000000-0000-4000-8000-0000000000aa\", \"instrument_key\": \"a.sfz\"}}]}";
    CHECK_CODE(parse(synth::description(o)), fr::Code::kUnresolvedReference); }
  CHECK_CODE(parse(replace(good, "\"type\": \"organ\"", "\"type\": \"organ\", \"bogus\": 1")), fr::Code::kBadDescription);
  CHECK_CODE(parse(replace(good, "\"high_key\": 59", "\"high_key\": 300")), fr::Code::kBadDescription);
  CHECK_CODE(parse(replace(good, "\"output\": \"aux\"", "\"output\": \"left\"")), fr::Code::kBadDescription);
  CHECK_CODE(parse(replace(good, "\"part\": 1, \"timbre\": {\"library_id\": \"00000000-0000-4000-8000-0000000000f3\"", "\"part\": 0, \"timbre\": {\"library_id\": \"00000000-0000-4000-8000-0000000000f3\"")),
             fr::Code::kBadDescription);  // part 0 twice in program 3
  CHECK_CODE(parse(replace(good, "\"live_sets\": {", "\"live_sets\": {\"file\": \"../x.vfls\", ")), fr::Code::kBadDescription);
  CHECK_CODE(parse(replace(good, "\"live_sets\": {", "\"live_sets\": {\"file\": \"set.txt\", ")), fr::Code::kBadDescription);
  CHECK_CODE(parse(replace(good, "\"live_sets\": {", "\"live_sets\": {\"file\": \"gospel.vfls\", ")), fr::Code::kOk);
  // check command semantic: same parser.
  std::string out;
  put(e.src / "release.json", good);
  CHECK(run_cmd(std::string("'") + FR_EXE + "' check --description '" + (e.src / "release.json").string() + "'", &out) == 0 && out.find("4 libraries, 4 factory Live Set(s): ok") != std::string::npos);
}

void test_build_layout_and_index() {
  Env e;
  const fs::path out = new_dir(e.work / "out");
  fr::BuildReport rep;
  CHECK_CODE(fr::build_release(e.request(e.desc(), out), rep), fr::Code::kOk);
  const fs::path rd = out / "1.0.0";
  CHECK(rep.release_dir == rd.string() && rep.libraries.size() == 4 && rep.programs.size() == 4 && rep.signer_key_id == hex(e.keys.id));
  // Layout: unit/ holds what a unit gets, registry-vault/ what only the registry gets, the index and its signature at the top. Nothing else (no staging, no packs).
  std::set<std::string> want = {"release-index.json", "release-index.sig", "unit/factory-live-sets.vfls"};
  for (const auto& l : rep.libraries) {
    for (const char* f : {"bundle.vlrp", "catalog.meta", "catalog.meta.sig", "pack.json"}) want.insert("unit/libraries/" + l.library_id + "/" + f);
    want.insert("registry-vault/" + l.library_id + "/content-key.vault");
  }
  std::set<std::string> have;
  const auto files = tree(rd);
  for (const auto& [k, v] : files) have.insert(k);
  CHECK(have == want);
  for (const auto& e2 : fs::directory_iterator(out)) CHECK(e2.path().filename() == "1.0.0");  // no staging directory left behind
  // No vault entry or key material in the unit image: the vault entry is 72 bytes under the KEK, and nothing under unit/ contains the KEK, the signing seed or a
  // content key (unwrapped from the vault).
  for (const auto& l : rep.libraries) {
    const Bytes v = files.at("registry-vault/" + l.library_id + "/content-key.vault");
    CHECK(v.size() == cp::kVaultEntrySize);
    cp::Key32 ck{};
    CHECK(cp::vault_entry_open(e.keys.kek, v, l.library_id, ck));
    for (const auto& [name, bytes] : files) {
      if (name.rfind("unit/", 0) != 0) continue;
      auto has = [&](std::span<const std::uint8_t> n) { return std::search(bytes.begin(), bytes.end(), n.begin(), n.end()) != bytes.end(); };
      CHECK(!has(ck) && !has(e.keys.kek) && !has(e.keys.seed));
    }
    struct stat st;
    ::stat((rd / "registry-vault" / l.library_id / "content-key.vault").c_str(), &st);
    CHECK((st.st_mode & 077) == 0);  // 0600
  }
  // Library rows: names, bundle ids, instruments with categories, revision.
  const auto& piano = rep.libraries[0];
  CHECK(piano.library_id == synth::kPianoId && piano.name == "Velora Factory Piano" && piano.revision == 1 && piano.instruments.size() == 1 && piano.instruments[0].key == "Factory_Grand.sfz" &&
        piano.instruments[0].category == "category.piano");
  const auto& keys_lib = rep.libraries[3];
  CHECK(keys_lib.instruments.size() == 3 && keys_lib.instruments[0].name == "Factory E.Piano" && keys_lib.instruments[0].category == "category.epiano" &&
        keys_lib.instruments[1].category == "category.synth_pad" && keys_lib.instruments[2].category == "category.keys");
  // The index: parse it, every file listed once with the right size and SHA-256 (computed here independently of the tool), sorted, the libraries and the program table.
  const std::string idx_text = read_text(rd / "release-index.json");
  fmt::JsonValue idx;
  std::string jerr;
  CHECK(fmt::parse_json(idx_text, idx, &jerr));
  CHECK(idx.find("schema")->string == fr::kIndexSchema && idx.find("release")->find("version")->string == "1.0.0" && idx.find("release")->find("root")->string == "dev" &&
        idx.find("signer_key_id")->string == hex(e.keys.id) && idx.find("kek_id")->string == hex(cp::kek_id(e.keys.kek)));
  const auto* farr = idx.find("files");
  CHECK(farr && farr->array.size() == want.size() - 2);  // everything but the index and its signature
  std::string prev;
  for (const auto& f : farr->array) {
    const std::string p = f.find("path")->string;
    CHECK(p > prev && files.count(p));
    prev = p;
    if (!files.count(p)) continue;
    CHECK(static_cast<std::size_t>(f.find("size")->number) == files.at(p).size());
    CHECK(f.find("sha256")->string == hex(fmt::Sha256::hash(files.at(p))));
  }
  const auto* larr = idx.find("libraries");
  CHECK(larr && larr->array.size() == 4);
  if (larr && larr->array.size() == 4) {
    CHECK(larr->array[0].find("library_id")->string == synth::kPianoId && larr->array[0].find("bundle_id")->string == piano.bundle_id && larr->array[0].find("revision")->number == 1);
    CHECK(larr->array[3].find("instruments")->array.size() == 3 && larr->array[3].find("files")->find("vault")->string == std::string("registry-vault/") + synth::kKeysId + "/content-key.vault");
  }
  const auto* ls = idx.find("live_sets");
  CHECK(ls && ls->find("file")->string == "unit/factory-live-sets.vfls" && ls->find("programs")->array.size() == 4);
  CHECK(ls->find("programs")->array[1].find("program_id")->number == 2 && ls->find("programs")->array[1].find("live_set_id")->string == hf::derive_live_set_id(2));
  // The signature file: 80 bytes, VRIS, Ed25519 over the documented statement, checked here with the verify-only primitive.
  const Bytes sigf = read_all(rd / "release-index.sig");
  CHECK(sigf.size() == 80 && std::memcmp(sigf.data(), "VRIS", 4) == 0 && sigf[4] == 1 && sigf[5] == 1 && sigf[6] == 0 && sigf[7] == 0 && std::memcmp(sigf.data() + 8, e.keys.id.data(), 8) == 0);
  {
    const fmt::Digest256 h = fmt::Sha256::hash(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(idx_text.data()), idx_text.size()));
    std::vector<std::uint8_t> msg;
    const std::string dom = "velora/factory-release-index/v1";
    msg.insert(msg.end(), dom.begin(), dom.end());
    msg.push_back(0);
    msg.insert(msg.end(), e.keys.id.begin(), e.keys.id.end());
    msg.insert(msg.end(), h.begin(), h.end());
    fmt::Signature sig{};
    std::memcpy(sig.data(), sigf.data() + 16, 64);
    CHECK(fmt::ed25519_verify_strict(e.keys.pub, msg, sig));
    CHECK(rep.index_sha256 == hex(h));
  }
  // The signed Live Sets verify against the same key and carry the program ids.
  {
    const Bytes lsb = files.at("unit/factory-live-sets.vfls");
    velora::liveset::v1::FactoryLiveSets sets;
    CHECK(hf::verify_container(std::string_view(reinterpret_cast<const char*>(lsb.data()), lsb.size()), e.keys.trusted, sets) == hf::ContainerStatus::kOk);
    CHECK(sets.release_version() == "1.0.0" && sets.root() == "dev" && sets.entries_size() == 4 && sets.entries(2).program_id() == 3 && sets.entries(2).live_set().name() == "Piano and Strings");
    CHECK(sets.entries(2).live_set().factory().release_version() == "1.0.0" && sets.entries(2).live_set().parts(1).timbre().library_id() == synth::kStringsId);
    CHECK(sets.entries(1).live_set().parts(0).engine().engine_type_id() == 2 && !sets.entries(1).live_set().parts(0).has_timbre());
  }
  // The release verifies, with and without re-verifying every bundle.
  fr::VerifyReport vr;
  CHECK_CODE(fr::verify_release(rd.string(), e.vopts(true), vr), fr::Code::kOk);
  CHECK(vr.ok() && vr.bundles_checked == 4 && vr.files_checked == want.size() - 2 && vr.live_sets == 4 && vr.release_version == "1.0.0" && vr.signer_key_id == hex(e.keys.id));
  CHECK_CODE(fr::verify_release(rd.string(), e.vopts(false), vr), fr::Code::kOk);
  // Every bundle's own signature is the release root's key, with the pack revision sealed into catalog.meta.sig.
  for (const auto& l : rep.libraries) {
    const std::string dir = (rd / "unit/libraries" / l.library_id).string();
    auto view = fmt::ProtectedBundleView::open_file(dir + "/bundle.vlrp", e.keys.trusted);
    CHECK(view && hex(view.value().info().bundle_id) == l.bundle_id && view.value().info().signer_key_id == e.keys.id);
    std::uint32_t rv = 0;
    const std::string meta = read_text(dir + "/catalog.meta"), seal = read_text(dir + "/catalog.meta.sig");
    CHECK(fmt::verify_catalog_meta_seal(e.keys.trusted, view.value().info().bundle_id, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(meta.data()), meta.size()),
                                        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(seal.data()), seal.size()), nullptr, &rv) == fmt::Error::kNone &&
          rv == 1);
  }
  // Another key does not verify the release.
  {
    Env other;
    fr::VerifyOptions vo;
    vo.trusted = &other.keys.trusted;
    CHECK_CODE(fr::verify_release(rd.string(), vo, vr), fr::Code::kVerifyFailed);
    CHECK(problem_codes(vr) == std::set<std::string>({"index_untrusted_key"}));
    fmt::TrustedKeys none;
    vo.trusted = &none;
    CHECK_CODE(fr::verify_release(rd.string(), vo, vr), fr::Code::kVerifyFailed);
  }
}

void test_reproducible() {
  Env e;
  const fs::path out1 = new_dir(e.work / "out1"), out2 = new_dir(e.work / "out2"), out3 = new_dir(e.work / "out3");
  fr::BuildReport r1, r2, r3;
  CHECK_CODE(fr::build_release(e.request(e.desc(), out1), r1), fr::Code::kOk);
  // Rebuild from the same sources, keys and vaults (even with the sources somewhere else): byte-identical, every file, the index and the signed Live Sets included.
  fs::copy(e.src, e.work / "src-moved", fs::copy_options::recursive);
  fr::ReleaseDescription moved;
  CHECK_CODE(fr::parse_release_description(synth::description(), (e.work / "src-moved").string(), moved), fr::Code::kOk);
  CHECK_CODE(fr::build_release(e.request(moved, out2, (out1 / "1.0.0" / "registry-vault").string()), r2), fr::Code::kOk);
  const auto t1 = tree(out1 / "1.0.0"), t2 = tree(out2 / "1.0.0");
  CHECK(t1.size() == t2.size() && !t1.empty());
  for (const auto& [name, bytes] : t1) {
    CHECK(t2.count(name) == 1);
    if (t2.count(name)) CHECK(t2.at(name) == bytes);
  }
  CHECK(t1.at("release-index.json") == t2.at("release-index.json") && t1.at("release-index.sig") == t2.at("release-index.sig") && r1.index_sha256 == r2.index_sha256);
  CHECK(t1.at("unit/factory-live-sets.vfls") == t2.at("unit/factory-live-sets.vfls"));
  // Without the previous vaults a library gets a FRESH content key: other bundles and vaults, but still a valid release; the Live Sets (no key involved) are the same.
  CHECK_CODE(fr::build_release(e.request(e.desc(), out3), r3), fr::Code::kOk);
  const auto t3 = tree(out3 / "1.0.0");
  const std::string b = "unit/libraries/" + std::string(synth::kPianoId) + "/bundle.vlrp", v = "registry-vault/" + std::string(synth::kPianoId) + "/content-key.vault";
  CHECK(t3.at(b) != t1.at(b) && t3.at(v) != t1.at(v) && t3.at("release-index.json") != t1.at("release-index.json"));
  CHECK(t3.at("unit/factory-live-sets.vfls") == t1.at("unit/factory-live-sets.vfls"));
  // Another signing key: other signatures, the same bundles' ciphertext would differ too (the bundle manifest is signed) -- the index and the Live Sets differ.
  {
    const fs::path kd2 = e.work / "keys2";
    Keys k2 = make_keys(kd2);
    fr::BuildRequest rq = e.request(e.desc(), new_dir(e.work / "out4"), (out1 / "1.0.0" / "registry-vault").string());
    rq.kek = &e.keys.kek;  // the vaults open only with the first KEK
    rq.signing_seed = &k2.sg;
    fr::BuildReport r4;
    CHECK_CODE(fr::build_release(rq, r4), fr::Code::kOk);
    const auto t4 = tree(e.work / "out4" / "1.0.0");
    CHECK(t4.at("release-index.sig") != t1.at("release-index.sig") && t4.at("unit/factory-live-sets.vfls") != t1.at("unit/factory-live-sets.vfls") && t4.at(b) != t1.at(b));
    CHECK(t4.at(v) == t1.at(v));  // the vault entry is the same key under the same KEK
  }
  // A revision bump changes catalog.meta.sig (and with it the index) but not bundle.vlrp.
  {
    synth::Options o;
    o.revision = 2;
    fr::BuildReport r5;
    CHECK_CODE(fr::build_release(e.request(e.desc(o), new_dir(e.work / "out5"), (out1 / "1.0.0" / "registry-vault").string()), r5), fr::Code::kOk);
    const auto t5 = tree(e.work / "out5" / "1.0.0");
    const std::string seal = "unit/libraries/" + std::string(synth::kPianoId) + "/catalog.meta.sig";
    CHECK(t5.at(b) == t1.at(b) && t5.at(seal) != t1.at(seal) && r5.libraries[0].revision == 2);
  }
  // The CLI with --reuse-vaults rebuilds the same bytes (the documented way to rebuild a release).
  {
    put(e.src / "release.json", synth::description());
    const std::string base = std::string("'") + FR_EXE + "' build --description '" + (e.src / "release.json").string() + "' --keys-dir '" + e.keys_dir.string() + "' --register-file '" +
                             (e.work / "vendor-ids").string() + "' --reuse-vaults '" + (out1 / "1.0.0" / "registry-vault").string() + "' --out ";
    std::string log;
    new_dir(e.work / "cli1");
    new_dir(e.work / "cli2");
    CHECK(run_cmd(base + "'" + (e.work / "cli1").string() + "'", &log) == 0);
    CHECK(run_cmd(base + "'" + (e.work / "cli2").string() + "'", &log) == 0);
    const auto c1 = tree(e.work / "cli1" / "1.0.0"), c2 = tree(e.work / "cli2" / "1.0.0");
    CHECK(c1 == c2 && c1 == t1);
    CHECK(run_cmd(base + "'" + (e.work / "cli1").string() + "'", &log) == 13 && log.find("already exists") != std::string::npos);  // output_failed without --overwrite
    CHECK(run_cmd(base + "'" + (e.work / "cli1").string() + "' --overwrite", &log) == 0 && tree(e.work / "cli1" / "1.0.0") == t1);
  }
}

void test_overwrite_keeps_other_keys() {
  Env e;
  const fs::path out = new_dir(e.work / "out");
  fr::BuildReport r1, r2, r3;
  CHECK_CODE(fr::build_release(e.request(e.desc(), out), r1), fr::Code::kOk);
  const auto first = tree(out / "1.0.0");
  fr::BuildRequest again = e.request(e.desc(), out);
  CHECK_CODE(fr::build_release(again, r2), fr::Code::kOutputFailed);  // exists, no --overwrite
  CHECK(tree(out / "1.0.0") == first);
  // Overwrite with the same vaults: replaced, nothing kept aside.
  again.overwrite = true;
  again.reuse_vaults_dir = (out / "1.0.0" / "registry-vault").string();
  CHECK_CODE(fr::build_release(again, r2), fr::Code::kOk);
  CHECK(r2.kept_old.empty() && tree(out / "1.0.0") == first);
  // Overwrite with FRESH content keys: the index would change under the same version -> refused (raise the version); the existing release is untouched.
  again.reuse_vaults_dir.clear();
  CHECK_CODE(fr::build_release(again, r3), fr::Code::kOutputFailed);
  CHECK(tree(out / "1.0.0") == first);
  for (const auto& de : fs::directory_iterator(out)) CHECK(de.path().filename() == "1.0.0");
}

void test_refusals() {
  Env e;
  const fs::path out = new_dir(e.work / "out");
  fr::BuildReport rep;
  // Root mismatch: the keys are the dev root's, the release says factory-test.
  {
    synth::Options o;
    o.root = "factory-test";
    CHECK_CODE(fr::build_release(e.request(e.desc(o), new_dir(e.work / "o1")), rep), fr::Code::kLibraryIdUnregistered);  // dev-range ids are refused for non-dev roots, before keys matter
  }
  // Vendor-id register: random v4 ids (not the dev range) must be registered; a conflict is refused with content-pack's own error.
  const std::string id_a = "3f1d6a52-9b7e-4c1a-8d2e-5a6b7c8d9e0f", id_b = "7a8b9c0d-1e2f-4a3b-8c4d-5e6f708192a3";
  synth::Options o;
  o.piano_id = id_a;
  fr::ReleaseDescription d = e.desc(o);
  fr::BuildRequest rq = e.request(d, out);
  CHECK_CODE(fr::build_release(rq, rep), fr::Code::kRegisterIo);  // no register file at all (a non-dev-range id needs one)
  put(e.work / "vendor-ids", "# test register\n");
  CHECK_CODE(fr::build_release(rq, rep), fr::Code::kLibraryIdUnregistered);  // the pipe / strings / keys ids are dev range, which root dev may use; the piano id is not registered
  CHECK(!fs::exists(out / "1.0.0"));
  // The register binds id_a to another NAME: conflict (the same id under a new name).
  put(e.work / "vendor-ids", "# test register\n" + id_a + "\tSomebody Else's Piano\tfounder\t2026-01-01\n");
  CHECK_CODE(fr::build_release(rq, rep), fr::Code::kLibraryIdConflict);
  CHECK(!fs::exists(out / "1.0.0"));
  // The register binds the NAME to another id: conflict the other way.
  put(e.work / "vendor-ids", "# test register\n" + id_b + "\tVelora Factory Piano\tfounder\t2026-01-01\n");
  CHECK_CODE(fr::build_release(rq, rep), fr::Code::kLibraryIdConflict);
  // Registered correctly: builds. With --register the new id is appended after the pack.
  put(e.work / "vendor-ids", "# test register\n" + id_a + "\tVelora Factory Piano\tfounder\t2026-01-01\n");
  CHECK_CODE(fr::build_release(rq, rep), fr::Code::kOk);
  CHECK(fs::exists(out / "1.0.0" / "unit/libraries" / id_a / "bundle.vlrp"));
  {
    put(e.work / "vendor-ids", "# empty test register\n");
    rq.register_new = true;
    rq.owner = "founder";
    rq.out_dir = new_dir(e.work / "out-reg").string();
    CHECK_CODE(fr::build_release(rq, rep), fr::Code::kOk);
    CHECK(read_text(e.work / "vendor-ids").find(id_a + "\tVelora Factory Piano\tfounder\t2026-10-20") != std::string::npos);
  }
  // Unresolved reference: an instrument key the library does not have.
  {
    synth::Options x;
    x.piano_key = "Nope.sfz";
    CHECK_CODE(fr::build_release(e.request(e.desc(x), new_dir(e.work / "o2")), rep), fr::Code::kUnresolvedReference);
    CHECK(!fs::exists(e.work / "o2" / "1.0.0") && fs::is_empty(e.work / "o2"));  // nothing published, no staging left
  }
  // A Live Set that does not validate (tempo out of range).
  {
    fr::ReleaseDescription x = e.desc();
    x.live_sets[0].tempo = 1000.0f;
    CHECK_CODE(fr::build_release(e.request(x, new_dir(e.work / "o3")), rep), fr::Code::kInvalidLiveSet);
    x = e.desc();
    x.live_sets[0].parts[0].part = 9;  // a v1 Live Set has 8 parts
    CHECK_CODE(fr::build_release(e.request(x, new_dir(e.work / "o4")), rep), fr::Code::kInvalidLiveSet);
    // Engine parameters are checked against the registry BEFORE anything is packed: an unknown id, a value out of range, a fraction for an integer parameter.
    for (int variant = 0; variant < 4; ++variant) {
      x = e.desc();
      auto& pr = x.live_sets[1].parts[0].params[0];
      if (variant == 0) pr.param_id = 60000;
      if (variant == 1) pr.value = 9;
      if (variant == 2) pr.value = 4.5f;
      if (variant == 3) x.live_sets[1].master.push_back({999, 1.0f});
      const fs::path o5 = new_dir(e.work / ("o5_" + std::to_string(variant)));
      CHECK_CODE(fr::build_release(e.request(x, o5), rep), fr::Code::kInvalidLiveSet);
      CHECK(fs::is_empty(o5));
    }
    x = e.desc();
    CHECK_CODE(fr::build_release(e.request(x, new_dir(e.work / "o5ok")), rep), fr::Code::kOk);
  }
  // The register is touched only by the final commit: a build that fails after the packs (unresolved reference) leaves it as it was, ids and all.
  {
    put(e.work / "vendor-ids", "# test register\n");
    synth::Options x;
    x.piano_id = id_a;
    x.piano_key = "Nope.sfz";
    fr::BuildRequest r2 = e.request(e.desc(x), new_dir(e.work / "o10"));
    r2.register_new = true;
    r2.owner = "founder";
    CHECK_CODE(fr::build_release(r2, rep), fr::Code::kUnresolvedReference);
    CHECK(read_text(e.work / "vendor-ids") == "# test register\n");
    synth::Options y;
    y.piano_id = id_a;
    r2 = e.request(e.desc(y), new_dir(e.work / "o11"));
    r2.register_new = true;
    r2.owner = "founder";
    CHECK_CODE(fr::build_release(r2, rep), fr::Code::kOk);
    CHECK(read_text(e.work / "vendor-ids").find(id_a) != std::string::npos);
  }
  // --reuse-vaults: a directory that does not exist, or a library without an entry, is vault_failed unless --allow-new-keys.
  {
    fr::BuildReport first;
    CHECK_CODE(fr::build_release(e.request(e.desc(), new_dir(e.work / "o12")), first), fr::Code::kOk);
    fr::BuildRequest r3 = e.request(e.desc(), new_dir(e.work / "o13"), (e.work / "no-such-vaults").string());
    CHECK_CODE(fr::build_release(r3, rep), fr::Code::kVaultFailed);
    CHECK(fs::is_empty(e.work / "o13"));
    fs::copy(e.work / "o12" / "1.0.0" / "registry-vault", e.work / "partial-vaults", fs::copy_options::recursive);
    fs::remove_all(e.work / "partial-vaults" / synth::kKeysId);
    r3 = e.request(e.desc(), new_dir(e.work / "o14"), (e.work / "partial-vaults").string());
    CHECK_CODE(fr::build_release(r3, rep), fr::Code::kVaultFailed);
    CHECK(fs::is_empty(e.work / "o14"));
    r3.allow_new_keys = true;
    CHECK_CODE(fr::build_release(r3, rep), fr::Code::kOk);
    // the libraries that had entries keep their key (same vault bytes), the new one has a fresh key
    CHECK(read_all(e.work / "o14" / "1.0.0" / "registry-vault" / synth::kPianoId / "content-key.vault") == read_all(e.work / "o12" / "1.0.0" / "registry-vault" / synth::kPianoId / "content-key.vault"));
    CHECK(read_all(e.work / "o14" / "1.0.0" / "registry-vault" / synth::kKeysId / "content-key.vault") != read_all(e.work / "o12" / "1.0.0" / "registry-vault" / synth::kKeysId / "content-key.vault"));
    // --previous-index: program ids are never removed; content may change only with a higher release version; a library's bundle only with a higher revision.
    const std::string prev = (e.work / "o12" / "1.0.0" / "release-index.json").string();
    const std::string vaults = (e.work / "o12" / "1.0.0" / "registry-vault").string();
    fr::ReleaseDescription d2 = e.desc();
    d2.version = "1.1.0";
    fr::BuildRequest r4 = e.request(d2, new_dir(e.work / "o15"), vaults);
    r4.previous_index_path = prev;
    CHECK_CODE(fr::build_release(r4, rep), fr::Code::kOk);  // nothing changed but the version
    d2.live_sets.pop_back();
    r4 = e.request(d2, new_dir(e.work / "o16"), vaults);
    r4.previous_index_path = prev;
    CHECK_CODE(fr::build_release(r4, rep), fr::Code::kProgramHistory);  // program 4 removed
    CHECK(fs::is_empty(e.work / "o16"));
    fr::ReleaseDescription d3 = e.desc();
    d3.live_sets[0].name = "Renamed grand";  // content changed, the version is not raised
    r4 = e.request(d3, new_dir(e.work / "o17"), vaults);
    r4.previous_index_path = prev;
    CHECK_CODE(fr::build_release(r4, rep), fr::Code::kProgramHistory);
    d3.version = "1.0.1";
    r4 = e.request(d3, new_dir(e.work / "o18"), vaults);
    r4.previous_index_path = prev;
    CHECK_CODE(fr::build_release(r4, rep), fr::Code::kOk);
    // an older release than the previous one, and a changed bundle without a revision bump
    fr::ReleaseDescription d4 = e.desc();
    d4.version = "0.9.0";
    r4 = e.request(d4, new_dir(e.work / "o19"), vaults);
    r4.previous_index_path = prev;
    CHECK_CODE(fr::build_release(r4, rep), fr::Code::kProgramHistory);
    fs::copy(e.src, e.work / "src3", fs::copy_options::recursive);
    ifx::write_sfz(e.work / "src3" / "piano", "Factory_Grand", 7, 6000);
    fr::ReleaseDescription d5;
    CHECK_CODE(fr::parse_release_description(synth::description(), (e.work / "src3").string(), d5), fr::Code::kOk);
    d5.version = "1.2.0";
    r4 = e.request(d5, new_dir(e.work / "o20"), vaults);
    r4.previous_index_path = prev;
    CHECK_CODE(fr::build_release(r4, rep), fr::Code::kProgramHistory);
    d5.libraries[0].revision = 2;
    r4 = e.request(d5, new_dir(e.work / "o21"), vaults);
    r4.previous_index_path = prev;
    CHECK_CODE(fr::build_release(r4, rep), fr::Code::kOk);
    // not an index
    put(e.work / "notindex.json", "{}");
    r4 = e.request(e.desc(), new_dir(e.work / "o22"), vaults);
    r4.previous_index_path = (e.work / "notindex.json").string();
    CHECK_CODE(fr::build_release(r4, rep), fr::Code::kBadDescription);
  }
  // A source that the importer skips: import_skipped.
  {
    fs::copy(e.src, e.work / "src2", fs::copy_options::recursive);
    ifx::write_unsupported_sfz(e.work / "src2" / "piano", "Factory_Grand");
    fr::ReleaseDescription x;
    CHECK_CODE(fr::parse_release_description(synth::description(), (e.work / "src2").string(), x), fr::Code::kOk);
    CHECK_CODE(fr::build_release(e.request(x, new_dir(e.work / "o6")), rep), fr::Code::kImportSkipped);
  }
  // Keys: missing file, a key readable by others, the output directory missing.
  {
    fr::BuildRequest bad = e.request(e.desc(), new_dir(e.work / "o7"));
    bad.kek = nullptr;
    CHECK_CODE(fr::build_release(bad, rep), fr::Code::kInternal);
    bad = e.request(e.desc(), e.work / "does-not-exist");
    CHECK_CODE(fr::build_release(bad, rep), fr::Code::kOutputFailed);
    // --reuse-vaults with a vault entry of another KEK: vault_failed
    Keys k2 = make_keys(e.work / "keys3");
    fr::BuildReport ok;
    CHECK_CODE(fr::build_release(e.request(e.desc(), new_dir(e.work / "o8")), ok), fr::Code::kOk);
    bad = e.request(e.desc(), new_dir(e.work / "o9"), (e.work / "o8" / "1.0.0" / "registry-vault").string());
    bad.kek = &k2.kek;
    bad.signing_seed = &k2.sg;
    CHECK_CODE(fr::build_release(bad, rep), fr::Code::kVaultFailed);
    CHECK(fs::is_empty(e.work / "o9"));
  }
}

void test_cli() {
  Env e;
  put(e.src / "release.json", synth::description());
  const std::string exe = std::string("'") + FR_EXE + "' ";
  const std::string desc = "--description '" + (e.src / "release.json").string() + "' ";
  std::string log;
  CHECK(run_cmd(exe, &log) == 2 && log.find("usage:") != std::string::npos);
  CHECK(run_cmd(exe + "frobnicate", &log) == 2);
  CHECK(run_cmd(exe + "build " + desc, &log) == 2);  // no --out
  CHECK(run_cmd(exe + "build " + desc + "--out /tmp --signing-key 0123", &log) != 0);
  CHECK(run_cmd(exe + "build " + desc + "--register --out /tmp", &log) == 2);  // --register needs --owner
  CHECK(run_cmd(exe + "verify", &log) == 2);
  const std::string out = (e.work / "cli").string();
  fs::create_directories(out);
  CHECK(run_cmd(exe + "build " + desc + "--out '" + out + "' --keys-dir '" + e.keys_dir.string() + "' --register-file '" + (e.work / "vendor-ids").string() + "'", &log) == 0);
  CHECK(log.find("built Velora Factory Library 1.0.0") != std::string::npos && log.find("program   4  Split Keys") != std::string::npos && log.find("Never ship registry-vault/") != std::string::npos);
  const std::string rel = out + "/1.0.0";
  put(e.work / "trust.pub-list", "dev content-manifest " + hex(e.keys.id) + " " + hex(e.keys.pub) + "\n");
  put_bytes(e.work / "signer.pub", e.keys.pub);
  CHECK(run_cmd(exe + "verify '" + rel + "' --trust-list '" + (e.work / "trust.pub-list").string() + "'", &log) == 0 && log.find("ok: Velora Factory Library 1.0.0 (root dev)") != std::string::npos);
  CHECK(run_cmd(exe + "verify '" + rel + "' --trusted-pub '" + (e.work / "signer.pub").string() + "' --bundles", &log) == 0 && log.find("4 bundle(s) re-verified") != std::string::npos);
  CHECK(run_cmd(exe + "verify '" + rel + "'", &log) == 2);  // a trusted key is required
  CHECK(run_cmd(exe + "verify '" + rel + "' --trusted-pub '" + (e.work / "signer.pub").string() + "' --trust-list '" + (e.work / "trust.pub-list").string() + "'", &log) == 2);
  // A tampered release: exit 14 and the exact problem on stderr.
  flip(fs::path(rel) / "unit/factory-live-sets.vfls", 120);
  CHECK(run_cmd(exe + "verify '" + rel + "' --trusted-pub '" + (e.work / "signer.pub").string() + "'", &log) == 14 && log.find("hash_mismatch unit/factory-live-sets.vfls") != std::string::npos);
  // A trust list that is not one.
  CHECK(run_cmd(exe + "verify '" + rel + "' --trust-list '" + (e.work / "nope").string() + "'", &log) == 4);
  // A key on the command line is not accepted: an option that would carry key bytes is a usage error (exit 2), and the bytes are not echoed.
  {
    const std::string secret = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    for (const char* opt : {"--signing-seed", "--kek-hex", "--content-key", "--seed"}) {
      CHECK(run_cmd(exe + "build " + desc + "--out '" + out + "' " + opt + " " + secret, &log) == 2);
      CHECK(log.find(secret) == std::string::npos && log.find("usage:") != std::string::npos);
    }
  }
  // A root other than dev needs explicit keys: the default dev key directory is not used.
  {
    synth::Options o;
    o.root = "factory-test";
    put(e.src / "release-ft.json", synth::description(o));
    CHECK(run_cmd(exe + "build --description '" + (e.src / "release-ft.json").string() + "' --out '" + out + "'", &log) == 4 && log.find("needs --keys-dir") != std::string::npos);
    CHECK(run_cmd(exe + "build --description '" + (e.src / "release-ft.json").string() + "' --out '" + out + "' --keys-dir '" + e.keys_dir.string() + "'", &log) == 5);  // sidecars say dev
  }
}

// A non-dev root end to end through the CLI: keys with factory-test sidecars, random v4 ids that must be in the register (--register adds them at the final commit),
// a trust list line for root factory-test, and a dev key refused for that root.
void test_factory_test_root() {
  Env e;
  const fs::path ftkeys = e.work / "ftkeys";
  Keys ft = make_keys(ftkeys, "factory-test");
  synth::Options o;
  o.root = "factory-test";
  o.piano_id = "3f1d6a52-9b7e-4c1a-8d2e-5a6b7c8d9e0f";
  o.pipe_id = "7a8b9c0d-1e2f-4a3b-8c4d-5e6f708192a3";
  o.strings_id = "0b1c2d3e-4f50-4a61-8b72-93a4b5c6d7e8";
  o.keys_id = "1c2d3e4f-5061-4b72-9c83-a4b5c6d7e8f9";
  put(e.src / "release-ft.json", synth::description(o));
  const std::string exe = std::string("'") + FR_EXE + "' ";
  const std::string common = "build --description '" + (e.src / "release-ft.json").string() + "' --keys-dir '" + ftkeys.string() + "' --register-file '" + (e.work / "ids").string() + "' --out ";
  std::string log;
  const fs::path out = new_dir(e.work / "ft-out");
  put(e.work / "ids", "# register\n");
  CHECK(run_cmd(exe + common + "'" + out.string() + "'", &log) == 7);  // not registered
  CHECK(read_text(e.work / "ids") == "# register\n" && fs::is_empty(out));
  CHECK(run_cmd(exe + common + "'" + out.string() + "' --register --owner founder --date 2026-10-20", &log) == 0);
  const std::string reg = read_text(e.work / "ids");
  for (const std::string& id : {o.piano_id, o.pipe_id, o.strings_id, o.keys_id}) CHECK(reg.find(id) != std::string::npos);
  // Registered now: a rebuild (same vaults) needs no --register, and is identical.
  CHECK(run_cmd(exe + common + "'" + out.string() + "' --overwrite --reuse-vaults '" + (out / "1.0.0/registry-vault").string() + "'", &log) == 0);
  const fs::path rel = out / "1.0.0";
  put(e.work / "ft.pub-list", "factory-test content-manifest " + hex(ft.id) + " " + hex(ft.pub) + "\n");
  CHECK(run_cmd(exe + "verify '" + rel.string() + "' --trust-list '" + (e.work / "ft.pub-list").string() + "' --bundles", &log) == 0 && log.find("(root factory-test)") != std::string::npos);
  // The same key listed for root dev does not vouch for a factory-test release (the signer is bound to its root).
  put(e.work / "dev.pub-list", "dev content-manifest " + hex(ft.id) + " " + hex(ft.pub) + "\n");
  CHECK(run_cmd(exe + "verify '" + rel.string() + "' --trust-list '" + (e.work / "dev.pub-list").string() + "'", &log) == 14 && log.find("index_root_mismatch") != std::string::npos);
  // A dev key set is refused for root factory-test (sidecar says dev): root_mismatch.
  CHECK(run_cmd(exe + common + "'" + out.string() + "' --overwrite --keys-dir '" + e.keys_dir.string() + "'", &log) == 5);
  // Dev-range ids are refused outside root dev.
  put(e.src / "release-ft2.json", synth::description([] { synth::Options x; x.root = "factory-test"; return x; }()));
  CHECK(run_cmd(exe + "build --description '" + (e.src / "release-ft2.json").string() + "' --keys-dir '" + ftkeys.string() + "' --register-file '" + (e.work / "ids").string() + "' --out '" +
                    out.string() + "' --overwrite", &log) == 7);
}

void test_verify_tamper() {
  Env e;
  const fs::path out = new_dir(e.work / "out");
  fr::BuildReport rep;
  CHECK_CODE(fr::build_release(e.request(e.desc(), out), rep), fr::Code::kOk);
  const fs::path good = out / "1.0.0";
  const std::string pid = synth::kPianoId;
  const std::string bundle = "unit/libraries/" + pid + "/bundle.vlrp", meta = "unit/libraries/" + pid + "/catalog.meta", seal = "unit/libraries/" + pid + "/catalog.meta.sig",
                    vault = "registry-vault/" + pid + "/content-key.vault", lsf = "unit/factory-live-sets.vfls";
  const fs::path work = e.work / "t";
  auto codes = [&](bool bundles = false) { return verify_codes(e, work, bundles); };
  using S = std::set<std::string>;
  // The control: untouched.
  copy_release(good, work);
  CHECK(codes(true).empty());

  // Any change to a hashed file is reported with the file: bundle, catalog, seal, report, vault, Live Sets (a single bit anywhere).
  for (const std::string& f : {bundle, meta, seal, vault, lsf, "unit/libraries/" + pid + "/pack.json"}) {
    copy_release(good, work);
    flip(work / f, 8, 0x01);
    // (the Live Set file is also parsed: a changed key id there is an untrusted signer)
    CHECK(codes() == (f == lsf ? S({"hash_mismatch", "live_sets_invalid"}) : S({"hash_mismatch"})));
    fr::VerifyReport r;
    (void)fr::verify_release(work.string(), e.vopts(), r);
    CHECK(r.problems.size() == (f == lsf ? 2u : 1u) && r.problems[0].path == f);
  }
  // A flip in a ciphertext block deep in a bundle.
  {
    copy_release(good, work);
    flip(work / bundle, fs::file_size(work / bundle) - 3);
    CHECK(codes() == S({"hash_mismatch"}));
  }
  // Size: truncated / extended.
  {
    copy_release(good, work);
    fs::resize_file(work / bundle, fs::file_size(work / bundle) - 1);
    CHECK(codes() == S({"size_mismatch"}));
    copy_release(good, work);
    { std::ofstream(work / meta, std::ios::binary | std::ios::app) << "x"; }
    CHECK(codes() == S({"size_mismatch"}));
  }
  // Missing / extra / non-regular files.
  {
    copy_release(good, work);
    fs::remove(work / bundle);
    CHECK(codes() == S({"file_missing"}));
    copy_release(good, work);
    put(work / "unit/extra.txt", "planted");
    CHECK(codes() == S({"unexpected_file"}));
    copy_release(good, work);
    put(work / ("registry-vault/" + pid + "/second.vault"), "planted");
    CHECK(codes() == S({"unexpected_file"}));
    copy_release(good, work);
    fs::remove(work / meta);
    fs::create_symlink(work / "release-index.json", work / meta);
    CHECK(codes() == S({"file_not_regular"}));
    copy_release(good, work);
    fs::create_symlink("/etc/hosts", work / "unit/link");
    CHECK(codes() == S({"unexpected_entry"}));
    copy_release(good, work);
    ::mkfifo((work / "unit/fifo").c_str(), 0600);
    CHECK(codes() == S({"unexpected_entry"}));
  }
  // The index and its signature.
  {
    copy_release(good, work);
    flip(work / "release-index.json", 100, 0x01);
    CHECK(codes() == S({"index_signature_invalid"}));
    copy_release(good, work);
    flip(work / "release-index.sig", 40);
    CHECK(codes() == S({"index_signature_invalid"}));
    copy_release(good, work);
    flip(work / "release-index.sig", 10);  // the signer key id
    CHECK(codes() == S({"index_untrusted_key"}));
    copy_release(good, work);
    flip(work / "release-index.sig", 0);  // magic
    CHECK(codes() == S({"index_sig_malformed"}));
    copy_release(good, work);
    fs::resize_file(work / "release-index.sig", 79);
    CHECK(codes() == S({"index_sig_malformed"}));
    copy_release(good, work);
    fs::remove(work / "release-index.sig");
    CHECK(codes() == S({"index_missing"}));
    copy_release(good, work);
    fs::remove(work / "release-index.json");
    CHECK(codes() == S({"index_missing"}));
    // Every single-bit flip of the 80-byte signature file is refused.
    std::size_t refused = 0;
    for (std::size_t i = 0; i < 80; ++i) {
      copy_release(good, work);
      flip(work / "release-index.sig", i, 0x01);
      fr::VerifyReport r;
      if (fr::verify_release(work.string(), e.vopts(), r).code == fr::Code::kVerifyFailed) ++refused;
    }
    CHECK(refused == 80);
    // A release that someone else signed, with their own consistent index, is not trusted.
    Env other;
    fr::BuildReport orep;
    CHECK_CODE(fr::build_release(other.request(other.desc(), new_dir(other.work / "o")), orep), fr::Code::kOk);
    fr::VerifyReport r;
    CHECK_CODE(fr::verify_release((other.work / "o" / "1.0.0").string(), e.vopts(), r), fr::Code::kVerifyFailed);
    CHECK(problem_codes(r) == S({"index_untrusted_key"}));
  }
  // The index re-signed by the key holder after a file changed: hashes agree, but --bundles still finds a damaged bundle / catalog seal / Live Sets.
  {
    copy_release(good, work);
    flip(work / bundle, fs::file_size(work / bundle) - 3);
    reindex(e, work, bundle);
    CHECK(codes(false).empty());                                          // the hash check alone cannot see it ...
    CHECK(codes(true) == S({"bundle_block_invalid"}));                     // ... re-verifying the bundle does
    copy_release(good, work);
    flip(work / bundle, 40);  // inside the header / manifest region
    reindex(e, work, bundle);
    CHECK(!codes(true).empty() && codes(true).count("bundle_invalid") == 1);
    copy_release(good, work);
    flip(work / meta, 5);
    reindex(e, work, meta);
    CHECK(codes(false).empty() && codes(true) == S({"bundle_meta_seal_invalid"}));
    copy_release(good, work);
    flip(work / lsf, 100);
    reindex(e, work, lsf);
    CHECK(codes(false) == S({"live_sets_invalid"}));  // the Live Set container is always checked
  }
  // A swapped bundle: library A's bundle under library B's name (its hash is re-indexed): identity mismatch.
  {
    copy_release(good, work);
    fs::copy_file(work / "unit/libraries" / synth::kPipeId / "bundle.vlrp", work / bundle, fs::copy_options::overwrite_existing);
    reindex(e, work, bundle);
    const S c = codes(true);
    CHECK(c.count("bundle_identity_mismatch") == 1);
  }
  // A Live Set file of another release (older, valid under the same key) does not match this index.
  {
    synth::Options o;
    o.version = "0.9.0";
    fr::BuildReport orep;
    CHECK_CODE(fr::build_release(e.request(e.desc(o), new_dir(e.work / "older"), (good / "registry-vault").string()), orep), fr::Code::kOk);
    copy_release(good, work);
    fs::copy_file(e.work / "older" / "0.9.0" / lsf, work / lsf, fs::copy_options::overwrite_existing);
    reindex(e, work, lsf);
    CHECK(codes(false) == S({"live_sets_invalid"}));
  }
  // The index re-signed with its library / program table changed: each field is checked against the files it names.
  {
    const std::string dflt = hf::derive_live_set_id(1);
    copy_release(good, work);
    edit_index(e, work, "\"library_id\": \"" + pid + "\"", "\"library_id\": \"00000000-0000-4000-8000-0000000000aa\"");
    const S c = codes(true);
    CHECK(c.count("layout_invalid") == 1 && c.count("bundle_identity_mismatch") == 1);
    copy_release(good, work);
    edit_index(e, work, "\"revision\": 1,", "\"revision\": 2,");
    CHECK(codes(true) == S({"bundle_revision_mismatch"}));
    CHECK(codes(false).empty());  // (only the bundle's own seal knows the revision)
    copy_release(good, work);
    edit_index(e, work, "\"live_set_id\": \"" + dflt + "\"", "\"live_set_id\": \"" + hf::derive_live_set_id(99) + "\"");
    CHECK(codes(false) == S({"live_sets_invalid"}));
    copy_release(good, work);
    edit_index(e, work, "\"program_id\": 2,", "\"program_id\": 12,");
    CHECK(codes(false) == S({"live_sets_invalid"}));
    {
      std::string idx = read_text(good / "release-index.json");  // a program whose recorded content hash differs
      const std::size_t at = idx.find("\"sha256\"", idx.find("\"programs\""));
      copy_release(good, work);
      edit_index(e, work, idx.substr(at, 20), "\"sha256\": \"00000000000");
      CHECK(codes(false) == S({"live_sets_invalid"}));
    }
    // Layout: a vault entry that would ship under unit/, a library file somewhere else, a path outside unit/ and registry-vault/.
    copy_release(good, work);
    fs::copy_file(work / vault, work / "unit/libraries" / pid / "content-key.vault");
    {
      std::uint64_t sz = 0;
      const std::string sha = fr::sha256_file_hex((work / vault).string(), &sz);
      edit_index(e, work, "\"files\": [", "\"files\": [\n    {\"path\": \"unit/libraries/" + pid + "/content-key.vault\", \"size\": " + std::to_string(sz) + ", \"sha256\": \"" + sha + "\"},");
    }
    CHECK(codes(false) == S({"layout_invalid"}));
    copy_release(good, work);
    edit_index(e, work, "\"vault\": \"" + vault + "\"", "\"vault\": \"unit/libraries/" + pid + "/pack.json\"");
    CHECK(codes(false) == S({"layout_invalid"}));
    copy_release(good, work);
    fs::create_directories(work / "elsewhere");
    fs::rename(work / "unit/libraries" / pid / "pack.json", work / "elsewhere/pack.json");
    edit_index(e, work, "\"path\": \"unit/libraries/" + pid + "/pack.json\"", "\"path\": \"elsewhere/pack.json\"");
    {
      const S cc = codes(false);
      CHECK(cc.count("layout_invalid") == 1 && cc.count("library_file_not_in_index") == 1);
    }
  }
  // A release directory that is not one.
  CHECK(verify_codes(e, e.work / "nope") == S({"index_missing"}));
}

// ---- install on the Mac host and play (dev key provider builds) ---------------------------------------------------------------------------------

#ifdef FR_HAS_DEV_KEYS
namespace lsp = velora::liveset::v1;
namespace ls = velora::host::liveset;
namespace core = velora::core;
namespace host = velora::host;

// The registry's job for one unit: unwrap the vaulted content key with the KEK and wrap it to the device's public key.
std::string wrap_for_device(const Env& e, const fs::path& release, const std::string& library_id) {
  const Bytes v = read_all(release / "registry-vault" / library_id / "content-key.vault");
  cp::Key32 ck{};
  CHECK(cp::vault_entry_open(e.keys.kek, v, library_id, ck));
  std::array<std::uint8_t, cp::kWrappedKeySize> blob{};
  CHECK(cp::wrapped_key_seal_random(ck, library_id, e.keys.dev_pub, blob));
  const fs::path p = e.work / "wrapped" / (library_id + ".bin");
  put_bytes(p, blob, 0600);
  cp::wipe(ck.data(), ck.size());
  return p.string();
}

std::string vfls_path(const fs::path& rd) { return (rd / "unit/factory-live-sets.vfls").string(); }

struct Engine {
  Env& env;
  std::string data_root;
  std::unique_ptr<host::DevKeyProvider> provider;
  host::DamageRegistry damage;
  std::shared_ptr<host::FsPort> fsp = host::make_posix_fs();
  std::unique_ptr<core::BlockRenderer> br = std::make_unique<core::BlockRenderer>(8);
  core::SampleEngine sample{[] {
    core::SampleEngine::Config c;
    c.max_voices = 340;
    return c;
  }()};
  core::OrganEngine organ;
  host::ControlEventQueue rt_events;
  host::ControlPlane plane;
  host::LibraryResolver resolver;
  std::unique_ptr<host::ProtectedHeadProvider> heads;
  std::unique_ptr<host::LiveSetLoader> loader;
  float l[core::kBlockFrames], r[core::kBlockFrames];

  Engine(Env& e, const std::string& root)
      : env(e), data_root(root), plane(br->snapshots(), br->last_values(), rt_events, [] { return std::uint64_t{0}; }), resolver(fsp, root) {
    std::string err;
    provider = host::DevKeyProvider::from_bytes(e.keys.dev_priv, {}, err);
    CHECK(provider != nullptr);
    CHECK(br->bind_engine(sample));
    CHECK(br->bind_engine(organ));
    resolver.set_protected_trust(std::make_shared<const fmt::TrustedKeys>(e.keys.trusted));
    heads = std::make_unique<host::ProtectedHeadProvider>(fsp, *provider, &damage);
    plane.set_damage_registry(&damage);
    loader = std::make_unique<host::LiveSetLoader>(*br, plane, resolver, *heads, &sample);
  }
  // Plays a chord (keys under the zone of every part) for `blocks` blocks and returns the peak.
  float play(const std::vector<std::uint32_t>& keys, int blocks = 24) {
    for (std::uint32_t k : keys) CHECK(br->submit({core::make_note_on(0, 0, k, 65535), 0}));
    float peak = 0;
    for (int i = 0; i < blocks; ++i) {
      br->process_block(l, r);
      for (std::uint32_t f = 0; f < core::kBlockFrames; ++f) peak = std::max({peak, std::fabs(l[f]), std::fabs(r[f])});
    }
    for (std::uint32_t k : keys) CHECK(br->submit({core::make_note_off(0, 0, k, 0), 0}));
    for (int i = 0; i < 400; ++i) br->process_block(l, r);
    (void)plane.reclaim();
    return peak;
  }
};

void test_install_and_play() {
  Env e;
  const fs::path out = new_dir(e.work / "out");
  fr::BuildReport rep;
  const fr::ReleaseDescription desc = e.desc();
  CHECK_CODE(fr::build_release(e.request(desc, out), rep), fr::Code::kOk);
  const fs::path rd = out / "1.0.0";
  fr::VerifyReport vr;
  CHECK_CODE(fr::verify_release(rd.string(), e.vopts(true), vr), fr::Code::kOk);

  // 1. Install every bundle with the host librarian (the dev trust list is the release root's key), each with the wrapped key the registry would deliver.
  const std::string data = (e.work / "data").string();
  fs::create_directories(data);
  for (const auto& l : rep.libraries) {
    lib::InstallOptions io;
    io.root = data;
    io.source_dir = (rd / "unit/libraries" / l.library_id).string();
    io.wrapped_key_path = wrap_for_device(e, rd, l.library_id);
    io.trusted = &e.keys.trusted;
    const lib::InstallResult ir = lib::install_protected(io);
    CHECK(ir.status == lib::InstallStatus::kInstalled && ir.bundle_id == l.bundle_id && ir.pack_revision == 1);
    if (!ir.ok()) std::printf("   install %s: %s %s\n", l.library_id.c_str(), lib::to_string(ir.status), ir.detail.c_str());
  }
  // The untrusted key (the stock dev list) refuses them: nothing installs on trust.
  {
    fmt::TrustedKeys none;
    lib::InstallOptions io;
    io.root = (e.work / "data-untrusted").string();
    fs::create_directories(io.root);
    io.source_dir = (rd / "unit/libraries" / rep.libraries[0].library_id).string();
    io.wrapped_key_path = (e.work / "wrapped" / (rep.libraries[0].library_id + ".bin")).string();
    io.trusted = &none;
    CHECK(lib::install_protected(io).reason == lib::InstallReason::kUntrustedKey);
  }

  // 2. Every factory instrument plays: a note (a chord across the keyboard) through the engine's loader, resolver, dev key provider and decrypting head provider
  //    gives sound.
  Engine eng(e, data);
  std::size_t instruments = 0;
  for (const auto& l : rep.libraries) {
    for (const auto& ins : l.instruments) {
      lsp::LiveSet probe = ls::make_default_live_set("5b1c2d3e-4f50-4a61-8b72-93a4b5c6d7e8", "probe", 8);
      probe.mutable_parts(0)->mutable_timbre()->set_library_id(l.library_id);
      probe.mutable_parts(0)->mutable_timbre()->set_instrument_key(ins.key);
      probe.mutable_parts(0)->set_volume(1.0f);
      const host::LiveSetLoadResult res = eng.loader->load(probe);
      CHECK(res.ok() && res.parts[0].state == core::PartState::kReady && res.parts[0].resolve == host::ResolveStatus::kOk);
      if (!res.ok()) std::printf("   load %s / %s: %s %s\n", l.library_id.c_str(), ins.key.c_str(), host::to_string(res.status), res.detail.c_str());
      const float peak = eng.play({40, 52, 60, 64, 72, 84});
      CHECK(peak > 0.001f);
      if (!(peak > 0.001f)) std::printf("   %s / %s is silent (peak %g)\n", l.name.c_str(), ins.key.c_str(), peak);
      ++instruments;
    }
  }
  CHECK(instruments == 6);

  // 3. Each instrument shows its category: through the librarian service (ListLibraries / ListInstruments), which reads the installed bundles and their sealed catalog.meta.
  {
    lib::LibraryServiceOptions o;
    o.data_dir = data;
    o.socket_path = e.work.string() + "/s.sock";
    if (o.socket_path.size() > 90) o.socket_path = "/tmp/vl_fr_" + std::to_string(::getpid()) + ".sock";
    o.trusted_keys = e.keys.trusted;
    lib::LibraryService svc(o);
    std::string err;
    CHECK(svc.start(err));
    ltest::Client c;
    CHECK(c.connect(o.socket_path) && c.handshake("fr"));
    ws::v1::LibraryClientMessage m;
    m.mutable_list_libraries();
    auto lr = c.call(m);
    CHECK(lr && lr->has_library_list() && lr->library_list().total() == 4);
    std::size_t seen = 0;
    for (const auto& l : rep.libraries) {
      ws::v1::LibraryClientMessage li;
      li.mutable_list_instruments()->set_library_id(l.library_id);
      auto reply = c.call(li);
      CHECK(reply && reply->has_instrument_list() && reply->instrument_list().instruments_size() == static_cast<int>(l.instruments.size()));
      if (!reply || !reply->has_instrument_list()) continue;
      for (const auto& want : l.instruments) {
        bool found = false;
        for (const auto& got : reply->instrument_list().instruments()) {
          if (got.instrument_key() != want.key) continue;
          found = true;
          // The expected category comes from the DESCRIPTION (an instrument override, else the library's default), not from the tool's own report.
          std::string expect_cat;
          std::string expect_name = want.name;
          for (const auto& dl : desc.libraries) {
            if (dl.pack.library_id != l.library_id) continue;
            expect_cat = dl.pack.category;
            for (const auto& ov : dl.pack.instruments)
              if (ov.key == want.key && !ov.category.empty()) expect_cat = ov.category;
          }
          const auto cat = fmt::parse_assignable_category(expect_cat);
          CHECK(cat && got.category() == static_cast<std::uint32_t>(*cat) && got.name() == want.name && want.category == std::string(fmt::key(*cat)));
          ++seen;
        }
        CHECK(found);
      }
    }
    CHECK(seen == 6);
    for (const auto& li : lr->library_list().libraries()) CHECK(li.kind() == ws::v1::LIBRARY_KIND_FACTORY);
    svc.stop();
  }

  // 4. First boot: the engine's control thread imports the signed factory Live Sets through the single writer; nothing is missing; each loads and sounds.
  const std::string ls_dir = (e.work / "live_sets").string();
  fs::create_directories(ls_dir);
  ls::StoreOptions so;
  so.dir = ls_dir;
  std::unique_ptr<ls::LiveSetStore> store;
  CHECK(ls::LiveSetStore::open(so, store) == ls::Status::kOk);
  hf::InstallOptions io;
  io.trusted = &e.keys.trusted;
  io.resolves = hf::resolver_of(eng.resolver);
  const std::string vfls = (rd / "unit/factory-live-sets.vfls").string();
  hf::InstallResult fi = hf::install_factory_live_sets_file(*store, vfls, io);
  CHECK(fi.ok() && fi.imported == std::vector<std::uint32_t>({1, 2, 3, 4}) && fi.missing.empty() && fi.release_version == "1.0.0");
  if (!fi.ok()) std::printf("   factory install: %s %s\n", hf::to_string(fi.status), fi.detail.c_str());
  for (const auto& p : rep.programs) {
    ls::LoadResult lr;
    CHECK(store->read(p.live_set_id, lr) == ls::Status::kOk && hf::is_factory(lr.live_set) && lr.live_set.name() == p.name);
    const host::LiveSetLoadResult res = eng.loader->load(lr.live_set);
    CHECK(res.ok());
    std::size_t with_timbre = 0;
    for (int i = 0; i < lr.live_set.parts_size(); ++i) {
      if (!lr.live_set.parts(i).has_timbre()) continue;
      ++with_timbre;
      CHECK(res.parts[static_cast<std::size_t>(i)].state == core::PartState::kReady && res.parts[static_cast<std::size_t>(i)].resolve == host::ResolveStatus::kOk);  // no Missing library
    }
    CHECK(with_timbre == (p.program_id == 1 ? 1u : p.program_id == 2 ? 0u : 2u));
    for (const auto& part : res.parts) CHECK(part.state != core::PartState::kMissingLibrary && part.state != core::PartState::kEngineNotAvailable);
    // Every key zone of the program sounds on its own: the split program has the lower zone (key 48: the SF2 part) and the upper (key 60 + one octave: the pipes);
    // the others sound across the keyboard.
    const std::vector<std::vector<std::uint32_t>> zones = p.program_id == 4 ? std::vector<std::vector<std::uint32_t>>{{48}, {60}} : std::vector<std::vector<std::uint32_t>>{{48}, {72}};
    for (const auto& z : zones) {
      const float peak = eng.play(z);
      CHECK(peak > 0.001f);
      if (!(peak > 0.001f)) std::printf("   program %u (%s) is silent at key %u\n", p.program_id, p.name.c_str(), z[0]);
    }
  }
  // Idempotent on the next boot; a second engine on the same store directory is refused by the writer lock.
  const std::uint64_t m = store->mutations();
  fi = hf::install_factory_live_sets_file(*store, vfls, io);
  CHECK(fi.ok() && fi.imported.empty() && fi.unchanged.size() == 4 && store->mutations() == m);

  // 4b. The real tools as processes: `librarian install-protected` of one library into another root (same bytes as the in-process install), and the real engine-host
  //     importing the signed Live Sets on first boot with the installed libraries (--factory-live-sets-required: a refusal would be fatal), loading the first Live Set.
#ifdef LIBRARIAN_EXE
  {
    const std::string tl = (e.work / "dev.pub-list").string();
    put(tl, "dev content-manifest " + hex(e.keys.id) + " " + hex(e.keys.pub) + "\n");
    const std::string root2 = (e.work / "data-cli").string();
    fs::create_directories(root2);
    const auto& l = rep.libraries[2];
    std::string log;
    CHECK(run_cmd(std::string("'") + LIBRARIAN_EXE + "' install-protected '" + (rd / "unit/libraries" / l.library_id).string() + "' --root '" + root2 + "' --wrapped-key '" +
                      (e.work / "wrapped" / (l.library_id + ".bin")).string() + "' --trust-list '" + tl + "'", &log) == 0 && log.find("installed") != std::string::npos);
    CHECK(read_all(fs::path(root2) / "lib" / l.library_id / l.bundle_id / "bundle.vlrp") == read_all(fs::path(data) / "lib" / l.library_id / l.bundle_id / "bundle.vlrp"));
#ifdef ENGINE_HOST_EXE
    const std::string eh_data = (e.work / "eh-data").string();
    CHECK(run_cmd(std::string("'") + ENGINE_HOST_EXE + "' --no-audio --duration 1 --data-dir '" + eh_data + "' --library-root '" + data + "' --socket '/tmp/vl_fr_eh_" + std::to_string(::getpid()) +
                      ".sock' --trust-list '" + tl + "' --dev-device-key '" + (e.keys_dir / "device.x25519.priv").string() + "' --factory-live-sets '" + vfls_path(rd) +
                      "' --factory-live-sets-required", &log) == 0);
    CHECK(log.find(": 4 imported, 0 updated, 0 unchanged, 0 deleted by the user, 0 conflict(s)") != std::string::npos && log.find("live set part 0: ready") != std::string::npos &&
          log.find("missing_library") == std::string::npos);
    std::size_t n = 0;
    for (const auto& de : fs::directory_iterator(eh_data + "/live_sets")) n += de.path().extension() == ".liveset";
    CHECK(n == 4);
    // The engine-host process was given a store in a fresh data dir; a second boot imports nothing.
    CHECK(run_cmd(std::string("'") + ENGINE_HOST_EXE + "' --no-audio --duration 1 --data-dir '" + eh_data + "' --library-root '" + data + "' --socket '/tmp/vl_fr_eh_" + std::to_string(::getpid()) +
                      ".sock' --trust-list '" + tl + "' --dev-device-key '" + (e.keys_dir / "device.x25519.priv").string() + "' --factory-live-sets '" + vfls_path(rd) + "'", &log) == 0);
    CHECK(log.find("0 imported, 0 updated, 4 unchanged") != std::string::npos);
#endif
  }
#endif

  // 5. A unit that lacks a library: the install is refused with every missing (library_id, instrument_key), and nothing is written.
  {
    const std::string empty_root = (e.work / "data-empty").string();
    fs::create_directories(empty_root);
    Engine bare(e, empty_root);
    const std::string d2 = (e.work / "live_sets2").string();
    fs::create_directories(d2);
    ls::StoreOptions so2;
    so2.dir = d2;
    std::unique_ptr<ls::LiveSetStore> st2;
    CHECK(ls::LiveSetStore::open(so2, st2) == ls::Status::kOk);
    hf::InstallOptions io2;
    io2.trusted = &e.keys.trusted;
    io2.resolves = hf::resolver_of(bare.resolver);
    const hf::InstallResult bad = hf::install_factory_live_sets_file(*st2, vfls, io2);
    CHECK(bad.status == hf::FactoryStatus::kUnresolvedReferences && bad.missing.size() == 5);  // program 1: 1, program 2 (organ): 0, program 3: 2, program 4: 2
    std::set<std::pair<std::string, std::string>> pairs;
    for (const auto& mr : bad.missing) pairs.insert({mr.library_id, mr.instrument_key});
    CHECK(pairs.count({synth::kPianoId, "Factory_Grand.sfz"}) && pairs.count({synth::kStringsId, "Factory_Strings.sfz"}) && pairs.count({synth::kKeysId, "0:0"}) &&
          pairs.count({synth::kPipeId, "Factory_Pipe.sfz"}));
    std::size_t files = 0;
    for (const auto& de : fs::directory_iterator(d2)) files += de.path().filename() == ".writer.lock" ? 1 : 100;
    CHECK(files == 1);  // only the writer lock file
  }
  // 6. The same install with the wrong release trust (a different root's key) is refused before anything is read.
  {
    Env other;
    hf::InstallOptions io3 = io;
    io3.trusted = &other.keys.trusted;
    CHECK(hf::install_factory_live_sets_file(*store, vfls, io3).status == hf::FactoryStatus::kUntrustedKey);
  }
}
#endif

}  // namespace

int main() {
  test_description();
  test_build_layout_and_index();
  test_reproducible();
  test_overwrite_keeps_other_keys();
  test_refusals();
  test_cli();
  test_factory_test_root();
  test_verify_tamper();
#ifdef FR_HAS_DEV_KEYS
  test_install_and_play();
#else
  std::printf("factory_release_test: dev key provider not built: the install-and-play part is SKIPPED\n");
  if (g_fail == 0) return 77;  // ctest reports a skip (SKIP_RETURN_CODE 77), not a pass
#endif
  if (g_fail == 0) std::printf("factory_release_test: ok\n");
  return g_fail == 0 ? 0 : 1;
}
