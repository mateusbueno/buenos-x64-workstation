#include "vault.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <mutex>

namespace velora::registry {
namespace cp = velora::contentpack;

struct KeyVault::Impl {
  std::string dir;
  void* page = MAP_FAILED;
  std::size_t page_size = 0;
  bool locked = false;
  std::mutex mu;  // the content-key scratch space is shared
  // layout of the dedicated page: [0,32) KEK, [32,64) plaintext content key scratch
  Key32& kek() { return *reinterpret_cast<Key32*>(static_cast<std::uint8_t*>(page)); }
  Key32& scratch() { return *reinterpret_cast<Key32*>(static_cast<std::uint8_t*>(page) + 32); }
  ~Impl() {
    if (page != MAP_FAILED) {
      cp::wipe(page, page_size);
      if (locked) ::munlock(page, page_size);  // only ever this private mapping
      ::munmap(page, page_size);
    }
  }
};

const char* to_string(WrapStatus s) {
  switch (s) {
    case WrapStatus::kOk: return "ok";
    case WrapStatus::kUnknownLibrary: return "unknown_library";
    case WrapStatus::kVaultUnreadable: return "vault_unavailable";
    case WrapStatus::kVaultEntryInvalid: return "vault_entry_invalid";
    case WrapStatus::kWrapFailed: return "key_wrap_failed";
  }
  return "key_wrap_failed";
}

std::unique_ptr<KeyVault> KeyVault::create(const std::string& vault_dir, const std::string& kek_file, bool allow_no_mlock, std::string* err) {
  struct stat st;
  if (::stat(vault_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
    if (err) *err = vault_dir + ": not a directory";
    return nullptr;
  }
  std::unique_ptr<KeyVault> v(new KeyVault());
  v->impl_ = std::make_unique<Impl>();
  Impl& im = *v->impl_;
  im.dir = vault_dir;
  im.page_size = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
  im.page = ::mmap(nullptr, im.page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (im.page == MAP_FAILED) {
    if (err) *err = "cannot allocate the key page";
    return nullptr;
  }
#ifdef MADV_DONTDUMP
  (void)::madvise(im.page, im.page_size, MADV_DONTDUMP);
#endif
  im.locked = ::mlock(im.page, im.page_size) == 0;
  if (!im.locked && !allow_no_mlock) {
    if (err) *err = std::string("the KEK page can not be locked in RAM (") + std::strerror(errno) + "): raise RLIMIT_MEMLOCK (systemd LimitMEMLOCK=) or pass --allow-no-mlock for development";
    return nullptr;
  }
  if (!read_secret_file(kek_file, im.kek().data(), 32, err)) return nullptr;
  return v;
}

bool KeyVault::locked() const { return impl_->locked; }

KeyVault::~KeyVault() = default;

std::string KeyVault::kek_id_hex() const {
  const auto id = cp::kek_id(impl_->kek());
  return hex_encode(id.data(), id.size());
}

WrapStatus KeyVault::wrap(std::string_view library_id, const Key32& device_pub, WrappedBlob& out) const {
  out.fill(0);
  if (!valid_library_id(library_id)) return WrapStatus::kUnknownLibrary;
  const std::string path = impl_->dir + "/" + std::string(library_id) + "/" + "content-key.vault";
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
  if (fd < 0) return errno == ENOENT || errno == ENOTDIR ? WrapStatus::kUnknownLibrary : WrapStatus::kVaultUnreadable;
  struct stat st;
  std::array<std::uint8_t, cp::kVaultEntrySize> entry{};
  bool ok = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && static_cast<std::size_t>(st.st_size) == entry.size();
  if (ok) {
    std::size_t got = 0;
    while (got < entry.size()) {
      const ssize_t r = ::read(fd, entry.data() + got, entry.size() - got);
      if (r < 0 && errno == EINTR) continue;
      if (r <= 0) break;
      got += static_cast<std::size_t>(r);
    }
    ok = got == entry.size();
  }
  ::close(fd);
  if (!ok) return WrapStatus::kVaultEntryInvalid;
  // The plaintext content key lives only in the scratch space of the locked page, for the length of this call.
  std::lock_guard<std::mutex> lk(impl_->mu);
  Key32& content = impl_->scratch();
  const bool opened = cp::vault_entry_open(impl_->kek(), entry, library_id, content);
  const bool sealed = opened && cp::wrapped_key_seal_random(content, library_id, device_pub, out);
  cp::wipe(content.data(), content.size());
  if (!opened) return WrapStatus::kVaultEntryInvalid;
  if (!sealed) {
    out.fill(0);
    return WrapStatus::kWrapFailed;
  }
  return WrapStatus::kOk;
}

}  // namespace velora::registry
