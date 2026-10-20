// Story 6.5 negative control: decrypting a protected block inside a [[clang::nonblocking]] function must be reported by RealtimeSanitizer (the reader allocates, hashes
// and runs AES; it is a non-real-time operation by contract). CTest passes it only when RTSan itself reports the violation. This is what proves that "the audio callback
// does not decrypt" (rtsan_protected_test) is a property the sanitizer enforces.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "protected_fixtures.hpp"
#include "velora/format/protected_reader.hpp"

namespace fmt = velora::format;

static fmt::Error decrypt_in_rt(const fmt::ProtectedBundleReader& r, std::vector<std::int32_t>& out, fmt::BlockDecoder& dec) [[clang::nonblocking]] {
  auto res = r.read_block(0, 0, out, dec);  // allocates a ciphertext buffer, hashes, runs AES: RTSan violation
  return res.ok() ? fmt::Error::kNone : res.error();
}

int main() {
  pfx::TmpDir tmp;
  const pfx::Keys keys = pfx::make_keys(5);
  const auto lib = pfx::install_library(tmp.path, 1, {{"multi.sfz", pfx::Spec::kMulti, 0.1f, 5000}}, keys);
  fmt::ContentKey ck;
  ck.bytes = keys.content;
  auto reader = fmt::ProtectedBundleReader::open_file((lib.dir / "bundle.vlrp").string(), keys.trusted, ck);
  if (!reader.ok()) return 2;
  fmt::BlockDecoder dec;
  std::vector<std::int32_t> out(4096 * 2);
  return decrypt_in_rt(reader.value(), out, dec) == fmt::Error::kNone ? 0 : 1;
}
