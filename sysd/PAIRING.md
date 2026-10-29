# Velora Studio & Unit Pairing Architecture

This document records the security design, key-agreement model, and lifecycle for pairing Velora Studio with the Velora 73 instrument (Story 9.6, AR38, FR96, UX-DR37, UX-DR46, UX-DR77).

---

## 1. Trust Model and Boundaries

1. **Physical Link as Trust Boundary:**
   The connection between Velora Studio and the physical unit is a direct, point-to-point USB-C link operating in peripheral role (CDC-NCM interface with a single open TCP port on link-local address space). The instrument does not route traffic or expose management services on external interfaces (NFR24).

2. **Physical-Presence Verification:**
   Authorization requires physical access to the instrument's display. When pairing is initiated from the device Settings menu, `sysd` generates an ephemeral 6-digit numeric code (`000000`–`999999`, ~20 bits entropy) generated from the hardware CSPRNG (`getentropy`).

3. **Protection against Brute Force:**
   - **Global Attempt Limit:** At most 5 invalid attempts are permitted per pairing window.
   - **Window Expiry:** The pairing window automatically closes after 2 minutes (120 seconds).
   - **Constant-Time Comparison:** The code is verified in constant time (`PairingWindow::constant_time_equals`) to prevent timing side-channels.
   - **One Code, One Pair:** A successful pairing immediately closes the pairing window.

---

## 2. Token Exchange & Key Agreement

```
  Velora Studio                                 Velora 73 (sysd)
       |                                                |
       |  (Physical presence: user opens Action Sheet)  |
       |  --------------------------------------------> |  Generates random 6-digit code
       |                                                |  Displays code on screen
       |                                                |
       |  Hello { client_id, client_version, ... }      |
       | ---------------------------------------------> |
       |  HelloAck { device_id, model, paired=false }   |
       | <--------------------------------------------- |
       |                                                |
       |  PairRequest { code, computer_name }           |
       | ---------------------------------------------> |  Constant-time code match
       |                                                |  Generate 256-bit token (getentropy)
       |                                                |  Compute token_hash = SHA256(token)
       |                                                |  Store (client_id, name, token_hash)
       |  PairReply { paired=true, pairing_token }      |  Action Sheet closes
       | <--------------------------------------------- |
       |                                                |
       | Store token in device_pairing.json (0600)      |
       | Title Bar: "● VELORA 73 · CONNECTED VIA USB · PAIRED"
```

1. **Client Identity:**
   The client creates a stable `client_id` (1..64 chars of `[A-Za-z0-9_.-]`) and provides a human-readable `computer_name` (sanitized to max 64 printable characters).

2. **Long-Term Secret:**
   Upon successful code verification, `sysd` draws 256 bits of entropy from `/dev/urandom` / `getentropy`. It computes the SHA-256 hex digest of the token and stores it in the atomic, mode 0600 file `<data>/bridge/paired_computers.txt`.

3. **Single Secret Exposure:**
   The plaintext 256-bit token is sent over the local link once in `PairReply.pairing_token`. `sysd` discards the plaintext from memory and keeps only the SHA-256 hash.

4. **Client-Side Secret Storage:**
   Velora Studio stores the token indexed by the unit's unique `device_id` in `<app_data>/device_pairing.json` with file permissions 0600. A token issued by one unit is never presented to any other `device_id`.

---

## 3. Subsequent Connections (Re-Authentication)

When a previously paired computer connects again:
1. Studio sends `Hello` with `client_id` and the stored `pairing_token` for that `device_id`.
2. `sysd` checks:
   - Does `client_id` exist in `paired_computers.txt`?
   - Does `constant_time_equals(SHA256(presented_token), stored_hash)` match?
3. If valid, the session is marked `paired = true`, timestamp is updated (`touch`), and `HelloAck` replies with `paired: true`.
4. The computer does not need to enter a pairing code again.

---

## 4. Deauthorization (Forget Computer)

1. The instrument's Settings menu (`SettingsView › Computer (Velora Studio)`) lists all paired computers with their name and last connected time.
2. When the user taps "Forget", `sysd` deletes the computer entry from `paired_computers.txt` via atomic file replace (`PairedStore::remove(id)`).
3. If that computer attempts to connect again or sends subsequent messages, `sysd` rejects it with `ERROR_CODE_NOT_PAIRED`. Studio detects this and prompts for first-time pairing again.

---

## 5. Failure Modes and Fallback Behaviors

| Condition | Bridge / sysd Response | Studio Response |
|---|---|---|
| Invalid code attempt (1..4) | `ERROR_CODE_PAIRING_CODE_INVALID` with `attempts_left` | Displays "That code doesn't match. Check the Velora screen." |
| 5th invalid attempt | Window closes; `ERROR_CODE_PAIRING_CLOSED` (`reason=attempts`) | Displays lockout error; prompts user to restart pairing on the instrument |
| 2 minutes window expiry | Window closes; `ERROR_CODE_PAIRING_CLOSED` (`reason=expired`) | Prompts user that pairing window expired on the instrument |
| Forgotten computer | Rejects with `ERROR_CODE_NOT_PAIRED` | Transitions to `unpaired`; presents pairing code dialog |
| Unpaired command attempt | Drops command with `ERROR_CODE_NOT_PAIRED` | Command fails safely; Live Sets and sound parameters unmodified |
| Audio plane safety | Pairing logic runs entirely on non-real-time worker threads | Main audio engine / I2S audio / DSP rendering never interrupted (zero xruns) |
