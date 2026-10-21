# tools/registry

Role: the vendor **device registry and content-key vault** (Story 6.8: AR34 device registry, AR33 partial: registration from unit #1, serial and SoM link).
It records every unit (device ID, X25519 public key, SoM ID, serial, tier, provisioning date, station, status), and hands provisioning stations the content keys of
the libraries a unit may play, **wrapped to that unit's public key** (`wrapped_key_v1`). A repaired or re-provisioned unit asks again (Stories 6.16, 6.17 use this
API; they are not implemented here).

Dependency rule: **vendor server only, never shipped.** It links OpenSSL (TLS 1.3 and the cipher API), SQLite and the shared `velora_vault_crypto`
(`tools/common/vault_crypto`, the `vault_entry_v1` / `wrapped_key_v1` code that used to live in `tools/content-pack`, moved without behaviour change).
`tools/ci/check_content_pack_not_linked.py` (ctest `content_pack_not_linked`) fails when any CMake file outside `tools/content-pack`, `tools/registry` and
`tools/common/vault_crypto` names `velora_registry`, `velora_content_pack` or `velora_vault_crypto`, and when the two tools name each other's library.
`registry_flagged_by_no_decrypt_scan` shows the shipped-binary scanner would flag the registry's cipher calls.

Read `tools/keys/KEY-HIERARCHY.md` first (roots, KEK, `vault_entry_v1`, `wrapped_key_v1`). The HTTP API is in [API.md](API.md).

## Commands

```
velora-registry serve --db FILE --vault-dir DIR --kek FILE --server-cert FILE --server-key FILE --client-ca FILE
                      [--root dev|factory-test|production] [--expect-ca-sha256 HEX] [--expect-kek-id HEX16]
                      [--listen ADDR] [--port N] [--port-file FILE] [--workers N] [--rate-limit N] [--register-limit N]
                      [--max-body BYTES] [--max-conn-per-ip N] [--revoked-certs FILE] [--station-libraries FILE]
                      [--anchor-dir DIR --backup-key FILE] [--allow-no-mlock]
velora-registry verify-audit --db FILE [--anchor-dir DIR --backup-key FILE]
velora-registry backup  --db FILE --backup-key FILE --kek FILE --dest-dir DIR
velora-registry restore --backup FILE --backup-key FILE --db FILE [--force] [--anchor-dir DIR]
```

Exit codes: 0 ok, 2 usage, 4 bad key file (KEK or backup key: not a regular file of exactly 32 bytes, or readable by group/others; also a backup key equal to the KEK), 5 bad config (a pinned
CA fingerprint or KEK id that does not match, a non-dev root without its pins, a malformed `--station-libraries`), 6 database failed (cannot open, wrong schema version, or locked by another server),
7 audit chain failed (also: the database is behind or contradicts the newest anchor), 8 backup failed, 9 restore failed, 10 server failed (certificate, port, ...).

* `serve` runs until SIGINT / SIGTERM. **It verifies the whole audit chain, the audit schema and (with `--anchor-dir`) the newest anchor before it listens, and refuses to start (exit 7) if any of them fails.**
  It takes an exclusive `flock` on `<db>.lock`, so a second server, or a `restore` over the live file, is refused. It writes `service_started` (root, KEK id, CA fingerprint; never a key)
  and `service_stopped` rows. `--port 0` picks an ephemeral port (`--port-file` receives it). Core dumps are disabled and, on Linux, `PR_SET_DUMPABLE` is cleared.
* **`--root` is bound, not just named.** `--root factory-test` and `--root production` **require** `--expect-ca-sha256` (the SHA-256 of the DER of the client CA; every certificate in `--client-ca` must have it)
  and `--expect-kek-id` (the id the KEK file must have, `velora-content-pack` prints it as `kek_id`; `KEY-HIERARCHY.md` section 2): a dev KEK or the wrong CA bundle in a production service fails to start with exit 5.
  For `--root dev` both are optional.
* `verify-audit` walks the whole hash chain (below), the schema and, with `--anchor-dir`, the newest anchor. Exit 7 and a message naming the first bad entry on any failure.
* `backup` / `restore`: see "Backup".

