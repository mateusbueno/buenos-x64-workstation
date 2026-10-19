#!/usr/bin/env python3
"""Validate a filled Hammond M-solo mapping.json against mapping.template.json.

Checks: every template control present, no TODO/null left in used fields,
numbers within MIDI range, position labels/values consistent, source cited.
Standard library only.  Usage:
    validate_mapping.py mapping.json [--template mapping.template.json]
    validate_mapping.py --self-test
Exit code 0 = valid, 1 = problems found, 2 = usage/IO error.
"""
import argparse
import copy
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_TEMPLATE = os.path.join(HERE, "mapping.template.json")


def _is_int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def _unfilled(v):
    return v is None or (isinstance(v, str) and ("TODO" in v.upper() or not v.strip()))


def _find_todos(obj, path, out):
    if isinstance(obj, dict):
        for k, v in obj.items():
            if k == "_comment":
                continue
            _find_todos(v, f"{path}.{k}", out)
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            _find_todos(v, f"{path}[{i}]", out)
    elif isinstance(obj, str) and "TODO" in obj.upper():
        out.append(f"{path}: still contains TODO")


def _check_msg(cid, c, tmpl_c, errs):
    m = c.get("message")
    p = f"controls[{cid}].message"
    if not isinstance(m, dict):
        errs.append(f"{p}: missing or not an object")
        return
    typ = m.get("type")
    if typ not in ("CC", "NRPN"):
        errs.append(f"{p}.type: must be 'CC' or 'NRPN' (got {typ!r})")
        return
    if typ == "CC":
        used, unused = ["number"], ["nrpn_msb", "nrpn_lsb"]
    else:
        used, unused = ["nrpn_msb", "nrpn_lsb"], ["number"]
    for f in used:
        v = m.get(f)
        if not _is_int(v):
            errs.append(f"{p}.{f}: must be an integer (got {v!r})")
        elif not 0 <= v <= 127:
            errs.append(f"{p}.{f}: {v} outside MIDI range 0..127")
    for f in unused:
        if m.get(f) is not None:
            errs.append(f"{p}.{f}: must be null for type {typ}")
    bits = m.get("value_bits")
    if bits not in (7, 14):
        errs.append(f"{p}.value_bits: must be 7 or 14 (got {bits!r})")
        bits = None
    vmin, vmax = m.get("value_min"), m.get("value_max")
    top = 127 if bits == 7 else 16383
    ok = True
    for f, v in (("value_min", vmin), ("value_max", vmax)):
        if not _is_int(v):
            errs.append(f"{p}.{f}: must be an integer (got {v!r})")
            ok = False
        elif v < 0 or (bits and v > top):
            errs.append(f"{p}.{f}: {v} outside 0..{top}")
            ok = False
    if ok and vmin > vmax:
        errs.append(f"{p}: value_min {vmin} > value_max {vmax}")
        ok = False
    pm = m.get("position_mapping")
    if not isinstance(pm, list) or not pm:
        errs.append(f"{p}.position_mapping: must be a non-empty list of "
                    f'{{"position": label, "value": int}}')
        return
    labels = []
    for i, e in enumerate(pm):
        if not isinstance(e, dict) or "position" not in e or "value" not in e:
            errs.append(f"{p}.position_mapping[{i}]: needs 'position' and 'value'")
            continue
        lab, val = e["position"], e["value"]
        if _unfilled(lab if isinstance(lab, str) else str(lab)):
            errs.append(f"{p}.position_mapping[{i}].position: empty/TODO")
        labels.append(str(lab))
        if not _is_int(val):
            errs.append(f"{p}.position_mapping[{i}].value: must be an integer")
        elif ok and not vmin <= val <= vmax:
            errs.append(f"{p}.position_mapping[{i}].value: {val} outside "
                        f"value_min..value_max ({vmin}..{vmax})")
    if len(set(labels)) != len(labels):
        errs.append(f"{p}.position_mapping: duplicate position labels")
    exp = tmpl_c.get("positions_expected")
    if exp is not None and sorted(labels) != sorted(exp):
        errs.append(f"{p}.position_mapping: labels {sorted(labels)} != expected {sorted(exp)}")


