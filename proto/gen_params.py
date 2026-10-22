#!/usr/bin/env python3
"""Parameter registry generator (Story 2.1).

Reads proto/params.json and emits, deterministically (same input -> same bytes):
  core  : a C++ header for engine/core (numeric tables, no names)
  host  : a C++ header for engine/host (names, enum value names)
  proto : ws_v1_params.proto, the `ws.v1` enums (ParamId and one enum per registry enum)

Validation (any failure exits non-zero, names the offending ids/params, and writes nothing):
  duplicate id / duplicate name, missing or unknown class, reused reserved id, bad type/scope,
  range/default consistency, enum references, CC number clashes.

Usage: gen_params.py --defs params.json [--core PATH] [--host PATH] [--proto PATH]
Python 3 standard library only.
"""
import argparse
import json
import os
import re
import sys

NAME_RE = re.compile(r"^[a-z][a-z0-9_]*$")
CLASSES = ("structural", "performance")
TYPES = ("int", "uint", "float", "bool", "enum")
SCOPES = ("part", "master", "insert")
MAX_ID = 16383  # 14 bits: the UMP assignable controller carries the id as bank (7 bits) + index (7 bits)
RESERVED_EVENT_FIRST = 0x3FE0  # ids 0x3FE0..0x3FFF are reserved for the control/encoder UMP events on channel 15 (engine/core assign_events.hpp)
REQUIRED = ("id", "name", "class", "type", "scope", "unit", "default")


class RegistryError(Exception):
    pass


def camel(name):
    return "".join(p[:1].upper() + p[1:] for p in name.split("_") if p)


def upper(name):
    return name.upper()


def _num(v, what):
    if isinstance(v, bool) or not isinstance(v, (int, float)) or v != v or v in (float("inf"), float("-inf")):
        raise RegistryError(f"{what}: expected a finite number, got {v!r}")
    return v


