#include "velora/host/library_resolver.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

#include "velora/format/byte_source.hpp"
#include "velora/format/hash.hpp"

namespace velora::host {
namespace {

// ByteSource backed by the FsPort: every pread is counted; blocks are never touched at open.
class PortSource final : public format::ByteSource {
 public:
  PortSource(std::shared_ptr<FsPort> fs, int fd, std::uint64_t size)
      : fs_(std::move(fs)), fd_(fd), size_(size) {}
  ~PortSource() override { fs_->close(fd_); }
  std::uint64_t size() const override { return size_; }
  const std::uint8_t* direct(std::uint64_t, std::size_t) const override { return nullptr; }
  bool set_no_cache() const noexcept override { return fs_->set_no_cache(fd_); }
  format::Error read_at(std::uint64_t off, void* dst, std::size_t n) const override {
    if (off > size_ || n > size_ - off) return format::Error::kTruncated;
    if (n == 0) return format::Error::kNone;
    return fs_->pread(fd_, off, dst, n) == FsStatus::kOk ? format::Error::kNone
                                                         : format::Error::kIo;
  }

 private:
  std::shared_ptr<FsPort> fs_;
  int fd_;
  std::uint64_t size_;
};

bool is_lower_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

// A bundle_id directory name: 64 lowercase hex characters (SHA-256), optional trailing '/'.
bool parse_bundle_dir(std::string target, std::string& out) {
  if (!target.empty() && target.back() == '/') target.pop_back();
  if (target.size() != 64) return false;
  if (!std::all_of(target.begin(), target.end(), is_lower_hex)) return false;
  out = std::move(target);
  return true;
}

}  // namespace

const char* to_string(ResolveStatus s) {
  switch (s) {
    case ResolveStatus::kOk: return "ok";
    case ResolveStatus::kInvalidId: return "invalid_id";
    case ResolveStatus::kMissingLibrary: return "missing_library";
    case ResolveStatus::kMissingInstrument: return "missing_instrument";
    case ResolveStatus::kBundleError: return "bundle_error";
    case ResolveStatus::kInvalidLink: return "invalid_link";
    case ResolveStatus::kIdentityMismatch: return "identity_mismatch";
    case ResolveStatus::kProtectedDowngrade: return "protected_downgrade";
    case ResolveStatus::kProtectedUnsupported: return "protected_unsupported";
  }
  return "?";
}

bool parse_library_id(std::string_view s, format::Uuid& out) {
  out = {};
  if (s.size() != 36) return false;
  std::size_t nibble = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
      continue;
    }
    if (!is_lower_hex(c)) {
      out = {};
      return false;
    }
    const unsigned v = static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10);
    if (nibble % 2 == 0) out[nibble / 2] = static_cast<std::uint8_t>(v << 4);
    else out[nibble / 2] = static_cast<std::uint8_t>(out[nibble / 2] | v);
    ++nibble;
  }
  return nibble == 32;
}

LibraryResolver::LibraryResolver(std::shared_ptr<FsPort> fs, std::string root)
    : fs_(std::move(fs)), root_(std::move(root)) {
  assert(fs_ && "LibraryResolver needs an FsPort");
  if (root_.empty()) root_ = kDefaultLibraryRoot;
  while (root_.size() > 1 && root_.back() == '/') root_.pop_back();
}

