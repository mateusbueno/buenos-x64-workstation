# Library Format, Protection and Transfer

## Container

- Block-indexed, so streaming can seek to any point of a sample. An index table maps sample to blocks.
- Lossless compression per block in FLAC style (linear prediction + Rice coding), about 50% ratio. Decoded output is bit-identical to the source. libFLAC (BSD) may be used.
- Heads are decoded to PCM into locked RAM at load. A note's attack never waits on decode.
- Up to 32-bit float / 96 kHz.

## Protection (factory and partner libraries only)

| Layer | Purpose |
|---|---|
| AES-CTR per block | Encrypts without breaking random-access streaming |
| Per-library content key | One key per library |
| Per-device key in RK3588 OTP / OP-TEE, provisioned at the factory | Wraps the content key, so a copied file does not play on another unit |
| Ed25519-signed manifest + per-block hashes | A tampered file is rejected |
| RK3588 secure boot | A custom OS cannot boot to read keys (the way the Kronos was broken) |

The goal is to deter extraction, not to be unbreakable. User imports (SFZ/SF2) are never encrypted.

## Transfer paths

- **USB-A host import:** the user plugs in an external drive, and the unit converts, indexes, and stores the library. User imports stay unencrypted. Factory and partner libraries arrive already encrypted and signed by the off-device content-pack tool. The unit only verifies and installs them, and plaintext vendor content never reaches the unit (architecture AD-14).
- **Desktop companion app:** manages libraries, does heavy SFZ conversion on the computer, and activates partner libraries (future). It transfers over a mediated, MTP-like USB protocol and never over mass storage.
- **Imports are atomic:** a library becomes visible only after it has been fully written and verified.
- The NVMe is sealed and not user-accessible.