def load_and_validate(path):
    def bad_constant(name):
        raise ValueError(f"non-finite number {name} is not allowed")

    try:
        with open(path, encoding="utf-8") as fh:
            doc = json.load(fh, parse_constant=bad_constant)
    except (OSError, ValueError) as e:
        raise RegistryError(f"{path}: cannot read definition file: {e}")
    if not isinstance(doc, dict):
        raise RegistryError("top level must be a JSON object")
    errors = []

    def err(msg):
        errors.append(msg)

    if doc.get("schema") != 1:
        err("schema must be 1")
    limits = doc.get("limits", {})
    if not isinstance(limits, dict):
        raise RegistryError("limits must be an object")
    for key, kind in (("enums", dict), ("reserved", list), ("params", list)):
        if not isinstance(doc.get(key, kind()), kind):
            raise RegistryError(f"{key} must be a {'list' if kind is list else 'object'}")
    parts = limits.get("parts")
    slots = limits.get("insert_slots")
    if not isinstance(parts, int) or not 1 <= parts <= 16:
        err("limits.parts must be an integer 1..16")
    if not isinstance(slots, int) or not 1 <= slots <= 16:
        err("limits.insert_slots must be an integer 1..16")

    enums = doc.get("enums", {})
    for en, vals in enums.items():
        if not NAME_RE.match(en):
            err(f"enum '{en}': bad name")
        if not isinstance(vals, list) or not vals:
            err(f"enum '{en}': must be a non-empty list")
            continue
        seen = set()
        for v in vals:
            if not isinstance(v, str) or not NAME_RE.match(v):
                err(f"enum '{en}': bad value {v!r}")
            elif v in seen:
                err(f"enum '{en}': duplicate value '{v}'")
            seen.add(v)

    reserved = {}
    for r in doc.get("reserved", []):
        if not isinstance(r, dict):
            err(f"reserved entry {r!r}: must be an object")
            continue
        rid = r.get("id")
        if not isinstance(rid, int) or isinstance(rid, bool) or not 1 <= rid <= MAX_ID:
            err(f"reserved entry {r!r}: bad id")
        elif rid in reserved:
            err(f"reserved id {rid} listed twice")
        else:
            reserved[rid] = r.get("name", "")

    by_id, by_name, by_cc, by_const = {}, {}, {}, {}
    params = []
    for idx, p in enumerate(doc.get("params", [])):
        if not isinstance(p, dict):
            err(f"param #{idx}: must be an object, got {p!r}")
            continue
        label = f"param #{idx} '{p.get('name', '?')}' (id {p.get('id', '?')})"
        missing = [k for k in REQUIRED if k not in p]
        if "class" in missing:
            err(f"{label}: missing class (every parameter is structural or performance)")
            missing.remove("class")
        if missing:
            err(f"{label}: missing field(s) {', '.join(missing)}")
            continue
        pid, name = p["id"], p["name"]
        if not isinstance(pid, int) or isinstance(pid, bool) or not 1 <= pid <= MAX_ID:
            err(f"{label}: id must be an integer 1..{MAX_ID}")
            continue
        if pid >= RESERVED_EVENT_FIRST:
            err(f"{label}: ids {RESERVED_EVENT_FIRST}..{MAX_ID} are reserved for control events (fence, encoder steps)")
            continue
        if not isinstance(name, str) or not NAME_RE.match(name):
            err(f"{label}: bad name")
            continue
        if any(isinstance(p[k], (dict, list)) for k in REQUIRED if k in p):
            err(f"{label}: fields must be scalars")
            continue
        if "class" in p and p["class"] not in CLASSES:
            err(f"{label}: class must be one of {CLASSES}, got {p['class']!r}")
        if p["type"] not in TYPES:
            err(f"{label}: type must be one of {TYPES}")
            continue
        if p["scope"] not in SCOPES:
            err(f"{label}: scope must be one of {SCOPES}")
        if p.get("class") == "performance" and p["scope"] == "insert":
            err(f"{label}: a performance parameter cannot have insert scope")
        if not isinstance(p["unit"], str) or not p["unit"]:
            err(f"{label}: unit must be a non-empty string")
        if pid in reserved:
            err(f"{label}: id {pid} is reserved/deprecated ('{reserved[pid]}') and must not be reused")
        if pid in by_id:
            err(f"duplicate id {pid}: '{by_id[pid]['name']}' and '{name}'")
            continue
        if name in by_name:
            err(f"duplicate name '{name}': ids {by_name[name]['id']} and {pid}")
            continue
        const = "k" + camel(name)
        if const in by_const:
            err(f"{label}: C++ constant {const} clashes with '{by_const[const]}'")
        by_const[const] = name

        q = dict(p)
        # Story 4.10: may an ASSIGN encoder drive it? Performance parameters default to yes; a structural parameter never (it has no live value).
        asg = p.get("assignable")
        if asg is not None and not isinstance(asg, bool):
            err(f"{label}: assignable must be true or false")
            asg = None
        if asg and p.get("class") in CLASSES and p.get("class") != "performance":
            err(f"{label}: only a performance parameter can be assignable")
        q["assignable"] = bool(p.get("class") == "performance" and asg is not False)
        t = p["type"]
        if t == "enum":
            en = p.get("enum")
            if en not in enums:
                err(f"{label}: unknown enum {en!r}")
                continue
            vals = enums[en]
            if p["default"] not in vals:
                err(f"{label}: default {p['default']!r} is not a value of enum '{en}'")
                continue
            q["min"], q["max"], q["default"] = 0, len(vals) - 1, vals.index(p["default"])
        else:
            if "min" not in p or "max" not in p:
                err(f"{label}: min and max are required")
                continue
            lo, hi, df = (_num(p["min"], label), _num(p["max"], label), _num(p["default"], label))
            if t == "bool":
                if (lo, hi) != (0, 1) or df not in (0, 1):
                    err(f"{label}: bool needs min 0, max 1 and default 0 or 1")
            elif t in ("int", "uint"):
                if any(float(x) != int(x) for x in (lo, hi, df)):
                    err(f"{label}: {t} range/default must be integral")
                if t == "uint" and lo < 0:
                    err(f"{label}: uint min must be >= 0")
            if not lo < hi:
                err(f"{label}: min {lo} must be below max {hi}")
            elif not lo <= df <= hi:
                err(f"{label}: default {df} outside range {lo}..{hi}")
            q["min"], q["max"], q["default"] = lo, hi, df
        if "cc" in p:
            cc = p["cc"]
            if not isinstance(cc, int) or isinstance(cc, bool) or not (1 <= cc <= 119) or cc in (6, 32, 38, 64, 98, 99, 100, 101):
                err(f"{label}: cc must be 1..119 and not a reserved controller")
            elif cc in by_cc:
                err(f"{label}: cc {cc} already mapped to '{by_cc[cc]}'")
            elif p.get("class") != "performance" or p["scope"] != "part":
                err(f"{label}: only performance parameters with part scope can have a cc")
            else:
                by_cc[cc] = name
        by_id[pid] = q
        by_name[name] = q
        params.append(q)
    # Every structural parameter needs storage in core::Snapshot (snapshot.hpp): master scope -> Snapshot::master,
    # part scope -> PartCfg::{output,timbre_ref,arp}, insert scope -> InsertSlotCfg. Anything else has nowhere to live.
    stored_part = {
        "part_output", "timbre_ref",
        "arp_on", "arp_mode", "arp_rate", "arp_octaves", "arp_gate", "arp_swing", "arp_latch"
    }
    stored_insert = {"insert_type", "insert_bypass"}
    for q in params:
        if q.get("class") != "structural" or q["scope"] == "master":
            continue
        ok = q["name"] in stored_part if q["scope"] == "part" else (
            q["name"] in stored_insert or re.fullmatch(r"insert_param_[1-9][0-9]*", q["name"]))
        if not ok:
            err(f"param '{q['name']}' (id {q['id']}): structural parameter has no Snapshot storage "
                f"(add a field to core::Snapshot and list it in gen_params.py)")
    ins = sorted((q for q in params if re.fullmatch(r"insert_param_[0-9]+", q["name"])),
                 key=lambda q: int(q["name"].rsplit("_", 1)[1]))
    for k, q in enumerate(ins):
        if q["name"] != f"insert_param_{k + 1}":
            err(f"insert params must be named insert_param_1..N without gaps; found '{q['name']}' at position {k + 1}")
            break
        if q["id"] != ins[0]["id"] + k:
            err(f"insert_param_* ids must be contiguous: '{q['name']}' has id {q['id']}, expected {ins[0]['id'] + k}")
            break
    for en, vals in enums.items():
        if not isinstance(vals, list):
            continue
        for i, v in enumerate(vals):
            c = f"k{camel(en)}{camel(v)}" if isinstance(v, str) else None
            if c and c in by_const:
                err(f"C++ constant {c} (enum '{en}' value '{v}') clashes with '{by_const[c]}'")
            if c:
                by_const[c] = f"{en}.{v}"
        cc_ = f"k{camel(en)}Count"
        if cc_ in by_const:
            err(f"C++ constant {cc_} (count of enum '{en}') clashes with '{by_const[cc_]}'")
        by_const[cc_] = f"{en}.count"
    if errors:
        raise RegistryError("\n".join(errors))
    params.sort(key=lambda x: x["id"])
    # Dense indices for the RT last-value table (performance) and the snapshot master block.
    perf_part = perf_master = master = 0
    for q in params:
        q["perf_index"] = q["master_index"] = -1
        if q["class"] == "performance":
            if q["scope"] == "part":
                q["perf_index"] = perf_part
                perf_part += 1
            else:
                q["perf_index"] = perf_master
                perf_master += 1
        elif q["scope"] == "master":
            q["master_index"] = master
            master += 1
    return {"parts": parts, "slots": slots, "enums": enums, "params": params,
            "perf_part": perf_part, "perf_master": perf_master, "master": master,
            "reserved": sorted(reserved.items())}


