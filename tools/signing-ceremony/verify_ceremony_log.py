#!/usr/bin/env python3
"""Completeness checker for a Story 6.10 signing ceremony log (DRAFT).

Usage:
  verify_ceremony_log.py LOG.json     check a filled log (exit 0 = complete)
  verify_ceremony_log.py --self-test  run built-in tests

Checks structure and completeness only. It does not verify cryptography and
does not by itself satisfy Story 6.10; humans sign off the real ceremony.
"""
import copy
import json
import re
import sys

REQUIRED_KEY_ROLES = ["boot_signing", "rauc_signing", "content_manifest_signing", "kek"]
REQUIRED_STEPS = ["S%02d" % i for i in range(1, 13)]
HEX64 = re.compile(r"^[0-9a-fA-F]{64}$")
PLACEHOLDERS = {"todo", "tbd", "?", ""}


def walk(node, path=""):
    """Yield (path, value) for every leaf; skips keys starting with '_'."""
    if isinstance(node, dict):
        for k, v in node.items():
            if k.startswith("_"):
                continue
            yield from walk(v, f"{path}.{k}" if path else k)
    elif isinstance(node, list):
        for i, v in enumerate(node):
            yield from walk(v, f"{path}[{i}]")
    else:
        yield path, node


def check(log):
    errs = []
    for p, v in walk(log):
        if v is None or (isinstance(v, str) and v.strip().lower() in PLACEHOLDERS):
            errs.append(f"unfilled: {p}")
    if errs:
        return errs  # structural checks below assume values are present

    # participants: one operator, >=2 distinct witnesses, distinct names
    parts = log["participants"]
    roles = [p["role"] for p in parts]
    if roles.count("operator") != 1:
        errs.append("participants: need exactly one operator")
    if sum(1 for r in roles if r.startswith("witness")) < 2:
        errs.append("participants: need at least two witnesses")
    names = [p["name"] for p in parts]
    if len(set(names)) != len(names):
        errs.append("participants: names must be distinct")

    eq = log["equipment"]
    if eq["laptop"]["airgap_confirmed"] is not True:
        errs.append("equipment.laptop.airgap_confirmed must be true")
    for p, v in (("laptop.os_image_sha256", eq["laptop"]["os_image_sha256"]),
                 ("toolchain.pinned_manifest_sha256", eq["toolchain"]["pinned_manifest_sha256"])):
        if not HEX64.match(str(v)):
            errs.append(f"equipment.{p}: not a 64-hex SHA-256")
    toks = eq["tokens"]
    if len(toks) != 2:
        errs.append("equipment.tokens: need exactly two tokens")
    else:
        if toks[0]["serial"] == toks[1]["serial"]:
            errs.append("equipment.tokens: serials must differ")
        if toks[0]["make_model"] != toks[1]["make_model"]:
            errs.append("equipment.tokens: both tokens must be the same model (AC1)")

    # keys
    by_role = {k["role"]: k for k in log["keys"]}
    for r in REQUIRED_KEY_ROLES:
        if r not in by_role:
            errs.append(f"keys: missing role {r}")
            continue
        k = by_role[r]
        if k["generated_in_hsm"] is not True:
            errs.append(f"keys.{r}: generated_in_hsm must be true")
        if not HEX64.match(str(k["public_key_sha256"])):
            errs.append(f"keys.{r}: public_key_sha256 not 64-hex")
        if k["exportable"] is True and k["backed_up_to_backup_token"] is not True:
            errs.append(f"keys.{r}: exportable key must exist only as backup on token 2")
    fps = [k["public_key_sha256"] for k in log["keys"]]
    if len(set(fps)) != len(fps):
        errs.append("keys: duplicate fingerprints")

    # steps
    ids = [s["id"] for s in log["steps"]]
    for sid in REQUIRED_STEPS:
        if sid not in ids:
            errs.append(f"steps: missing {sid}")
    for s in log["steps"]:
        if str(s["result"]).lower() != "pass":
            errs.append(f"steps.{s['id']}: result is not 'pass'")

    st = log["signing_test"]
    for f in ("moved_on_removable_media_only", "pin_required_observed",
              "signature_verified_against_published_public_key"):
        if st[f] is not True:
            errs.append(f"signing_test.{f} must be true")
    if not HEX64.match(str(st["artifact_sha256"])):
        errs.append("signing_test.artifact_sha256 not 64-hex")

    rd = log["recovery_drill"]
    if rd["signature_verified"] is not True:
        errs.append("recovery_drill.signature_verified must be true")
    if str(rd["token_used"]).lower() != "backup":
        errs.append("recovery_drill.token_used must be 'backup'")
    if not HEX64.match(str(rd["test_artifact_sha256"])):
        errs.append("recovery_drill.test_artifact_sha256 not 64-hex")

    if not HEX64.match(str(log["log_signature"]["log_file_sha256"])):
        errs.append("log_signature.log_file_sha256 not 64-hex")
    return errs


