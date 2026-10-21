# tools/factory-release

Role: the offline pipeline that turns the founder's factory library (SFZ/WAV or SF2 sources) and the factory Live Sets into **one packed, signed, versioned, reproducible release**: every
library as a protected bundle under the root's keys, one signed Live Set file with vendor-assigned program ids, a signed release index with the hash of every file, and the vaulted content
keys for the registry (Story 6.9: FR34, AR31 in use, EXT14 founder-supplied content).

Dependency rule: host tooling, **never shipped**, like `tools/content-pack` (it links `velora_content_pack`, the protected writer for Ed25519 signing, and `velora_control` for the Live Set proto and the
factory Live Set container). Nothing in a device image, the desktop build or the librarian may depend on it: `tools/ci/check_content_pack_not_linked.py` guards `velora_factory_release` as it guards
`velora_content_pack`, and the ctest `factory_release_flagged_by_no_decrypt_scan` shows the shipped-binary scanner would flag it.

Read `tools/content-pack/README.md` first (the pack tool, keys and the root guard, the vendor ID register), `tools/keys/KEY-HIERARCHY.md` (roots, `vault_entry_v1`, `wrapped_key_v1`) and
`engine/host/README.md` ("Factory Live Sets": the signed file and how the engine installs it).

## Commands

```
velora-factory-release build  --description FILE --out DIR [--keys-dir DIR] [--signing-key FILE] [--kek FILE] [--reuse-vaults DIR] [--allow-new-keys]
                              [--previous-index FILE] [--register --owner NAME] [--register-file FILE] [--date YYYY-MM-DD] [--dev-trust-list FILE] [--overwrite]
velora-factory-release check  --description FILE
velora-factory-release verify RELEASE_DIR (--trust-list FILE | --trusted-pub FILE) [--bundles]
```

* **`build`** packs every library with content-pack's `run_pack` (the very same code path as `velora-content-pack pack`, in process), then builds the Live Sets, the index and its signature, and publishes
  `<out>/<version>/` (staged next to it in `.release-<version>.<pid>` and renamed, so a failed build leaves nothing). Keys, the root guard and the register behave exactly as in content-pack
  (`--keys-dir`, `--signing-key`, `--kek`, `--dev-trust-list`, `--register-file`; the code is shared: `tools/content-pack/src/key_files.cpp`). No option takes a key value. `--register --owner NAME` appends the
  new ids of the release to the register **only in a final commit step**, after every pack, Live Set, reference and the index are validated and written (the packs run against a staged copy of the register; a build that
  fails earlier leaves the register untouched; a dev-range id is never registered). Before anything is packed, the Live Sets are checked against the parameter registry (`proto/params.json`: an unknown parameter id, a
  value out of range or a fraction for an integer parameter is `invalid_live_set`) and the v1 part count. The signing key and the KEK are the root's: the bundles, the catalog seals, the Live Set file and the
  index are all signed by the same content-manifest key, so a unit needs exactly one trusted key per root.
* **`--reuse-vaults DIR`** is how a release is rebuilt: `DIR` is the `registry-vault/` of an earlier build; a library found there keeps its content key (units already hold wrapped keys for it). A directory that does
  not exist, or a library without an entry in it, is `vault_failed` unless `--allow-new-keys` says a new library may get a fresh random key. Without `--reuse-vaults` every library has a new key: that is the first
  release, and a rebuild without the vaults is a *different* release (documented below under Reproducibility).
* **`--overwrite`** replaces an existing `<out>/<version>/` only when the new index is byte-identical to the old one (same sources, keys, vaults: nothing to replace but the files); a build whose index would differ under
  the same version is refused with `output_failed`: raise `release.version`. The only copy of a content key is thus never replaced by a rebuild.
* **`--previous-index FILE`** (the `release-index.json` of the previous release) enforces the release history: a program id of the previous release that is missing now is `program_history` (program ids are never
  removed or reused); a program whose content changed (hash of the entry without the release version, in the index as `sha256`) needs a higher `release.version`; a library whose bundle changed needs a higher
  `revision`; a release older than the previous one is refused.
* **`check`** parses and validates the description only (no sources are read).
* **`verify`** checks a release directory against its index (below); `--bundles` additionally re-verifies each bundle with
  `velora_format_verify` (signature, hash list, every ciphertext block) and each `catalog.meta` seal. A trusted key is mandatory (`--trust-list` in the `tools/keys/trust/<root>.pub-list` format, or
  `--trusted-pub` a raw 32-byte Ed25519 key): the index names its own signer, which proves nothing by itself. A trust-list line carries a **root** (`dev`, `factory-test`, `production`); a signer listed for one
  root does not vouch for a release (or a factory Live Set file) of another (`index_root_mismatch`, `live_sets_invalid`/`root_mismatch`): a dev key claiming production is refused. `--trusted-pub` has no root and binds nothing.

