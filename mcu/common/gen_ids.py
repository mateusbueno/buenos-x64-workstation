#!/usr/bin/env python3
"""Generates the MCU control/button/LED ID maps from the single source mcu/common/ids.json.

Outputs (into --out-dir):
  mcu/mcu_ids.h             C header (#defines, one per ID, plus counts and the map version)
  velora/host/mcu_ids.hpp   C++ constexpr tables for engine/host
  MCU-ID-MAP.md             the versioned partner document (also published under the architecture folder)

Modes:
  gen_ids.py --out-dir DIR            generate the three files into DIR
  gen_ids.py --check                  validate ids.json and verify the committed MCU-ID-MAP.md copies are up to date
  gen_ids.py --write-published        regenerate the committed MCU-ID-MAP.md copies
  gen_ids.py --self-test              prove the validator rejects duplicate IDs, missing fields, reused IDs

Validation failures (exit 1): duplicate ID or name within a namespace, missing/ill-typed field, ID 0 or > 255,
min > max, an ID listed in retired_ids that is in use again (IDs are never reused), stale published document.
Python standard library only.
"""
import argparse
import copy
import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SRC = os.path.join(HERE, "ids.json")
PUBLISHED = [
    os.path.join(HERE, "MCU-ID-MAP.md"),
    os.path.join(ROOT, "_bmad-output", "planning-artifacts", "architecture",
                 "architecture-buenos-x64-workstation-2026-10-17", "MCU-ID-MAP.md"),
]
NAMESPACES = [("controls", "CTRL", "Controls (CONTROL frame)"),
              ("buttons", "BTN", "Buttons (BUTTON frame)"),
              ("leds", "LED", "LEDs (LED_SET frame)")]
FIELDS = {"id": int, "name": str, "kind": str, "bits": int, "min": int, "max": int, "legend": str}
KINDS = {"controls": {"encoder", "fader", "wheel", "pedal", "pedal_switch"},
         "buttons": {"button", "toggle"},
         "leds": {"led", "ring"}}


class IdError(Exception):
    pass


IDENT_RE = re.compile(r"^[A-Z][A-Z0-9_]*$")


def _int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def validate(d):
    try:
        _validate(d)
    except (KeyError, TypeError, AttributeError, ValueError) as e:
        raise IdError(f"malformed ids.json: {type(e).__name__}: {e}")


