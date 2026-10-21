# Signing and content-key hierarchy

Story 6.1 (AR34 partial, NFR23 partial). This document is the contract for every signing, verification and key-wrapping step in Epic 6 and for the
update flow of Epic 8. Stories 6.2 (bundle format), 6.3 (content-pack tool), 6.5 (OP-TEE key path), 6.8 (device registry and vault), 6.10 (production
ceremony), 6.11-6.13 (images) implement against it.

Status of the pieces it relates to: the dev key tool, the trust lists and the trusted-key embedding exist in this directory. The boot-signing chain
(Story 1.6, `tools/spikes/secure-boot/REPORT.md`) and the fused-board device key (Story 1.7) are not implemented yet, so the boot-image key rows below
state the intended trust relation; the algorithm of the boot key follows the chain chosen in 1.6 and is the only open item of this document.

## 1. Three separate roots

There are three independent roots: **dev**, **factory-test** and **production**. A root is a set of four keys. No key, certificate, KEK or trust list
is shared between roots, and an image variant trusts the public keys of exactly one root.

| Root | Purpose | Where its private halves may exist |
|---|---|---|
| dev | developer boards (unfused or fused with the dev root), host tests, the dev image variant | developer machines, outside the repository, via `velora_keys.py`; CI never holds them |
| factory-test | the factory-test image variant that runs the line tests (ports, MCU, keybed, codec loopback, display, touch, NVMe) | a vendor-controlled signing host for the factory-test image; not an HSM, not a developer laptop; created in Story 6.13 |
| production | the production image variant, fused units, the shipped factory library | **only inside an offline HSM**, generated there at the Story 6.10 ceremony (non-exportable, or exported only as an encrypted backup to the second token); only public keys and their hashes ever leave |

Production private keys never exist on a developer machine, in CI, in the repository, in Yocto build inputs or in backups outside the second HSM
token. Nothing in this repository can generate a production key: the dev tool writes root `dev` only.

## 2. Keys per root

Each root has four keys.

| Key | Algorithm | What it signs or protects | Public half is trusted by |
|---|---|---|---|
| Boot-image signing key | per the chain chosen in Story 1.6 (barebox `rksecure` or U-Boot + Rockchip tools); signs loader, TF-A, OP-TEE, bootloader and the kernel FIT | the boot path of the device | the boot ROM through the fused OTP hash of the root public key (Story 1.7 for dev, Story 6.9 for production); later stages trust the key embedded in the previous stage |
| RAUC bundle signing key | X.509 key and certificate chain of the root, as RAUC requires (algorithm per RAUC/Yocto, ECDSA P-256 intended) | OS update bundles (Epic 8) | the RAUC keyring (`ca.cert`) baked into the rootfs of each variant |
| Content-manifest signing key | **Ed25519** | the manifest of every protected bundle (Story 6.2) and of the content pack (Story 6.3) | the trusted-key list embedded in the librarian, the engine and `formatlib` of each variant (section 4) |
| Content-key vault KEK | 256-bit AES key | the vendor vault's stored per-library content keys (Story 6.8) | not applicable: a secret, never trusted by a device; used only by the vendor registry/vault and the pack tool |

