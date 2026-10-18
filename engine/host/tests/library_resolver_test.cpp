// Library resolver tests (Story 1.14). Timings are printed for the developer's Mac and are
// "host, not representative"; only loose sanity bounds are asserted.
#include <unistd.h>

#include <algorithm>
#include <sys/stat.h>

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "library_gen.hpp"
#include "velora/host/fs_port.hpp"
#include "velora/host/library_resolver.hpp"

namespace fs = std::filesystem;
using namespace velora::host;
using velora::libgen::kInstrumentA;
using velora::libgen::kInstrumentB;
using velora::libgen::library_id_for_index;

static int g_fail = 0;
#define CHECK(c)                                               \
  do {                                                         \
    if (!(c)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

namespace {

// Records every path handed to the port.
class RecordingFs final : public FsPort {
 public:
  explicit RecordingFs(std::shared_ptr<FsPort> inner) : inner_(std::move(inner)) {}
  FsStat lstat(const std::string& p) override { note(p); return inner_->lstat(p); }
  FsStatus readlink(const std::string& p, std::string& t) override { note(p); return inner_->readlink(p, t); }
  FsStatus open_read(const std::string& p, int& fd, std::uint64_t& s) override { note(p); return inner_->open_read(p, fd, s); }
  FsStatus pread(int fd, std::uint64_t o, void* d, std::size_t n) override { return inner_->pread(fd, o, d, n); }
  void close(int fd) override { inner_->close(fd); }
  FsStatus list_dir(const std::string& p, std::vector<std::string>& n) override { note(p); return inner_->list_dir(p, n); }
  std::vector<std::string> paths;

 private:
  void note(const std::string& p) { paths.push_back(p); }
  std::shared_ptr<FsPort> inner_;
};

struct Env {
  std::shared_ptr<CountingFs> counting = std::make_shared<CountingFs>(make_posix_fs());
  LibraryResolver resolver;
  explicit Env(const std::string& root) : resolver(counting, root) {}
};

// Temp tree removed on scope exit, including on early return.
struct TmpDir {
  std::string path;
  TmpDir() {
    path = (fs::temp_directory_path() / "velora-libres-XXXXXX").string();
    if (!mkdtemp(path.data())) { std::printf("mkdtemp failed\n"); std::exit(1); }
  }
  ~TmpDir() { std::error_code ec; fs::remove_all(path, ec); }
  TmpDir(const TmpDir&) = delete;
  TmpDir& operator=(const TmpDir&) = delete;
};

// Counts successful opens and closes to detect descriptor leaks.
class LeakTracker final : public FsPort {
 public:
  explicit LeakTracker(std::shared_ptr<FsPort> inner) : inner_(std::move(inner)) {}
  FsStat lstat(const std::string& p) override { return inner_->lstat(p); }
  FsStatus readlink(const std::string& p, std::string& t) override { return inner_->readlink(p, t); }
  FsStatus open_read(const std::string& p, int& fd, std::uint64_t& s) override {
    const FsStatus r = inner_->open_read(p, fd, s);
    if (r == FsStatus::kOk) ++opened;
    return r;
  }
  FsStatus pread(int fd, std::uint64_t o, void* d, std::size_t n) override { return inner_->pread(fd, o, d, n); }
  void close(int fd) override { ++closed; inner_->close(fd); }
  FsStatus list_dir(const std::string& p, std::vector<std::string>& n) override { return inner_->list_dir(p, n); }
  int opened = 0, closed = 0;

 private:
  std::shared_ptr<FsPort> inner_;
};

// FsPort stub that injects faults into an otherwise real port.
class FaultFs final : public FsPort {
 public:
  FaultFs(std::shared_ptr<FsPort> inner, bool lstat_io, bool readlink_io, int pread_fail_on)
      : inner_(std::move(inner)), lstat_io_(lstat_io), readlink_io_(readlink_io), fail_on_(pread_fail_on) {}
  FsStat lstat(const std::string& p) override {
    if (lstat_io_) return FsStat{FsStatus::kIo, FsKind::kNone, 0};
    return inner_->lstat(p);
  }
  FsStatus readlink(const std::string& p, std::string& t) override {
    return readlink_io_ ? FsStatus::kIo : inner_->readlink(p, t);
  }
  FsStatus open_read(const std::string& p, int& fd, std::uint64_t& s) override { return inner_->open_read(p, fd, s); }
  FsStatus pread(int fd, std::uint64_t o, void* d, std::size_t n) override {
    if (++preads_ == fail_on_) return FsStatus::kIo;
    return inner_->pread(fd, o, d, n);
  }
  void close(int fd) override { inner_->close(fd); }
  FsStatus list_dir(const std::string& p, std::vector<std::string>& n) override { return inner_->list_dir(p, n); }

 private:
  std::shared_ptr<FsPort> inner_;
  bool lstat_io_, readlink_io_;
  int fail_on_, preads_ = 0;
};

// Returns false when the requested sparse padding could not be created.
bool gen(const std::string& root, std::uint32_t n, std::uint64_t pad = 0, std::uint32_t first = 0) {
  velora::libgen::Options o;
  o.root = root;
  o.libraries = n;
  o.pad_bytes = pad;
  o.first_index = first;
  bool pad_ok = true;
  std::string err;
  if (!velora::libgen::generate_all(o, &pad_ok, &err)) {
    std::printf("generate failed: %s\n", err.c_str());
    std::exit(1);
  }
  return pad_ok;
}

std::string bundle_dir(const std::string& root, std::uint32_t idx) {
  const std::string link = root + "/lib/" + library_id_for_index(idx) + "/current";
  return root + "/lib/" + library_id_for_index(idx) + "/" + fs::read_symlink(link).string();
}

std::vector<std::uint8_t> slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
void spit(const std::string& p, const std::vector<std::uint8_t>& b) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

struct Scale {
  FsOpCounts ops;
  double avg_us;
};

// Resolves a library from the middle of the tree; returns op counts for one resolve (including the
// close when the handle is released) and an average wall time.
Scale measure(const std::string& root, std::uint32_t idx) {
  Env env(root);
  Scale s{};
  {
    auto r = env.resolver.resolve(library_id_for_index(idx), kInstrumentA);
    CHECK(r.ok());
    if (r.ok()) {
      CHECK(r.instrument_info().key == kInstrumentA);
      CHECK(r.bundle->samples().size() == 2);
    }
  }
  s.ops = env.counting->counts();
  const int kIters = 20;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kIters; ++i) {
    auto r = env.resolver.resolve(library_id_for_index(idx), kInstrumentB);
    CHECK(r.ok());
  }
  const auto t1 = std::chrono::steady_clock::now();
  s.avg_us = std::chrono::duration<double, std::micro>(t1 - t0).count() / kIters;
  return s;
}

void test_hit_and_ops() {
  TmpDir tmp_;
  const std::string root = tmp_.path;
  gen(root, 10);
  Env env(root);
  {
    auto r = env.resolver.resolve(library_id_for_index(3), kInstrumentA);
    CHECK(r.status == ResolveStatus::kOk);
    CHECK(r.bundle && r.bundle->library_id().size() == 16);
    CHECK(velora::format::uuid_to_string(r.bundle->library_id()) == library_id_for_index(3));
    CHECK(r.instrument_info().sample_count == 1);
    // The handle is usable: decode a block from the opened bundle.
    std::vector<std::int32_t> out(velora::format::kBlockFrames * 2);
    auto n = r.bundle->read_block(r.instrument_info().first_sample, 0, out);
    CHECK(n.ok() && n.value() == velora::format::kBlockFrames);
  }
  const FsOpCounts c = env.counting->counts();
  // lstat(current), lstat(bundle dir) | readlink | open | pread header, manifest, index | close.
  // The pread of block 0 above is test code, so it is counted here too.
  CHECK(c.lstat == 2);
  CHECK(c.readlink == 1);
  CHECK(c.open == 1);
  CHECK(c.pread == 4);
  CHECK(c.close == 1);
  CHECK(c.list_dir == 0);
}

void test_scale() {
  struct Case { std::uint32_t n; std::uint64_t pad; };
  std::vector<Case> cases = {{10, 0}, {1000, std::uint64_t(1) << 30}, {3000, 0}};
  const char* big = std::getenv("VELORA_SCALE_TEST");
  if (big && std::string(big) == "1") cases.push_back({20000, 0});
  else std::printf("scale: 20,000-library case skipped (set VELORA_SCALE_TEST=1 to run it)\n");
  std::vector<Scale> res;
  for (const Case& c : cases) {
    TmpDir tmp_;
  const std::string root = tmp_.path;
    const auto g0 = std::chrono::steady_clock::now();
    if (!gen(root, c.n, c.pad)) {
      std::printf("scale: %u libraries SKIPPED: sparse padding of %llu GiB not possible here\n", c.n,
                  static_cast<unsigned long long>(c.pad >> 30));
      continue;
    }
    const double gs = std::chrono::duration<double>(std::chrono::steady_clock::now() - g0).count();
    const Scale s = measure(root, c.n / 2);
    std::printf("scale: %u libraries (pad %llu GiB each, sparse): ops lstat=%llu readlink=%llu open=%llu "
                "pread=%llu close=%llu list_dir=%llu, resolve avg %.1f us (host, not representative; "
                "generation %.1f s)\n",
                c.n, static_cast<unsigned long long>(c.pad >> 30),
                (unsigned long long)s.ops.lstat, (unsigned long long)s.ops.readlink,
                (unsigned long long)s.ops.open, (unsigned long long)s.ops.pread,
                (unsigned long long)s.ops.close, (unsigned long long)s.ops.list_dir, s.avg_us, gs);
    res.push_back(s);
    CHECK(s.ops.list_dir == 0);
    CHECK(s.avg_us < 50000.0);  // loose absolute sanity bound only
    {
      // Startup: resolving a few requested libraries touches only their own paths.
      auto rec = std::make_shared<RecordingFs>(make_posix_fs());
      auto counting = std::make_shared<CountingFs>(rec);
      LibraryResolver resolver(counting, root);
      const std::uint32_t picks[] = {0, c.n / 3, c.n - 1};
      for (std::uint32_t p : picks) CHECK(resolver.resolve(library_id_for_index(p), kInstrumentA).ok());
      CHECK(counting->counts().list_dir == 0);
      CHECK(counting->counts().open == 3);
      CHECK(counting->counts().total() == 3 * res[0].ops.total());
      for (const std::string& path : rec->paths) {
        bool belongs = false;
        for (std::uint32_t p : picks) belongs |= path.find(library_id_for_index(p)) != std::string::npos;
        CHECK(belongs);
        CHECK(path != root + "/lib" && path != root);
      }
    }
  }
  for (const Scale& s : res) {
    CHECK(s.ops == res[0].ops);
    CHECK(s.ops.total() == 2 + 1 + 1 + 3 + 1);
  }
}

void test_missing_and_hostile() {
  TmpDir tmp_;
  const std::string root = tmp_.path;
  gen(root, 4);
  Env env(root);

  // Unknown (well-formed) library: one lstat, nothing else.
  auto r = env.resolver.resolve("00000000-0000-5000-8000-000000000000", kInstrumentA);
  CHECK(r.status == ResolveStatus::kMissingLibrary && !r.bundle);
  CHECK(env.counting->counts().total() == 1);

  // Dangling current.
  const std::string id2 = library_id_for_index(2);
  fs::remove(root + "/lib/" + id2 + "/current");
  fs::create_directory_symlink(std::string(64, 'a'), root + "/lib/" + id2 + "/current");
  CHECK(env.resolver.resolve(id2, kInstrumentA).status == ResolveStatus::kMissingLibrary);

  // Missing instrument; other instruments of that library are not substituted.
  r = env.resolver.resolve(library_id_for_index(1), "piano/other.sfz");
  CHECK(r.status == ResolveStatus::kMissingInstrument && !r.bundle);
  r = env.resolver.resolve(library_id_for_index(1), "Piano/Soft.sfz");  // case-sensitive
  CHECK(r.status == ResolveStatus::kMissingInstrument);
  // Normalised form of the key resolves.
  CHECK(env.resolver.resolve(library_id_for_index(1), "piano\\soft.sfz").ok());
  CHECK(env.resolver.resolve(library_id_for_index(1), "./piano//soft.sfz").ok());

  // Hostile ids: rejected with zero filesystem ops.
  env.counting->reset();
  const std::string good = library_id_for_index(0);
  const char* bad_libs[] = {"", "../x", "/etc/passwd", "..", ".", "not-a-uuid",
                            "00000000-0000-5000-8000-00000000000",    // short
                            "00000000-0000-5000-8000-0000000000000",  // long
                            "00000000_0000_5000_8000_000000000000",
                            "0000000000005000800000000000000000000",
                            "../../00000000-0000-5000-8000-0000000000",
                            "ABCDEF00-0000-5000-8000-000000000000",  // not canonical (upper case)
                            "0000000g-0000-5000-8000-000000000000"};
  for (const char* b : bad_libs) CHECK(env.resolver.resolve(b, kInstrumentA).status == ResolveStatus::kInvalidId);
  const char* bad_keys[] = {"", "../x", "a/../../x", "/abs/path.sfz", "C:\\x.sfz", ".", "a\nb.sfz"};
  for (const char* k : bad_keys) CHECK(env.resolver.resolve(good, k).status == ResolveStatus::kInvalidId);
  CHECK(env.counting->counts().total() == 0);
}

void test_corrupt() {
  TmpDir tmp_;
  const std::string root = tmp_.path;
  gen(root, 6);
  Env env(root);

  // Truncated file.
  {
    const std::string f = bundle_dir(root, 0) + "/" + kBundleFileName;
    auto b = slurp(f);
    b.resize(b.size() - 10);
    spit(f, b);
    auto r = env.resolver.resolve(library_id_for_index(0), kInstrumentA);
    CHECK(r.status == ResolveStatus::kBundleError && r.bundle_error == velora::format::Error::kTruncated);
  }
  // Corrupt manifest (byte inside the manifest region).
  {
    const std::string f = bundle_dir(root, 1) + "/" + kBundleFileName;
    auto b = slurp(f);
    b[128 + 20] ^= 0x40;
    spit(f, b);
    auto r = env.resolver.resolve(library_id_for_index(1), kInstrumentA);
    CHECK(r.status == ResolveStatus::kBundleError && r.bundle_error == velora::format::Error::kCorruptManifest);
  }
  // Corrupt index: manifest size is in the header at offset 16; index follows the manifest.
  {
    const std::string f = bundle_dir(root, 2) + "/" + kBundleFileName;
    auto b = slurp(f);
    std::uint64_t msize = 0;
    for (int i = 7; i >= 0; --i) msize = (msize << 8) | b[16 + i];
    b[128 + msize + 3] ^= 0x01;
    spit(f, b);
    auto r = env.resolver.resolve(library_id_for_index(2), kInstrumentA);
    CHECK(r.status == ResolveStatus::kBundleError && r.bundle_error == velora::format::Error::kCorruptIndex);
  }
  // Not a bundle / empty file / missing file.
  {
    const std::string f = bundle_dir(root, 3) + "/" + kBundleFileName;
    spit(f, std::vector<std::uint8_t>(300, 0x55));
    auto r = env.resolver.resolve(library_id_for_index(3), kInstrumentA);
    CHECK(r.status == ResolveStatus::kBundleError && r.bundle_error == velora::format::Error::kBadMagic);
    spit(f, {});
    r = env.resolver.resolve(library_id_for_index(3), kInstrumentA);
    CHECK(r.status == ResolveStatus::kBundleError && r.bundle_error == velora::format::Error::kTruncated);
    fs::remove(f);
    r = env.resolver.resolve(library_id_for_index(3), kInstrumentA);
    CHECK(r.status == ResolveStatus::kBundleError && r.bundle_error == velora::format::Error::kIo);
  }
  // A healthy neighbour is unaffected.
  CHECK(env.resolver.resolve(library_id_for_index(4), kInstrumentA).ok());
}

void test_bad_links() {
  TmpDir tmp_;
  const std::string root = tmp_.path;
  gen(root, 4);
  Env env(root);
  auto relink = [&](std::uint32_t idx, const std::string& target) {
    const std::string link = root + "/lib/" + library_id_for_index(idx) + "/current";
    fs::remove(link);
    fs::create_symlink(target, link);
  };
  const std::string hex1 = fs::path(bundle_dir(root, 1)).filename().string();
  const std::string id0 = library_id_for_index(0);

  relink(0, "../" + library_id_for_index(1) + "/" + hex1);  // points outside lib/<id>/
  CHECK(env.resolver.resolve(id0, kInstrumentA).status == ResolveStatus::kInvalidLink);
  relink(0, bundle_dir(root, 1));                           // absolute
  CHECK(env.resolver.resolve(id0, kInstrumentA).status == ResolveStatus::kInvalidLink);
  relink(0, "current");                                     // loop onto itself
  CHECK(env.resolver.resolve(id0, kInstrumentA).status == ResolveStatus::kInvalidLink);
  relink(0, "..");
  CHECK(env.resolver.resolve(id0, kInstrumentA).status == ResolveStatus::kInvalidLink);
  relink(0, hex1 + "/..");
  CHECK(env.resolver.resolve(id0, kInstrumentA).status == ResolveStatus::kInvalidLink);

  // current is a real directory / regular file, not a symlink.
  {
    const std::string link = root + "/lib/" + library_id_for_index(2) + "/current";
    fs::remove(link);
    fs::create_directory(link);
    CHECK(env.resolver.resolve(library_id_for_index(2), kInstrumentA).status == ResolveStatus::kInvalidLink);
    fs::remove(link);
    spit(link, {1, 2, 3});
    CHECK(env.resolver.resolve(library_id_for_index(2), kInstrumentA).status == ResolveStatus::kInvalidLink);
  }
  // The <bundle_id> entry is itself a symlink (to another library's bundle dir): rejected.
  {
    const std::string id3 = library_id_for_index(3);
    const std::string hex3 = fs::path(bundle_dir(root, 3)).filename().string();
    fs::remove_all(root + "/lib/" + id3 + "/" + hex3);
    fs::create_directory_symlink(bundle_dir(root, 1), root + "/lib/" + id3 + "/" + hex3);
    CHECK(env.resolver.resolve(id3, kInstrumentA).status == ResolveStatus::kInvalidLink);
  }
  // A bundle copied under another library's path (and bundle_id name) is an identity mismatch.
  {
    const std::string id0b = library_id_for_index(0);
    fs::create_directory(root + "/lib/" + id0b + "/" + hex1);
    fs::copy_file(bundle_dir(root, 1) + "/" + kBundleFileName, root + "/lib/" + id0b + "/" + hex1 + "/" + kBundleFileName);
    relink(0, hex1);
    CHECK(env.resolver.resolve(id0b, kInstrumentA).status == ResolveStatus::kIdentityMismatch);
    relink(0, hex1 + "/");  // trailing slash form is accepted syntactically; still the mismatch
    CHECK(env.resolver.resolve(id0b, kInstrumentA).status == ResolveStatus::kIdentityMismatch);
  }
}

void test_non_regular_bundle_file() {
  TmpDir tmp_;
  const std::string root = tmp_.path;
  gen(root, 4);
  auto leak = std::make_shared<LeakTracker>(make_posix_fs());
  LibraryResolver resolver(leak, root);
  auto expect_bundle_error = [&](std::uint32_t idx) {
    auto r = resolver.resolve(library_id_for_index(idx), kInstrumentA);
    CHECK(r.status == ResolveStatus::kBundleError && !r.bundle);
    CHECK(leak->opened == leak->closed);
  };
  // (a) symlink to a valid bundle
  {
    const std::string f = bundle_dir(root, 0) + "/" + kBundleFileName;
    const std::string other = bundle_dir(root, 1) + "/" + kBundleFileName;
    fs::remove(f);
    fs::create_symlink(other, f);
    expect_bundle_error(0);
  }
  // (b) directory
  {
    const std::string f = bundle_dir(root, 1) + "/" + kBundleFileName;
    fs::remove(f);
    fs::create_directory(f);
    expect_bundle_error(1);
  }
  // (c) FIFO: must not hang
  {
    const std::string f = bundle_dir(root, 2) + "/" + kBundleFileName;
    fs::remove(f);
    CHECK(::mkfifo(f.c_str(), 0644) == 0);
    expect_bundle_error(2);
  }
}

void test_fault_injection() {
  TmpDir tmp_;
  const std::string root = tmp_.path;
  gen(root, 2);
  const std::string id = library_id_for_index(1);
  struct Spec { bool lstat_io, readlink_io; int pread_fail; };
  const Spec specs[] = {{true, false, 0}, {false, true, 0}, {false, false, 1}, {false, false, 2}, {false, false, 3}};
  for (const Spec& sp : specs) {
    auto leak = std::make_shared<LeakTracker>(make_posix_fs());
    LibraryResolver resolver(
        std::make_shared<FaultFs>(leak, sp.lstat_io, sp.readlink_io, sp.pread_fail), root);
    auto r = resolver.resolve(id, kInstrumentA);
    CHECK(r.status == ResolveStatus::kBundleError && r.bundle_error != velora::format::Error::kNone);
    CHECK(r.status != ResolveStatus::kMissingLibrary && !r.bundle);
    CHECK(leak->opened == leak->closed);
  }
}

}  // namespace

int main() {
  test_non_regular_bundle_file();
  test_fault_injection();
  test_hit_and_ops();
  test_missing_and_hostile();
  test_corrupt();
  test_bad_links();
  test_scale();
  if (g_fail) {
    std::printf("library_resolver_test: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("library_resolver_test: ok\n");
  return 0;
}
