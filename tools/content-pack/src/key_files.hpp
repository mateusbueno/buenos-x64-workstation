#pragma once
// Key files of a root and the root guard (Story 6.3, moved out of main.cpp in Story 6.9 so tools/factory-release resolves and checks keys with the SAME code as
// `velora-content-pack pack`). Behaviour is unchanged; README.md ("Keys and the root guard") describes it.
#include <cstddef>
#include <cstdint>
#include <string>

#include "crypto.hpp"
#include "errors.hpp"
#include "pack_description.hpp"
#include "velora/format/protected_format.hpp"

namespace velora::contentpack {

// Raw key file: a regular file of exactly `n` bytes; a private one must not be readable by group or others. kBadKeyFile otherwise.
Failure read_key_file(const std::string& path, std::uint8_t* out, std::size_t n, bool secret, const char* what);

// The dev key directory of velora_keys.py: $XDG_DATA_HOME/velora-dev-keys, else ~/.velora-dev-keys ("" when neither is set).
std::string default_dev_dir();

// Resolves the signing seed and the KEK of `root` and applies the root guard to both (sidecar root / key_id / dev_only, DEV-ONLY.txt, a dev-root key never serves
// another root). `keys_dir` holds content-manifest.ed25519.seed and kek.aes256; `signing_key` / `kek` override one file each; for root dev without any of them the
// default dev key directory is used. `dev_trust_list` is the committed dev trust list (tools/keys/trust/dev.pub-list).
Failure load_root_keys(Root root, const std::string& keys_dir, const std::string& signing_key, const std::string& kek, const std::string& dev_trust_list,
                       Secret<velora::format::SigningSeed>& seed, Secret<Key32>& kek_out);

}  // namespace velora::contentpack