ResolveResult LibraryResolver::resolve(std::string_view library_id,
                                       std::string_view instrument_key) const {
  ResolveResult res;

  // 1. Validate ids; nothing touches the filesystem before this passes.
  format::Uuid want_lib{};
  if (!parse_library_id(library_id, want_lib)) return res;  // kInvalidId
  auto key = format::normalize_instrument_key(instrument_key);
  if (!key) return res;

  // Story 12.9: Built-in engine timbres (Init FM, Init VA) resolve directly without filesystem access
  format::Uuid builtin_id{};
  parse_library_id(kBuiltinLibraryId, builtin_id);
  if (want_lib == builtin_id) {
    if (key.value() == kInitFmInstrumentKey) {
      res.status = ResolveStatus::kOk;
      res.library_id = std::string(library_id);
      res.bundle_dir = "builtin";
      res.is_builtin = true;
      res.builtin_info.key = kInitFmInstrumentKey;
      res.builtin_info.name = kInitFmName;
      return res;
    }
    if (key.value() == kInitVaInstrumentKey) {
      res.status = ResolveStatus::kOk;
      res.library_id = std::string(library_id);
      res.bundle_dir = "builtin";
      res.is_builtin = true;
      res.builtin_info.key = kInitVaInstrumentKey;
      res.builtin_info.name = kInitVaName;
      return res;
    }
    if (key.value() == kInitTineInstrumentKey) {
      res.status = ResolveStatus::kOk;
      res.library_id = std::string(library_id);
      res.bundle_dir = "builtin";
      res.is_builtin = true;
      res.builtin_info.key = kInitTineInstrumentKey;
      res.builtin_info.name = kInitTineName;
      return res;
    }
    if (key.value() == kInitReedInstrumentKey) {
      res.status = ResolveStatus::kOk;
      res.library_id = std::string(library_id);
      res.bundle_dir = "builtin";
      res.is_builtin = true;
      res.builtin_info.key = kInitReedInstrumentKey;
      res.builtin_info.name = kInitReedName;
      return res;
    }
    res.status = ResolveStatus::kMissingInstrument;
    return res;
  }

  // 2. `current` must be a symlink to a sibling <bundle_id> directory.
  const std::string lib_dir = root_ + "/" + kLibDirName + "/" + std::string(library_id);
  const std::string link = lib_dir + "/" + kCurrentLinkName;
  const FsStat ls = fs_->lstat(link);
  if (ls.status == FsStatus::kNotFound) {
    res.status = ResolveStatus::kMissingLibrary;
    return res;
  }
  if (ls.status != FsStatus::kOk) {
    res.status = ResolveStatus::kBundleError;
    res.bundle_error = format::Error::kIo;
    return res;
  }
  if (ls.kind != FsKind::kSymlink) {
    res.status = ResolveStatus::kInvalidLink;
    return res;
  }
  std::string target, bundle_dir_name;
  const FsStatus rl = fs_->readlink(link, target);
  if (rl == FsStatus::kNotFound) {  // removed between lstat and readlink
    res.status = ResolveStatus::kMissingLibrary;
    return res;
  }
  if (rl == FsStatus::kIo || rl == FsStatus::kTooLong) {
    res.status = rl == FsStatus::kIo ? ResolveStatus::kBundleError : ResolveStatus::kInvalidLink;
    if (rl == FsStatus::kIo) res.bundle_error = format::Error::kIo;
    return res;
  }
  if (rl != FsStatus::kOk || !parse_bundle_dir(target, bundle_dir_name)) {
    res.status = ResolveStatus::kInvalidLink;  // absolute, nested, "..", loop onto itself, ...
    return res;
  }
  const std::string bundle_dir = lib_dir + "/" + bundle_dir_name;
  const FsStat ds = fs_->lstat(bundle_dir);
  if (ds.status == FsStatus::kNotFound) {  // dangling current
    res.status = ResolveStatus::kMissingLibrary;
    return res;
  }
  if (ds.status != FsStatus::kOk || ds.kind != FsKind::kDirectory) {
    res.status = ds.status == FsStatus::kOk ? ResolveStatus::kInvalidLink
                                            : ResolveStatus::kBundleError;
    if (ds.status != FsStatus::kOk) res.bundle_error = format::Error::kIo;
    return res;
  }

  // 3. Open the bundle: header, manifest and index only.
  int fd = -1;
  std::uint64_t size = 0;
  const FsStatus os = fs_->open_read(bundle_dir + "/" + kBundleFileName, fd, size);
  if (os == FsStatus::kNotFound) return resolve_protected(library_id, want_lib, bundle_dir, bundle_dir_name, key.value());
  if (os != FsStatus::kOk) {
    res.status = ResolveStatus::kBundleError;
    res.bundle_error = format::Error::kIo;
    return res;
  }
  // Story 6.5: a bundle.vlb beside a protected bundle's files is a planted unprotected copy of a factory library (a downgrade): refused, typed, before it is read.
  for (const char* protected_name : {kProtectedBundleFileName, kWrappedKeyFileName}) {
    if (fs_->lstat(bundle_dir + "/" + protected_name).status != FsStatus::kNotFound) {
      fs_->close(fd);
      res.status = ResolveStatus::kProtectedDowngrade;
      return res;
    }
  }
  // Check if it's a ParametricBundle ('VLPB')
  char magic[4]{};
  if (size >= 4 && fs_->pread(fd, 0, magic, 4) != FsStatus::kOk) {
    fs_->close(fd);
    res.status = ResolveStatus::kBundleError;
    res.bundle_error = format::Error::kIo;
    return res;
  }
  if (size >= 4 && std::memcmp(magic, "VLPB", 4) == 0) {
    std::vector<uint8_t> pbuf(size);
    if (fs_->pread(fd, 0, pbuf.data(), size) == FsStatus::kOk) {
      fs_->close(fd);
      auto pb_res = format::ParametricBundle::deserialize(pbuf);
      if (pb_res) {
        auto pb = std::make_shared<format::ParametricBundle>(pb_res.take());
        if (pb->library_id() != want_lib || format::to_hex(pb->bundle_id()) != bundle_dir_name) {
          res.status = ResolveStatus::kIdentityMismatch;
          return res;
        }
        for (size_t idx = 0; idx < pb->timbres().size(); ++idx) {
          if (pb->timbres()[idx].key == key.value()) {
            res.status = ResolveStatus::kOk;
            res.instrument = idx;
            res.bundle_dir = bundle_dir;
            res.library_id = std::string(library_id);
            res.builtin_info.key = pb->timbres()[idx].key;
            res.builtin_info.name = pb->timbres()[idx].name;
            res.parametric_bundle = std::move(pb);
            return res;
          }
        }
        res.status = ResolveStatus::kMissingInstrument;
        return res;
      }
      res.status = ResolveStatus::kBundleError;
      res.bundle_error = pb_res.error();
      return res;
    }
  }

  auto opened = format::BundleReader::open_source(std::make_unique<PortSource>(fs_, fd, size));
  if (!opened) {
    res.status = ResolveStatus::kBundleError;
    res.bundle_error = opened.error();
    return res;
  }
  auto bundle = std::make_shared<format::BundleReader>(opened.take());

  // 4. The bundle must be the one its path claims.
  if (bundle->library_id() != want_lib ||
      format::to_hex(bundle->bundle_id()) != bundle_dir_name) {
    res.status = ResolveStatus::kIdentityMismatch;
    return res;
  }

  // 5. Find the instrument (exact key; no fallback).
  const auto ins = bundle->instruments();
  const auto it = std::find_if(ins.begin(), ins.end(),
                               [&](const format::InstrumentInfo& i) { return i.key == key.value(); });
  if (it == ins.end()) {
    res.status = ResolveStatus::kMissingInstrument;
    return res;
  }
  res.instrument = static_cast<std::size_t>(it - ins.begin());
  res.bundle = std::move(bundle);
  res.bundle_dir = bundle_dir;
  res.library_id = std::string(library_id);
  res.status = ResolveStatus::kOk;
  return res;
}