## Vault

`--vault-dir` is the output directory of `velora-content-pack pack --out DIR`: `<DIR>/<library_id>/content-key.vault` (72-byte `vault_entry_v1`, mode 0600).
Entries are read on every request, so a library packed after the registry started is served without a restart. **The vault directory is NOT part of the database backup**
(`velora-registry backup` saves the device table and the audit log only): back it up separately (it holds only `vault_entry_v1` ciphertext under the KEK, which must be backed up separately again, offline).

The KEK is loaded from `--kek` (raw 32 bytes, **not readable by group or others**, otherwise exit 4) into **one dedicated, page-aligned, `mlock`ed anonymous mapping** (excluded from core dumps where the OS has
`MADV_DONTDUMP`) that holds nothing else but the 32-byte scratch space for the content key being wrapped; it is wiped and unmapped at exit. **If the page can not be locked in RAM the server does not start**
(raise `RLIMIT_MEMLOCK`, systemd `LimitMEMLOCK=`; `--allow-no-mlock` exists for development and tests only and prints a warning). The dedicated mapping matters because `munlock` does not nest: the shared
`Secret<T>` helper locks and unlocks whatever page its bytes sit on, so a long-lived key must not live on a page other locked data shares.

For each requested library the registry opens the entry **inside the wrap operation only** (`KeyVault::wrap`, serialised by a mutex because the scratch space is shared): the plaintext content key sits in the locked
scratch space for the length of one X25519/HKDF/AES-GCM wrap and is wiped before the call returns, on every path. Responses carry only `wrapped_key_v1` blobs. Neither the log, the audit table, the database nor a backup
ever holds a content key: the test scans every file the registry and the spawned CLI wrote, and the captured log, for every secret it created (content keys, KEK, backup key, device private keys) as raw bytes, hex
(lower and upper case), base64 and base64url (padded and unpadded; base64 in the three alignments it can take inside a longer string), and has a negative control that plants a key and requires the scan to find it.

## Data model

SQLite file (created 0600, `PRAGMA synchronous = FULL`, foreign keys on, schema version in `user_version`; a database of another version is refused).

