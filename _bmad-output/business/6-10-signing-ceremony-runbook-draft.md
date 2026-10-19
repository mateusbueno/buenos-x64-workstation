**DRAFT FOR FOUNDER — does not satisfy Story 6.10 acceptance criteria**

# Story 6.10 — Production signing root ceremony runbook (draft)

Prepared 2026-10-19. This is preparation material. The Story 6.10 ACs are satisfied only by a real ceremony, a real signed log and the repo deliverable `tools/keys/CEREMONY.md` (does not exist yet; `tools/keys/` is absent). This draft is meant to seed that file once blanks are answered.

Sources read: `_bmad-output/planning-artifacts/epics.md` Story 6.10 (lines 4378-4401), Story 6.1 (4124-4148), 6.12 (4434-4463), 6.13 (4465-4489), 6.14 (4491-4518), 6.15 (4520-4539), 8.2 (4894-4916), AR34 (373), NFR23 (273); `ARCHITECTURE-SPINE.md` AD-25 (lines 329-341).

## 0. What the repo states, and what it does not

| Fact | Source |
|---|---|
| Two HSM tokens, same model; air-gapped laptop, pinned verified toolchain | epics.md:4390 |
| Keys: production boot-signing, RAUC-signing, content-manifest, KEK; generated inside HSM, non-exportable or exported only as encrypted backup to second token; only public keys and hashes leave | epics.md:4392 |
| Log records date, participants, device serials, key fingerprints; log is signed | epics.md:4393 |
| Artifacts in/out on removable media only; signing needs HSM PIN; signed artifact verifies against published production public key | epics.md:4397 |
| Recovery drill: test artifact signed from backup token, verifies, drill logged | epics.md:4401 |
| Content-manifest key is Ed25519 | epics.md:4138, 4165 |
| Content key is 256-bit, AES-CTR per block (content key, not a ceremony key) | epics.md:4194, 4164 |
| Three roots (dev, factory-test, production); production private keys only in offline HSM | epics.md:4138-4139; Spine:334 |
| Prod image trusts only prod keys; build refuses dev public key in production variant | epics.md:4148 |
| Production boot key hash is fused (Story 6.14); RAUC production keyring shipped (6.14, 8.2) | epics.md:4505-4507 |
| Production signing requests come from CI as unsigned bundle plus signing request; signed result attached back; no prod key reachable from CI | epics.md:4908 |
| Release record lists per variant image/bundle hashes and signing key fingerprints | epics.md:4916 |