def _validate(d):
    if not isinstance(d, dict):
        raise IdError("top level must be an object")
    for key in ("schema", "document_version", "protocol_version", "retired_ids"):
        if key not in d:
            raise IdError(f"missing top-level field '{key}'")
    if not _int(d["schema"]):
        raise IdError("schema must be an integer")
    if not isinstance(d["document_version"], str) or not d["document_version"]:
        raise IdError("document_version must be a non-empty string")
    if not _int(d["protocol_version"]) or not 0 < d["protocol_version"] < 256 or (d["protocol_version"] >> 4) == 0:
        raise IdError("protocol_version must be an integer 16..255 (major in the high nibble, never 0)")
    if not isinstance(d["retired_ids"], dict):
        raise IdError("retired_ids must be an object")
    for ns, _, _ in NAMESPACES:
        if ns not in d or not isinstance(d[ns], list) or not d[ns]:
            raise IdError(f"missing or empty namespace '{ns}'")
        retired_list = d["retired_ids"].get(ns, [])
        if not isinstance(retired_list, list) or not all(_int(x) for x in retired_list):
            raise IdError(f"retired_ids.{ns} must be a list of integers")
        retired = set(retired_list)
        ids, names = {}, {}
        for e in d[ns]:
            if not isinstance(e, dict):
                raise IdError(f"{ns}: entry {e!r} is not an object")
            for f, t in FIELDS.items():
                if f not in e:
                    raise IdError(f"{ns}: entry {e.get('name', e)} is missing field '{f}'")
                ok = _int(e[f]) if t is int else isinstance(e[f], t)
                if not ok:
                    raise IdError(f"{ns}: {e['name']}: field '{f}' has the wrong type")
            if not IDENT_RE.match(e["name"]):
                raise IdError(f"{ns}: name '{e['name']}' is not an upper-case C identifier")
            if ns == "buttons":
                if "states" not in e:
                    raise IdError(f"buttons: {e['name']} is missing field 'states'")
                if not _int(e["states"]) or e["states"] < 1:
                    raise IdError(f"buttons: {e['name']}: states must be an integer >= 1")
            if not 1 <= e["id"] <= 255:
                raise IdError(f"{ns}: {e['name']}: id {e['id']} out of range 1..255")
            if e["kind"] not in KINDS[ns]:
                raise IdError(f"{ns}: {e['name']}: unknown kind '{e['kind']}'")
            if e["min"] > e["max"]:
                raise IdError(f"{ns}: {e['name']}: min > max")
            if not 1 <= e["bits"] <= 16:
                raise IdError(f"{ns}: {e['name']}: bits must be 1..16")
            if e["min"] >= 0:
                fits = e["max"] <= (1 << e["bits"]) - 1
            else:
                fits = e["min"] >= -(1 << (e["bits"] - 1)) and e["max"] <= (1 << (e["bits"] - 1)) - 1
            if not fits:
                raise IdError(f"{ns}: {e['name']}: range {e['min']}..{e['max']} does not fit in {e['bits']} bits")
            if (e["kind"] == "encoder") != (e["min"] < 0):
                raise IdError(f"{ns}: {e['name']}: only encoders are signed (step delta)")
            if e["id"] in ids:
                raise IdError(f"{ns}: duplicate id {e['id']} ({ids[e['id']]} and {e['name']})")
            if e["name"] in names:
                raise IdError(f"{ns}: duplicate name {e['name']}")
            if e["id"] in retired:
                raise IdError(f"{ns}: id {e['id']} ({e['name']}) was retired and must never be reused")
            ids[e["id"]] = e["name"]
            names[e["name"]] = e["id"]


def content_hash(d):
    return hashlib.sha256(json.dumps(d, sort_keys=True, separators=(",", ":"),
                                     ensure_ascii=True).encode()).hexdigest()


def ver_str(v):
    return f"{v >> 4}.{v & 15} (wire byte 0x{v:02X})"


def gen_c(d):
    h = content_hash(d)
    o = ["/* GENERATED by mcu/common/gen_ids.py from mcu/common/ids.json. Do not edit. */",
         "#ifndef MCU_IDS_H", "#define MCU_IDS_H", "",
         f'#define MCU_ID_MAP_VERSION "{d["document_version"]}"',
         f'#define MCU_ID_MAP_HASH "{h}"',
         f"#define MCU_ID_MAP_PROTOCOL_VERSION {d['protocol_version']}u", ""]
    for ns, pre, title in NAMESPACES:
        o.append(f"/* {title} */")
        for e in d[ns]:
            o.append(f"#define MCU_{pre}_{e['name']} {e['id']}u")
        o.append(f"#define MCU_{pre}_COUNT {len(d[ns])}u")
        o.append("")
    o.append("#include <stdint.h>")
    o.append("")
    o.append("/* Range lookup used by the frame library to validate CONTROL / BUTTON / LED_SET payloads. */")
    o.append("typedef struct mcu_ctrl_range {")
    o.append("  uint8_t id;")
    o.append("  uint8_t is_delta; /* encoders: value is an int16 step delta */")
    o.append("  int32_t min;")
    o.append("  int32_t max;")
    o.append("} mcu_ctrl_range;")
    o.append("")
    o.append("static inline const mcu_ctrl_range* mcu_ctrl_lookup(uint8_t id) {")
    o.append("  static const mcu_ctrl_range table[MCU_CTRL_COUNT] = {")
    for e in d["controls"]:
        o.append(f"      {{{e['id']}u, {1 if e['kind'] == 'encoder' else 0}u, {e['min']}, {e['max']}}},")
    o.append("  };")
    o.append("  for (unsigned i = 0; i < MCU_CTRL_COUNT; ++i) {")
    o.append("    if (table[i].id == id) return &table[i];")
    o.append("  }")
    o.append("  return (const mcu_ctrl_range*)0;")
    o.append("}")
    for ns, fn, cnt in (("buttons", "mcu_button_id_known", "MCU_BTN_COUNT"), ("leds", "mcu_led_id_known", "MCU_LED_COUNT")):
        o.append("")
        o.append(f"static inline int {fn}(uint8_t id) {{")
        o.append(f"  static const uint8_t ids[{cnt}] = {{" + ", ".join(f"{e['id']}u" for e in d[ns]) + "};")
        o.append(f"  for (unsigned i = 0; i < {cnt}; ++i) {{")
        o.append("    if (ids[i] == id) return 1;")
        o.append("  }")
        o.append("  return 0;")
        o.append("}")
    o.append("")
    o.append("#endif /* MCU_IDS_H */")
    return "\n".join(o) + "\n"


