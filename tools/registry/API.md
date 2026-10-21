# Registry HTTP API (v1)

HTTP/1.1 over **TLS 1.3 with mutual authentication** (see README.md for roles and limits). One request per connection (`Connection: close`), `Content-Length` bodies only (no chunked encoding),
JSON in and out. Paths are case-sensitive. All bodies are UTF-8 JSON objects; unknown or duplicate members are refused. Roles: **station** (certificate `OU=velora-station`) and **admin** (`OU=velora-admin`).

## Errors

Every error has the same shape and a stable `code`; the message is for people, the code for programs:

```json
{"error": {"code": "duplicate_serial", "message": "an active device with this serial number is already registered"}}
```

Some errors carry one more member (`field`, `device_id`, `library_id`).

| HTTP | code | meaning |
|---|---|---|
| 400 | `bad_request` | malformed HTTP (request line, headers, Content-Length) |
| 400 | `invalid_request` | body is not valid JSON, not an object, unknown/duplicate member, missing or mistyped `library_ids` |
| 400 | `invalid_field` (`field`) | a registration field is missing or malformed |
| 400 | `invalid_device_id`, `invalid_library_id`, `too_many_libraries` | path / library id malformed; more than 64 libraries |
| 401 | `unauthenticated` | no identity (the TLS layer normally refuses first: the handshake fails) |
| 403 | `forbidden` | role not allowed for this endpoint, or the certificate subject is neither station nor admin |
| 403 | `library_not_allowed` (`library_id`) | `--station-libraries` is configured and this station may not request this library |
| 403 | `device_revoked` | the device is revoked and receives no keys |
| 404 | `not_found`, `unknown_device` (`device_id`), `unknown_library` (`library_id`) | |
| 405 | `method_not_allowed` | |
| 408 | `request_timeout` | request not completed within the connection's absolute deadline (handshake included) |
| 409 | `duplicate_device_id`, `duplicate_serial`, `duplicate_public_key`, `invalid_replaces`, `already_revoked` | |
| 413 | `body_too_large` | body above the limit (64 KiB default) |
| 429 | `rate_limited` | `Retry-After` header; the first refusal of a window is audited, later ones only counted |
| 429 | `registration_rate_limited` | too many registrations from this station in the last hour (`--register-limit`, default 600) |
| 431 | `headers_too_large` | |
| 500 | `vault_unavailable`, `vault_entry_invalid`, `key_wrap_failed`, `audit_failed`, `internal_error` | server side; no key was issued and nothing was written. 5xx answers are audited with outcome "error". `audit_failed`: the audit log could not be appended (also when its consistency check failed) |
| 501 | `unsupported_transfer_encoding` | |
| 505 | `http_version_not_supported` | HTTP/1.1 only |

A failed TLS handshake (no client certificate, wrong CA, expired certificate, TLS below 1.3) has no HTTP response at all: the connection is closed. It is audited as `tls_handshake_failed`.

## `POST /v1/devices` (station): register a device

```json
{"device_id": "dev-0001", "device_public_key": "<64 hex digits, X25519>", "som_id": "som-123", "serial_number": "SN-0001",
 "tier": "standard", "provisioning_date": "2026-10-20", "replaces": "dev-0000"}
```

`device_id` and `replaces`: 1-64 characters of `a-z 0-9 . _ : -` starting with a letter or digit (no `/`, no upper case); `som_id`, `serial_number`: 1-64 characters of `A-Za-z0-9._:/-` starting with a letter or digit
(serial numbers are unique case-insensitively); `tier`: the same charset, up to 32; `provisioning_date`: a real `YYYY-MM-DD`; `device_public_key`: 64 hex digits of a usable X25519 key: bit 255 clear, below 2^255-19,
not a small-order point (0, 1, the two order-8 points, p-1), else `invalid_field`. Proof of possession of the private key is not checked.
`replaces` (optional): the `device_id` of a revoked record to supersede; the new record must have the same serial number (case-insensitive) or the same SoM id as that record, else `invalid_replaces`.
The station id is taken from the certificate.

`201` with the device object (below). `409`: `duplicate_device_id` / `duplicate_serial` (an *active* record has it), `duplicate_public_key` (any record ever had it), `invalid_replaces`.

## `GET /v1/devices/{device_id}` (station or admin)

`200` with the device object: the active record of that id, else the most recent (revoked) one (a revoked record is readable and says `"status": "revoked"`; it never receives keys). `404 unknown_device`.

```json
{"device_id": "dev-0001", "device_public_key": "<hex>", "som_id": "som-123", "serial_number": "SN-0001", "tier": "standard", "provisioning_date": "2026-10-20",
 "station_id": "station-01", "status": "active", "replaces": null, "registered_at": "2026-10-20T12:00:00Z",
 "revoked_at": null, "revoked_by": null, "revoke_reason": null}
```

## `POST /v1/devices/{device_id}/revoke` (admin)

Optional body `{"reason": "text, at most 200 characters, no control characters"}`. `200` with the device object (`status: "revoked"`, `revoked_by: "admin:<cn>"`). `404 unknown_device`,
`409 already_revoked`. A station gets `403 forbidden`.

## Wrapped keys (station)

Both forms return the same body. All or nothing: if any library is unknown or its vault entry does not open, nothing is issued and nothing is recorded as issued.

* `POST /v1/devices/{device_id}/wrapped-keys` with `{"library_ids": ["<uuid>", ...]}`
* `GET /v1/devices/{device_id}/wrapped-keys?library_id=<uuid>[&library_id=<uuid>...]` (or `library_id=<uuid>,<uuid>`)

`library_id`: canonical lower-case `8-4-4-4-12` UUID text, 1 to 64 distinct ids.

```json
{"device_id": "dev-0001", "keys": [{"library_id": "3f1d6a52-9b7e-4c1a-8d2e-5a6b7c8d9e0f", "wrapped_key": "<210 hex digits = 105 bytes of wrapped_key_v1>"}]}
```

The blob is `wrapped_key_v1` of `tools/keys/KEY-HIERARCHY.md` section 6 for that device's registered public key and that `library_id`: only the device's private key opens it, a fresh ephemeral key and nonce are
used on every request. Errors: `403 device_revoked`, `404 unknown_device`, `404 unknown_library`, `500 vault_entry_invalid` (the entry does not open with the configured KEK or is damaged), `500 vault_unavailable`,
`500 key_wrap_failed`, `500 audit_failed`. Every issued key is audited (device, library, station, time), never the key.
