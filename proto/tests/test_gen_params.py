#!/usr/bin/env python3
"""Generator failure cases and determinism: runs proto/gen_params.py on bad definition files."""
import copy
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
PROTO = os.path.dirname(HERE)
GEN = os.path.join(PROTO, "gen_params.py")
with open(os.path.join(PROTO, "params.json"), encoding="utf-8") as fh:
    GOOD = json.load(fh)


def run(doc, outs=("core", "host", "proto")):
    d = tempfile.mkdtemp()
    defs = os.path.join(d, "params.json")
    with open(defs, "w", encoding="utf-8") as fh:
        json.dump(doc, fh)
    cmd = [sys.executable, "-I", GEN, "--defs", defs]
    paths = {}
    for o in outs:
        paths[o] = os.path.join(d, "out_" + o)
        cmd += ["--" + o, paths[o]]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r, paths


def run_raw(text):
    d = tempfile.mkdtemp()
    defs = os.path.join(d, "params.json")
    with open(defs, "w", encoding="utf-8") as fh:
        fh.write(text)
    out = os.path.join(d, "o")
    r = subprocess.run([sys.executable, "-I", GEN, "--defs", defs, "--core", out], capture_output=True, text=True)
    return r


def mutate(fn):
    doc = copy.deepcopy(GOOD)
    fn(doc)
    return doc


def read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def d_name(i):
    return GOOD["params"][i]["name"]


