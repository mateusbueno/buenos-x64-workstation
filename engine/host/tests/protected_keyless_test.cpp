// Story 6.5, the parts of the protected-library support that hold NO device key and need no dev key provider, so they are built and run even in a configuration
// with VELORA_DEV_KEY_PROVIDER=OFF: SecureKeyHandle (locked, dump-excluded, wiped), the DamageRegistry, the trust-list parser, and the resolver's verification of
// bundle.vlrp (typed bundle_error per damaged region, the downgrade by a planted bundle.vlb, the constant op count).
#include <sys/resource.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "protected_fixtures.hpp"
#include "velora/host/damage_registry.hpp"
#include "velora/host/fs_port.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/host/secure_memory.hpp"
#include "velora/host/trust_list.hpp"

namespace fs = std::filesystem;
using namespace velora;

static int g_fail = 0;
#define CHECK(c)                                               \
  do {                                                         \
    if (!(c)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

namespace {

using pfx::Bytes;
using pfx::TmpDir;

std::shared_ptr<const format::TrustedKeys> trust_of(const pfx::Keys& k) { return std::make_shared<const format::TrustedKeys>(k.trusted); }

void flip_byte(const fs::path& p, std::uint64_t at) {
  std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
  f.seekg(static_cast<std::streamoff>(at));
  char c = 0;
  f.get(c);
  f.seekp(static_cast<std::streamoff>(at));
  f.put(static_cast<char>(c ^ 0x10));
}

void test_secure_key_handle() {
  std::vector<bool> zeros;
  std::vector<host::SecureKeyHandle::WipeEvent> events;
  host::SecureKeyHandle::Options o;
  o.on_wipe = [&](const host::SecureKeyHandle::WipeEvent& e) {
    bool z = true;
    for (std::size_t i = 0; i < e.size; ++i) z = z && e.memory[i] == 0;
    zeros.push_back(z);
    events.push_back(e);
  };
  ports::KeyStatus st = ports::KeyStatus::kInternal;
  std::weak_ptr<host::SecureKeyHandle> weak;
  {
    auto h = host::SecureKeyHandle::allocate(o, st);
    CHECK(h && st == ports::KeyStatus::kOk && h->locked());
    if (h) {
      weak = h;
      std::memset(h->writable().data(), 0xAA, 32);
      std::memset(h->extra(0, 64).data(), 0xBB, 64);  // the unwrap temporaries live in the free part of the page
      CHECK(h->extra_capacity() >= 64 && h->page_size() >= 4096);
#if defined(MADV_DONTDUMP)
      CHECK(h->dump_excluded());
#endif
#if defined(__linux__)
      // /proc/self/smaps: the key's page is locked ("lo") and excluded from core dumps ("dd").
      std::ifstream sm("/proc/self/smaps");
      std::string line;
      const auto addr = reinterpret_cast<std::uintptr_t>(h->page_address());
      bool in_region = false, found = false;
      while (std::getline(sm, line)) {
        unsigned long long lo_a = 0, hi_a = 0;
        if (std::sscanf(line.c_str(), "%llx-%llx ", &lo_a, &hi_a) == 2) in_region = addr >= lo_a && addr < hi_a;
        else if (in_region && line.rfind("VmFlags:", 0) == 0) {
          found = true;
          CHECK(line.find(" lo") != std::string::npos);
          CHECK(line.find(" dd") != std::string::npos);
        }
      }
      CHECK(found);
#endif
      auto copy = h;  // a second reference: nothing is wiped while either exists
      h.reset();
      CHECK(events.empty());
    }
  }
  CHECK(weak.expired() && events.size() == 1 && zeros[0] && events[0].locked);  // wiped (the whole page, extras included), observed in the very memory

  // A lock that fails refuses the key; nothing is leaked or reported.
  host::SecureKeyHandle::Options bad;
  int tries = 0;
  bad.lock_fn = [&](void*, std::size_t) {
    ++tries;
    return false;
  };
  CHECK(host::SecureKeyHandle::allocate(bad, st) == nullptr && st == ports::KeyStatus::kMemoryLockFailed && tries == 1);

  CHECK(host::disable_core_dumps());
  struct rlimit rl;
  CHECK(::getrlimit(RLIMIT_CORE, &rl) == 0 && rl.rlim_cur == 0 && rl.rlim_max == 0);
  CHECK(host::memlock_budget_bytes() != 0 && host::secure_page_bytes() >= 4096);
  std::uint8_t buf[64];
  std::memset(buf, 0xAB, sizeof buf);
  host::secure_wipe(buf, sizeof buf);
  bool z = true;
  for (std::uint8_t v : buf) z = z && v == 0;
  CHECK(z);
}

void test_damage_registry() {
  host::DamageRegistry reg;
  int calls = 0;
  reg.set_listener([&] { ++calls; });
  CHECK(reg.reports() == 0 && reg.io_errors() == 0 && reg.damaged_libraries().empty() && reg.seq() == 0);
  reg.report({"lib-b", "x.sfz", 0, 3, "content_integrity"});
  reg.report({"lib-a", "y.sfz", 1, 0, "content_integrity"});
  reg.report({"lib-b", "x.sfz", 0, 4, "content_integrity"});
  CHECK(reg.reports() == 3 && calls == 3 && reg.library_damaged("lib-a") && reg.library_damaged("lib-b") && !reg.library_damaged("lib-c"));
  CHECK((reg.damaged_libraries() == std::vector<std::string>{"lib-a", "lib-b"}) && reg.events().size() == 3);
  reg.report_io_error("lib-a");
  CHECK(reg.io_errors() == 1 && calls == 4 && reg.damaged_libraries().size() == 2);  // an I/O error damages nothing
  const std::uint64_t seq = reg.seq();
  reg.clear_library("lib-b");  // reloaded: no longer damaged, its events go, the monotone counters stay
  CHECK(!reg.library_damaged("lib-b") && reg.events().size() == 1 && reg.reports() == 3 && calls == 5 && reg.seq() > seq);
  reg.clear_library("lib-b");  // already clean: nothing happens, no push
  CHECK(calls == 5);
  for (int i = 0; i < 100; ++i) reg.report({"lib-z", "z", 0, static_cast<std::uint64_t>(i), "content_integrity"});
  CHECK(reg.events().size() <= host::DamageRegistry::kMaxEvents && reg.reports() == 103);  // events are capped for display, the count is exact
}

void test_trust_list_parser() {
  TmpDir tmp;
  const pfx::Keys k = pfx::make_keys(5), k2 = pfx::make_keys(6);
  const std::string id = format::to_hex(k.sign_id), pub = format::to_hex(k.sign_pub), id2 = format::to_hex(k2.sign_id), pub2 = format::to_hex(k2.sign_pub);
  auto write = [&](const std::string& name, const std::string& text) {
    const fs::path p = fs::path(tmp.path) / name;
    std::ofstream(p) << text;
    return p.string();
  };
  auto load = [&](const std::string& text, std::string& err) {
    format::TrustedKeys t;
    const bool ok = host::load_trust_list_file(write("list", text), t, err);
    return ok ? static_cast<int>(t.size()) : -1;
  };
  std::string err;
  CHECK(load("# comment only\n", err) == 0);
  CHECK(load("dev content-manifest " + id + " " + pub + "  # trailing comment\n\n" + "dev content-manifest " + id2 + " " + pub2 + "\n", err) == 2);
  CHECK(load("dev content-manifest " + id + " " + pub + "\n" + "dev content-manifest " + id + " " + pub + "\n", err) == -1 && err.find("listed twice") != std::string::npos);
  CHECK(load("dev content-manifest " + id2 + " " + pub + "\n", err) == -1 && err.find("first 8 bytes of SHA-256") != std::string::npos);  // a key id that is not the key's
  std::string bad_hex = pub;
  bad_hex[3] = 'z';
  CHECK(load("dev content-manifest " + id + " " + bad_hex + "\n", err) == -1 && err.find("not hex") != std::string::npos);
  CHECK(load("dev content-manifest " + id + " " + pub.substr(2) + "\n", err) == -1 && err.find("not hex") != std::string::npos);  // wrong length
  CHECK(load("dev signing " + id + " " + pub + "\n", err) == -1 && err.find("unknown role") != std::string::npos);
  CHECK(load("dev content-manifest " + id + "\n", err) == -1 && err.find("expected") != std::string::npos);
  CHECK(load("dev content-manifest " + id + " " + pub + " extra\n", err) == -1);
  format::TrustedKeys t;
  CHECK(!host::load_trust_list_file((fs::path(tmp.path) / "missing").string(), t, err) && err.find("cannot be read") != std::string::npos);
}

void test_resolver_verification() {
  TmpDir tmp;
  const pfx::Keys keys = pfx::make_keys(5);
  const std::vector<pfx::Spec> specs = {{"piano.sfz", pfx::Spec::kConstFloat, 0.1f}};
  const auto lib = pfx::install_library(tmp.path, 1, specs, keys);
  const auto plain = pfx::install_library(tmp.path, 2, specs, keys, {.protect = false});
  auto counting = std::make_shared<host::CountingFs>(host::make_posix_fs());
  host::LibraryResolver res(counting, tmp.path);
  // A host with no trusted keys plays no protected library (it never opens one on trust): typed, and the part stays silent.
  {
    auto r = res.resolve(lib.library_id, "piano.sfz");
    CHECK(r.status == host::ResolveStatus::kProtectedUnsupported && !r.ok());
    CHECK(std::string(host::to_string(r.status)) == "protected_unsupported");
  }
  res.set_protected_trust(trust_of(keys));
  counting->reset();
  auto r = res.resolve(lib.library_id, "piano.sfz");
  CHECK(r.ok() && r.is_protected() && r.protected_view && !r.bundle);
  CHECK(r.library_id == lib.library_id && r.bundle_dir == lib.dir.string() && r.instrument_info().key == "piano.sfz");
  CHECK(counting->counts().list_dir == 0);
  // An unprotected library: the op count is the constant of the resolver (4 lstat, readlink, open, 3 pread, close = 10), whatever sits beside it.
  counting->reset();
  {
    auto u = res.resolve(plain.library_id, "piano.sfz");
    CHECK(u.ok() && !u.is_protected() && u.bundle);
  }  // the result's bundle closes its descriptor here
  const host::FsOpCounts c = counting->counts();
  CHECK(c.lstat == 4 && c.readlink == 1 && c.open == 1 && c.pread == 3 && c.close == 1 && c.list_dir == 0 && c.total() == 10);
  CHECK(res.resolve(lib.library_id, "nope.sfz").status == host::ResolveStatus::kMissingInstrument);
  CHECK(res.resolve("../etc", "piano.sfz").status == host::ResolveStatus::kInvalidId);
  // A signer this engine does not trust.
  {
    host::LibraryResolver r2(host::make_posix_fs(), tmp.path);
    r2.set_protected_trust(trust_of(pfx::make_keys(6)));
    auto t = r2.resolve(lib.library_id, "piano.sfz");
    CHECK(t.status == host::ResolveStatus::kBundleError && t.bundle_error == format::Error::kUntrustedKey && t.from_protected);
  }

  // One flipped byte in each region of the signed file: the EXACT typed cause, from_protected set, no instrument.
  const std::uint64_t size = lib.bundle.size();
  const std::uint64_t blocks_bytes = 4096ull * 4 * 2 + (12000 - 8192) * 4;
  const std::uint64_t seal_at = size - blocks_bytes - 128;
  std::uint64_t msize = 0;
  for (int i = 7; i >= 0; --i) msize = (msize << 8) | lib.bundle[16 + static_cast<std::size_t>(i)];
  struct Case {
    const char* what;
    std::uint64_t at;
    format::Error want;
  };
  const Case cases[] = {
      {"header magic", 0, format::Error::kBadMagic},
      {"header (index hash field)", 100, format::Error::kCorruptIndex},
      {"manifest", 128 + 20, format::Error::kCorruptManifest},
      {"manifest (last byte)", 128 + msize - 1, format::Error::kCorruptManifest},
      {"index", 128 + msize + 10, format::Error::kCorruptIndex},
      {"seal signer key id", seal_at + 10, format::Error::kUntrustedKey},
      {"seal signature", seal_at + 20, format::Error::kSignatureInvalid},
      {"seal signature (last byte)", seal_at + 79, format::Error::kSignatureInvalid},
  };
  for (const Case& cs : cases) {
    TmpDir t2;
    const auto copy = pfx::install_library(t2.path, 1, specs, keys);
    flip_byte(copy.dir / "bundle.vlrp", cs.at);
    host::LibraryResolver r2(host::make_posix_fs(), t2.path);
    r2.set_protected_trust(trust_of(keys));
    const auto rr = r2.resolve(copy.library_id, "piano.sfz");
    if (rr.status != host::ResolveStatus::kBundleError || rr.bundle_error != cs.want || !rr.from_protected || rr.protected_view) {
      std::printf("FAIL: flip in %s at %llu: status %s, error %s (want %s)\n", cs.what, static_cast<unsigned long long>(cs.at), host::to_string(rr.status), format::to_string(rr.bundle_error),
                  format::to_string(cs.want));
      ++g_fail;
    }
  }
  // A bundle copied under another library id is the wrong identity, not a library.
  {
    TmpDir t2;
    const auto a = pfx::install_library(t2.path, 1, specs, keys);
    const std::string other_lib = format::uuid_to_string(pfx::library_uuid(77));
    fs::create_directories(fs::path(t2.path) / "lib" / other_lib);
    fs::copy(a.dir, fs::path(t2.path) / "lib" / other_lib / a.bundle_id);
    fs::create_symlink(a.bundle_id, fs::path(t2.path) / "lib" / other_lib / "current");
    host::LibraryResolver r2(host::make_posix_fs(), t2.path);
    r2.set_protected_trust(trust_of(keys));
    CHECK(r2.resolve(other_lib, "piano.sfz").status == host::ResolveStatus::kIdentityMismatch);
  }

  // The downgrade: an unprotected bundle.vlb planted beside the protected files of a factory library is refused, typed, before it is read.
  {
    TmpDir t2;
    const auto a = pfx::install_library(t2.path, 1, specs, keys);
    // The attacker's bundle.vlb: the same library id and the same bundle directory name is impossible for different bytes, so the planted file is simply an unprotected
    // bundle of that library placed in the bundle directory; the resolver refuses on the names alone.
    format::BundleWriter w;
    w.set_library_id(pfx::library_uuid(1));
    pfx::add_spec(w, specs[0]);
    auto plain_bytes = w.finalize();
    CHECK(plain_bytes.ok());
    host::LibraryResolver r2(host::make_posix_fs(), t2.path);
    r2.set_protected_trust(trust_of(keys));
    CHECK(r2.resolve(a.library_id, "piano.sfz").ok());  // before the planting: plays
    pfx::write_file(a.dir / "bundle.vlb", plain_bytes.value());
    auto d = r2.resolve(a.library_id, "piano.sfz");
    CHECK(d.status == host::ResolveStatus::kProtectedDowngrade && !d.ok() && std::string(host::to_string(d.status)) == "protected_downgrade");
    // wrapped-key.bin alone beside a bundle.vlb is enough as well (the vlrp removed, the key blob left).
    fs::remove(a.dir / "bundle.vlrp");
    CHECK(r2.resolve(a.library_id, "piano.sfz").status == host::ResolveStatus::kProtectedDowngrade);
    fs::remove(a.dir / "wrapped-key.bin");
    CHECK(r2.resolve(a.library_id, "piano.sfz").status != host::ResolveStatus::kProtectedDowngrade);  // vlb only: an ordinary (here mis-identified) unprotected library
  }
  {  // only a bundle.vlb (a normal user library): unchanged behaviour
    TmpDir t2;
    const auto u2 = pfx::install_library(t2.path, 3, specs, keys, {.protect = false});
    host::LibraryResolver r2(host::make_posix_fs(), t2.path);
    CHECK(r2.resolve(u2.library_id, "piano.sfz").ok());
    r2.set_protected_trust(trust_of(keys));
    CHECK(r2.resolve(u2.library_id, "piano.sfz").ok());
  }
}

}  // namespace

int main() {
  test_secure_key_handle();
  test_damage_registry();
  test_trust_list_parser();
  test_resolver_verification();
  if (g_fail == 0) std::printf("protected_keyless_test: ok\n");
  return g_fail == 0 ? 0 : 1;
}