def cpp_str(s):
    return json.dumps(s, ensure_ascii=True)


def gen_cpp(d):
    h = content_hash(d)
    o = ["// GENERATED by mcu/common/gen_ids.py from mcu/common/ids.json. Do not edit.",
         "#pragma once", "#include <cstddef>", "#include <cstdint>", "",
         "namespace velora::host::mcu_ids {", "",
         f'inline constexpr const char* kMapVersion = "{d["document_version"]}";',
         f'inline constexpr const char* kMapHash = "{h}";',
         f"inline constexpr std::uint8_t kMapProtocolVersion = {d['protocol_version']};", "",
         "struct IdInfo {", "  std::uint8_t id;", "  const char* name;", "  const char* kind;",
         "  std::uint8_t bits;", "  std::int32_t min;", "  std::int32_t max;", "  std::uint8_t states;",
         "  const char* legend;", "};", ""]
    for ns, _, _ in NAMESPACES:
        cname = {"controls": "kControls", "buttons": "kButtons", "leds": "kLeds"}[ns]
        o.append(f"inline constexpr IdInfo {cname}[] = {{")
        for e in d[ns]:
            o.append(f"    {{{e['id']}, {cpp_str(e['name'])}, {cpp_str(e['kind'])}, {e['bits']}, {e['min']}, "
                     f"{e['max']}, {e.get('states', 1)}, {cpp_str(e['legend'])}}},")
        o.append("};")
        o.append(f"inline constexpr std::size_t {cname}Count = sizeof({cname}) / sizeof({cname}[0]);")
        o.append("")
    for ns, pre, _ in NAMESPACES:
        o.append(f"namespace {ns} {{")
        for e in d[ns]:
            o.append(f"inline constexpr std::uint8_t {e['name']} = {e['id']};")
        o.append("}  // namespace " + ns)
        o.append("")
    o.append("}  // namespace velora::host::mcu_ids")
    return "\n".join(o) + "\n"


