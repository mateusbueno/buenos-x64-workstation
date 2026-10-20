// Story 6.2: a binary that links ONLY velora_format_verify (no decrypt target). It opens a protected bundle with a trusted
// public key (hex file) and verifies every ciphertext block hash. tools/ci/check_format_no_decrypt.py scans this binary
// for cipher/decrypt symbols; running it proves the verify layer is complete without them.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "velora/format/protected_verify.hpp"

using namespace velora::format;

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: format_verify_probe BUNDLE TRUSTED_PUBKEY_HEX_FILE\n");
    return 2;
  }
  std::ifstream f(argv[2]);
  std::string hex;
  f >> hex;
  if (hex.size() != 64) return 3;
  PublicKey pub{};
  for (int i = 0; i < 32; ++i) {
    unsigned v = 0;
    if (std::sscanf(hex.c_str() + 2 * i, "%2x", &v) != 1) return 3;
    pub[i] = static_cast<std::uint8_t>(v);
  }
  TrustedKeys keys({TrustedKey{key_id_of_public(pub), pub}});
  auto v = ProtectedBundleView::open_file(argv[1], keys);
  if (!v) {
    std::printf("open failed: %s\n", to_string(v.error()));
    return 1;
  }
  if (Error e = v.value().verify_all_blocks(); e != Error::kNone) {
    std::printf("block verification failed: %s\n", to_string(e));
    return 1;
  }
  std::printf("verified %llu blocks, %zu samples, no decrypt\n", static_cast<unsigned long long>(v.value().total_blocks()),
              v.value().samples().size());
  // Wrong key list must be refused with the typed error.
  auto bad = ProtectedBundleView::open_file(argv[1], TrustedKeys());
  if (bad.ok() || bad.error() != Error::kUntrustedKey) return 1;
  return 0;
}
