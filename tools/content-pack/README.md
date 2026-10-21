# tools/content-pack

Role: the offline tool that turns a source library (SFZ + WAV, or an SF2) into an encrypted, signed protected bundle with a vendor `library_id`, and keeps
the content key only in the key vault format (Story 6.3: AR31, FR30, FR31, FR34 packing, FR24 user imports untouched).

Dependency rule: host tooling, **never shipped**. It links the encrypting and signing code (`velora_format_protected_writer`), the decrypting reader
(`velora_format_decrypt`, used to verify what it wrote), the librarian importer (`velora_librarian`) and OpenSSL libcrypto. Nothing in a device image, the
desktop build or the librarian may depend on it. `content_pack_flagged_by_no_decrypt_scan` (ctest) shows that `tools/ci/check_format_no_decrypt.py` flags
its cipher calls, while `format_no_decrypt` and `librarian_no_crypto` keep proving the shipped targets are clean.

The key-handling primitives (`vault_entry_v1`, `wrapped_key_v1`, X25519, AES-GCM, HKDF, secret hygiene) live in `tools/common/vault_crypto` (library `velora_vault_crypto`, moved here from `src/crypto.*` in Story 6.8 without behaviour change) and are shared with the device registry (`tools/registry`); this tool's tests still pin the vectors. `tools/ci/check_content_pack_not_linked.py` keeps both tools and the shared library out of every shipped target.

Story 6.9: key resolution and the root guard (`read_key_file`, `load_root_keys`) moved from `main.cpp` to `src/key_files.{hpp,cpp}` without behaviour change, so `tools/factory-release` resolves and checks the root's keys with the same code; `tools/factory-release` builds a whole factory release on `run_pack`.

Read `tools/keys/KEY-HIERARCHY.md` first (roots, keys, `vault_entry_v1`, `wrapped_key_v1`) and `formatlib/README.md` ("Protected bundles").

## Commands

```
velora-content-pack pack --description FILE --out DIR [--keys-dir DIR] [--signing-key FILE] [--kek FILE]
                         [--device-pub FILE]... [--reuse-vault-entry FILE | --content-key-file FILE] [--deterministic-vault]
                         [--register --owner NAME] [--register-file FILE] [--overwrite]
velora-content-pack register --description FILE --owner NAME [--register-file FILE] [--date YYYY-MM-DD]
velora-content-pack verify --bundle FILE --vault-entry FILE --kek FILE --trusted-pub FILE
velora-content-pack check-register [--register-file FILE]
```

