// Story 6.4: install of protected bundles by the librarian (library level and CLI; the Library API over a socket is install_service_test).
//
// Covered: the happy path (what lands where, `current`, no staging, nothing from the pack that must not be copied), idempotence and bundle revisions, downgrade /
// unprotected / user-library refusals, trust and signature, the library id, the wrapped-key blob (structure only), the flipped-bit sweep over header, manifest, index,
// seal, blocks, catalog.meta and catalog.meta.sig, truncation, space, cancel, drive removal, a fault at every stage and the recovery after it, bounded memory,
// the catalog.meta binding, the Factory rows (describe_bundle, reconcile) and the `librarian install-protected` CLI.
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <new>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>

#include "lib_test_util.hpp"
#include "protected_fixtures.hpp"
#include "velora/format/protected_format.hpp"
#include "velora/librarian/catalog.hpp"
#include "velora/librarian/install.hpp"
#include "velora/librarian/library_index.hpp"
#include "velora/librarian/library_reconcile.hpp"
#include "velora/librarian/preflight.hpp"
#include "velora/librarian/trust.hpp"

// --- an allocation tracker for the bounded-memory tests: every operator new in this program is counted while `g_track` is set ---------------------------------
namespace {
std::atomic<bool> g_track{false};
std::atomic<long long> g_live{0}, g_peak{0};
std::atomic<std::size_t> g_max_one{0};
void note_alloc(std::size_t n) {
  if (!g_track.load(std::memory_order_relaxed)) return;
  const long long live = g_live.fetch_add(static_cast<long long>(n)) + static_cast<long long>(n);
  long long pk = g_peak.load();
  while (live > pk && !g_peak.compare_exchange_weak(pk, live)) {}
  std::size_t m = g_max_one.load();
  while (n > m && !g_max_one.compare_exchange_weak(m, n)) {}
}
}  // namespace
void* operator new(std::size_t n) {
  void* p = std::malloc(n + 16);
  if (!p) throw std::bad_alloc();
  *static_cast<std::size_t*>(p) = n;
  note_alloc(n);
  return static_cast<char*>(p) + 16;
}
void operator delete(void* q) noexcept {
  if (!q) return;
  char* b = static_cast<char*>(q) - 16;
  if (g_track.load(std::memory_order_relaxed)) g_live.fetch_sub(static_cast<long long>(*reinterpret_cast<std::size_t*>(b)));
  std::free(b);
}
void operator delete(void* q, std::size_t) noexcept { ::operator delete(q); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete[](void* q) noexcept { ::operator delete(q); }
void operator delete[](void* q, std::size_t) noexcept { ::operator delete(q); }

using namespace velora::librarian;
namespace fs = std::filesystem;
namespace fmt = velora::format;
using ltest::TempDir;
using pfx::Bytes;

namespace {

std::uint64_t rd64(const std::uint8_t* p) {
  std::uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

const pfx::Keys& K1() {
  static const pfx::Keys k = pfx::make_keys(1);
  return k;
}
const pfx::Keys& K2() {
  static const pfx::Keys k = pfx::make_keys(2);
  return k;
}

InstallOptions opts(const std::string& root, const pfx::Pack& p, const pfx::Keys& keys = K1()) {
  InstallOptions o;
  o.root = root;
  o.source_dir = p.dir.string();
  o.trusted = &keys.trusted;
  return o;
}

// A private copy of a pack directory (the tests that damage files work on a copy, so the original pack stays whole).
pfx::Pack clone(const pfx::Pack& p, const std::string& to) {
  pfx::Pack q = p;
  fs::remove_all(to);
  fs::create_directories(fs::path(to).parent_path());
  fs::copy(p.dir, to, fs::copy_options::recursive);
  q.dir = to;
  return q;
}

std::set<std::string> dir_names(const fs::path& d) {
  std::set<std::string> out;
  std::error_code ec;
  for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) out.insert(it->path().filename().string());
  return out;
}
bool staging_clean(const std::string& root) {
  const auto n = dir_names(fs::path(root) / "lib" / ".staging");
  return n.empty() || n == std::set<std::string>{".lock"};
}
bool nothing_published(const std::string& root) {
  for (const auto& n : dir_names(fs::path(root) / "lib"))
    if (n.size() == 36) return false;
  return staging_clean(root);
}
std::string current_of(const std::string& root, const std::string& lib) {
  std::error_code ec;
  const fs::path l = fs::path(root) / "lib" / lib / "current";
  return fs::is_symlink(l, ec) ? fs::read_symlink(l, ec).string() : std::string();
}

void test_happy_path() {
  TempDir t;
  const std::string root = t.sub("data");
  fs::create_directories(root);
  const pfx::Pack p = pfx::make_pack(t.sub("packs"), {}, K1());
  CHECK(!p.bundle_id.empty() && p.blocks == 2);
  const InstallResult r = install_protected(opts(root, p));
  CHECK(r.status == InstallStatus::kInstalled && r.reason == InstallReason::kNone && r.ok());
  CHECK(r.library_id == p.library_id && r.bundle_id == p.bundle_id && r.bundle_created && r.current_flipped);
  CHECK(r.instruments == 2 && r.blocks == 2 && r.bundle_bytes == p.bundle.size());
  const fs::path bd = fs::path(root) / "lib" / p.library_id / p.bundle_id;
  CHECK(r.bundle_dir == bd.string());
  CHECK(current_of(root, p.library_id) == p.bundle_id);
  // exactly the four files, byte for byte; the wrapped key sits next to the bundle as wrapped-key.bin; the vault entry and the report are NOT copied
  CHECK(dir_names(bd) == (std::set<std::string>{"bundle.vlrp", "wrapped-key.bin", "catalog.meta", "catalog.meta.sig"}));
  CHECK(pfx::read_bytes(bd / "bundle.vlrp") == p.bundle && pfx::read_bytes(bd / "wrapped-key.bin") == p.wrapped);
  CHECK(pfx::read_bytes(bd / "catalog.meta") == p.meta && pfx::read_bytes(bd / "catalog.meta.sig") == p.seal);
  CHECK(staging_clean(root));
  // the installed bundle verifies by itself
  auto v = fmt::ProtectedBundleView::open_file((bd / "bundle.vlrp").string(), K1().trusted);
  CHECK(v.ok() && v.value().verify_all_blocks() == fmt::Error::kNone);
  // a pack given by explicit paths (bundle + named wrapped key), the directory name irrelevant
  TempDir t2;
  fs::create_directories(t2.sub("data"));
  InstallOptions o;
  o.root = t2.sub("data");
  o.bundle_path = (p.dir / "bundle.vlrp").string();
  o.wrapped_key_path = (p.dir / p.wrapped_name).string();
  o.trusted = &K1().trusted;
  const InstallResult r2 = install_protected(o);
  CHECK(r2.status == InstallStatus::kInstalled && r2.bundle_id == p.bundle_id);
}

void test_idempotent_and_revisions() {
  TempDir t;
  const std::string root = t.sub("data");
  fs::create_directories(root);
  const pfx::Pack p1 = pfx::make_pack(t.sub("packs"), {}, K1());
  CHECK(install_protected(opts(root, p1)).status == InstallStatus::kInstalled);
  const auto before = pfx::list_tree(fs::path(root) / "lib");
  const InstallResult again = install_protected(opts(root, p1));  // the same bundle: unchanged, nothing touched
  CHECK(again.status == InstallStatus::kUnchanged && again.reason == InstallReason::kAlreadyInstalled && again.ok() && !again.bundle_created && !again.current_flipped);
  CHECK(pfx::list_tree(fs::path(root) / "lib") == before);
  // a new revision of the same library (a higher signed pack_revision): new bundle id, `current` flips, the previous bundle stays (a lease)
  pfx::PackSpec s2;
  s2.variant = 1;
  s2.revision = 2;
  const pfx::Pack p2 = pfx::make_pack(t.sub("packs2"), s2, K1());
  CHECK(p2.bundle_id != p1.bundle_id);
  const InstallResult r2 = install_protected(opts(root, p2));
  CHECK(r2.status == InstallStatus::kInstalled && current_of(root, p1.library_id) == p2.bundle_id && r2.pack_revision == 2 && r2.pruned == 0);
  CHECK(fs::is_directory(fs::path(root) / "lib" / p1.library_id / p1.bundle_id) && staging_clean(root));
  // a replay of the older signed bundle is a downgrade; so is the same revision with another bundle; a higher revision is accepted
  {
    const InstallResult r3 = install_protected(opts(root, p1));
    CHECK(r3.status == InstallStatus::kRejected && r3.reason == InstallReason::kDowngrade && r3.detail.find("older") != std::string::npos);
    CHECK(current_of(root, p1.library_id) == p2.bundle_id && staging_clean(root));
    pfx::PackSpec same_rev;
    same_rev.variant = 9;
    same_rev.revision = 2;
    const pfx::Pack ps = pfx::make_pack(t.sub("packs_same"), same_rev, K1());
    const InstallResult rs = install_protected(opts(root, ps));
    CHECK(rs.status == InstallStatus::kRejected && rs.reason == InstallReason::kDowngrade && rs.detail.find("another bundle") != std::string::npos);
    CHECK(install_protected(opts(root, p2)).status == InstallStatus::kUnchanged);  // equal revision, same bundle
  }
  pfx::PackSpec s3;
  s3.variant = 2;
  s3.revision = 3;
  const pfx::Pack p3 = pfx::make_pack(t.sub("packs3"), s3, K1());
  const InstallResult r3 = install_protected(opts(root, p3));
  // at most the new and the previous bundle stay: the oldest protected bundle directory is removed once the flip is durable
  CHECK(r3.status == InstallStatus::kInstalled && r3.pruned == 1 && current_of(root, p1.library_id) == p3.bundle_id);
  CHECK(!fs::exists(fs::path(root) / "lib" / p1.library_id / p1.bundle_id) && fs::is_directory(fs::path(root) / "lib" / p1.library_id / p2.bundle_id) &&
        fs::is_directory(fs::path(root) / "lib" / p1.library_id / p3.bundle_id) && staging_clean(root));
  // a vendor rollback is a NEW, higher revision that carries the old content
  {
    pfx::PackSpec rb = {};
    rb.revision = 4;  // the content of p1 (variant 0), revision 4
    const pfx::Pack pr = pfx::make_pack(t.sub("packs_rb"), rb, K1());
    const InstallResult rr = install_protected(opts(root, pr));
    CHECK(rr.status == InstallStatus::kInstalled && current_of(root, p1.library_id) == pr.bundle_id);
  }
  // a damaged installed copy of the same bundle is moved aside and replaced by the normal publish (it stays the current bundle's id)
  {
    TempDir u;
    fs::create_directories(u.sub("data"));
    CHECK(install_protected(opts(u.sub("data"), p1)).status == InstallStatus::kInstalled);
    const fs::path f = fs::path(u.sub("data")) / "lib" / p1.library_id / p1.bundle_id / "bundle.vlrp";
    Bytes b = pfx::read_bytes(f);
    b[b.size() - 5] ^= 1;
    pfx::write_bytes(f, b);
    const InstallResult r4 = install_protected(opts(u.sub("data"), p1));
    CHECK(r4.status == InstallStatus::kInstalled && r4.bundle_created && r4.detail.find("damaged") != std::string::npos);
    CHECK(pfx::read_bytes(f) == p1.bundle && current_of(u.sub("data"), p1.library_id) == p1.bundle_id && staging_clean(u.sub("data")));
    // and one that lost its bundle file altogether
    fs::remove(f);
    CHECK(install_protected(opts(u.sub("data"), p1)).status == InstallStatus::kInstalled && pfx::read_bytes(f) == p1.bundle);
  }
  // per-device redelivery: the same bundle with another wrapped-key blob replaces the stored blob (atomically, same validation), the bundle stays unchanged
  {
    TempDir u;
    fs::create_directories(u.sub("data"));
    CHECK(install_protected(opts(u.sub("data"), p1)).status == InstallStatus::kInstalled);
    pfx::PackSpec re;
    re.recipient = 0xB4;
    pfx::Pack pr = pfx::make_pack(u.sub("redeliver"), re, K1());  // same content: same bundle_id, another wrapped key
    CHECK(pr.bundle_id == p1.bundle_id && pr.wrapped != p1.wrapped);
    const InstallResult r = install_protected(opts(u.sub("data"), pr));
    CHECK(r.status == InstallStatus::kUnchanged && r.wrapped_key_updated && r.reason == InstallReason::kAlreadyInstalled);
    const fs::path wk = fs::path(u.sub("data")) / "lib" / p1.library_id / p1.bundle_id / "wrapped-key.bin";
    CHECK(pfx::read_bytes(wk) == pr.wrapped && !fs::exists(fs::path(wk).string() + ".tmp"));
    struct stat st;
    CHECK(::stat(wk.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    const InstallResult r2b = install_protected(opts(u.sub("data"), pr));  // nothing to replace now
    CHECK(r2b.status == InstallStatus::kUnchanged && !r2b.wrapped_key_updated);
  }
}

void test_refusals() {
  TempDir t;
  const std::string root = t.sub("data");
  fs::create_directories(root);
  // downgrade: a format 3.1 bundle is installed, then a 3.0 one of the same library
  pfx::PackSpec ext;
  ext.extension = true;
  const pfx::Pack p_ext = pfx::make_pack(t.sub("packs_ext"), ext, K1());
  CHECK(p_ext.minor == 1);
  pfx::PackSpec plain;
  plain.variant = 2;
  plain.revision = 2;  // a HIGHER revision, but an older format minor: still a downgrade
  const pfx::Pack p_plain = pfx::make_pack(t.sub("packs_plain"), plain, K1());
  CHECK(p_plain.minor == 0);
  CHECK(install_protected(opts(root, p_ext)).status == InstallStatus::kInstalled);
  const InstallResult dg = install_protected(opts(root, p_plain));
  CHECK(dg.status == InstallStatus::kRejected && dg.reason == InstallReason::kDowngrade && dg.detail.find("format 3.1") != std::string::npos);
  CHECK(current_of(root, p_ext.library_id) == p_ext.bundle_id && !fs::exists(fs::path(root) / "lib" / p_plain.library_id / p_plain.bundle_id) && staging_clean(root));

  // an UNPROTECTED bundle (format 2) is never installed as a Factory library
  {
    TempDir u;
    fs::create_directories(u.sub("data"));
    const fs::path dir = fs::path(u.sub("up")) / "3f1d6a52-9b7e-4c1a-8d2e-5a6b7c8d9e0f";
    pfx::Pack p = pfx::make_pack(u.sub("p"), {}, K1());
    auto plainw = pfx::make_writer({});
    auto bytes = plainw.finalize();
    CHECK(bytes.ok());
    pfx::write_bytes(dir / "bundle.vlrp", bytes.value());
    for (const char* f : {"catalog.meta", "catalog.meta.sig"}) pfx::write_bytes(dir / f, pfx::read_bytes(p.dir / f));
    pfx::write_bytes(dir / "wrapped-key.01.bin", p.wrapped);
    InstallOptions o;
    o.root = u.sub("data");
    o.source_dir = dir.string();
    o.trusted = &K1().trusted;
    const InstallResult r = install_protected(o);
    CHECK(r.status == InstallStatus::kRejected && r.reason == InstallReason::kNotProtected);
    CHECK(nothing_published(u.sub("data")));
  }
  // a user library under the same id (an unprotected `current`) is never replaced
  {
    TempDir u;
    const std::string uroot = u.sub("data");
    const pfx::Pack p = pfx::make_pack(u.sub("p"), {}, K1());
    const fs::path ub = fs::path(uroot) / "lib" / p.library_id / std::string(64, 'a');
    fs::create_directories(ub);
    pfx::write_bytes(ub / "bundle.vlb", Bytes{1, 2, 3});
    fs::create_symlink(std::string(64, 'a'), fs::path(uroot) / "lib" / p.library_id / "current");
    const InstallResult r = install_protected(opts(uroot, p));
    CHECK(r.status == InstallStatus::kRejected && r.reason == InstallReason::kLibraryIdInUse);
    CHECK(current_of(uroot, p.library_id) == std::string(64, 'a') && staging_clean(uroot));
  }
  // a missing bundle file, a missing data root, no trusted list
  {
    InstallOptions o = opts(root, p_ext);
    o.source_dir = t.sub("nowhere");
    o.expected_library_id = p_ext.library_id;
    CHECK(install_protected(o).reason == InstallReason::kMissingFile);
    InstallOptions o2 = opts(t.sub("no-root"), p_ext);
    CHECK(install_protected(o2).status == InstallStatus::kInvalidArgument);
    InstallOptions o3 = opts(root, p_ext);
    o3.trusted = nullptr;
    CHECK(install_protected(o3).status == InstallStatus::kInvalidArgument);
  }
}

void test_trust_and_signature() {
  TempDir t;
  const std::string root = t.sub("data");
  fs::create_directories(root);
  const pfx::Pack p = pfx::make_pack(t.sub("packs"), {}, K1());
  // signed by a key this build does not trust
  InstallResult r = install_protected(opts(root, p, K2()));
  CHECK(r.status == InstallStatus::kRejected && r.reason == InstallReason::kUntrustedKey && nothing_published(root));
  // an empty trust list trusts nothing
  fmt::TrustedKeys none;
  InstallOptions o = opts(root, p);
  o.trusted = &none;
  CHECK(install_protected(o).reason == InstallReason::kUntrustedKey && nothing_published(root));
  // a trusted id with another key behind it, or a list that names one id twice, is not trusted either
  fmt::TrustedKeys wrong({fmt::TrustedKey{K1().id, K2().pub}});
  o.trusted = &wrong;
  CHECK(install_protected(o).reason == InstallReason::kUntrustedKey);
  fmt::TrustedKeys twice({fmt::TrustedKey{K1().id, K1().pub}, fmt::TrustedKey{K1().id, K1().pub}});
  o.trusted = &twice;
  CHECK(install_protected(o).reason == InstallReason::kUntrustedKey);
  // a flipped bit in the signature (inside the seal): signature_invalid
  {
    pfx::Pack q = clone(p, t.sub("q_sig") + "/" + p.library_id);
    const std::uint64_t idx_off = rd64(q.bundle.data() + 24), idx_size = rd64(q.bundle.data() + 32);
    q.bundle[idx_off + idx_size + 20] ^= 0x04;  // seal: magic(4) alg(1) zero(3) key id(8) signature(64): byte 20 is inside the signature
    pfx::write_bytes(q.dir / "bundle.vlrp", q.bundle);
    const InstallResult s = install_protected(opts(root, q));
    CHECK(s.status == InstallStatus::kRejected && s.reason == InstallReason::kSignatureInvalid && nothing_published(root));
  }
  // the trust-list parser: the committed format, a bad id, a repeated id
  {
    const std::string good = t.sub("good.pub-list");
    char id[17], pub[65];
    for (int i = 0; i < 8; ++i) std::snprintf(id + 2 * i, 3, "%02x", K1().id[i]);
    for (int i = 0; i < 32; ++i) std::snprintf(pub + 2 * i, 3, "%02x", K1().pub[i]);
    pfx::write_bytes(good, pfx::as_bytes(std::string("# comment\ndev content-manifest ") + id + " " + pub + "  # trailing\n\n"));
    fmt::TrustedKeys loaded;
    std::string err;
    CHECK(load_trust_list(good, loaded, err) && loaded.size() == 1 && loaded.find(K1().id) != nullptr);
    InstallOptions lo = opts(root, p);
    lo.trusted = &loaded;
    CHECK(install_protected(lo).status == InstallStatus::kInstalled);
    pfx::write_bytes(t.sub("bad1"), pfx::as_bytes(std::string("dev content-manifest 0000000000000000 ") + pub + "\n"));
    CHECK(!load_trust_list(t.sub("bad1"), loaded, err) && err.find("key id") != std::string::npos);
    pfx::write_bytes(t.sub("bad2"), pfx::as_bytes(std::string("dev content-manifest ") + id + " " + pub + "\ndev content-manifest " + id + " " + pub + "\n"));
    CHECK(!load_trust_list(t.sub("bad2"), loaded, err) && err.find("twice") != std::string::npos);
    pfx::write_bytes(t.sub("bad3"), pfx::as_bytes("dev nonsense\n"));
    CHECK(!load_trust_list(t.sub("bad3"), loaded, err));
    CHECK(!load_trust_list(t.sub("missing"), loaded, err));
  }
}

void test_library_id() {
  TempDir t;
  const std::string root = t.sub("data");
  fs::create_directories(root);
  const pfx::Pack p = pfx::make_pack(t.sub("packs"), {}, K1());
  InstallOptions o = opts(root, p);
  o.expected_library_id = "00000000-0000-4000-8000-000000000042";
  InstallResult r = install_protected(o);
  CHECK(r.status == InstallStatus::kRejected && r.reason == InstallReason::kLibraryIdMismatch && nothing_published(root));
  o.expected_library_id = "not-a-uuid";
  CHECK(install_protected(o).status == InstallStatus::kInvalidArgument);
  // the pack directory is named after another library than the bundle holds
  fs::create_directories(t.sub("renamed"));
  fs::copy(p.dir, fs::path(t.sub("renamed")) / "00000000-0000-4000-8000-000000000043", fs::copy_options::recursive);
  InstallOptions o2 = opts(root, p);
  o2.source_dir = (fs::path(t.sub("renamed")) / "00000000-0000-4000-8000-000000000043").string();
  CHECK(install_protected(o2).reason == InstallReason::kLibraryIdMismatch && nothing_published(root));
  o2.expected_library_id = p.library_id;  // an explicit id wins over the directory name
  CHECK(install_protected(o2).status == InstallStatus::kInstalled);
}

void test_wrapped_key() {
  TempDir t;
  const std::string root = t.sub("data");
  fs::create_directories(root);
  const pfx::Pack p = pfx::make_pack(t.sub("packs"), {}, K1());
  auto with_blob = [&](const Bytes& blob, const std::string& name, const std::string& into) {
    static int n = 0;
    pfx::Pack q = clone(p, t.sub("wk" + std::to_string(++n)) + "/" + p.library_id);
    for (const auto& e : fs::directory_iterator(q.dir))
      if (e.path().filename().string().rfind("wrapped-key.", 0) == 0) fs::remove(e.path());
    pfx::write_bytes(q.dir / name, blob);
    fs::create_directories(into);
    return install_protected(opts(into, q));
  };
  auto expect_bad = [&](const Bytes& blob, const char* what) {
    const InstallResult r = with_blob(blob, p.wrapped_name, root);
    if (!(r.status == InstallStatus::kRejected && r.reason == InstallReason::kWrappedKeyInvalid && nothing_published(root))) {
      std::printf("FAIL wrapped key case %s\n", what);
      ++g_fail;
    }
  };
  Bytes b = p.wrapped;
  { Bytes x = b; x.pop_back(); expect_bad(x, "104 bytes"); }
  { Bytes x = b; x.push_back(0); expect_bad(x, "106 bytes"); }
  { Bytes x = b; x[0] ^= 1; expect_bad(x, "magic"); }
  { Bytes x = b; x[3] = '2'; expect_bad(x, "version-ish magic byte"); }
  { Bytes x = b; x[4] = 0x02; expect_bad(x, "algorithm"); }
  { Bytes x = b; for (int i = 13; i < 45; ++i) x[i] = 0; expect_bad(x, "zero ephemeral key"); }
  expect_bad({}, "empty");
  // the recipient key id in `wrapped-key.<id>.bin` must be the one in the blob (bytes 5..12)
  {
    const InstallResult r = with_blob(pfx::make_wrapped_key(0x77), p.wrapped_name, root);
    CHECK(r.status == InstallStatus::kRejected && r.reason == InstallReason::kWrappedKeyInvalid && r.detail.find("recipient") != std::string::npos && nothing_published(root));
  }
  // missing, and ambiguous (two candidates, none named)
  {
    pfx::Pack q = clone(p, t.sub("wk_amb") + "/" + p.library_id);
    for (const auto& e : fs::directory_iterator(q.dir))
      if (e.path().filename().string().rfind("wrapped-key.", 0) == 0) fs::remove(e.path());
    InstallResult r = install_protected(opts(root, q));
    CHECK(r.reason == InstallReason::kWrappedKeyInvalid && r.detail.find("missing") != std::string::npos);
    const Bytes other = pfx::make_wrapped_key(0x77);
    const std::string other_name = "wrapped-key." + fmt::to_hex(std::span<const std::uint8_t>(other.data() + 5, 8)) + ".bin";
    pfx::write_bytes(q.dir / p.wrapped_name, p.wrapped);
    pfx::write_bytes(q.dir / other_name, other);
    r = install_protected(opts(root, q));
    CHECK(r.reason == InstallReason::kWrappedKeyInvalid && r.detail.find("ambiguous") != std::string::npos && nothing_published(root));
    InstallOptions named = opts(root, q);  // naming the one for this device resolves it
    named.wrapped_key_path = (q.dir / other_name).string();
    r = install_protected(named);
    CHECK(r.status == InstallStatus::kInstalled);
    CHECK(pfx::read_bytes(fs::path(root) / "lib" / q.library_id / q.bundle_id / "wrapped-key.bin") == other);
  }
  // THE LIMIT, pinned: the librarian cannot tell a blob of another library, another pack or a damaged payload from the right one (it has no device key and the
  // blob carries no library id: that binding is the AAD of the unwrap, Story 6.5). Every single-bit flip of the 105 bytes is installed EXCEPT those in the magic,
  // the algorithm byte (0..4) and the recipient key id (5..12, which must match the file name); the flipped blob is stored as delivered. This is a denial of
  // service for that one library at worst (the content key does not unwrap), never wrong audio: the content authenticity is the manifest signature + block hashes.
  {
    int rejected = 0, installed = 0;
    for (std::size_t i = 0; i < b.size(); ++i) {
      Bytes x = b;
      x[i] ^= static_cast<std::uint8_t>(1u << (i % 8));
      const std::string into = t.sub("sweep_wk" + std::to_string(i));
      const InstallResult r = with_blob(x, p.wrapped_name, into);
      const bool should_reject = i <= 12;
      if (should_reject) {
        ++rejected;
        if (!(r.status == InstallStatus::kRejected && r.reason == InstallReason::kWrappedKeyInvalid && nothing_published(into))) {
          std::printf("FAIL wrapped key byte %zu was not rejected\n", i);
          ++g_fail;
        }
      } else {
        ++installed;
        if (!(r.status == InstallStatus::kInstalled && pfx::read_bytes(fs::path(into) / "lib" / p.library_id / p.bundle_id / "wrapped-key.bin") == x)) {
          std::printf("FAIL wrapped key byte %zu did not install as delivered\n", i);
          ++g_fail;
        }
      }
    }
    CHECK(rejected == 13 && installed == 92);
    // a blob made for ANOTHER library installs (the librarian cannot see the library inside the AAD): the documented limit
    const Bytes swapped = pfx::make_wrapped_key(0xA1);  // same recipient id, other bytes
    Bytes other_lib = swapped;
    other_lib[60] ^= 0x5a;
    const InstallResult r = with_blob(other_lib, p.wrapped_name, t.sub("swapped"));
    CHECK(r.status == InstallStatus::kInstalled);
  }
  // modes: the wrapped key is private to the owner (0600), the rest of the bundle directory is world readable (0644 files, 0755 directory)
  {
    TempDir u;
    fs::create_directories(u.sub("data"));
    CHECK(install_protected(opts(u.sub("data"), p)).status == InstallStatus::kInstalled);
    const fs::path bd = fs::path(u.sub("data")) / "lib" / p.library_id / p.bundle_id;
    struct stat st;
    CHECK(::stat((bd / "wrapped-key.bin").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    for (const char* f : {"bundle.vlrp", "catalog.meta", "catalog.meta.sig"}) CHECK(::stat((bd / f).c_str(), &st) == 0 && (st.st_mode & 0777) == 0644);
    CHECK(::stat(bd.c_str(), &st) == 0 && (st.st_mode & 0777) == 0755);
  }
  CHECK(wrapped_key_problem(b.data(), b.size()).empty());
}

// The exact reason a flipped bit must give, by file class and position inside the class.
InstallReason expected_reason(const std::string& cls, std::size_t pos_in_class) {
  if (cls == "blocks") return InstallReason::kBlockHashMismatch;
  if (cls == "seal") {
    if (pos_in_class >= 8 && pos_in_class < 16) return InstallReason::kUntrustedKey;       // the signer's key id: not in the trust list any more
    if (pos_in_class >= 16 && pos_in_class < 80) return InstallReason::kSignatureInvalid;  // the Ed25519 signature
    return InstallReason::kCorruptBundle;                                                  // magic, algorithm, reserved zero bytes
  }
  return InstallReason::kCorruptBundle;  // header (but see the version bytes), manifest, index: their hashes / layout
}

// One flipped bit in each of the sampled positions of every file: rejected with the EXACT typed reason, nothing published, no staging, the catalog empty - checked
// after EVERY flip.
void test_bitflip_sweep() {
  TempDir t;
  const std::string root = t.sub("data");
  fs::create_directories(root);
  const pfx::Pack p = pfx::make_pack(t.sub("packs"), {}, K1());
  const std::uint64_t man_off = rd64(p.bundle.data() + 8), man_size = rd64(p.bundle.data() + 16);
  const std::uint64_t idx_off = rd64(p.bundle.data() + 24), idx_size = rd64(p.bundle.data() + 32);
  const std::uint64_t data_off = rd64(p.bundle.data() + 40);
  const std::uint64_t seal_off = idx_off + idx_size;
  CHECK(man_off == 128 && seal_off + 128 == data_off && data_off < p.bundle.size());
  pfx::Pack q = clone(p, t.sub("sweep") + "/" + p.library_id);  // the damaged files are written here; the original stays whole
  std::unique_ptr<Catalog> cat;
  CHECK(Catalog::open(root, cat) == CatalogStatus::kOk);
  struct Pos {
    std::string cls;
    std::size_t abs, in_class;
  };
  std::vector<Pos> positions;
  for (std::size_t i = 0; i < 128; ++i) positions.push_back({"header", i, i});
  for (std::size_t i = 0; i < man_size; ++i) positions.push_back({"manifest", static_cast<std::size_t>(man_off) + i, i});
  for (std::size_t i = 0; i < idx_size; ++i) positions.push_back({"index", static_cast<std::size_t>(idx_off) + i, i});
  for (std::size_t i = 0; i < 128; ++i) positions.push_back({"seal", static_cast<std::size_t>(seal_off) + i, i});
  for (std::size_t i = static_cast<std::size_t>(data_off); i < p.bundle.size(); i += 61) positions.push_back({"blocks", i, i - static_cast<std::size_t>(data_off)});
  positions.push_back({"blocks", p.bundle.size() - 1, p.bundle.size() - 1 - static_cast<std::size_t>(data_off)});
  std::map<std::string, int> per_class;
  int bad = 0;
  std::vector<LibraryRecord> rows;
  for (const Pos& ps : positions) {
    Bytes b = p.bundle;
    b[ps.abs] ^= static_cast<std::uint8_t>(1u << (ps.abs % 8));
    pfx::write_bytes(q.dir / "bundle.vlrp", b);
    const InstallResult r = install_protected(opts(root, q));
    ++per_class[ps.cls];
    bool ok = r.status == InstallStatus::kRejected && r.reason != InstallReason::kNone;
    if (ok && ps.cls == "header" && ps.in_class >= 4 && ps.in_class < 8)  // the version word: an older / unknown version
      ok = r.reason == InstallReason::kNotProtected || r.reason == InstallReason::kUnsupportedVersion || r.reason == InstallReason::kCorruptBundle;
    else if (ok)
      ok = r.reason == expected_reason(ps.cls, ps.in_class);
    // after EVERY flip: nothing published, no staging leftover, no catalog row
    ok = ok && nothing_published(root) && staging_clean(root) && cat->list_libraries(rows, false) == CatalogStatus::kOk && rows.empty();
    if (!ok) {
      std::printf("FAIL flip in %s at %zu: %s %s\n", ps.cls.c_str(), ps.abs, to_string(r.status), to_string(r.reason));
      ++bad;
      ++g_fail;
    }
  }
  CHECK(bad == 0);
  std::printf("bit-flip sweep of bundle.vlrp: %zu flips (header %d, manifest %d, index %d, seal %d, blocks %d), each with its exact reason, none installed\n", positions.size(),
              per_class["header"], per_class["manifest"], per_class["index"], per_class["seal"], per_class["blocks"]);
  CHECK(per_class["blocks"] >= 20 && positions.size() > 500);
  // catalog.meta and catalog.meta.sig: every byte, reason catalog_meta_invalid ONLY
  int meta_flips = 0;
  for (const char* file : {"catalog.meta", "catalog.meta.sig"}) {
    const Bytes orig = std::string(file) == "catalog.meta" ? p.meta : p.seal;
    for (std::size_t i = 0; i < orig.size(); ++i) {
      Bytes b = orig;
      b[i] ^= static_cast<std::uint8_t>(1u << (i % 8));
      pfx::write_bytes(q.dir / file, b);
      const InstallResult r = install_protected(opts(root, q));
      ++meta_flips;
      if (!(r.status == InstallStatus::kRejected && r.reason == InstallReason::kCatalogMetaInvalid && nothing_published(root) && staging_clean(root) &&
            cat->list_libraries(rows, false) == CatalogStatus::kOk && rows.empty())) {
        std::printf("FAIL flip in %s at %zu: %s %s\n", file, i, to_string(r.status), to_string(r.reason));
        ++g_fail;
      }
    }
  }
  pfx::write_bytes(q.dir / "catalog.meta", p.meta);
  pfx::write_bytes(q.dir / "catalog.meta.sig", p.seal);
  CHECK(meta_flips > 250 && nothing_published(root));
  std::printf("bit-flip sweep of catalog.meta and catalog.meta.sig: %d flips, each catalog_meta_invalid, none installed\n", meta_flips);
  // truncations of the bundle at every structural boundary and a few cut points
  int cuts = 0;
  for (std::size_t len : {std::size_t(0), std::size_t(1), std::size_t(64), std::size_t(127), std::size_t(128), std::size_t(man_off + man_size), std::size_t(idx_off),
                          std::size_t(seal_off), std::size_t(seal_off + 64), std::size_t(data_off), std::size_t(data_off + 1), p.bundle.size() - 1}) {
    Bytes b(p.bundle.begin(), p.bundle.begin() + static_cast<std::ptrdiff_t>(len));
    pfx::write_bytes(q.dir / "bundle.vlrp", b);
    const InstallResult r = install_protected(opts(root, q));
    ++cuts;
    CHECK(r.status == InstallStatus::kRejected && r.reason == InstallReason::kCorruptBundle && nothing_published(root));
  }
  // appended bytes (the header's total_size no longer matches)
  {
    Bytes b = p.bundle;
    b.push_back(0);
    pfx::write_bytes(q.dir / "bundle.vlrp", b);
    CHECK(install_protected(opts(root, q)).reason == InstallReason::kCorruptBundle);
  }
  CHECK(cuts == 12 && nothing_published(root));
  // and the untouched pack still installs on the same root afterwards
  CHECK(install_protected(opts(root, p)).status == InstallStatus::kInstalled);
}

// Validly SIGNED bundles that are still wrong: an inconsistent hash-list root, an unknown format minor. (A re-signed bundle has a new bundle_id, so these also show
// that the typed reason comes from the bundle, not from a hash mismatch.)
void test_signed_but_wrong() {
  TempDir t;
  const std::string root = t.sub("data");
  fs::create_directories(root);
  const pfx::Pack p = pfx::make_pack(t.sub("packs"), {}, K1());
  {
    pfx::Pack q = clone(p, t.sub("hl") + "/" + p.library_id);
    pfx::write_bytes(q.dir / "bundle.vlrp", pfx::resign(p.bundle, K1().seed, [](Bytes& manifest, Bytes&) { manifest[96 + 5] ^= 0x01; }));
    const InstallResult r = install_protected(opts(root, q));
    CHECK(r.status == InstallStatus::kRejected && r.reason == InstallReason::kHashListMismatch && nothing_published(root));
  }
  {
    pfx::Pack q = clone(p, t.sub("ver") + "/" + p.library_id);
    pfx::write_bytes(q.dir / "bundle.vlrp", pfx::resign(p.bundle, K1().seed, [](Bytes& manifest, Bytes& header) {
                       manifest[2] = 2;  // minor 2 in the manifest's version word ...
                       header[6] = 2;    // ... and in the header's
                     }));
    const InstallResult r = install_protected(opts(root, q));
    CHECK(r.status == InstallStatus::kRejected && r.reason == InstallReason::kUnsupportedVersion && nothing_published(root));
  }
  {  // validly signed, structurally corrupt manifest (an instrument count that cannot fit)
    pfx::Pack q = clone(p, t.sub("cnt") + "/" + p.library_id);
    pfx::write_bytes(q.dir / "bundle.vlrp", pfx::resign(p.bundle, K1().seed, [](Bytes& manifest, Bytes&) { manifest[20 + 3] = 0x7f; }));
    const InstallResult r = install_protected(opts(root, q));
    CHECK(r.status == InstallStatus::kRejected && r.reason == InstallReason::kCorruptBundle && nothing_published(root));
  }
  CHECK(install_protected(opts(root, p)).status == InstallStatus::kInstalled);
}

void test_catalog_meta_binding() {
  TempDir t;
  const std::string root = t.sub("data");
  fs::create_directories(root);
  const pfx::Pack p = pfx::make_pack(t.sub("packs"), {}, K1());
  // 1. a catalog.meta and seal of ANOTHER bundle of the same signer do not fit this one
  pfx::PackSpec other;
  other.variant = 3;
  const pfx::Pack o2 = pfx::make_pack(t.sub("other"), other, K1());
  {
    pfx::Pack q = clone(p, t.sub("cm1") + "/" + p.library_id);
    pfx::write_bytes(q.dir / "catalog.meta.sig", o2.seal);
    const InstallResult r = install_protected(opts(root, q));
    CHECK(r.status == InstallStatus::kRejected && r.reason == InstallReason::kCatalogMetaInvalid);
  }
  // 2. a valid seal (right key, right bundle) over a catalog.meta that does not name the bundle's instruments is refused
  {
    CatalogMeta m;
    CHECK(parse_catalog_meta(std::string(p.meta.begin(), p.meta.end()), m));
    m.instruments.pop_back();
    pfx::Pack q = clone(p, t.sub("cm2") + "/" + p.library_id);
    const Bytes text = pfx::as_bytes(catalog_meta_text(m));
    pfx::write_bytes(q.dir / "catalog.meta", text);
    auto vw = fmt::ProtectedBundleView::open_memory(p.bundle, K1().trusted);
    auto seal = fmt::sign_catalog_meta(K1().seed, vw.value().info().bundle_id, text);
    pfx::write_bytes(q.dir / "catalog.meta.sig", Bytes(seal.value().begin(), seal.value().end()));
    const InstallResult r = install_protected(opts(root, q));
    CHECK(r.status == InstallStatus::kRejected && r.reason == InstallReason::kCatalogMetaInvalid && r.detail.find("instruments") != std::string::npos);
  }
  // 3. the seal made by another (untrusted) key
  {
    pfx::Pack q = clone(p, t.sub("cm3") + "/" + p.library_id);
    auto vw = fmt::ProtectedBundleView::open_memory(p.bundle, K1().trusted);
    auto seal = fmt::sign_catalog_meta(K2().seed, vw.value().info().bundle_id, p.meta);
    pfx::write_bytes(q.dir / "catalog.meta.sig", Bytes(seal.value().begin(), seal.value().end()));
    const InstallResult r = install_protected(opts(root, q));
    CHECK(r.status == InstallStatus::kRejected && r.reason == InstallReason::kCatalogMetaInvalid);  // signed by another key than the bundle
  }
  // 4. missing or oversized files
  {
    pfx::Pack q = clone(p, t.sub("cm4") + "/" + p.library_id);
    fs::remove(q.dir / "catalog.meta");
    CHECK(install_protected(opts(root, q)).reason == InstallReason::kCatalogMetaInvalid);
    pfx::write_bytes(q.dir / "catalog.meta", p.meta);
    fs::remove(q.dir / "catalog.meta.sig");
    CHECK(install_protected(opts(root, q)).reason == InstallReason::kCatalogMetaInvalid);
    pfx::write_bytes(q.dir / "catalog.meta.sig", Bytes(200, 0));
    CHECK(install_protected(opts(root, q)).reason == InstallReason::kCatalogMetaInvalid);
  }
  CHECK(nothing_published(root));
}

struct CountingSource : fmt::ByteSource {
  std::string path;
  int fd = -1;
  std::uint64_t sz = 0;
  mutable std::atomic<std::uint64_t> max_read{0}, reads{0};
  std::atomic<std::uint64_t>* shared_max = nullptr;
  std::atomic<std::uint64_t>* shared_reads = nullptr;
  std::uint64_t fail_after = ~0ull;  // reads allowed before every read fails with kIo
  explicit CountingSource(const std::string& p) : path(p) {
    fd = ::open(p.c_str(), O_RDONLY);
    struct stat st;
    if (fd >= 0 && ::fstat(fd, &st) == 0) sz = static_cast<std::uint64_t>(st.st_size);
  }
  ~CountingSource() override {
    if (fd >= 0) ::close(fd);
  }
  std::uint64_t size() const override { return sz; }
  const std::uint8_t* direct(std::uint64_t, std::size_t) const override { return nullptr; }
  fmt::Error read_at(std::uint64_t off, void* dst, std::size_t n) const override {
    const std::uint64_t k = ++reads;
    if (shared_reads) ++*shared_reads;
    if (k > fail_after) return fmt::Error::kIo;
    std::uint64_t m = max_read.load();
    while (n > m && !max_read.compare_exchange_weak(m, n)) {}
    if (shared_max) {
      std::uint64_t sm = shared_max->load();
      while (n > sm && !shared_max->compare_exchange_weak(sm, n)) {}
    }
    if (off + n > sz) return fmt::Error::kTruncated;
    return ::pread(fd, dst, n, static_cast<off_t>(off)) == static_cast<ssize_t>(n) ? fmt::Error::kNone : fmt::Error::kIo;
  }
};

void test_bounded_memory() {
  TempDir t;
  const std::string root = t.sub("data");
  fs::create_directories(root);
  pfx::PackSpec big;
  big.big = true;
  big.instruments = 1;
  big.frames = 700000;  // 5.6 MB of float noise per sample: 171 blocks, a bundle 20 times the copy chunk
  const pfx::Pack p = pfx::make_pack(t.sub("packs"), big, K1());
  CHECK(p.bundle.size() > 5u << 20 && p.blocks > 100);
  std::atomic<std::uint64_t> max_read{0}, reads{0};
  InstallOptions o = opts(root, p);
  o.open_source = [&](const std::string& path) -> std::unique_ptr<fmt::ByteSource> {
    auto s = std::make_unique<CountingSource>(path);
    s->shared_max = &max_read;
    s->shared_reads = &reads;
    return s;
  };
  const InstallResult r = install_protected(o);
  CHECK(r.status == InstallStatus::kInstalled);
  // the bundle is never read (or held) whole: the largest read of the source is the copy chunk, the largest buffer is the chunk or one block
  CHECK(max_read.load() <= kInstallChunkBytes);
  CHECK(r.peak_buffer_bytes <= kInstallChunkBytes && r.peak_buffer_bytes > 0);
  CHECK(r.bundle_bytes > 20 * r.peak_buffer_bytes);
  CHECK(reads.load() > r.bundle_bytes / kInstallChunkBytes);
  std::printf("bounded memory: bundle %llu bytes, largest source read %llu, peak buffer %llu\n", static_cast<unsigned long long>(r.bundle_bytes),
              static_cast<unsigned long long>(max_read.load()), static_cast<unsigned long long>(r.peak_buffer_bytes));
  // a block flipped deep inside the big bundle is found by the hash check of the staged copy
  Bytes b = p.bundle;
  b[b.size() * 3 / 4] ^= 0x40;
  pfx::Pack q = p;
  pfx::write_bytes(q.dir / "bundle.vlrp", b);
  TempDir t2;
  fs::create_directories(t2.sub("data"));
  const InstallResult bad = install_protected(opts(t2.sub("data"), q));
  CHECK(bad.status == InstallStatus::kRejected && bad.reason == InstallReason::kBlockHashMismatch && nothing_published(t2.sub("data")));
}

// A header that claims huge sizes must not make the installer allocate them: the sizes are capped before anything is allocated, and a file whose signer is not trusted
// is refused after reading only the header and the seal.
void test_no_unbounded_allocation() {
  TempDir t;
  const std::string root = t.sub("data");
  fs::create_directories(root);
  const pfx::Pack p = pfx::make_pack(t.sub("packs"), {}, K1());
  pfx::Pack q = clone(p, t.sub("huge") + "/" + p.library_id);
  auto put64 = [](Bytes& b, std::size_t off, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b[off + i] = static_cast<std::uint8_t>(v >> (8 * i));
  };
  // a sparse file of `total` bytes with a hand-made header: magic, version 3, the given manifest / index sizes, and a seal with the given key id at its place
  auto make = [&](std::uint64_t man, std::uint64_t idx, std::uint64_t total, const fmt::KeyId& signer) {
    const std::string path = (q.dir / "bundle.vlrp").string();
    fs::remove(path);
    Bytes h(128, 0);
    std::copy_n("VLRB", 4, h.begin());
    h[4] = 3;
    put64(h, 8, 128);
    put64(h, 16, man);
    put64(h, 24, 128 + man);
    put64(h, 32, idx);
    const std::uint64_t data_off = 128 + man + idx + 128;
    put64(h, 40, data_off);
    put64(h, 48, total - data_off);
    put64(h, 56, total);
    pfx::write_bytes(path, h);
    Bytes seal(128, 0);
    std::copy_n("VSL1", 4, seal.begin());
    seal[4] = 1;
    std::copy(signer.begin(), signer.end(), seal.begin() + 8);
    const int fd = ::open(path.c_str(), O_WRONLY);
    CHECK(fd >= 0 && ::ftruncate(fd, static_cast<off_t>(total)) == 0);
    CHECK(::pwrite(fd, seal.data(), seal.size(), static_cast<off_t>(128 + man + idx)) == static_cast<ssize_t>(seal.size()));
    ::close(fd);
  };
  const std::uint64_t GiB = 1ull << 30;
  struct Case {
    const char* what;
    std::uint64_t man, idx, total;
    fmt::KeyId signer;
    InstallReason want;
  };
  const fmt::KeyId unknown{9, 9, 9, 9, 9, 9, 9, 9};
  const Case cases[] = {
      {"a 2 GiB manifest claim", 2 * GiB, 48, 3 * GiB, K1().id, InstallReason::kCorruptBundle},
      {"a 1.5 GiB index claim", 100, 1610612736ull, 2 * GiB, K1().id, InstallReason::kCorruptBundle},
      {"a 60 MiB manifest from an untrusted signer", 60ull << 20, 48, 70ull << 20, unknown, InstallReason::kUntrustedKey},
      {"a 384 MiB index from an untrusted signer", 100, 384ull << 20, 450ull << 20, unknown, InstallReason::kUntrustedKey},
  };
  for (const Case& c : cases) {
    make(c.man, c.idx, c.total, c.signer);
    g_live = 0;
    g_peak = 0;
    g_max_one = 0;
    g_track = true;
    const InstallResult r = install_protected(opts(root, q));
    g_track = false;
    if (!(r.status == InstallStatus::kRejected && r.reason == c.want)) {
      std::printf("FAIL %s: %s %s\n", c.what, to_string(r.status), to_string(r.reason));
      ++g_fail;
    }
    if (g_max_one.load() >= (1u << 20)) {
      std::printf("FAIL %s: one allocation of %zu bytes\n", c.what, g_max_one.load());
      ++g_fail;
    }
    CHECK(nothing_published(root));
    fs::remove(q.dir / "bundle.vlrp");
  }
  // describe_bundle / reconcile see the same files (they open the same way): no allocation, and an unexaminable bundle, not a crash
  {
    const std::string r2 = t.sub("data2");
    const fs::path bd = fs::path(r2) / "lib" / p.library_id / std::string(64, 'c');
    fs::create_directories(bd);
    make(2 * GiB, 48, 3 * GiB, K1().id);
    fs::copy_file(q.dir / "bundle.vlrp", bd / "bundle.vlrp");
    DescribeOptions d;
    d.trusted = &K1().trusted;
    g_max_one = 0;
    g_track = true;
    const DescribeResult dr = describe_bundle(bd.string(), d);
    g_track = false;
    CHECK(dr.status == DescribeStatus::kUnreadable && g_max_one.load() < (1u << 20));
    fs::remove(q.dir / "bundle.vlrp");
  }
}

// Memory is bounded for real: a ~140 MB bundle installs with a peak of allocations far below its size (operator new is counted over the whole install).
void test_bounded_memory_measured() {
  TempDir t;
  const std::string root = t.sub("data");
  fs::create_directories(root);
  pfx::PackSpec big;
  big.big = true;
  big.instruments = 6;
  big.frames = 2900000;  // 6 x 23 MB of float noise
  const pfx::Pack p = pfx::make_pack(t.sub("packs"), big, K1());
  CHECK(p.bundle.size() > 130u << 20 && p.blocks > 4000);
  const std::uint64_t size = p.bundle.size();
  pfx::Pack q = p;
  q.bundle.clear();
  q.bundle.shrink_to_fit();  // the test's own copy is not what is measured, and it is not needed any more
  g_live = 0;
  g_peak = 0;
  g_max_one = 0;
  g_track = true;
  const InstallResult r = install_protected(opts(root, q));
  g_track = false;
  CHECK(r.status == InstallStatus::kInstalled && r.bundle_bytes == size);
  std::printf("measured memory: bundle %llu bytes, peak of live allocations during the install %lld bytes, largest single allocation %zu bytes\n",
              static_cast<unsigned long long>(size), g_peak.load(), g_max_one.load());
  CHECK(g_peak.load() < 8ll << 20);                 // the manifest, the index and one chunk or block at a time
  CHECK(g_max_one.load() <= (1u << 20));            // never more than one block / the copy chunk in one piece
  CHECK(static_cast<std::uint64_t>(g_peak.load()) * 16 < size);
}

// A FIFO (or any non-regular file) in place of any file of the pack must not hang the installer: a typed refusal.
void test_fifo_does_not_hang() {
  ::alarm(120);  // a hang would end the test run
  TempDir t;
  const pfx::Pack p = pfx::make_pack(t.sub("packs"), {}, K1());
  struct One {
    const char* what;
    std::function<std::string(const pfx::Pack&)> victim;
  };
  const One ones[] = {{"bundle.vlrp", [](const pfx::Pack& q) { return (q.dir / "bundle.vlrp").string(); }},
                      {"wrapped key (found by name)", [](const pfx::Pack& q) { return (q.dir / q.wrapped_name).string(); }},
                      {"catalog.meta", [](const pfx::Pack& q) { return (q.dir / "catalog.meta").string(); }},
                      {"catalog.meta.sig", [](const pfx::Pack& q) { return (q.dir / "catalog.meta.sig").string(); }}};
  int n = 0;
  for (const One& one : ones) {
    const std::string root = t.sub("data" + std::to_string(++n));
    fs::create_directories(root);
    pfx::Pack q = clone(p, t.sub("fifo" + std::to_string(n)) + "/" + p.library_id);
    const std::string v = one.victim(q);
    fs::remove(v);
    CHECK(::mkfifo(v.c_str(), 0600) == 0);
    InstallOptions o = opts(root, q);
    const InstallResult r = install_protected(o);
    if (r.ok() || r.reason == InstallReason::kNone && r.status != InstallStatus::kIo) {
      std::printf("FAIL fifo in place of %s: %s %s\n", one.what, to_string(r.status), to_string(r.reason));
      ++g_fail;
    }
    CHECK(nothing_published(root));
    if (std::string(one.what).rfind("wrapped", 0) == 0) {  // also named explicitly
      o.wrapped_key_path = v;
      const InstallResult r2 = install_protected(o);
      CHECK(!r2.ok() && nothing_published(root));
    }
  }
  ::alarm(0);
}

void test_busy_unknown_space_relative_and_trust_variants() {
  TempDir t;
  const std::string root = t.sub("data");
  fs::create_directories(fs::path(root) / "lib" / ".staging");
  const pfx::Pack p = pfx::make_pack(t.sub("packs"), {}, K1());
  {  // the staging lock is held (an importer or another install is running): busy, nothing written
    const int fd = ::open((root + "/lib/.staging/.lock").c_str(), O_RDWR | O_CREAT, 0644);
    CHECK(fd >= 0 && ::flock(fd, LOCK_EX | LOCK_NB) == 0);
    const InstallResult r = install_protected(opts(root, p));
    CHECK(r.status == InstallStatus::kBusy && !r.ok() && nothing_published(root));
    ::close(fd);
    CHECK(install_protected(opts(root, p)).status == InstallStatus::kInstalled);
  }
  {  // unknown free space skips the check: even a "tiny" figure that comes with `false` does not stop the install
    TempDir u;
    fs::create_directories(u.sub("data"));
    InstallOptions o = opts(u.sub("data"), p);
    o.free_bytes_fn = [](const std::string&, std::uint64_t& f) {
      f = 1;
      return false;
    };
    const InstallResult r = install_protected(o);
    CHECK(r.status == InstallStatus::kInstalled && r.free_bytes == 0);
  }
  {  // a relative bundle path without a directory part: its directory is "."
    TempDir u;
    fs::create_directories(u.sub("data"));
    const fs::path cwd = fs::current_path();
    fs::current_path(p.dir);
    InstallOptions o;
    o.root = u.sub("data");
    o.bundle_path = "bundle.vlrp";
    o.trusted = &K1().trusted;
    const InstallResult r = install_protected(o);
    fs::current_path(cwd);
    CHECK(r.status == InstallStatus::kInstalled);
  }
  {  // the override is accepted only by the dev variant, whatever the build is called
    pfx::Pack q = p;
    char id[17], pub[65];
    for (int i = 0; i < 8; ++i) std::snprintf(id + 2 * i, 3, "%02x", K1().id[i]);
    for (int i = 0; i < 32; ++i) std::snprintf(pub + 2 * i, 3, "%02x", K1().pub[i]);
    const std::string tl = t.sub("v.pub-list");
    pfx::write_bytes(tl, pfx::as_bytes(std::string("dev content-manifest ") + id + " " + pub + "\n"));
    const fmt::TrustedKeys embedded;  // empty, as the committed dev list is
    fmt::TrustedKeys out;
    std::string err;
    CHECK(resolve_trusted_keys_for_variant("dev", false, embedded, tl, out, err) && out.size() == 1);
    CHECK(resolve_trusted_keys_for_variant("dev", false, embedded, "", out, err) && out.size() == 0);
    for (const auto& v : {std::make_pair("factory-test", false), std::make_pair("production", true), std::make_pair("dev", true)}) {
      err.clear();
      out = fmt::TrustedKeys{};
      CHECK(!resolve_trusted_keys_for_variant(v.first, v.second, embedded, tl, out, err) && out.size() == 0 && err.find("dev variant") != std::string::npos);
      CHECK(resolve_trusted_keys_for_variant(v.first, v.second, embedded, "", out, err));  // no override: the embedded list
    }
    // this build
    CHECK(std::string(embedded_trust_root()) == "dev" && trust_list_override_allowed());
    CHECK(resolve_trusted_keys(tl, out, err) && out.size() == 1);
    CHECK(!resolve_trusted_keys(t.sub("nope"), out, err));
    (void)q;
  }
}

void test_space_cancel_drive_removal() {
  TempDir t;
  const std::string root = t.sub("data");
  fs::create_directories(root);
  const pfx::Pack p = pfx::make_pack(t.sub("packs"), {}, K1());
  // space: required = the pack + the import reserve; one byte short is refused with the numbers, one byte more is fine
  const std::uint64_t pack_bytes = p.bundle.size() + p.meta.size() + p.seal.size() + p.wrapped.size();
  CHECK(required_install_bytes(pack_bytes) == pack_bytes + kImportReserveBytes);
  InstallOptions o = opts(root, p);
  o.free_bytes_fn = [&](const std::string&, std::uint64_t& f) {
    f = required_install_bytes(pack_bytes) - 1;
    return true;
  };
  InstallResult r = install_protected(o);
  CHECK(r.status == InstallStatus::kInsufficientSpace && r.required_bytes == required_install_bytes(pack_bytes) && r.free_bytes == r.required_bytes - 1);
  CHECK(nothing_published(root));
  o.free_bytes_fn = [&](const std::string&, std::uint64_t& f) {
    f = required_install_bytes(pack_bytes);
    return true;
  };
  // cancel while the bundle is copied (polled between chunks and blocks): aborted, the staging area removed, nothing published
  {
    int polls = 0;
    InstallOptions c = o;
    c.cancelled = [&] { return ++polls > 2; };
    r = install_protected(c);
    CHECK(r.status == InstallStatus::kAborted && nothing_published(root));
  }
  // the unknown free space skips the check
  o.free_bytes_fn = [](const std::string&, std::uint64_t&) { return false; };
  // drive removal: the source fails to read after the manifest was read, and is then gone
  {
    pfx::PackSpec big;
    big.big = true;
    big.instruments = 1;
    big.frames = 200000;
    const pfx::Pack bp = pfx::make_pack(t.sub("bigpack"), big, K1());
    std::atomic<bool> removed{false};
    InstallOptions d = opts(root, bp);
    int opened = 0;
    d.open_source = [&](const std::string& path) -> std::unique_ptr<fmt::ByteSource> {
      auto s = std::make_unique<CountingSource>(path);
      if (path.size() > 11 && path.compare(path.size() - 11, 11, "bundle.vlrp") == 0 && ++opened == 2) {  // the copy's source: it fails after a few chunks
        s->fail_after = 3;
        removed = true;
      }
      return s;
    };
    d.source_gone = [&] { return removed.load(); };
    r = install_protected(d);
    CHECK(r.status == InstallStatus::kDriveRemoved && nothing_published(root));
    // the same failure with the source still present is an I/O error, not a drive removal
    InstallOptions e = opts(root, bp);
    opened = 0;
    e.open_source = [&](const std::string& path) -> std::unique_ptr<fmt::ByteSource> {
      auto s = std::make_unique<CountingSource>(path);
      if (path.size() > 11 && path.compare(path.size() - 11, 11, "bundle.vlrp") == 0 && ++opened == 2) s->fail_after = 3;
      return s;
    };
    r = install_protected(e);
    CHECK(r.status == InstallStatus::kIo && nothing_published(root));
  }
  // and the pack installs afterwards
  CHECK(install_protected(opts(root, p)).status == InstallStatus::kInstalled);
}

// A fault at every stage point, as a kill would leave it: the next run (reconcile of the staging area, then the same install) ends with the library installed.
void test_stage_faults() {
  const Stage stages[] = {Stage::kStageCreated, Stage::kStaged, Stage::kVerified, Stage::kFsynced, Stage::kRenamed, Stage::kLinkCreated, Stage::kFlipped};
  for (const Stage st : stages) {
    TempDir t;
    const std::string root = t.sub("data");
    fs::create_directories(root);
    const pfx::Pack p = pfx::make_pack(t.sub("packs"), {}, K1());
    InstallOptions o = opts(root, p);
    o.fault.on_stage = [&](Stage s) { return s == st ? HookAction::kAbort : HookAction::kContinue; };
    const InstallResult r = install_protected(o);
    CHECK(r.status == InstallStatus::kAborted);
    const bool renamed = st == Stage::kRenamed || st == Stage::kLinkCreated || st == Stage::kFlipped;
    const bool flipped = st == Stage::kFlipped;
    CHECK(fs::is_directory(fs::path(root) / "lib" / p.library_id / p.bundle_id) == renamed);
    CHECK((current_of(root, p.library_id) == p.bundle_id) == flipped);
    // restart: the staging leftovers go; whatever is published is whole
    CHECK(reconcile_staging(root) >= 0 && staging_clean(root));
    std::unique_ptr<Catalog> cat;
    CHECK(Catalog::open(root, cat) == CatalogStatus::kOk);
    ReconcileReport rep;
    ReconcileOptions ro;
    ro.trusted = &K1().trusted;
    CHECK(reconcile_libraries(*cat, root, rep, ro) == CatalogStatus::kOk && rep.errors.empty());
    std::vector<LibraryRecord> rows;
    CHECK(cat->list_libraries(rows, true) == CatalogStatus::kOk);
    CHECK(rows.size() == (flipped ? 1u : 0u));  // a library is listed only when `current` names its whole bundle
    // the same install again
    const InstallResult again = install_protected(opts(root, p));
    CHECK(again.ok());
    CHECK(flipped ? again.status == InstallStatus::kUnchanged : again.status == InstallStatus::kInstalled);
    CHECK(again.bundle_created == !renamed && current_of(root, p.library_id) == p.bundle_id && staging_clean(root));
  }
}

void test_factory_rows() {
  TempDir t;
  const std::string root = t.sub("data");
  fs::create_directories(root);
  const pfx::Pack p = pfx::make_pack(t.sub("packs"), {}, K1());
  CHECK(install_protected(opts(root, p)).status == InstallStatus::kInstalled);
  const std::string bdir = (fs::path(root) / "lib" / p.library_id / p.bundle_id).string();
  DescribeOptions dopt;
  dopt.trusted = &K1().trusted;
  DescribeResult d = describe_bundle(bdir, dopt);
  CHECK(d.status == DescribeStatus::kOk && d.from_meta && d.record.kind == LibraryKind::kFactory);
  CHECK(d.record.name == "Vendor Grand Piano" && d.record.instruments.size() == 2 && d.record.instruments[0].category == 1);
  CHECK(d.record.instruments[0].name == "Instrument 0" && d.record.source_kind == "sfz");
  // without trusted keys, or with another key, the bundle cannot be examined (not a definite "no")
  CHECK(describe_bundle(bdir).status == DescribeStatus::kUnreadable);
  DescribeOptions other;
  other.trusted = &K2().trusted;
  CHECK(describe_bundle(bdir, other).status == DescribeStatus::kUnreadable);
  // catalog.meta altered after the install: the seal no longer fits
  {
    const fs::path mf = fs::path(bdir) / "catalog.meta";
    Bytes m = pfx::read_bytes(mf);
    const Bytes orig = m;
    m[m.size() / 2] ^= 1;
    pfx::write_bytes(mf, m);
    CHECK(describe_bundle(bdir, dopt).status == DescribeStatus::kMetaUnreadable);
    pfx::write_bytes(mf, orig);
    CHECK(describe_bundle(bdir, dopt).status == DescribeStatus::kOk);
  }
  // reconcile: a Factory row for a library that has none; a second pass changes nothing; without trusted keys the library is skipped and the row kept
  std::unique_ptr<Catalog> cat;
  CHECK(Catalog::open(root, cat) == CatalogStatus::kOk);
  ReconcileReport rep;
  ReconcileOptions ro;
  ro.trusted = &K1().trusted;
  CHECK(reconcile_libraries(*cat, root, rep, ro) == CatalogStatus::kOk && rep.rows_added == 1 && rep.errors.empty() && rep.skipped.empty());
  std::vector<LibraryRecord> rows;
  CHECK(cat->list_libraries(rows, true) == CatalogStatus::kOk && rows.size() == 1 && rows[0].kind == LibraryKind::kFactory && rows[0].bundle_id == p.bundle_id);
  ReconcileReport rep2;
  CHECK(reconcile_libraries(*cat, root, rep2, ro) == CatalogStatus::kOk && !rep2.changed() && rep2.rows_unchanged == 1);
  ReconcileReport rep3;
  CHECK(reconcile_libraries(*cat, root, rep3) == CatalogStatus::kOk && rep3.rows_removed == 0 && rep3.skipped.size() == 1 && rep3.uncertain);
  CHECK(cat->list_libraries(rows, false) == CatalogStatus::kOk && rows.size() == 1);
  // rebuilding the catalog from the bundles alone brings the Factory kind back (the kind is derived from the bundle, not from the catalog)
  ReconcileReport rep4;
  CHECK(rebuild_libraries(*cat, root, rep4, ro) == CatalogStatus::kOk);
  CHECK(cat->list_libraries(rows, false) == CatalogStatus::kOk && rows.size() == 1 && rows[0].kind == LibraryKind::kFactory);
  // verify_new_rows hashes the ciphertext of a Factory bundle: a block damaged on disk keeps it out of the catalog
  {
    TempDir u;
    fs::create_directories(u.sub("data"));
    CHECK(install_protected(opts(u.sub("data"), p)).status == InstallStatus::kInstalled);
    const fs::path bf = fs::path(u.sub("data")) / "lib" / p.library_id / p.bundle_id / "bundle.vlrp";
    Bytes b = pfx::read_bytes(bf);
    b[b.size() - 3] ^= 1;
    pfx::write_bytes(bf, b);
    std::unique_ptr<Catalog> c2;
    CHECK(Catalog::open(u.sub("data"), c2) == CatalogStatus::kOk);
    ReconcileReport r5;
    CHECK(reconcile_libraries(*c2, u.sub("data"), r5, ro) == CatalogStatus::kOk && r5.rows_added == 0 && r5.skipped.size() == 1);
    CHECK(r5.skipped[0].find("block_hash_mismatch") != std::string::npos);
  }
}

int run_cli(const std::string& args, std::string& err_out) {
  const std::string errf = "/tmp/vl_install_cli_" + std::to_string(::getpid()) + ".txt";
  const int rc = std::system(("'" LIBRARIAN_EXE "' " + args + " >/dev/null 2>'" + errf + "'").c_str());
  err_out = ltest::slurp(errf);
  std::remove(errf.c_str());
  return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

// The previous Factory bundle stays as a lease through a reconcile; once a third revision prunes it, a reader that had it open still reads it.
void test_factory_leases() {
  TempDir t;
  const std::string root = t.sub("data");
  fs::create_directories(root);
  pfx::PackSpec s1, s2, s3;
  s2.variant = 1;
  s2.revision = 2;
  s3.variant = 2;
  s3.revision = 3;
  const pfx::Pack p1 = pfx::make_pack(t.sub("p1"), s1, K1()), p2 = pfx::make_pack(t.sub("p2"), s2, K1()), p3 = pfx::make_pack(t.sub("p3"), s3, K1());
  CHECK(install_protected(opts(root, p1)).status == InstallStatus::kInstalled);
  const std::string old_file = (fs::path(root) / "lib" / p1.library_id / p1.bundle_id / "bundle.vlrp").string();
  const int lease = ::open(old_file.c_str(), O_RDONLY);
  CHECK(lease >= 0);
  CHECK(install_protected(opts(root, p2)).status == InstallStatus::kInstalled);
  std::unique_ptr<Catalog> cat;
  CHECK(Catalog::open(root, cat) == CatalogStatus::kOk);
  ReconcileOptions ro;
  ro.trusted = &K1().trusted;
  ReconcileReport rep;
  CHECK(reconcile_libraries(*cat, root, rep, ro) == CatalogStatus::kOk && rep.errors.empty() && rep.skipped.empty() && rep.rows_added == 1);
  std::vector<LibraryRecord> rows;
  CHECK(cat->list_libraries(rows, false) == CatalogStatus::kOk && rows.size() == 1 && rows[0].bundle_id == p2.bundle_id && rows[0].kind == LibraryKind::kFactory);
  CHECK(fs::exists(old_file) && !rep.changed() == false);  // the lease directory was left alone (rows_added only)
  CHECK(install_protected(opts(root, p3)).pruned == 1 && !fs::exists(old_file));
  char magic[4] = {};
  CHECK(::pread(lease, magic, 4, 0) == 4 && std::string(magic, 4) == "VLRB");
  ::close(lease);
  ReconcileReport rep2;
  CHECK(reconcile_libraries(*cat, root, rep2, ro) == CatalogStatus::kOk && rep2.rows_updated == 1 && rep2.skipped.empty());
  CHECK(cat->list_libraries(rows, false) == CatalogStatus::kOk && rows.size() == 1 && rows[0].bundle_id == p3.bundle_id);
  // rebuild without trusted keys must not lose the Factory row
  ReconcileReport rep3;
  CHECK(rebuild_libraries(*cat, root, rep3) == CatalogStatus::kOk);
  CHECK(cat->list_libraries(rows, false) == CatalogStatus::kOk && rows.size() == 1 && rows[0].kind == LibraryKind::kFactory && rows[0].bundle_id == p3.bundle_id);
  ReconcileReport rep4;
  CHECK(rebuild_libraries(*cat, root, rep4, ro) == CatalogStatus::kOk);
  CHECK(cat->list_libraries(rows, false) == CatalogStatus::kOk && rows.size() == 1 && rows[0].kind == LibraryKind::kFactory);
}

void test_cli() {
  TempDir t;
  const std::string root = t.sub("data");
  fs::create_directories(root);
  const pfx::Pack p = pfx::make_pack(t.sub("packs"), {}, K1());
  char id[17], pub[65];
  for (int i = 0; i < 8; ++i) std::snprintf(id + 2 * i, 3, "%02x", K1().id[i]);
  for (int i = 0; i < 32; ++i) std::snprintf(pub + 2 * i, 3, "%02x", K1().pub[i]);
  const std::string tl = t.sub("trust.pub-list");
  pfx::write_bytes(tl, pfx::as_bytes(std::string("dev content-manifest ") + id + " " + pub + "\n"));
  std::string err;
  CHECK(run_cli("install-protected '" + p.dir.string() + "' --root '" + root + "' --trust-list '" + tl + "'", err) == 0 && err.empty());
  CHECK(current_of(root, p.library_id) == p.bundle_id);
  CHECK(run_cli("install-protected '" + p.dir.string() + "' --root '" + root + "' --trust-list '" + tl + "'", err) == 0);  // unchanged
  // the embedded dev list is empty in the repository: a pack signed by a throwaway key is refused with the typed reason
  TempDir t2;
  fs::create_directories(t2.sub("data"));
  CHECK(run_cli("install-protected '" + p.dir.string() + "' --root '" + t2.sub("data") + "'", err) == 1 && err.find("untrusted_key") != std::string::npos);
  const std::string tl2 = t.sub("trust2.pub-list");
  char id2[17], pub2[65];
  for (int i = 0; i < 8; ++i) std::snprintf(id2 + 2 * i, 3, "%02x", K2().id[i]);
  for (int i = 0; i < 32; ++i) std::snprintf(pub2 + 2 * i, 3, "%02x", K2().pub[i]);
  pfx::write_bytes(tl2, pfx::as_bytes(std::string("dev content-manifest ") + id2 + " " + pub2 + "\n"));
  CHECK(run_cli("install-protected '" + p.dir.string() + "' --root '" + t2.sub("data") + "' --trust-list '" + tl2 + "'", err) == 1);
  CHECK(err.find("rejected (untrusted_key)") != std::string::npos && err.find("nothing published") != std::string::npos && nothing_published(t2.sub("data")));
  // a flipped bit: rejected, typed
  {
    pfx::Pack q = clone(p, t.sub("cli_bad") + "/" + p.library_id);
    Bytes b = p.bundle;
    b[b.size() - 10] ^= 0x01;
    pfx::write_bytes(q.dir / "bundle.vlrp", b);
    CHECK(run_cli("install-protected '" + q.dir.string() + "' --root '" + t2.sub("data") + "' --trust-list '" + tl + "'", err) == 1);
    CHECK(err.find("rejected (block_hash_mismatch)") != std::string::npos && nothing_published(t2.sub("data")));
  }
  CHECK(run_cli("install-protected", err) == 2);
  CHECK(run_cli("install-protected '" + p.dir.string() + "'", err) == 2);  // no --root
  CHECK(run_cli("install-protected '" + p.dir.string() + "' --root '" + root + "' --trust-list '" + t.sub("nope") + "'", err) == 2);
  CHECK(run_cli("install-protected '" + p.dir.string() + "' --root '" + root + "' --bogus x", err) == 2);
}

}  // namespace

int main() {
  test_happy_path();
  test_idempotent_and_revisions();
  test_refusals();
  test_trust_and_signature();
  test_library_id();
  test_wrapped_key();
  test_bitflip_sweep();
  test_signed_but_wrong();
  test_catalog_meta_binding();
  test_bounded_memory();
  test_no_unbounded_allocation();
  test_fifo_does_not_hang();
  test_busy_unknown_space_relative_and_trust_variants();
  test_bounded_memory_measured();
  test_space_cancel_drive_removal();
  test_stage_faults();
  test_factory_rows();
  test_factory_leases();
  test_cli();
  if (g_fail == 0) std::printf("librarian_install_test: ok\n");
  return g_fail == 0 ? 0 : 1;
}