PROTOCOL_MD = """## 1. Frame format

All multi-byte fields are little-endian. One format for both MCUs and the SoC, both directions.

| Offset | Size | Field | Note |
|---|---|---|---|
| 0 | 1 | Sync | Fixed `0xA5` |
| 1 | 1 | Protocol version | High nibble major, low nibble minor |
| 2 | 1 | Type | See section 3 |
| 3 | 1 | Length | Payload bytes, 0..64 |
| 4 | 2 | Seq | Increments by 1 per frame, wraps at 65536 |
| 6 | 4 | Tick | MCU tick at the event (free-running, at least 1 MHz) |
| 10 | n | Payload | Layout fixed per type, section 3 |
| 10+n | 2 | CRC-16 | CRC-16/CCITT-FALSE over every byte from version to the end of the payload, stored little-endian |

Total frame size is 12 + n bytes (at most 76).

## 2. CRC

CRC-16/CCITT-FALSE: polynomial `0x1021`, initial value `0xFFFF`, no input or output reflection, no final XOR.
Check value: the CRC of the ASCII string `123456789` is `0x29B1`.

## 3. Types and payloads

| Type | Code | Direction | Length | Payload |
|---|---|---|---|---|
| `HELLO` | 1 | MCU to SoC | 5 | `fw_version` u16 (major in the high byte, minor in the low byte), `protocol_version` u8, `board_id` u8 (1 keybed MCU, 2 panel MCU), `keybed_contacts` u8 (2 or 3; 0 on the panel MCU) |
| `KEY_CONTACT` | 2 | MCU to SoC | 3 | `key` u8 (0 = lowest key), `contact` u8 (0-based, in closing order), `state` u8 (1 closed, 0 open) |
| `AFTERTOUCH` | 3 | MCU to SoC | 3 | `target` u8 (`0xFF` = channel pressure, otherwise the key index), `value` u16 |
| `CONTROL` | 4 | MCU to SoC | 3 | `control_id` u8 (section 6), `value` u16: unsigned absolute value for faders, wheels and pedals; signed 16-bit two's complement step delta for encoders |
| `BUTTON` | 5 | MCU to SoC | 2 | `button_id` u8 (section 7), `state` u8 (1 pressed, 0 released); includes the VALUE push switch |
| `LED_SET` | 6 | SoC to MCU | 3 | `led_id` u8 (section 8), `brightness` u8 (0 off, 255 full), `arg` u8: colour index for single LEDs (0 = default colour), fill position 0..255 for encoder rings |
| `TIME_SYNC` | 7 | SoC to MCU to SoC | 9 | `kind` u8 (0 request, 1 reply), `token` u32 (chosen by the SoC in the request, echoed in the reply), `tick` u32 (reply: the MCU tick when the reply was built; request: 0) |
| `ENTER_BOOTLOADER` | 8 | SoC to MCU | 4 | `magic` u32 = `0x544F4F42` (bytes `42 4F 4F 54`) |

A frame whose length differs from the table, or whose type is unknown, is rejected by the decoder and counted. Typed pack/unpack also rejects out-of-range fields: `board_id` in {1, 2}, `keybed_contacts` in {0, 2, 3}, `state`/`pressed` in {0, 1}, control values within the range of the control's ID entry (faders, wheels, pedals unsigned at their resolution; encoder deltas -127..127), and unknown control, button or LED IDs.

## 4. Version policy

The version byte has the major version in the high nibble and the minor version in the low nibble.
The current version is 1.0 (wire byte `0x10`). A receiver whose current major is N accepts frames of major N (any minor)
and major N-1 (any minor, when N-1 is at least 1; major 0 is never valid). Every other major is rejected with a typed
error and a version-mismatch counter. Bump the minor for compatible additions (new types are dropped and counted by older
receivers) and the major for any incompatible change. The sync, length and CRC rules are the same in every major, so a
receiver can always skip a frame of a rejected major.

## 5. Decoder behaviour (reference implementation `mcu/common`)

The streaming decoder consumes one byte at a time and keeps a fixed buffer. On noise, a bad length, a CRC mismatch or a
truncated frame it discards the false sync byte and rescans the bytes it already holds for the next `0xA5`, so a real frame
that started inside a false one is still delivered. It never delivers a frame whose CRC is wrong. Counters: CRC errors,
sequence gaps and lost frames, version mismatches, framing errors, resyncs, dropped bytes, bad lengths, unknown types.

Sequence accounting: seq is 16-bit and wraps. Every CRC-valid frame consumes a seq, including frames that are then rejected
(unknown type, wrong length, unsupported major), so a minor-version addition creates no spurious gap. With `delta = (seq - (last + 1)) mod 65536`:
`delta == 0` is in order; `delta > 0x8000` is a duplicate or out-of-order frame, counted as a reorder and not as a loss
(the expectation does not move); otherwise `delta` frames were lost (one gap). A delivered `HELLO` resets the expectation
without recording a gap, because an MCU reboot restarts seq at 0. A caller that can tell the link went idle calls
`mcu_decoder_flush_stale()` to abandon a candidate frame stalled on a bogus length.
"""


