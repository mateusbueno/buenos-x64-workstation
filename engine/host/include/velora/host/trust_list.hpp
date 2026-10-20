#pragma once
// The engine's trusted content-manifest keys for protected libraries (Story 6.5). The set is INJECTED: the host (or a test) builds a format::TrustedKeys and
// hands it to LibraryResolver::set_protected_trust(). This reads one from a tools/keys trust list file (`tools/keys/trust/<root>.pub-list`: public keys only),
// the way the dev variant of the librarian does; the production image embeds its own set at build time (tools/keys, Story 6.9 onwards) and passes it the same way.
//
// Line format: `<root> <role> <key_id hex16> <public_key hex64>`, `#` starts a comment, role `content-manifest`. A key id that is not the first 8 bytes of
// the SHA-256 of its key, a malformed line or a key id listed twice is an error (nothing is returned).
#include <string>

#include "velora/format/protected_verify.hpp"

namespace velora::host {

bool load_trust_list_file(const std::string& path, velora::format::TrustedKeys& out, std::string& err);

}  // namespace velora::host