def validate(mapping, template):
    """Return a list of error strings (empty = valid)."""
    errs = []
    if not isinstance(mapping, dict):
        return ["mapping root must be a JSON object"]
    _find_todos(mapping, "$", errs)
    src = mapping.get("source")
    if not isinstance(src, dict):
        errs.append("source: block missing")
    else:
        for f in ("manual_title", "manual_edition", "pages"):
            if _unfilled(src.get(f)):
                errs.append(f"source.{f}: citation missing")
    ch = mapping.get("midi_channel")
    if not _is_int(ch) or not 1 <= ch <= 16:
        errs.append(f"midi_channel: must be integer 1..16 (got {ch!r})")
    ctrls = mapping.get("controls")
    if not isinstance(ctrls, list):
        errs.append("controls: must be a list")
        return errs
    by_id = {}
    for c in ctrls:
        cid = c.get("id") if isinstance(c, dict) else None
        if cid in by_id:
            errs.append(f"controls: duplicate id {cid!r}")
        by_id[cid] = c
    for tc in template["controls"]:
        cid = tc["id"]
        if cid not in by_id:
            errs.append(f"controls: required control {cid!r} missing")
            continue
        _check_msg(cid, by_id[cid], tc, errs)
    tids = {tc["id"] for tc in template["controls"]}
    for cid, c in by_id.items():
        if cid not in tids:
            _check_msg(cid, c, {}, errs)  # extra controls must be fully filled too
    return errs


def load_json(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def make_fake_filled(template):
    """SYNTHETIC test fixture only. Numbers are meaningless, not Hammond data."""
    m = copy.deepcopy(template)
    m.pop("_comment", None)
    m["source"] = {"manual_title": "FAKE", "manual_edition": "FAKE-ed", "pages": "0",
                   "transcribed_by": "selftest", "transcribed_on": "1970-01-01", "notes": "synthetic"}
    m["midi_channel"] = 1
    for i, c in enumerate(m["controls"]):
        exp = c.pop("positions_expected", None)
        labels = exp if exp is not None else ["a", "b", "c"]
        msg = c["message"]
        if i % 2:
            msg.update(type="NRPN", number=None, nrpn_msb=(i % 100) + 1, nrpn_lsb=(i % 90) + 2,
                       value_bits=14, value_min=0, value_max=1000 + 10 * len(labels))
            vals = [j * 10 for j in range(len(labels))]
        else:
            msg.update(type="CC", number=(i % 100) + 1, nrpn_msb=None, nrpn_lsb=None,
                       value_bits=7, value_min=0, value_max=100)
            vals = [j * 10 for j in range(len(labels))]
        msg["position_mapping"] = [{"position": l, "value": v} for l, v in zip(labels, vals)]
    return m


def self_test():
    t = load_json(DEFAULT_TEMPLATE)
    assert validate(t, t), "unfilled template must NOT validate"
    good = make_fake_filled(t)
    e = validate(good, t)
    assert not e, f"fake filled mapping should validate: {e}"

    def bad(fn, needle):
        m = copy.deepcopy(good)
        fn(m)
        errs = validate(m, t)
        assert any(needle in x for x in errs), (needle, errs)

    bad(lambda m: m["controls"].pop(), "missing")
    bad(lambda m: m["controls"][0]["message"].update(number=128), "outside MIDI range")
    bad(lambda m: m["controls"][0]["message"].update(number=None), "must be an integer")
    bad(lambda m: m["controls"][1]["message"].update(nrpn_msb=-1), "outside MIDI range")
    bad(lambda m: m["controls"][0]["message"].update(type="TODO"), "must be 'CC' or 'NRPN'")
    bad(lambda m: m["controls"][0]["message"].update(value_max=200), "outside 0..127")
    bad(lambda m: m["controls"][0]["message"].update(value_min=50, value_max=10), "value_min")
    bad(lambda m: m["controls"][0]["message"].update(position_mapping="TODO"), "position_mapping")
    bad(lambda m: m["controls"][0]["message"]["position_mapping"][0].update(position="x"), "expected")
    bad(lambda m: m["source"].update(pages="TODO"), "citation missing")
    bad(lambda m: m.pop("source"), "source")
    bad(lambda m: m.update(midi_channel=17), "midi_channel")
    bad(lambda m: m["controls"][0].update(note="TODO later"), "TODO")
    print("validate_mapping self-test: OK")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mapping", nargs="?", help="filled mapping.json")
    ap.add_argument("--template", default=DEFAULT_TEMPLATE)
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args(argv)
    if a.self_test:
        self_test()
        return 0
    if not a.mapping:
        ap.error("mapping path required (or --self-test)")
    try:
        mapping, template = load_json(a.mapping), load_json(a.template)
    except (OSError, ValueError) as ex:
        print(f"error: {ex}", file=sys.stderr)
        return 2
    errs = validate(mapping, template)
    if errs:
        print(f"INVALID: {len(errs)} problem(s)")
        for e in errs:
            print("  -", e)
        return 1
    print(f"VALID: {len(mapping['controls'])} controls, source cited")
    return 0


if __name__ == "__main__":
    sys.exit(main())
