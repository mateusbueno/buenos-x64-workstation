# tools/keys

Role: key hierarchy documentation, the dev key tool, and the trusted-key embedding for image variants (Story 6.1).

Dependency rule: standalone host tooling; Python 3 standard library only (no `cryptography`, no OpenSSL binding, nothing to add to
`THIRD_PARTY_NOTICES.md`). Nothing in `engine/core` may depend on it. The generated header is consumed by Stories 6.2, 6.3 and 6.5.

Read `KEY-HIERARCHY.md` first: roots, keys, holders, rotation, trust per image variant, dev key files and the `wrapped_key_v1` / `vault_entry_v1` formats.

## Dev keys

```
python3 tools/keys/velora_keys.py generate            # into $XDG_DATA_HOME/velora-dev-keys or ~/.velora-dev-keys
python3 tools/keys/velora_keys.py generate --out DIR [--force]
python3 tools/keys/velora_keys.py show [--dir DIR]    # key ids and public keys only
python3 tools/keys/velora_keys.py trust-entry [--dir DIR]   # a line for trust/dev.pub-list
```

The tool writes the dev content-manifest Ed25519 key, a dev KEK and a dev X25519 device key pair (raw 32-byte files, mode 0600, sidecar JSON with
`root: dev`). It refuses any output directory inside a git work tree and refuses to overwrite existing keys without `--force`. Ed25519 and X25519 are
implemented in `velora_keys.py` following RFC 8032 and RFC 7748 and checked against the RFC test vectors; they are not constant-time and are for dev
keys only. Production keys are made in an offline HSM (Story 6.10), never by this tool.

The committed `trust/dev.pub-list` is deliberately empty (only comments): every developer has their own dev key, so a dev build trusts it through
`--dev-keys-dir` / `DEV_KEYS_DIR`, or the developer appends the `trust-entry` line locally. Also: `velora_keys.py sign --dir DIR FILE` prints an Ed25519
signature (cross-check against the RFC 8032 vectors).

## Trusted keys per variant

```
python3 tools/keys/embed_trusted_keys.py --variant dev|factory-test|production --output FILE [--trust-dir DIR] [--dev-keys-dir DIR]
```

CMake: `include(tools/keys/TrustedKeys.cmake)` then `velora_embed_trusted_keys(<variant> [TRUST_DIR d] [OUTPUT_DIR d] [DEV_KEYS_DIR d])`; it sets
`VELORA_TRUSTED_KEYS_HEADER_<variant>` to the generated `velora_trusted_keys_<variant>.hpp` and fails the configure step when a dev public key would
go into a production variant or a production variant has no production key list. The committed `trust/production.pub-list` is deliberately empty
until the Story 6.10 ceremony, so a production embed is refused today.

## Tests (ctest)

* `keys_tools` runs `tests/test_keys.py`: RFC vectors, generate (formats, modes, labels, no private material in sidecars), refusal inside a git work tree
  and without `--force`, default directory, embedding rules and every refusal, and the same refusals through the CMake function in script mode
  (`tests/embed_variant.cmake`). Test keys are generated into temp directories.
* `keys_trusted_header` compiles the generated dev and (empty) factory-test headers from `tests/fixtures` together (public keys only; per-variant namespaces).
* `tests/wrap_ref.py` is a test-only pure-Python AES-GCM/HKDF used to pin the `wrapped_key_v1` and `vault_entry_v1` vectors of `KEY-HIERARCHY.md`.
* `no_private_keys` and `no_private_keys_selftest` run `tools/ci/check_no_private_keys.py` on the repository and on generated violating fixtures.