`devices`: `device_id`, `device_public_key` (X25519, 32 bytes), `som_id`, `serial_number`, `tier`, `provisioning_date`, `station_id` (the CN of the registering
station's certificate, never taken from the body), `status` (`active` | `revoked`), `replaces` (row link), `registered_at`, `revoked_at`, `revoked_by`, `revoke_reason`.

* **Identifiers.** `device_id` (and `replaces`): 1-64 characters of `a-z 0-9 . _ : -`, starting with a letter or digit. No `/` (it is a path segment) and no upper case (ids are compared byte for byte, so case variants would be
  distinct ids that look alike: they are rejected, not folded). `som_id`, `serial_number`, `tier`: letters, digits and `. _ : / -`. **Serial numbers are unique case-insensitively** (`SN-1` and `sn-1` are the same unit; the
  stored text keeps its case).
* **Public keys** must be usable X25519 keys: bit 255 clear, below p = 2^255-19, and not a small-order point (0, 1, the two order-8 points, p-1); otherwise `invalid_field`. **Proof of possession of the private key is not
  checked** (the registry only sees a public key); a challenge-response at registration (the unit signs or unwraps something under the key) is a documented future item for the provisioning stations (Stories 6.16, 6.17).
* Unique constraints (partial indexes, so concurrent writers, even in two processes, can not both win; a violation is mapped back to the constraint that fired):
  an **active** `device_id` exists once; an **active** serial number exists once (case-insensitively); a `device_public_key` exists once **ever** (a revoked unit's key can not be registered again: that would revive a
  revoked unit).
* **Supersede rule (SoM swap / repair).** A revoked record can be superseded exactly once, by registering with `"replaces": "<device_id of the revoked record>"`. The replacement must be the same unit: **same serial number
  (case-insensitive) or same SoM id** as the replaced record, else `invalid_replaces` (as it is for a replaced record that is not revoked, unknown, or already superseded). The new record may reuse the same `device_id`.
  A station can therefore not use `replaces` to take over an arbitrary revoked identity.

`audit`: `seq` (1, 2, 3, ...), `ts` (Unix ms), `actor` (`station:<id>`, `admin:<id>`, `anonymous:<ip>`, `system`), `action`, `device_id`, `library_id`, `outcome`
(`ok` | `refused` | `error`), `code` (typed error code of a refusal), `detail` (short; never a request body, never key material), `prev_hash`, `hash`; and `audit_head` (one row: the seq and hash of the last entry).

* **Append-only.** Triggers refuse UPDATE and DELETE of `audit` (and DELETE of `audit_head`) and any INSERT whose `seq` is not exactly the last one plus one (which also stops `REPLACE INTO`, which does not fire DELETE triggers).
  Every write (register, revoke, each wrapped-key issuance, start/stop, restore) and every refused call (authorisation, validation, duplicates, unknown device or library, revoked device, TLS handshake failures, 5xx answers) appends
  a row (the pre-router and flood paths follow the throttling rules below). A device write and its audit row are one transaction. For an issuance the audit rows are committed **before** the response leaves, in a transaction that first
  re-checks the device is still active: a revoke that lands between the lookup and the issuance wins (`device_revoked`, nothing recorded, no key returned).
* **Writes refuse a doubtful log.** Before every append the server checks that the last audit row, SQLite's AUTOINCREMENT counter and the stored head row agree; if not (truncation, rollback, edits under its feet) every
  write fails, no device row is written, no key leaves (`500 audit_failed`) and `serve` would not have started.
* **Hash chain.** `hash = SHA-256("velora/registry-audit/v1" 0x00 || prev_hash || seq(u64 BE) || ts(u64 BE) || len(u32 BE)||field for actor, action, device_id, library_id, outcome, code, detail)`;
  `prev_hash` of entry 1 is 32 zero bytes. `verify-audit` recomputes the chain and fails on a missing sequence number, on **tail truncation** (the AUTOINCREMENT counter remembers the highest number ever used), on a head row that
  is not the last entry, and on any change to the audit tables or the append-only triggers: **the `CREATE` text SQLite stores for them is compared with the expected text**, not just the names.
  **Limit, stated plainly: the chain is unkeyed.** Anyone who can write the database file can modify history and recompute every hash, or drop the triggers, edit, and put them back. Two things make that detectable
  instead of silent: (1) the **anchors** below, and (2) the startup check and the append check above. They do not make it impossible.
* **Anchors.** Every `backup` writes `registry-<stamp>.anchor` next to the encrypted backup: the audit head (`seq`, `hash`) and an HMAC-SHA256 of it under the backup key, in plain text. With `--anchor-dir DIR --backup-key KEY`,
  `serve` and `verify-audit` require the database to contain, at the newest anchor's sequence number, the same hash (otherwise: the log was rewritten) and not to be shorter than it (rolled back or truncated); `restore`
  checks the backup against its own anchor (must match exactly) and against the newest anchor in `--anchor-dir` (an older backup is allowed, with a printed warning; a contradicting history is refused). An attacker with write
  access to the database but not to the anchor directory and the backup key can not forge a matching anchor. Put the anchor directory (it is the backup directory) on another host or mount with its own permissions.

## Authentication, authorisation, limits

* **Mutual TLS 1.3 only** (OpenSSL; TLS 1.2 and below are refused; no session tickets). The client certificate must chain to `--client-ca`, the factory-test root
  (`KEY-HIERARCHY.md` section 1) and have a client-authentication key usage. No certificate, a certificate of another CA, an expired one, one without the right key usage, a **revoked** one (below) or any other
  verification failure ends in a **failed handshake**, audited as `tls_handshake_failed` with the claimed (unauthenticated) CN, the verification verdict and the TLS error.
* **Certificate revocation (deny-list).** `--revoked-certs FILE`: one certificate serial number (hex, `0x` optional) per line, `#` comments. It is checked in the handshake (verify callback), re-read when the file changes, and
  a malformed file at startup is an error (a later malformed edit keeps the previous list and logs it). Revoke a station by appending its serial; remove the file entry only if the certificate is re-trusted.
* **Roles from the certificate subject:** `OU=velora-station` (CN = station id): register devices, request wrapped keys, read devices. `OU=velora-admin` (CN = admin id): revoke devices, read
  devices. A valid certificate with any other OU gets `403 forbidden`. CN must be 1-64 characters of `A-Za-z0-9._:/-`.
* **Per-station library allow-list.** `--station-libraries FILE`: `<station CN> <library_id>... | *` per line. With a file, a station not listed (or asking for a library not listed) gets `403 library_not_allowed`, checked
  before anything is unwrapped; without a file every station may request every library (the default).
* **Revoked devices** receive no keys (`403 device_revoked`, audited, also for `GET` and for a superseded id); unknown devices `404 unknown_device`. `GET /v1/devices/{id}` of a revoked record is allowed and says `status: revoked`.
* **Limits and what is audited.**
  * Request line + headers 8 KiB (`431`), body 64 KiB by default (`413`), 40 headers, CRLF line breaks only (a bare CR or LF in the head is `400`), one request per connection (`Connection: close`).
  * **One absolute deadline per connection, handshake included** (10 s by default, measured from accept, so queue time counts; 5 s without progress in a single step): the sockets are non-blocking and every wait is a `poll` against the
    deadline, so a client that stalls the handshake or dribbles bytes can not hold a worker; it gets `408 request_timeout` (or is dropped in the handshake).
  * A pool of `--workers` threads (default 8), a queue of 64 pending connections, and **at most 16 concurrent connections per peer address** (`--max-conn-per-ip`); more are closed at once. A handler that throws is caught and logged.
  * **Authenticated callers:** `--rate-limit` requests per principal per minute (default 120; `429 rate_limited` with `Retry-After`); the first refusal of a window is audited, the rest only counted in the log.
    `--register-limit` registrations per station per hour (default 600; `429 registration_rate_limited`). At most 64 libraries per wrapped-key request.
  * **Refusals before the router** (malformed / oversized / incomplete requests, wrong-role certificates): the first per (actor, code) per 60 s is audited, later ones are counted and a `refusals_suppressed` row with the count is
    written when the next window opens. **Handshake failures:** at most 30 audited per peer IP per minute; the first suppressed one is logged, the rest are silent. So a flood costs a bounded number of rows; it is never the
    case that a single refusal is lost while the log is quiet.
* The server's TLS private key must be mode 0600 (the registry refuses to start otherwise).

Not done here: a CRL/OCSP protocol (the deny-list file is the mechanism), authentication of the *server's* certificate pinning on the station side (stations pin the CA; their code is Story 6.16), and a hardware token for the
server key. Stories 6.12 and 6.13 create the real factory-test root; until then use the throw-away CA that `tests/registry_test.cpp` builds with OpenSSL's API (no CLI or files in the repository).

## Backup

`velora-registry backup --db DB --backup-key KEYFILE --kek KEKFILE --dest-dir DIR` takes a consistent snapshot of the live database with SQLite's **online backup API into memory** (no plaintext copy touches
the disk, the server keeps running), verifies its audit chain (a broken chain still gets backed up, but the command then exits 7 with a warning), encrypts it and writes
`DIR/registry-<UTC yyyymmddThhmmssZ>.vrb` and `DIR/registry-<...>.anchor` (0600, temp file + fsync + rename). `DIR` must not be the database's directory **or inside it** (also through a symlink or `..`): it is the
**second location** (another disk or mount). A directory that already exists keeps its permissions; a new one is created 0700.
The backup key is a **separate** raw 32-byte file (0600), generated like the KEK (`head -c 32 /dev/urandom > backup.key; chmod 600 backup.key`); `backup` takes `--kek` only to **refuse a backup key equal to the KEK**.
Keep a copy of the backup key offline: **without it a backup is useless.** The backup contains the device table and the audit log; **it does not contain the vault directory or the KEK** (back those up separately).