def fnum(v):
    if isinstance(v, int) or float(v) == int(v) and abs(v) < 1e15:
        return "%d.0" % int(v)
    return repr(float(v))


HEADER = "// GENERATED by proto/gen_params.py from proto/params.json. Do not edit.\n"


def emit_core(r):
    P = r["params"]
    o = [HEADER, "#pragma once\n\n#include <array>\n#include <cstddef>\n#include <cstdint>\n\n",
         "namespace velora::core::params {\n\n",
         f"inline constexpr std::uint32_t kPartCount = {r['parts']};\n",
         f"inline constexpr std::uint32_t kInsertSlotCount = {r['slots']};\n",
         "inline constexpr std::uint32_t kInsertParamCount = %d;\n\n" %
         sum(1 for p in P if re.fullmatch(r"insert_param_[0-9]+", p["name"])),
         "enum class Class : std::uint8_t { kStructural, kPerformance };\n",
         "enum class Type : std::uint8_t { kInt, kUint, kFloat, kBool, kEnum };\n",
         "enum class Scope : std::uint8_t { kPart, kMaster, kInsert };\n\n",
         "struct ParamDef {\n  std::uint16_t id;\n  Class cls;\n  Type type;\n  Scope scope;\n"
         "  double min, max, def;\n  std::int16_t cc;           // MIDI CC number mapped to this parameter, -1 none\n"
         "  std::int16_t perf_index;   // performance: dense index (part scope or master scope), else -1\n"
         "  std::int16_t master_index; // structural master scope: dense index, else -1\n"
         "  bool assignable;           // Story 4.10: an ASSIGN encoder may drive it (performance parameters only; params.json 'assignable': false opts out)\n};\n\n",
         f"inline constexpr std::size_t kParamCount = {len(P)};\n",
         f"inline constexpr std::size_t kPerfPartParams = {r['perf_part']};\n",
         f"inline constexpr std::size_t kPerfMasterParams = {r['perf_master']};\n",
         f"inline constexpr std::size_t kPerfSlotCount = kPerfPartParams * {r['parts']} + kPerfMasterParams;\n",
         f"inline constexpr std::size_t kMasterParams = {r['master']};\n\n",
         "inline constexpr std::array<ParamDef, kParamCount> kParams{{\n"]
    for p in P:
        o.append("    {%d, Class::k%s, Type::k%s, Scope::k%s, %s, %s, %s, %d, %d, %d, %s},  // %s\n" % (
            p["id"], camel(p["class"]), camel(p["type"]), camel(p["scope"]),
            fnum(p["min"]), fnum(p["max"]), fnum(p["default"]), p.get("cc", -1),
            p["perf_index"], p["master_index"], "true" if p["assignable"] else "false", p["name"]))
    o.append("}};\n\n")
    for p in P:
        o.append(f"inline constexpr std::uint16_t k{camel(p['name'])} = {p['id']};\n")
    o.append("\n")
    for en, vals in r["enums"].items():
        for i, v in enumerate(vals):
            o.append(f"inline constexpr std::uint8_t k{camel(en)}{camel(v)} = {i};\n")
        o.append(f"inline constexpr std::uint8_t k{camel(en)}Count = {len(vals)};\n")
    o.append("""
// Lookup by id (binary search over the id-sorted table); nullptr when the id is not registered.
constexpr const ParamDef* find(std::uint32_t id) noexcept {
  std::size_t lo = 0, hi = kParams.size();
  while (lo < hi) {
    const std::size_t mid = (lo + hi) / 2;
    if (kParams[mid].id == id) return &kParams[mid];
    if (kParams[mid].id < id) lo = mid + 1; else hi = mid;
  }
  return nullptr;
}

// The performance parameter a MIDI CC number drives, nullptr when unmapped.
constexpr const ParamDef* find_by_cc(std::uint32_t cc) noexcept {
  for (const ParamDef& d : kParams)
    if (d.cc >= 0 && static_cast<std::uint32_t>(d.cc) == cc) return &d;
  return nullptr;
}

// Slot of a performance parameter in the last-value table for `part` (ignored for master scope).
constexpr std::size_t perf_slot(const ParamDef& d, std::uint32_t part) noexcept {
  return d.scope == Scope::kPart ? static_cast<std::size_t>(d.perf_index) * kPartCount + part
                                 : kPerfPartParams * kPartCount + static_cast<std::size_t>(d.perf_index);
}

}  // namespace velora::core::params
""")
    return "".join(o)