// Story 6.5: the directory has no bundle.vlb. Is there a protected bundle.vlrp? (One more op, only on this path: an unprotected library never gets here.)
ResolveResult LibraryResolver::resolve_protected(std::string_view library_id, const format::Uuid& want_lib, const std::string& bundle_dir,
                                                 const std::string& bundle_dir_name, const std::string& instrument_key) const {
  ResolveResult res;
  res.from_protected = true;
  int fd = -1;
  std::uint64_t size = 0;
  const FsStatus os = fs_->open_read(bundle_dir + "/" + kProtectedBundleFileName, fd, size);
  if (os != FsStatus::kOk) {  // neither file: exactly the verdict an absent bundle.vlb always had
    res.status = ResolveStatus::kBundleError;
    res.bundle_error = format::Error::kIo;
    return res;
  }
  auto source = std::make_unique<PortSource>(fs_, fd, size);  // closes fd on every path below
  if (!trusted_) {  // nothing is opened on trust: this host plays no protected library
    res.status = ResolveStatus::kProtectedUnsupported;
    return res;
  }
  auto opened = format::ProtectedBundleView::open_source(std::move(source), *trusted_);
  if (!opened) {
    res.status = ResolveStatus::kBundleError;
    res.bundle_error = opened.error();
    return res;
  }
  auto view = std::make_shared<const format::ProtectedBundleView>(opened.take());
  if (view->info().library_id != want_lib || format::to_hex(view->info().bundle_id) != bundle_dir_name) {
    res.status = ResolveStatus::kIdentityMismatch;
    return res;
  }
  const auto ins = view->instruments();
  const auto it = std::find_if(ins.begin(), ins.end(), [&](const format::InstrumentInfo& i) { return i.key == instrument_key; });
  if (it == ins.end()) {
    res.status = ResolveStatus::kMissingInstrument;
    return res;
  }
  res.instrument = static_cast<std::size_t>(it - ins.begin());
  res.from_protected = false;
  res.protected_view = std::move(view);
  res.bundle_dir = bundle_dir;
  res.library_id = std::string(library_id);
  res.status = ResolveStatus::kOk;
  return res;
}

}  // namespace velora::host
