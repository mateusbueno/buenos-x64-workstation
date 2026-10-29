#!/usr/bin/env python3
"""Story 9.1 CI gate: check composite USB gadget script for security and spec compliance.

Ensures:
- Exactly three functions are configured: UAC2, MIDI (f_midi2/f_midi), and CDC-NCM.
- Invariant (NFR24, AR38): No mass-storage, ACM/serial console, or other functions exist.
- Identifiers match spec: VID 0x1209, PID 0x5673, Product 'Velora 73', Manufacturer 'Velora'.
- Self-test mode supported via --self-test.
"""

import argparse
import os
import re
import sys

FORBIDDEN_PATTERNS = [
    r"\bmass_storage\b",
    r"\bf_mass_storage\b",
    r"\bacm\b",
    r"\bf_acm\b",
    r"\bf_serial\b",
    r"\brndis\b",
    r"\bf_rndis\b",
    r"\becm\b",
    r"\bf_ecm\b",
]

REQUIRED_PATTERNS = [
    (r'ID_VENDOR="0x1209"', "Fixed VID 0x1209"),
    (r'ID_PRODUCT="0x5673"', "Fixed PID 0x5673"),
    (r'PRODUCT="Velora 73"', "Product string 'Velora 73'"),
    (r'MANUFACTURER="Velora"', "Manufacturer 'Velora'"),
    (r'functions/uac2\.usb0', "UAC2 Audio function"),
    (r'functions/(midi2?\.usb0)', "MIDI function (midi2 or midi)"),
    (r'functions/ncm\.usb0', "CDC-NCM Ethernet function"),
    (r'configs/c\.1/uac2\.usb0', "UAC2 linked to configuration 1"),
    (r'configs/c\.1/midi\.usb0', "MIDI linked to configuration 1"),
    (r'configs/c\.1/ncm\.usb0', "NCM linked to configuration 1"),
]

def check_script_content(content: str) -> list[str]:
    errors = []

    # Check for forbidden functions
    for pattern in FORBIDDEN_PATTERNS:
        match = re.search(pattern, content, re.IGNORECASE)
        if match:
            # ignore comments that say 'No mass-storage'
            lines = content.splitlines()
            for line_no, line in enumerate(lines, 1):
                clean_line = line.strip()
                if clean_line.startswith("#"):
                    continue
                if re.search(pattern, line, re.IGNORECASE):
                    errors.append(f"Forbidden USB function or pattern '{pattern}' found at line {line_no}: {line.strip()}")

    # Check for required configurations
    for pattern, description in REQUIRED_PATTERNS:
        if not re.search(pattern, content):
            errors.append(f"Missing required configuration: {description} (pattern: {pattern})")

    # Verify that ONLY the 3 allowed function links exist in configs/c.1
    link_matches = re.findall(r"ln\s+-s\s+[\"']?functions/([^\"' ]+)[\"']?\s+configs/c\.1/([^\"' \n]+)", content)
    for target, link_name in link_matches:
        base_func = target.split("/")[0].split(".")[0]
        if base_func not in ("uac2", "midi", "midi2", "ncm", "$MIDI_FUNC"):
            errors.append(f"Unauthorized function linked to configuration: {target} -> {link_name}")

    return errors

def run_self_test() -> int:
    # 1. Valid script test
    valid_script = """
    ID_VENDOR="0x1209"
    ID_PRODUCT="0x5673"
    PRODUCT="Velora 73"
    MANUFACTURER="Velora"
    mkdir -p functions/uac2.usb0 functions/midi2.usb0 functions/ncm.usb0
    ln -s functions/uac2.usb0 configs/c.1/uac2.usb0
    ln -s functions/midi2.usb0 configs/c.1/midi.usb0
    ln -s functions/ncm.usb0 configs/c.1/ncm.usb0
    """
    errs = check_script_content(valid_script)
    if errs:
        print(f"Self-test failed on valid script: {errs}", file=sys.stderr)
        return 1

    # 2. Rejection of mass-storage
    bad_storage = valid_script + "\nmkdir -p functions/mass_storage.usb0\n"
    errs = check_script_content(bad_storage)
    if not any("mass_storage" in e for e in errs):
        print("Self-test failed: did not catch mass_storage", file=sys.stderr)
        return 1

    # 3. Rejection of ACM serial
    bad_serial = valid_script + "\nmkdir -p functions/acm.usb0\n"
    errs = check_script_content(bad_serial)
    if not any("acm" in e for e in errs):
        print("Self-test failed: did not catch acm", file=sys.stderr)
        return 1

    # 4. Rejection of wrong PID
    bad_pid = valid_script.replace('ID_PRODUCT="0x5673"', 'ID_PRODUCT="0x1234"')
    errs = check_script_content(bad_pid)
    if not any("0x5673" in e for e in errs):
        print("Self-test failed: did not catch wrong PID", file=sys.stderr)
        return 1

    print("check_usb_gadget self-test: ok")
    return 0

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="Workspace root directory")
    parser.add_argument("--script", default="sysd/scripts/velora-usb-gadget.sh", help="Path to gadget script")
    parser.add_argument("--self-test", action="store_true", help="Run self-test suite")
    args = parser.parse_args()

    if args.self_test:
        return run_self_test()

    script_path = os.path.join(args.root, args.script) if not os.path.isabs(args.script) else args.script
    if not os.path.isfile(script_path):
        print(f"Error: gadget script not found at {script_path}", file=sys.stderr)
        return 2

    with open(script_path, "r", encoding="utf-8") as f:
        content = f.read()

    errors = check_script_content(content)
    if errors:
        print(f"check_usb_gadget: FAILED ({len(errors)} error(s)):", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        return 1

    print(f"check_usb_gadget: ok ({script_path})")
    return 0

if __name__ == "__main__":
    sys.exit(main())