def emit_host(r):
    P = r["params"]
    o = [HEADER, "#pragma once\n\n#include <array>\n#include <cstddef>\n#include <cstdint>\n#include <string_view>\n\n",
         "namespace velora::host::param_names {\n\n",
         "struct ParamName {\n  std::uint16_t id;\n  const char* name;\n  const char* unit;\n  std::int16_t enum_index;  // index into kEnums, -1 when not an enum\n  const char* locale_key;\n};\n\n",
         "struct EnumInfo {\n  const char* name;\n  std::size_t count;\n  const char* const* values;\n};\n\n"]
    en_names = list(r["enums"].keys())
    for en in en_names:
        o.append(f"inline constexpr const char* k{camel(en)}Values[] = {{" +
                 ", ".join(json.dumps(v) for v in r["enums"][en]) + "};\n")
    o.append(f"\ninline constexpr std::array<EnumInfo, {len(en_names)}> kEnums{{{{\n")
    for en in en_names:
        o.append(f"    {{{json.dumps(en)}, {len(r['enums'][en])}, k{camel(en)}Values}},\n")
    o.append("}};\n\n")
    o.append(f"inline constexpr std::array<ParamName, {len(P)}> kParamNames{{{{\n")
    for p in P:
        ei = en_names.index(p["enum"]) if p["type"] == "enum" else -1
        loc_key = p.get("locale_key", "")
        o.append("    {%d, %s, %s, %d, %s},\n" % (p["id"], json.dumps(p["name"]), json.dumps(p["unit"]), ei, json.dumps(loc_key)))
    o.append("}};\n\n")
    o.append("""constexpr const ParamName* find_name(std::uint32_t id) noexcept {
  for (const ParamName& n : kParamNames)
    if (n.id == id) return &n;
  return nullptr;
}

constexpr std::uint16_t id_of(std::string_view name) noexcept {
  for (const ParamName& n : kParamNames)
    if (name == n.name) return n.id;
  return 0;
}

constexpr const char* locale_key_of(std::uint32_t id) noexcept {
  for (const ParamName& n : kParamNames)
    if (n.id == id) return n.locale_key;
  return "";
}

}  // namespace velora::host::param_names
""")
    return "".join(o)