The per-device key pair (X25519) is **not** a root key: each device has its own, generated inside OP-TEE on the unit (Stories 1.7, 6.5) and its public
half is registered in the vendor device registry (Story 6.8, `tools/registry`: it opens the vaulted keys with the KEK only inside the wrap operation and issues `wrapped_key_v1` to stations over mutual TLS). The dev root additionally provides a *dev device key pair* so the wrap/unwrap code of 6.3,
6.5 and 6.8 can be tested on the Mac. (Story 6.5 implements the engine side of the unwrap on the Mac with it: `engine/host/dev_keys`, `DevKeyProvider`, behind the key-provider port; the board's provider is the OP-TEE TA of Story 6.6.)

### Holder, storage and rotation, per key and root

Key kind and root in the first two columns. Production incident handling (compromise of any production key) is the Story 6.10 plan.

| Key | Root | Holder | Storage | Rotation |
|---|---|---|---|---|
| Boot-image signing | dev | the firmware developer | a key directory outside the repository, read by path by the Yocto signing step; its public hash is fused only on sacrificial dev boards | regenerate on demand or when a machine is lost, then re-flash dev boards |
| Boot-image signing | factory-test | the factory-test signing host operator | key file on that host (0600, access-logged); not on developer laptops | regenerate per factory-test image release and re-issue the image |
| Boot-image signing | production | nobody: the key never leaves the HSM | offline HSM (backup only as an encrypted export on the second token) | does not rotate on shipped units (the fused OTP hash is permanent); revocation is anti-rollback, Story 6.9; ceremony in Story 6.10 |
| RAUC bundle signing | dev | the developer | key and certificate in a directory outside the repository | regenerate on demand; images carrying the old certificate are re-flashed |
| RAUC bundle signing | factory-test | the factory-test signing host operator | key file on that host (0600, access-logged) | regenerate per factory-test image release |
| RAUC bundle signing | production | nobody: HSM only | offline HSM | new certificate is shipped in an update signed by the old one (overlap window), then the old is removed |
| Content-manifest signing | dev | the developer | `content-manifest.ed25519.seed` (raw 32-byte seed) in the dev key directory | regenerate on demand; update `trust/dev.pub-list` or pass `--dev-keys-dir`; re-sign dev packs |
| Content-manifest signing | factory-test | the factory-test signing host operator | key file on that host (0600) | per factory-test release; the trust list may hold old and new keys during the overlap |
| Content-manifest signing | production | nobody: HSM only | offline HSM; the content-pack tool (6.3) signs on the offline signing laptop | add the new public key to `trust/production.pub-list` and ship it in a signed update while the old key still verifies, then remove the old key |
| Content-key vault KEK | dev | the developer | `kek.aes256` (raw 32 bytes) in the dev key directory | regenerate on demand with `--force`; dev vault entries under the old KEK become unreadable |
| Content-key vault KEK | factory-test | the factory-test provisioning station operator | the station's secret store | per factory-test release; re-wrap every vault entry under the new KEK |
| Content-key vault KEK | production | nobody: HSM only; unwrap happens only inside the HSM-backed vault service | generated in the HSM; the vault stores content keys only wrapped under it | re-wrap every vault entry under the new KEK, keeping the old KEK on the second token until the vault is verified |

**What Story 6.1 generates.** `velora_keys.py` generates only the dev content-manifest key, the dev KEK and the dev device key pair. The dev boot-image
signing key is created by Story 1.6 (secure-boot chain) and the dev and factory-test RAUC keys and keyrings by Story 6.13 (image variants); this story
defines their place in the hierarchy but does not create them.

A trust list may hold several public keys of its root so a rotation overlap needs no code change.

## 3. Which images trust which public keys

| Image variant | Boot root | RAUC keyring | Content-manifest keys embedded | Notes |
|---|---|---|---|---|
| dev | dev | dev | dev only | SSH and debug UART; unfused or dev-fused boards |
| factory-test | factory-test | factory-test | factory-test only | line-test image, signed |
| production | production | production | production only | secure boot, no SSH, no console |

Consequences that are enforced by the build (section 4) or by the later stories:

* a dev-signed bundle or pack does not verify on a factory-test or production image, and the reverse;
* a production image cannot be built with a dev public key inside it, and cannot be built before a production key list exists;
* a unit fused with the production root never boots a dev or factory-test image (Story 6.9).

## 4. Trust lists and embedding

`tools/keys/trust/<root>.pub-list` holds **public keys only** and is committed:

```
<root> <role> <key_id hex16> <public_key hex64>      # '#' starts a comment
```

* `role` is `content-manifest` (Ed25519 public key, 32 bytes).
* `key_id` is the first 8 bytes of SHA-256(public key), in lowercase hex. The parser rejects an entry whose key id does not match.
* `production.pub-list` is **empty** until the Story 6.10 ceremony publishes the production public key.

`velora_embed_trusted_keys(<variant>)` (`TrustedKeys.cmake`, wrapping `embed_trusted_keys.py`) generates `velora_trusted_keys_<variant>.hpp` with
namespace `velora::keys::<variant>` (`factory_test` for factory-test) holding `kTrustedRoot`, `kIsProduction`, `kTrustedContentKeyCount` and `kTrustedContentKeys` (id and public key per entry), so several variants can be included together. The step **fails** (configure-time `FATAL_ERROR`, script exit 1,
nothing written) when:

* a dev public key would be embedded in a production variant (a `dev` entry in the production list, or `--dev-keys-dir`/`DEV_KEYS_DIR` on a non-dev variant);
* a production variant has no production key list or an empty one;
* a variant would embed a key of another root, or one public key appears under two roots, or a key id does not match its public key;
* a public key is not a canonical, on-curve, non-small-order Ed25519 point;
* for factory-test and production, a key appears in the committed `trust/dev.pub-list` or in any `tests/fixtures/*.pub-list` (or a `--deny-dir`): relabelling a dev or fixture key fails even with a custom `--trust-dir`;
* a local `--dev-keys-dir` key is listed under another root.

A refusal also deletes any existing output header.

`--dev-keys-dir` / `DEV_KEYS_DIR` lets a dev build also trust the key a developer generated locally, without committing it.

## 5. Dev key files

Written by `velora_keys.py generate` (default directory `$XDG_DATA_HOME/velora-dev-keys`, else `~/.velora-dev-keys`; the tool refuses any directory
inside a git work tree and refuses to overwrite without `--force`). Directory mode 0700, files 0600. Each key has a JSON sidecar with `"root": "dev"`
and `"dev_only": true`; `DEV-ONLY.txt` labels the directory. Sidecars never contain private material.

| File | Contents |
|---|---|
| `content-manifest.ed25519.seed` | the 32-byte Ed25519 **seed** of RFC 8032 (not a libsodium 64-byte secret key) |
| `content-manifest.ed25519.pub` | raw 32-byte Ed25519 public key |
| `kek.aes256` | raw 32-byte dev KEK |
| `device.x25519.priv`, `device.x25519.pub` | raw 32-byte X25519 scalar, stored **unclamped** (clamping happens inside X25519), and public key (RFC 7748) |
| `*.json` | sidecar: root, dev_only, role, algorithm, key_id, public key |

Key id of a key pair = first 8 bytes of SHA-256(public key). The KEK has no public half; its id is the first 8 bytes of SHA-256("velora-kek-id:" || KEK).

`tools/ci/check_no_private_keys.py` fails on PEM private-key blocks, files named like the raw private files above (`*.seed`, `*.priv`, `*.kek`,
`*.aes256`, `*.p12`, `*.pfx`, `*.jks`, `*.key`, `*.pem`, `*.der`, `*.pkcs8`, `*.gpg`, `id_rsa`, `id_ed25519`), anything under a `velora-dev-keys` / `.velora-dev-keys`
directory (any case), the `DEV-ONLY` marker text and `"dev_only": true` sidecars, 32-byte files named like a seed/private key/KEK, and `*.json` files carrying a
base64 or hex 32-byte private value (including JWK `d` and values inside lists), in the git index and the working tree. Reviewed exceptions go in `tools/ci/private_key_allowlist.txt`. It runs as ctest `no_private_keys`, in `tools/ci/docker-check.sh` and in the CI purity job.

## 6. Wrap formats (normative, fixed here for Stories 6.3, 6.5, 6.8)

All keys are raw bytes. `library_id` is the vendor library id as UTF-8, no terminator: the canonical lower-case 8-4-4-4-12 UUID text (36 bytes) for real libraries (`tools/content-pack`, Story 6.3, implements both formats against the vectors below, `tools/content-pack/tests/content_pack_test.cpp`). "||" is concatenation. Implementations MUST reproduce the pinned
vectors below; `tests/test_keys.py` checks them with a pure-Python AES-GCM/HKDF (`tests/wrap_ref.py`, itself checked against NIST GCM cases 13 and 14,
FIPS 197 and RFC 5869 case 1).

### `wrapped_key_v1` (content key wrapped for one device; stored next to the bundle, Stories 6.3, 6.5, 6.8)

```
offset  size  field
0       4     magic "VWK1"
4       1     alg = 0x01  (ephemeral X25519 + HKDF-SHA256 + AES-256-GCM)
5       8     recipient key id (key id of the device X25519 public key)
13      32    ephemeral X25519 public key
45      12    GCM nonce (random, fresh per wrap)
57      32    ciphertext of the 32-byte content key
89      16    GCM tag
total 105 bytes
```

* Wrap: `shared = X25519(ephemeral_private, device_public)`. Unwrap: `shared = X25519(device_private, ephemeral_public)`. An all-zero shared secret MUST be rejected.
* `wrapping_key = HKDF-SHA256(IKM = shared, salt = ephemeral_public || device_public, info = "velora/wrapped_key_v1/" || library_id, L = 32)` (RFC 5869).
* `AES-256-GCM(key = wrapping_key, nonce = blob[45:57], plaintext = content key, AAD = blob[0:45] || library_id)`, tag length 16, appended after the ciphertext.
* The ephemeral key is new for every wrap. A reader MUST check the magic, alg, total length 105 and the recipient key id before unwrapping.

The blob is bound to the device (recipient id and wrapping key), the library and the header. **Limitation:** there is no sender authentication: anyone who knows
a device public key can create a blob that unwraps to a key of their choosing. That can only make a library fail (denial of service); content authenticity
comes from the manifest signature and block hashes (Story 6.2), not from this blob.

Pinned vector (device private = bytes 0x01..0x20, ephemeral private = bytes 0x21..0x40, nonce = bytes 0xa0..0xab, content key = bytes 0xc0..0xdf,
library_id = `velora.test.library`):

| Item | Hex |
|---|---|
| device public | `07a37cbc142093c8b755dc1b10e86cb426374ad16aa853ed0bdfc0b2b86d1c7c` |
| shared secret | `a84dc7c3c8f058b1b2dc4cd1e9b5dc0a7987f88b6a9564cde3391fc421159e77` |
| wrapping key (HKDF output) | `e665ab101a53dad94fd3ac2902d09f512ab0e6400c7631f11b8938559eca08bf` |
| AAD | `56574b3101aaa8fff703b50b225869aff450549732cbaaed5e5df9b30a6da31cb0e5742bad5ad4a1a768f1a67b76656c6f72612e746573742e6c696272617279` |
| full blob (105 bytes) | `56574b3101aaa8fff703b50b225869aff450549732cbaaed5e5df9b30a6da31cb0e5742bad5ad4a1a768f1a67ba0a1a2a3a4a5a6a7a8a9aaab9eb9ebbe5dd3cc7205c0854c53b12f3a98da5488b008bb9b068e9230c82bb4809e6004e870a73f77e35234a13d20ce3a` |

### `vault_entry_v1` (content key at rest in the vendor vault, Story 6.8)

```
0   4   magic "VKV1"
4   8   KEK key id  (first 8 bytes of SHA-256("velora-kek-id:" || KEK))
12  12  GCM nonce (random)
24  32  ciphertext of the 32-byte content key
56  16  GCM tag
total 72 bytes
```

`AES-256-GCM(key = KEK, nonce = entry[12:24], plaintext = content key, AAD = entry[0:12] || "velora/vault_entry_v1/" || library_id)`, tag length 16.

**Known limitation:** the entry carries no key version or generation counter, so the vault cannot detect an old entry being replayed after a KEK rotation by
format alone; rotation is a vault-service procedure (re-wrap everything, then drop the old KEK) and a version field is a candidate for `vault_entry_v2`.

Pinned vector (KEK = bytes 0x40..0x5f, nonce = bytes 0xb0..0xbb, content key and library_id as above):

| Item | Hex |
|---|---|
| KEK key id | `9b660e663d66688a` |
| AAD | `564b56319b660e663d66688a76656c6f72612f7661756c745f656e7472795f76312f76656c6f72612e746573742e6c696272617279` |
| full entry (72 bytes) | `564b56319b660e663d66688ab0b1b2b3b4b5b6b7b8b9babbc3c14246058f2d5ef327ed33213b82046ed956fb90c000482b1fbae0556d2c4fee9f6c8b2270d1bca7a76c2f9658b9c4` |

### Vectors with a UUID-text `library_id`

Same keys, nonces and content key as above, `library_id` = `3f1d6a52-9b7e-4c1a-8d2e-5a6b7c8d9e0f` (generated by `tests/wrap_ref.py`; pinned in `tests/test_keys.py` and in `tools/content-pack/tests/content_pack_test.cpp`):

| Item | Hex |
|---|---|
| `wrapped_key_v1` blob | `56574b3101aaa8fff703b50b225869aff450549732cbaaed5e5df9b30a6da31cb0e5742bad5ad4a1a768f1a67ba0a1a2a3a4a5a6a7a8a9aaabb27952d3c6f1222c0cfd438d34d043034a13df431b860479b07ba77078486cc141bd3487091063b292da3d3b6643ee5d` |
| `vault_entry_v1` | `564b56319b660e663d66688ab0b1b2b3b4b5b6b7b8b9babbc3c14246058f2d5ef327ed33213b82046ed956fb90c000482b1fbae0556d2c4fe2b80d5f28f43d84b73e7b1212714196` |

### Key files

The Ed25519 file `*.ed25519.seed` is the 32-byte RFC 8032 **seed**, not a libsodium 64-byte secret key (seed || public); a library that wants the latter
must expand it. The X25519 file holds the **unclamped** 32-byte scalar; clamping is part of X25519 (RFC 7748) and must not be applied when storing.

### Signatures

Content manifests are signed with pure Ed25519 (RFC 8032, no prehash). The signing key id (8 bytes) travels beside the signature so a reader picks
the right entry of its trust list. Message layout and domain separation of the manifest are defined by Story 6.2 (`formatlib/README.md`, "Protected bundles"): the signed statement starts with the ASCII context
string `velora/protected-manifest/v1` and a 0x00 byte.

**Pack revision (Story 6.4).** The detached seal of a pack's `catalog.meta` (`catalog.meta.sig`, `formatlib/README.md`) carries a signed, monotonic `pack_revision` per library. A device installs a revision not lower than the installed one (the same revision only for the same bundle), so an older signed bundle cannot be replayed over a newer one; a deliberate rollback is a new, higher revision with the old content.

**Verifier strictness (Stories 6.2 and 6.3 MUST implement):** reject a signature whose `S >= L`; reject non-canonical encodings of `R` and of the public key
`A` (y >= p); reject a small-order `A` (the build already refuses such trusted keys); verify cofactorless, i.e. check `[S]B == R + [k]A` exactly, not with
the cofactor multiplied through. A signature check that passes with a different strictness must be treated as a failure.

Cross-check for implementers (RFC 8032 section 7.1, test 2): seed `4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb`, message `72`,
signature `92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00`.
`velora_keys.py sign` produces signatures with the dev key for this purpose.

## 7. Open items owned by later stories

* Boot-key algorithm and the RAUC certificate profile: Stories 1.6, 6.11.
* Fusing the production root hash and anti-rollback: Story 6.9.
* HSM generation, backups and the written ceremony: Story 6.10.
* Factory-test root creation and the factory-test image: Stories 6.12, 6.13.