## Release description (JSON, `schema_version` 1)

```json
{
  "schema_version": 1,
  "release": { "name": "Velora Factory Library", "version": "1.0.0" },
  "root": "factory-test",
  "libraries": [
    { "library_id": "3f1d6a52-9b7e-4c1a-8d2e-5a6b7c8d9e0f", "name": "Velora Factory Piano", "localized_names": { "pt-BR": "Piano de Fabrica" },
      "source": "piano/Factory_Grand.sfz", "category": "piano", "revision": 1 },
    { "library_id": "...", "name": "Velora Factory Keys", "source": "keys/Factory_Keys.sf2", "category": "keys", "revision": 1,
      "instruments": [ { "key": "0:0", "name": "Factory E.Piano", "category": "epiano" } ] }
  ],
  "live_sets": {
    "file": "factory-live-sets.vfls",
    "entries": [
      { "program_id": 1, "name": "Factory Grand", "tempo": 120,
        "parts": [ { "part": 0, "timbre": { "library_id": "3f1d6a52-...", "instrument_key": "Factory_Grand.sfz" }, "volume": 1.0 } ] },
      { "program_id": 2, "name": "Gospel Organ",
        "parts": [ { "part": 0, "engine": { "type": "organ", "params": [ { "param_id": 100, "value": 8 }, { "param_id": 101, "value": 8 }, { "param_id": 102, "value": 8 } ] } } ],
        "master": [ { "param_id": 520, "value": -3.0 } ] }
    ]
  }
}
```

| Field | Required | Meaning |
|---|---|---|
| `schema_version` | yes | the number `1` |
| `release.name` / `release.version` | yes | display name (1-128 chars) and **`MAJOR.MINOR.PATCH`** (decimal, no leading zeros). The release goes to `<out>/<version>/`; a Live Set install refuses a release older than the one installed |
| `root` | yes | `dev`, `factory-test` or `production`: selects the key set, the register rules and the trust list a unit needs. Every library is packed under this root (the content-pack root guard applies) |
| `libraries[]` | yes, 1..256 | one entry per vendor library: a content-pack description (`library_id`, `name`, `localized_names`, `source`, `category`, `instruments` as in `tools/content-pack/README.md`; `root` is the release's) plus `revision` |
| `libraries[].source` | yes | the `.sfz` / `.sf2`, absolute or **relative to the description file** |
| `libraries[].category` | yes | the default category of every instrument (the Story 5.5 closed set; `none` is refused). A library or instrument without an assignable category is `missing_category` |
| `libraries[].revision` | no (1) | the pack revision signed into `catalog.meta.sig`; **must rise with every release of a library** (a unit installs a revision only if it is not lower than the installed one) |
| `live_sets` | no | the factory Live Sets; `file` (default `factory-live-sets.vfls`, `[A-Za-z0-9._-]`, ends in `.vfls`) and `entries` (1..256) |
| `entries[].program_id` | yes | the **vendor-assigned program id**: a number >= 1, unique in the release (and across releases: it is the key of the first-boot import) |
| `entries[].name` / `tempo` | name yes | Live Set name (1..64 bytes) and tempo (default 120) |
| `entries[].parts[]` | yes | `part` 0..7 (v1: 8 parts; unlisted parts stay default and empty), `timbre` `{library_id, instrument_key}` **or** `engine` `{type: "sample"\|"organ"\|id, params: [{param_id, value}]}`, and optionally `low_key`, `high_key`, `octave`, `transpose`, `keyboard_on`, `volume`, `send_reverb`, `send_delay`, `output` (`main`\|`aux`). A part names an instrument **only** by `(library_id, instrument_key)`, the library must be one of the release's, and the key must be an instrument the packed library has (`unresolved_reference` otherwise) |
| `entries[].master[]` | no | master-scope registry params |

Unknown fields, duplicates, wrong types and bad values are `bad_description`; a library id or normalised name listed twice is `duplicate_library`; a program id of 0, invalid or used twice is
`duplicate_program_id`. Each Live Set is validated with the engine's own `liveset::validate` for the v1 tier (`invalid_live_set`).

## Output (`<out>/<version>/`)

```
release-index.json                  the index (below), deterministic JSON
release-index.sig                   80-byte detached Ed25519 signature of the index
unit/                               everything that goes onto a unit's image
  factory-live-sets.vfls            the signed factory Live Sets (engine/host/README.md, "Factory Live Sets")
  libraries/<library_id>/           bundle.vlrp, catalog.meta, catalog.meta.sig, pack.json   (as `librarian install-protected` reads a pack, minus the wrapped key)
registry-vault/                     for the REGISTRY only, never for a unit image or the desktop
  <library_id>/content-key.vault    vault_entry_v1 (72 bytes), the content key under the root's KEK; mode 0600
```

A unit's wrapped key is **not** part of the release: the content key is wrapped to each unit's device key at provisioning (the registry unwraps the vaulted key with the KEK inside the wrap operation, Story 6.8). To
install on a dev machine, wrap the vaulted key to a developer's device key and pass it to `librarian install-protected <unit/libraries/ID> --wrapped-key FILE` (`factory_release_test` does exactly that with the registry's
primitives).

