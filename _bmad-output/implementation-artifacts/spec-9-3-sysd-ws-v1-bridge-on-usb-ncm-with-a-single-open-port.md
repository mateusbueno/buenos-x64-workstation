# Story 9.3: sysd ws.v1 Bridge on USB NCM with a Single Open Port

## Summary
Implemented the locked-down USB CDC-NCM single-port networking and security architecture for Velora Studio communication per Story 9.3, AR25, AR38, NFR24, and NFR44:
- **Single Open Port Policy (`sysd/nftables/velora-ncm-firewall.nft`)**:
  - `nftables` packet filter enforcing `policy drop` on the `usb0` interface.
  - Allows only essential link configuration ICMP/NDP and exactly one ingress TCP service port: `tcp dport 50051 accept`.
  - Drops all unauthorized inbound traffic, including SSH (port 22), DNS (port 53), HTTP (port 80/443), and Telnet (port 23).
  - Explicitly rejects and drops all packet forwarding/routing across `usb0`.
- **Link-Local Network Architecture (`sysd/NCM-BRIDGE.md`)**:
  - Documents the RFC 3927 subnet `169.254.73.0/24` with device address `169.254.73.1` and Studio host address `169.254.73.2`.
  - Length-prefixed framing over TCP: 4-byte big-endian `uint32_t` length + protobuf `ws::v1` messages.
- **C++ Constants & Server Integration (`sysd/include/velora/sysd/bridge_server.hpp`, `sysd/tests/bridge_test.cpp`)**:
  - Exposes `kDefaultStudioBridgePort = 50051`, `kDefaultNcmDeviceIp`, `kDefaultNcmHostIp`, and `kDefaultNcmSubnet`.
  - Added unit test `test_ncm_single_port_policy()` in `sysd_bridge_tests`.
- **Automated CI Firewall Audit (`tools/ci/check_ncm_firewall.py`)**:
  - Validates nftables rules syntax, asserts default drop, verifies port 50051 is open and that simulated port scans for common attack ports (22, 53, 80, 443, etc.) are all dropped.
  - Includes `--self-test` suite integrated into root `CMakeLists.txt`.

## Verification
- `check_ncm_firewall` and `check_ncm_firewall_selftest` pass in CTest.
- `sysd_bridge_tests` passes (including `test_ncm_single_port_policy`).
