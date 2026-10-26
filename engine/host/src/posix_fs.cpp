#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

#include "velora/host/fs_port.hpp"

namespace velora::host {
namespace {

FsStatus from_errno() {
  return (errno == ENOENT || errno == ENOTDIR) ? FsStatus::kNotFound : FsStatus::kIo;
}

class PosixFs final : public FsPort {
 public:
  FsStat lstat(const std::string& path) override {
    struct stat st;
    FsStat r;
    if (::lstat(path.c_str(), &st) != 0) {
      r.status = from_errno();
      return r;
    }
    r.status = FsStatus::kOk;
    r.size = static_cast<std::uint64_t>(st.st_size);
    if (S_ISREG(st.st_mode)) r.kind = FsKind::kFile;
    else if (S_ISDIR(st.st_mode)) r.kind = FsKind::kDirectory;
    else if (S_ISLNK(st.st_mode)) r.kind = FsKind::kSymlink;
    else r.kind = FsKind::kOther;
    return r;
  }

  FsStatus readlink(const std::string& path, std::string& target) override {
    char buf[PATH_MAX];
    const ssize_t n = ::readlink(path.c_str(), buf, sizeof buf);
    if (n < 0) return errno == EINVAL ? FsStatus::kNotRegular : from_errno();
    if (static_cast<std::size_t>(n) >= sizeof buf) return FsStatus::kTooLong;
    target.assign(buf, static_cast<std::size_t>(n));
    return FsStatus::kOk;
  }

  FsStatus open_read(const std::string& path, int& fd, std::uint64_t& size) override {
    const int f = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (f < 0) return errno == ELOOP ? FsStatus::kNotRegular : from_errno();
    struct stat st{};
    if (::fstat(f, &st) != 0) {
      ::close(f);
      return FsStatus::kIo;
    }
    if (!S_ISREG(st.st_mode)) {
      ::close(f);
      return FsStatus::kNotRegular;
    }
    fd = f;
    size = static_cast<std::uint64_t>(st.st_size);
    return FsStatus::kOk;
  }

  FsStatus pread(int fd, std::uint64_t off, void* dst, std::size_t n) override {
    auto* p = static_cast<std::uint8_t*>(dst);
    while (n > 0) {
      const ssize_t r = ::pread(fd, p, n, static_cast<off_t>(off));
      if (r < 0) {
        if (errno == EINTR) continue;
        return FsStatus::kIo;
      }
      if (r == 0) return FsStatus::kIo;
      p += r;
      off += static_cast<std::uint64_t>(r);
      n -= static_cast<std::size_t>(r);
    }
    return FsStatus::kOk;
  }

  void close(int fd) override { ::close(fd); }

  bool set_no_cache(int fd) override {
#if defined(__APPLE__)
    return ::fcntl(fd, F_NOCACHE, 1) == 0;
#else
    (void)fd;  // Linux: O_DIRECT is the board's io_uring reader (aligned buffers); no page-cache bypass in the portable reader
    return false;
#endif
  }

  FsStatus list_dir(const std::string& path, std::vector<std::string>& names) override {
    DIR* d = ::opendir(path.c_str());
    if (!d) return from_errno();
    while (const dirent* e = ::readdir(d)) names.emplace_back(e->d_name);
    ::closedir(d);
    return FsStatus::kOk;
  }
};

}  // namespace

std::shared_ptr<FsPort> make_posix_fs() { return std::make_shared<PosixFs>(); }

FsStat CountingFs::lstat(const std::string& p) {
  lstat_.fetch_add(1, std::memory_order_relaxed);
  return inner_->lstat(p);
}
FsStatus CountingFs::readlink(const std::string& p, std::string& t) {
  readlink_.fetch_add(1, std::memory_order_relaxed);
  return inner_->readlink(p, t);
}
FsStatus CountingFs::open_read(const std::string& p, int& fd, std::uint64_t& size) {
  open_.fetch_add(1, std::memory_order_relaxed);
  return inner_->open_read(p, fd, size);
}
FsStatus CountingFs::pread(int fd, std::uint64_t off, void* dst, std::size_t n) {
  pread_.fetch_add(1, std::memory_order_relaxed);
  return inner_->pread(fd, off, dst, n);
}
void CountingFs::close(int fd) {
  close_.fetch_add(1, std::memory_order_relaxed);
  inner_->close(fd);
}
FsStatus CountingFs::list_dir(const std::string& p, std::vector<std::string>& names) {
  list_dir_.fetch_add(1, std::memory_order_relaxed);
  return inner_->list_dir(p, names);
}

FsOpCounts CountingFs::counts() const {
  FsOpCounts c;
  c.lstat = lstat_.load(std::memory_order_relaxed);
  c.readlink = readlink_.load(std::memory_order_relaxed);
  c.open = open_.load(std::memory_order_relaxed);
  c.pread = pread_.load(std::memory_order_relaxed);
  c.close = close_.load(std::memory_order_relaxed);
  c.list_dir = list_dir_.load(std::memory_order_relaxed);
  return c;
}

void CountingFs::reset() {
  lstat_ = readlink_ = open_ = pread_ = close_ = list_dir_ = 0;
}

}  // namespace velora::host