**`release-index.json`** (key order and layout fixed, no timestamps): `schema` (`velora.factory-release-index/1`), `tool`, `release` `{name, version, root}`, `signer_key_id`, `kek_id`, `libraries[]`
(`library_id`, `name`, `revision`, `bundle_id`, `files` `{bundle, catalog_meta, catalog_meta_seal, report, vault}`, `instruments[]` `{key, name, category}`), `live_sets` (`null`, or `file` and the **program id table**
`programs[]` `{program_id, name, live_set_id, sha256}`) and `files[]`: **every other file** of the release, sorted by path, with `size` and `sha256` (vault entries included).

**`release-index.sig`**: `"VRIS"` | version `u8` = 1 | algorithm `u8` = 1 (Ed25519) | reserved `u16` = 0 | signer key id `[8]` | signature `[64]`. The signature covers
`"velora/factory-release-index/v1" 0x00 || signer key id (8) || SHA-256(release-index.json bytes)`; the signer is the root's content-manifest key, the same one that signed the bundles and the Live Set file.

**`verify`** reports every problem (at most 200) with a stable name and exits 14: `index_missing`, `index_sig_malformed`, `index_untrusted_key`, `index_signature_invalid`, `index_malformed`, `bad_path`,
`file_missing`, `file_not_regular`, `file_unreadable`, `size_mismatch`, `hash_mismatch`, `unexpected_file`, `unexpected_entry` (a symlink, FIFO, ...), `library_file_not_in_index`; with `--bundles`: `bundle_invalid`,
`bundle_block_invalid`, `bundle_identity_mismatch`, `bundle_instruments_mismatch`, `bundle_meta_unreadable`, `bundle_meta_seal_invalid`, `bundle_revision_mismatch`; always for the Live Sets: `live_sets_invalid`
(untrusted signer, bad signature, a different release, a program id table that differs from the index, a reference to a library outside the release), `live_sets_index_malformed`, `live_sets_unreadable`; and the layout invariants `layout_invalid` (every path is under `unit/` or `registry-vault/`; a library's files are exactly `unit/libraries/<library_id>/{bundle.vlrp, catalog.meta, catalog.meta.sig, pack.json}` and
`registry-vault/<library_id>/content-key.vault`; nothing named `content-key.vault` under `unit/`; the vault directory holds `<library_id>/content-key.vault` only). The hash
check cannot see a damaged bundle whose index was re-signed by the key holder; `--bundles` does.

## Reproducibility

Rebuilding from **the same sources, keys, revisions and the previous `registry-vault/`** (`--reuse-vaults`) gives **byte-identical** output: every bundle, catalog seal, report, vault entry, the signed Live Set file, the
index and its signature (Ed25519 and the content-derived AES-CTR nonce are deterministic; the vault nonce is derived with `--deterministic-vault`, which the release always uses; no clock or path enters any file; the
sources may sit anywhere provided file names are unchanged). `factory_release_test` checks this file by file, from another source location, and also that a fresh content key, another signing key and another revision
change exactly what they must. **Reading of the acceptance criterion "rebuilt from the same sources and keys":** *keys* includes the content keys, which live in the vaults, so a rebuild reuses them (`--reuse-vaults`); a rebuild with new random
content keys is a new release (different ciphertext, vault entries and index), not a failure to reproduce. Limit: encoded FLAC frames may differ between libFLAC versions, so reproduce a release with the same libFLAC
(the decoded PCM, the PCM hashes and `library_id` never change). The index SHA-256 of the synthetic test release is therefore **not** pinned here: it depends on the libFLAC build and on the random content keys of
the test, and a pin that is stable on one machine would break on another.

