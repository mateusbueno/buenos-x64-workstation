#pragma once
// Tiny file-system port used by the library resolver. Every filesystem access of the resolver goes
// through it, so a counting wrapper can assert "no scanning" (zero list_dir ops, bounded op count).
// Control/I-O thread only; never called from the audio thread.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace velora::host {

enum class FsStatus : std::uint8_t { kOk = 0, kNotFound, kNotRegular, kTooLong, kIo };
enum class FsKind : std::uint8_t { kNone = 0, kFile, kDirectory, kSymlink, kOther };

struct FsStat {
  FsStatus status = FsStatus::kIo;
  FsKind kind = FsKind::kNone;  // lstat semantics: a symlink is reported as kSymlink
  std::uint64_t size = 0;
};

class FsPort {
 public:
  virtual ~FsPort() = default;
  // lstat: does not follow a final symlink. kNotFound for ENOENT/ENOTDIR.
  virtual FsStat lstat(const std::string& path) = 0;
  // Target of a symlink, verbatim.
  virtual FsStatus readlink(const std::string& path, std::string& target) = 0;
  // Opens a regular file read-only without following a final symlink; reports its size.
  virtual FsStatus open_read(const std::string& path, int& fd, std::uint64_t& size) = 0;
  // Reads exactly n bytes at off (kIo on short read or error).
  virtual FsStatus pread(int fd, std::uint64_t off, void* dst, std::size_t n) = 0;
  virtual void close(int fd) = 0;
  // Directory listing. The resolver never calls this; it exists so tests can prove it.
  virtual FsStatus list_dir(const std::string& path, std::vector<std::string>& names) = 0;
};

// POSIX implementation (macOS and Linux).
std::shared_ptr<FsPort> make_posix_fs();

struct FsOpCounts {
  std::uint64_t lstat = 0, readlink = 0, open = 0, pread = 0, close = 0, list_dir = 0;
  std::uint64_t total() const { return lstat + readlink + open + pread + close + list_dir; }
  friend bool operator==(const FsOpCounts&, const FsOpCounts&) = default;
};

// Forwards to `inner` and counts every call.
class CountingFs final : public FsPort {
 public:
  explicit CountingFs(std::shared_ptr<FsPort> inner) : inner_(std::move(inner)) {}
  FsStat lstat(const std::string& path) override;
  FsStatus readlink(const std::string& path, std::string& target) override;
  FsStatus open_read(const std::string& path, int& fd, std::uint64_t& size) override;
  FsStatus pread(int fd, std::uint64_t off, void* dst, std::size_t n) override;
  void close(int fd) override;
  FsStatus list_dir(const std::string& path, std::vector<std::string>& names) override;

  FsOpCounts counts() const;
  void reset();

 private:
  std::shared_ptr<FsPort> inner_;
  std::atomic<std::uint64_t> lstat_{0}, readlink_{0}, open_{0}, pread_{0}, close_{0}, list_dir_{0};
};

}  // namespace velora::host
