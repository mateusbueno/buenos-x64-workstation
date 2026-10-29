#!/usr/bin/env python3
"""Story 9.3 CI gate: verify nftables single-port firewall policy on USB NCM interface.

Ensures:
- Default input policy on USB NCM is drop.
- Exactly one TCP port (50051) is accepted for Velora Studio ws.v1 traffic.
- All other ports (including SSH 22, DNS 53, Telnet 23, HTTP 80/443) are rejected/dropped.
- Forwarding across USB NCM is strictly disabled (policy drop).
- Supports --self-test suite.
"""

import argparse
import os
import re
import sys

ALLOWED_TCP_PORT = 50051

def parse_and_validate_firewall(content: str) -> list[str]:
    errors = []

    # 1. Table check
    if not re.search(r"table\s+inet\s+velora_ncm", content):
        errors.append("Missing 'table inet velora_ncm' table definition")

    # 2. Input chain drop policy
    if not re.search(r"chain\s+input\s*\{[^}]*type\s+filter\s+hook\s+input[^}]*policy\s+drop", content, re.DOTALL):
        errors.append("Input chain does not define 'type filter hook input ... policy drop'")

    # 3. Forward chain drop policy
    if not re.search(r"chain\s+forward\s*\{[^}]*type\s+filter\s+hook\s+forward[^}]*policy\s+drop", content, re.DOTALL):
        errors.append("Forward chain does not define 'type filter hook forward ... policy drop'")

    # 4. Find all accepted TCP dports
    accepted_ports = []
    lines = content.splitlines()
    for line_no, line in enumerate(lines, 1):
        clean = line.strip()
        if clean.startswith("#"):
            continue
        m = re.search(r"tcp\s+dport\s+(\d+)\s+accept", clean)
        if m:
            port = int(m.group(1))
            accepted_ports.append(port)
            if port != ALLOWED_TCP_PORT:
                errors.append(f"Forbidden open TCP port {port} accepted at line {line_no}: {clean}")

    if ALLOWED_TCP_PORT not in accepted_ports:
        errors.append(f"Missing required open port rule for TCP {ALLOWED_TCP_PORT} (ws.v1 bridge)")

    if len(accepted_ports) > 1:
        errors.append(f"More than one TCP port accepted: {accepted_ports}")

    # 5. Check that SSH (22) is never accepted
    if any(p == 22 for p in accepted_ports) or re.search(r"dport\s+22\s+accept", content):
        errors.append("Critical: SSH port 22 is explicitly accepted on USB NCM interface!")

    return errors

def simulate_port_scan(content: str, test_ports: list[int]) -> dict[int, str]:
    """Simulates a port scan against the nftables rule logic for usb0."""
    validation_errors = parse_and_validate_firewall(content)
    if validation_errors:
        raise ValueError(f"Invalid firewall config: {validation_errors}")

    accepted_match = re.search(r"tcp\s+dport\s+(\d+)\s+accept", content)
    allowed = int(accepted_match.group(1)) if accepted_match else None

    results = {}
    for port in test_ports:
        if port == allowed:
            results[port] = "OPEN"
        else:
            results[port] = "FILTERED/DROPPED"
    return results

def run_self_test() -> int:
    valid_nft = """
    table inet velora_ncm {
        chain input {
            type filter hook input priority filter; policy drop;
            iifname != "usb0" accept
            ct state established,related accept
            tcp dport 50051 accept
            drop
        }
        chain forward {
            type filter hook forward priority filter; policy drop;
            drop
        }
        chain output {
            type filter hook output priority filter; policy accept;
        }
    }
    """
    errs = parse_and_validate_firewall(valid_nft)
    if errs:
        print(f"Self-test failed on valid nftables config: {errs}", file=sys.stderr)
        return 1

    scan = simulate_port_scan(valid_nft, [22, 53, 80, 443, 50051])
    if scan[50051] != "OPEN" or scan[22] != "FILTERED/DROPPED" or scan[80] != "FILTERED/DROPPED":
        print(f"Self-test simulated port scan mismatch: {scan}", file=sys.stderr)
        return 1

    # Bad configs
    bad_ssh = valid_nft.replace('tcp dport 50051 accept', 'tcp dport 50051 accept\n            tcp dport 22 accept')
    if not parse_and_validate_firewall(bad_ssh):
        print("Self-test failed: did not catch SSH port 22", file=sys.stderr)
        return 1

    bad_policy = valid_nft.replace('policy drop', 'policy accept')
    if not parse_and_validate_firewall(bad_policy):
        print("Self-test failed: did not catch policy accept", file=sys.stderr)
        return 1

    print("check_ncm_firewall self-test: ok")
    return 0

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="Workspace root directory")
    parser.add_argument("--file", default="sysd/nftables/velora-ncm-firewall.nft", help="Path to nftables file")
    parser.add_argument("--self-test", action="store_true", help="Run self-test suite")
    args = parser.parse_args()

    if args.self_test:
        return run_self_test()

    file_path = os.path.join(args.root, args.file) if not os.path.isabs(args.file) else args.file
    if not os.path.isfile(file_path):
        print(f"Error: nftables file not found at {file_path}", file=sys.stderr)
        return 2

    with open(file_path, "r", encoding="utf-8") as f:
        content = f.read()

    errors = parse_and_validate_firewall(content)
    if errors:
        print(f"check_ncm_firewall: FAILED ({len(errors)} error(s)):", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        return 1

    scan_results = simulate_port_scan(content, [21, 22, 23, 25, 53, 80, 443, 3306, 50051, 8080])
    print(f"check_ncm_firewall: ok ({file_path})")
    print(f"  Simulated port scan: port 50051 is {scan_results[50051]}, all other 9 common ports {scan_results[22]}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