def gen_md(d):
    h = content_hash(d)
    o = [f"# MCU frame protocol and ID map, version {d['document_version']}", "",
         "Generated from `mcu/common/ids.json` by `mcu/common/gen_ids.py`. Do not edit by hand. "
         "Published to the hardware partners (HARDWARE-INTERFACES.md section 5 says the software team owns these maps).", "",
         f"- Document version: **{d['document_version']}**",
         f"- Protocol version: **{ver_str(d['protocol_version'])}**",
         f"- Source hash (SHA-256 of the canonical ids.json): `{h}`",
         "- IDs are stable and never reused. A retired ID stays listed in `retired_ids` of the source file.",
         "- Each namespace (controls, buttons, LEDs) has its own ID space; 0 is never assigned.", "", PROTOCOL_MD]
    for ns, pre, title in NAMESPACES:
        sec = {"controls": "6", "buttons": "7", "leds": "8"}[ns]
        o.append(f"## {sec}. {title}")
        o.append("")
        if ns == "controls":
            o.append("Encoders report a signed step delta per event (no absolute value, no pickup). Faders, wheels and "
                     "pedals report the unsigned value at the stated resolution; an MCU with a coarser ADC shifts its "
                     "reading left to the stated resolution. The pitch wheel rests at 2048.")
        elif ns == "buttons":
            o.append("Debounced press (1) and release (0). Toggle buttons report press and release like any button; the "
                     "SoC owns the toggle state and drives one LED per state.")
        else:
            o.append("The MCU never sets an LED on its own. `brightness` 0..255; `arg` is the colour index for single "
                     "LEDs and the fill position (0..255) for rings. Toggle state LEDs: two for DRAWBARS/MIXER, two for "
                     "PARTS/SCENES, three for PART/ALL/ASSIGN.")
        o.append("")
        if ns == "buttons":
            o.append("| ID | Name | Kind | States | Legend |")
            o.append("|---|---|---|---|---|")
            for e in d[ns]:
                o.append(f"| {e['id']} (`0x{e['id']:02X}`) | `{e['name']}` | {e['kind']} | {e['states']} | {e['legend']} |")
        else:
            o.append("| ID | Name | Kind | Bits | Range | Legend |")
            o.append("|---|---|---|---|---|---|")
            for e in d[ns]:
                o.append(f"| {e['id']} (`0x{e['id']:02X}`) | `{e['name']}` | {e['kind']} | {e['bits']} | "
                         f"{e['min']}..{e['max']} | {e['legend']} |")
        o.append("")
        retired = d["retired_ids"].get(ns, [])
        o.append("Retired IDs (never reuse): " + (", ".join(str(r) for r in retired) if retired else "none") + ".")
        o.append("")
    md = "\n".join(o)
    check_sections(md)
    return md


def check_sections(md):
    heads = [int(m.group(1)) for m in re.finditer(r"^## (\d+)\. ", md, re.M)]
    if len(heads) != len(set(heads)):
        raise IdError(f"duplicate section numbers in the generated document: {heads}")
    for m in re.finditer(r"\bsection (\d+)\b", md):
        if int(m.group(1)) not in heads:
            raise IdError(f"reference to missing section {m.group(1)}")


def load(path=SRC):
    try:
        with open(path, encoding="utf-8") as f:
            d = json.load(f)
    except (OSError, ValueError) as e:
        raise IdError(f"cannot read {path}: {e}")
    validate(d)
    return d


