#include "library_gen.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstring>
#include <vector>

#include "velora/format/bundle.hpp"
#include "velora/host/library_resolver.hpp"

namespace velora::libgen {
namespace {

// mkdir -p. An existing path must be a directory (stat follows symlinks to directories).
bool mkdir_p(const std::string& p) {
  if (p.empty()) return false;
  for (std::size_t i = 1; i <= p.size(); ++i) {
    if (i != p.size() && p[i] != '/') continue;
    const std::string part = p.substr(0, i);
    if (::mkdir(part.c_str(), 0755) == 0) continue;
    struct stat st{};
    if (errno != EEXIST || ::stat(part.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
      errno = errno == EEXIST ? ENOTDIR : errno;
      return false;
    }
  }
  return true;
}

bool write_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  const std::uint8_t* p = bytes.data();
  std::size_t n = bytes.size();
  while (n > 0) {
    const ssize_t w = ::write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      return false;
    }
    p += w;
    n -= static_cast<std::size_t>(w);
  }
  return ::close(fd) == 0;
}

std::vector<std::int32_t> tone(std::uint64_t frames, double freq) {
  std::vector<std::int32_t> v(frames * 2);
  for (std::uint64_t i = 0; i < frames; ++i) {
    const auto x = static_cast<std::int32_t>(std::lround(std::sin(double(i) * freq) * 20000.0));
    v[2 * i] = x;
    v[2 * i + 1] = -x;
  }
  return v;
}

}  // namespace

std::string library_id_for_index(std::uint32_t index) {
  const std::string fp = "velora-libgen/" + std::to_string(index);
  const auto id = format::library_id_from_fingerprint(
      {reinterpret_cast<const std::uint8_t*>(fp.data()), fp.size()});
  return format::uuid_to_string(id);
}

bool generate_library(const Options& opt, std::uint32_t index, Generated* out, bool* pad_ok,
                      std::string* err) {
  auto fail = [&](const std::string& m) {
    if (err) *err = m + ": " + std::strerror(errno);
    return false;
  };
  const std::string fp = "velora-libgen/" + std::to_string(index);
  const auto lib_uuid = format::library_id_from_fingerprint(
      {reinterpret_cast<const std::uint8_t*>(fp.data()), fp.size()});

  format::BundleWriter w;
  w.set_library_id(lib_uuid);
  const char* keys[2] = {kInstrumentA, kInstrumentB};
  for (int k = 0; k < 2; ++k) {
    auto ins = w.add_instrument(keys[k], keys[k]);
    if (!ins) {
      if (err) *err = std::string("add_instrument: ") + format::to_string(ins.error());
      return false;
    }
    const auto pcm = tone(4096 + 300, 0.03 * (k + 1) + 0.0001 * (index % 97));
    format::SampleInput si;
    si.name = "s0";
    si.format = {2, 16, false, 48000};
    si.frame_count = pcm.size() / 2;
    si.data = pcm;
    if (format::Error e = w.add_sample(ins.value(), si); e != format::Error::kNone) {
      if (err) *err = std::string("add_sample: ") + format::to_string(e);
      return false;
    }
  }
  auto bytes = w.finalize();
  if (!bytes) {
    if (err) *err = std::string("finalize: ") + format::to_string(bytes.error());
    return false;
  }
  auto probe = format::BundleReader::open_memory(bytes.value());
  if (!probe) {
    if (err) *err = std::string("probe: ") + format::to_string(probe.error());
    return false;
  }
  const std::string bundle_id = format::to_hex(probe.value().bundle_id());
  const std::string lib_id = format::uuid_to_string(lib_uuid);

  const std::string lib_root = opt.root + "/" + host::kLibDirName;
  const std::string lib_dir = lib_root + "/" + lib_id;
  const std::string bdir = lib_dir + "/" + bundle_id;
  if (!mkdir_p(bdir))
    return fail("mkdir " + bdir);
  if (!write_file(bdir + "/" + host::kBundleFileName, bytes.value())) return fail("write bundle");

  if (opt.pad_bytes > 0) {
    bool ok = false;
    const int fd = ::open((bdir + "/" + kPaddingFileName).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
      ok = ::ftruncate(fd, static_cast<off_t>(opt.pad_bytes)) == 0;
      ::close(fd);
    }
    if (pad_ok && !ok) *pad_ok = false;
  }

  // Flip `current` last (symlink to a temp name, then rename over).
  const std::string link = lib_dir + "/" + host::kCurrentLinkName;
  const std::string tmp = lib_dir + "/.current.tmp";
  ::unlink(tmp.c_str());
  if (::symlink(bundle_id.c_str(), tmp.c_str()) != 0) return fail("symlink");
  if (::rename(tmp.c_str(), link.c_str()) != 0) {
    const int saved = errno;
    ::unlink(tmp.c_str());
    errno = saved;
    return fail("rename current");
  }
  if (out) *out = {lib_id, bundle_id};
  return true;
}

bool generate_all(const Options& opt, bool* pad_ok, std::string* err) {
  if (pad_ok) *pad_ok = true;
  if (static_cast<std::uint64_t>(opt.first_index) + opt.libraries > 0x100000000ull) {
    if (err) *err = "first_index + libraries overflows uint32";
    return false;
  }
  for (std::uint32_t i = 0; i < opt.libraries; ++i)
    if (!generate_library(opt, opt.first_index + i, nullptr, pad_ok, err)) return false;
  return true;
}

}  // namespace velora::libgen