`velora-registry restore --backup FILE --backup-key KEYFILE --db TARGET [--force] [--anchor-dir DIR]`:

1. decrypts to memory (wrong key, damaged, truncated, extended: fail with nothing written);
2. refuses if `TARGET-journal`/`-wal`/`-shm` exist, if `TARGET` exists and `--force` is absent, if `TARGET.lock` is held by a running registry (flock), or if SQLite reports `TARGET` locked (`BEGIN EXCLUSIVE`);
3. writes the snapshot to a private temp file next to TARGET, and **validates that file** (`PRAGMA integrity_check`, the audit chain, schema and head, the backup's own anchor, and `--anchor-dir`'s newest anchor) **before TARGET is touched**;
4. appends a `restore` entry to the audit log of the temp file (the chain continues across the restore; the file is therefore the snapshot plus one row: the printed SHA-256 is that of the snapshot itself);
5. moves it into place: `link()` without `--force` (an existing file can never be replaced, even by a race), `rename()` with it.

Any failure before step 5 leaves TARGET byte-identical (the test checks the SHA-256 after each kind of failure) and removes the temp file. The tests restore a backup taken while the server was writing, check the rows and the chain,
and then **start a server on the restored database**, issue a key that unwraps, and check that duplicate serials and revocations survived.

**File format `VRB1`** (all integers big endian):