def outputs(d, out_dir):
    return {os.path.join(out_dir, "mcu", "mcu_ids.h"): gen_c(d),
            os.path.join(out_dir, "velora", "host", "mcu_ids.hpp"): gen_cpp(d),
            os.path.join(out_dir, "MCU-ID-MAP.md"): gen_md(d)}


def self_test():
    base = load()
    def expect_fail(mut, what):
        d = copy.deepcopy(base)
        mut(d)
        try:
            validate(d)
        except IdError:
            return
        raise SystemExit(f"self-test FAILED: {what} was accepted")
    expect_fail(lambda d: d["controls"][1].__setitem__("id", d["controls"][0]["id"]), "duplicate control id")
    expect_fail(lambda d: d["leds"][1].__setitem__("id", d["leds"][0]["id"]), "duplicate led id")
    expect_fail(lambda d: d["buttons"][1].__setitem__("name", d["buttons"][0]["name"]), "duplicate button name")
    expect_fail(lambda d: d["controls"][0].pop("bits"), "missing field")
    expect_fail(lambda d: d["buttons"][0].pop("states"), "missing states")
    expect_fail(lambda d: d["controls"][0].__setitem__("id", 0), "id 0")
    expect_fail(lambda d: d["controls"][0].__setitem__("id", 256), "id 256")
    expect_fail(lambda d: d["controls"][0].__setitem__("min", 999), "min > max")
    expect_fail(lambda d: d["retired_ids"].__setitem__("leds", [d["leds"][0]["id"]]), "reused retired id")
    expect_fail(lambda d: d.pop("document_version"), "missing document_version")
    expect_fail(lambda d: d["controls"][0].__setitem__("name", "bad name"), "non-identifier name")
    expect_fail(lambda d: d["controls"].__setitem__(0, "oops"), "non-dict entry")
    expect_fail(lambda d: d.__setitem__("document_version", 1), "non-string document_version")
    expect_fail(lambda d: d["controls"][9].__setitem__("bits", 8), "range does not fit bits")
    expect_fail(lambda d: d["buttons"][-1].__setitem__("states", 0), "states 0")
    expect_fail(lambda d: d["controls"][0].__setitem__("bits", True), "bool as int")
    expect_fail(lambda d: d.__setitem__("retired_ids", []), "retired_ids wrong type")
    try:
        check_sections("## 1. a\n## 1. b\n")
        raise SystemExit("self-test FAILED: duplicate section numbers accepted")
    except IdError:
        pass
    try:
        check_sections("## 1. a\nsee section 7\n")
        raise SystemExit("self-test FAILED: dangling section reference accepted")
    except IdError:
        pass
    print("gen_ids self-test: ok")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--write-published", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    try:
        if a.self_test:
            self_test()
            return 0
        d = load()
        if a.out_dir:
            for path, text in outputs(d, a.out_dir).items():
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "w", encoding="utf-8", newline="\n") as f:
                    f.write(text)
        if a.write_published:
            for p in PUBLISHED:
                if not os.path.isdir(os.path.dirname(p)):
                    print(f"skipping {p}: directory absent")
                    continue
                with open(p, "w", encoding="utf-8", newline="\n") as f:
                    f.write(gen_md(d))
        if a.check:
            want = gen_md(d)
            for p in PUBLISHED:
                if p != PUBLISHED[0] and not os.path.isdir(os.path.dirname(p)):
                    print(f"note: skipping published copy {p}: directory absent")
                    continue
                try:
                    with open(p, encoding="utf-8") as f:
                        have = f.read()
                except OSError:
                    raise IdError(f"published document missing: {p}")
                if have != want:
                    raise IdError(f"published document is stale: {p} (run gen_ids.py --write-published)")
            print("mcu id map: ok (ids.json valid, published documents up to date)")
        if not (a.out_dir or a.check or a.write_published):
            ap.error("nothing to do")
    except IdError as e:
        print(f"gen_ids: ERROR: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