class GeneratorTests(unittest.TestCase):
    def fails(self, doc, *needles):
        r, paths = run(doc)
        self.assertNotEqual(r.returncode, 0, "generator should fail")
        for n in needles:
            self.assertIn(n, r.stderr)
        for p in paths.values():
            self.assertFalse(os.path.exists(p), "nothing may be emitted for an invalid definition")

    def test_valid_definition_emits_all_three(self):
        r, paths = run(GOOD)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("kDrawbar8 = 102", read(paths["core"]))
        self.assertIn("PARAM_DRAWBAR_8 = 102", read(paths["proto"]))
        self.assertIn('"drawbar_8"', read(paths["host"]))

    def test_deterministic(self):
        _, pa = run(GOOD)
        _, pb = run(GOOD)
        for k in pa:
            self.assertEqual(read(pa[k]), read(pb[k]))

    def test_duplicate_id_names_both_params(self):
        def f(d):
            d["params"][1]["id"] = d["params"][0]["id"]
        self.fails(mutate(f), "duplicate id 100", "drawbar_16", "drawbar_5_1_3")

    def test_missing_class(self):
        def f(d):
            del d["params"][3]["class"]
        self.fails(mutate(f), "missing class", d_name(3))

    def test_unknown_class(self):
        def f(d):
            d["params"][3]["class"] = "both"
        self.fails(mutate(f), "class must be one of")

    def test_reused_reserved_id(self):
        def f(d):
            d["params"][0]["id"] = 99
        self.fails(mutate(f), "id 99", "reserved")

    def test_duplicate_name(self):
        def f(d):
            d["params"][1]["name"] = d["params"][0]["name"]
        self.fails(mutate(f), "duplicate name 'drawbar_16'")

    def test_default_out_of_range(self):
        def f(d):
            d["params"][0]["default"] = 99
        self.fails(mutate(f), "outside range")

    def test_inverted_range(self):
        def f(d):
            d["params"][0]["min"], d["params"][0]["max"] = 8, 0
        self.fails(mutate(f), "must be below max")

    def test_enum_default_not_a_value(self):
        def f(d):
            for p in d["params"]:
                if p["name"] == "rotary_speed":
                    p["default"] = "warp"
        self.fails(mutate(f), "not a value of enum")

    def test_id_beyond_14_bits(self):
        def f(d):
            d["params"][0]["id"] = 20000
        self.fails(mutate(f), "1..16383")

    def test_duplicate_cc(self):
        def f(d):
            d["params"][1]["cc"] = d["params"][0]["cc"]
        self.fails(mutate(f), "cc 70 already mapped")

    def test_performance_insert_scope_rejected(self):
        def f(d):
            d["params"][0]["scope"] = "insert"
        self.fails(mutate(f), "insert scope")

    def no_traceback(self, r):
        self.assertNotEqual(r.returncode, 0)
        self.assertNotIn("Traceback", r.stderr)
        self.assertIn("gen_params: error", r.stderr)

    def test_malformed_shapes_are_reported_not_tracebacks(self):
        for text in ("[]", "3", '{"schema":1,"limits":[],"params":[]}', '{"schema":1,"limits":{"parts":8,"insert_slots":5},"params":[3]}',
                     '{"schema":1,"limits":{"parts":8,"insert_slots":5},"params":{}}',
                     '{"schema":1,"limits":{"parts":8,"insert_slots":5},"enums":[]}',
                     '{"schema":1,"limits":{"parts":8,"insert_slots":5},"reserved":[1]}',
                     '{"schema":1,"limits":{"parts":8,"insert_slots":5},"params":[{"id":1,"name":"a","class":["x"],"type":"int","scope":"part","unit":"u","default":0}]}'):
            self.no_traceback(run_raw(text))

    def test_non_finite_numbers(self):
        raw = json.dumps(GOOD)
        for bad in ("NaN", "Infinity", "-Infinity"):
            self.no_traceback(run_raw(raw.replace('"max": 8, "default": 0, "cc": 70', '"max": %s, "default": 0, "cc": 70' % bad, 1)
                                      if '"max": 8, "default": 0, "cc": 70' in raw else raw.replace('"default": 0.0', '"default": ' + bad, 1)))

    def test_non_integral_int_range(self):
        def f(d):
            d["params"][0]["max"] = 8.5
        self.fails(mutate(f), "integral")

    def test_constant_clash_with_enum_count(self):
        def f(d):
            d["params"][0]["name"] = "part_output_count"
        self.fails(mutate(f), "kPartOutputCount", "clashes")

    def test_constant_clash_with_enum_value(self):
        def f(d):
            d["params"][0]["name"] = "insert_type_rotary"
        self.fails(mutate(f), "kInsertTypeRotary", "clashes")

    def test_structural_param_without_snapshot_storage(self):
        def f(d):
            d["params"].append({"id": 701, "name": "mystery", "class": "structural", "type": "int", "scope": "part",
                                "unit": "u", "min": 0, "max": 3, "default": 0})
        self.fails(mutate(f), "no Snapshot storage", "mystery")

    def test_insert_params_must_be_contiguous(self):
        def f(d):
            for p in d["params"]:
                if p["name"] == "insert_param_3":
                    p["id"] = 399
        self.fails(mutate(f), "contiguous")

    def test_insert_params_without_gaps(self):
        def f(d):
            d["params"] = [p for p in d["params"] if p["name"] != "insert_param_2"]
        self.fails(mutate(f), "without gaps")

    def test_reserved_ids_are_emitted_into_the_proto_enum(self):
        _, paths = run(GOOD)
        self.assertIn("reserved 99;", read(paths["proto"]))
        self.assertIn('reserved "PARAM_ORGAN_MASTER_DRAFT";', read(paths["proto"]))

    def test_generated_headers_compile(self):
        cxx = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
        if not cxx:
            self.skipTest("no C++ compiler")
        r, paths = run(GOOD)
        tu = os.path.join(os.path.dirname(paths["core"]), "tu.cpp")
        incdir = os.path.join(os.path.dirname(paths["core"]), "inc", "velora", "core")
        hincdir = os.path.join(os.path.dirname(paths["core"]), "inc", "velora", "host")
        os.makedirs(incdir)
        os.makedirs(hincdir)
        shutil.copy(paths["core"], os.path.join(incdir, "params_gen.hpp"))
        shutil.copy(paths["host"], os.path.join(hincdir, "params_names_gen.hpp"))
        with open(tu, "w") as fh:
            fh.write('#include "velora/core/params_gen.hpp"\n#include "velora/host/params_names_gen.hpp"\n'
                     "static_assert(velora::core::params::find(100) != nullptr);\n"
                     "static_assert(velora::core::params::kParams.size() == velora::core::params::kParamCount);\n"
                     "static_assert(velora::host::param_names::id_of(\"drawbar_16\") == 100);\n"
                     "int main() { return 0; }\n")
        c = subprocess.run([cxx, "-std=c++20", "-fsyntax-only", "-I", os.path.join(os.path.dirname(paths["core"]), "inc"), tu],
                           capture_output=True, text=True)
        self.assertEqual(c.returncode, 0, c.stderr)

    def test_generated_proto_runs_through_protoc(self):
        protoc = shutil.which("protoc")
        if not protoc:
            self.skipTest("no protoc")
        _, paths = run(GOOD)
        d = os.path.dirname(paths["proto"])
        shutil.copy(paths["proto"], os.path.join(d, "ws_v1_params.proto"))
        out = os.path.join(d, "pbout")
        os.makedirs(out)
        c = subprocess.run([protoc, "-I", d, "-I", PROTO, "--cpp_out", out, os.path.join(d, "ws_v1_params.proto"),
                            os.path.join(PROTO, "ws_v1.proto")], capture_output=True, text=True)
        self.assertEqual(c.returncode, 0, c.stderr)

    def test_unreadable_file(self):
        r = subprocess.run([sys.executable, "-I", GEN, "--defs", "/nonexistent/params.json"],
                           capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)


if __name__ == "__main__":
    unittest.main()