```
offset size field
0      4    magic "VRB1"
4      1    version 0x01
5      1    algorithm 0x01 = AES-256-GCM, key by HKDF-SHA256
6      2    reserved, zero
8      4    chunk size = 65536
12     32   random salt
44     4    random nonce prefix
48     8    plaintext length (the SQLite file, serialised)
56     8    creation time, Unix seconds
64     ...  chunks: for i = 0 .. n-1 (n = max(1, ceil(length / 65536))): ciphertext (65536 bytes; the last chunk the remainder) || 16-byte GCM tag
```

`key = HKDF-SHA256(IKM = backup key file, salt = file salt, info = "velora/registry-backup/v1", L = 32)`; chunk nonce = nonce prefix (4) || i (u64 BE, 8); chunk AAD = the 64 header bytes || i (u64 BE) ||
last-chunk flag (1 byte). Binding the header and the index into every chunk makes reordering, dropping or extending chunks and editing the header fail authentication; the total file size is checked exactly.

**Nightly job (examples, UNTESTED on a server):** `deploy/velora-registry-backup.service` + `deploy/velora-registry-backup.timer` (systemd; 02:30, `Persistent=true`, prunes backups older than 30 days) and
`deploy/velora-registry.service`. A cron equivalent:

```
30 2 * * * velora-registry /opt/velora/bin/velora-registry backup --db /var/lib/velora-registry/registry.db --backup-key /etc/velora-registry/backup.key --kek /etc/velora-registry/kek.aes256 --dest-dir /mnt/registry-backup >>/var/log/velora-registry-backup.log 2>&1
```

Restore drill: stop the service, then `velora-registry restore --backup /mnt/registry-backup/registry-....vrb --backup-key backup.key --db /var/lib/velora-registry/registry.db --force --anchor-dir /mnt/registry-backup`, then start the service. Restore the vault directory from its own backup. Run it on a spare
machine after setting the job up and then quarterly.

## Tests

`registry_test` (ctest): server on a localhost ephemeral port with a throw-away PKI generated in the test with OpenSSL's API (keys and certificates only in the test's temporary directory, removed on success);
register + wrapped-key issuance (the blobs unwrap with the device private key to the original content keys, via the shared `wrapped_key_open`); duplicates (id, serial, public key, supersede rules); revoked
devices; no certificate, certificate of another CA, expired certificate, TLS 1.2, wrong role and admin-only revoke by a station; the audit chain, its tamper / truncation / trigger-removal detection and the append-only
triggers; backup / restore round trip, wrong key, damaged and truncated files; no content key, KEK or backup key in the log, the audit rows, the database, its journal or any backup; fuzz of the HTTP parser and the
JSON handler; 12 parallel registrations of one serial (exactly one wins); the rate limiter; timeouts; configuration refusals; and the spawned `velora-registry serve` end to end (start, mutual TLS, SIGTERM).