* **Keys and the root guard.** `--keys-dir` holds the file names of `tools/keys` (`content-manifest.ed25519.seed`, `kek.aes256`); `--signing-key` / `--kek` override one file
  each. For root `dev` without any of them the dev key directory of `velora_keys.py` is used (`$XDG_DATA_HOME/velora-dev-keys`, else `~/.velora-dev-keys`);
  the other roots need an explicit location (production keys live in the offline signing laptop's HSM workflow, Story 6.10). A secret file must be a
  regular file of exactly 32 bytes and not readable by group or others. Root guard, per key file: for root `dev` a sidecar next to the key
  (`content-manifest.json` for the signing seed, `kek.json` for the KEK) is optional, but if present its `root` must be `dev` and its `key_id` the key's real id; for
  **any other root the sidecar is required** and must say that root, carry a `key_id` equal to the id computed from the key (Ed25519: first 8 bytes of SHA-256 of
  the public key; KEK: `kek_id`), and not be `dev_only`; a `DEV-ONLY.txt` marker in the key's directory is refused. A sidecar without a string `root` is `bad_key_file`,
  every other guard failure is `root_mismatch`. Independently of any sidecar, a non-dev root refuses a signing key whose id is on the dev trust list
  (`tools/keys/trust/dev.pub-list`, or `--dev-trust-list FILE`) or equals the developer's dev key (`content-manifest.ed25519.pub` in the default dev key directory), and
  a KEK equal to the default dev directory's `kek.aes256`: relabelling a dev key does not make it a factory-test or production key. `--signing-key` and `--kek` may name
  different directories; each is checked against its own sidecar.
* **No key in argv.** No option takes a key value; keys are read from files. `--device-pub` is a raw 32-byte X25519 public key file (repeatable).
* **Content key.** A fresh random 256-bit key from the OS CSPRNG (OpenSSL `RAND_bytes`) unless one is reused: `--reuse-vault-entry FILE` takes the key out of
  the `content-key.vault` of an earlier run (needs the same KEK; the clean way to rebuild a release), `--content-key-file FILE` reads a 0600 raw key file
  (**root `dev` only**; refused for factory-test and production; an all-zero key is refused everywhere). The key lives in locked (`mlock`, best effort), wiped memory; core dumps are disabled.
  It reaches the disk only as `vault_entry_v1` under the root's KEK and, per `--device-pub`, as `wrapped_key_v1`. The `library_id` used as the AAD text of both
  formats is the canonical lower-case UUID string (36 bytes).
* **Reproducibility.** Same source library content, same content key, same signing key: identical `bundle.vlrp` bytes (Ed25519 is deterministic and the AES-CTR
  nonce is derived from the content, `BundleWriter::derive_nonce`). Neither the bundle, `catalog.meta` (which records only the source file's name) nor `pack.json`
  (source file name only) contains the packer's directory layout, so the files are identical when the same sources sit at another path - provided the source
  file keeps its name (it is the instrument key and part of the catalog) and the library name is the same. The vault entry uses a random GCM nonce by default and
  so differs between runs; `--deterministic-vault` derives it from the key and library id (HMAC-SHA256, `vault_nonce_deterministic`). With
  `--reuse-vault-entry ... --deterministic-vault`, **every file of the output directory is identical between runs except `wrapped-key.*`**, whose ephemeral key and nonce are
  fresh on every wrap (KEY-HIERARCHY.md); both blobs unwrap to the same key. A release is verified by repeating that command and comparing all other files.
* **`--revision N`** (Story 6.4, default 1): the pack revision signed into `catalog.meta.sig` (it does not change `bundle.vlrp`). It must rise with every release of a library: a device installs a revision only if it is not lower than the installed one, and the same revision only for the same bundle. A deliberate vendor rollback is a NEW, higher revision carrying the old content. `pack.json` records `pack_revision`.
* **`--overwrite`** replaces an existing `<out>/<library_id>/` only when its `content-key.vault` opens with this KEK to the SAME content key as the new pack (a rebuild
  with `--reuse-vault-entry`); an existing pack with another key, or one whose vault entry does not open, is kept as `<out>/.old-<library_id>.<time>.<pid>` so the only
  copy of a key is never deleted by a rebuild (`pack.json` is unaffected; the CLI prints nothing special, look for `.old-*`).
* **`verify`** opens a protected bundle with the key from a vault entry (signature against the given public key, hash list, every block hash, decryption,
  per-sample PCM hashes).

## Pack description (JSON, `schema_version` 1)

```json
{
  "schema_version": 1,
  "library_id": "3f1d6a52-9b7e-4c1a-8d2e-5a6b7c8d9e0f",
  "name": "Vendor Grand Piano",
  "localized_names": { "pt-BR": "Piano de Cauda do Fabricante" },
  "source": "grand/grand.sfz",
  "root": "factory-test",
  "category": "piano",
  "instruments": [ { "key": "grand.sfz", "name": "Grand (close)", "category": "category.piano" } ]
}
```

| Field | Required | Meaning |
|---|---|---|
| `schema_version` | yes | the number `1` |
| `library_id` | yes | the vendor library id, a canonical UUID (8-4-4-4-12 hex, either case, stored lower case); not nil. Real ids are random v4 UUIDs registered in `vendor-ids` |
| `name` | yes | library display name (1-256 chars, no control characters); the register identifies a library by this name |
| `localized_names` | no | locale tag -> name; carried into `pack.json` only (the importer and catalog have no localized names yet) |
| `source` | yes | the `.sfz` or `.sf2` file, absolute or relative to the description file |
| `root` | yes | `dev`, `factory-test` or `production`: selects the key set and the register rules |
| `category` | yes | default category of every instrument: the Story 5.5 closed set, as a key (`category.piano`), slug (`piano`) or English name; `none` is refused |
| `instruments` | no | per-instrument overrides: `key` (the bundle's instrument key: the SFZ file name, or `bank:preset` for an SF2) with `name` and/or `category`. An unknown key is refused by the importer |

Unknown fields, duplicates, wrong types and bad values are `bad_description`.

## Import path

The tool calls `velora::librarian::import_file`, the same importer as a user import (same parser, same sample loading, same encoder, same bit-exact
verification against the source), with two additive `ImportOptions` that user imports never set: `library_id` (the vendor id replaces the path-derived one) and
`bundle_sink` (build-only mode: everything is built and verified in memory, nothing is staged, locked or written, and the writer is handed to the tool, which
finalizes it as a protected bundle). So no plaintext bundle, block or PCM is written anywhere (no staging directory, no temp file). Unsupported or damaged sources
are skipped exactly as for users (`import_skipped`, with the importer's reasons).

## Output

`<out>/<library_id>/` (written to a staging directory next to it, then renamed; an existing directory needs `--overwrite`):

| File | Content |
|---|---|
| `bundle.vlrp` | the protected bundle (format version 3): AES-256-CTR blocks, signed manifest |
| `content-key.vault` | `vault_entry_v1` (72 bytes), the content key under the root's KEK |
| `wrapped-key.<device key id>.bin` | `wrapped_key_v1` (105 bytes) per `--device-pub` |
| `catalog.meta` | library name and per-instrument names and categories (the format the librarian's catalog reads) |
| `catalog.meta.sig` | Story 6.4: an 84-byte detached Ed25519 seal that binds `catalog.meta` and the **pack revision** to this `bundle_id` with the bundle's own signing key (`VCM2`, algorithm, revision, key id, signature over `"velora/catalog-meta/v1" 0x00 \|\| bundle_id \|\| SHA-256(catalog.meta) \|\| key id \|\| revision`); the librarian installs a pack only when it verifies |
| `pack.json` | report: ids, nonce, signer and KEK key ids, sizes, hashes, instruments. Never a key |

## Vendor ID register (`vendor-ids`)

A committed TAB-separated text file: `library_id`, `name`, `owner`, `date`. A pack is refused with `library_id_conflict` when its id is assigned to another
name or its name to another id, and with `library_id_unregistered` when the id is not listed. A new id is appended only by the explicit step: the `register`
command, or `pack --register --owner NAME`, which appends **after** the pack is published (a failed pack does not take the id). `pack` takes the exclusive lock on
`vendor-ids.lock` (opened without following symlinks) **before it checks the register and holds it until the pack is published and the id appended**, so two packs of
one id cannot both pass the check: the second waits, then sees the first's entry and fails with `library_id_conflict` (or is a no-op repeat when it is the same
library). If the append fails (register unwritable) the freshly published pack directory is withdrawn again (and a replaced previous one restored) and the error
is the register's. The file is rewritten atomically (temp file, fsync, rename, fsync of its directory).
**Identity is the name plus the id; there is no source column**: the register binds an id to one library name and a name to one id, it does not bind either to a
source tree or file, so two different sources packed under the same registered id and name are not told apart by the register. Names compare trimmed,
whitespace-collapsed and ASCII case-folded (`Vendor  PIANO ` is `vendor piano`); names (in descriptions and the register) may not contain invisible or
bidirectional-control characters (zero-width, BOM, soft hyphen, U+202A-202E ...); Unicode confusables (look-alike letters) are not folded. Only random (version 4) ids can
be registered and the dev range cannot; a register file listing either is rejected. The range `00000000-0000-4000-8000-xxxxxxxxxxxx` is reserved for dev and tests:
root `dev` may use it without an entry, other roots refuse it, and it is never registered. The shipped register holds only that documented rule.
`--register-file` selects another register (the tests use temp files).

## Exit codes

| Code | Name | Meaning |
|---|---|---|
| 0 | ok | |
| 2 | usage | bad command line (also: a key given in argv, `--content-key-file` for production) |
| 3 | bad_description | description unreadable or invalid |
| 4 | bad_key_file | key file missing, wrong size, too-open permissions, an all-zero content key, or a sidecar without a string `root` |
| 5 | root_mismatch | the keys belong to another root than the description |
| 6 | library_id_conflict | the id or the name is assigned to another library in the register (also: registering a dev-range id) |
| 7 | library_id_unregistered | the id is not in the register (or is a dev-range id outside root dev) |
| 8 | register_io | register missing (for a non-dev id), unreadable, malformed or not writable; also a bad `--owner` / `--date` of `register` |
| 9 | import_skipped | the source was skipped by the importer (unsupported features or damaged) |
| 10 | import_failed | the importer failed (I/O, verification, invalid argument such as an unknown instrument key) |
| 11 | bundle_failed | the protected bundle could not be built or did not verify |
| 12 | vault_failed | the vault entry or a wrapped key could not be made or opened (also `--reuse-vault-entry` with another KEK or library) |
| 13 | output_failed | output exists without `--overwrite`, or cannot be written |
| 14 | verify_failed | `verify`: signature, hash, key or content check failed |
| 70 | internal | |

Errors go to stderr as `error: <name>: <message>`.

## Tests (ctest `content_pack_test`, hermetic: temp dirs, random keys, no network)

* the pinned `wrapped_key_v1` and `vault_entry_v1` vectors of KEY-HIERARCHY.md, plus the UUID-text `library_id` vectors generated by `tools/keys/tests/wrap_ref.py` (also pinned
  in `tools/keys/tests/test_keys.py`), a flipped bit anywhere, wrong library/KEK/device, an all-zero shared secret;
* the description schema, the register (conflict both ways, append, no-op repeat, dev range, malformed files, the committed file);
* a generated WAV+SFZ library (16-bit stereo, 24-bit mono, 16-bit mono) and a generated SF2: pack, decrypt with the vaulted key (unwrapped via the dev KEK and,
  separately, via the dev device key) and compare the PCM with the source bit for bit;
* no plaintext on disk, checked independently of the tool's own write monitor: `TMPDIR` of the test and the CLI points into the scratch directory; its file list and
  sizes are snapshotted around every `run_pack` and CLI `pack` (SFZ packs, the SF2 pack, the reproducibility packs, the CLI packs), only the pack's own files
  may appear, and every new file is scanned for the content key (raw and hex), the source's raw PCM windows and windows of the unprotected bundle's blocks; the
  stdout/stderr of the CLI never carries the key. The scan is shown to hit a plain bundle copy (block windows), the source WAV (PCM windows; the plain bundle does not
  contain raw PCM, its blocks are FLAC coded) and a planted key;
* the root guard (bare key under factory-test, mislabelled `key_id`, mixed `--signing-key`/`--kek` directories, `DEV-ONLY.txt`, sidecar without a string root,
  `dev_only`, a dev key relabelled as factory-test via the dev trust list and via the dev key directory), key resolution (`--signing-key`/`--kek` overrides, the
  default dev directory), `core-limit` (the process runs with RLIMIT_CORE 0);
* the register under concurrency: two processes packing one id under two names (one wins, the other exits 6 and publishes nothing), a failed append withdraws the pack;
* reproducibility (API and CLI: every file but `wrapped-key.*` identical between two runs and between two source locations with a reused key and `--deterministic-vault`; different
  bytes with a fresh key; the overwrite rules incl. keeping an old pack with another key);
* render: **Story 6.5 closes the substitute of this story.** The pack's `bundle.vlrp` is rendered STRAIGHT by `velora-offline-render --bundle bundle.vlrp --dev-device-key <dev device key>
  --wrapped-key <the pack's wrapped-key blob> --trust-list <the signer's pub-list>` (the engine's protected path: signature verified, key unwrapped by the dev key provider into locked
  memory, blocks hash-checked and decrypted in the renderer's process) and the WAV is byte-identical to the render of a normal published import of the same library, with the scratch
  tree gaining only that WAV (no decrypted block, no key on any disk); another device's key refuses it with `key_unavailable`. The Story 6.3 evidence stays as the cross-check that does
  not use the engine's reader: the library decrypted with the vaulted key and rebuilt without protection is byte-identical to the unprotected bundle the importer itself makes for the
  same vendor id (its own `finalize()` and, on disk, a normal published import with that `library_id`), and `velora-offline-render` on a short MIDI suite renders both to identical WAVs
  (not silent); a negative control with one altered PCM value (still a valid bundle) renders to a different WAV. SF2 sources are covered by the decrypt/PCM tests, not by a render
  comparison. The general protected-vs-unprotected render test (MIDI suite, both interpolations, Live Set path, typed refusals) is `tools/offline-render/tests/protected_render_test.cpp`;
* the importer's additive options (a vendor `library_id` also decides the `current`/Import-again lookup of a published SF2 import; build-only SF2 import never checkpoints and
  writes nothing even with a non-empty root) and the CLI's exit codes, including `verify` on a tampered bundle, an untrusted signer, a wrong KEK and missing files;
* ctest `content_pack_not_linked` (`tools/ci/check_content_pack_not_linked.py`): no CMake file outside `tools/content-pack` links `velora_content_pack`.

## Known limits

* The whole library is held in memory while it is packed (the importer's finalize is not streaming); fine for the factory library on a build machine.
* `localized_names` are not yet used by the catalog. A default `category` is mandatory so every instrument has one.
* A packer that can run two instances against one register must use a local file system (flock).
* `mlock` is best effort (a low `RLIMIT_MEMLOCK` does not stop the tool); swap and hibernation images of the signing machine are out of the tool's reach.
* The CLI does not rotate keys or re-wrap vault entries (Story 6.8).
