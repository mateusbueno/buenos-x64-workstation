// Story 6.4: the trusted keys embedded for this image variant (see velora/librarian/trust.hpp). The generated header is public keys only.
#include "velora/librarian/trust.hpp"

#include VELORA_LIBRARIAN_TRUST_HEADER
#include "velora/librarian/install.hpp"

namespace velora::librarian {

velora::format::TrustedKeys embedded_trusted_keys() { return velora::format::TrustedKeys::from_generated(VELORA_LIBRARIAN_TRUST_NS::kTrustedContentKeys); }

const char* embedded_trust_root() { return VELORA_LIBRARIAN_TRUST_NS::kTrustedRoot; }

#ifdef VELORA_LIBRARIAN_TRUST_OVERRIDE
bool trust_list_override_allowed() { return std::string(VELORA_LIBRARIAN_TRUST_NS::kTrustedRoot) == "dev"; }
#else
bool trust_list_override_allowed() { return false; }  // compiled out: only a dev-variant build reads a trust list from a file
#endif

bool resolve_trusted_keys(const std::string& override_path, velora::format::TrustedKeys& out, std::string& err) {
  if (!override_path.empty() && !trust_list_override_allowed()) {
    err = "--trust-list is available only in the dev variant of the librarian";
    return false;
  }
  return resolve_trusted_keys_for_variant(VELORA_LIBRARIAN_TRUST_NS::kTrustedRoot, VELORA_LIBRARIAN_TRUST_NS::kIsProduction, embedded_trusted_keys(), override_path, out, err);
}

}  // namespace velora::librarian
