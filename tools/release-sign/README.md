# tools/release-sign (Story 8.2)

Offline release-signing request generator, detached HSM signature attacher, per-root `.raucb` bundle verifier, and `release-record.json` traceability tool for the three Velora cryptographic roots (`dev`, `factory-test`, `prod`).

## Commands

```bash
# 1. In the release build environment (no private key present):
python3 tools/release-sign/velora_release_sign.py prepare-request \
  --input-dir /path/to/unsigned-build \
  --out-dir /path/to/signing-request \
  --root prod \
  --version 1.6.0

# 2. After the offline HSM ceremony signs signing_message_hex:
python3 tools/release-sign/velora_release_sign.py attach-signature \
  --request /path/to/signing-request/signing-request.json \
  --unsigned-payload /path/to/signing-request/unsigned.payload \
  --signature-hex <128-hex-ed25519-signature> \
  --signer-pub-hex <64-hex-prod-rauc-pub> \
  --signer-root prod \
  --root-ca-pub-hex <64-hex-prod-root-ca-pub> \
  --out-bundle /path/to/velora-1.6.0-prod.raucb \
  --record /path/to/release-record.json \
  --ceremony-id CEREMONY-PROD-20261029

# 3. Verify a bundle against a single unit root (e.g. prod):
python3 tools/release-sign/velora_release_sign.py verify-bundle \
  --bundle /path/to/velora-1.6.0-prod.raucb \
  --trusted-root prod

# 4. Trace any field unit's installed version or bundle hash to its signing event:
python3 tools/release-sign/velora_release_sign.py trace-unit \
  --record /path/to/release-record.json \
  --version 1.6.0 --variant prod
```