## Founder deliverable (what Story 11.17 must hand over)

```
<delivery>/
  release.json                       the release description (above); ids and program ids vendor-assigned and registered
  <library folder>/<name>.sfz        one folder per library; the .sfz (one instrument) or .sf2 (many presets) with its WAV samples, referenced by relative paths
  <library folder>/samples/*.wav     only the opcode subset of the Story 5 importer; a source the importer skips fails the build (import_skipped)
```

* one `library_id` per library (random v4 UUID, **registered** in `tools/content-pack/vendor-ids` with `--register --owner NAME` or `velora-content-pack register`), a distinct display name, a default category
  and, where an SF2 holds different categories, per-instrument overrides;
* the factory piano, the organ presets and the other categories of the v1 success signal as Live Sets: one `entries[]` per Live Set with its vendor-assigned `program_id` (never reused, never renumbered);
* WAV 16/24-bit PCM (the importer's formats), sample names without spaces in SFZ `sample=` paths; the synthetic tree of `tests/synthetic_factory.hpp` is the executable example of this layout;
* instrument keys are the SFZ file name (or `bank:preset`): they are the names Live Sets refer to, so renaming a file between releases breaks every Live Set that uses it.

## Release checklist

1. `velora-factory-release check --description release.json` (syntax, ids, program ids, categories).
2. Raise `release.version` and every changed library's `revision`; keep the same `library_id`s.
3. On the signing machine with the root's keys: `velora-factory-release build --description release.json --out releases --keys-dir <root keys> --reuse-vaults <previous registry-vault> [--register --owner NAME]`.
   Production keys come from the Story 6.10 ceremony; the dev root's keys never sign a factory-test or production release (the root guard refuses them).
   Record the libFLAC version (`flac --version` / the build log) with the release: it decides whether the encoded bundles can be reproduced byte for byte. Pass `--previous-index` (the last release's index) from the
   second release on.
4. `velora-factory-release verify releases/<version> --trust-list <root>.pub-list --bundles` on a second machine (or at least a second shell): ok.
5. Rebuild once more into another directory with `--reuse-vaults releases/<version>/registry-vault` and compare the two trees (`diff -r`): identical.
6. Hand `registry-vault/` to the registry (below). Put `unit/` on the provisioning image. Archive the whole release directory and the `release.json` that built it.
7. On a dev unit or the Mac: install each `unit/libraries/<id>` (`librarian install-protected`) and boot `engine-host` with `--factory-live-sets unit/factory-live-sets.vfls` (add `--factory-live-sets-required` to make a refused
   import fatal). **This flow exists in dev-key builds only** (`VELORA_DEV_KEY_PROVIDER`: the flag, `--trust-list` and the dev device key are not even in the binary of any other build). The production wiring (the image's
   embedded trusted keys, `factory-live-sets.vfls` on the image, the OP-TEE key provider) comes with the provisioning image (Epic 6 images, Story 6.11-6.13) and calls the same `install_factory_live_sets` API.

## Handing the vaults to the registry

`registry-vault/` has exactly the layout the registry serves from: `<library_id>/content-key.vault`, a 72-byte `vault_entry_v1` (the library's content key under the root's KEK), mode 0600. The registry's
`--vault-dir` (`velora-registry serve --vault-dir DIR ...`, `tools/registry/README.md`, "Vault") is that directory: copy the contents of `registry-vault/` into the registry's vault directory (entries are read on
every request, so no restart is needed) and keep the KEK with the registry (production: the HSM workflow of Story 6.10). The registry then wraps each key to a unit's device key at provisioning, inside the wrap
operation only. The vault directory is never part of the unit image, the desktop build or the librarian, is not part of the registry's database backup (back it up on its own, offline), and a later release keeps
the same keys through `--reuse-vaults`.

## Exit codes

Codes 2-14 are exactly those of `velora-content-pack` (a pack failure is passed through, so a vendor-id conflict is still 6).

| Code | Name | Meaning |
|---|---|---|
| 0 | ok | |
| 2 | usage | bad command line (also: a key given in argv) |
| 3 | bad_description | description unreadable or invalid |
| 4 | bad_key_file | key file missing, wrong size, too-open permissions, an unreadable trust list |
| 5 | root_mismatch | the keys belong to another root than the description |
| 6 | library_id_conflict | an id or a name is assigned to another library in the register |
| 7 | library_id_unregistered | an id is not in the register (or a dev-range id outside root dev) |
| 8 | register_io | register missing / unreadable / malformed / not writable |
| 9 | import_skipped | a source was skipped by the importer |
| 10 | import_failed | the importer failed |
| 11 | bundle_failed | a protected bundle (or a signature) could not be built or did not verify |
| 12 | vault_failed | a vault entry could not be made or opened (incl. `--reuse-vaults` with another KEK) |
| 13 | output_failed | the release exists without `--overwrite`, or cannot be written |
| 14 | verify_failed | `verify` found a problem (listed on stderr) |
| 20 | duplicate_library | the same library id or name twice in the release |
| 21 | duplicate_program_id | a program id of 0, malformed or used twice |
| 22 | missing_category | a library or an instrument without an assignable category |
| 23 | unresolved_reference | a factory Live Set names a library outside the release or an instrument the library does not have |
| 24 | invalid_live_set | a factory Live Set that does not validate for the v1 tier, or whose engine / master parameters are not in the registry or out of range |
| 25 | program_history | against `--previous-index`: a program id removed, content changed without a higher version / revision, an older release |
| 70 | internal | |

## Tests (ctest `factory_release_test`, `factory_live_set_tests`; hermetic: temp dirs, random keys, no network)

* `factory_release_test` on a deterministic **synthetic** tree (`tests/synthetic_factory.hpp`: three SFZ libraries, an SF2 with per-instrument overrides, four factory Live Sets: a piano, an organ by engine parameters,
  a layer, a split): the description (every refusal with its code); build (layout, nothing but the documented files, no key material under `unit/`, vault mode 0600, the index recomputed independently, the signature
  checked with the verify-only primitive, every bundle and seal re-verified); reproducibility (from another source location, via the CLI, `--overwrite`) and what must differ (fresh key, other signing key, new revision);
  the register (unregistered, id and name conflicts both ways, `--register`), root mismatch, unresolved references, invalid Live Sets, skipped sources, missing or foreign keys; the tamper sweep (a bit flipped in every
  kind of file, sizes, missing / extra / non-regular files, the index and every byte of its 80-byte signature file, another signer, a swapped bundle, an older Live Set file, and an index re-signed over a damaged bundle that
  only `--bundles` finds); the CLI and its exit codes; and, with the dev key provider, **the install on the Mac host**: every bundle installed with `librarian::install_protected` under the release root's key with a
  registry-style wrapped key, every factory instrument loaded and played by the engine's loader / resolver / dev key provider / decrypting head provider (non-silent), each instrument's category through the
  librarian service (`ListInstruments`), the first-boot import of the signed Live Sets through the `LiveSetStore`, every factory Live Set loaded with no missing library and sounding, the import idempotent, and a unit
  without the libraries refused with every missing `(library_id, instrument_key)`.
* `factory_live_set_tests` (`engine/host/tests`): the container byte layout and signature statement, a sweep of single-bit flips, the install rules (idempotence, user deletion, a user's file, release order,
  references, every rejection, crash recovery, the control thread), the derived Live Set ids pinned against an independent computation. `edit_session_tests` covers the read-only mark.
* `engine_host_factory_smoke` (the real engine-host: first-boot import, idempotence, deletion kept, refusals with the typed cause, `--factory-live-sets-required` fatal); `factory_release_test` also runs the real
  `librarian install-protected` and `engine-host --factory-live-sets` processes on the built release. The `--test-inject-note` hook only drives the librarian's "performing" state (no audio), so sound is proven in-process.
* `factory_release_test` returns 77 (ctest: skipped, `SKIP_RETURN_CODE`) when the dev key provider is not built and the install-and-play part cannot run.
* `content_pack_not_linked` (`tools/ci/check_content_pack_not_linked.py`) and `factory_release_flagged_by_no_decrypt_scan` (a missing `nm` fails under `CI`, see `check_format_no_decrypt.py`).

## Known limits

* The whole library is held in memory while it is packed (content-pack's limit); fine for a build machine.
* Only v1 Live Sets (8 parts) are authored; scenes, inserts and the arpeggiator are not part of the release description yet (the container and the installer already carry whole `LiveSet` messages, so adding
  them is a description change).
* The founder's real content is not in the repository (Story 11.17); the synthetic tree proves the pipeline, not the sound.
* The register commit is the last step before publishing; if publishing then fails, the ids already appended stay assigned (the register is append-only).