NOT stated anywhere in the repo, therefore left as blanks (section 11): HSM make/model, interface (PKCS#11 etc.), algorithm and key size for boot, RAUC and KEK keys, and the secure-boot chain's required key type (Story 1.6 selects the chain; `tools/spikes/secure-boot/REPORT.md` was not checked as part of this draft and is the first place to look), PIN/quorum scheme, number of people, backup format, rotation period, compromise SLA, escrow location.

## 1. Roles

| Role | Who (blank) | Duty |
|---|---|---|
| Ceremony operator | ______ | Only person who touches the laptop and tokens; reads each step aloud |
| Witness 1 | ______ | Watches screen and media; initials every step in the witness column |
| Witness 2 | ______ | Independent of operator; verifies fingerprints against the printed sheet |
| Founder / key owner | ______ | Approves runbook version, receives public outputs, holds one escrow item |
| Escrow custodian(s) | ______ | Holds the backup token and/or sealed PIN material (see section 6) |

Constraints enforced by the verifier: exactly one operator, at least two witnesses, all distinct people. Whether the operator and a witness may be the same person as the founder is a decision (section 11); a single-founder company will likely need external witnesses (for example a lawyer or an accountant).

## 2. Preconditions (all must be true before S01)

1. Runbook version frozen and hash recorded in the log header.
2. Story 6.1 key hierarchy doc exists and names these four roles for the production root.
3. Public-key consumers known: Story 1.6 boot chain's fuse format (boot key hash), RAUC keyring (6.12/6.14), content-manifest trust list (6.1), registry KEK use (epics.md:4338).
4. Pinned toolchain manifest prepared on a connected machine, and its SHA-256 printed on paper for the witnesses.
5. Two tokens of the same model bought, unopened, tamper-bagged; serials photographed.
6. Room booked; no phones or radios at the table (blank: whether a camera records the screen).
7. Printed copy of this runbook and a blank log for hand notes.

## 3. Equipment checklist

| # | Item | Serial / ID recorded in log | Check |
|---|---|---|---|
| 1 | Air-gapped laptop, radios physically disabled or removed (blank: model) | `equipment.laptop` | [ ] |
| 2 | Laptop OS image, hash verified (blank: which OS) | `os_image_sha256` | [ ] |
| 3 | Pinned toolchain bundle on read-only media | `toolchain.pinned_manifest_sha256` | [ ] |
| 4 | HSM token 1 (primary) | `tokens[0]` | [ ] |
| 5 | HSM token 2 (backup), same model | `tokens[1]` | [ ] |
| 6 | Removable media A, "in": unsigned test artifact, read-only | | [ ] |
| 7 | Removable media B, "public-out": receives public keys, hashes, signed log | `removable_media` | [ ] |
| 8 | Tamper-evident bags and numbered seals, sealed envelopes | `tamper_seal_id` | [ ] |
| 9 | Printed fingerprint sheet (blank form) and pens | | [ ] |
| 10 | Clock source for UTC time (blank: how verified offline) | | [ ] |

## 4. Scripted step list

Commands are intentionally not written: they depend on the HSM and toolchain choice (section 11). Each step has an expected-output description, and the log carries result and witness initials. Step IDs match `tools/signing-ceremony/ceremony_log.template.json`.

| Step | Action | Expected output | AC | Witness / sign-off |
|---|---|---|---|---|
| S01 | Roll call; check identities; read roles aloud; record date UTC and participants | Names and ID checks in log header | AC1 (participants, date) | W1 ____ W2 ____ |
| S02 | Inspect room and tamper seals on tokens and media; photograph | Seal IDs match purchase record | AC1 | W1 ____ W2 ____ |
| S03 | Confirm laptop is air-gapped (radios off, no cables); boot from the verified image | Boot ok; hash of image equals printed value | AC1 (air-gapped) | W1 ____ W2 ____ |
| S04 | Verify toolchain manifest hash against printed sheet; install/mount pinned toolchain | Hash equal; tool versions equal the manifest | AC1 (pinned, verified) | W1 ____ W2 ____ |
| S05 | Open tokens; read and record make, model, serial, firmware of both; confirm same model | Two distinct serials, one model | AC1 (two tokens, same model) | W1 ____ W2 ____ |
| S06 | Initialise token 1 and token 2 (blank: PIN scheme, how many PINs, who sets them) | Tokens initialised; PIN requirement confirmed by a failed no-PIN operation | AC1, AC2 (PIN) | W1 ____ W2 ____ |
| S07 | Generate on token 1, inside the HSM: production boot-signing, RAUC-signing and content-manifest keys, and production KEK. Content-manifest key is Ed25519 (epics.md:4138); other algorithms per section 11 | For each: key present, flags show non-extractable or wrap-only export, label/ID recorded | AC1 | W1 ____ W2 ____ |
| S08 | Create encrypted backup of the keys to token 2 (or, if the HSM cannot, generate separately on token 2; decision in section 11 as this changes fingerprints) | Token 2 holds the keys; public key hashes on token 2 equal token 1 | AC1 (backup), AC3 | W1 ____ W2 ____ |
| S09 | Export public keys only; compute SHA-256 of each; operator reads, both witnesses compare to screen, write on fingerprint sheet; confirm no private material on media B | 4 public keys, 4 fingerprints, identical on sheet and screen | AC1 (only public keys and hashes leave) | W1 ____ W2 ____ |
| S10 | Signing test per signing procedure (section 5): carry unsigned test artifact in on media A, sign with token 1 after PIN, carry signature out on media B, verify against the exported public key | Verification ok; signing refused without PIN | AC2 | W1 ____ W2 ____ |
| S11 | Recovery drill (section 6): power down, remove token 1, sign a different test artifact from token 2, verify against the same published public key | Verification ok with identical fingerprint | AC3 | W1 ____ W2 ____ |
| S12 | Fill log; compute log hash; sign the log (blank: method, e.g. each participant signs a printed copy and the operator signs the digital file); seal tokens and envelopes; hand to custodians; wipe laptop session | Signed log; seals numbered; custody receipts | AC1 (log signed) | W1 ____ W2 ____ |

After the ceremony: run `python3 -I tools/signing-ceremony/verify_ceremony_log.py filled-log.json`. It only checks completeness and consistency (all fields filled, two distinct serials of one model, four key roles generated in HSM with 64-hex fingerprints, passing steps, drill on backup token verified). It does not check cryptography.

## 5. Routine signing procedure (AC2, and Story 8.2)

1. CI produces the unsigned production RAUC bundle and a signing request (epics.md:4908). Request carries bundle SHA-256, version, security version (Story 6.15) and the requesting commit.
2. Copy request and bundle to removable media (never a network). Scan on a separate machine (blank: AV/scan requirement).
3. On the offline laptop: verify bundle hash equals the request, show hash to a witness (blank: is a witness required for every routine signing, or only the ceremony).
4. Sign with the RAUC key. HSM prompts the PIN; no PIN, no signature.
5. Write signed bundle plus signature to the out media; verify against the published production public key before leaving the room.
6. Append an entry to the signing audit log (section 9).
7. Attach the signed result back to the release; the release record lists per variant hashes and signing fingerprints (epics.md:4916).

Boot-image signing for production boot stages (Story 6.14) and content-manifest signing follow the same shape; boot-key rotation interacts with fuses (see 7).

## 6. Backup, escrow and recovery drill (AC3)

- Backup token: kept physically apart from the primary (blank: location type, bank box, notary, home safe). The two must never share a single point of loss (fire, theft).
- PINs: blank whether split (for example M-of-N) or sealed envelopes. A single human memory is not recoverable.
- Drill timing: at the ceremony (S11) and then at a blank interval (suggest yearly).
- Drill steps: retrieve backup token under witness, boot offline laptop, sign a fresh test artifact (different from the S10 artifact), verify against published public key, return the token, reseal, log with a date, witness and artifact hash (`recovery_drill`).
- Pass criterion: signature verifies with the same fingerprint as the published key. Fail means: stop, treat as a key-loss incident, investigate before any release is signed.
- Full-loss case (both tokens lost): production root cannot sign. Units already fused with the production key hash cannot be updated by a new key (fuse is irreversible per Story 1.6 and 6.14 context). Whether the secure-boot chain supports multiple or revocable boot keys is a blank to resolve in the Story 1.6 report (section 11).

## 7. Revocation and compromise procedure (draft; not in the repo ACs)

The repo does not define revocation. This section is proposed procedure; mark every item as a founder decision.

| Trigger | Suspected scope | First actions (hour 0) | Follow-up |
|---|---|---|---|
| Lost or stolen token | One token | Treat as compromised if PIN protected only by weak scheme; stop signing; retrieve other token under witness | Decide whether to rotate (needs new ceremony) |
| PIN exposure | One token | Change PIN on that token in a witnessed session; review audit log for signings since exposure | |
| Suspect signature seen in the wild | RAUC key | Stop releases; compare signed-artifact audit log against what is in the field | Rotate RAUC key: new ceremony, new keyring in the next release signed by the old key (blank: does keyring update by bundle work given 6.12 rejects keys outside the keyring, so the old key must sign the transition) |
| Boot-key compromise | Boot-signing | Largest impact: fused hash on shipped units; options depend on the chain (blank) and OTP anti-rollback (6.15) | Only a unit-by-unit remedy if the chain has no key revocation |
| Content-manifest key compromise | Ed25519 | Stop publishing bundles; ship a trust-list update (blank: how clients get it) | Re-sign libraries with new key |
| KEK compromise | Vault of content keys | Content keys are wrapped under the KEK (epics.md:4195); rotate and re-wrap vault; registry affected (epics.md:4338) | Notify per legal advice (blank) |

Common steps: freeze signing; record the incident in the audit log; preserve evidence; inventory every artifact signed since the last known-good date; decide customer notification with the lawyer; schedule a new ceremony (new log, new ceremony ID, same runbook). Dev and factory-test roots are separate (Spine:334), so their compromise never touches production but must still be rotated and CI keyrings replaced.

## 8. AC mapping summary

| AC | Given / When / Then (short) | Steps | Log fields |
|---|---|---|---|
| AC1 | Two same-model tokens, air-gapped laptop, pinned toolchain; ceremony per `tools/keys/CEREMONY.md`; four keys in HSM, only public keys and hashes leave; log with date, participants, serials, fingerprints, signed | S01-S09, S12 | participants, equipment, keys, public_outputs, log_signature |
| AC2 | Release to sign; offline laptop; removable media only; PIN required; verifies against published key | S06, S10, routine procedure (5) | signing_test |
| AC3 | Backup token recovery drill, logged | S08, S11 | recovery_drill |
| Open deliverable | `tools/keys/CEREMONY.md` is named by AC1 and does not exist | Convert this draft once blanks are answered | |

## 9. Audit log template

Ceremony log: `tools/signing-ceremony/ceremony_log.template.json` (all values null). One file per ceremony, signed, stored with both the escrow copy and the public repo copy once reviewed (blank: where; the log contains serials and fingerprints but no secrets).

Routine signing log (one line per signing event, append-only, kept on the offline laptop media and copied out on media B):

| Date UTC | Operator | Witness | Request ID | Artifact name | Artifact SHA-256 | Key role used | Key fingerprint | Token serial | Result | Signature file SHA-256 |
|---|---|---|---|---|---|---|---|---|---|---|
| ____ | ____ | ____ | ____ | ____ | ____ | ____ | ____ | ____ | ____ | ____ |

## 10. Dependencies on other stories (so the ceremony is not run too early)

- 1.6: chain selection and signing format (decides boot key type and what the fuse takes).
- 6.1: `tools/keys/KEY-HIERARCHY.md` (role names, rotation, trust map).
- 6.12/6.13: RAUC keyring layout per root.
- 6.14: fuse step consumes the production boot key hash; do not fuse a unit before this ceremony's hashes are published.
- 6.15: security version in signed metadata; routine signing requests carry it.
- 8.2: consumer of the routine signing procedure.

## 11. Blanks to answer (founder decisions)

1. HSM make/model and interface: ______ (repo names none)
2. Key algorithm/size for boot-signing: ______ (depends on Story 1.6 chain; check `tools/spikes/secure-boot/REPORT.md`)
3. Key algorithm/size for RAUC signing: ______
4. KEK type (symmetric or asymmetric) and size: ______ (content key is 256-bit per epics.md:4194, KEK size not stated)
5. Backup mechanism: encrypted export to token 2 vs independent keys on token 2 (changes fingerprints, and AC1 wording allows only the export form): ______
6. Participants and who counts as independent witness: ______
7. PIN scheme (single, M-of-N, sealed envelopes) and who holds each part: ______
8. Escrow locations for token 2 and sealed PIN material: ______
9. Laptop model/OS and offline toolchain pinning method: ______
10. Is witness required on every routine signing: ______
11. Rotation interval, drill interval, and compromise SLA: ______
12. Whether the boot chain supports multiple or revocable boot keys: ______
13. Log-signing method (paper plus digital?): ______
14. Whether the ceremony is recorded on camera: ______