def _filled():
    h = lambda c: c * 64
    log = {
        "ceremony_id": "T-1", "runbook_version": "t", "date_utc": "2000-01-01", "location": "t",
        "participants": [
            {"role": "operator", "name": "A", "id_checked_by": "B", "signature_ref": "x"},
            {"role": "witness_1", "name": "B", "id_checked_by": "A", "signature_ref": "x"},
            {"role": "witness_2", "name": "C", "id_checked_by": "A", "signature_ref": "x"}],
        "equipment": {
            "laptop": {"make_model": "t", "serial": "1", "airgap_confirmed": True, "os_image_sha256": h("a")},
            "toolchain": {"pinned_manifest_sha256": h("b"), "verified_by": "B"},
            "tokens": [
                {"label": "primary", "make_model": "M", "serial": "1", "firmware_version": "1", "tamper_seal_id": "s1"},
                {"label": "backup", "make_model": "M", "serial": "2", "firmware_version": "1", "tamper_seal_id": "s2"}],
            "removable_media": [{"label": "public-out", "serial": "9", "sha256_of_contents": h("c")}]},
        "keys": [{"role": r, "algorithm": "t", "key_id": r, "public_key_sha256": h(c),
                  "generated_in_hsm": True, "exportable": False, "backed_up_to_backup_token": True}
                 for r, c in zip(REQUIRED_KEY_ROLES, "1234")],
        "steps": [{"id": s, "performed_at_utc": "t", "result": "pass", "witness_initials": "B", "notes": "-"}
                  for s in REQUIRED_STEPS],
        "signing_test": {"artifact_sha256": h("d"), "moved_on_removable_media_only": True,
                         "pin_required_observed": True,
                         "signature_verified_against_published_public_key": True, "token_used": "primary"},
        "recovery_drill": {"performed_at_utc": "t", "token_used": "backup", "test_artifact_sha256": h("e"),
                           "signature_verified": True, "witness": "B"},
        "storage": {"primary_token_location": "t", "backup_token_location": "u", "pin_custody_note": "t"},
        "public_outputs": {"published_to": "t", "files_sha256": h("f")},
        "log_signature": {"log_file_sha256": h("0"), "signed_by": "A", "signature_method": "t", "signature_ref": "t"},
    }
    return log


def self_test():
    good = _filled()
    assert check(good) == [], check(good)
    cases = {}
    c = copy.deepcopy(good); c["keys"][0]["generated_in_hsm"] = None; cases["null value"] = c
    c = copy.deepcopy(good); c["keys"][1]["location"] = "TODO"; cases["TODO value"] = c
    c = copy.deepcopy(good); del c["keys"][3]; cases["missing kek"] = c
    c = copy.deepcopy(good); c["keys"][0]["generated_in_hsm"] = False; cases["key outside hsm"] = c
    c = copy.deepcopy(good); c["equipment"]["tokens"][1]["serial"] = "1"; cases["same serial"] = c
    c = copy.deepcopy(good); c["equipment"]["tokens"][1]["make_model"] = "Z"; cases["different models"] = c
    c = copy.deepcopy(good); c["recovery_drill"]["signature_verified"] = False; cases["drill failed"] = c
    c = copy.deepcopy(good); c["recovery_drill"]["token_used"] = "primary"; cases["drill on primary"] = c
    c = copy.deepcopy(good); c["steps"].pop(); cases["missing step"] = c
    c = copy.deepcopy(good); c["participants"].pop(); cases["one witness"] = c
    c = copy.deepcopy(good); c["keys"][0]["public_key_sha256"] = "abc"; cases["bad fingerprint"] = c
    for name, bad in cases.items():
        assert check(bad), f"self-test: '{name}' should fail"
    print(f"self-test OK (1 good, {len(cases)} bad cases rejected)")


def main(argv):
    if argv[1:] == ["--self-test"]:
        self_test()
        return 0
    if len(argv) != 2:
        print(__doc__)
        return 2
    with open(argv[1], encoding="utf-8") as f:
        log = json.load(f)
    errs = check(log)
    for e in errs:
        print("FAIL", e)
    print("INCOMPLETE" if errs else "COMPLETE (structure only; not a substitute for sign-off)")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