def emit_proto(r):
    P = r["params"]
    o = ["// GENERATED by proto/gen_params.py from proto/params.json. Do not edit.\n",
         'syntax = "proto3";\n\npackage ws.v1;\n\n',
         "// Parameter ids. Numeric values are stable and never reused (see proto/params.json 'reserved').\n",
         "enum ParamId {\n  PARAM_ID_UNSPECIFIED = 0;\n"]
    for p in P:
        o.append(f"  PARAM_{upper(p['name'])} = {p['id']};\n")
    if r["reserved"]:
        o.append("  // retired ids: never reused\n")
        o.append("  reserved " + ", ".join(str(i) for i, _ in r["reserved"]) + ";\n")
        names = [n for _, n in r["reserved"] if n]
        if names:
            o.append("  reserved " + ", ".join(json.dumps("PARAM_" + upper(n)) for n in names) + ";\n")
    o.append("}\n")
    for en, vals in r["enums"].items():
        o.append(f"\n// Registry enum '{en}'; the numeric value is the value of the matching parameter.\n")
        o.append(f"enum {camel(en)} {{\n")
        for i, v in enumerate(vals):
            o.append(f"  {upper(en)}_{upper(v)} = {i};\n")
        o.append("}\n")
    return "".join(o)


def write_atomic(path, text):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    os.replace(tmp, path)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--defs", required=True)
    ap.add_argument("--core")
    ap.add_argument("--host")
    ap.add_argument("--proto")
    a = ap.parse_args(argv)
    try:
        reg = load_and_validate(a.defs)
    except RegistryError as e:
        print(f"gen_params: error in {a.defs}:\n{e}", file=sys.stderr)
        return 1
    outs = ((a.core, emit_core), (a.host, emit_host), (a.proto, emit_proto))
    for path, fn in outs:
        if path:
            write_atomic(path, fn(reg))
    return 0


if __name__ == "__main__":
    sys.exit(main())
