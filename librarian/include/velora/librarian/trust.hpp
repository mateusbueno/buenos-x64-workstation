#pragma once
// Story 6.4: the trusted content-signing public keys of this build. They are the keys tools/keys embeds for the image variant the librarian is built for
// (velora_trusted_keys_<variant>.hpp, generated at configure time from tools/keys/trust/<variant>.pub-list; the CMake cache variable
// VELORA_LIBRARIAN_TRUST_VARIANT picks the variant, `dev` by default, and VELORA_DEV_KEYS_DIR adds a developer's own local dev key to the dev variant). Public
// keys only. The `dev` variant may also read a trust list from a file (`--trust-list`), so a developer or a test can name a throwaway key; the factory-test and
// production variants cannot be overridden at run time.
#include <string>

#include "velora/format/protected_verify.hpp"

namespace velora::librarian {

velora::format::TrustedKeys embedded_trusted_keys();
const char* embedded_trust_root();  // "dev", "factory-test" or "production"
bool trust_list_override_allowed();  // true only for the dev variant

// The embedded keys, or, when `override_path` is not empty, the keys of that `*.pub-list` file (load_trust_list, install.hpp). false with `err` set when the
// override is refused (not the dev variant) or unreadable.
bool resolve_trusted_keys(const std::string& override_path, velora::format::TrustedKeys& out, std::string& err);

}  // namespace velora::librarian
