// Story 6.4: install of protected bundles. VERIFY ONLY: this file (and everything it links) can check a signature and a SHA-256, never run a cipher.
#include "velora/librarian/install.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <new>
#include <set>
#include <sstream>

#include "velora/format/hash.hpp"
#include "velora/format/protected_format.hpp"
#include "velora/host/library_resolver.hpp"
#include "velora/librarian/library_index.hpp"
#include "velora/librarian/preflight.hpp"

namespace velora::librarian {
namespace fs = std::filesystem;
namespace fmt = velora::format;

const char* to_string(InstallReason r) {
  switch (r) {
    case InstallReason::kNone: return "none";
    case InstallReason::kSignatureInvalid: return "signature_invalid";
    case InstallReason::kUntrustedKey: return "untrusted_key";
    case InstallReason::kBlockHashMismatch: return "block_hash_mismatch";
    case InstallReason::kHashListMismatch: return "hash_list_mismatch";
    case InstallReason::kCorruptBundle: return "corrupt_bundle";
    case InstallReason::kWrappedKeyInvalid: return "wrapped_key_invalid";
    case InstallReason::kAlreadyInstalled: return "already_installed";
    case InstallReason::kLibraryIdMismatch: return "library_id_mismatch";
    case InstallReason::kNotProtected: return "not_protected";
    case InstallReason::kUnsupportedVersion: return "unsupported_version";
    case InstallReason::kDowngrade: return "downgrade";
    case InstallReason::kCatalogMetaInvalid: return "catalog_meta_invalid";
    case InstallReason::kLibraryIdInUse: return "library_id_in_use";
    case InstallReason::kMissingFile: return "missing_file";
    case InstallReason::kIoError: return "io_error";
  }
  return "?";
}

const char* to_string(InstallStatus s) {
  switch (s) {
    case InstallStatus::kInstalled: return "installed";
    case InstallStatus::kUnchanged: return "unchanged";
    case InstallStatus::kRejected: return "rejected";
    case InstallStatus::kInvalidArgument: return "invalid argument";
    case InstallStatus::kInsufficientSpace: return "insufficient space";
    case InstallStatus::kIo: return "I/O error";
    case InstallStatus::kBusy: return "another import or install is running";
    case InstallStatus::kAborted: return "aborted";
    case InstallStatus::kDriveRemoved: return "drive removed";
  }
  return "?";
}

std::uint64_t required_install_bytes(std::uint64_t pack_bytes) {
  return pack_bytes > (~0ull - kImportReserveBytes) ? ~0ull : pack_bytes + kImportReserveBytes;
}

std::string wrapped_key_problem(const std::uint8_t* d, std::size_t n) {
  if (n != kWrappedKeyBlobSize) return "a wrapped_key_v1 blob is " + std::to_string(kWrappedKeyBlobSize) + " bytes, this one is " + std::to_string(n);
  if (std::memcmp(d, "VWK1", 4) != 0) return "the magic is not VWK1";
  if (d[4] != 0x01) return "unknown wrapped_key algorithm " + std::to_string(d[4]);
  bool zero = true;
  for (std::size_t i = 13; i < 45; ++i) zero = zero && d[i] == 0;  // ephemeral X25519 public key
  if (zero) return "the ephemeral public key is all zero";
  return {};
}

bool load_trust_list(const std::string& path, fmt::TrustedKeys& out, std::string& err) {
  std::ifstream in(path);
  if (!in) {
    err = path + ": cannot be read";
    return false;
  }
  auto unhex = [](const std::string& h, std::uint8_t* dst, std::size_t n) {
    if (h.size() != 2 * n) return false;
    for (std::size_t i = 0; i < n; ++i) {
      int v = 0;
      for (int k = 0; k < 2; ++k) {
        const char c = h[2 * i + k];
        const int x = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (x < 0) return false;
        v = v * 16 + x;
      }
      dst[i] = static_cast<std::uint8_t>(v);
    }
    return true;
  };
  std::vector<fmt::TrustedKey> keys;
  std::set<std::array<std::uint8_t, 8>> ids;
  std::string line;
  int ln = 0;
  while (std::getline(in, line)) {
    ++ln;
    const std::size_t hash = line.find('#');
    if (hash != std::string::npos) line.erase(hash);
    std::istringstream ss(line);
    std::vector<std::string> f;
    for (std::string t; ss >> t;) f.push_back(t);
    if (f.empty()) continue;
    auto bad = [&](const char* why) {
      err = path + ":" + std::to_string(ln) + ": " + why;
      return false;
    };
    if (f.size() != 4) return bad("expected `<root> <role> <key_id> <public_key>`");
    if (f[1] != "content-manifest") return bad("unknown role (only content-manifest)");
    fmt::TrustedKey k;
    if (!unhex(f[2], k.id.data(), k.id.size()) || !unhex(f[3], k.pub.data(), k.pub.size())) return bad("key id or public key is not hex of the right length");
    if (fmt::key_id_of_public(k.pub) != k.id) return bad("the key id is not the first 8 bytes of SHA-256(public key)");
    if (!ids.insert(k.id).second) return bad("the key id is listed twice");
    keys.push_back(k);
  }
  out = fmt::TrustedKeys(std::move(keys));
  return true;
}

namespace {

struct Fd {
  int fd = -1;
  Fd() = default;
  explicit Fd(int f) : fd(f) {}
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  ~Fd() {
    if (fd >= 0) ::close(fd);
  }
  bool ok() const { return fd >= 0; }
};

bool full_sync(int fd) {
#ifdef __APPLE__
  if (::fcntl(fd, F_FULLFSYNC) == 0) return true;
#endif
  return ::fsync(fd) == 0;
}
bool fsync_path(const std::string& path, bool dir) {
  Fd f(::open(path.c_str(), dir ? (O_RDONLY | O_DIRECTORY) : O_RDONLY));
  return f.ok() && full_sync(f.fd);
}
bool mkdir_synced(const std::string& path, const std::string& parent) {
  if (::mkdir(path.c_str(), 0755) == 0) return fsync_path(parent, true);
  return errno == EEXIST;
}
std::string err_text(const std::string& what) { return what + ": " + std::strerror(errno); }
bool is_dir(const std::string& p) {
  struct stat st;
  return ::lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool exists(const std::string& p) {
  struct stat st;
  return ::lstat(p.c_str(), &st) == 0;
}
bool same_device(const std::string& a, const std::string& b) {
  struct stat sa, sb;
  return ::stat(a.c_str(), &sa) == 0 && ::stat(b.c_str(), &sb) == 0 && sa.st_dev == sb.st_dev;
}
bool io_errno(int e) { return e == EIO || e == ENXIO || e == ENODEV || e == ESTALE || e == ENOTCONN; }

std::atomic<unsigned> g_counter{0};

// A plain POSIX file as a ByteSource (the default of InstallOptions::open_source): size from fstat, reads by pread, never mapped, never loaded whole.
class PosixSource : public fmt::ByteSource {
 public:
  explicit PosixSource(int fd, std::uint64_t size) : fd_(fd), size_(size) {}
  ~PosixSource() override {
    if (fd_ >= 0) ::close(fd_);
  }
  std::uint64_t size() const override { return size_; }
  const std::uint8_t* direct(std::uint64_t, std::size_t) const override { return nullptr; }
  fmt::Error read_at(std::uint64_t off, void* dst, std::size_t n) const override {
    if (off > size_ || n > size_ - off) return fmt::Error::kTruncated;
    auto* p = static_cast<std::uint8_t*>(dst);
    while (n > 0) {
      const ssize_t r = ::pread(fd_, p, n, static_cast<off_t>(off));
      if (r < 0 && errno == EINTR) continue;
      if (r <= 0) return r == 0 ? fmt::Error::kTruncated : fmt::Error::kIo;
      p += r;
      off += static_cast<std::uint64_t>(r);
      n -= static_cast<std::size_t>(r);
    }
    return fmt::Error::kNone;
  }

 private:
  int fd_;
  std::uint64_t size_;
};

// Opens `path` as a ByteSource; null with errno set on failure.
std::unique_ptr<fmt::ByteSource> open_posix(const std::string& path) {
  // O_NONBLOCK: opening a FIFO (or a device) for reading must not wait for a writer; only a regular file is accepted, and it is then made blocking again.
  const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
  if (fd < 0) return nullptr;
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    const int e = errno;
    ::close(fd);
    errno = e;
    return nullptr;
  }
  if (!S_ISREG(st.st_mode)) {
    ::close(fd);
    errno = EINVAL;  // not a regular file (a FIFO, a device, a directory)
    return nullptr;
  }
  const int fl = ::fcntl(fd, F_GETFL);
  if (fl >= 0) ::fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
  return std::make_unique<PosixSource>(fd, static_cast<std::uint64_t>(st.st_size));
}

bool read_local(const std::string& path, std::size_t max, std::vector<std::uint8_t>& out) {
  auto s = open_posix(path);
  if (!s || s->size() > max) return false;
  out.resize(static_cast<std::size_t>(s->size()));
  return out.empty() || s->read_at(0, out.data(), out.size()) == fmt::Error::kNone;
}

InstallReason reason_of(fmt::Error e) {
  switch (e) {
    case fmt::Error::kSignatureInvalid: return InstallReason::kSignatureInvalid;
    case fmt::Error::kUntrustedKey: return InstallReason::kUntrustedKey;
    case fmt::Error::kBlockHashMismatch: return InstallReason::kBlockHashMismatch;
    case fmt::Error::kHashListMismatch: return InstallReason::kHashListMismatch;
    case fmt::Error::kNotProtected: return InstallReason::kNotProtected;
    case fmt::Error::kUnsupportedVersion: return InstallReason::kUnsupportedVersion;
    case fmt::Error::kCatalogMetaInvalid: return InstallReason::kCatalogMetaInvalid;
    case fmt::Error::kIo: return InstallReason::kIoError;
    default: return InstallReason::kCorruptBundle;  // bad magic, truncation, corrupt header / manifest / index / block, out of range
  }
}

struct Paths {
  std::string bundle, wrapped_key, meta, seal;
};

std::string basename_of(const std::string& p) { return fs::path(p).filename().string(); }

}  // namespace

static InstallResult install_protected_impl(const InstallOptions& o);

InstallResult install_protected(const InstallOptions& o) {
  // No exception leaves the install (the worker thread would terminate): an allocation failure or a library exception is an I/O failure with nothing published.
  try {
    return install_protected_impl(o);
  } catch (const std::bad_alloc&) {
    InstallResult r;
    r.status = InstallStatus::kIo;
    r.reason = InstallReason::kIoError;
    r.detail = "out of memory";
    return r;
  } catch (const std::exception& e) {
    InstallResult r;
    r.status = InstallStatus::kIo;
    r.reason = InstallReason::kIoError;
    r.detail = std::string("unexpected failure: ") + e.what();
    return r;
  }
}

static InstallResult install_protected_impl(const InstallOptions& o) {
  InstallResult res;
  auto set = [&](InstallStatus s, InstallReason r, std::string why) -> InstallResult& {
    res.status = s;
    res.reason = r;
    res.detail = std::move(why);
    return res;
  };
  auto reject = [&](InstallReason r, std::string why) -> InstallResult& { return set(InstallStatus::kRejected, r, std::move(why)); };
  auto fail_io = [&](std::string why) -> InstallResult& { return set(InstallStatus::kIo, InstallReason::kIoError, std::move(why)); };

  if (o.root.empty() || !o.trusted) return set(InstallStatus::kInvalidArgument, InstallReason::kNone, "an install needs a data root and a trusted-key list");
  {
    struct stat st;
    if (::stat(o.root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
      return set(InstallStatus::kInvalidArgument, InstallReason::kNone, "the data root " + o.root + " is not an existing directory");
  }
  fmt::Uuid expected{};
  bool have_expected = false;
  if (!o.expected_library_id.empty()) {
    if (!fmt::uuid_from_string(o.expected_library_id, expected))
      return set(InstallStatus::kInvalidArgument, InstallReason::kNone, "expected library_id '" + o.expected_library_id + "' is not a canonical UUID");
    have_expected = true;
  } else if (!o.source_dir.empty()) {
    // A content-pack directory is named after its library_id.
    std::string name = fs::path(o.source_dir).lexically_normal().filename().string();
    if (name.empty()) name = fs::path(o.source_dir).lexically_normal().parent_path().filename().string();
    have_expected = fmt::uuid_from_string(name, expected);
  }

  // --- the files of the pack ---------------------------------------------------------------------------------------------------------------------
  Paths p;
  std::string wrapped_beside;  // where `wrapped-key.*.bin` is looked for when no wrapped key is named
  {
    const std::string dir = o.source_dir;
    p.bundle = !o.bundle_path.empty() ? o.bundle_path : dir.empty() ? std::string() : dir + "/" + kProtectedBundleFileName;
    if (p.bundle.empty()) return set(InstallStatus::kInvalidArgument, InstallReason::kNone, "an install needs a pack directory or a bundle path");
    std::string beside = !dir.empty() ? dir : fs::path(p.bundle).parent_path().string();
    if (beside.empty()) beside = ".";
    p.meta = !o.catalog_meta_path.empty() ? o.catalog_meta_path : beside + "/" + kCatalogMetaFileName;
    p.seal = !o.catalog_meta_seal_path.empty() ? o.catalog_meta_seal_path : beside + "/" + fmt::kCatalogMetaSealFileName;
    p.wrapped_key = o.wrapped_key_path;
    wrapped_beside = beside;
  }
  auto open_src = [&](const std::string& path) -> std::unique_ptr<fmt::ByteSource> { return o.open_source ? o.open_source(path) : open_posix(path); };
  auto gone = [&](const std::string& path) {
    if (o.source_gone && o.source_gone()) return true;
    struct stat st;
    return ::stat(path.c_str(), &st) != 0 && (errno == ENOENT || errno == ENOTDIR || io_errno(errno));
  };

  // 1. the bundle: header, manifest and index hashes, trusted signer, signature, hash-list root.
  std::unique_ptr<fmt::ByteSource> src = open_src(p.bundle);
  if (!src) {
    if (errno == ENOENT || errno == ENOTDIR) return reject(InstallReason::kMissingFile, p.bundle + ": no such file");
    if (io_errno(errno)) return set(InstallStatus::kDriveRemoved, InstallReason::kNone, err_text("cannot open " + p.bundle));
    return fail_io(err_text("cannot open " + p.bundle));
  }
  res.bundle_bytes = src->size();
  const std::uint64_t source_size = src->size();
  auto opened = fmt::ProtectedBundleView::open_source(std::move(src), *o.trusted);
  if (!opened) {
    if (opened.error() == fmt::Error::kIo && gone(p.bundle)) return set(InstallStatus::kDriveRemoved, InstallReason::kNone, "the source was removed while it was read");
    return reject(reason_of(opened.error()), std::string("the bundle does not open: ") + fmt::to_string(opened.error()));
  }
  fmt::ProtectedBundleView view = opened.take();
  const fmt::ProtectedInfo info = view.info();
  res.library_id = fmt::uuid_to_string(info.library_id);
  res.bundle_id = fmt::to_hex(info.bundle_id);
  res.instruments = static_cast<std::uint32_t>(view.instruments().size());
  res.samples = static_cast<std::uint32_t>(view.samples().size());
  for (const auto& ins : view.instruments()) res.regions += static_cast<std::uint32_t>(ins.regions.size());
  res.blocks = view.total_blocks();

  // 2. the library id.
  if (info.library_id == fmt::Uuid{}) return reject(InstallReason::kCorruptBundle, "the bundle carries the nil library_id");
  if (have_expected && expected != info.library_id)
    return reject(InstallReason::kLibraryIdMismatch, "the bundle is library " + res.library_id + ", not " + fmt::uuid_to_string(expected));

  // 3. the wrapped-key blob, by structure only.
  if (p.wrapped_key.empty()) {
    std::vector<std::string> found;
    std::error_code ec;
    for (fs::directory_iterator it(wrapped_beside, ec), end; !ec && it != end; it.increment(ec)) {
      const std::string n = it->path().filename().string();
      struct stat st;
      if (n.rfind("wrapped-key.", 0) == 0 && n.size() > 16 && n.compare(n.size() - 4, 4, ".bin") == 0 && ::stat(it->path().c_str(), &st) == 0 && S_ISREG(st.st_mode))
        found.push_back(it->path().string());
    }
    if (found.empty()) return reject(InstallReason::kWrappedKeyInvalid, "missing: no wrapped-key.*.bin next to the bundle (name one explicitly)");
    if (found.size() > 1)
      return reject(InstallReason::kWrappedKeyInvalid, "ambiguous: " + std::to_string(found.size()) + " wrapped-key.*.bin files; name the one for this device");
    p.wrapped_key = found[0];
  }
  std::vector<std::uint8_t> wrapped, meta_bytes, seal_bytes;
  auto read_small = [&](const std::string& path, std::size_t max, std::vector<std::uint8_t>& out, InstallReason missing_reason, const char* what) -> bool {
    auto s = open_src(path);
    if (!s) {
      if (errno == ENOENT || errno == ENOTDIR) reject(missing_reason, std::string(what) + ": " + path + " does not exist");
      else if (io_errno(errno)) set(InstallStatus::kDriveRemoved, InstallReason::kNone, err_text(std::string("cannot open ") + path));
      else fail_io(err_text(std::string("cannot open ") + path));
      return false;
    }
    if (s->size() > max) {
      reject(missing_reason, std::string(what) + ": " + path + " is larger than " + std::to_string(max) + " bytes");
      return false;
    }
    out.resize(static_cast<std::size_t>(s->size()));
    if (!out.empty()) {
      if (fmt::Error e = s->read_at(0, out.data(), out.size()); e != fmt::Error::kNone) {
        if (gone(path)) set(InstallStatus::kDriveRemoved, InstallReason::kNone, "the source was removed while it was read");
        else fail_io(std::string("cannot read ") + path);
        return false;
      }
    }
    return true;
  };
  if (!read_small(p.wrapped_key, kWrappedKeyBlobSize + 1, wrapped, InstallReason::kWrappedKeyInvalid, "wrapped key")) return res;
  if (const std::string why = wrapped_key_problem(wrapped.data(), wrapped.size()); !why.empty())
    return reject(InstallReason::kWrappedKeyInvalid, "wrapped key " + basename_of(p.wrapped_key) + ": " + why);
  {  // `wrapped-key.<recipient key id>.bin`: the id in the name is the one in the blob (bytes 5..12, KEY-HIERARCHY.md section 6)
    const std::string n = basename_of(p.wrapped_key);
    if (n.size() == 12 + 16 + 4 && n.rfind("wrapped-key.", 0) == 0 && n.compare(n.size() - 4, 4, ".bin") == 0 &&
        n.substr(12, 16).find_first_not_of("0123456789abcdef") == std::string::npos &&
        n.substr(12, 16) != fmt::to_hex(std::span<const std::uint8_t>(wrapped.data() + 5, 8)))
      return reject(InstallReason::kWrappedKeyInvalid, "wrapped key " + n + ": the recipient key id in the name is not the one in the blob");
  }

  // 4. catalog.meta, sealed for this bundle.
  if (!read_small(p.meta, kMaxCatalogMetaBytes, meta_bytes, InstallReason::kCatalogMetaInvalid, "catalog.meta")) return res;
  if (!read_small(p.seal, fmt::kCatalogMetaSealSize + 1, seal_bytes, InstallReason::kCatalogMetaInvalid, "catalog.meta.sig")) return res;
  {
    fmt::KeyId seal_signer{};
    fmt::Signature unused{};
    std::uint32_t rev = 0;
    if (!fmt::decode_catalog_meta_seal(seal_bytes, seal_signer, rev, unused)) return reject(InstallReason::kCatalogMetaInvalid, "catalog.meta.sig is malformed");
    if (seal_signer != info.signer_key_id) return reject(InstallReason::kCatalogMetaInvalid, "catalog.meta.sig is signed by another key than the bundle");
    const fmt::Error e = fmt::verify_catalog_meta_seal(*o.trusted, info.bundle_id, meta_bytes, seal_bytes);
    res.pack_revision = rev;
    if (e == fmt::Error::kUntrustedKey) return reject(InstallReason::kUntrustedKey, "catalog.meta.sig: the signer is not trusted");
    if (e != fmt::Error::kNone) return reject(InstallReason::kCatalogMetaInvalid, "catalog.meta does not match its seal (altered, or sealed for another bundle)");
    CatalogMeta meta;
    if (!parse_catalog_meta(std::string_view(reinterpret_cast<const char*>(meta_bytes.data()), meta_bytes.size()), meta))
      return reject(InstallReason::kCatalogMetaInvalid, "catalog.meta is not valid (format, record or category)");
    std::set<std::string> manifest_keys, meta_keys;
    for (const auto& ins : view.instruments()) manifest_keys.insert(ins.key);
    for (const auto& mi : meta.instruments) meta_keys.insert(mi.key);
    if (manifest_keys != meta_keys) return reject(InstallReason::kCatalogMetaInvalid, "catalog.meta does not name exactly the instruments of the bundle");
  }

  // --- the data root: lock, then everything that depends on what is installed -------------------------------------------------------------------------
  const std::string lib_root = (fs::path(o.root) / host::kLibDirName).string();
  const std::string staging_root = lib_root + "/" + kStagingDirName;
  if (!mkdir_synced(lib_root, o.root) || !mkdir_synced(staging_root, lib_root) || !is_dir(staging_root)) return fail_io(err_text("cannot create " + staging_root));
  Fd lock(::open((staging_root + "/" + kLockFileName).c_str(), O_RDWR | O_CREAT, 0644));
  if (!lock.ok()) return fail_io(err_text("cannot open lock file"));
  if (::flock(lock.fd, LOCK_EX | LOCK_NB) != 0)
    return errno == EWOULDBLOCK ? set(InstallStatus::kBusy, InstallReason::kNone, "another import or install holds " + staging_root) : fail_io(err_text("cannot lock " + staging_root));
  if (o.fault.pretend_cross_device || !same_device(staging_root, lib_root)) return fail_io("the staging directory " + staging_root + " is not on the destination filesystem");
  {
    const int rc = reconcile_staging(o.root);
    if (rc < 0) return fail_io("cannot clean up leftovers in " + staging_root);
    res.reconciled = static_cast<std::size_t>(rc);
  }
  const std::string lib_dir = lib_root + "/" + res.library_id;
  res.bundle_dir = lib_dir + "/" + res.bundle_id;

  auto verify_blocks = [&](const fmt::ProtectedBundleView& v, bool& aborted) -> fmt::Error {
    std::vector<std::uint8_t> buf;
    aborted = false;
    for (std::uint64_t b = 0; b < v.total_blocks(); ++b) {
      if (o.cancelled && o.cancelled()) {
        aborted = true;
        return fmt::Error::kNone;
      }
      if (fmt::Error e = v.read_ciphertext(b, buf); e != fmt::Error::kNone) return e;
      res.peak_buffer_bytes = std::max<std::uint64_t>(res.peak_buffer_bytes, buf.size());
    }
    return fmt::Error::kNone;
  };

  // 5. what is installed already: a user library under this id, a newer bundle or revision, this very bundle.
  {
    std::error_code ec;
    for (fs::directory_iterator it(lib_dir, ec), end; !ec && it != end; it.increment(ec)) {  // stale `.current.tmp.*` of a crashed flip (safe under the lock)
      if (it->path().filename().string().rfind(".current.tmp", 0) == 0) {
        std::error_code rm;
        fs::remove(it->path(), rm);
        ++res.reconciled;
      }
    }
  }
  std::string current_id;  // the bundle `current` names, "" when there is none
  {
    char cur[PATH_MAX];
    const ssize_t n = ::readlink((lib_dir + "/" + host::kCurrentLinkName).c_str(), cur, sizeof cur - 1);
    if (n > 0) current_id.assign(cur, static_cast<std::size_t>(n));
  }
  std::string note;  // told in the result's detail of a success
  if (!current_id.empty() && current_id.find('/') == std::string::npos) {
    const std::string cur_dir = lib_dir + "/" + current_id;
    if (exists(cur_dir + "/" + host::kBundleFileName) && !exists(cur_dir + "/" + kProtectedBundleFileName))
      return reject(InstallReason::kLibraryIdInUse, "library " + res.library_id + " is a user library; a factory bundle cannot replace it");
    if (exists(cur_dir + "/" + kProtectedBundleFileName)) {
      // The current bundle's own signed revision decides whether this pack is a replay. A current bundle that cannot be examined (damaged, signed by a key
      // that is no longer trusted) makes the history unknown: the install proceeds and says so.
      auto have = fmt::ProtectedBundleView::open_file(cur_dir + "/" + kProtectedBundleFileName, *o.trusted);
      std::vector<std::uint8_t> cm, cs;
      std::uint32_t cur_rev = 0;
      if (have && read_local(cur_dir + "/" + kCatalogMetaFileName, kMaxCatalogMetaBytes, cm) && read_local(cur_dir + "/" + fmt::kCatalogMetaSealFileName, fmt::kCatalogMetaSealSize, cs) &&
          fmt::verify_catalog_meta_seal(*o.trusted, have.value().info().bundle_id, cm, cs, nullptr, &cur_rev) == fmt::Error::kNone) {
        if (res.pack_revision < cur_rev)
          return reject(InstallReason::kDowngrade, "pack revision " + std::to_string(res.pack_revision) + " is older than the installed revision " + std::to_string(cur_rev));
        if (res.pack_revision == cur_rev && have.value().info().bundle_id != info.bundle_id)
          return reject(InstallReason::kDowngrade, "revision " + std::to_string(cur_rev) + " is installed with another bundle; a new bundle needs a higher revision");
        if (have.value().info().format_minor > info.format_minor)
          return reject(InstallReason::kDowngrade, "the installed bundle is format 3." + std::to_string(have.value().info().format_minor) + ", this one is 3." + std::to_string(info.format_minor));
      } else {
        note = "the current bundle could not be examined, its revision is unknown: the replay check was skipped";
      }
    }
  }
  const std::string link = lib_dir + "/" + host::kCurrentLinkName;
  // Flips `current` (symlink to a temporary name, then rename over it) and makes the flip durable. Once the rename is done the flip HAS happened, whatever the
  // fsync says.
  auto flip_current = [&]() -> bool {
    const std::string tmp = lib_dir + "/.current.tmp." + std::to_string(::getpid());
    ::unlink(tmp.c_str());
    if (::symlink(res.bundle_id.c_str(), tmp.c_str()) != 0) {
      fail_io(err_text("symlink"));
      return false;
    }
    if (o.fault.on_stage && o.fault.on_stage(Stage::kLinkCreated) == HookAction::kAbort) {
      res.status = InstallStatus::kAborted;
      res.detail = "stopped at link-created";
      return false;
    }
    if (::rename(tmp.c_str(), link.c_str()) != 0) {
      const std::string why = err_text("rename current");
      ::unlink(tmp.c_str());
      fail_io(why);
      return false;
    }
    res.current_flipped = true;
    if (!fsync_path(lib_dir, true)) {
      fail_io(err_text("fsync " + lib_dir));
      return false;
    }
    return true;
  };
  // After the flip is durable: the bundle directories of this library that are protected and neither the new one nor the one that was current before are removed
  // (an open descriptor on them - a lease - survives the unlink). At most the new and the previous bundle stay.
  auto prune_old = [&](const std::string& previous) {
    std::error_code ec;
    std::vector<fs::path> doomed;
    for (fs::directory_iterator it(lib_dir, ec), end; !ec && it != end; it.increment(ec)) {
      const std::string n = it->path().filename().string();
      if (n.size() != 64 || n == res.bundle_id || n == previous) continue;
      if (n.find_first_not_of("0123456789abcdef") != std::string::npos) continue;
      if (exists(it->path().string() + "/" + kProtectedBundleFileName) && !exists(it->path().string() + "/" + host::kBundleFileName)) doomed.push_back(it->path());
    }
    for (const fs::path& d : doomed) {
      std::error_code rm;
      fs::remove_all(d, rm);
      if (!rm) ++res.pruned;
    }
    if (res.pruned) fsync_path(lib_dir, true);
  };
  std::string damaged_aside;
  if (is_dir(res.bundle_dir)) {
    // The same bundle (its id is the hash of its manifest) is in place already - an earlier install, or one that was cut short between the rename and the
    // flip. If it is intact, `current` is brought to it when it is not there yet; a different wrapped-key blob (the vendor re-delivered the pack for this
    // device) replaces the stored one atomically. If it is damaged it is moved out of the way (into the staging area, which the next run empties) and the normal
    // publish follows.
    auto existing = fmt::ProtectedBundleView::open_file(res.bundle_dir + "/" + kProtectedBundleFileName, *o.trusted);
    bool aborted = false;
    const bool intact = existing && existing.value().info().bundle_id == info.bundle_id && verify_blocks(existing.value(), aborted) == fmt::Error::kNone;
    if (aborted) return set(InstallStatus::kAborted, InstallReason::kNone, "cancelled");
    if (intact) {
      std::vector<std::uint8_t> have_wrapped;
      if (!read_local(res.bundle_dir + "/" + kWrappedKeyFileName, kWrappedKeyBlobSize + 1, have_wrapped) || have_wrapped != wrapped) {
        const std::string tmp = res.bundle_dir + "/" + kWrappedKeyFileName + ".tmp", dst = res.bundle_dir + "/" + kWrappedKeyFileName;
        Fd f(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
        std::size_t done = 0;
        while (f.ok() && done < wrapped.size()) {
          const ssize_t w = ::write(f.fd, wrapped.data() + done, wrapped.size() - done);
          if (w < 0 && errno == EINTR) continue;
          if (w <= 0) break;
          done += static_cast<std::size_t>(w);
        }
        if (!f.ok() || done != wrapped.size() || !full_sync(f.fd) || ::rename(tmp.c_str(), dst.c_str()) != 0 || !fsync_path(res.bundle_dir, true)) {
          ::unlink(tmp.c_str());
          return fail_io(err_text("cannot replace " + dst));
        }
        res.wrapped_key_updated = true;
      }
      if (current_id == res.bundle_id) {
        set(InstallStatus::kUnchanged, InstallReason::kAlreadyInstalled, res.wrapped_key_updated ? "this bundle is already installed; its wrapped key was replaced" : "this bundle is already installed");
        return res;
      }
      if (!flip_current()) return res;
      prune_old(current_id);
      if (o.fault.on_stage && o.fault.on_stage(Stage::kFlipped) == HookAction::kAbort) return set(InstallStatus::kAborted, InstallReason::kNone, "stopped at link-flipped");
      res.status = InstallStatus::kInstalled;
      res.reason = InstallReason::kNone;
      res.detail = note;
      return res;
    }
    damaged_aside = staging_root + "/damaged-" + std::to_string(::getpid()) + "-" + std::to_string(g_counter.fetch_add(1));
    if (::rename(res.bundle_dir.c_str(), damaged_aside.c_str()) != 0) return fail_io(err_text(res.bundle_dir + " is damaged and cannot be moved aside"));
    fsync_path(lib_dir, true);
    note = (note.empty() ? "" : note + "; ") + "a damaged copy of this bundle was replaced";
  }

  // 6. space.
  {
    const std::uint64_t pack_bytes = source_size + meta_bytes.size() + seal_bytes.size() + wrapped.size();
    res.required_bytes = required_install_bytes(pack_bytes);
    std::uint64_t freeb = 0;
    bool known = false;
    if (o.free_bytes_fn) {
      known = o.free_bytes_fn(o.root, freeb);
    } else {
      struct statvfs sv{};
      known = ::statvfs(o.root.c_str(), &sv) == 0;
      if (known) freeb = static_cast<std::uint64_t>(sv.f_bavail) * static_cast<std::uint64_t>(sv.f_frsize);
    }
    res.free_bytes = known ? freeb : 0;
    if (known && res.required_bytes > freeb) return set(InstallStatus::kInsufficientSpace, InstallReason::kNone, "not enough free space for this install");
  }

  // 7. stage: the bundle copied in bounded chunks, the small files written, everything fsynced.
#define VELORA_HOOK(stage)                                                         \
  do {                                                                             \
    if (o.fault.on_stage && o.fault.on_stage(stage) == HookAction::kAbort) {       \
      res.status = InstallStatus::kAborted;                                        \
      res.detail = "stopped at " #stage;                                           \
      return res;                                                                  \
    }                                                                              \
  } while (0)
  const std::string stage_dir = staging_root + "/" + std::to_string(::getpid()) + "-" +
                                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(g_counter.fetch_add(1));
  if (::mkdir(stage_dir.c_str(), 0755) != 0) return fail_io(err_text("mkdir " + stage_dir));
  auto cleanup = [&]() {
    std::error_code e;
    fs::remove_all(stage_dir, e);
  };
  auto stop = [&](InstallStatus s, InstallReason r, std::string why) -> InstallResult& {
    cleanup();
    return set(s, r, std::move(why));
  };
  VELORA_HOOK(Stage::kStageCreated);
  const std::string staged = stage_dir + "/" + kProtectedBundleFileName;
  {
    auto in = open_src(p.bundle);
    if (!in) return stop(io_errno(errno) ? InstallStatus::kDriveRemoved : InstallStatus::kIo, InstallReason::kNone, err_text("cannot open " + p.bundle));
    if (in->size() != source_size) return stop(InstallStatus::kRejected, InstallReason::kCorruptBundle, "the bundle changed while it was installed");
    Fd out(::open(staged.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644));
    if (!out.ok()) return stop(InstallStatus::kIo, InstallReason::kIoError, err_text("cannot create " + staged));
    std::vector<std::uint8_t> chunk(kInstallChunkBytes);
    res.peak_buffer_bytes = std::max<std::uint64_t>(res.peak_buffer_bytes, chunk.size());
    std::uint64_t chunks = 0;
    for (std::uint64_t off = 0; off < source_size;) {
      if (o.cancelled && o.cancelled()) return stop(InstallStatus::kAborted, InstallReason::kNone, "cancelled");
      const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), source_size - off));
      if (fmt::Error e = in->read_at(off, chunk.data(), n); e != fmt::Error::kNone) {
        if (gone(p.bundle)) return stop(InstallStatus::kDriveRemoved, InstallReason::kNone, "the source was removed while it was read");
        return stop(InstallStatus::kIo, InstallReason::kIoError, std::string("cannot read ") + p.bundle + ": " + fmt::to_string(e));
      }
      std::size_t done = 0;
      while (done < n) {
        const ssize_t w = ::write(out.fd, chunk.data() + done, n - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return stop(InstallStatus::kIo, InstallReason::kIoError, err_text("cannot write " + staged));
        done += static_cast<std::size_t>(w);
      }
      off += n;
      if (o.on_copy_chunk) o.on_copy_chunk(++chunks);
    }
    if (!full_sync(out.fd)) return stop(InstallStatus::kIo, InstallReason::kIoError, err_text("fsync " + staged));
  }
  auto write_small = [&](const char* name, const std::vector<std::uint8_t>& bytes, mode_t mode) -> bool {
    Fd f(::open((stage_dir + "/" + name).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode));
    if (!f.ok()) return false;
    std::size_t done = 0;
    while (done < bytes.size()) {
      const ssize_t w = ::write(f.fd, bytes.data() + done, bytes.size() - done);
      if (w < 0 && errno == EINTR) continue;
      if (w <= 0) return false;
      done += static_cast<std::size_t>(w);
    }
    return ::fchmod(f.fd, mode) == 0 && full_sync(f.fd);
  };
  if (!write_small(kWrappedKeyFileName, wrapped, 0600) || !write_small(kCatalogMetaFileName, meta_bytes, 0644) || !write_small(fmt::kCatalogMetaSealFileName, seal_bytes, 0644))
    return stop(InstallStatus::kIo, InstallReason::kIoError, err_text("cannot write the pack files into " + stage_dir));
  if (!fsync_path(stage_dir, true)) return stop(InstallStatus::kIo, InstallReason::kIoError, err_text("fsync " + stage_dir));
  if (o.cancelled && o.cancelled()) return stop(InstallStatus::kAborted, InstallReason::kNone, "cancelled");
  VELORA_HOOK(Stage::kStaged);

  // 8. verify the STAGED copy: it must be the very bundle that was opened, and every ciphertext block must match the signed index.
  {
    auto sv = fmt::ProtectedBundleView::open_file(staged, *o.trusted);
    if (!sv) return stop(InstallStatus::kRejected, reason_of(sv.error()), std::string("the staged bundle does not open: ") + fmt::to_string(sv.error()));
    if (sv.value().info().bundle_id != info.bundle_id || sv.value().info().library_id != info.library_id)
      return stop(InstallStatus::kRejected, InstallReason::kCorruptBundle, "the staged bundle is not the bundle that was opened");
    bool aborted = false;
    const fmt::Error e = verify_blocks(sv.value(), aborted);
    if (aborted) return stop(InstallStatus::kAborted, InstallReason::kNone, "cancelled");
    if (e != fmt::Error::kNone) return stop(InstallStatus::kRejected, reason_of(e), std::string("a block of the staged bundle failed verification: ") + fmt::to_string(e));
  }
  VELORA_HOOK(Stage::kVerified);

  // 9. durable, then published.
  if (!fsync_path(staged, false) || !fsync_path(stage_dir, true)) return stop(InstallStatus::kIo, InstallReason::kIoError, err_text("fsync staged bundle"));
  VELORA_HOOK(Stage::kFsynced);
  if (::mkdir(lib_dir.c_str(), 0755) != 0 && errno != EEXIST) return stop(InstallStatus::kIo, InstallReason::kIoError, err_text("mkdir " + lib_dir));
  if (!is_dir(lib_dir)) return stop(InstallStatus::kIo, InstallReason::kIoError, lib_dir + " is not a directory");
  if (::rename(stage_dir.c_str(), res.bundle_dir.c_str()) != 0) {
    const bool xdev = errno == EXDEV;
    const std::string why = err_text("rename to " + res.bundle_dir);
    cleanup();
    return set(InstallStatus::kIo, InstallReason::kIoError, xdev ? "the staging directory is on another filesystem: " + why : why);
  }
  res.bundle_created = true;
  if (!fsync_path(lib_dir, true) || !fsync_path(lib_root, true)) return fail_io(err_text("fsync " + lib_dir));
  VELORA_HOOK(Stage::kRenamed);

  // 10. flip `current`, then the cleanup that needs the flip to be durable.
  if (!flip_current()) return res;
  prune_old(current_id);
  if (!damaged_aside.empty()) {
    std::error_code rm;
    fs::remove_all(damaged_aside, rm);
  }
  VELORA_HOOK(Stage::kFlipped);
  res.status = InstallStatus::kInstalled;
  res.reason = InstallReason::kNone;
  res.detail = note;
  return res;
#undef VELORA_HOOK
}

bool resolve_trusted_keys_for_variant(const char* root, bool is_production, const fmt::TrustedKeys& embedded, const std::string& override_path, fmt::TrustedKeys& out,
                                      std::string& err) {
  if (override_path.empty()) {
    out = embedded;
    return true;
  }
  if (is_production || std::string(root) != "dev") {
    err = "--trust-list is available only in the dev variant of the librarian";
    return false;
  }
  return load_trust_list(override_path, out, err);
}

}  // namespace velora::librarian
