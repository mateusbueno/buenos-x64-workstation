# Velora 73 — USB NCM Network Bridge & Single-Port Security Policy

Architecture and security specification for the USB CDC-NCM interface between the Velora 73 instrument and Velora Studio, per Story 9.3 (AR25, AR38, NFR24, NFR44).

---

## 1. Network Interface & Link-Local Configuration

- **Network Interface**: `usb0` (spawned by Linux composite gadget function `f_ncm`).
- **IP Addressing**:
  - RFC 3927 Link-Local subnet: `169.254.73.0/24`
  - **Instrument (Device)**: `169.254.73.1` (Static Link-Local with fall-back DHCP server `systemd-networkd`)
  - **Velora Studio (Host)**: `169.254.73.2` (assigned via DHCP or auto-configured via Link-Local / IPv4LL)
  - **Subnet Mask**: `255.255.255.0` (`/24`)

---

## 2. Single Open Port Architecture (Port 50051)

`sysd` binds its TCP Bridge Server to `0.0.0.0:50051` (or specifically `169.254.73.1:50051`).
Per NFR24 and AR38, **exactly one TCP port is accessible over the USB cable**:

- **Port**: `50051` (TCP)
- **Protocol**: Length-prefixed `ws.v1` framing:
  - 4-byte big-endian `uint32_t` payload length header.
  - Protobuf-encoded `ws::v1::ClientMessage` / `ws::v1::ServerMessage`.
- **Allowed Services**:
  - `Hello` (Client version, features, client ID negotiation)
  - `Pairing` (PIN handshake and persistent device authorization)
  - `LiveSet` / `Snapshot` forwarding to `engine` control thread
  - `Librarian` transfer forwarding to `librarian`
  - `Config` and `FirmwareUpdate` handling in `sysd`

---

## 3. Firewall & Lockdown Policy (`nftables`)

Configured in [`sysd/nftables/velora-ncm-firewall.nft`](file:///Users/mateusbueno/Sites/buenos-x64-workstation/sysd/nftables/velora-ncm-firewall.nft):
1. **Default Policy**: `drop` on interface `usb0`.
2. **Explicit Port Allowlist**:
   - `tcp dport 50051 accept` (Sole permitted ingress service port).
3. **Explicit Drops / Blocked Services on USB NCM**:
   - **Port 22 (SSH)**: Strictly dropped. SSH is confined to the physical Ethernet port on the dev board; never exposed over the musician's USB cable.
   - **Port 53 (DNS)**: Dropped.
   - **Port 80 / 443 (HTTP/HTTPS)**: Dropped.
   - **Port 23 (Telnet)**: Dropped.
   - **Routing/Forwarding**: All forward chains dropped (`policy drop`). Velora cannot be used as a network bridge or gateway.
4. **Automated Verification**:
   - Tested by [`tools/ci/check_ncm_firewall.py`](file:///Users/mateusbueno/Sites/buenos-x64-workstation/tools/ci/check_ncm_firewall.py) and simulated port scans in `sysd_ncm_firewall_test`.
